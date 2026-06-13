// Live MP3 radio over plain HTTP. Pulls an endless Icecast-style MP3 stream with
// esp_http_client, decodes it frame-by-frame with Helix (fixed-point, ~30 KB, no PSRAM)
// and pushes PCM to the shared I2S sink. A radio is meant to play forever, so a dropped
// connection just reconnects rather than ending playback. No TLS on purpose — the device
// reaches the station over HTTP (see the Radio app); TLS would not fit beside the decoder
// on this PSRAM-less chip.
#include "nucleo_audio_priv.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "nucleo_eventbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "audio.http";

#define IN_SZ   NUCLEO_AUDIO_IN_SZ    // shared decode scratch sizes (nucleo_audio_priv.h)
#define OUT_SZ  NUCLEO_AUDIO_OUT_SZ

void nucleo_audio_stream_url(const char *url)
{
    // Fail-closed on https://. The CA bundle used to be attached here "just in case", which made
    // TLS reachable de-facto: one https station in radio.json (user-editable) put a ~40-50 KB
    // mbedTLS handshake — outside the arbiter, with no heap bar, on this task's 8 KB stack —
    // INSIDE an endless ~1.5 s reconnect loop, right beside the Helix decoder. Product rule is
    // "radio = plain HTTP" (header note above); now the code enforces it instead of gambling.
    if (!strncmp(url, "https://", 8)) {
        ESP_LOGW(TAG, "https station refused (radio is HTTP-only): %s", url);
        nucleo_event_publish("radio.error", "{\"reason\":\"https_unsupported\"}");
        return;
    }

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { ESP_LOGE(TAG, "MP3InitDecoder failed (out of RAM?)"); return; }

    uint8_t *in = nucleo_audio_in;        // shared scratch — file & radio decoders never run at once
    int16_t *out = nucleo_audio_out;
    bool logged = false;

    // Reconnect loop: keeps the radio alive across network blips until stop is requested.
    while (nucleo_audio_keep_running()) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 3000,    // bounds how long a blocked read can ignore a stop request: a
                                   // longer timeout left this task stuck in read() after stop, so it
                                   // closed the shared I2S LATE — after the next app had reopened it
                                   // → device audio dead until reboot. 3 s = quick, clean handoff.
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
        ESP_LOGI(TAG, "connected: %s (status %d)", url, status);
        if (status < 200 || status >= 400) {                   // bad endpoint -> back off, retry
            esp_http_client_close(cli); esp_http_client_cleanup(cli);
            for (int i = 0; i < 20 && nucleo_audio_keep_running(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t *rp = in; int left = 0; bool dropped = false;
        while (nucleo_audio_keep_running() && !dropped) {
            if (left < 1441) {                                 // refill: keep at least one frame
                memmove(in, rp, left); rp = in;
                int n = esp_http_client_read(cli, (char *)(in + left), IN_SZ - left);
                if (n > 0) { left += n; nucleo_audio_add_file_bytes((uint32_t)n); }
                else { dropped = true; break; }                // 0/<0 -> server closed -> reconnect
            }
            int off = MP3FindSyncWord(rp, left);
            if (off < 0) { left = 0; continue; }
            rp += off; left -= off;

            int e = MP3Decode(dec, &rp, &left, out, 0);
            if (e == ERR_MP3_NONE) {
                MP3FrameInfo fi; MP3GetLastFrameInfo(dec, &fi);
                if (fi.outputSamps > 0) {
                    esp_err_t ie = nucleo_audio_i2s_rate(fi.samprate, fi.nChans);
                    if (!logged) {
                        logged = true;
                        ESP_LOGI(TAG, "first frame: %d Hz, %d ch, %d kbps, i2s=%s",
                                 fi.samprate, fi.nChans, fi.bitrate / 1000, esp_err_to_name(ie));
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
