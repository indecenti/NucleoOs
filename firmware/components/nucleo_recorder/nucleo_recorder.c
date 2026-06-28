#include "nucleo_recorder.h"
#include "nucleo_auth.h"
#include "nucleo_board.h"
#include "nucleo_codec.h"   // board-aware mic HAL (PDM original / ES8311 ADC on ADV)
#include "nucleo_storage.h" // SD free-space guard for long takes
#include "nucleo_eventbus.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"   // effective-rate measurement (ADV mic slow-audio diagnosis)
#include "driver/i2s_pdm.h"
#include "lwip/sockets.h"   // SO_SNDTIMEO on the stream socket: a vanished client can't pin the mic
#include "cJSON.h"

static const char *TAG = "recorder";

// Capture format. Mono 16 kHz / 16-bit = ~32 KB/s on the SD — light and plenty
// for voice. The browser can resample/encode to MP3 afterwards.
#define REC_RATE_HZ   16000
#define REC_BITS      16
#define REC_CHANNELS  1
#define REC_DIR       NUCLEO_SD_MOUNT "/data/Recordings"

static i2s_chan_handle_t s_rx = NULL;
// Single-owner mic claim, taken with compare-exchange as the FIRST action of every entry point and
// released LAST on every exit path. The old check-then-act ("recording || task || streaming?" ...
// then claim) let two concurrent starts (httpd task + native UI) both pass the check and fight over
// the mic — one killed the other's take. Owner held until the worker FULLY exits (incl. WAV finalize).
enum { MIC_IDLE = 0, MIC_REC = 1, MIC_STREAM = 2 };
static atomic_int  s_owner = MIC_IDLE;
static atomic_bool s_recording = false;   // set false to ask the task to stop
static atomic_bool s_streaming = false;   // a /api/rec/stream client is holding the mic (live dictation)
static atomic_bool s_stream_stop = false; // ask the dictation worker to wind down (httpd is about to stop)
static TaskHandle_t s_task = NULL;
static char s_path[160] = {0};            // current/last file (web path, /data/...)
static uint32_t s_bytes = 0;              // PCM bytes written in the active file
static int s_level = 0;                   // last RMS level 0..100 (for the meter)

// Little-endian WAV header for streamed PCM. data_len/riff sizes are patched on stop.
static void write_wav_header(FILE *f, uint32_t data_len)
{
    uint32_t byte_rate = REC_RATE_HZ * REC_CHANNELS * (REC_BITS / 8);
    uint16_t block_align = REC_CHANNELS * (REC_BITS / 8);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16; memcpy(h + 16, &fmt_len, 4);
    uint16_t pcm = 1; memcpy(h + 20, &pcm, 2);
    uint16_t ch = REC_CHANNELS; memcpy(h + 22, &ch, 2);
    uint32_t rate = REC_RATE_HZ; memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    uint16_t bits = REC_BITS; memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_len, 4);
    fwrite(h, 1, sizeof(h), f);
}

static void publish_rec(const char *topic, const char *extra_json)
{
    char d[224];
    snprintf(d, sizeof(d), "{\"path\":\"%s\"%s%s}", s_path,
             extra_json ? "," : "", extra_json ? extra_json : "");
    nucleo_event_publish(topic, d);
}

// Bring up the PDM mic on demand: allocate + configure the RX channel for 16 kHz mono
// 16-bit and enable it. On success *out holds the channel; on failure nothing leaks.
// Shared by the SD recorder (record_task) and the live dictation stream (stream_get) —
// the two are mutually exclusive, so a single helper is safe.
// Board-aware mic HAL: PDM (original) or ES8311 standard-I2S ADC (ADV). See nucleo_codec.
static esp_err_t mic_open(i2s_chan_handle_t *out) { return nucleo_codec_mic_open(REC_RATE_HZ, out); }
static void mic_close(i2s_chan_handle_t rx)       { nucleo_codec_mic_close(rx); }

static void record_task(void *arg)
{
    char abs[200];
    snprintf(abs, sizeof(abs), NUCLEO_SD_MOUNT "%s", s_path);
    FILE *f = fopen(abs, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", abs);
        atomic_store(&s_recording, false); s_task = NULL; atomic_store(&s_owner, MIC_IDLE);
        vTaskDelete(NULL); return;
    }

    esp_err_t err = mic_open(&s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        fclose(f);
        atomic_store(&s_recording, false); s_task = NULL; atomic_store(&s_owner, MIC_IDLE);
        vTaskDelete(NULL); return;
    }

    write_wav_header(f, 0);                 // placeholder, patched at the end
    s_bytes = 0;

    // SD-space guard for long (1-2 h) takes: capture free bytes at start; stop GRACEFULLY before the card
    // fills (a failed fwrite would silently truncate/corrupt the WAV). 16 MB reserve keeps the FAT + the
    // header patch safe. A 2 h take is ~230 MB, so this only ever fires on a near-full card.
    const nucleo_storage_info_t *st0 = nucleo_storage_info();
    uint64_t free0 = st0 ? st0->free_bytes : 0;
    const uint64_t SD_RESERVE = 16ULL * 1024 * 1024;

    static int16_t buf[512];                // 1 KB DMA read chunk
    int meter_acc = 0, meter_n = 0;
    int64_t rec_t0 = esp_timer_get_time();  // DIAG: measure the REAL capture rate (bytes/time) on the ADV
    while (atomic_load(&s_recording)) {
        if (free0 && (uint64_t)s_bytes + SD_RESERVE >= free0) {   // card nearly full -> stop cleanly + notify
            ESP_LOGW(TAG, "SD nearly full (wrote %u of ~%llu free) — stopping take", (unsigned)s_bytes, (unsigned long long)free0);
            publish_rec("rec.lowspace", NULL);
            atomic_store(&s_recording, false);
            break;
        }
        size_t got = 0;
        // Read through the codec HAL, NOT i2s_channel_read directly: on the ADV it decimates the
        // ES8311's native 48 kHz down to REC_RATE_HZ, so the WAV plays back at the right speed
        // (a raw read there captured ~3x too slow). Pass-through on the original PDM mic.
        esp_err_t rd = nucleo_codec_mic_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200));
        // it returns ESP_ERR_TIMEOUT with a PARTIAL fill when it can't satisfy
        // the whole request within the timeout — the `got` bytes are still valid audio. The
        // old code skipped on any non-ESP_OK and so discarded every partial read, recording
        // 0 bytes forever. Only a genuine zero-byte read (or a hard error) is worth skipping.
        if (got == 0) {
            if (rd != ESP_OK && rd != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "i2s read: %s", esp_err_to_name(rd));
            continue;
        }
        fwrite(buf, 1, got, f);
        s_bytes += got;
        // cheap peak meter over the chunk -> rec.level event a few times/sec
        int n = got / 2, peak = 0;
        for (int i = 0; i < n; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > peak) peak = v; }
        meter_acc = peak; meter_n++;
        if (meter_n >= 4) {                 // ~ every 0.8 s of audio
            s_level = (meter_acc * 100) / 32768;
            char lv[32]; snprintf(lv, sizeof(lv), "\"level\":%d", s_level);
            publish_rec("rec.level", lv);
            meter_n = 0;
            int64_t el = esp_timer_get_time() - rec_t0;   // DIAG: effective Hz = samples / wall-time
            if (el > 200000)
                ESP_LOGW(TAG, "[rec-rate] %lld samp / %lld ms = %d Hz (atteso %d)",
                         (long long)(s_bytes / 2), (long long)(el / 1000),
                         (int)((int64_t)(s_bytes / 2) * 1000000 / el), REC_RATE_HZ);
        }
    }

    mic_close(s_rx);
    s_rx = NULL;
    fflush(f);
    fseek(f, 0, SEEK_SET);                   // patch sizes now that length is known
    write_wav_header(f, s_bytes);
    fclose(f);

    uint32_t secs = s_bytes / (REC_RATE_HZ * (REC_BITS / 8) * REC_CHANNELS);
    char meta[48]; snprintf(meta, sizeof(meta), "\"bytes\":%u,\"seconds\":%u", (unsigned)s_bytes, (unsigned)secs);
    publish_rec("rec.stopped", meta);
    nucleo_event_publish("fs.changed", "{\"op\":\"write\",\"path\":\"" REC_DIR "\"}");
    ESP_LOGI(TAG, "saved %s (%u bytes)", abs, (unsigned)s_bytes);
    s_task = NULL;
    atomic_store(&s_owner, MIC_IDLE);     // released LAST: no new claim can race the WAV finalize above
    vTaskDelete(NULL);
}

esp_err_t nucleo_recorder_init(void)
{
    // L'init hardware dell'I2S ora è differito per salvare RAM e DMA (avviato on-demand in record_task)
    ESP_LOGI(TAG, "PDM mic handler ready (On-Demand allocation mode)");
    return ESP_OK;
}

// ---- shared control API (used by both the HTTP handlers and app_recorder.cpp) ----
esp_err_t nucleo_recorder_start(void)
{
    int idle = MIC_IDLE;
    if (!atomic_compare_exchange_strong(&s_owner, &idle, MIC_REC)) return ESP_ERR_INVALID_STATE;

    mkdir(REC_DIR, 0775);
    // Name the take after the wall clock once NTP has set it: rec-YYYYMMDD-HHMMSS.wav sorts
    // chronologically and never collides. Before the first sync the clock reads 1970, so fall
    // back to the monotonic event sequence (which also never repeats within a boot).
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char fname[40];
    if (tm.tm_year + 1900 >= 2024)
        strftime(fname, sizeof(fname), "rec-%Y%m%d-%H%M%S.wav", &tm);
    else
        snprintf(fname, sizeof(fname), "rec-%u.wav", (unsigned)nucleo_event_current_seq());
    snprintf(s_path, sizeof(s_path), "/data/Recordings/%s", fname);
    s_level = 0;
    atomic_store(&s_recording, true);
    if (xTaskCreate(record_task, "rec", 4096, NULL, 5, &s_task) != pdPASS) {
        atomic_store(&s_recording, false);
        atomic_store(&s_owner, MIC_IDLE);
        return ESP_FAIL;
    }
    publish_rec("rec.started", NULL);
    return ESP_OK;
}

void        nucleo_recorder_stop(void)          { atomic_store(&s_recording, false); }
bool        nucleo_recorder_is_recording(void)  { return atomic_load(&s_recording); }
// True from the mic being CLAIMED until it is FULLY released (RX channel closed + WAV finalized):
// `s_owner` is cleared LAST in record_task (after mic_close), whereas is_recording() goes false the
// instant stop() is called — while the I2S RX channel is still open and draining. Speaker playback
// shares the I2S WS line, so it must gate on THIS, not is_recording(), to avoid a TX/RX collision.
bool        nucleo_recorder_is_busy(void)       { return atomic_load(&s_owner) != MIC_IDLE; }
int         nucleo_recorder_level(void)         { return s_level; }
uint32_t    nucleo_recorder_seconds(void)       { return s_bytes / (REC_RATE_HZ * (REC_BITS / 8) * REC_CHANNELS); }
const char *nucleo_recorder_path(void)          { return s_path; }

// POST /api/rec/start -> begins a new recording, returns its file path.
static esp_err_t start_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = nucleo_recorder_start();
    if (err == ESP_ERR_INVALID_STATE) {
        if (atomic_load(&s_recording) || s_task) {
            // ESP-IDF's httpd_err_code_t has no 409; set the status line explicitly.
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "already recording");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mic not ready");
        }
        return ESP_FAIL;
    }
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task"); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    char out[200]; snprintf(out, sizeof(out), "{\"ok\":true,\"recording\":true,\"path\":\"%s\"}", s_path);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// POST /api/rec/stop -> asks the task to finalise the WAV (header patched async).
static esp_err_t stop_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    nucleo_recorder_stop();
    httpd_resp_set_type(req, "application/json");
    char out[200]; snprintf(out, sizeof(out), "{\"ok\":true,\"recording\":false,\"path\":\"%s\"}", s_path);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// GET /api/rec/status -> current recorder state for the UI.
static esp_err_t status_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    // `busy` is the single truth the web mic gate (micgate.js) reads to know the mic is taken,
    // whichever way: an SD recording (s_recording) OR a live dictation stream (s_streaming).
    bool rec = nucleo_recorder_is_recording();
    bool str = atomic_load(&s_streaming);
    char out[256];
    snprintf(out, sizeof(out),
             "{\"recording\":%s,\"streaming\":%s,\"busy\":%s,\"path\":\"%s\",\"seconds\":%u,\"level\":%d,\"rate\":%d}",
             rec ? "true" : "false", str ? "true" : "false", (rec || str) ? "true" : "false", s_path,
             (unsigned)nucleo_recorder_seconds(), s_level, REC_RATE_HZ);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// GET /api/rec/stream -> live raw PCM (16 kHz mono int16 LE) for in-browser dictation.
// The browser feeds these samples straight into its own speech recognizer (Vosk WASM),
// so the on-device mic drives dictation with NO cloud and NO browser-mic permission (and
// no secure-context requirement, since the audio arrives over fetch, not getUserMedia).
//
// NON-BLOCKING: the handler hands the request to a worker task via the ESP-IDF async API
// (httpd_req_async_handler_begin) and returns immediately, so the single httpd task stays
// FREE to serve other requests DURING a dictation session — saving the transcript to SD,
// toggling the display, polling status. The worker owns the socket and streams until the
// client disconnects (send fails) or a safety cap, then completes the async request. The mic
// is exclusive: streaming and recording-to-SD never overlap (s_streaming / s_recording).
#define REC_STREAM_MAX_MS  (5 * 60 * 1000)     // hard cap so a wedged socket can't pin the mic forever
static TaskHandle_t s_stream_task = NULL;

static void stream_worker(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    i2s_chan_handle_t rx = NULL;
    esp_err_t err = mic_open(&rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream I2S init: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mic");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        // Bound the chunk send. If the client vanishes WITHOUT a TCP RST (laptop sleeps, Wi-Fi drops),
        // httpd_resp_send_chunk() would otherwise block indefinitely — s_stream_stop is only checked at
        // the loop top, so a wedged send pins s_streaming/s_owner past stream_abort()'s wait window. The
        // mic then stays CLAIMED with no live client, and the native Recorder falsely reports "mic busy
        // (web)". A 2 s send timeout guarantees the worker unwinds and releases the mic regardless.
        int sfd = httpd_req_to_sockfd(req);
        if (sfd >= 0) {
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        static int16_t buf[512];                 // 1 KB chunk = 32 ms of audio at 16 kHz
        uint32_t sent_ms = 0;
        ESP_LOGI(TAG, "dictation stream started");
        for (;;) {
            if (atomic_load(&s_stream_stop)) break;   // httpd is stopping: release the async req FIRST
            size_t got = 0;
            // Codec HAL read: decimates the ADV's 48 kHz ES8311 capture to REC_RATE_HZ so Vosk
            // (which expects 16 kHz mono) gets correctly-paced audio. Pass-through on the PDM mic.
            esp_err_t rd = nucleo_codec_mic_read(rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200));
            if (got == 0) {                      // partial-read timeout is normal; only log real errors
                if (rd != ESP_OK && rd != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "stream i2s read: %s", esp_err_to_name(rd));
                continue;
            }
            if (httpd_resp_send_chunk(req, (const char *)buf, got) != ESP_OK) break;   // client gone
            sent_ms += (uint32_t)(got / 2) * 1000 / REC_RATE_HZ;
            if (sent_ms >= REC_STREAM_MAX_MS) break;
        }
        mic_close(rx);
        httpd_resp_send_chunk(req, NULL, 0);     // terminate the chunked response (no-op if socket already gone)
        ESP_LOGI(TAG, "dictation stream ended");
    }
    httpd_req_async_handler_complete(req);       // release the async request + its socket
    atomic_store(&s_streaming, false);
    s_stream_task = NULL;
    atomic_store(&s_owner, MIC_IDLE);            // released LAST (after the async req is fully gone)
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    // Claim the mic: single CAS on the owner (same gate as recording-to-SD), so a refused stream
    // never clears a flag it didn't set and two entry points can never both pass the check.
    int idle = MIC_IDLE;
    if (!atomic_compare_exchange_strong(&s_owner, &idle, MIC_STREAM)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "mic busy");
        return ESP_FAIL;
    }
    atomic_store(&s_streaming, true);
    atomic_store(&s_stream_stop, false);          // fresh session: clear any abort left by a prior teardown
    // Detach the request onto a worker task so the single httpd task is freed immediately.
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        atomic_store(&s_streaming, false);
        atomic_store(&s_owner, MIC_IDLE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async");
        return ESP_FAIL;
    }
    if (xTaskCreate(stream_worker, "rec_stream", 4096, copy, 5, &s_stream_task) != pdPASS) {
        httpd_req_async_handler_complete(copy);   // release the async copy (it sends nothing, doesn't close the sock)
        atomic_store(&s_streaming, false);
        atomic_store(&s_owner, MIC_IDLE);
        return ESP_FAIL;                           // ESP_FAIL makes httpd close the socket so the client's fetch fails fast
    }
    return ESP_OK;                                 // worker streams on; httpd task is free
}

// Wind down a live dictation stream BEFORE the HTTP server stops. The worker holds an async copy of
// the request (httpd_req_async_handler_begin): stopping httpd under it is a use-after-free in the
// worker. Bounded wait (worker loop wakes at least every ~200 ms); on timeout we log and proceed —
// strictly better than the guaranteed race of not waiting at all.
void nucleo_recorder_stream_abort(void)
{
    if (!atomic_load(&s_streaming)) return;
    atomic_store(&s_stream_stop, true);
    // Budget covers the worst case: a chunk send wedged on a dead socket unblocks after the 2 s
    // SO_SNDTIMEO, then mic_close + async-complete. 3.5 s leaves margin so s_streaming is reliably
    // false (the async request fully released) before httpd_stop() proceeds.
    for (int i = 0; i < 175 && atomic_load(&s_streaming); i++) vTaskDelay(pdMS_TO_TICKS(20));   // <= 3.5 s
    if (atomic_load(&s_streaming)) ESP_LOGW(TAG, "stream worker did not wind down in time");
}

// Who holds the mic right now (s_owner is the single gate; values match nucleo_mic_owner_t).
nucleo_mic_owner_t nucleo_recorder_owner(void) { return (nucleo_mic_owner_t)atomic_load(&s_owner); }

// Non-blocking preempt for the on-device operator. Only a web dictation stream is preemptible; a
// finalizing record-to-SD take is the device's own and is left to complete.
bool nucleo_recorder_release_stream(void)
{
    int o = atomic_load(&s_owner);
    if (o == MIC_IDLE) return true;
    if (o == MIC_STREAM) atomic_store(&s_stream_stop, true);   // worker winds down; SO_SNDTIMEO bounds it
    return false;
}

esp_err_t nucleo_recorder_register(httpd_handle_t server)
{
    httpd_uri_t routes[] = {
        { .uri = "/api/rec/start",  .method = HTTP_POST, .handler = start_post },
        { .uri = "/api/rec/stop",   .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/rec/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/rec/stream", .method = HTTP_GET,  .handler = stream_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "recorder API ready: /api/rec/{start,stop,status,stream}");
    return ESP_OK;
}
