// nucleo_mic.c — the microphone an app may have (nucleo_mic.h). The claim, the board's mic and the speaker gate
// are the recorder's (nucleo_recorder_app_open / _close): one owner for every user of the mic.
#include "nucleo_mic.h"
#include "nucleo_app.h"
#include "nucleo_recorder.h"
#include "nucleo_codec.h"
#include "nucleo_audio.h"
#include "nucleo_micspec.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

static const char *TAG = "mic";

// store_apps.cpp: may the foreground app use the microphone (built-in, or a store app whose record grants "mic")?
extern bool nucleo_app_mic_allowed(void);

static i2s_chan_handle_t s_rx = NULL;
static int s_level = 0;

static void wdt(void) { if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); }

int nucleo_mic_open(int rate_hz)
{
    if (!nucleo_app_mic_allowed()) {
        ESP_LOGW(TAG, "%s: no microphone permission", nucleo_app_active_id() ? nucleo_app_active_id() : "?");
        return NUCLEO_MIC_DENIED;
    }
    if (rate_hz != NUCLEO_MIC_RATE) return NUCLEO_MIC_DENIED;
    if (s_rx) return 0;                                            // already ours
    if (nucleo_audio_is_playing() || nucleo_micspec_running()) return NUCLEO_MIC_BUSY;
    const esp_err_t e = nucleo_recorder_app_open(rate_hz, &s_rx);
    if (e == ESP_ERR_INVALID_STATE) return NUCLEO_MIC_BUSY;
    if (e != ESP_OK || !s_rx) { s_rx = NULL; ESP_LOGW(TAG, "open: %s", esp_err_to_name(e)); return NUCLEO_MIC_ERROR; }
    s_level = 0;
    return 0;
}

int nucleo_mic_read(int16_t *pcm, int max_samples, int timeout_ms)
{
    if (!s_rx) return NUCLEO_MIC_NOTOPEN;
    if (!pcm || max_samples <= 0) return 0;
    int got_total = 0, left = timeout_ms < 0 ? 0 : timeout_ms;
    // 100 ms slices that feed the watchdog; partial reads are valid audio (nucleo_codec_mic_read's contract).
    do {
        const int slice = left > 100 ? 100 : left;
        size_t got = 0;
        wdt();
        nucleo_codec_mic_read(s_rx, pcm + got_total, (size_t)(max_samples - got_total) * sizeof(int16_t), &got,
                              pdMS_TO_TICKS(slice > 0 ? slice : 1));
        got_total += (int)(got / sizeof(int16_t));
        left -= slice;
    } while (got_total < max_samples && left > 0);
    int peak = 0;
    for (int i = 0; i < got_total; i++) { const int v = abs((int)pcm[i]); if (v > peak) peak = v; }
    if (got_total) s_level = peak * 100 / 32768;
    return got_total;
}

int nucleo_mic_level(void) { return s_rx ? s_level : 0; }

void nucleo_mic_close(void)
{
    if (!s_rx) return;
    nucleo_recorder_app_close(s_rx);
    s_rx = NULL;
    s_level = 0;
}

void nucleo_mic_close_all(void) { nucleo_mic_close(); }
