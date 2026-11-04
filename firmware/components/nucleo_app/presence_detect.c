// presence_detect.c — see presence_detect.h.
#include "presence_detect.h"
#include <math.h>
#include <string.h>

static void reset_norm(pd_state_t *s)
{
    s->lo = 0;
    s->ref_ok = false;
    for (int i = 0; i < PD_FLOOR_N; i++) s->fmin[i] = -1;
    s->fhead = 0;
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
    s->fbucket_ms = now_ms;
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
// Feature = 1 - Pearson correlation between this frame's amplitude profile and the reference profile.
// Correlation ignores a common gain, so an AGC step is not motion; phase is not used at all, because
// without sanitising it carries a random offset per packet (CFO, timing) that drowns any body.
#define PD_REF_ALPHA 0.05f          // reference profile follows ~20 frames (~0.5 s at 40 frames/s)
void pd_feed(pd_state_t *s, const int8_t *b, int len, int8_t rssi, bool first_word_invalid)
{
    int n = len / 2 < 64 ? len / 2 : 64, cnt = 0;
    if (n < 8) return;
    float a[PD_TONES], sum = 0;
    for (int i = first_word_invalid ? 2 : 0; i < n && cnt < PD_TONES; i++) {
        int k = i < 32 ? i : i - 64;
        if (k == 0 || k > 26 || k < -26) continue;
        float im = b[2 * i], re = b[2 * i + 1];
        a[cnt] = sqrtf(re * re + im * im);
        sum += a[cnt++];
    }
    if (cnt < 8 || sum < 1e-3f) return;
    float inv = cnt / sum;           // profile with mean 1: the gain is gone before it reaches the reference
    for (int j = 0; j < cnt; j++) a[j] *= inv;
    float d = 0;
    if (s->ref_ok) {
        float sxy = 0, sxx = 0, syy = 0, mr = 0;
        for (int j = 0; j < cnt; j++) mr += s->ref[j];
        mr /= cnt;
        for (int j = 0; j < cnt; j++) {
            float x = a[j] - 1.0f, y = s->ref[j] - mr;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        float r = sxx > 1e-9f && syy > 1e-9f ? sxy / sqrtf(sxx * syy) : 1.0f;
        d = r < 1 ? 1 - r : 0;
        for (int j = 0; j < cnt; j++) s->ref[j] += (a[j] - s->ref[j]) * PD_REF_ALPHA;
    } else {
        memcpy(s->ref, a, sizeof(float) * cnt);
        for (int j = cnt; j < PD_TONES; j++) s->ref[j] = 1;
        s->ref_ok = true;
    }
    s->dev[s->frames % PD_WIN] = d;
    s->rssi = rssi;
    s->frames++;
}

// Noise floor = the lowest level seen over the last 2 minutes (PD_FLOOR_N buckets of PD_FLOOR_MS), measured
// on the ADV: an empty room sits at a flat level with no dips (0.04 there), while people moving about give
// high peaks with frequent dips well below it (0.02); someone standing still next to the device reads even
// lower (0.01). A floor that rose as an average either chased three people walking about until they read
// "clear" within a minute (fast) or kept the low level learnt while the user calibrated next to it and
// called the empty room "present" (slow). The minimum does neither; a calibration done next to the device
// is forgotten 2 minutes later.
static void floor_push(pd_state_t *s, float x, uint32_t now_ms)
{
    if (now_ms - s->fbucket_ms >= PD_FLOOR_MS) {
        s->fhead = (s->fhead + 1) % PD_FLOOR_N;
        s->fmin[s->fhead] = -1;
        s->fbucket_ms = now_ms;
    }
    if (s->fmin[s->fhead] < 0 || x < s->fmin[s->fhead]) s->fmin[s->fhead] = x;
    float lo = -1;
    for (int i = 0; i < PD_FLOOR_N; i++)
        if (s->fmin[i] >= 0 && (lo < 0 || s->fmin[i] < lo)) lo = s->fmin[i];
    s->lo = lo;
}

// Motion = how far the level stands above the floor, in units of PD_NOISE_SPAN floors -> [0,1]: at the
// default threshold (0.35) a level 2.4x the room's own noise counts. No scaling by a decaying peak: on the
// ADV a burst of 3 people (level 40x the floor) stretched the scale for a minute and their ordinary moving
// about (3-4x the floor) then read as "clear".
#define PD_NOISE_SPAN 4.0f
static float normalise(float x, float lo)
{
    float r = lo * PD_NOISE_SPAN;
    float m = r > 1e-7f ? (x - lo) / r : 0;
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
        float x = 0;   // the Wi-Fi task keeps writing the ring; a torn sample is harmless
        for (int i = 0; i < n; i++) x += s->dev[i];
        s->level = x / n;
        floor_push(s, s->level, now_ms);
        m = normalise(s->level, s->lo);
    }
    s->motion = m;

    if (s->phase == PD_CALIB) {
        // Learning the empty / still room: the floors settle, nothing is reported.
        s->shown = 0;
        if (now_ms - s->calib_start_ms >= PD_CALIB_MS) {
            s->phase = PD_RUN;
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
