// Mic Spectrum analysis engine — one DSP brain feeding every visual.
//
// Captures the PDM mic (16 kHz mono, shared format with the recorder) in a background
// task, runs a windowed radix-2 FFT + time-domain autocorrelation, and publishes a
// compact snapshot the native app renders as bars / waterfall / scope / tuner. No PSRAM:
// all scratch is a few KB on the heap, alive only while the engine runs.
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS_BANDS  32     // log-spaced perceptual display bands (bars + waterfall)
#define MS_WAVE   240    // auto-triggered scope samples (one per screen column)

// Errors surfaced to the app so it can explain a black screen instead of silently failing.
enum { MS_OK = 0, MS_ERR_BUSY = 1, MS_ERR_OOM = 2, MS_ERR_I2S = 3 };

// One analysis frame (~31/s). The app copies it under lock and reads it however each mode needs.
typedef struct {
    uint8_t  bands[MS_BANDS];  // 0..255 AGC-normalized, perceptually curved magnitude per band
    int16_t  wave[MS_WAVE];    // DC-removed, auto-triggered time samples (raw int16 amplitude)
    int      level;            // peak loudness 0..100 (for the meter)
    int      rms;              // RMS loudness 0..100
    int      level_db;         // peak dBFS (negative; 0 = full scale)
    int      dom_hz;           // dominant spectral frequency (parabola-refined)
    int      centroid_hz;      // spectral centroid — perceived "brightness"
    int      pitch_cHz;        // detected fundamental * 100 (centi-Hz); 0 = unvoiced
    int      note_idx;         // 0..11 (C..B), or -1 when unvoiced
    int      octave;           // scientific octave of the detected note
    int      cents;            // deviation from equal temperament, -50..+50
    int      clarity;          // pitch confidence 0..100
    uint8_t  onset;            // spectral-flux transient strength 0..255 (decays) — beat reactivity
    uint32_t seq;              // increments per frame (poll for "is there a new one")
} ms_snapshot_t;

// Open the mic + start the DSP task. Returns ESP_ERR_INVALID_STATE if the mic is already
// taken (recording/dictation) — last_error() then reports MS_ERR_BUSY.
esp_err_t   nucleo_micspec_start(void);
void        nucleo_micspec_stop(void);
bool        nucleo_micspec_running(void);

// Copy the most recent snapshot. False until the first frame is produced.
bool        nucleo_micspec_get(ms_snapshot_t *out);

int         nucleo_micspec_last_error(void);
const char *nucleo_micspec_note_name(int idx);  // "C".."B", or "-" for idx<0

#ifdef __cplusplus
}
#endif
