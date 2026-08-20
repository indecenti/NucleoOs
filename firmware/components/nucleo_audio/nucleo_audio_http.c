// Live MP3 radio over plain HTTP: ONE task (the audio player task) pulls the endless Icecast-style
// stream with esp_http_client, decodes it frame-by-frame with Helix (fixed-point, no PSRAM) and
// pushes PCM to the shared I2S sink.
//
// SINGLE-TASK BY MEASUREMENT, not by accident. A producer/consumer split (dedicated "radio-rx" task
// + a FreeRTOS stream-buffer jitter ring) was built, flashed and measured on the ADV: the extra
// task stack (6 KB) + second HTTP context + the ring itself pushed the streaming session past what
// this PSRAM-less heap can carry next to the ~20 KB Helix decoder — esp_http_client_init died with
// "Allocation failed", then socket creation with "Connection failed, sock < 0", in an endless loop,
// and the radio never made a sound. The proven shape is this one: the read blocks at most
// timeout_ms, the I2S DMA paces the loop, and the freed RAM is worth more to the Wi-Fi RX pool
// than any jitter ring we can afford.
//
// The session order matters: the I2S sink is pre-opened BEFORE the decoder grabs the big blocks
// (its DMA descriptors are the first thing to die on a carved-up heap, and a failed open used to be
// discovered only at the first frame — total silence with every other subsystem green).
//
// A radio is meant to play forever: a dropped connection reconnects, an EAGAIN read timeout is a
// WAIT (three in a row = a dead stream worth rebuilding), and the decoder resyncs on the next MP3
// sync word. No TLS on purpose — the device reaches stations over plain HTTP (see the Radio app).
#include "nucleo_audio_priv.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "nucleo_eventbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "audio.http";

#define IN_SZ   NUCLEO_AUDIO_IN_SZ    // shared decode scratch sizes (nucleo_audio_priv.h)
#define OUT_SZ  NUCLEO_AUDIO_OUT_SZ

void nucleo_audio_stream_url(const char *url)
{
    // Fail-closed on https://: one https station in user-editable radio.json would drop a ~40-50 KB
    // mbedTLS handshake next to the decoder on this task's stack, inside an endless reconnect loop.
    if (!strncmp(url, "https://", 8)) {
        ESP_LOGW(TAG, "https station refused (radio is HTTP-only): %s", url);
        nucleo_event_publish("radio.error", "{\"reason\":\"https_unsupported\"}");
        return;
    }

    // I2S FIRST (see header). Every station in the catalog streams 44.1 kHz stereo; a different
    // first frame just re-tunes the already-open channel (cheap).
    esp_err_t ie0 = nucleo_audio_i2s_rate(44100, 2);
    ESP_LOGW(TAG, "i2s preopen: %s(0x%x)", esp_err_to_name(ie0), (unsigned)ie0);
    if (ie0 != ESP_OK)
        ESP_LOGW(TAG, "i2s preopen failed: %s (free=%u largest=%u)", esp_err_to_name(ie0),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        nucleo_audio_do_reclaim();                 // free what the app layer can, then one more try
        dec = MP3InitDecoder();
    }
    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder failed (out of RAM?) free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        nucleo_event_publish("radio.error", "{\"reason\":\"no_ram_decoder\"}");
        return;
    }
    ESP_LOGW(TAG, "stream start: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    uint8_t *in = nucleo_audio_in;        // shared scratch — file & radio decoders never run at once
    int16_t *out = nucleo_audio_out;
    bool logged = false;

    // Reconnect loop: keeps the radio alive across network blips until stop is requested.
    while (nucleo_audio_keep_running()) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 4000,    // headers/read leash — measured: 3 s missed slow first responses;
                                   // still under the engine's 4.5 s stop wait
            .buffer_size = 1024,
            .user_agent = "NucleoOS-Radio/1.0",
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        esp_err_t err = esp_http_client_open(cli, 0);          // GET, no request body
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open %s failed: %s", url, esp_err_to_name(err));
            esp_http_client_cleanup(cli);
            for (int i = 0; i < 15 && nucleo_audio_keep_running(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            continue;                                          // back off ~1.5 s, retry
        }
        esp_http_client_fetch_headers(cli);                    // ignore length: the stream is live
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGW(TAG, "connected: %s (status %d)", url, status);
        if (status < 200 || status >= 400) {                   // bad/timed-out response -> back off, retry
            esp_http_client_close(cli); esp_http_client_cleanup(cli);
            for (int i = 0; i < 40 && nucleo_audio_keep_running(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            continue;                                          // ~4 s: don't hammer the DNS/server
        }

        uint8_t *rp = in; int left = 0; bool dropped = false;
        uint32_t rx_total = 0; int zero_reads = 0;
        while (nucleo_audio_keep_running() && !dropped) {
            if (left < 1441) {                                 // refill: keep at least one whole frame
                memmove(in, rp, left); rp = in;
                int n = esp_http_client_read(cli, (char *)(in + left), IN_SZ - left);
                if (n == -ESP_ERR_HTTP_EAGAIN) n = 0;          // benign timeout — NOT a dead stream
                if (n > 0) { left += n; rx_total += (uint32_t)n; zero_reads = 0; nucleo_audio_add_file_bytes((uint32_t)n); }
                else if (n < 0) { ESP_LOGW(TAG, "read error %d after %u B", n, (unsigned)rx_total); dropped = true; }
                else if (++zero_reads >= 3) {                  // ~12 s of true silence -> rebuild
                    ESP_LOGW(TAG, "silent stream after %u B — reconnecting", (unsigned)rx_total);
                    dropped = true;
                }
                if (dropped) break;
                if (left == 0) continue;                       // nothing yet: wait again (stop stays live)
            }
            int off = MP3FindSyncWord(rp, left);
            if (off < 0) { left = 0; continue; }
            rp += off; left -= off;

            int e = MP3Decode(dec, &rp, &left, out, 0);
            if (e == ERR_MP3_NONE) {
                MP3FrameInfo fi; MP3GetLastFrameInfo(dec, &fi);
                if (fi.outputSamps > 0) {
                    esp_err_t ie = nucleo_audio_i2s_rate(fi.samprate, fi.nChans);
                    if (ie != ESP_OK) {                        // dead sink = total silence: heal, don't shrug
                        nucleo_audio_do_reclaim();
                        ie = nucleo_audio_i2s_rate(fi.samprate, fi.nChans);
                    }
                    if (!logged) {
                        logged = true;
                        ESP_LOGW(TAG, "first frame: %d Hz, %d ch, %d kbps, i2s=%s(0x%x) free=%u largest=%u",
                                 fi.samprate, fi.nChans, fi.bitrate / 1000, esp_err_to_name(ie), (unsigned)ie,
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                    }
                    nucleo_audio_i2s_write(out, (size_t)fi.outputSamps * sizeof(int16_t));
                    nucleo_audio_add_samples((uint32_t)(fi.outputSamps / (fi.nChans < 1 ? 1 : fi.nChans)), fi.samprate);
                }
            } else if (e == ERR_MP3_INDATA_UNDERFLOW || e == ERR_MP3_MAINDATA_UNDERFLOW) {
                left = 0;                                       // need more bytes -> refill
            } else {
                if (left > 0) { rp++; left--; }                 // skip a byte past the bad frame
            }
        }

        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        if (nucleo_audio_keep_running()) { ESP_LOGW(TAG, "stream dropped, reconnecting"); vTaskDelay(pdMS_TO_TICKS(500)); }
    }
    MP3FreeDecoder(dec);
}
