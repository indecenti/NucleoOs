// AVCEB — Anima Voice Contextual Event Bus (v2: MFCC + CMN + self-consolidating templates)
//
// Pipeline:
//   FN held → I2S PCM streamed into vdsp_acc (framing+MFCC happen incrementally,
//   raw audio never held) → VAD (RMS) segments audio into "bursts" (one per word) →
//   each burst → CMN'd canonical MFCC → banded DTW match → label →
//   labels accumulated into a sentence → fed to nucleo_anima_query() →
//   anima_result_t dispatched locally (nucleo_app_launch_id) OR remotely via WS.
//
// Learning mode: next PTT saves the burst's MFCC template (.tpl, versioned), no match.
//
// SELF-IMPROVEMENT: every confident match EMA-blends the spoken utterance back into
// its template (vdsp_consolidate) — the recognizer sharpens toward your own voice with
// use, fully offline. Adapted templates are persisted (debounced) on PTT release.
//
// Memory budget (PSRAM-less ESP32-S3, ~76 KB free heap) — lazy, freed-when-idle:
//   • vdsp_ctx (~7 KB) precomputed tables          : resident only while voice is ENABLED.
//   • vdsp_acc (~24 KB) + PCM + 2 match buffers (~7 KB) : on PTT press, freed on release.
//   • templates live on SD, STREAMED one at a time during a match (peak ~7 KB) — no big
//     resident cache, so the recognizer fits no matter how many words are trained.
#include "nucleo_voice.h"
#include "nucleo_voice_dsp.h"
#include "nucleo_kbd.h"
#include "nucleo_eventbus.h"
#include "nucleo_board.h"
#include "nucleo_anima.h"
#include "nucleo_tts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nucleo_ui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

// Aspetta che la voce in corso finisca (no-op se idle) — serializza le due frasi bilingui del traduttore.
extern void nucleo_audio_wait_idle(uint32_t max_ms);

static const char *TAG = "voice";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define VOICE_RATE_HZ        16000
#define VOICE_CHUNK          512           // samples per I2S read (~32 ms)
#define VAD_SILENCE_MS       300           // consecutive silence → burst end
// Adaptive VAD: the speech threshold tracks the ambient noise floor instead of a
// fixed level — captures a quiet speaker in a silent room AND rejects ambient in a
// noisy one. threshold = clamp(noise_floor × MULT, MIN..MAX).
#define VAD_NOISE_INIT       120           // initial noise-floor estimate at PTT start
#define VAD_MULT             3             // speech threshold = noise_floor × MULT
#define VAD_RMS_MIN          250           // threshold never below this
#define VAD_RMS_MAX          4000          // …nor above this
#define VOICE_MAX_TPLS       20            // max loaded templates
#define TPL_PATH             NUCLEO_SD_MOUNT "/system/voice"
#define VOICE_LANG           "it"          // anima reply language
#define TPL_MAGIC            0x33505456u   // 'VTP3' — format version (MFCC + delta features)

// Accept policy (distances are per-frame-averaged L1 MFCC units from vdsp_dtw).
// Two scale-robust gates instead of one hand-tuned threshold:
//   • absolute ceiling — a loose sanity gate that rejects pure garbage;
//   • margin — the winner must clearly beat the runner-up (best*NUM ≤ second*DEN),
//     scale-free so it survives the unknown absolute scale of real voice;
//   • per-template radius — self-calibrates toward your own match distances.
// Accept if (ceiling) AND (margin OR within-radius). See docs/voice.md.
#define VOICE_ABS_CEILING    8000          // hard sanity ceiling
#define VOICE_MARGIN_NUM     4
#define VOICE_MARGIN_DEN     3             // margin accept: best ≤ 0.75 × second-best
#define VOICE_ADAPT_ALPHA    38            // EMA pull (~0.15) of a confirmed utterance into its template

// Forward declarations
extern int  nucleo_ws_client_count(void);
extern bool nucleo_app_launch_id(const char *id);
extern const char *nucleo_app_native_id(const char *anima_id);   // id registro ("media-player") -> nativo ("music")
// Polyphonic "ready, talk now" earcon. Played the instant PTT engages, BEFORE the mic opens —
// the speaker and the PDM mic share the I2S WS line (mutually exclusive), so they must be strictly
// sequential, and the chime is the user's cue to start speaking. Blocks until the (short, SD-cached)
// chime finishes. Defined in nucleo_notify.cpp; resolved at final link (main pulls both components
// in), so nucleo_voice needs no REQUIRES on nucleo_app → no component cycle. Same pattern as above.
extern void nucleo_voice_ready_chime(void);
// "Done listening" earcon, played AFTER the mic closes and recognition runs. The UI loop keeps the
// screen blanked + the 32 KB canvas freed until VH_PTT drops, so playing this BEFORE we release the
// hold means the panel comes back only once the end beep has finished. Same link-time-extern escape
// hatch as the ready chime above (no REQUIRES cycle on nucleo_app).
extern void nucleo_voice_done_chime(void);


// ---------------------------------------------------------------------------
// State — heap pointers, lazily allocated; BSS cost ~ a few hundred bytes.
// ---------------------------------------------------------------------------
// ── Lazy lifecycle (no RAM at boot) ─────────────────────────────────────────
// The 16 KB task exists ONLY while voice is actually wanted. Holders request it;
// when the last holder leaves, the task frees everything and self-deletes, giving
// the stack back. Exclusive mode SUSPENDS (a separate flag) so it never spuriously
// recreates the engine. See voice_recompute().
#define VH_APP    0x1u                          // Voice app foreground
#define VH_LEARN  0x2u                          // transient web-armed learning (auto-expires)
#define VH_PIN    0x4u                          // explicit manual "listen" (STATUS tab)
#define VH_PTT    0x8u                          // transient push-to-talk: GO held in the UI loop
#define VOICE_TASK_PENDING ((TaskHandle_t)1)
#define VOICE_LEARN_TTL_US (30LL * 1000 * 1000) // web-armed learn auto-release

// PTT spin-up heap bars: the recognizer allocates ~38 KB per session (vdsp_acc ~24 KB is one
// contiguous block) — refuse the press when that can't fit beside whatever else is running.
#define VOICE_PTT_MIN_BLOCK (26 * 1024)   // contiguous: the ~24 KB accumulator + margin
#define VOICE_PTT_MIN_FREE  (44 * 1024)   // total: ~38 KB of buffers + working margin
// Heavy-work arbiter probe (one-TLS-at-a-time token): resolved at final link, same escape hatch
// as the chime below — no REQUIRES edge on nucleo_arb for one read-only predicate.
extern bool nucleo_arb_busy(void);
static uint32_t     s_hold               = 0;   // active-holder bitmask
static bool         s_suspended          = false;
static TaskHandle_t s_task               = NULL; // NULL=down, PENDING=creating
static volatile bool s_run               = false;
static portMUX_TYPE s_life_mux           = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_learn_deadline_us  = 0;
static void voice_task(void *arg);
static void voice_recompute(void);
static void voice_hold_set(uint32_t bit, bool on);
static bool        s_learning_mode        = false;
static char        s_learning_word[32]    = {0};

static vdsp_ctx   *s_ctx                  = NULL;   // ~7 KB precomputed tables (cache)
static vdsp_acc   *s_acc                  = NULL;   // ~24 KB streaming accumulator (per-PTT)
static int16_t    *s_pcm                  = NULL;   // capture chunk (per-PTT)
// NO resident template store: with delta features a full 20-word cache would be
// ~67 KB — too big for the device's ~76 KB free heap. Templates live on SD and
// are streamed one at a time during a match (peak ~7 KB), so the recognizer
// certainly fits regardless of how many words are trained.
static vdsp_template *s_scan              = NULL;   // per-template read buffer (~3.3 KB, per-PTT)
static vdsp_template *s_win               = NULL;   // best-match copy, for consolidation (~3.3 KB)
static int32_t     s_win_radius           = 0;
static char        s_win_label[32]        = {0};
static char        s_tokbuf[16][32]       = {{0}};  // stable backing for this PTT's matched tokens

static bool        s_is_listening         = false;
static volatile bool s_ptt_on             = false;  // GO held? set by nucleo_voice_ptt() from the UI loop
static char        s_live_sentence[128]   = {0};
static portMUX_TYPE s_live_mux            = portMUX_INITIALIZER_UNLOCKED;

// ── Poll-based introspection for native apps (the live event sink is owned by the WS
//    layer). Written by the voice task on core 1, read by the UI loop on core 0; the
//    short struct copies are guarded by s_intro_mux. seq fields signal freshness. ──────
static portMUX_TYPE s_intro_mux           = portMUX_INITIALIZER_UNLOCKED;
static volatile nucleo_voice_learn_t s_learn_result = NUCLEO_VOICE_LEARN_NONE;
static nucleo_voice_match_t  s_last_match  = {0};
static nucleo_voice_result_t s_last_result = {0};
static volatile bool s_test_mode          = false;  // recognize but don't execute (native "Voce" console)

bool nucleo_voice_is_listening(void) { return s_is_listening; }
// True for the WHOLE PTT session: VH_PTT is set the instant GO engages and cleared only after the
// engine has captured, recognized/dispatched, AND played the end beep. The UI loop polls this to hold
// the screen dark + the canvas freed across the async tail (recognition runs after button release).
bool nucleo_voice_ptt_engaged(void) { return (s_hold & VH_PTT) != 0; }
void nucleo_voice_get_live_sentence(char *out, size_t max) {
    if (!out || max == 0) return;
    portENTER_CRITICAL(&s_live_mux);
    strncpy(out, s_live_sentence, max - 1);
    out[max - 1] = '\0';
    portEXIT_CRITICAL(&s_live_mux);
}

static void live_append(const char *label) {
    portENTER_CRITICAL(&s_live_mux);
    if (s_live_sentence[0])
        strncat(s_live_sentence, " ", sizeof(s_live_sentence) - strlen(s_live_sentence) - 1);
    strncat(s_live_sentence, label, sizeof(s_live_sentence) - strlen(s_live_sentence) - 1);
    portEXIT_CRITICAL(&s_live_mux);
}

// ---------------------------------------------------------------------------
// Template persistence — versioned .tpl on SD.
// ---------------------------------------------------------------------------
static void write_tpl(const char *label, const vdsp_template m, int32_t radius)
{
    mkdir(TPL_PATH, 0775);
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.tpl", TPL_PATH, label);
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot write %s", path); return; }
    uint32_t magic = TPL_MAGIC;
    char hdr[32] = {0};
    snprintf(hdr, sizeof(hdr), "%s", label);
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(hdr, 1, 32, f);
    fwrite(m, sizeof(vdsp_template), 1, f);
    fwrite(&radius, sizeof(radius), 1, f);
    fclose(f);
    ESP_LOGI(TAG, "Template saved: %s (radius=%ld)", label, (long)radius);
}

// Pass 1: rename incompatible/old-format .tpl to .tpl.old so the Voice Manager
// web app (which lists raw .tpl) stops showing them as trained. Done in its own
// scan to avoid mutating the directory during the load enumeration.
static void quarantine_incompatible(void)
{
    char bad[8][256];
    int  nbad = 0;
    DIR *dir = opendir(TPL_PATH);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && nbad < 8) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".tpl") != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", TPL_PATH, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t magic = 0;
        size_t got = fread(&magic, sizeof(magic), 1, f);
        fclose(f);
        if (got != 1 || magic != TPL_MAGIC)
            snprintf(bad[nbad++], sizeof(bad[0]), "%s", e->d_name);
    }
    closedir(dir);
    for (int i = 0; i < nbad; i++) {
        char from[512], to[520];
        strlcpy(from, TPL_PATH, sizeof(from));
        strlcat(from, "/", sizeof(from));
        strlcat(from, bad[i], sizeof(from));
        strlcpy(to, from, sizeof(to));
        strlcat(to, ".old", sizeof(to));
        rename(from, to);
        ESP_LOGW(TAG, "Quarantined incompatible template '%s' -> .old (re-train it)", bad[i]);
    }
}

// Quarantine incompatible .tpl once per session (on first PTT). There is no
// resident load — templates are streamed from SD per match (tpl_match_sd).
static void tpls_quarantine_once(void)
{
    static bool done = false;
    if (done) return;
    mkdir(TPL_PATH, 0775);
    quarantine_incompatible();
    done = true;
}

// ---------------------------------------------------------------------------
// I2S helpers
// ---------------------------------------------------------------------------
static esp_err_t voice_mic_open(i2s_chan_handle_t *out)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan, NULL, &rx);
    if (err == ESP_OK) {
        i2s_pdm_rx_config_t cfg = {
            .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(VOICE_RATE_HZ),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = { .clk = NUCLEO_MIC_PIN_CLK, .din = NUCLEO_MIC_PIN_DATA,
                          .invert_flags = { .clk_inv = false } },
        };
        err = i2s_channel_init_pdm_rx_mode(rx, &cfg);
    }
    if (err == ESP_OK) err = i2s_channel_enable(rx);
    if (err != ESP_OK) { if (rx) i2s_del_channel(rx); return err; }
    *out = rx;
    return ESP_OK;
}

static void voice_mic_close(i2s_chan_handle_t rx)
{
    if (!rx) return;
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
}

static int rms_i16(const int16_t *s, int n)
{
    if (n <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int32_t)s[i] * s[i];
    return (int)sqrtf((float)(sum / n));
}

// ---------------------------------------------------------------------------
// Match a burst against every .tpl on SD, streaming one template at a time into
// s_scan (RAM-frugal: no resident cache). The winner's MFCC is copied into s_win
// with its label/radius for the accept gates + consolidation. Returns best
// distance; *second = runner-up (margin gate); *found=false if no templates.
// ---------------------------------------------------------------------------
static int32_t tpl_match_sd(const vdsp_template obs, int32_t *second, bool *found)
{
    int32_t best = INT32_MAX, sec = INT32_MAX;
    *found = false;
    s_win_label[0] = '\0';
    DIR *dir = opendir(TPL_PATH);
    if (!dir) { *second = sec; return best; }
    struct dirent *e;
    int scanned = 0;
    while ((e = readdir(dir)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".tpl") != 0) continue;
        char path[300];
        strlcpy(path, TPL_PATH, sizeof(path));
        strlcat(path, "/", sizeof(path));
        strlcat(path, e->d_name, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t magic = 0;
        char hdr[32] = {0};
        int32_t radius = VDSP_RADIUS_INIT;
        if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != TPL_MAGIC) { fclose(f); continue; }
        if (fread(hdr, 1, 32, f) != 32 || fread(s_scan, sizeof(vdsp_template), 1, f) != 1) { fclose(f); continue; }
        if (fread(&radius, sizeof(radius), 1, f) != 1 ||
            radius < VDSP_RADIUS_MIN || radius > VDSP_RADIUS_MAX) radius = VDSP_RADIUS_INIT;
        fclose(f);

        int32_t d = vdsp_dtw(obs, *s_scan);
        if (d < best) {
            sec = best; best = d; *found = true;
            memcpy(s_win, s_scan, sizeof(vdsp_template));
            s_win_radius = radius;
            snprintf(s_win_label, sizeof(s_win_label), "%s", hdr);
        } else if (d < sec) {
            sec = d;
        }
        if (++scanned >= VOICE_MAX_TPLS) break;   // bound worst-case match time
    }
    closedir(dir);
    *second = sec;
    return best;
}

// Snapshot the dispatched result for native pollers (the Voce console + the home-screen
// toast). Format outside the spinlock; only the struct copy + seq bump are inside it.
static void store_result(const char *sentence, int action, const char *reply, bool matched, bool launched)
{
    nucleo_voice_result_t v = {0};
    // strlcpy (not snprintf "%s") so the truncation is explicit and cannot trip the
    // component's -Werror=format-truncation on the small UI-bound buffers.
    strlcpy(v.sentence, sentence ? sentence : "", sizeof(v.sentence));
    strlcpy(v.reply,    reply ? reply : "",       sizeof(v.reply));
    v.action = action; v.matched = matched; v.launched = launched;
    portENTER_CRITICAL(&s_intro_mux);
    v.seq = s_last_result.seq + 1;
    s_last_result = v;
    portEXIT_CRITICAL(&s_intro_mux);
}

// ---------------------------------------------------------------------------
// Semantic Fusion: fuse the recognized words into a sentence, resolve it with
// ANIMA, then EITHER route to a connected web client OR act locally (launch app /
// speak). In test mode the result is recorded but NOT executed (no side effects).
// ---------------------------------------------------------------------------
static void semantic_dispatch(const char *tokens[], int ntok)
{
    if (ntok == 0) return;

    char sentence[128] = {0};
    for (int i = 0; i < ntok; i++) {
        if (i) strncat(sentence, " ", sizeof(sentence) - strlen(sentence) - 1);
        strncat(sentence, tokens[i], sizeof(sentence) - strlen(sentence) - 1);
    }
    ESP_LOGI(TAG, "Semantic fusion: '%s'", sentence);

    if (!nucleo_anima_try_lock()) {
        ESP_LOGW(TAG, "Anima busy, voice command dropped");
        nucleo_event_publish("voice/state", "{\"error\":\"anima_busy\"}");
        return;
    }
    anima_result_t r = nucleo_anima_query(sentence, VOICE_LANG);
    nucleo_anima_unlock();

    if (r.tier == ANIMA_TIER_NONE) {
        ESP_LOGW(TAG, "Anima: no intent for '%s'", sentence);
        store_result(sentence, ANIMA_ACT_NONE, "", false, false);
        char ev[256];
        snprintf(ev, sizeof(ev), "{\"sentence\":\"%s\",\"matched\":false}", sentence);
        nucleo_event_publish("voice/result", ev);
        return;
    }

    // For LAUNCH the human-facing payload is the app id; for everything else it is the reply.
    const char *disp = (r.action == ANIMA_ACT_LAUNCH) ? r.arg : r.reply;

    // Test mode: record what WOULD happen and stop — no launch, no TTS, no WS routing.
    if (s_test_mode) {
        store_result(sentence, r.action, disp, true, false);
        ESP_LOGI(TAG, "→ test mode: '%s' intent='%s' (not executed)", sentence, r.intent);
        return;
    }

    int ws_clients = nucleo_ws_client_count();
    ESP_LOGI(TAG, "Intent='%s' action=%d arg='%s' ws_clients=%d",
             r.intent, r.action, r.arg, ws_clients);

    if (ws_clients > 0) {
        if (r.action == ANIMA_ACT_LAUNCH) {
            char ev[128];
            snprintf(ev, sizeof(ev), "{\"action\":\"launch\",\"id\":\"%s\"}", r.arg);
            nucleo_event_publish("voice.launch", ev);
        } else {
            char ev[256];
            snprintf(ev, sizeof(ev), "{\"action\":\"answer\",\"reply\":\"%.*s\"}", 200, r.reply);
            nucleo_event_publish("voice/result", ev);
        }
        store_result(sentence, r.action, disp, true, false);   // the web client owns execution
        ESP_LOGI(TAG, "→ WS Client routed. Native UI muted.");
    } else {
        bool launched = false;
        if (r.action == ANIMA_ACT_LAUNCH) {
            const char *nat = nucleo_app_native_id(r.arg);   // id registro ("media-player") -> nativo ("music")
            launched = nucleo_app_launch_id(nat);
            ESP_LOGI(TAG, "→ Local launch '%s' (native '%s'): %s", r.arg, nat, launched ? "ok" : "not found");
            if (!launched) ESP_LOGW(TAG, "App non trovata");
        } else if (r.action == ANIMA_ACT_SYSTEM || r.action == ANIMA_ACT_ANSWER) {
            ESP_LOGI(TAG, "→ Local result: %.*s", 80, r.reply);
            // Voce on-device (nessun client web): parla, ma non conoscenza/calcolatrice -> "leggila".
            if (r.reply[0]) {
                bool knowledge = r.tier == ANIMA_TIER_FACT || r.tier == ANIMA_TIER_STITCH || r.tier == ANIMA_TIER_REMOTE;
                if (knowledge) {
                    // Voce a mosaico: di' il GIST breve (prima frase); say() legge se non coperto dal pool.
                    char gist[160]; nucleo_tts_first_sentence(r.reply, gist, sizeof gist);
                    nucleo_tts_say(gist, VOICE_LANG);
                // TRADUTTORE: pronuncia la traduzione nella SUA lingua (es. "dog" in inglese), non in IT.
                } else if (!strcmp(r.intent, "translate")) {
                    // BILINGUE: cornice "cane in inglese" (lingua UI=it) poi pausa poi la TRADUZIONE "dog"
                    // nella lingua target. Sorgente straniera (parola inglese) -> ripiego sulla sola parola.
                    char tw[80], tl[8];
                    if (nucleo_tts_translate_word(r.reply, tw, sizeof tw, tl, sizeof tl)) {
                        char *cm = strchr(tw, ','); if (cm) *cm = 0;
                        char *tgt = tw; while (*tgt == ' ') tgt++;
                        const char *src_lang = (tl[0] == 'e' && tl[1] == 'n') ? "it" : "en";
                        char p1[120]; int o = 0; const char *p1lang;
                        if (!strcmp(src_lang, "it")) {            // sorgente IT (= lingua UI del PTT) -> cornice intera
                            const char *colon = strstr(r.reply, ": ");
                            for (const char *p = r.reply; *p && (!colon || p < colon) && o < (int)sizeof(p1) - 1; p++)
                                if (*p != '"') p1[o++] = *p;
                            p1lang = VOICE_LANG;
                        } else {                                  // sorgente straniera (EN) -> solo la parola, sua lingua
                            const char *q1 = strchr(r.reply, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                            if (q1 && q2) for (const char *p = q1 + 1; p < q2 && o < (int)sizeof(p1) - 1; p++) p1[o++] = *p;
                            p1lang = src_lang;
                        }
                        p1[o] = 0;
                        if (p1[0]) { nucleo_tts_say(p1, p1lang); nucleo_audio_wait_idle(2500); }
                        nucleo_tts_say(tgt, tl);
                    }
                    else nucleo_tts_say(r.reply, VOICE_LANG);     // omografo/miss -> say (guard -> leggila)
                // Il calcolo NON va piu' a "leggila": say() "parlabilizza" = % ^ (mathspeak) -> "Fa 16" si dice.
                } else if (r.action == ANIMA_ACT_SYSTEM && strstr(r.reply, "{value}")) {
                    // Risolvi il template {value} PRIMA di parlare (come app_anima/speak_result): senza,
                    // si vocalizzerebbero le graffe -> guardia "sa di codice" -> "leggila". Qui (path voce,
                    // deps ridotte) risolviamo l'ORA on-device; gli altri valori di stato -> "leggila".
                    time_t now = time(NULL); struct tm tmv;
                    if (!strcmp(r.arg, "time") && now > 1672531200 && localtime_r(&now, &tmv)) {
                        char spoken[64];
                        nucleo_tts_speak_time(spoken, sizeof spoken, tmv.tm_hour, tmv.tm_min, VOICE_LANG);
                        nucleo_tts_say(spoken, VOICE_LANG);
                    } else nucleo_tts_read_hint(VOICE_LANG);
                } else if (nucleo_tts_has_mathtypo(r.reply)) {
                    // Formula densa (geometria/fisica) -> di' il solo RISULTATO numerico se pulito.
                    char res[48], spoken[80];
                    if (nucleo_tts_eq_result(r.reply, res, sizeof res)) {
                        snprintf(spoken, sizeof spoken, "Il risultato e' %s.", res);
                        nucleo_tts_say(spoken, VOICE_LANG);
                    } else nucleo_tts_say(r.reply, VOICE_LANG);
                } else {
                    nucleo_tts_say(r.reply, VOICE_LANG);
                }
            }
        }
        // Snapshot AFTER acting so `launched` is truthful — the home-screen toast reads this.
        store_result(sentence, r.action, disp, true, launched);
    }
}

// ---------------------------------------------------------------------------
// Handle one finished burst: learn it, or match → token (+ self-consolidate).
// `final` is true only when the mic is already CLOSED (PTT release): the
// self-consolidation write-back to SD is deferred to that point so it never
// blocks the loop while I2S DMA is live (a mid-PTT burst boundary lands during the
// user's deliberate pause, so its match reads only drop silence, never speech).
// ---------------------------------------------------------------------------
static void handle_burst(const vdsp_template obs, const char *tokens[], int *ntok, bool final)
{
    // Atomic consume of the learn arm (word+mode+deadline travel together — the armer writes them
    // from another task/core, see arm_learning_mode). SD write + publish stay OUTSIDE the critical.
    char lw[sizeof(s_learning_word)]; bool learning;
    portENTER_CRITICAL(&s_intro_mux);
    learning = s_learning_mode;
    if (learning) {
        memcpy(lw, s_learning_word, sizeof(lw));
        s_learning_mode = false; s_learn_deadline_us = 0;
    }
    portEXIT_CRITICAL(&s_intro_mux);
    if (learning) {
        write_tpl(lw, obs, VDSP_RADIUS_INIT);
        nucleo_event_publish("voice/learned", "{\"status\":\"ok\"}");
        s_learn_result = NUCLEO_VOICE_LEARN_OK;   // native trainer polls this to advance its UI
        voice_hold_set(VH_LEARN, false);          // one-shot: drop the transient hold
        return;
    }

    int32_t second = INT32_MAX;
    bool found = false;
    int32_t best = tpl_match_sd(obs, &second, &found);
    if (!found) return;

    bool ceil_ok   = best <= VOICE_ABS_CEILING;
    bool radius_ok = best <= s_win_radius;
    bool margin_ok = (second < INT32_MAX) &&
                     ((int64_t)best * VOICE_MARGIN_NUM <= (int64_t)second * VOICE_MARGIN_DEN);

    if (!ceil_ok || !(radius_ok || margin_ok)) {
        ESP_LOGD(TAG, "no match (best=%ld second=%ld radius=%ld)",
                 (long)best, (long)second, (long)s_win_radius);
        return;
    }
    if (*ntok < 16) {
        strlcpy(s_tokbuf[*ntok], s_win_label, sizeof(s_tokbuf[0]));  // copy: s_win_label is reused next match
        tokens[*ntok] = s_tokbuf[*ntok];
        (*ntok)++;
        live_append(s_win_label);
        ESP_LOGI(TAG, "match '%s' best=%ld second=%ld radius=%ld",
                 s_win_label, (long)best, (long)second, (long)s_win_radius);
    }
    // Telemetry for on-device threshold/health inspection (web + /api/logs).
    char ev[176];
    snprintf(ev, sizeof(ev), "{\"word\":\"%s\",\"dist\":%ld,\"second\":%ld,\"radius\":%ld}",
             s_win_label, (long)best, (long)second, (long)s_win_radius);
    nucleo_event_publish("voice/match", ev);

    // Snapshot the same telemetry for native pollers (the Voce console's live feed +
    // per-command reliability self-check). Built outside the spinlock, copied inside.
    nucleo_voice_match_t m = {0};
    strlcpy(m.word, s_win_label, sizeof(m.word));
    m.dist = best; m.second = second; m.radius = s_win_radius;
    portENTER_CRITICAL(&s_intro_mux);
    m.seq = s_last_match.seq + 1;
    s_last_match = m;
    portEXIT_CRITICAL(&s_intro_mux);

    // Self-improvement: an in-radius match sharpens the template AND tightens the radius
    // toward your real distances (margin-only accepts never loosen it). Deferred to PTT
    // release (final) so the SD write never stalls the loop while the mic DMA is live.
    if (radius_ok && final) {
        vdsp_consolidate(*s_win, obs, VOICE_ADAPT_ALPHA);
        int32_t nr = vdsp_radius_update(s_win_radius, best);
        write_tpl(s_win_label, *s_win, nr);
    }
}

// ---------------------------------------------------------------------------
// Free everything (called by the task when voice is disabled → RAM reclaim).
// Runs ONLY in the voice task, so no races with capture buffers.
// ---------------------------------------------------------------------------
static void voice_free_all(void)
{
    if (s_acc)  { vdsp_acc_free(s_acc);  s_acc  = NULL; }
    if (s_pcm)  { free(s_pcm);           s_pcm  = NULL; }
    if (s_scan) { free(s_scan);          s_scan = NULL; }
    if (s_win)  { free(s_win);           s_win  = NULL; }
    if (s_ctx)  { vdsp_ctx_free(s_ctx);  s_ctx  = NULL; }
}

// Lazy-allocate the per-session buffers + cache. Returns false on OOM.
static bool voice_ensure_ready(void)
{
    if (!s_ctx)  { s_ctx  = vdsp_ctx_create();                 if (!s_ctx)  { ESP_LOGE(TAG, "OOM ctx");  return false; } }
    if (!s_acc)  { s_acc  = vdsp_acc_create(s_ctx);            if (!s_acc)  { ESP_LOGE(TAG, "OOM acc");  return false; } }
    if (!s_pcm)  { s_pcm  = (int16_t *)malloc(VOICE_CHUNK * sizeof(int16_t)); if (!s_pcm) return false; }
    if (!s_scan) { s_scan = (vdsp_template *)malloc(sizeof(vdsp_template));   if (!s_scan) { ESP_LOGE(TAG, "OOM scan"); return false; } }
    if (!s_win)  { s_win  = (vdsp_template *)malloc(sizeof(vdsp_template));   if (!s_win)  { ESP_LOGE(TAG, "OOM win");  return false; } }
    tpls_quarantine_once();
    return true;
}

// ---------------------------------------------------------------------------
// Voice Task
// ---------------------------------------------------------------------------
static void voice_task(void *arg)
{
    ESP_LOGI(TAG, "AVCEB v2 engine started (lazy). FN=PTT.");
    portENTER_CRITICAL(&s_life_mux);
    s_task = xTaskGetCurrentTaskHandle();   // claim the slot (recompute reserved it as PENDING)
    portEXIT_CRITICAL(&s_life_mux);

    i2s_chan_handle_t rx         = NULL;
    bool              ptt_active = false;
    bool              have_burst = false;
    int               silence_ms = 0;
    int               noise_floor = VAD_NOISE_INIT;   // adaptive VAD, reset per PTT
    const char       *tokens[16];
    int               ntokens    = 0;
    vdsp_template     obs;

    while (1) {
        // No holder left (or exclusive-suspended) → free EVERYTHING and give the
        // 16 KB stack back by self-deleting. Re-checked under the lock so a holder
        // that re-appears during teardown cancels it (s_run flipped back to true).
        if (!s_run) {
            if (rx) {
                voice_mic_close(rx); rx = NULL; ptt_active = false; s_is_listening = false;
                nucleo_event_publish("voice/state", "{\"listening\":false}");  // don't leave the UI hung
            }
            voice_free_all();
            portENTER_CRITICAL(&s_life_mux);
            bool die = !s_run;
            if (die) s_task = NULL;
            portEXIT_CRITICAL(&s_life_mux);
            if (die) { ESP_LOGI(TAG, "voice engine down (-16 KB)"); vTaskDelete(NULL); }
            continue;   // re-held mid-teardown: keep running
        }

        // A web-armed learn that never received a PTT must not pin the engine forever. The
        // expiry fires even while STILL ARMED (s_learning_mode true) — otherwise arming the
        // engine from the web and never holding GO would pin the 16 KB task indefinitely —
        // but never while a capture is in flight (!ptt_active).
        if ((s_hold & VH_LEARN) && !ptt_active) {
            // Deadline is a 64-bit value written by the armer on another task/core: read+clear it
            // inside the critical section (a torn read could mis-fire or never fire the expiry).
            bool expired = false, was_armed = false;
            portENTER_CRITICAL(&s_intro_mux);
            if (s_learn_deadline_us && esp_timer_get_time() > s_learn_deadline_us) {
                s_learn_deadline_us = 0; expired = true;
                was_armed = s_learning_mode; s_learning_mode = false;
            }
            portEXIT_CRITICAL(&s_intro_mux);
            if (expired) {
                if (was_armed) {
                    s_learn_result = NUCLEO_VOICE_LEARN_TOO_SHORT;            // unstick the native trainer
                    nucleo_event_publish("voice/learned", "{\"status\":\"too_short\"}");  // and the web wizard
                }
                voice_hold_set(VH_LEARN, false);   // may drop s_run → teardown next iteration
                continue;
            }
        }

        // PTT is driven by the GO side button in the UI loop (nucleo_voice_ptt), the single
        // owner of the GPIO0 timing — so the engine never races that loop on the same pin.
        bool fn = s_ptt_on;

        // Reclaim an orphaned PTT hold: a GO press so brief that on(true)+off(false) both
        // happened before the task even saw the rising edge leaves VH_PTT set with nothing to
        // capture. Drop it here so the engine can tear down instead of staying pinned.
        if (!fn && !ptt_active && (s_hold & VH_PTT)) {
            voice_hold_set(VH_PTT, false);
            continue;
        }

        // ── PTT rising edge ──────────────────────────────────────────────────
        if (fn && !ptt_active) {
            // Fail-fast BEFORE the ~38 KB spin-up: if a TLS fetch is in flight (arbiter busy) or
            // the heap can't host the recognizer beside it, refuse this PTT. The fetch passed its
            // heap bars when the RAM was free — letting PTT grab ~24 KB contiguous + ~38 KB total
            // mid-handshake starves it past its own gate. No chime = the "not ready" cue (the
            // chime is the start-talking signal); the user just presses again in a moment.
            if (nucleo_arb_busy() ||
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < VOICE_PTT_MIN_BLOCK ||
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < VOICE_PTT_MIN_FREE) {
                ESP_LOGW(TAG, "PTT refused: arbiter/heap busy (block %u, free %u)",
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                voice_hold_set(VH_PTT, false);
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            // Sound the polyphonic "ready" chime FIRST: (1) before allocating the recognizer's
            // ~38 KB of buffers, so it plays with maximum free heap; (2) before opening the mic,
            // since the speaker and PDM mic share the I2S WS line (mutually exclusive). It is also
            // the user's cue for exactly when to start talking, covering the engine's spin-up.
            nucleo_voice_ready_chime();
            if (!s_ptt_on) { voice_hold_set(VH_PTT, false); continue; }  // released during the chime → glancing hold, nothing to capture
            if (!voice_ensure_ready()) { voice_hold_set(VH_PTT, false); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
            if (voice_mic_open(&rx) == ESP_OK) {
                vdsp_acc_reset(s_acc);
                have_burst = false;
                silence_ms = 0;
                ntokens    = 0;
                // Seed the VAD noise floor from a short ambient sniff (the MINIMUM RMS over a
                // few ~32 ms chunks) so the FIRST word is judged against the real room — captures
                // a quiet speaker in a silent room AND rejects ambient in a noisy one. MIN is
                // robust if the user starts talking immediately (syllable gaps stay low); the
                // clamp keeps an accidental loud onset from deafening the recognizer. Kept short
                // (~96 ms) to barely clip onset; the window length is an on-device tunable.
                noise_floor = VAD_NOISE_INIT;
                int seed = INT32_MAX;
                for (int w = 0; w < 3 && s_ptt_on; w++) {
                    size_t cg = 0;
                    i2s_channel_read(rx, s_pcm, VOICE_CHUNK * sizeof(int16_t), &cg, pdMS_TO_TICKS(50));
                    int cn = (int)(cg / sizeof(int16_t));
                    if (cn > 0) { int rr = rms_i16(s_pcm, cn); if (rr < seed) seed = rr; }
                }
                if (seed != INT32_MAX) {
                    if (seed < VAD_NOISE_INIT) seed = VAD_NOISE_INIT;
                    if (seed > 2000)           seed = 2000;
                    noise_floor = seed;
                }
                if (!s_ptt_on) {   // released during calibration → nothing to capture
                    voice_mic_close(rx); rx = NULL;
                    voice_hold_set(VH_PTT, false);
                    continue;
                }
                ptt_active = true;
                s_is_listening = true;
                portENTER_CRITICAL(&s_live_mux);
                s_live_sentence[0] = '\0';
                portEXIT_CRITICAL(&s_live_mux);
                nucleo_event_publish("voice/state", "{\"listening\":true}");
                ESP_LOGI(TAG, "PTT start (noise_floor=%d)", noise_floor);
            } else {
                ESP_LOGE(TAG, "mic open failed");
                voice_hold_set(VH_PTT, false);   // don't keep the engine pinned on a failed open
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        // ── PTT falling edge ─────────────────────────────────────────────────
        if (!fn && ptt_active) {
            ptt_active = false;
            s_is_listening = false;
            voice_mic_close(rx); rx = NULL;
            nucleo_event_publish("voice/state", "{\"listening\":false}");

            // Flush any pending burst — mic is closed now, so this is the `final` burst:
            // self-consolidation write-backs are allowed here (off the live-mic path).
            if (have_burst && vdsp_acc_finalize(s_acc, obs) > 0) {
                handle_burst(obs, tokens, &ntokens, true);
            }
            have_burst = false;

            bool was_learning;
            portENTER_CRITICAL(&s_intro_mux);
            was_learning = s_learning_mode;
            if (was_learning) { s_learning_mode = false; s_learn_deadline_us = 0; }
            portEXIT_CRITICAL(&s_intro_mux);
            if (was_learning) {
                // Armed but nothing captured → report too-short (both surfaces).
                nucleo_event_publish("voice/learned", "{\"status\":\"too_short\"}");
                s_learn_result = NUCLEO_VOICE_LEARN_TOO_SHORT;
                voice_hold_set(VH_LEARN, false);
            } else if (ntokens > 0) {
                semantic_dispatch(tokens, ntokens);
            }
            // EMA-adapted templates are written back to SD inline (handle_burst);
            // reclaim the big per-session buffers, keep only the small ctx cache.
            if (s_acc)  { vdsp_acc_free(s_acc); s_acc  = NULL; }
            if (s_pcm)  { free(s_pcm);  s_pcm  = NULL; }
            if (s_scan) { free(s_scan); s_scan = NULL; }
            if (s_win)  { free(s_win);  s_win  = NULL; }
            // End-of-interaction "done" beep. Played HERE — mic already closed, big buffers already
            // freed (so it runs with maximum free heap), and BEFORE dropping VH_PTT — because the UI
            // loop keeps the screen blanked + the canvas freed until VH_PTT clears: gating the hold
            // release on the beep is what makes the panel come back only AFTER the end beep finishes.
            // Skipped if GO is already held again (a re-press chains straight into the next capture).
            if (!s_ptt_on) nucleo_voice_done_chime();
            // PTT is done: drop our transient hold. The engine tears down (returns 16 KB) on the
            // next loop UNLESS another holder remains (always-on pin, Voice app, or an armed learn).
            // Guard: if GO was pressed AGAIN while we were recognizing (semantic_dispatch can be
            // slow), keep the hold so the engine survives and serves the new press immediately.
            if (!s_ptt_on) voice_hold_set(VH_PTT, false);
            ESP_LOGI(TAG, "PTT end, tokens=%d", ntokens);
            continue;
        }

        // ── Capture + VAD ────────────────────────────────────────────────────
        if (ptt_active && rx && s_pcm) {
            size_t got = 0;
            i2s_channel_read(rx, s_pcm, VOICE_CHUNK * sizeof(int16_t), &got, pdMS_TO_TICKS(50));
            int n = (int)(got / sizeof(int16_t));
            if (n <= 0) continue;

            int rms = rms_i16(s_pcm, n);
            int thr = noise_floor * VAD_MULT;
            if (thr < VAD_RMS_MIN) thr = VAD_RMS_MIN;
            if (thr > VAD_RMS_MAX) thr = VAD_RMS_MAX;
            if (rms >= thr) {
                silence_ms = 0;
                vdsp_acc_push(s_acc, s_pcm, n);
                have_burst = true;
            } else {
                // Track the ambient noise floor from non-speech frames: drop fast,
                // rise slow, so a brief loud blip doesn't desensitise the VAD.
                if (rms < noise_floor) noise_floor = rms;
                else                   noise_floor += (rms - noise_floor) / 16;
                silence_ms += (n * 1000) / VOICE_RATE_HZ;
                if (silence_ms >= VAD_SILENCE_MS && have_burst) {
                    // Mid-PTT word boundary (mic still open): match for the token but DEFER any
                    // self-consolidation write so the SD I/O can't stall the live DMA (final=false).
                    if (vdsp_acc_finalize(s_acc, obs) > 0)
                        handle_burst(obs, tokens, &ntokens, false);
                    vdsp_acc_reset(s_acc);
                    have_burst = false;
                    silence_ms = 0;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
// Bring the engine up/down to match the holders. Cheap and idempotent — called
// from any holder mutation (and from the task itself when a learn hold expires).
// The 16 KB task is created ONLY when a holder is present, and self-deletes when
// the last one leaves (see the teardown at the top of voice_task).
static void voice_recompute(void)
{
    bool need_create = false;
    portENTER_CRITICAL(&s_life_mux);
    bool want = (s_hold != 0) && !s_suspended;
    s_run = want;
    if (want && s_task == NULL) { s_task = VOICE_TASK_PENDING; need_create = true; }  // reserve the slot
    portEXIT_CRITICAL(&s_life_mux);

    if (need_create) {
        // Pinned to core 1 (real-time audio convention; nucleo_audio's player is there
        // too) so I2S capture + MFCC + DTW stay off core 0's Wi-Fi/lwIP/httpd.
        if (xTaskCreatePinnedToCore(voice_task, "voice_task", 16384, NULL, 4, NULL, 1) != pdPASS) {
            portENTER_CRITICAL(&s_life_mux); s_task = NULL; portEXIT_CRITICAL(&s_life_mux);
            ESP_LOGE(TAG, "voice task create failed");
        } else {
            ESP_LOGI(TAG, "voice engine up (+16 KB, on demand)");
        }
    }
}

static void voice_hold_set(uint32_t bit, bool on)
{
    portENTER_CRITICAL(&s_life_mux);
    if (on) s_hold |= bit; else s_hold &= ~bit;
    portEXIT_CRITICAL(&s_life_mux);
    voice_recompute();
}

// ── Persistent "always listen" (voice.alwaysOn in the shared SD settings.json) ──
// Read at boot to decide whether to pin the engine up; written by the native Voice
// app and POST /api/voice/always so the choice survives reboot.
#define VOICE_SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/settings.json"

static cJSON *voice_settings_load(void)   // caller owns the returned cJSON (or NULL)
{
    FILE *f = fopen(VOICE_SETTINGS_PATH, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static bool voice_read_setting_always_on(void)
{
    cJSON *root = voice_settings_load();
    if (!root) return false;
    cJSON *v = cJSON_GetObjectItem(root, "voice");
    bool on = v && cJSON_IsTrue(cJSON_GetObjectItem(v, "alwaysOn"));
    cJSON_Delete(root);
    return on;
}

// Read-modify-write: preserve every other key the web Settings app owns.
static void voice_persist_always_on(bool on)
{
    cJSON *root = voice_settings_load();
    if (!root) root = cJSON_CreateObject();
    cJSON *v = cJSON_GetObjectItem(root, "voice");
    if (!cJSON_IsObject(v)) { cJSON_DeleteItemFromObject(root, "voice"); v = cJSON_AddObjectToObject(root, "voice"); }
    if (v) {
        cJSON_DeleteItemFromObject(v, "alwaysOn");
        cJSON_AddBoolToObject(v, "alwaysOn", on);
    }
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return;
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s.tmp", VOICE_SETTINGS_PATH);
    FILE *w = fopen(tmp, "wb");
    if (w) {
        fwrite(out, 1, strlen(out), w); fclose(w);
        remove(VOICE_SETTINGS_PATH); rename(tmp, VOICE_SETTINGS_PATH);   // atomic-ish swap
    }
    cJSON_free(out);
}

void nucleo_voice_set_always_on(bool on)
{
    voice_hold_set(VH_PIN, on);     // live: engine up now / freed if no other holder
    voice_persist_always_on(on);    // survives reboot
    ESP_LOGI(TAG, "always-on %s", on ? "ON" : "OFF");
}

bool nucleo_voice_always_on(void) { return (s_hold & VH_PIN) != 0; }

esp_err_t nucleo_voice_init(void)
{
    // Lazy by design: NO task and NO heap at boot — the 16 KB engine is created on
    // demand and torn down (stack returned) when no holder remains. The ONE exception
    // is the user's persistent "always listen" opt-in: honour it here so PTT works
    // from the home screen for those who chose it.
    if (voice_read_setting_always_on()) {
        nucleo_voice_enable(true);   // pins VH_PIN → brings the engine up now
        ESP_LOGI(TAG, "AVCEB v2: always-on (from settings.json).");
    } else {
        ESP_LOGI(TAG, "AVCEB v2 ready (lazy; 0 KB until needed). PTT=FN key.");
    }
    return ESP_OK;
}

// ── Poll-based introspection for native apps (see header) ────────────────────
nucleo_voice_learn_t nucleo_voice_take_learn_result(void)
{
    nucleo_voice_learn_t r;
    portENTER_CRITICAL(&s_intro_mux);
    r = s_learn_result;
    s_learn_result = NUCLEO_VOICE_LEARN_NONE;   // one-shot: consumed on read
    portEXIT_CRITICAL(&s_intro_mux);
    return r;
}

bool nucleo_voice_last_match(nucleo_voice_match_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_intro_mux);
    *out = s_last_match;
    portEXIT_CRITICAL(&s_intro_mux);
    return out->seq != 0;
}

bool nucleo_voice_last_result(nucleo_voice_result_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_intro_mux);
    *out = s_last_result;
    portEXIT_CRITICAL(&s_intro_mux);
    return out->seq != 0;
}

void nucleo_voice_set_test_mode(bool on) { s_test_mode = on; }
bool nucleo_voice_test_mode(void)        { return s_test_mode; }

// Manual on/off pin (Voice app STATUS tab). Sticky until explicitly cleared.
void nucleo_voice_enable(bool enable) { voice_hold_set(VH_PIN, enable); }

// App/UI hold: keep the engine up while a foreground app needs it (released on leave).
void nucleo_voice_request(bool on) { voice_hold_set(VH_APP, on); }

// Push-to-Talk trigger from the GO side button (the UI loop owns the button timing: quick tap =
// torch, hold = talk). on=true engages PTT and brings the lazy engine up; the task then sounds the
// "ready" chime and captures while held. on=false ends it — but we DON'T drop the engine here: the
// task tears its own hold down only AFTER processing the captured burst, so a release can never race
// the recognizer to teardown (which would swallow the command).
void nucleo_voice_ptt(bool on)
{
    if (on) { s_ptt_on = true;  voice_hold_set(VH_PTT, true); }   // create the engine if down; it reads s_ptt_on next tick
    else      s_ptt_on = false;                                   // task sees the falling edge, processes, self-releases VH_PTT
}

// Exclusive-mode suspend — frees the engine WITHOUT clearing the holders, and
// restores it on exit only if a holder is still present (never spuriously recreates).
void nucleo_voice_suspend(bool suspend)
{
    portENTER_CRITICAL(&s_life_mux);
    s_suspended = suspend;
    portEXIT_CRITICAL(&s_life_mux);
    voice_recompute();
}

esp_err_t nucleo_voice_arm_learning_mode(const char *word)
{
    if (!word || !word[0]) return ESP_ERR_INVALID_ARG;
    // Two armers exist (httpd voice_learn_post + native trainer) and the voice task consumes on
    // core 1: word+mode+deadline must travel as one unit or the consumer can read a half-written
    // label (corrupt .tpl) / a torn 64-bit deadline. Short critical section, log outside.
    portENTER_CRITICAL(&s_intro_mux);
    strncpy(s_learning_word, word, sizeof(s_learning_word) - 1);
    s_learning_word[sizeof(s_learning_word) - 1] = '\0';
    s_learning_mode = true;
    s_learn_deadline_us = esp_timer_get_time() + VOICE_LEARN_TTL_US;
    portEXIT_CRITICAL(&s_intro_mux);
    voice_hold_set(VH_LEARN, true);   // ensure the engine is up to capture the next PTT
    ESP_LOGI(TAG, "Learning mode armed for word: '%s'", word);
    return ESP_OK;
}
