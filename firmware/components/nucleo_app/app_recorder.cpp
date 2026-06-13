// Voice Recorder v2 — record the Cardputer mic to a WAV on the microSD (/data/Recordings) with a
// live scrolling waveform + peak meter, then turn each take into a Grok-powered note.
//
//  LIBRARY  : takes newest-first, date/time + duration, AI badge. Enter plays (live progress bar,
//             Space pause, , . seek); A opens the AI reader; t auto-titles; n renames; d deletes.
//  AI READER: a music-player-style tabbed sheet over one take — SUMMARY / SCRIPT (transcript) /
//             ACTIONS (to-dos) / ASK (Q&A). RIGHT/LEFT (or 1-4) switch tabs; Up/Down scroll; Enter
//             generates the missing artifact (or asks a question); Space still plays the take so you
//             can listen while you read. Each artifact is cached next to the take as a sidecar
//             (.txt/.sum.txt/.act.txt/.ask.txt) and generated off the UI thread, so the screen never
//             freezes on the network.
//  SETTINGS : TAB opens a 2-tab sheet — AI (auto-summary, auto-title, language) / INFO (Grok status,
//             storage, take count).
//
// The cloud chain (no on-device ASR exists) is WAV -> Whisper (auto-language) -> teacher LLM, shared
// with /api/transcribe via nucleo_anima_transcribe/summarize/actions/qa/title. The take audio is kept
// (a recorder must never lose the recording); only the user deletes it.
//
// Input note: NK_LEFT and NK_BACK never reach on_key — the framework routes them to the back handler
// (rec_back), so tabs cycle with RIGHT and Left/Esc are handled there; action keys are char keys.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
extern "C" {
#include "nucleo_recorder.h"
#include "nucleo_audio.h"
#include "nucleo_storage.h"
#include "nucleo_board.h"
#include "nucleo_ui.h"
#include "nucleo_anima.h"          // cloud transcribe + summary/actions/qa/title (no on-device ASR)
#include "nucleo_exclusive.h"      // dedicated-mode RAM reclaim for the heavy TLS audio upload (Wi-Fi stays)
#include "notify_synth.h"          // zero-RAM polyphonic synth -> the GO push-to-talk start/stop cues
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

static const char *REC_TAG = "recorder";

#include "app_gfx.h"

// ---- Design tokens (RGB565), aligned with the Music player for a cohesive look -------------------
static const unsigned short BG = 0x0841, SURF = 0x10A2, CAP = 0x1A8B, FG = 0xFFFF, MUTED = 0x8C71,
                            DIM = 0x52CB, LINE = 0x2945, ACC = 0xF96B /* recorder warm-red accent */,
                            GRN = 0x8FF3, AMBER = 0xFE8C, VIO = 0xCDBF, WAVE = 0x3B6E, INK = 0x0000;

#define REC_PATH      NUCLEO_SD_MOUNT "/data/Recordings"
#define SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/recorder_ui.json"
#define REC_BPS  (16000 * 2)            // 16 kHz mono 16-bit = bytes/sec (matches nucleo_recorder)
#define WAV_HDR  44
#define WAVE_N   72                     // scrolling waveform columns
#define TABBAR_H 22                     // segmented tab-bar height
#define REC_MAX  24
#define REC_TXTBUF 4096                 // reader buffer (fits a full transcript)

// ---- modes & tabs ---------------------------------------------------------------------------------
enum { M_LIST = 0, M_DETAIL, M_SETTINGS, M_ASK };   // M_ASK = full-screen cross-note answer reader
enum { T_SUMMARY = 0, T_SCRIPT, T_ACTIONS, T_ASK, T_COUNT };
enum { ST_AI = 0, ST_INFO, ST_COUNT };
// UI language flag — cached in enter() from sys_lang() (SD read, too costly in draw loops).
// Must be declared before det_tab_label() which references it.
static bool s_en = false;

// Tab labels are language-selected at draw time via det_tab_label().
static const char *const DET_TABS_IT[] = { "RIASSUNTO", "TESTO", "AZIONI", "CHIEDI" };
static const char *const DET_TABS_EN[] = { "SUMMARY",   "SCRIPT", "ACTIONS", "ASK"  };
static const char *const SET_TABS[]    = { "AI", "INFO" };
static inline const char *det_tab_label(int i) { return s_en ? DET_TABS_EN[i] : DET_TABS_IT[i]; }

static int  s_mode    = M_LIST;
static int  s_tab     = T_SUMMARY;      // active AI-reader tab
static int  s_set_tab = ST_AI;
static int  s_set_row = 0;

// ---- persisted settings ---------------------------------------------------------------------------
static bool s_auto_summary = true;      // summarize automatically after a recording
static bool s_auto_title   = false;     // also rename the take from a Grok/Claude title
static int  s_lang         = 0;         // 0 auto, 1 it, 2 en

// ---- active cloud engine, cached on enter() (honest UI + "two brains": Claude/Grok reasons, Whisper hears)
static char s_eng_short[10] = "AI";     // "Claude" | "Grok" | "AI"
static char s_eng_model[40] = "";       // e.g. "claude-sonnet-4-6" / "llama-3.1-8b-instant"
static bool s_eng_ready     = false;    // a chat-teacher key is configured (any provider)

// ---- take list (malloc'd while OPEN, freed on exit so the RAM goes back to ANIMA's heap) ----------
struct Rec {
    char     name[48];                  // on-disk filename (with .wav)
    char     label[24];                 // list label: humanized date/time, or custom name
    uint32_t bytes;
    uint32_t secs;
    time_t   mtime;
    bool     has_ai;                    // a .sum.txt or .act.txt sidecar exists
};
static Rec *s_list = nullptr;
static int  s_count, s_sel, s_playing = -1;
static uint32_t s_total_secs;
static bool s_was_rec;

// ---- AI reader buffer (malloc'd only while the reader is open) ------------------------------------
static char *s_text = nullptr;          // current tab's text
static bool  s_empty = true;            // current tab has no content yet
static int   s_scroll = 0;
static int   s_total_lines = 0;   // last reader draw: total wrapped lines (deterministic scroll clamp)
static int   s_page_lines  = 1;   // last reader draw: lines that fit on screen (page-scroll step)
static bool  s_more = false;            // last draw reported text below the fold

// ---- background AI worker (one job at a time) -----------------------------------------------------
enum { JOB_TRANSCRIBE = 1, JOB_SUMMARY, JOB_ACTIONS, JOB_QA, JOB_TITLE, JOB_ASKALL };

// GO-button push-to-talk state machine (hold GO to record). Sequenced from on_tick because the speaker
// and PDM mic share the I2S word-select line (mutually exclusive): start cue -> mic -> stop -> end cue.
enum { PTT_IDLE = 0, PTT_ARMING, PTT_RECORDING, PTT_ENDCUE };
static int s_ptt = PTT_IDLE;
static volatile int s_job = 0;          // 0 idle, 1 working, 2 done, 3 error
static int   s_job_kind = 0;
static char  s_jobname[48];             // take filename being processed
static char  s_jobpath[80];             // its absolute path (REC_PATH/<name>)
static char  s_job_q[160];              // question (JOB_QA)
static bool  s_ai_screen_freed = false; // true while the 32 KB launcher canvas is released for the cloud TLS
static char  s_renamed[48];             // set by the worker after a title-rename; tick re-selects it

// ---- live recording visuals -----------------------------------------------------------------------
static uint8_t  s_wave[WAVE_N];
static int      s_wave_head;
static int64_t  s_wave_last_us;
static int      s_peak;

// ---- forward declarations -------------------------------------------------------------------------
static void scan(void);
static void play_sel(void);
static void detail_load(void);
static bool start_job(int kind, const char *q);
static void set_mode(int m);
static void set_hint_for_mode(void);
static void close_detail(void);
static void rec_tab(void);
static bool rec_back(int key);
static void ask_all(void);
static int  build_corpus(char *buf, int cap);

// ---- settings I/O ---------------------------------------------------------------------------------
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb"); if (!f) return;
    char buf[128]; int n = (int)fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf); if (!root) return;
    cJSON *a = cJSON_GetObjectItem(root, "auto_summary"); if (cJSON_IsBool(a)) s_auto_summary = cJSON_IsTrue(a);
    cJSON *t = cJSON_GetObjectItem(root, "auto_title");   if (cJSON_IsBool(t)) s_auto_title   = cJSON_IsTrue(t);
    cJSON *l = cJSON_GetObjectItem(root, "lang");         if (cJSON_IsNumber(l)) s_lang = (int)l->valuedouble;
    if (s_lang < 0 || s_lang > 2) s_lang = 0;
    cJSON_Delete(root);
}

static void save_settings(void)
{
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (f) {
        fprintf(f, "{\"auto_summary\":%s,\"auto_title\":%s,\"lang\":%d}\n",
                s_auto_summary ? "true" : "false", s_auto_title ? "true" : "false", s_lang);
        fclose(f);
    }
}

// ---- small helpers --------------------------------------------------------------------------------
static void wave_reset(void) { memset(s_wave, 0, sizeof(s_wave)); s_wave_head = 0; s_wave_last_us = 0; s_peak = 0; }

// Minimal system-language read (settings.json ui.language) — fallback when Whisper auto-detect fails.
static const char *sys_lang(void)
{
    static char l[4]; strcpy(l, "it");
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/settings.json", "r");
    if (f) { char b[1024]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        char *p = strstr(b, "\"language\"");
        if (p && (p = strchr(p, ':')) && (p = strchr(p, '"')) && !strncasecmp(p + 1, "en", 2)) strcpy(l, "en");
    }
    return l;
}

// Language hint for the worker: explicit setting, else "auto" (Whisper detects).
static const char *lang_hint(void) { return s_lang == 1 ? "it" : s_lang == 2 ? "en" : "auto"; }

// TR(it,en) picks the right string — keeps the small-screen labels readable in the user's language.
#define TR(it_, en_) (s_en ? (en_) : (it_))

static int load_text(const char *abs, char *buf, int cap)   // sidecar -> buf; returns length, 0 if absent
{
    FILE *f = fopen(abs, "r"); if (!f) { buf[0] = 0; return 0; }
    int n = (int)fread(buf, 1, cap - 1, f); fclose(f); if (n < 0) n = 0; buf[n] = 0; return n;
}

static void human_free(char *out, size_t n)
{
    const nucleo_storage_info_t *st = nucleo_storage_info();
    if (st && st->mounted && st->total_bytes) {
        double gb = st->free_bytes / 1e9;
        if (gb >= 1.0) snprintf(out, n, "%.1f GB free", gb);
        else           snprintf(out, n, "%u MB free", (unsigned)(st->free_bytes / 1000000ULL));
    } else out[0] = '\0';
}

// REC_PATH/<base-of-name><suffix>, e.g. ("rec-1.wav", ".sum.txt") -> ".../rec-1.sum.txt".
static void sidecar_of(char *out, int cap, const char *name, const char *suffix)
{
    char base[64]; snprintf(base, sizeof base, "%s", name);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    snprintf(out, cap, "%s/%s%s", REC_PATH, base, suffix);
}
static void write_sidecar(const char *name, const char *suffix, const char *body)
{
    char p[300]; sidecar_of(p, sizeof p, name, suffix);
    FILE *f = fopen(p, "w"); if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
}

// Newest first; fall back to name when timestamps tie (or the clock was unset at capture).
static int cmp(const void *a, const void *b)
{
    const Rec *x = (const Rec *)a, *y = (const Rec *)b;
    if (x->mtime != y->mtime) return (y->mtime > x->mtime) ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

// Auto-named takes (rec-*) show their date/time; a renamed take shows its custom name (minus .wav).
static void make_label(Rec *r)
{
    if (!strncmp(r->name, "rec-", 4) && r->mtime > 0) {
        struct tm tm; localtime_r(&r->mtime, &tm);
        strftime(r->label, sizeof(r->label), "%b %d  %H:%M", &tm);
        return;
    }
    size_t n = strlen(r->name);
    if (n >= sizeof(r->label)) n = sizeof(r->label) - 1;
    memcpy(r->label, r->name, n); r->label[n] = '\0';
    char *dot = strrchr(r->label, '.');
    if (dot && !strcasecmp(dot, ".wav")) *dot = '\0';
}

static void scan(void)
{
    s_count = 0; s_total_secs = 0;
    if (!s_list) return;
    DIR *dir = opendir(REC_PATH);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && s_count < REC_MAX) {
            if (e->d_name[0] == '.') continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".wav")) continue;
            Rec *r = &s_list[s_count++];
            snprintf(r->name, sizeof(r->name), "%s", e->d_name);
            char abs[256]; snprintf(abs, sizeof(abs), "%s/%s", REC_PATH, e->d_name);
            struct stat st;
            if (stat(abs, &st) == 0) { r->bytes = (uint32_t)st.st_size; r->mtime = st.st_mtime; }
            else                     { r->bytes = 0; r->mtime = 0; }
            r->secs = r->bytes > WAV_HDR ? (r->bytes - WAV_HDR) / REC_BPS : 0;
            s_total_secs += r->secs;
            char sf[300]; struct stat ss;
            sidecar_of(sf, sizeof sf, e->d_name, ".sum.txt"); r->has_ai = (stat(sf, &ss) == 0);
            if (!r->has_ai) { sidecar_of(sf, sizeof sf, e->d_name, ".act.txt"); r->has_ai = (stat(sf, &ss) == 0); }
            make_label(r);
        }
        closedir(dir);
        qsort(s_list, s_count, sizeof(Rec), cmp);
    }
    if (s_sel >= s_count) s_sel = s_count ? s_count - 1 : 0;
}

static void play_sel(void)
{
    if (nucleo_recorder_is_recording() || s_sel >= s_count) return;     // record XOR play
    char abs[256]; snprintf(abs, sizeof(abs), "%s/%s", REC_PATH, s_list[s_sel].name);
    if (nucleo_audio_play(abs) == ESP_OK) s_playing = s_sel;
}

static void seek_playing(bool fwd)
{
    uint32_t el = nucleo_audio_elapsed_ms(), dur = nucleo_audio_duration_ms();
    long t = (long)el + (fwd ? 5000 : -5000);
    if (t < 0) t = 0;
    if (dur && t > (long)dur - 500) t = (long)dur - 500;
    nucleo_audio_seek((uint32_t)t);
    nucleo_app_request_draw();
}

static void delete_sel(void)
{
    if (nucleo_recorder_is_recording() || s_count == 0) return;
    char title[40]; snprintf(title, sizeof(title), "Delete %s?", s_list[s_sel].label);
    const char *opts[] = { "Cancel", "Delete" };
    if (nucleo_ui_menu(title, opts, 2) != 1) return;
    if (s_playing == s_sel) { nucleo_audio_stop(); s_playing = -1; }
    // Remove the take and every AI sidecar that belongs to it.
    static const char *const SX[] = { ".wav", ".txt", ".sum.txt", ".act.txt", ".ask.txt" };
    for (int k = 0; k < 5; k++) { char p[300]; sidecar_of(p, sizeof p, s_list[s_sel].name, SX[k]); unlink(p); }
    scan();
    if (s_sel >= s_count) s_sel = s_count ? s_count - 1 : 0;
}

// Strip unsafe / decorative chars and clamp length so "<base>.wav" stays within Rec.name[48].
static void sanitize_name(const char *src, char *dst, int cap)
{
    int j = 0; bool sp = false;
    for (const char *p = src; *p && j < cap - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') continue;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') { if (j == 0 || sp) continue; sp = true; dst[j++] = ' '; continue; }
        sp = false; dst[j++] = (char)c;
    }
    while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '.')) j--;   // trim trailing space/dot
    dst[j] = 0;
}

// Rename a take and ALL its sidecars to <newbase> (collision-safe). Records s_renamed for re-select.
static void rename_take(const char *oldname, const char *newbase)
{
    char obase[64]; snprintf(obase, sizeof obase, "%s", oldname);
    { char *dot = strrchr(obase, '.'); if (dot) *dot = 0; }
    if (!newbase[0] || !strcmp(obase, newbase)) return;

    char fbase[48]; snprintf(fbase, sizeof fbase, "%.38s", newbase);
    for (int i = 2; i <= 30; i++) {                              // resolve a free slot
        char p[300]; snprintf(p, sizeof p, "%s/%s.wav", REC_PATH, fbase);
        struct stat ss; if (stat(p, &ss) != 0) break;
        snprintf(fbase, sizeof fbase, "%.34s %d", newbase, i);
    }
    static const char *const SX[] = { ".wav", ".txt", ".sum.txt", ".act.txt", ".ask.txt" };
    for (int k = 0; k < 5; k++) {
        char from[300], to[300];
        snprintf(from, sizeof from, "%s/%s%s", REC_PATH, obase, SX[k]);
        snprintf(to,   sizeof to,   "%s/%s%s", REC_PATH, fbase, SX[k]);
        struct stat ss; if (stat(from, &ss) == 0) rename(from, to);
    }
    snprintf(s_renamed, sizeof s_renamed, "%s.wav", fbase);
}

// Manual rename from the library (n).
static void rename_sel(void)
{
    if (nucleo_recorder_is_recording() || s_count == 0) return;
    char nm[40];
    snprintf(nm, sizeof(nm), "%s", s_list[s_sel].name);
    char *dot = strrchr(nm, '.'); if (dot && !strcasecmp(dot, ".wav")) *dot = '\0';
    nucleo_ui_input("Rename note", nm, sizeof(nm), 0);
    char clean[40]; sanitize_name(nm, clean, sizeof clean);
    if (!clean[0]) return;
    if (s_playing == s_sel) { nucleo_audio_stop(); s_playing = -1; }
    s_renamed[0] = 0;
    rename_take(s_list[s_sel].name, clean);
    scan();
    if (s_renamed[0]) for (int i = 0; i < s_count; i++) if (!strcmp(s_list[i].name, s_renamed)) { s_sel = i; break; }
}

// ---- background AI worker --------------------------------------------------------------------------
static bool a_has_suffix(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

// Gather every take's transcript (.txt, excluding the AI .sum/.act/.ask variants) into `buf`, each
// prefixed with its take name, capped to `cap`. Returns bytes written. This is the device's whole
// job for cross-note Q&A: it just concatenates small text; Claude (1M context) does the reasoning
// over the entire voice-note history. Snippets are bounded so one long note can't crowd out the rest.
static int build_corpus(char *buf, int cap)
{
    int len = 0; if (cap > 0) buf[0] = 0;
    DIR *dir = opendir(REC_PATH); if (!dir) return 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && len < cap - 800) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".txt")) continue;                  // transcripts end in .txt
        if (a_has_suffix(e->d_name, ".sum.txt") || a_has_suffix(e->d_name, ".act.txt") ||
            a_has_suffix(e->d_name, ".ask.txt")) continue;              // skip the AI derivatives
        char p[300]; snprintf(p, sizeof p, "%s/%s", REC_PATH, e->d_name);
        char snip[700]; int n = load_text(p, snip, sizeof snip);
        if (n <= 0) continue;
        int blen = (int)(dot - e->d_name);                             // take name without ".txt"
        len += snprintf(buf + len, cap - len, "# %.*s\n%.640s\n\n", blen, e->d_name, snip);
    }
    closedir(dir);
    return len;
}

// Ensure a transcript exists (cloud Whisper), then derive whatever the job asked for, writing the
// sidecars. Runs OFF the UI task so the screen never freezes on the network.
static void do_title(const char *text, const char *lang)
{
    char t[96];
    if (nucleo_anima_title(text, lang, t, sizeof t) <= 0) return;
    char clean[40]; sanitize_name(t, clean, sizeof clean);
    if ((int)strlen(clean) < 2) return;
    rename_take(s_jobname, clean);
}

static void ai_task(void *arg)
{
    (void)arg;
    char *text = (char *)malloc(4096), *out = (char *)malloc(1024); char det[16] = {0};
    if (!text || !out) {
        ESP_LOGE(REC_TAG, "ai_task OOM at entry (free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(text); free(out); s_job = 3; vTaskDelete(NULL); return;
    }
    ESP_LOGI(REC_TAG, "ai_task start kind=%d file=%s free=%u largest=%u",
             s_job_kind, s_jobname,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Enter DEDICATED MODE: reclaim httpd + L1 + mDNS + voice (~70 KB) so TLS has enough heap.
    bool nx = false;
    if (nucleo_anima_online_available()) {
        nucleo_exclusive_info_t inf;
        nx = nucleo_exclusive_enter(NX_NET_APP, &inf);
        ESP_LOGI(REC_TAG, "exclusive=%d post-reclaim free=%u largest=%u",
                 (int)nx, (unsigned)inf.free_after, (unsigned)inf.largest_after);
        // RAM gate aligned with the ACTUAL cloud-TLS need: ~38 KB free + a ~10 KB contiguous block
        // (mbedTLS variable buffers grow as needed — same condition online_tls_heap_too_low uses
        // successfully for the web path). The OLD "largest < 40 KB CONTIGUOUS" was ~4x stricter than the
        // real handshake and rejected a workable heap (e.g. the just-freed 32 KB canvas block), which is
        // exactly why the summary kept aborting. The per-request online_tls_heap_too_low gate inside
        // transcribe/summarize is the FINAL OOM safety, so bailing here only skips a doomed attempt.
        if (inf.free_after < 40 * 1024 || inf.largest_after < 16 * 1024) {
            ESP_LOGE(REC_TAG, "ai_task: heap too low (free=%u largest=%u) — aborting",
                     (unsigned)inf.free_after, (unsigned)inf.largest_after);
            s_job = 3; goto done;
        }
    } else {
        ESP_LOGW(REC_TAG, "ai_task: device offline — no AI possible");
        s_job = 3; goto done;
    }

    if (s_job_kind == JOB_ASKALL) {
        if (build_corpus(text, 4096) <= 0) {
            ESP_LOGW(REC_TAG, "askall: corpus empty"); s_job = 3; goto done;
        }
        if (nucleo_anima_qa(text, s_job_q, sys_lang(), out, 1024) <= 0) {
            ESP_LOGW(REC_TAG, "askall: qa failed"); s_job = 3; goto done;
        }
        char p[300]; snprintf(p, sizeof p, "%s/.askall.txt", REC_PATH);
        FILE *f = fopen(p, "w"); if (f) { fprintf(f, "Q: %s\n\n", s_job_q); fwrite(out, 1, strlen(out), f); fclose(f); }
        s_job = 2; goto done;
    }

    {
    char tpath[300]; sidecar_of(tpath, sizeof tpath, s_jobname, ".txt");
    int tl = load_text(tpath, text, 4096);
    if (tl <= 0) {                                               // need to transcribe first
        ESP_LOGI(REC_TAG, "transcribing %s (lang_hint=%s)", s_jobname, lang_hint());
        tl = nucleo_anima_transcribe(s_jobpath, lang_hint(), text, 4096, det, sizeof det);
        if (tl < 0 && s_lang == 0) {
            ESP_LOGW(REC_TAG, "transcribe auto failed, retrying with sys_lang=%s", sys_lang());
            tl = nucleo_anima_transcribe(s_jobpath, sys_lang(), text, 4096, det, sizeof det);
        }
        if (tl < 0) {
            ESP_LOGE(REC_TAG, "transcribe FAILED for %s — check Whisper key/heap", s_jobname);
            s_job = 3; goto done;
        }
        ESP_LOGI(REC_TAG, "transcribe OK: %d chars, lang=%s", tl, det[0] ? det : "?");
        write_sidecar(s_jobname, ".txt", text);
    } else {
        ESP_LOGI(REC_TAG, "transcript cached (%d chars), skipping Whisper", tl);
    }
    const char *lang = det[0] ? det : (s_lang == 2 ? "en" : s_lang == 1 ? "it" : sys_lang());
    switch (s_job_kind) {
    case JOB_SUMMARY:
        if (nucleo_anima_summarize(text, lang, out, 1024) <= 0) {
            ESP_LOGE(REC_TAG, "summarize FAILED"); s_job = 3; goto done;
        }
        write_sidecar(s_jobname, ".sum.txt", out);
        if (s_auto_title) do_title(text, lang);
        break;
    case JOB_ACTIONS:
        if (nucleo_anima_actions(text, lang, out, 1024) <= 0) {
            ESP_LOGE(REC_TAG, "actions FAILED"); s_job = 3; goto done;
        }
        write_sidecar(s_jobname, ".act.txt", out);
        break;
    case JOB_QA: {
        if (nucleo_anima_qa(text, s_job_q, lang, out, 1024) <= 0) {
            ESP_LOGE(REC_TAG, "qa FAILED"); s_job = 3; goto done;
        }
        char p[300]; sidecar_of(p, sizeof p, s_jobname, ".ask.txt");
        FILE *f = fopen(p, "w"); if (f) { fprintf(f, "Q: %s\n\n", s_job_q); fwrite(out, 1, strlen(out), f); fclose(f); }
        break; }
    case JOB_TITLE:
        do_title(text, lang);
        break;
    default: break;
    }
    s_job = 2;
    ESP_LOGI(REC_TAG, "ai_task kind=%d DONE OK", s_job_kind);
    }
done:
    if (s_job == 3) ESP_LOGE(REC_TAG, "ai_task kind=%d FAILED → s_job=3", s_job_kind);
    if (nx) nucleo_exclusive_exit();
    free(text); free(out);
    vTaskDelete(NULL);
}

// FREE the 32 KB launcher canvas for the duration of an AI job. The job's cloud TLS handshake needs a
// big CONTIGUOUS block (~40 KB); the recorder's exclusive-mode reclaim alone left the canvas sitting
// mid-heap, capping the largest free block at ~31 KB → the job aborted. Releasing the canvas (like the
// media apps do) gives the contiguous RAM back. UI-task ONLY (start_job/ask_all/tick), so the worker
// never races draw(). Restored by tick() the moment the job ends; the next app's enter re-acquires it
// anyway, so a mid-job leave can't strand the buffer.
static void ai_screen_free(void)
{
    if (s_ai_screen_freed) return;
    nucleo_app_set_direct_draw(true);   // tell the launcher NOT to lazily re-acquire the canvas
    nucleo_screen_release();            // give ~32 KB back to the heap for the TLS
    s_ai_screen_freed = true;
}
static void ai_screen_restore(void)
{
    if (!s_ai_screen_freed) return;
    s_ai_screen_freed = false;
    nucleo_app_set_direct_draw(false);
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_request_draw();
}

static bool start_job(int kind, const char *q)
{
    if (s_job == 1 || s_count == 0 || nucleo_recorder_is_recording() || s_sel >= s_count) return false;
    snprintf(s_jobname, sizeof s_jobname, "%s", s_list[s_sel].name);
    snprintf(s_jobpath, sizeof s_jobpath, "%s/%s", REC_PATH, s_jobname);
    s_job_kind = kind; s_renamed[0] = 0;
    if (q) snprintf(s_job_q, sizeof s_job_q, "%s", q); else s_job_q[0] = 0;
    s_job = 1;
    ai_screen_free();   // free the 32 KB canvas so the TLS gets a contiguous block (restored in tick when the job ends)
    // 16 KB stack: the mbedTLS handshake to the cloud is stack-hungry (mirrors the ANIMA worker).
    if (xTaskCreate(ai_task, "rec-ai", 16384, nullptr, tskIDLE_PRIORITY + 2, nullptr) != pdPASS) { s_job = 3; ai_screen_restore(); return false; }
    return true;
}

// Cross-note Q&A: ask one question against EVERY transcribed note at once (Claude/Grok long context).
// A second brain over your whole voice-note history — the device only gathers the text.
static void ask_all(void)
{
    if (s_job == 1 || s_count == 0 || nucleo_recorder_is_recording()) return;
    char q[160] = "";
    nucleo_ui_input(TR("Chiedi a tutte le note", "Ask across all notes"), q, sizeof q, 0);
    if (!q[0]) { nucleo_app_request_draw(); return; }
    snprintf(s_job_q, sizeof s_job_q, "%s", q);
    s_job_kind = JOB_ASKALL; s_renamed[0] = 0; s_job = 1;
    ai_screen_free();
    if (xTaskCreate(ai_task, "rec-ai", 16384, nullptr, tskIDLE_PRIORITY + 2, nullptr) != pdPASS) { s_job = 3; ai_screen_restore(); }
    nucleo_app_request_draw();
}

static int kind_for_tab(int t) { return t == T_SCRIPT ? JOB_TRANSCRIBE : t == T_SUMMARY ? JOB_SUMMARY : t == T_ACTIONS ? JOB_ACTIONS : JOB_QA; }

// ---- shared drawing widgets -----------------------------------------------------------------------
// Persistent segmented tab bar (mirrors the Music player): active tab is a filled capsule.
// Overload accepting a label-getter function for language-aware tabs.
static void draw_tabbar(int y0, const char *const *labels, int n, int active)
{
    d.fillRect(0, y0, 240, TABBAR_H, BG);
    int seg = 240 / (n < 1 ? 1 : n);
    d.setTextSize(1);
    for (int i = 0; i < n; i++) {
        int x = i * seg, maxc = (seg - 6) / 6; if (maxc < 1) maxc = 1;
        char lab[16]; snprintf(lab, sizeof lab, "%.*s", maxc, labels[i]);
        int tw = (int)strlen(lab) * 6;
        if (i == active) { d.fillRoundRect(x + 3, y0 + 2, seg - 6, TABBAR_H - 5, 7, ACC); d.setTextColor(INK, ACC); }
        else             { d.setTextColor(MUTED, BG); }
        d.setCursor(x + (seg - tw) / 2, y0 + 7); d.print(lab);
    }
    d.drawFastHLine(0, y0 + TABBAR_H - 1, 240, LINE);
}
static void draw_tabbar_fn(int y0, const char *(*fn)(int), int n, int active)
{
    d.fillRect(0, y0, 240, TABBAR_H, BG);
    int seg = 240 / (n < 1 ? 1 : n);
    d.setTextSize(1);
    for (int i = 0; i < n; i++) {
        int x = i * seg;
        const char *lbl = fn(i);
        int maxc = (seg - 6) / 6; if (maxc < 1) maxc = 1;
        char lab[16]; snprintf(lab, sizeof lab, "%.*s", maxc, lbl);
        int tw = (int)strlen(lab) * 6;
        if (i == active) { d.fillRoundRect(x + 3, y0 + 2, seg - 6, TABBAR_H - 5, 7, ACC); d.setTextColor(INK, ACC); }
        else             { d.setTextColor(MUTED, BG); }
        d.setCursor(x + (seg - tw) / 2, y0 + 7); d.print(lab);
    }
    d.drawFastHLine(0, y0 + TABBAR_H - 1, 240, LINE);
}

// Word-wrap `buf` into x..x+w (FreeSans9pt7b), drawing from logical line `scroll`, up to `maxlines`.
// Returns true if text remains below the last drawn line (for a "more below" indicator).
static bool draw_wrapped(const char *buf, int x, int y, int w, int LH, int maxlines, int scroll, int *out_total = nullptr)
{
    d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
    int drawn = 0, idx = 0;
    const char *p = buf;
    while (*p) {
        if (!out_total && drawn >= maxlines) break;
        if (*p == '\n') { 
            if (idx >= scroll && drawn < maxlines) { y += LH; drawn++; } 
            idx++; p++; continue; 
        }
        if (*p == ' ' && (p == buf || p[-1] == '\n')) { p++; continue; }
        const char *we = p;
        while (*we && *we != '\n') {
            const char *nsp = strchr(we, ' ');
            const char *nnl = strchr(we, '\n');
            const char *end_word = nsp ? nsp : (nnl ? nnl : we + strlen(we));
            if (nnl && nnl < end_word) end_word = nnl;
            char tmp[100]; int len = (int)(end_word - p); if (len > 99) len = 99;
            memcpy(tmp, p, len); tmp[len] = 0;
            if (d.textWidth(tmp) > w) {
                if (we == p) {                                   // single word too long -> hard break
                    int t = 1;
                    while (p + t <= end_word) { memcpy(tmp, p, t); tmp[t] = 0; if (d.textWidth(tmp) > w) { t--; break; } t++; }
                    we = p + (t > 0 ? t : 1);
                }
                break;
            }
            we = end_word; if (*we == ' ') we++;
            if (*we == '\n' || *we == 0) break;
        }
        if (idx >= scroll && drawn < maxlines) {
            char line[100]; int len = (int)(we - p); if (len > 99) len = 99;
            memcpy(line, p, len); line[len] = 0;
            d.setTextColor(FG, BG); d.setCursor(x, y); d.print(line); 
            y += LH; drawn++; 
        }
        idx++; p = we;
    }
    d.setFont(&fonts::Font0);
    if (out_total) *out_total = idx;
    return out_total ? (idx > scroll + maxlines) : (*p != 0);
}

// Deterministic reader scrolling shared by the detail + ask readers. Clamps s_scroll against the LAST
// draw's measured counts (s_total_lines/s_page_lines) so DOWN always reaches the true bottom — the old
// "scroll only while s_more" gating (a draw side-effect) stopped a line short on long notes, which is
// why the text "wouldn't scroll all the way down". dir: -1/+1 line, -2/+2 page. Returns true if moved.
static bool reader_scroll(int dir)
{
    int maxs = s_total_lines - s_page_lines; if (maxs < 0) maxs = 0;
    int step = s_page_lines > 1 ? s_page_lines - 1 : 1;   // page keeps one line of overlap for reading continuity
    int old = s_scroll;
    if      (dir == -1) s_scroll -= 1;
    else if (dir == +1) s_scroll += 1;
    else if (dir == -2) s_scroll -= step;
    else if (dir == +2) s_scroll += step;
    if (s_scroll < 0)    s_scroll = 0;
    if (s_scroll > maxs) s_scroll = maxs;
    return s_scroll != old;
}

static void meter(int y, int level)
{
    d.drawRoundRect(10, y, 220, 8, 4, LINE);
    if (level > 100) level = 100;
    if (level < 0) level = 0;
    unsigned short col = level > 80 ? ACC : level > 40 ? AMBER : GRN;
    d.fillRoundRect(10, y, 220 * level / 100, 8, 4, col);
    if (s_peak > 2) { int px = 10 + 220 * s_peak / 100; if (px > 228) px = 228; d.drawFastVLine(px, y - 1, 10, FG); }
}

static void waveform(int x, int y, int w, int h)
{
    d.drawFastHLine(x, y + h / 2, w, LINE);
    int cols = WAVE_N, step = w / cols; if (step < 1) step = 1;
    int cy = y + h / 2;
    for (int i = 0; i < cols; i++) {
        int lv = s_wave[(s_wave_head + i) % WAVE_N];
        int bh = lv * (h - 2) / 100; if (bh < 1 && lv > 0) bh = 1;
        unsigned short col = lv > 80 ? ACC : lv > 40 ? AMBER : (lv > 0 ? GRN : WAVE);
        int bx = x + i * step;
        if (bh <= 0) { d.drawPixel(bx, cy, WAVE); continue; }
        d.fillRect(bx, cy - bh / 2, step > 1 ? step - 1 : 1, bh, col);
    }
}

// One-line transport footer for the currently-playing take.
static void draw_transport(int footer_y)
{
    uint32_t el = nucleo_audio_elapsed_ms() / 1000, dur = nucleo_audio_duration_ms() / 1000;
    int prog = nucleo_audio_progress(); if (prog < 0) prog = 0; if (prog > 100) prog = 100;
    d.setTextColor(GRN, BG); d.setCursor(6, footer_y + 2); d.print(nucleo_audio_is_paused() ? "||" : ">");
    char tl[14]; snprintf(tl, sizeof(tl), " %u:%02u", (unsigned)(el / 60), (unsigned)(el % 60)); d.print(tl);
    int bx = 64, bw = 110, by = footer_y + 5;
    d.drawRoundRect(bx, by, bw, 4, 2, LINE); d.fillRoundRect(bx, by, bw * prog / 100, 4, 2, GRN);
    char tr[14]; snprintf(tr, sizeof(tr), "%u:%02u", (unsigned)(dur / 60), (unsigned)(dur % 60));
    d.setTextColor(MUTED, BG); d.setCursor(bx + bw + 6, footer_y + 2); d.print(tr);
}

// ---- AI reader (tabbed detail) --------------------------------------------------------------------
static void detail_load(void)
{
    if (!s_text) return;
    s_text[0] = 0; s_empty = true; s_scroll = 0; s_more = false; s_total_lines = 0; s_page_lines = 1;
    if (s_sel >= s_count) return;
    const char *suffix = s_tab == T_SUMMARY ? ".sum.txt" : s_tab == T_SCRIPT ? ".txt" :
                         s_tab == T_ACTIONS ? ".act.txt" : ".ask.txt";
    char p[300]; sidecar_of(p, sizeof p, s_list[s_sel].name, suffix);
    s_empty = (load_text(p, s_text, REC_TXTBUF) <= 0);
}

static void open_detail(void)
{
    if (s_sel >= s_count) return;
    if (!s_text) s_text = (char *)malloc(REC_TXTBUF);
    if (!s_text) return;                                         // OOM -> stay on the list
    if (s_job == 3) s_job = 0;                                   // clear stale error — new note, new attempt
    s_tab = T_SUMMARY;
    detail_load();
    set_mode(M_DETAIL);
}
static void close_detail(void) { if (s_text) { free(s_text); s_text = nullptr; } }

static void detail_placeholder(int x, int y, int tab)
{
    const char *t1, *t2;
    switch (tab) {
    case T_SUMMARY: t1 = TR("Nessun riassunto",  "No summary yet");
                    t2 = TR("Invio: genera",      "Enter to generate"); break;
    case T_SCRIPT:  t1 = TR("Nessuna trascrizione","No transcript yet");
                    t2 = TR("Invio: trascrivi",   "Enter to transcribe"); break;
    case T_ACTIONS: t1 = TR("Nessuna azione",     "No action items");
                    t2 = TR("Invio: estrai azioni","Enter to extract"); break;
    default:        t1 = TR("Fai una domanda",    "Ask about this note");
                    t2 = TR("Invio: scrivi domanda","Enter to type a question"); break;
    }
    d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
    d.setTextColor(MUTED, BG); d.setCursor(x, y);      d.print(t1);
    d.setTextColor(GRN, BG);   d.setCursor(x, y + 22); d.print(t2);
    d.setFont(&fonts::Font0);
}

static void detail_draw(int top, int h)
{
    draw_tabbar_fn(top, det_tab_label, T_COUNT, s_tab);
    int y0 = top + TABBAR_H, footer_y = top + h - 12;
    d.fillRect(0, y0, 240, footer_y - y0, BG);

    bool here = (s_sel < s_count);
    bool busy = (s_job == 1 && here && !strcmp(s_jobname, s_list[s_sel].name) && s_job_kind == kind_for_tab(s_tab));
    s_more = false;
    if (busy) {
        d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
        char b[40]; snprintf(b, sizeof b, "%s %s...",
            s_eng_ready ? s_eng_short : "AI", TR("sta elaborando", "is working"));
        d.setTextColor(AMBER, BG); d.setCursor(12, y0 + 30); d.print(b);
        d.setTextColor(MUTED, BG); d.setCursor(12, y0 + 50);
        d.print(TR("trascrizione + ragionamento cloud", "transcribe + reason in the cloud"));
        d.setFont(&fonts::Font0);
    } else if (s_empty || !s_text || !s_text[0]) {
        detail_placeholder(10, y0 + 22, s_tab);
    } else {
        const int LH = 20, FONT_H = 22, firstbase = y0 + 18;   // row PITCH is LH, but FreeSans9pt7b draws ~FONT_H tall
        // Reserve a FULL glyph height below the last row so it clears the footer bar: the old "footer_y - 4"
        // let the bottom line bleed UNDER the footer (drawn after the text), so the last line was cut off.
        int maxlines = (footer_y - FONT_H - firstbase) / LH + 1; if (maxlines < 1) maxlines = 1;
        int total_lines = 0;
        s_more = draw_wrapped(s_text, 8, firstbase, 216, LH, maxlines, s_scroll, &total_lines);
        s_total_lines = total_lines; s_page_lines = maxlines;   // feed the deterministic scroll clamp (reader_scroll)
        if (total_lines > maxlines) {
            int sb_x = 231, sb_y = y0 + 6, sb_h = footer_y - y0 - 12;
            d.drawFastVLine(sb_x + 2, sb_y, sb_h, LINE);
            int thumb_h = (sb_h * maxlines) / total_lines; if (thumb_h < 12) thumb_h = 12;
            int max_s = total_lines - maxlines; if (max_s < 1) max_s = 1;
            int thumb_y = sb_y + (s_scroll * (sb_h - thumb_h)) / max_s;
            d.fillRoundRect(sb_x, thumb_y, 5, thumb_h, 2, ACC);
            if (s_scroll > 0) d.fillTriangle(229, sb_y - 1, 237, sb_y - 1, 233, sb_y - 6, ACC);                       // ▲ more above
            if (s_more)       d.fillTriangle(229, sb_y + sb_h + 1, 237, sb_y + sb_h + 1, 233, sb_y + sb_h + 6, ACC);  // ▼ more below
        }
    }

    d.fillRect(0, footer_y, 240, 12, BG); d.drawFastHLine(0, footer_y, 240, LINE);
    d.setTextSize(1);
    if (s_job == 3) {
        d.setTextColor(ACC, BG); d.setCursor(8, footer_y + 2);
        d.print(TR("AI non disponibile (offline/chiave)", "AI failed (offline / no key)"));
        return;
    }
    if (nucleo_audio_is_playing() && s_playing >= 0) { draw_transport(footer_y); return; }
    const char *hint = s_tab == T_ASK
        ? TR("Invio: domanda   </> tab   spc audio", "Enter ask   </> tab   spc play")
        : (s_empty
            ? TR("Invio: genera   </> tab   spc audio",  "Enter make   </> tab   spc play")
            : TR("su/giu riga  ,/. pagina  </> tab  spc", "up/dn line  ,/. page  </> tab  spc"));
    d.setTextColor(MUTED, BG); d.setCursor(8, footer_y + 2); d.print(hint);
}

static void detail_action(void)        // Enter inside the reader
{
    if (s_job == 1) return;                                      // worker running, don't pile up
    if (s_job == 3) s_job = 0;                                   // retry after error: clear flag first
    if (s_tab == T_ASK) {
        char q[160] = "";
        nucleo_ui_input(TR("Chiedi a ", "Ask "), q, sizeof q, 0);
        if (q[0]) start_job(JOB_QA, q);
        nucleo_app_request_draw(); return;
    }
    if (!s_empty) return;                                        // already have content -> scroll, don't regen
    start_job(kind_for_tab(s_tab), nullptr);
    nucleo_app_request_draw();
}

static void detail_key(int key, char ch)
{
    if (key == NK_RIGHT)            { s_tab = (s_tab + 1) % T_COUNT; detail_load(); set_hint_for_mode(); nucleo_app_request_draw(); return; }
    if (ch >= '1' && ch <= '4')     { s_tab = ch - '1'; detail_load(); set_hint_for_mode(); nucleo_app_request_draw(); return; }
    if (key == NK_UP)               { if (reader_scroll(-1)) nucleo_app_request_draw(); return; }
    if (key == NK_DOWN)             { if (reader_scroll(+1)) nucleo_app_request_draw(); return; }
    if (key == NK_ENTER)            { detail_action(); return; }
    bool playing = nucleo_audio_is_playing();
    if (ch == ' ')                  { if (playing) nucleo_audio_toggle_pause(); else play_sel(); nucleo_app_request_draw(); return; }
    if (playing && (ch == ',' || ch == '.' || ch == '<' || ch == '>')) { seek_playing(ch == '.' || ch == '>'); return; }
    // Not playing: ,/. (or </>) PAGE through a long note fast — line-by-line was painful on this tiny screen.
    if (ch == '.' || ch == '>')     { if (reader_scroll(+2)) nucleo_app_request_draw(); return; }
    if (ch == ',' || ch == '<')     { if (reader_scroll(-2)) nucleo_app_request_draw(); return; }
    if (ch == 's' || ch == 'S')     { nucleo_audio_stop(); s_playing = -1; nucleo_app_request_draw(); return; }
}

// ---- cross-note answer reader (M_ASK) -------------------------------------------------------------
static void ask_draw(int top, int h)
{
    int y0 = app_ui_title(TR("Chiedi a tutte le note", "Ask all notes"), VIO, s_eng_ready ? s_eng_short : nullptr);
    int footer_y = top + h - 12;
    d.fillRect(0, y0, 240, footer_y - y0, BG);
    if (!s_text || !s_text[0]) {
        d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1); d.setTextColor(MUTED, BG);
        d.setCursor(10, y0 + 24); d.print(TR("Nessuna risposta.", "No answer.")); d.setFont(&fonts::Font0);
    } else {
        const int LH = 20, FONT_H = 22, firstbase = y0 + 18;   // row PITCH is LH, but FreeSans9pt7b draws ~FONT_H tall
        // Reserve a FULL glyph height below the last row so it clears the footer bar: the old "footer_y - 4"
        // let the bottom line bleed UNDER the footer (drawn after the text), so the last line was cut off.
        int maxlines = (footer_y - FONT_H - firstbase) / LH + 1; if (maxlines < 1) maxlines = 1;
        int total_lines = 0;
        s_more = draw_wrapped(s_text, 8, firstbase, 216, LH, maxlines, s_scroll, &total_lines);
        s_total_lines = total_lines; s_page_lines = maxlines;   // feed the deterministic scroll clamp (reader_scroll)
        if (total_lines > maxlines) {
            int sb_x = 231, sb_y = y0 + 6, sb_h = footer_y - y0 - 12;
            d.drawFastVLine(sb_x + 2, sb_y, sb_h, LINE);
            int thumb_h = (sb_h * maxlines) / total_lines; if (thumb_h < 12) thumb_h = 12;
            int max_s = total_lines - maxlines; if (max_s < 1) max_s = 1;
            int thumb_y = sb_y + (s_scroll * (sb_h - thumb_h)) / max_s;
            d.fillRoundRect(sb_x, thumb_y, 5, thumb_h, 2, ACC);
            if (s_scroll > 0) d.fillTriangle(229, sb_y - 1, 237, sb_y - 1, 233, sb_y - 6, ACC);                       // ▲ more above
            if (s_more)       d.fillTriangle(229, sb_y + sb_h + 1, 237, sb_y + sb_h + 1, 233, sb_y + sb_h + 6, ACC);  // ▼ more below
        }
    }
    d.fillRect(0, footer_y, 240, 12, BG); d.drawFastHLine(0, footer_y, 240, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, footer_y + 2);
    d.print(TR("su/giu riga  ,/. pagina  esc: indietro", "up/dn line  ,/. page  esc back"));
}

// ---- settings -------------------------------------------------------------------------------------
static void draw_opt(int y, bool focus, const char *label, const char *val, bool toggle, bool on)
{
    const int hh = 24;
    d.fillRoundRect(6, y, 228, hh, 7, focus ? CAP : SURF);
    if (focus) d.fillRoundRect(6, y + 4, 4, hh - 8, 2, ACC);
    d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
    d.setTextColor(focus ? FG : MUTED, focus ? CAP : SURF); d.setCursor(16, y + 7); d.print(label);
    d.setFont(&fonts::Font0);
    if (toggle) {
        int sw = 36, sh = 16, bx = 228 - sw - 6, vy = y + (hh - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, on ? GRN : 0x4208);
        d.fillCircle(on ? bx + sw - sh / 2 : bx + sh / 2, vy + sh / 2, sh / 2 - 2, on ? INK : MUTED);
    } else if (val) {
        int vw = (int)strlen(val) * 6 + 10, bx = 228 - vw - 6, vy = y + (hh - 14) / 2;
        d.fillRoundRect(bx, vy, vw, 14, 4, focus ? ACC : LINE);
        d.setTextColor(focus ? INK : FG, focus ? ACC : LINE); d.setCursor(bx + 5, vy + 4); d.print(val);
    }
}

static void info_row(int y, const char *label, const char *val, unsigned short vc)
{
    d.setTextColor(MUTED, BG); d.setCursor(12, y); d.print(label);
    d.setTextColor(vc, BG);    d.setCursor(150, y); d.print(val);
}

static void settings_draw(int top, int h)
{
    draw_tabbar(top, SET_TABS, ST_COUNT, s_set_tab);
    int y0 = top + TABBAR_H, footer_y = top + h - 12;
    d.fillRect(0, y0, 240, footer_y - y0, BG);

    if (s_set_tab == ST_AI) {
        const char *langv = s_lang == 1 ? "IT" : s_lang == 2 ? "EN" : "Auto";
        int y = y0 + 6;
        draw_opt(y,      s_set_row == 0, TR("Riassunto auto", "Auto summary"), nullptr, true, s_auto_summary);
        draw_opt(y + 28, s_set_row == 1, TR("Titolo auto",    "Auto title"),   nullptr, true, s_auto_title);
        draw_opt(y + 56, s_set_row == 2, TR("Lingua",         "Language"),     langv,   false, false);
    } else {
        d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
        bool on = nucleo_anima_online_available();
        char fr[24]; human_free(fr, sizeof fr);
        const char *m = s_eng_model;
        if (!strncmp(m, "claude-", 7)) m += 7;                  // shorten "claude-sonnet-4-6" -> "sonnet-4-6"
        char mdl[16]; snprintf(mdl, sizeof mdl, "%.14s", m[0] ? m : "-");
        int y = y0 + 8;
        info_row(y,      "Engine",  s_eng_ready ? s_eng_short : "not set", s_eng_ready ? GRN : ACC);
        info_row(y + 18, "Model",   mdl,                                   MUTED);
        info_row(y + 36, "Online",  on ? "yes" : "no",                     on ? GRN : ACC);
        info_row(y + 54, "Storage", fr[0] ? fr : "n/d",                    FG);
        d.setFont(&fonts::Font0);
    }

    d.fillRect(0, footer_y, 240, 12, BG); d.drawFastHLine(0, footer_y, 240, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, footer_y + 2);
    d.print(s_set_tab == ST_AI
        ? TR("su/giu riga  Invio: cambia  </> tab", "up/dn row   ent change   </> tab")
        : TR("</> tab   Esc: indietro", "</> tab   esc back"));
}

static void settings_key(int key, char ch)
{
    (void)ch;
    if (key == NK_RIGHT) { s_set_tab = (s_set_tab + 1) % ST_COUNT; s_set_row = 0; set_hint_for_mode(); nucleo_app_request_draw(); return; }
    if (s_set_tab != ST_AI) return;                            // INFO has no rows
    if (key == NK_UP)   { if (s_set_row > 0) s_set_row--; nucleo_app_request_draw(); return; }
    if (key == NK_DOWN) { if (s_set_row < 2) s_set_row++; nucleo_app_request_draw(); return; }
    if (key == NK_ENTER) {
        if      (s_set_row == 0) s_auto_summary = !s_auto_summary;
        else if (s_set_row == 1) s_auto_title   = !s_auto_title;
        else                     s_lang = (s_lang + 1) % 3;
        save_settings(); nucleo_app_request_draw();
    }
}

// ---- library + recording overlay ------------------------------------------------------------------
static const char *rl_label(int i, void *) { return s_list[i].label; }
static const char *rl_right(int i, void *)
{
    static char b[12]; uint32_t s = s_list[i].secs;
    snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); return b;
}
static unsigned short rl_color(int i, void *) { return i == s_playing ? GRN : (s_list[i].has_ai ? VIO : ACC); }

static void draw_recording(void)
{
    bool arming = (s_ptt == PTT_ARMING && !nucleo_recorder_is_recording());   // GO held, start cue playing
    int y0 = app_ui_title(arming ? TR("Pronto…", "Ready…") : TR("Registrazione", "Recording"), ACC, nullptr);
    if (arming) {                                            // the mic opens the instant the cue finishes
        d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1); d.setTextColor(GRN, BG);
        d.setCursor(12, y0 + 34); d.print(TR("Tieni GO e parla…", "Hold GO and speak…"));
        d.setFont(&fonts::Font0);
        return;
    }
    uint32_t s = nucleo_recorder_seconds();
    char t[24]; snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    bool blink = (esp_timer_get_time() / 500000) & 1;
    d.fillCircle(18, y0 + 12, 5, blink ? ACC : 0x6000);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(34, y0 + 5); d.print("REC "); d.print(t);
    waveform(10, y0 + 26, 220, 40);
    meter(y0 + 70, nucleo_recorder_level());
    char fr[24]; human_free(fr, sizeof(fr));
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, y0 + 84);
    d.print(s_ptt == PTT_RECORDING ? TR("Rilascia GO per fermare", "Release GO to stop")
                                   : TR("R/Spazio: ferma", "R/Space: stop"));
    if (fr[0]) { d.print("   "); d.print(fr); }
}

static void draw_library(int top, int h)
{
    char sub[32];
    if (s_count) snprintf(sub, sizeof(sub), "%d %s  %u:%02u",
                          s_count, TR(s_count == 1 ? "nota" : "note", s_count == 1 ? "take" : "takes"),
                          (unsigned)(s_total_secs / 60), (unsigned)(s_total_secs % 60));
    else         snprintf(sub, sizeof(sub), TR("0 note", "0 takes"));
    int y0 = app_ui_title(TR("Registratore", "Voice Recorder"), ACC, sub);

    if (s_count == 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(12, y0 + 18);
        d.print(TR("Nessuna registrazione", "No recordings yet"));
        d.setTextColor(GRN, BG); d.setCursor(12, y0 + 34);
        d.print(TR("Premi R per registrare", "Press R to record"));
        char fr[24]; human_free(fr, sizeof(fr));
        if (fr[0]) { d.setTextColor(MUTED, BG); d.setCursor(12, y0 + 52); d.print(fr); }
        return;
    }

    int footer_y = top + h - 12;
    app_ui_list(y0, footer_y - y0 - 2, s_count, s_sel, rl_label, rl_right, rl_color, nullptr);
    d.fillRect(0, footer_y, 240, 12, BG); d.drawFastHLine(0, footer_y, 240, LINE);
    d.setTextSize(1);

    if (s_job == 1) {
        char b[32]; snprintf(b, sizeof b, "%s %s...", s_eng_ready ? s_eng_short : "AI",
                             TR("sta lavorando", "working"));
        d.setTextColor(AMBER, BG); d.setCursor(8, footer_y + 2); d.print(b); return;
    }
    if (s_job == 3) {
        d.setTextColor(ACC, BG); d.setCursor(8, footer_y + 2);
        d.print(TR("AI non disponibile", "AI failed (offline)")); return;
    }
    if (nucleo_audio_is_playing() && s_playing >= 0) { draw_transport(footer_y); return; }

    // Footer: info nota selezionata a sinistra, hint azione AI chiara a destra.
    const Rec *r = &s_list[s_sel];
    char det[32];
    if (r->mtime > 0) {
        struct tm tm; localtime_r(&r->mtime, &tm);
        strftime(det, sizeof(det), "%d/%m %H:%M", &tm);        // compact: 13/06 14:30
    } else snprintf(det, sizeof(det), "%s", r->name);
    d.setTextColor(r->has_ai ? VIO : MUTED, BG); d.setCursor(8, footer_y + 2); d.print(det);

    // Hint AI a destra: distingue "apri AI reader" da "genera AI" in modo leggibile.
    const char *hint = r->has_ai ? TR("A: leggi AI", "A: AI reader") : TR("A: crea AI", "A: gen AI");
    d.setTextColor(r->has_ai ? VIO : ACC, BG);
    d.setCursor(240 - 6 * (int)strlen(hint) - 4, footer_y + 2); d.print(hint);
}

// ---- mode plumbing --------------------------------------------------------------------------------
static void set_hint_for_mode(void)
{
    if (nucleo_recorder_is_recording()) {
        nucleo_app_set_hint(s_ptt == PTT_RECORDING ? TR("Rilascia GO per fermare", "Release GO to stop")
                                                   : TR("R/Spazio: ferma", "R/Space: stop")); return;
    }
    switch (s_mode) {
    case M_DETAIL:
        nucleo_app_set_hint(s_tab == T_ASK
            ? TR("</> tab  Invio: chiedi  Spc: audio  Esc: back",
                 "</> tab  Enter ask  Spc play  Esc back")
            : TR("</> tab  su/giu riga  ,/. pagina  Invio: genera",
                 "</> tab  up/dn line  ,/. page  Enter make"));
        break;
    case M_SETTINGS:
        nucleo_app_set_hint(TR("</> tab  su/giu  Invio: cambia  Esc: back",
                              "</> tab  up/dn  Enter change  Esc back"));
        break;
    case M_ASK:
        nucleo_app_set_hint(TR("su/giu riga  ,/. pagina  Esc: back", "up/dn line  ,/. page  Esc back"));
        break;
    default:
        nucleo_app_set_hint(TR("Invio play  A: AI  K: chiedi a tutte le note  1-9: vai  R: reg",
                              "Enter play  A: AI  K: ask all notes  1-9: jump  R: rec"));
    }
}
static void set_mode(int m)
{
    if (m != M_DETAIL && m != M_ASK) close_detail();           // hand the reader buffer back to the heap
    s_mode = m;
    set_hint_for_mode();
    nucleo_app_request_draw();
}

// ---- input handlers -------------------------------------------------------------------------------
static void list_key(int key, char ch)
{
    if (nucleo_recorder_is_recording()) {                      // recording: only stop
        if (ch == 'r' || ch == 'R' || ch == ' ') {
            nucleo_recorder_stop();
            if (s_ptt == PTT_RECORDING) s_ptt = PTT_ENDCUE;    // keyboard-stopped a GO-held take → state + end cue follow
            nucleo_app_request_draw();
        }
        return;
    }
    bool playing = nucleo_audio_is_playing();
    if (playing && (ch == ',' || ch == '.' || ch == '<' || ch == '>')) { seek_playing(ch == '.' || ch == '>'); return; }

    // Smartwatch quick-jump: a digit selects the Nth take (1 = newest). Intercept BEFORE app_ui_list_key,
    // whose type-ahead would otherwise swallow the digit. Keeps a long library one keypress away.
    if (ch >= '1' && ch <= '9') {
        int n = ch - '1';
        if (n < s_count) { s_sel = n; if (s_job == 3) s_job = 0; nucleo_app_request_draw(); }
        return;
    }

    if (app_ui_list_key(key, ch, &s_sel, s_count, rl_label, nullptr)) {
        if (s_job == 3) s_job = 0;                               // changing note clears stale error
        nucleo_app_request_draw(); return;
    }
    else if (key == NK_ENTER)         play_sel();
    else if (ch == 'a' || ch == 'A')  { open_detail(); return; }
    else if (ch == 'd' || ch == 'D')  delete_sel();
    else if (ch == 'n' || ch == 'N')  rename_sel();
    else if (ch == 't' || ch == 'T')  { if (s_count) start_job(JOB_TITLE, nullptr); }   // AI title + rename
    else if (ch == 'k' || ch == 'K')  { ask_all(); return; }                            // cross-note Q&A
    else if (ch == 's' || ch == 'S')  { nucleo_audio_stop(); s_playing = -1; }
    else if (ch == ' ')               { if (playing) nucleo_audio_toggle_pause(); else { nucleo_audio_stop(); s_playing = -1; play_sel(); } }
    else if (ch == 'r' || ch == 'R')  { nucleo_audio_stop(); s_playing = -1; wave_reset();
        // The mic is single-owner: if a web dictation/recording stream holds it, start() refuses —
        // say so instead of silently arming a "stop" hint for a recording that never began.
        if (nucleo_recorder_start() == ESP_OK)
            nucleo_app_set_hint(TR("R/Spazio: ferma", "R/Space: stop"));
        else
            nucleo_app_set_hint(TR("Mic occupato (uso web)", "Mic busy (web stream)"));
    }
    else return;
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_mode == M_SETTINGS) { settings_key(key, ch); return; }
    if (s_mode == M_DETAIL)   { detail_key(key, ch);   return; }
    if (s_mode == M_ASK) {                                     // cross-note answer reader: scroll only
        if      (key == NK_UP)   { if (reader_scroll(-1)) nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { if (reader_scroll(+1)) nucleo_app_request_draw(); }
        else if (ch == '.' || ch == '>') { if (reader_scroll(+2)) nucleo_app_request_draw(); }   // page down
        else if (ch == ',' || ch == '<') { if (reader_scroll(-2)) nucleo_app_request_draw(); }   // page up
        return;
    }
    list_key(key, ch);
}

// TAB: list -> settings, settings/detail/ask -> list. Never mid-record.
static void rec_tab(void)
{
    if (nucleo_recorder_is_recording()) return;
    if (s_mode == M_DETAIL || s_mode == M_SETTINGS || s_mode == M_ASK) set_mode(M_LIST);
    else { s_set_tab = ST_AI; s_set_row = 0; set_mode(M_SETTINGS); }
}

// Back/Left routing (framework hands us the key code): Left = previous tab, Esc/Back = pop a level.
static bool rec_back(int key)
{
    if (s_mode == M_ASK) { set_mode(M_LIST); return true; }
    if (s_mode == M_DETAIL) {
        if (key == NK_LEFT) { s_tab = (s_tab + T_COUNT - 1) % T_COUNT; detail_load(); set_hint_for_mode(); nucleo_app_request_draw(); return true; }
        set_mode(M_LIST); return true;
    }
    if (s_mode == M_SETTINGS) {
        if (key == NK_LEFT) { s_set_tab = (s_set_tab + ST_COUNT - 1) % ST_COUNT; s_set_row = 0; set_hint_for_mode(); nucleo_app_request_draw(); return true; }
        set_mode(M_LIST); return true;
    }
    return false;                                              // library: let the framework close the app
}

// ── GO-button push-to-talk: hold GO to record ─────────────────────────────────────────────────────
// Two short polyphonic cues bracket the take — a bright RISING triad on start ("go"), a settling FALLING
// triad on stop ("done") — synthesized ONCE to tiny cached WAVs (~8 KB), streamed with a zero PCM buffer
// (notify_synth_voices_wav). Speaker and PDM mic share the I2S WS line, so playback and recording are
// MUTUALLY EXCLUSIVE: cue and mic never overlap (sequenced in on_tick), so the mic can't capture a cue and
// a cue can't fail on a busy I2S. RAM held: one int of state + a few voice floats on the stack.
#define CUE_START_WAV  REC_PATH "/.cue_start.wav"
#define CUE_STOP_WAV   REC_PATH "/.cue_stop.wav"
static void rec_cue_ensure(void)
{
    static bool ready = false;
    if (ready) return;
    struct stat st;
    if (stat(CUE_START_WAV, &st) != 0) {                         // DO5 -> MI5 -> SOL5, staggered: rising "go"
        notify_voice_t up[3];
        notify__voice(&up[0], 523.25f, 0.00f, 0.16f);
        notify__voice(&up[1], 659.25f, 0.06f, 0.16f);
        notify__voice(&up[2], 783.99f, 0.12f, 0.20f);
        notify_synth_voices_wav(up, 3, CUE_START_WAV, NOTIFY_SND_RATE);
    }
    if (stat(CUE_STOP_WAV, &st) != 0) {                          // SOL5 -> MI5 -> DO5, staggered: settling "done"
        notify_voice_t dn[3];
        notify__voice(&dn[0], 783.99f, 0.00f, 0.16f);
        notify__voice(&dn[1], 659.25f, 0.06f, 0.16f);
        notify__voice(&dn[2], 523.25f, 0.12f, 0.22f);
        notify_synth_voices_wav(dn, 3, CUE_STOP_WAV, NOTIFY_SND_RATE);
    }
    ready = true;
}

// Framework GO-hold hook (nucleo_app_set_ptt_handler): engage(true)/release(false). The mic/speaker
// sequencing lives in on_tick — here we only flip state and fire the start cue / close the mic.
static void rec_ptt(bool on)
{
    if (on) {
        if (s_ptt != PTT_IDLE || nucleo_recorder_is_recording()) return;       // already busy (PTT or manual R)
        if (nucleo_audio_is_playing()) { nucleo_audio_stop(); s_playing = -1; } // free the speaker first
        rec_cue_ensure();
        s_ptt = PTT_ARMING;                          // start cue plays now; on_tick opens the mic when it ends
        nucleo_audio_play(CUE_START_WAV);            // if I2S is busy this no-ops -> on_tick just records at once
    } else {
        if (s_ptt == PTT_ARMING)         { nucleo_audio_stop(); s_ptt = PTT_IDLE; }        // released during the cue
        else if (s_ptt == PTT_RECORDING) { nucleo_recorder_stop(); s_ptt = PTT_ENDCUE; }  // end cue plays in on_tick
        else return;                                                                       // nothing in flight (handler latched but app already closed) — leave the new foreground app's UI alone
        set_hint_for_mode();
    }
    nucleo_app_request_draw();
}

// ---- lifecycle ------------------------------------------------------------------------------------
static void enter(void)
{
    load_settings();
    // Cache UI language once per open (sys_lang() hits SD — avoid calling it in draw loops).
    s_en = !strncasecmp(sys_lang(), "en", 2);
    nucleo_app_set_tab_handler(rec_tab);
    nucleo_app_set_back_handler(rec_back);
    nucleo_app_set_ptt_handler(rec_ptt);            // GO-hold = push-to-talk record while this app is open
    s_ptt = PTT_IDLE;
    rec_cue_ensure();                               // synth the start/stop cues once (cached on SD; no-op after)
    if (!s_list) s_list = (Rec *)malloc(sizeof(Rec) * REC_MAX);
    s_was_rec = nucleo_recorder_is_recording();
    s_sel = 0; s_playing = -1; s_mode = M_LIST; s_tab = T_SUMMARY; s_set_tab = ST_AI; s_set_row = 0;
    if (s_job != 1) s_job = 0;                                 // keep a mid-flight worker's state
    wave_reset();
    scan();
    // Cache the active cloud engine for honest UI ("Claude"/"Grok" + model). Cheap; re-read each open.
    char prov[16] = "";
    s_eng_ready = nucleo_anima_teacher_info(prov, sizeof prov, s_eng_model, sizeof s_eng_model);
    snprintf(s_eng_short, sizeof s_eng_short, "%s", !s_eng_ready ? "AI" : (!strcmp(prov, "anthropic") ? "Claude" : "Grok"));
    set_hint_for_mode();
}

// Playback stops on leave; the recorder + AI worker keep running (they don't touch s_list/s_text).
static void exit_app(void)
{
    nucleo_app_set_ptt_handler(nullptr);   // release the GO-hold hook (framework also clears it on switch)
    if (s_ptt == PTT_RECORDING && nucleo_recorder_is_recording()) nucleo_recorder_stop();  // save, don't orphan, a GO-held take
    s_ptt = PTT_IDLE;
    nucleo_audio_stop();
    if (s_list) { free(s_list); s_list = nullptr; s_count = 0; }
    close_detail();
    s_ai_screen_freed = false;   // the launcher re-acquires the canvas on the next app's enter; keep our flag honest
}

static void tick(void)
{
    bool need = false;
    bool rec = nucleo_recorder_is_recording();
    if (s_was_rec && !rec) {                                   // recording just finished
        scan(); s_sel = 0;
        if (s_auto_summary && s_job != 1) start_job(JOB_SUMMARY, nullptr);
        set_hint_for_mode();
        need = true;
    }
    s_was_rec = rec;

    // GO push-to-talk sequencing (speaker⊕mic share the I2S WS line, so the steps NEVER overlap):
    if (s_ptt == PTT_ARMING && !nucleo_audio_is_playing()) {   // start cue finished -> open the mic
        if (nucleo_recorder_start() == ESP_OK) { s_ptt = PTT_RECORDING; wave_reset(); s_was_rec = true; set_hint_for_mode(); }
        else                                   { s_ptt = PTT_IDLE; }   // mic busy (web dictation) -> abort cleanly
        need = true;
    }
    if (s_ptt == PTT_ENDCUE && !nucleo_recorder_is_busy() && !nucleo_audio_is_playing()) {
        nucleo_audio_play(CUE_STOP_WAV); s_ptt = PTT_IDLE; need = true;   // "done" cue, ONLY once the mic RX is FULLY released (is_busy, not is_recording — the channel drains ~200ms after stop on the shared I2S WS line)
    }

    if (rec) {                                                // sample mic level for the live waveform
        int64_t now = esp_timer_get_time();
        if (now - s_wave_last_us >= 50000) {
            s_wave_last_us = now;
            int lv = nucleo_recorder_level(); if (lv < 0) lv = 0; if (lv > 100) lv = 100;
            s_wave[s_wave_head] = (uint8_t)lv; s_wave_head = (s_wave_head + 1) % WAVE_N;
            if (lv > s_peak) s_peak = lv; else if (s_peak > 0) s_peak -= 2;
            need = true;
        }
    }

    if (s_ai_screen_freed && s_job != 1) { ai_screen_restore(); need = true; }   // job ended (done/error) → reclaim the canvas for the UI

    if (s_job == 2) {                                          // a worker finished
        int kind = s_job_kind; s_job = 0; scan();
        if (s_renamed[0]) {
            for (int i = 0; i < s_count; i++) { if (!strcmp(s_list[i].name, s_renamed)) { s_sel = i; break; } }
            s_renamed[0] = 0;
        }
        if (kind == JOB_ASKALL) {                             // open the cross-note answer reader
            if (!s_text) s_text = (char *)malloc(REC_TXTBUF);
            if (s_text) {
                char p[300]; snprintf(p, sizeof p, "%s/.askall.txt", REC_PATH);
                load_text(p, s_text, REC_TXTBUF);
                s_scroll = 0; s_more = false; s_total_lines = 0; s_page_lines = 1; s_mode = M_ASK; set_hint_for_mode();
            }
        } else if (s_mode == M_DETAIL) detail_load();
        need = true;
    }

    bool playing = nucleo_audio_is_playing();
    if (!playing && s_playing != -1) { s_playing = -1; need = true; }
    if (playing && (s_mode == M_LIST || s_mode == M_DETAIL)) {  // keep the progress bar live (~1 Hz)
        static uint32_t last_el = 0; uint32_t el = nucleo_audio_elapsed_ms() / 1000;
        if (el != last_el) { last_el = el; need = true; }
    }
    if (need) nucleo_app_request_draw();
}

static const char *job_kind_label(int k)
{
    switch (k) {
        case JOB_SUMMARY:    return TR("Riassunto", "Summary");
        case JOB_TRANSCRIBE: return TR("Trascrizione", "Transcription");
        case JOB_ACTIONS:    return TR("Azioni", "Action items");
        case JOB_QA:         return TR("Risposta", "Answer");
        case JOB_TITLE:      return TR("Titolo", "Title");
        case JOB_ASKALL:     return TR("Risposta (tutte)", "Answer (all notes)");
        default:             return "AI";
    }
}
// Direct-draw "working" screen while the canvas is freed for the cloud TLS (drawn direct: `d` is the panel).
static void draw_ai_working(int top, int h)
{
    int cy = top + h / 2;
    const char *lbl = job_kind_label(s_job_kind);
    d.setFont(&fonts::Font2); d.setTextColor(FG, BG);
    d.setCursor(120 - (int)d.textWidth(lbl) / 2, cy - 26); d.print(lbl);
    d.setFont(&fonts::Font0);
    const char *w1 = TR("Elaboro online...", "Working online...");
    d.setTextColor(GRN, BG); d.setCursor(120 - (int)strlen(w1) * 3, cy - 2); d.print(w1);
    const char *w2 = TR("RAM liberata per il modello", "RAM freed for the model");
    d.setTextColor(MUTED, BG); d.setCursor(120 - (int)strlen(w2) * 3, cy + 14); d.print(w2);
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);                            // clear once -> no stale pixels across modes
    if (s_ai_screen_freed) { draw_ai_working(top, h); return; }  // AI job in flight: canvas freed → minimal direct screen
    if (nucleo_recorder_is_recording() || s_ptt == PTT_ARMING) { draw_recording(); return; }
    if (s_mode == M_SETTINGS)           { settings_draw(top, h); return; }
    if (s_mode == M_DETAIL)             { detail_draw(top, h);   return; }
    if (s_mode == M_ASK)                { ask_draw(top, h);      return; }
    draw_library(top, h);
}

extern "C" void nucleo_register_recorder(void)
{
    static const nucleo_app_def_t app = {
        "recorder", "Voice Recorder", "Media",
        "Record the mic, then summarize/transcribe with Grok. TAB=settings.",
        'R', 0xF96B, enter, on_key, tick, draw, exit_app
    };
    nucleo_app_register(&app);
}
