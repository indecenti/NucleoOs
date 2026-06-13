// Device-side notification backbone. Thin glue over two pure, host-tested pieces:
//   - notify_synth.h    : polyphonic chime synthesized to SD (streamed, ~zero RAM)
//   - notify_journal.h  : append-only, size-rotated history (O(1), ~zero RAM)
// Everything here is event-driven — called when something happens, never on a timer. See
// docs/notify-protocol.md and nucleo_notify.h.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_err.h"

extern "C" {
#include "nucleo_board.h"
#include "nucleo_audio.h"
#include "nucleo_notify.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}
#include "notify_synth.h"
#include "notify_journal.h"

// Surfaces owned elsewhere (the UI task and the bus); declared, not linked here.
extern "C" unsigned int nucleo_event_publish(const char *topic, const char *payload);
extern "C" void nucleo_notify_post(const char *title, const char *body);  // on-screen banner (UI task)
extern "C" bool nucleo_ui_is_remote(void);                                // true while a web client drives the device

#define JRNL_PATH NUCLEO_SD_MOUNT "/system/notify.jsonl"
#define SND_DIR   NUCLEO_SD_MOUNT "/system/sounds"
#define JRNL_CAP  6144L                                                    // ~6 KB live + ~6 KB backup = bounded history

static const char *lvl_str(notify_level_t l)
{
    switch (l) { case NOTIFY_SUCCESS: return "success"; case NOTIFY_WARN: return "warn";
                 case NOTIFY_CRITICAL: return "critical"; default: return "info"; }
}
static notify_snd_t lvl_snd(notify_level_t l)
{
    switch (l) { case NOTIFY_SUCCESS: return NOTIFY_SND_SUCCESS; case NOTIFY_WARN: return NOTIFY_SND_WARN;
                 case NOTIFY_CRITICAL: return NOTIFY_SND_CRITICAL; default: return NOTIFY_SND_INFO; }
}
static notify_src_t src_id(const char *src)
{
    if (!src) return NOTIFY_SRC_SYSTEM;
    if (!strcmp(src, "calendar")) return NOTIFY_SRC_CALENDAR;
    if (!strcmp(src, "anima"))    return NOTIFY_SRC_ANIMA;
    if (!strcmp(src, "voice"))    return NOTIFY_SRC_VOICE;
    if (!strcmp(src, "recorder")) return NOTIFY_SRC_RECORDER;
    if (!strcmp(src, "ota"))      return NOTIFY_SRC_OTA;
    if (!strcmp(src, "app"))      return NOTIFY_SRC_APP;
    return NOTIFY_SRC_SYSTEM;
}

// Minimal JSON string escaper into a bounded buffer; clips at `max` chars of SOURCE so the bus
// payload stays inside NUCLEO_EVENT_PAYLOAD_MAX (208 B). Always NUL-terminates and never emits a
// dangling escape. Returns bytes written (excl. NUL).
static int json_escape(char *dst, int dstsz, const char *src, int max)
{
    int o = 0;
    if (dstsz <= 0) return 0;
    for (int i = 0; src && src[i] && i < max && o < dstsz - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *rep = nullptr; char esc[3] = { '\\', 0, 0 };
        if (c == '"') { esc[1] = '"'; rep = esc; }
        else if (c == '\\') { esc[1] = '\\'; rep = esc; }
        else if (c == '\n') { esc[1] = 'n'; rep = esc; }
        else if (c == '\r') { esc[1] = 'r'; rep = esc; }
        else if (c == '\t') { esc[1] = 't'; rep = esc; }
        else if (c < 0x20) continue;                          // drop other control chars
        if (rep) { if (o >= dstsz - 2) break; dst[o++] = rep[0]; dst[o++] = rep[1]; }
        else dst[o++] = (char)c;
    }
    dst[o] = 0;
    return o;
}

// Lazily synthesize the (source, level) earcon to SD (once per combo; cached). Writes the path into
// the caller's buffer (reentrant — no shared state) and returns it, or NULL on a synth failure.
static const char *ensure_chime(const char *src, notify_level_t lvl, char *path, int sz)
{
    snprintf(path, sz, "%s/notify_%s_%s.wav", SND_DIR, src ? src : "system", lvl_str(lvl));
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 44) return path;             // already cached
    mkdir(SND_DIR, 0777);
    if (notify_synth_wav(src_id(src), lvl_snd(lvl), path, 0) != 0) return nullptr;   // 0 -> 12 kHz
    return path;
}

extern "C" void nucleo_notify_emit(const char *src, notify_level_t lvl, const char *id,
                                   const char *title, const char *body, const char *action)
{
    if (!src) src = "system";
    long ts = (long)time(NULL);

    // Escaped, length-clipped fields so the JSON stays valid AND inside the 208 B bus slot.
    char e_src[16], e_ttl[80], e_bd[64], e_act[40], e_id[28];
    json_escape(e_src, sizeof e_src, src, 12);
    json_escape(e_ttl, sizeof e_ttl, title ? title : "", 72);
    json_escape(e_bd,  sizeof e_bd,  body ? body : "", 56);
    json_escape(e_act, sizeof e_act, action ? action : "", 36);
    json_escape(e_id,  sizeof e_id,  id ? id : "", 24);

    // Compact payload (no icon: the web maps src->icon). ts in seconds (the web upscales to ms).
    char payload[208];
    int n = snprintf(payload, sizeof payload,
        "{\"src\":\"%s\",\"lvl\":\"%s\",\"ttl\":\"%s\",\"bd\":\"%s\",\"act\":\"%s\",\"id\":\"%s\",\"ts\":%ld}",
        e_src, lvl_str(lvl), e_ttl, e_bd, e_act, e_id, ts);
    if (n < 0) return;
    if (n >= (int)sizeof payload) {                                       // overflow guard: drop body, retry
        snprintf(payload, sizeof payload,
            "{\"src\":\"%s\",\"lvl\":\"%s\",\"ttl\":\"%s\",\"bd\":\"\",\"act\":\"%s\",\"id\":\"%s\",\"ts\":%ld}",
            e_src, lvl_str(lvl), e_ttl, e_act, e_id, ts);
    }

    // 1) History on SD (append + O(1) size rotation; reader parses on demand, not us).
    notify_journal_append(JRNL_PATH, payload, JRNL_CAP);

    // 2) Broadcast — the web Notification Center renders it (toast + history + chime).
    nucleo_event_publish("notify.post", payload);

    // 3) Standalone surface: the device owns the alert only when no web client is driving it.
    if (!nucleo_ui_is_remote()) {
        char wavpath[80];
        const char *wav = ensure_chime(src, lvl, wavpath, sizeof wavpath);
        // Skip the earcon if audio is ALREADY playing (e.g. the ANIMA confirmation voice): a chime from a
        // background task (cal-svc) must not stomp/contend the foreground voice. The playback lock makes it
        // race-SAFE either way, but skipping keeps the voice intact and avoids needless stop/start churn.
        if (wav && !nucleo_audio_is_playing()) nucleo_audio_play(wav);    // INVALID_STATE while recording — harmless
        nucleo_notify_post(title ? title : src, body ? body : "");
    }
}

// Polyphonic "ready, talk now" earcon for Push-to-Talk. The voice engine calls this the instant it
// engages, BEFORE opening the mic: the speaker and the PDM mic share the I2S WS line (GPIO43), so
// playback and capture are mutually exclusive — they MUST be sequential. It also gives the user a
// precise cue for when to start speaking, which conveniently covers the engine's spin-up latency.
// We reuse the same source-signature ("voice") + level-chord (SUCCESS = ascending "go!") synth that
// notifications use, so it's a single cached WAV on SD (synth runs at most once, ever). BLOCKS until
// the short chime finishes (≤ ~1.2 s safety cap) so the caller can safely claim the shared WS line
// next. The voice component reaches this via a link-time extern (no REQUIRES cycle on nucleo_app).
extern "C" void nucleo_voice_ready_chime(void)
{
    char path[80];
    const char *wav = ensure_chime("voice", NOTIFY_SUCCESS, path, sizeof path);
    if (!wav) return;
    if (nucleo_audio_play(wav) != ESP_OK) return;          // busy (e.g. a recording holds the I2S) → skip, never block PTT
    vTaskDelay(pdMS_TO_TICKS(30));                          // let the player task claim the channel before we poll
    for (int i = 0; i < 120 && nucleo_audio_is_playing(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    nucleo_audio_stop();                                    // free the I2S TX pins before the mic claims the shared WS line
}

// "Done listening" earcon for Push-to-Talk: the counterpart to the ready chime, played by the voice
// engine AFTER the mic has closed and recognition has run, to close the interaction (and, by design,
// it gates the UI restoring the blanked screen — the panel comes back only once this finishes). A
// distinct neutral chord (NOTIFY_INFO) so it reads as "finished", not the ascending "go!" of SUCCESS.
// Cached separately on SD. The mic is already released here, so there is no WS-line contention; we
// still stop the player afterwards to free the I2S TX pins. Reached via a link-time extern (see above).
extern "C" void nucleo_voice_done_chime(void)
{
    char path[80];
    const char *wav = ensure_chime("voice", NOTIFY_INFO, path, sizeof path);
    if (!wav) return;
    if (nucleo_audio_play(wav) != ESP_OK) return;          // player busy → skip, never block the PTT teardown
    vTaskDelay(pdMS_TO_TICKS(30));
    for (int i = 0; i < 120 && nucleo_audio_is_playing(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    nucleo_audio_stop();
}
