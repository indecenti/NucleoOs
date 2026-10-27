// presence_detect.c — see presence_detect.h.
#include "presence_detect.h"
#include <math.h>
#include <string.h>

static void reset_norm(pd_state_t *s)
{
    s->vmin = s->pmin = 0;
    s->vmax = s->pmax = 0.001f;
}

void pd_init(pd_state_t *s, float thr, uint32_t now_ms)
{
    memset(s, 0, sizeof *s);
    s->thr = thr < 0.05f ? 0.05f : thr > 0.95f ? 0.95f : thr;
    s->rssi = -90;
    s->bucket_start_ms = now_ms;
    pd_calibrate(s, now_ms);
}

void pd_calibrate(pd_state_t *s, uint32_t now_ms)
{
    reset_norm(s);
    s->phase = PD_CALIB;
    s->calib_start_ms = now_ms;
    s->motion = s->shown = s->held = 0;
    s->over = 0;
    if (s->present && s->n_ev && !s->ev[0].end_ms) s->ev[0].end_ms = now_ms ? now_ms : 1;
    s->present = s->moving = false;
}

// The callback runs in the Wi-Fi task: keep it to one pass over the subcarriers and a ring write.
// Only the L-LTF's real tones count: the buffer starts with 64 (imag, real) pairs in the order
// k = 0..31, -32..-1; k = 0 is DC and |k| > 26 are guard bands (always ~0, pure noise).
// Amplitude feature = spatial turbulence std/mean across tones: the radio's AGC scales every tone of a
// packet alike, so a gain step cancels out instead of reading as motion.
void pd_feed(pd_state_t *s, const int8_t *b, int len, int8_t rssi, bool first_word_invalid)
{
    int n = len / 2 < 64 ? len / 2 : 64, cnt = 0, valid = 0;
    if (n < 8) return;
    float sum = 0, sum2 = 0, sin_sum = 0;
    for (int i = first_word_invalid ? 2 : 0; i < n; i++) {
        int k = i < 32 ? i : i - 64;
        if (k == 0 || k > 26 || k < -26) continue;
        float im = b[2 * i], re = b[2 * i + 1];
        float a = sqrtf(re * re + im * im);
        sum += a; sum2 += a * a; cnt++;
        if (a > 1e-4f) { sin_sum += im / a; valid++; }
    }
    if (cnt < 8 || sum < 1e-3f) return;
    float mean = sum / cnt, var = sum2 / cnt - mean * mean;
    int h = (int)(s->frames % PD_WIN);
    s->amp[h] = sqrtf(var > 0 ? var : 0) / mean;
    s->pha[h] = valid ? sin_sum / valid : 0;
    s->rssi = rssi;
    s->frames++;
}

static float variance(const float *v, int n)
{
    float m = 0, var = 0;
    for (int i = 0; i < n; i++) m += v[i];
    m /= n;
    for (int i = 0; i < n; i++) { float d = v[i] - m; var += d * d; }
    return var / n;
}

// Asymmetric-EMA floor (falls fast, rises slowly) + a peak that decays -> [0,1].
// The range never shrinks below PD_NOISE_SPAN x the floor: in a still room the peak decays onto the noise,
// and a plain (x - lo) / (hi - lo) then stretches pure noise to full scale (the original firmware's false
// "presence" in an empty room). Motion has to beat the room's own noise by that margin to count.
#define PD_NOISE_SPAN 4.0f
static float normalise(float x, float *lo, float *hi)
{
    if (*lo < 0.0001f) *lo = x;
    else *lo += (x - *lo) * (x < *lo ? 0.1f : 0.002f);
    if (x > *hi) *hi = x; else *hi += (x - *hi) * 0.005f;
    float r = *hi - *lo;
    if (r < *lo * PD_NOISE_SPAN) r = *lo * PD_NOISE_SPAN;
    float m = r > 0.0001f ? (x - *lo) / r : 0;
    return m < 0 ? 0 : m > 1 ? 1 : m;
}

static void history(pd_state_t *s, uint32_t now_ms)
{
    uint8_t v = (uint8_t)(s->shown * 255.0f + 0.5f);
    if (v > s->bucket_max) s->bucket_max = v;
    while (now_ms - s->bucket_start_ms >= PD_BUCKET_MS) {
        s->hist[s->hist_head] = s->bucket_max;
        s->hist_head = (s->hist_head + 1) % PD_HIST;
        s->bucket_max = 0;
        s->bucket_start_ms += PD_BUCKET_MS;
        if (now_ms - s->bucket_start_ms > PD_HIST * PD_BUCKET_MS) s->bucket_start_ms = now_ms;   // long stall
    }
}

void pd_step(pd_state_t *s, uint32_t now_ms)
{
    int n = s->frames < PD_WIN ? (int)s->frames : PD_WIN;
    float m = 0;
    if (n >= 8) {
        float a[PD_WIN], p[PD_WIN];
        memcpy(a, s->amp, sizeof a);   // snapshot: the Wi-Fi task keeps writing; a torn sample is harmless
        memcpy(p, s->pha, sizeof p);
        m = 0.6f * normalise(variance(a, n), &s->vmin, &s->vmax)
          + 0.4f * normalise(variance(p, n), &s->pmin, &s->pmax);
    }
    s->motion = m;

    if (s->phase == PD_CALIB) {
        // Learning the empty / still room: the floors settle, nothing is reported.
        s->shown = 0;
        if (now_ms - s->calib_start_ms >= PD_CALIB_MS) {
            s->phase = PD_RUN;
            s->vmax = s->vmin * 1.5f + 0.001f;   // forget peaks seen while the user walked away
            s->pmax = s->pmin * 1.5f + 0.001f;
            s->clear_since_ms = now_ms;
        }
        history(s, now_ms);
        return;
    }

    s->moving = m > s->thr;
    s->over = s->moving ? s->over + 1 : 0;
    if (s->over >= 2) {                                  // two steps in a row: not a single noisy frame
        if (!s->present) {
            s->present = true;
            s->held = 0;
            memmove(&s->ev[1], &s->ev[0], sizeof s->ev[0] * (PD_EVENTS - 1));
            s->ev[0].start_ms = now_ms; s->ev[0].end_ms = 0; s->ev[0].peak = 0;
            if (s->n_ev < PD_EVENTS) s->n_ev++;
        }
        s->last_motion_ms = now_ms;
        if (m > s->held) s->held = m;
        s->shown = m;
    } else if (s->present) {
        uint32_t since = now_ms - s->last_motion_ms;
        if (since >= PD_HOLD_MS) {
            s->present = false;
            s->shown = 0;
            s->clear_since_ms = now_ms;
            s->ev[0].end_ms = now_ms ? now_ms : 1;
        } else {
            float fade = 1.0f - (float)since / PD_HOLD_MS;   // 1 -> 0 over the hold
            s->shown = s->held * (0.10f + 0.90f * fade);
            if (m > s->shown) s->shown = m;
        }
    } else {
        s->shown = m < s->thr ? m * 0.5f : m;               // sub-threshold ripples stay visible, damped
    }
    if (s->present) {
        uint8_t pk = (uint8_t)(s->held * 255.0f + 0.5f);
        if (pk > s->ev[0].peak) s->ev[0].peak = pk;
    }
    history(s, now_ms);
}

float pd_calib_progress(const pd_state_t *s, uint32_t now_ms)
{
    if (s->phase != PD_CALIB) return 1;
    float p = (float)(now_ms - s->calib_start_ms) / PD_CALIB_MS;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

float pd_distance_m(int rssi)
{
    float d = powf(10.0f, (-40.0f - (float)rssi) / 27.0f);
    return d < 0.5f ? 0.5f : d > 30 ? 30 : d;
}

int pd_quality(int rssi, int rate)
{
    int q_sig = rssi >= -50 ? 100 : rssi <= -85 ? 0 : (rssi + 85) * 100 / 35;
    int q_rate = rate >= 20 ? 100 : rate * 5;
    return q_sig < q_rate ? q_sig : q_rate;
}
