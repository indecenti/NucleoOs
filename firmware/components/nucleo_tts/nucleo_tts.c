// nucleo_tts — firmware service: plans (nucleo_tts_plan) and ASSEMBLES the clips' PCM into one
// temporary WAV, then plays it via nucleo_audio (battle-tested path, NOT touched). See nucleo_tts.h.
//
// Efficient RETRIEVAL (RAM/CPU/IO-light): NO 28k files in a FAT folder (stat would be O(N)).
// The clips live in TWO files per language — index.bin (slug->offset,len, sorted) + clips.pcm (concatenated
// PCM). Finding a clip = a binary search on index.bin via fseek (nucleo_tts_index.c). ~zero
// RAM, non-fragmenting block. Assembly is streaming (1KB stack buffer).
//
// CORRECTNESS: the voice is never wrong. If the utterance has a word NOT covered (UNKNOWN token from
// the planner) it plays the canonical "read_it" phrase ("leggila sullo schermo") instead of mispronouncing it.
#include "nucleo_tts.h"
#include "nucleo_tts_index.h"
#include "nucleo_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   // heap diagnostics for the silent-drop log (largest/free block)
#include "esp_task_wdt.h"   // feed the task WDT during a long SD render (slow ADV SD): many clips = cumulative > 8s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>      // stat: cache-hit for single-slug pronunciations (WAV memoization)
#include <dirent.h>        // opendir/readdir: purges the WAV cache when the speed changes

static const char *TAG = "tts";
static bool say_one_clip(const char *lang, const char *slug);   // fwd: used by the "leggila" fallbacks

#define TTS_DIR    "/sd/data/tts"
#define OUT_PATH   TTS_DIR "/_say.wav"
#define CFG_PATH   TTS_DIR "/speak.cfg"
#define SPEED_PATH TTS_DIR "/speed.cfg"
#define VER_PATH   TTS_DIR "/fmt.ver"   // version of the rendered-WAV format: changing it invalidates the cache
#define TTS_RENDER_FMT 2                // BUMP when the rendered PCM changes (here: +anti-click at clip edges)
#define TTS_MAX_CHARS 220    // beyond that -> "leggila" (offline). Raised from 140 to 220; the limit is the voice_task's 16KB stack
#define TTS_MAX_SKIP     2   // unknown words TOLERATED in a sentence (skipped); beyond that -> "leggila"
#define TTS_SKIP_PAUSE_MS 150 // micro-pause in place of each skipped word (a natural gap, not words stuck together)

static char s_def_lang[8] = "it";
static bool s_enabled = true;        // "speak when queried" toggle (persisted on SD); default ON
static int  s_speed_pct = TTS_SPEED_DEF;   // reading speed % (WAV header rate); persisted on SD

static void idx_path(char *buf, size_t cap, const char *lang) { snprintf(buf, cap, "%s/%s/index.bin", TTS_DIR, lang); }
static void pcm_path(char *buf, size_t cap, const char *lang) { snprintf(buf, cap, "%s/%s/clips.pcm", TTS_DIR, lang); }

// Deterministic cache path for SINGLE-slug pronunciations (read_it, greetings, "non lo so", fixed
// answers...): the WAV depends only on (lang, slug), so it's assembled ONCE and reused -> on
// repeats there's neither assembly nor SD writes (on this HW the SD shares the SPI bus with the display:
// less writing = less UI blocking). Slugs are alphanumeric/underscore -> safe filenames; the set is
// bounded by the fixed-clip vocabulary (a few dozen small files, naturally bounded growth).
// NB: if the voice package (clips.pcm) is UPDATED the cache must be invalidated by hand: rm /data/tts/c_*.wav.
static void cache_path(char *buf, size_t cap, const char *lang, const char *slug)
{
    snprintf(buf, cap, "%s/c_%s_%s.wav", TTS_DIR, lang, slug);
}

// planner callback: the slug exists if the index (passed in ud) finds it.
static bool has_clip(const char *slug, void *ud)
{
    return ud && slug && slug[0] && tts_index_find((tts_index_t *)ud, slug, NULL, NULL);
}

// ---- speak toggle (persisted) ----------------------------------------------------------
void nucleo_tts_set_enabled(bool on)
{
    s_enabled = on;
    FILE *f = fopen(CFG_PATH, "wb");
    if (f) { fputc(on ? '1' : '0', f); fclose(f); }
    ESP_LOGI(TAG, "parla = %s", on ? "ON" : "OFF");
}
bool nucleo_tts_enabled(void) { return s_enabled; }
static void load_enabled(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) { s_enabled = true; return; }
    int c = fgetc(f); fclose(f);
    s_enabled = (c != '0');
}

// ---- reading speed (persisted) --------------------------------------------------------
// The per-slug cache "bakes" the rate into the WAV header: changing the speed invalidates it, so the fixed
// clips (read_it/greetings/"non lo so"...) get re-assembled at the new speed. A few small files, and it only happens
// when the user moves the slider (rare) -> an O(n) purge is acceptable. No recursion: the cache is entirely
// in TTS_DIR (the clips.pcm/index.bin blobs live in the it/en subfolders, which we don't touch here).
static void purge_clip_cache(void)
{
    DIR *d = opendir(TTS_DIR);
    if (!d) return;
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t ln = strlen(n);
        if (ln > 6 && n[0] == 'c' && n[1] == '_' && !strcmp(n + ln - 4, ".wav")) {
            snprintf(path, sizeof path, "%s/%s", TTS_DIR, n);
            remove(path);
        }
    }
    closedir(d);
}

// Cache migration at boot: if the rendered-WAV format has changed (e.g. now with anti-click at the edges),
// the per-slug memoized WAVs have the old PCM "baked in" -> they must be discarded once, so the fixed
// phrases (read_it/greetings/"non lo so") also play with the new treatment. Self-invalidating (writes the version).
static void migrate_cache(void)
{
    int v = 0;
    FILE *f = fopen(VER_PATH, "rb");
    if (f) { char b[8] = {0}; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k < sizeof b ? k : sizeof b - 1] = 0; v = atoi(b); }
    if (v == TTS_RENDER_FMT) return;
    purge_clip_cache();
    f = fopen(VER_PATH, "wb");
    if (f) { char b[8]; int k = snprintf(b, sizeof b, "%d", TTS_RENDER_FMT); if (k > 0) fwrite(b, 1, (size_t)k, f); fclose(f); }
    ESP_LOGI(TAG, "cache WAV invalidata (formato reso %d)", TTS_RENDER_FMT);
}

static void load_speed(void)
{
    FILE *f = fopen(SPEED_PATH, "rb");
    if (!f) { s_speed_pct = TTS_SPEED_DEF; return; }
    char buf[8] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
    int v = atoi(buf);
    s_speed_pct = nucleo_tts_speed_clamp(v > 0 ? v : TTS_SPEED_DEF);
}

void nucleo_tts_set_speed(int pct)
{
    int v = nucleo_tts_speed_clamp(pct);
    if (v == s_speed_pct) return;       // same value: no SD write, and the WAV cache stays valid
    s_speed_pct = v;
    FILE *f = fopen(SPEED_PATH, "wb");
    if (f) { char buf[8]; int k = snprintf(buf, sizeof buf, "%d", v); if (k > 0) fwrite(buf, 1, (size_t)k, f); fclose(f); }
    purge_clip_cache();                 // the WAV cache has the old rate baked into the header
    ESP_LOGI(TAG, "velocita' lettura = %d%%", v);
}
int nucleo_tts_speed(void) { return s_speed_pct; }

static bool voice_installed(const char *lang)
{
    char p[64]; idx_path(p, sizeof p, lang);
    tts_index_t ix;
    if (!tts_index_open(&ix, p)) return false;
    tts_index_close(&ix);
    return true;
}

bool nucleo_tts_init(const char *lang)
{
    if (lang && lang[0]) { strncpy(s_def_lang, lang, sizeof(s_def_lang) - 1); s_def_lang[sizeof(s_def_lang) - 1] = 0; }
    load_enabled();
    load_speed();
    migrate_cache();    // if the rendered-WAV format has changed (anti-click), discard the stale WAV cache
    bool ok = voice_installed(s_def_lang);
    ESP_LOGI(TAG, "voce %s: %s, parla=%s, vel=%d%%", s_def_lang, ok ? "installata" : "assente", s_enabled ? "ON" : "OFF", s_speed_pct);
    return ok;
}
bool nucleo_tts_available(void) { return voice_installed(s_def_lang); }
void nucleo_tts_stop(void)      { nucleo_audio_stop(); }   // staged API: no caller yet (TTS trigger pending)

// ---- WAV out + assembly -------------------------------------------------------------------
static void wav_header(uint8_t h[44], uint32_t data_bytes, uint32_t rate)
{
    uint32_t br = rate * 1 * 2;                       // byteRate (mono, 16-bit)
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;  memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
    uint32_t f16 = 16;                memcpy(h + 16, &f16, 4);
    uint16_t pcm = 1, ch = 1, ba = 2, bps = 16;
    memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bps, 2);
    memcpy(h + 36, "data", 4);        memcpy(h + 40, &data_bytes, 4);
}

// Copies `len` bytes of PCM from the blob (at offset `off`) into the output. -> bytes written. Smooths the
// clip edges (anti-click): transforms each chunk IN PLACE before writing it, accounting for where it falls in the
// clip (`copied`). No change to the streaming IO/FD structure: just a pure transform inserted.
static uint32_t copy_clip(FILE *blob, FILE *out, uint32_t off, uint32_t len, uint8_t *buf, size_t bufsz)
{
    if (fseek(blob, (long)off, SEEK_SET) != 0) return 0;
    uint32_t left = len, written = 0, copied = 0;
    while (left) {
        size_t want = left < bufsz ? left : bufsz;
        size_t got = fread(buf, 1, want, blob);
        if (!got) break;
        nucleo_tts_declick_chunk(buf, (int)got, copied, len, TTS_DECLICK_SAMPLES);   // clip edges -> ~0 (no "tick")
        written += (uint32_t)fwrite(buf, 1, got, out);
        copied  += (uint32_t)got;
        left -= (uint32_t)got;
    }
    return written;
}
static uint32_t put_silence(FILE *out, int ms, uint32_t rate, uint8_t *buf, size_t bufsz)
{
    uint32_t bytes = (uint32_t)((long)rate * ms / 1000) * 2;
    memset(buf, 0, bufsz);
    uint32_t left = bytes, written = 0;
    while (left) { size_t n = left < bufsz ? left : bufsz; written += (uint32_t)fwrite(buf, 1, n, out); left -= (uint32_t)n; }
    return written;
}

// Assembles the tokens (CLIP read from the blob via the index, PAUSE = silence) into `out_path` (WAV) and plays it.
// NB: `ix` is BORROWED from the caller (say/say_one_clip), which ALWAYS closes it after this render
// on every return — it must NOT be closed here (that would be a double-close). render only handles the blob and output.
static bool render(const char *lang, const tts_token_t *tok, int n, tts_index_t *ix, const char *out_path)
{
    char bp[64]; pcm_path(bp, sizeof bp, lang);
    FILE *blob = fopen(bp, "rb");
    if (!blob) { ESP_LOGW(TAG, "blob mancante: %s", bp); return false; }
    nucleo_audio_stop();                              // releases the output from an ongoing read
    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(blob); ESP_LOGE(TAG, "open out failed"); return false; }

    // SPEED (zero-RAM): the header declares base*speed%, so the DAC scans the same samples
    // faster -> both clips AND pauses (silence generated at the BASE rate) shrink together. No
    // sample touched. At 100% it's identical to before. The rate baked here is also the one in the cached WAV.
    uint32_t out_rate = nucleo_tts_speed_rate(ix->rate, s_speed_pct);
    uint8_t buf[1024], hdr[44];
    wav_header(hdr, 0, out_rate); fwrite(hdr, 1, 44, out);
    uint32_t data = 0, off, len;
    for (int i = 0; i < n; i++) {
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // render reads MANY clips from SD: on slow SD the cumulative time exceeds the 8s WDT -> feed per token
        if (tok[i].kind == TTS_TOK_PAUSE)
            data += put_silence(out, tok[i].ms, ix->rate, buf, sizeof buf);   // ms at BASE rate: shrinks along with playback
        else if (tok[i].kind == TTS_TOK_CLIP && tts_index_find(ix, tok[i].slug, &off, &len))
            data += copy_clip(blob, out, off, len, buf, sizeof buf);
        else if (tok[i].kind == TTS_TOK_UNKNOWN)
            data += put_silence(out, TTS_SKIP_PAUSE_MS, ix->rate, buf, sizeof buf);  // tolerated skipped word: a micro-gap
        // CLIP slug missing from the index: skipped (coverage already decided upstream)
    }
    wav_header(hdr, data, out_rate);
    fseek(out, 0, SEEK_SET); fwrite(hdr, 1, 44, out);
    fclose(out); fclose(blob);

    if (data == 0) { remove(out_path); return false; }   // no truncated WAV left in cache
    esp_err_t e = nucleo_audio_play(out_path);
    if (e == ESP_OK) nucleo_audio_fade_in(60);
    else ESP_LOGW(TAG, "play '%s' FALLITO (err %d) -> voce muta su questo enunciato", out_path, (int)e);
    return e == ESP_OK;
}

// Plays the single clip `slug` (e.g. "read_it"). MEMOIZED: if the WAV (lang,slug) is already in cache it
// plays it directly (zero assembly, zero SD write); otherwise it assembles it into the cache file
// once. It's the hottest path in TTS (every "leggila" fallback + all fixed answers go through here).
static bool say_one_clip(const char *lang, const char *slug)
{
    char cache[96]; cache_path(cache, sizeof cache, lang, slug);
    char bp[64]; pcm_path(bp, sizeof bp, lang);
    struct stat st, sb;
    // cache-hit ONLY if the WAV exists, is complete, and is NOT older than the voice package: this way an
    // update to clips.pcm invalidates the stale WAVs on its own (if FAT timestamps aren't reliable
    // it degrades to the worst case = manual behavior, never wrong audio).
    if (stat(cache, &st) == 0 && st.st_size > 44 &&
        !(stat(bp, &sb) == 0 && sb.st_mtime > st.st_mtime)) {
        esp_err_t e = nucleo_audio_play(cache);           // play() stops any ongoing audio on its own
        if (e == ESP_OK) nucleo_audio_fade_in(60);
        return e == ESP_OK;
    }
    char p[64]; idx_path(p, sizeof p, lang);
    tts_index_t ix;
    if (!tts_index_open(&ix, p)) return false;
    tts_token_t t; memset(&t, 0, sizeof t); t.kind = TTS_TOK_CLIP;
    snprintf(t.slug, sizeof t.slug, "%s", slug);
    bool r = render(lang, &t, 1, &ix, cache);             // assembles ONCE into the cache file
    tts_index_close(&ix);
    return r;
}

// say()'s engine: plans it and, if EVERYTHING is covered, speaks it. If it's NOT (code/too long or
// an uncovered word): with a non-NULL `fallback` it speaks THAT (a short confirmation, e.g. "Done") instead of
// "leggila" -> for operations the OUTCOME matters more than the exact text (file names, event details).
static bool say_impl(const char *text, const char *lang, const char *fallback, bool quiet)
{
    if (!s_enabled || !text || !text[0]) return false;
    const char *l = (lang && lang[0]) ? lang : s_def_lang;

    // "Speech-ifies" the math symbols (= % ^) FIRST of all: without this, '=' would trip the guard
    // and the whole sentence would fall back to "leggila" (this was the "Fa 16 isn't spoken" bug). No-op on normal text.
    char mtext[640];   // 640 = headroom for the mathspeak expansion of text up to TTS_MAX_CHARS=220
    nucleo_tts_mathspeak(text, mtext, sizeof mtext, l);
    text = mtext;

    // OFFLINE guard: too long or "looks like code" -> don't read out unintelligible chunks. (Already
    // runs on the TTS task: the fallback to "leggila" here is SYNCHRONOUS, not a new enqueue.)
    if (!nucleo_tts_text_speakable(text, TTS_MAX_CHARS))
        return (fallback && fallback[0]) ? say_impl(fallback, l, NULL, false) : (quiet ? false : say_one_clip(l, "read_it"));

    char p[64]; idx_path(p, sizeof p, l);
    tts_index_t ix;
    if (!tts_index_open(&ix, p)) { ESP_LOGW(TAG, "voce %s non installata", l); return false; }

    // Whole FIXED answer (greetings, "non lo so", pairing...): if the entire text is a single clip,
    // play it (natural prosody, beyond the planner's 6-word phrase-match limit).
    char full[TTS_IDX_SLUG]; uint32_t foff, flen;
    nucleo_tts_full_slug(text, full, sizeof full);
    if (full[0] && tts_index_find(&ix, full, &foff, &flen)) {
        tts_index_close(&ix);             // it's a single clip: delegates to the memoized (cacheable) path
        return say_one_clip(l, full);
    }

    // TOK_MAX == MAX_UNITS (160): holds all the tokens of an utterance <=220 chars; ~9.6KB heap, freed right after render.
    enum { TOK_MAX = 160 };
    // SIZE THE BUFFER TO THE ACTUAL TEXT, not the 220-char worst case. mathspeak has already expanded the
    // symbols, and every remaining char emits AT MOST one token (digits collapse: "14"->"quattordici" = 1
    // token), so strlen+8 is a safe upper bound. WHY IT MATTERS: the full 160-token malloc is ~9.6KB
    // CONTIGUOUS; on this fragmented PSRAM-less heap it FAILED after the first answer fragmented things ->
    // say_impl returned false SILENTLY (no voice, not even "leggila") -> "the first reply gets read, then
    // starting from the next message it stops reading anything". A short reply ("Sono le 14 e 30", "Fa 16") now needs
    // ~0.6KB, which fits even when the largest block is small. Falls back to TOK_MAX for long sentences.
    int cap = (int)strlen(text) + 8; if (cap > TOK_MAX) cap = TOK_MAX; if (cap < 8) cap = 8;
    tts_token_t *tok = malloc((size_t)cap * sizeof(tts_token_t));
    if (!tok) {   // genuinely out of heap even for the small buffer — log it so /api/logs shows the silent drop
        ESP_LOGW(TAG, "say: tok malloc FAILED cap=%d (%uB) largest=%u free=%u -> SILENT \"%.40s\"",
                 cap, (unsigned)(cap * sizeof(tts_token_t)),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), text);
        tts_index_close(&ix); return false;
    }
    int n = nucleo_tts_plan(text, l, has_clip, &ix, tok, cap);

    int unknown = 0, clips = 0;
    char miss[224]; miss[0] = 0;
    for (int i = 0; i < n; i++) {
        if (tok[i].kind == TTS_TOK_CLIP) { clips++; continue; }
        if (tok[i].kind != TTS_TOK_UNKNOWN) continue;
        unknown++;
        size_t ml = strlen(miss);
        if (ml + strlen(tok[i].slug) + 3 < sizeof miss) { if (ml) strcat(miss, ", "); strcat(miss, tok[i].slug); }
    }

    // TOLERANCE: a few uncovered words do NOT discard the whole sentence. render() replaces them with a
    // micro-pause, so we speak the covered part. It only falls back to "leggila" if the uncovered words exceed
    // TTS_MAX_SKIP (the sentence would lose its meaning) or if NO word is covered (the output would be empty).
    if (n <= 0 || clips == 0 || unknown > TTS_MAX_SKIP) {
        free(tok); tts_index_close(&ix);                 // close BEFORE falling back (max 1 open index)
        if (fallback && fallback[0]) return say_impl(fallback, l, NULL, false);   // short confirmation instead of "leggila"
        // DIAGNOSIS: logs the uncovered clips (visible in /api/logs EXACTLY what's missing).
        if (unknown > TTS_MAX_SKIP) ESP_LOGW(TAG, "\"%s\": %d clip scoperte [%s] > soglia %d -> read_it", text, unknown, miss, TTS_MAX_SKIP);
        else if (n > 0)             ESP_LOGW(TAG, "\"%s\": nessuna parola coperta -> read_it", text);
        else                        ESP_LOGW(TAG, "\"%s\": planner vuoto -> read_it", text);
        return quiet ? false : say_one_clip(l, "read_it");   // quiet: stays silent and signals "uncovered" to the caller
    }
    if (unknown > 0)                                      // 1..TTS_MAX_SKIP uncovered: tolerated, speak the rest
        ESP_LOGI(TAG, "\"%s\": %d parola/e scoperta/e saltata/e [%s], pronuncio la parte coperta", text, unknown, miss);
    bool r = render(l, tok, n, &ix, OUT_PATH);   // variable sentence: not memoizable -> scratch file
    free(tok);
    tts_index_close(&ix);
    return r;
}

// Public API: SYNCHRONOUS (the speech runs on the calling task). On this constrained HW (8 open files
// max, ~fragmented heap) a dedicated TTS task cost too much (10KB stack + 3 concurrent FDs) and
// exhausted FD/heap (broke music + opening _say.wav). The anti-freeze protection is the FATFS timeout
// (5s < 8s watchdog) + petting the watchdog: a slow SD degrades to a short delay, not a hang/reboot.
bool nucleo_tts_read_hint(const char *lang)
{
    if (!s_enabled) return false;
    return say_one_clip((lang && lang[0]) ? lang : s_def_lang, "read_it");
}
bool nucleo_tts_say(const char *text, const char *lang)                            { return say_impl(text, lang, NULL, false); }
bool nucleo_tts_say_or(const char *text, const char *fallback, const char *lang)   { return say_impl(text, lang, fallback, false); }
// SILENT on an uncovered word: speaks the sentence if covered, otherwise returns false WITHOUT playing "leggila". This way the
// caller (the knowledge voice in app_anima) emits just ONE read_hint for several sentences -> never two "leggi".
bool nucleo_tts_say_quiet(const char *text, const char *lang)                      { return say_impl(text, lang, NULL, true); }
