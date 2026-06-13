// nucleo_tts — servizio firmware: pianifica (nucleo_tts_plan) e ASSEMBLA i PCM delle clip in un solo
// WAV temporaneo, poi lo suona via nucleo_audio (path collaudato, NON toccato). Vedi nucleo_tts.h.
//
// RETRIEVAL efficiente (RAM/CPU/IO-light): NIENTE 28k file in cartella FAT (lo stat sarebbe O(N)).
// Le clip stanno in DUE file per lingua — index.bin (slug->offset,len, ordinato) + clips.pcm (PCM
// concatenato). Trovare una clip = ricerca binaria su index.bin via fseek (nucleo_tts_index.c). RAM
// ~zero, blocco non frammentante. L'assemblaggio e' streaming (buffer 1KB sullo stack).
//
// CORRETTEZZA: la voce non sbaglia mai. Se l'enunciato ha una parola NON coperta (token UNKNOWN dal
// planner) si suona la frase canonica "read_it" ("leggila sullo schermo") invece di leggere male.
#include "nucleo_tts.h"
#include "nucleo_tts_index.h"
#include "nucleo_audio.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>      // stat: cache-hit delle pronunce a singolo slug (memoizzazione WAV)
#include <dirent.h>        // opendir/readdir: purge della cache WAV quando cambia la velocita'

static const char *TAG = "tts";
static bool say_one_clip(const char *lang, const char *slug);   // fwd: usata dai fallback "leggila"

#define TTS_DIR    "/sd/data/tts"
#define OUT_PATH   TTS_DIR "/_say.wav"
#define CFG_PATH   TTS_DIR "/speak.cfg"
#define SPEED_PATH TTS_DIR "/speed.cfg"
#define VER_PATH   TTS_DIR "/fmt.ver"   // versione del formato dei WAV resi: cambia -> invalida la cache
#define TTS_RENDER_FMT 2                // BUMP quando cambia il PCM reso (qui: +anti-click sui bordi clip)
#define TTS_MAX_CHARS 140    // oltre -> "leggila" (offline): risposte piu' lunghe non si ascoltano bene

static char s_def_lang[8] = "it";
static bool s_enabled = true;        // "parla quando interrogato" (persistito su SD); default ON
static int  s_speed_pct = TTS_SPEED_DEF;   // velocita' di lettura % (rate header WAV); persistita su SD

static void idx_path(char *buf, size_t cap, const char *lang) { snprintf(buf, cap, "%s/%s/index.bin", TTS_DIR, lang); }
static void pcm_path(char *buf, size_t cap, const char *lang) { snprintf(buf, cap, "%s/%s/clips.pcm", TTS_DIR, lang); }

// Path cache deterministico per le pronunce a SINGOLO slug (read_it, identita', "non lo so", risposte
// fisse...): il WAV dipende solo da (lang, slug), quindi si assembla UNA volta e si riusa -> alle
// ripetizioni niente assemblaggio ne' scrittura SD (su questo HW la SD condivide il bus SPI col display:
// meno scrittura = meno blocco UI). I slug sono alfanumerici/underscore -> nomi file sicuri; l'insieme e'
// limitato dal vocabolario di clip fisse (decine di file piccoli, crescita naturalmente bounded).
// NB: se si AGGIORNA il pacchetto voce (clips.pcm) la cache va invalidata a mano: rm /data/tts/c_*.wav.
static void cache_path(char *buf, size_t cap, const char *lang, const char *slug)
{
    snprintf(buf, cap, "%s/c_%s_%s.wav", TTS_DIR, lang, slug);
}

// callback del planner: lo slug esiste se l'indice (passato in ud) lo trova.
static bool has_clip(const char *slug, void *ud)
{
    return ud && slug && slug[0] && tts_index_find((tts_index_t *)ud, slug, NULL, NULL);
}

// ---- interruttore parla (persistito) ----------------------------------------------------------
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

// ---- velocita' di lettura (persistita) --------------------------------------------------------
// La cache per-slug "cuoce" il rate nell'header del WAV: cambiando velocita' va buttata, cosi' i fissi
// (read_it/identita'/"non lo so"...) si ri-assemblano al nuovo passo. Pochi file piccoli, e succede SOLO
// quando l'utente muove lo slider (raro) -> purge O(n) accettabile. Niente ricorsione: la cache sta tutta
// in TTS_DIR (i blob clips.pcm/index.bin stanno nelle sottocartelle it/en, qui non li tocchiamo).
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

// Migrazione cache all'avvio: se il formato dei WAV resi e' cambiato (es. ora con anti-click sui bordi),
// i WAV memoizzati per-slug hanno il vecchio PCM "cotto" -> vanno buttati una volta, cosi' anche le frasi
// fisse (read_it/identita'/"non lo so") suonano col nuovo trattamento. Self-invalidante (scrive la ver).
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
    bool changed = (v != s_speed_pct);
    s_speed_pct = v;
    FILE *f = fopen(SPEED_PATH, "wb");
    if (f) { char buf[8]; int k = snprintf(buf, sizeof buf, "%d", v); if (k > 0) fwrite(buf, 1, (size_t)k, f); fclose(f); }
    if (changed) purge_clip_cache();    // i WAV cache hanno il vecchio rate cotto nell'header
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
    migrate_cache();    // se il formato dei WAV resi e' cambiato (anti-click), butta i WAV cache stantii
    bool ok = voice_installed(s_def_lang);
    ESP_LOGI(TAG, "voce %s: %s, parla=%s, vel=%d%%", s_def_lang, ok ? "installata" : "assente", s_enabled ? "ON" : "OFF", s_speed_pct);
    return ok;
}
bool nucleo_tts_available(void) { return voice_installed(s_def_lang); }
void nucleo_tts_stop(void)      { nucleo_audio_stop(); }   // staged API: no caller yet (TTS trigger pending)

// ---- WAV out + assemblaggio -------------------------------------------------------------------
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

// Copia `len` byte di PCM dal blob (a offset `off`) nell'uscita. -> byte scritti. Smussa i bordi della
// clip (anti-click): trasforma in loco ogni chunk PRIMA di scriverlo, tenendo conto di dove cade nella
// clip (`copied`). Nessun cambio alla struttura IO/FD dello streaming: solo un transform puro inserito.
static uint32_t copy_clip(FILE *blob, FILE *out, uint32_t off, uint32_t len, uint8_t *buf, size_t bufsz)
{
    if (fseek(blob, (long)off, SEEK_SET) != 0) return 0;
    uint32_t left = len, written = 0, copied = 0;
    while (left) {
        size_t want = left < bufsz ? left : bufsz;
        size_t got = fread(buf, 1, want, blob);
        if (!got) break;
        nucleo_tts_declick_chunk(buf, (int)got, copied, len, TTS_DECLICK_SAMPLES);   // bordi clip -> ~0 (no "tic")
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

// Assembla i token (CLIP letti dal blob via indice, PAUSE = silenzio) in `out_path` (WAV) e suona.
// NB: `ix` e' PRESTATO dal chiamante (say/say_one_clip), che lo chiude SEMPRE dopo questo render
// su ogni return — qui NON va chiuso (sarebbe un double-close). render gestisce solo blob e out.
static bool render(const char *lang, const tts_token_t *tok, int n, tts_index_t *ix, const char *out_path)
{
    char bp[64]; pcm_path(bp, sizeof bp, lang);
    FILE *blob = fopen(bp, "rb");
    if (!blob) { ESP_LOGW(TAG, "blob mancante: %s", bp); return false; }
    nucleo_audio_stop();                              // libera l'uscita da una lettura in corso
    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(blob); ESP_LOGE(TAG, "open out failed"); return false; }

    // VELOCITA' (zero-RAM): l'header dichiara base*velocita%, cosi' il DAC scandisce gli stessi campioni
    // piu' in fretta -> clip E pause (silenzio generato sotto a rate BASE) si accorciano insieme. Nessun
    // sample toccato. A 100% e' identico a prima. Il rate cotto qui e' anche quello del WAV in cache.
    uint32_t out_rate = nucleo_tts_speed_rate(ix->rate, s_speed_pct);
    uint8_t buf[1024], hdr[44];
    wav_header(hdr, 0, out_rate); fwrite(hdr, 1, 44, out);
    uint32_t data = 0, off, len;
    for (int i = 0; i < n; i++) {
        if (tok[i].kind == TTS_TOK_PAUSE)
            data += put_silence(out, tok[i].ms, ix->rate, buf, sizeof buf);   // ms a rate BASE: si accorcia col play
        else if (tok[i].kind == TTS_TOK_CLIP && tts_index_find(ix, tok[i].slug, &off, &len))
            data += copy_clip(blob, out, off, len, buf, sizeof buf);
        // UNKNOWN o slug assente: saltato (la coverage e' gia' decisa a monte)
    }
    wav_header(hdr, data, out_rate);
    fseek(out, 0, SEEK_SET); fwrite(hdr, 1, 44, out);
    fclose(out); fclose(blob);

    if (data == 0) { remove(out_path); return false; }   // niente WAV monco in cache
    esp_err_t e = nucleo_audio_play(out_path);
    if (e == ESP_OK) nucleo_audio_fade_in(60);
    else ESP_LOGW(TAG, "play '%s' FALLITO (err %d) -> voce muta su questo enunciato", out_path, (int)e);
    return e == ESP_OK;
}

// Suona la singola clip `slug` (es. "read_it"). MEMOIZZATO: se il WAV (lang,slug) e' gia' in cache lo
// riproduce diretto (zero assemblaggio, zero scrittura SD); altrimenti lo assembla NEL file cache una
// volta. E' il path piu' caldo del TTS (tutti i fallback "leggila" + le risposte fisse passano di qui).
static bool say_one_clip(const char *lang, const char *slug)
{
    char cache[96]; cache_path(cache, sizeof cache, lang, slug);
    char bp[64]; pcm_path(bp, sizeof bp, lang);
    struct stat st, sb;
    // cache-hit SOLO se il WAV esiste, e' completo, e NON e' piu' vecchio del pacchetto voce: cosi' un
    // aggiornamento di clips.pcm invalida da se' i WAV stantii (se i timestamp FAT non sono affidabili
    // degrada al caso peggiore = comportamento manuale, mai ad audio errato).
    if (stat(cache, &st) == 0 && st.st_size > 44 &&
        !(stat(bp, &sb) == 0 && sb.st_mtime > st.st_mtime)) {
        esp_err_t e = nucleo_audio_play(cache);           // play() ferma da se' l'audio in corso
        if (e == ESP_OK) nucleo_audio_fade_in(60);
        return e == ESP_OK;
    }
    char p[64]; idx_path(p, sizeof p, lang);
    tts_index_t ix;
    if (!tts_index_open(&ix, p)) return false;
    tts_token_t t; memset(&t, 0, sizeof t); t.kind = TTS_TOK_CLIP;
    snprintf(t.slug, sizeof t.slug, "%s", slug);
    bool r = render(lang, &t, 1, &ix, cache);             // assembla UNA volta nel file cache
    tts_index_close(&ix);
    return r;
}

// Motore di say(): pianifica e, se TUTTO e' coperto, pronuncia. Se NON lo e' (codice/troppo lungo o
// parola scoperta): con `fallback` non-NULL pronuncia QUELLO (conferma breve, es. "Fatto") invece di
// "leggila" -> per le operazioni l'ESITO conta piu' del testo esatto (nomi file, dettagli evento).
static bool say_impl(const char *text, const char *lang, const char *fallback)
{
    if (!s_enabled || !text || !text[0]) return false;
    const char *l = (lang && lang[0]) ? lang : s_def_lang;

    // "Parlabilizza" i simboli matematici (= % ^) PRIMA di tutto: senza, '=' farebbe scattare la guardia
    // e tutta la frase finirebbe in "leggila" (era il "Fa 16 non si pronuncia"). No-op sul testo normale.
    char mtext[416];
    nucleo_tts_mathspeak(text, mtext, sizeof mtext, l);
    text = mtext;

    // Guardia OFFLINE: troppo lungo o "sa di codice" -> non leggere porzioni incomprensibili. (Gira gia'
    // sul task TTS: il ripiego a "leggila" e' SINCRONO qui, non un nuovo enqueue.)
    if (!nucleo_tts_text_speakable(text, TTS_MAX_CHARS))
        return (fallback && fallback[0]) ? say_impl(fallback, l, NULL) : say_one_clip(l, "read_it");

    char p[64]; idx_path(p, sizeof p, l);
    tts_index_t ix;
    if (!tts_index_open(&ix, p)) { ESP_LOGW(TAG, "voce %s non installata", l); return false; }

    // Risposta FISSA intera (identita', "non lo so", pairing...): se l'intero testo e' una clip unica,
    // suonala (prosodia naturale, oltre il limite di 6 parole del match-frase del planner).
    char full[TTS_IDX_SLUG]; uint32_t foff, flen;
    nucleo_tts_full_slug(text, full, sizeof full);
    if (full[0] && tts_index_find(&ix, full, &foff, &flen)) {
        tts_index_close(&ix);             // e' una singola clip: delega al path memoizzato (cache-abile)
        return say_one_clip(l, full);
    }

    // 96 token bastano (testo <=140 char dalla guardia); ~5.4KB heap, liberati subito dopo render.
    enum { TOK_MAX = 96 };
    tts_token_t *tok = malloc(TOK_MAX * sizeof(tts_token_t));
    if (!tok) { tts_index_close(&ix); return false; }
    int n = nucleo_tts_plan(text, l, has_clip, &ix, tok, TOK_MAX);

    int unknown = 0;
    char miss[160]; miss[0] = 0;
    for (int i = 0; i < n; i++) if (tok[i].kind == TTS_TOK_UNKNOWN) {
        unknown++;
        size_t ml = strlen(miss);
        if (ml + strlen(tok[i].slug) + 3 < sizeof miss) { if (ml) strcat(miss, ", "); strcat(miss, tok[i].slug); }
    }

    if (n <= 0 || unknown > 0) {
        free(tok); tts_index_close(&ix);                 // chiudi PRIMA di ripiegare (max 1 indice aperto)
        if (fallback && fallback[0]) return say_impl(fallback, l, NULL);   // conferma breve invece di "leggila"
        // DIAGNOSI: logga le clip scoperte (in /api/logs si vede ESATTAMENTE cosa manca).
        if (unknown > 0) ESP_LOGW(TAG, "\"%s\": %d clip scoperte [%s] -> read_it (pacchetto voce incompleto?)", text, unknown, miss);
        else             ESP_LOGW(TAG, "\"%s\": planner vuoto -> read_it", text);
        return say_one_clip(l, "read_it");
    }
    bool r = render(l, tok, n, &ix, OUT_PATH);   // frase variabile: non memoizzabile -> file scratch
    free(tok);
    tts_index_close(&ix);
    return r;
}

// API pubblica: SINCRONA (la pronuncia gira sul task chiamante). Su questo HW stretto (8 file aperti
// max, heap ~frammentato) un task TTS dedicato costava troppo (10KB stack + 3 FD concorrenti) e
// esauriva FD/heap (rompeva musica + apertura _say.wav). La protezione anti-freeze e' il FATFS-timeout
// (5s < watchdog 8s) + pet del watchdog: una SD lenta degrada a ritardo breve, non a hang/reboot.
bool nucleo_tts_read_hint(const char *lang)
{
    if (!s_enabled) return false;
    return say_one_clip((lang && lang[0]) ? lang : s_def_lang, "read_it");
}
bool nucleo_tts_say(const char *text, const char *lang)                            { return say_impl(text, lang, NULL); }
bool nucleo_tts_say_or(const char *text, const char *fallback, const char *lang)   { return say_impl(text, lang, fallback); }
