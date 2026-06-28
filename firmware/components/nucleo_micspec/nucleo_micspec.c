// Mic Spectrum DSP engine. See nucleo_micspec.h. Pure ESP-IDF + <math.h>.
#include "nucleo_micspec.h"
#include "nucleo_board.h"
#include "nucleo_codec.h"             // board-aware mic HAL (PDM original / ES8311 ADC on ADV)
#include "nucleo_recorder.h"          // mic single-owner gate (is_busy)
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_pdm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MS_RATE  16000
#define MS_FFT   512                  // 32 ms window
#define MS_BINS  (MS_FFT / 2)         // 256 usable magnitude bins (0..8 kHz)
#define BIN_HZ   ((float)MS_RATE / (float)MS_FFT)   // 31.25 Hz / bin

static const char *TAG = "micspec";

// Speaker playback shares GPIO43 with the mic (WS == PDM clk). Refuse to grab the mic while a track/
// chime is playing so we never collide the I2S TX line. Symmetric to nucleo_audio's micspec_running() guard.
extern bool nucleo_audio_is_playing(void);
static const char *NOTE_NAMES[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

static i2s_chan_handle_t s_rx   = NULL;
static TaskHandle_t      s_task = NULL;
static atomic_bool       s_run  = false;
static atomic_int        s_err  = MS_OK;
static SemaphoreHandle_t s_lock = NULL;
static ms_snapshot_t     s_pub;                 // published frame (guarded by s_lock)
static bool              s_have = false;

// Per-run scratch: one combined 10 KB block so the allocator sees a single request, not five
// 2 KB fragments. On the ADV (no-PSRAM, ~16 KB largest free after NX_NET_APP exclusive) five
// separate malloc(2048) would exhaust the largest block before the task stack is created.
static float *s_scratch = NULL;       // base of the combined block (5 * MS_FFT floats)
static float *s_td   = NULL;          // DC-removed time window (for autocorrelation)
static float *s_re   = NULL;          // FFT real (Hann-windowed input, then real part)
static float *s_im   = NULL;          // FFT imag
static float *s_hann = NULL;          // precomputed Hann window
static float *s_acf  = NULL;          // autocorrelation cache (indexed by lag)
static float  s_prevband[MS_BANDS];   // previous frame bands (spectral flux / onset)
static int    s_bin_lo[MS_BANDS], s_bin_hi[MS_BANDS];  // log band -> FFT bin span
static float  s_agc   = 1.0f;         // adaptive reference (EMA of frame max magnitude)
static float  s_onset = 0.0f;         // decaying onset envelope

// ---- in-place radix-2 FFT (mirrors nucleo_voice_dsp's host-verified core) ----
static void ms_fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float vr = re[b] * cwr - im[b] * cwi;
                float vi = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] = re[a] + vr; im[a] = im[a] + vi;
                float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr; cwr = nwr;
            }
        }
    }
}

// Log-spaced band edges (55 Hz .. 7.6 kHz), each at least one bin wide.
static void build_bands(void)
{
    const float f_lo = 55.0f, f_hi = 7600.0f;
    for (int b = 0; b < MS_BANDS; b++) {
        float fa = f_lo * powf(f_hi / f_lo, (float)b / MS_BANDS);
        float fb = f_lo * powf(f_hi / f_lo, (float)(b + 1) / MS_BANDS);
        int la = (int)(fa / BIN_HZ), lb = (int)(fb / BIN_HZ);
        if (la < 1) la = 1;
        if (lb <= la) lb = la + 1;
        if (lb > MS_BINS) lb = MS_BINS;
        s_bin_lo[b] = la; s_bin_hi[b] = lb;
    }
}

// Board-aware mic HAL: PDM (original) or ES8311 standard-I2S ADC (ADV). See nucleo_codec.
static esp_err_t mic_open(void) { return nucleo_codec_mic_open(MS_RATE, &s_rx); }
static void mic_close(void)     { nucleo_codec_mic_close(s_rx); s_rx = NULL; }

// Block until `need` samples are read (handling i2s partial-read timeouts), or the engine stops.
static bool fill_window(int16_t *raw, int need)
{
    int got_total = 0;
    while (got_total < need && atomic_load(&s_run)) {
        size_t got = 0;
        // Codec HAL read: on the ADV it decimates the ES8311's native 48 kHz to MS_RATE (16 kHz),
        // so the FFT bins and pitch detection map to true frequencies. Pass-through on the PDM mic.
        esp_err_t rd = nucleo_codec_mic_read(s_rx, (char *)(raw + got_total),
                                        (need - got_total) * sizeof(int16_t), &got, pdMS_TO_TICKS(200));
        if (got > 0) got_total += (int)(got / sizeof(int16_t));
        else if (rd != ESP_OK && rd != ESP_ERR_TIMEOUT) return false;
    }
    return got_total >= need;
}

static float acf_at(const float *x, int n, int lag)
{
    float r = 0.0f;
    for (int i = 0; i + lag < n; i++) r += x[i] * x[i + lag];
    return r;
}

// Monophonic pitch via normalized autocorrelation + parabolic peak refinement.
static void detect_pitch(const float *x, int n, float rms_norm, ms_snapshot_t *s)
{
    s->note_idx = -1; s->pitch_cHz = 0; s->cents = 0; s->octave = 0; s->clarity = 0;
    if (rms_norm < 0.010f) return;                       // silence: don't invent a note

    int minlag = (int)(MS_RATE / 1000.0f);               // up to ~1 kHz fundamental
    int maxlag = (int)(MS_RATE / 70.0f);                 // down to ~70 Hz
    if (maxlag > n - 2) maxlag = n - 2;

    float r0 = acf_at(x, n, 0);
    if (r0 < 1.0f) return;

    float best = 0.0f; int bl = 0;
    for (int lag = minlag; lag <= maxlag; lag++) {
        float r = acf_at(x, n, lag);
        s_acf[lag] = r;
        if (r > best) { best = r; bl = lag; }
    }
    if (bl <= minlag) return;

    float clarity = best / r0;                            // 0..1, monophonic confidence
    if (clarity < 0.45f) return;

    // Parabolic interpolation around the peak lag for sub-sample period accuracy.
    float a = s_acf[bl - 1], b = s_acf[bl], c = (bl + 1 <= maxlag) ? s_acf[bl + 1] : s_acf[bl];
    float denom = (a - 2.0f * b + c);
    float shift = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;
    if (shift > 1.0f) shift = 1.0f; else if (shift < -1.0f) shift = -1.0f;
    float period = (float)bl + shift;
    if (period < 1.0f) return;

    float f0 = (float)MS_RATE / period;
    float midi_f = 69.0f + 12.0f * log2f(f0 / 440.0f);
    int   midi = (int)lroundf(midi_f);
    if (midi < 12 || midi > 108) return;                 // outside a sane musical range

    s->pitch_cHz = (int)lroundf(f0 * 100.0f);
    s->note_idx  = ((midi % 12) + 12) % 12;
    s->octave    = midi / 12 - 1;
    s->cents     = (int)lroundf((midi_f - midi) * 100.0f);
    s->clarity   = (int)lroundf(clarity * 100.0f);
}

static void process_frame(int16_t *raw)
{
    ms_snapshot_t s; memset(&s, 0, sizeof s);

    // --- time domain: DC removal, peak + RMS loudness ---
    long mean = 0;
    for (int i = 0; i < MS_FFT; i++) mean += raw[i];
    int dc = (int)(mean / MS_FFT);
    double sumsq = 0; int peak = 0;
    for (int i = 0; i < MS_FFT; i++) {
        int v = raw[i] - dc;
        s_td[i] = (float)v;
        int av = v < 0 ? -v : v;
        if (av > peak) peak = av;
        sumsq += (double)v * v;
    }
    float rms = sqrtf((float)(sumsq / MS_FFT));
    float peak_norm = peak / 32768.0f, rms_norm = rms / 32768.0f;
    s.level    = (int)(peak_norm * 140.0f); if (s.level > 100) s.level = 100;
    s.rms      = (int)(rms_norm  * 200.0f); if (s.rms  > 100) s.rms  = 100;
    s.level_db = peak_norm > 0.0003f ? (int)(20.0f * log10f(peak_norm)) : -90;

    // --- frequency domain ---
    for (int i = 0; i < MS_FFT; i++) { s_re[i] = s_td[i] * s_hann[i]; s_im[i] = 0.0f; }
    ms_fft(s_re, s_im, MS_FFT);

    // magnitude, dominant bin, spectral centroid
    float mag_max = 0.0f; int dom_bin = 1;
    double csum = 0, cw = 0;
    static float mag[MS_BINS];
    for (int k = 1; k < MS_BINS; k++) {
        float m = sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]);
        mag[k] = m;
        if (m > mag_max) { mag_max = m; dom_bin = k; }
        csum += (double)m * k; cw += m;
    }
    // parabolic refine of the dominant peak
    {
        float al = mag[dom_bin > 1 ? dom_bin - 1 : 1], be = mag[dom_bin],
              ce = mag[dom_bin < MS_BINS - 1 ? dom_bin + 1 : dom_bin];
        float den = al - 2 * be + ce, sh = den != 0 ? 0.5f * (al - ce) / den : 0;
        s.dom_hz = (int)((dom_bin + sh) * BIN_HZ);
    }
    s.centroid_hz = cw > 1 ? (int)((csum / cw) * BIN_HZ) : 0;

    // AGC: track the running max so quiet and loud sources both fill the display.
    s_agc = s_agc * 0.92f + mag_max * 0.08f;
    if (s_agc < 1.0f) s_agc = 1.0f;
    float ref = s_agc * 1.15f;

    // bands: average magnitude per log band -> perceptual curve -> 0..255; spectral flux for onset
    float flux = 0.0f;
    for (int b = 0; b < MS_BANDS; b++) {
        float acc = 0.0f; int lo = s_bin_lo[b], hi = s_bin_hi[b];
        for (int k = lo; k < hi; k++) acc += mag[k];
        float bm = acc / (hi - lo);
        float norm = bm / ref; if (norm > 1.0f) norm = 1.0f;
        float vis = powf(norm, 0.5f);                    // lift low energy for the eye
        int   val = (int)(vis * 255.0f); if (val > 255) val = 255;
        s.bands[b] = (uint8_t)val;
        float d = norm - s_prevband[b]; if (d > 0) flux += d;   // only rising energy = onset
        s_prevband[b] = norm;
    }
    // onset envelope: fast attack on a flux spike, slow decay -> a beat pulse the UI can read
    float onset_in = flux / MS_BANDS;                    // 0..1
    if (onset_in > s_onset) s_onset = onset_in; else s_onset = s_onset * 0.80f;
    int onv = (int)(s_onset * 900.0f); if (onv > 255) onv = 255;
    s.onset = (uint8_t)onv;

    detect_pitch(s_td, MS_FFT, rms_norm, &s);

    // --- scope: auto-trigger on a rising zero crossing for a stable trace ---
    int start = 0;
    for (int i = 1; i < MS_FFT / 2; i++) {
        if (s_td[i - 1] < 0 && s_td[i] >= 0) { start = i; break; }
    }
    for (int i = 0; i < MS_WAVE; i++) {
        int idx = start + i; if (idx >= MS_FFT) idx = MS_FFT - 1;
        float v = s_td[idx];
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        s.wave[i] = (int16_t)v;
    }

    s.seq = s_pub.seq + 1;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(&s_pub, &s, sizeof s_pub);
        s_have = true;
        xSemaphoreGive(s_lock);
    }
}

static void dsp_task(void *arg)
{
    (void)arg;
    static int16_t raw[MS_FFT];
    while (atomic_load(&s_run)) {
        if (!fill_window(raw, MS_FFT)) break;
        if (!atomic_load(&s_run)) break;
        process_frame(raw);
    }
    mic_close();
    s_task = NULL;
    atomic_store(&s_run, false);
    vTaskDelete(NULL);
}

static void free_scratch(void)
{
    free(s_scratch); s_scratch = NULL;
    s_td = s_re = s_im = s_hann = s_acf = NULL;
}

esp_err_t nucleo_micspec_start(void)
{
    if (atomic_load(&s_run)) return ESP_OK;
    if (nucleo_recorder_is_busy() || nucleo_audio_is_playing()) { atomic_store(&s_err, MS_ERR_BUSY); return ESP_ERR_INVALID_STATE; }

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_scratch = (float *)malloc(sizeof(float) * MS_FFT * 5);   // one 10 KB block
    if (!s_lock || !s_scratch) {
        atomic_store(&s_err, MS_ERR_OOM); free_scratch(); return ESP_ERR_NO_MEM;
    }
    s_td   = s_scratch;
    s_re   = s_scratch + MS_FFT;
    s_im   = s_scratch + MS_FFT * 2;
    s_hann = s_scratch + MS_FFT * 3;
    s_acf  = s_scratch + MS_FFT * 4;
    for (int i = 0; i < MS_FFT; i++) s_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (MS_FFT - 1));
    memset(s_prevband, 0, sizeof s_prevband);
    s_agc = 1.0f; s_onset = 0.0f; s_have = false;
    memset(&s_pub, 0, sizeof s_pub);
    build_bands();

    esp_err_t err = mic_open();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mic open: %s", esp_err_to_name(err));
        atomic_store(&s_err, MS_ERR_I2S); free_scratch(); return err;
    }
    atomic_store(&s_err, MS_OK);
    atomic_store(&s_run, true);
    if (xTaskCreate(dsp_task, "micspec", 4096, NULL, 5, &s_task) != pdPASS) {
        atomic_store(&s_run, false); mic_close(); free_scratch();
        atomic_store(&s_err, MS_ERR_OOM); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "spectrum engine up");
    return ESP_OK;
}

void nucleo_micspec_stop(void)
{
    if (!atomic_load(&s_run) && !s_task) { free_scratch(); return; }
    atomic_store(&s_run, false);
    for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));   // <= ~600 ms for the task to exit
    free_scratch();
    ESP_LOGI(TAG, "spectrum engine down");
}

bool nucleo_micspec_running(void) { return atomic_load(&s_run); }
int  nucleo_micspec_last_error(void) { return atomic_load(&s_err); }

bool nucleo_micspec_get(ms_snapshot_t *out)
{
    if (!out || !s_lock) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_have) { memcpy(out, &s_pub, sizeof *out); ok = true; }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

const char *nucleo_micspec_note_name(int idx)
{
    return (idx >= 0 && idx < 12) ? NOTE_NAMES[idx] : "-";
}
