// nucleo_voice_dsp — see header. Pure C / <math.h> only.
#include "nucleo_voice_dsp.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PREEMPH   0.97f
#define DTW_BAND  8
#define DTW_INF   0x3FFFFFFF
#define HOLD_CAP  (VDSP_FRAME_LEN + VDSP_MAX_PUSH)   // 912

// ── Precomputed tables ──────────────────────────────────────────────────────
struct vdsp_ctx {
    float hann[VDSP_FRAME_LEN];
    // Sparse mel filterbank: filter m covers FFT bins [start[m] .. start[m]+len[m]-1]
    int   mel_start[VDSP_NMEL];
    int   mel_len[VDSP_NMEL];
    float mel_w[VDSP_NMEL][VDSP_MELW_MAX];
    float dct[VDSP_NMFCC][VDSP_NMEL];
};

static inline float hz_to_mel(float f) { return 2595.0f * log10f(1.0f + f / 700.0f); }
static inline float mel_to_hz(float m) { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

vdsp_ctx *vdsp_ctx_create(void)
{
    vdsp_ctx *c = (vdsp_ctx *)calloc(1, sizeof(vdsp_ctx));
    if (!c) return NULL;

    for (int n = 0; n < VDSP_FRAME_LEN; n++)
        c->hann[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (VDSP_FRAME_LEN - 1));

    // Mel points: NMEL+2 edges from 0 Hz to Nyquist, equally spaced in mel.
    float mlo = hz_to_mel(0.0f), mhi = hz_to_mel(VDSP_RATE_HZ / 2.0f);
    int bin[VDSP_NMEL + 2];
    for (int i = 0; i < VDSP_NMEL + 2; i++) {
        float mel = mlo + (mhi - mlo) * i / (VDSP_NMEL + 1);
        float hz  = mel_to_hz(mel);
        int b = (int)floorf(hz * VDSP_FFT_SIZE / VDSP_RATE_HZ);
        if (b < 0) b = 0;
        if (b > VDSP_NBINS - 1) b = VDSP_NBINS - 1;
        bin[i] = b;
    }
    for (int m = 0; m < VDSP_NMEL; m++) {
        int lo = bin[m], mid = bin[m + 1], hi = bin[m + 2];
        if (mid <= lo) mid = lo + 1;
        if (hi <= mid) hi = mid + 1;
        if (hi > VDSP_NBINS - 1) hi = VDSP_NBINS - 1;
        int len = hi - lo + 1;
        if (len > VDSP_MELW_MAX) len = VDSP_MELW_MAX;
        c->mel_start[m] = lo;
        c->mel_len[m]   = len;
        for (int j = 0; j < len; j++) {
            int k = lo + j;
            float w;
            if (k <= mid) w = (mid > lo) ? (float)(k - lo) / (mid - lo) : 1.0f;
            else          w = (hi > mid) ? (float)(hi - k) / (hi - mid) : 0.0f;
            if (w < 0.0f) w = 0.0f;
            c->mel_w[m][j] = w;
        }
    }
    // DCT-II matrix (orthogonal-ish; absolute scale is irrelevant after CMN+threshold tuning)
    for (int i = 0; i < VDSP_NMFCC; i++)
        for (int m = 0; m < VDSP_NMEL; m++)
            c->dct[i][m] = cosf((float)M_PI * i * (m + 0.5f) / VDSP_NMEL);

    return c;
}

void vdsp_ctx_free(vdsp_ctx *c) { free(c); }

// ── In-place iterative radix-2 FFT ──────────────────────────────────────────
void vdsp_fft(float *re, float *im, int n)
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

// ── Streaming burst accumulator ─────────────────────────────────────────────
struct vdsp_acc {
    vdsp_ctx *ctx;
    float  hold[HOLD_CAP];     // pre-emphasized samples awaiting framing
    int    hold_n;
    float  raw_prev;           // last raw sample (pre-emphasis continuity)
    float  raw[VDSP_MAX_FRAMES][VDSP_NMFCC];  // raw MFCC frames (pre-CMN)
    float  canon[VDSP_CANON_LEN][VDSP_NMFCC]; // canonical static MFCC (for delta computation)
    int    nframes;
    // per-frame scratch
    float  re[VDSP_FFT_SIZE], im[VDSP_FFT_SIZE];
    float  pow[VDSP_NBINS];
    float  frame[VDSP_FRAME_LEN];
};

vdsp_acc *vdsp_acc_create(vdsp_ctx *c)
{
    if (!c) return NULL;
    vdsp_acc *a = (vdsp_acc *)calloc(1, sizeof(vdsp_acc));
    if (!a) return NULL;
    a->ctx = c;
    return a;
}

void vdsp_acc_free(vdsp_acc *a) { free(a); }

void vdsp_acc_reset(vdsp_acc *a)
{
    if (!a) return;
    a->hold_n = 0; a->raw_prev = 0.0f; a->nframes = 0;
}

// Process one 400-sample (pre-emphasized) window in `frame` -> one MFCC frame.
static void acc_emit(vdsp_acc *a)
{
    if (a->nframes >= VDSP_MAX_FRAMES) return;
    vdsp_ctx *c = a->ctx;

    for (int i = 0; i < VDSP_FRAME_LEN; i++) a->re[i] = a->frame[i] * c->hann[i];
    for (int i = VDSP_FRAME_LEN; i < VDSP_FFT_SIZE; i++) a->re[i] = 0.0f;
    memset(a->im, 0, sizeof(a->im));
    vdsp_fft(a->re, a->im, VDSP_FFT_SIZE);

    for (int k = 0; k < VDSP_NBINS; k++)
        a->pow[k] = a->re[k] * a->re[k] + a->im[k] * a->im[k];

    float mel[VDSP_NMEL], peak = 0.0f;
    for (int m = 0; m < VDSP_NMEL; m++) {
        float e = 0.0f;
        int s = c->mel_start[m], len = c->mel_len[m];
        for (int j = 0; j < len; j++) e += a->pow[s + j] * c->mel_w[m][j];
        mel[m] = e;
        if (e > peak) peak = e;
    }
    // Relative spectral floor: clamp quiet bands to peak*1e-3 so broadband noise
    // can't dominate the log of a near-silent band (60 dB dynamic-range cap).
    float floor_e = peak * 1e-3f + 1e-10f;
    float logmel[VDSP_NMEL];
    for (int m = 0; m < VDSP_NMEL; m++)
        logmel[m] = logf(mel[m] > floor_e ? mel[m] : floor_e);
    float *out = a->raw[a->nframes];
    for (int i = 0; i < VDSP_NMFCC; i++) {
        float s = 0.0f;
        for (int m = 0; m < VDSP_NMEL; m++) s += logmel[m] * c->dct[i][m];
        out[i] = s;
    }
    a->nframes++;
}

static void acc_push_small(vdsp_acc *a, const int16_t *s, int m)
{
    for (int i = 0; i < m; i++) {
        float x = (float)s[i];
        a->hold[a->hold_n++] = x - PREEMPH * a->raw_prev;
        a->raw_prev = x;
    }
    while (a->hold_n >= VDSP_FRAME_LEN) {
        memcpy(a->frame, a->hold, VDSP_FRAME_LEN * sizeof(float));
        acc_emit(a);
        int rem = a->hold_n - VDSP_FRAME_HOP;
        memmove(a->hold, a->hold + VDSP_FRAME_HOP, rem * sizeof(float));
        a->hold_n = rem;
    }
}

void vdsp_acc_push(vdsp_acc *a, const int16_t *pcm, int n)
{
    if (!a || !pcm) return;
    while (n > 0) {
        int m = n < VDSP_MAX_PUSH ? n : VDSP_MAX_PUSH;
        acc_push_small(a, pcm, m);
        pcm += m; n -= m;
    }
}

static inline int16_t qclamp(float v)
{
    long q = lroundf(v * VDSP_Q);
    if (q >  32767) q =  32767;
    if (q < -32768) q = -32768;
    return (int16_t)q;
}

int vdsp_acc_finalize(vdsp_acc *a, vdsp_template out)
{
    if (!a || a->nframes < VDSP_MIN_FRAMES) return -1;
    int R = a->nframes;

    // CMN — remove the per-utterance cepstral mean (kills the channel/mic offset).
    for (int k = 0; k < VDSP_NMFCC; k++) {
        float mean = 0.0f;
        for (int f = 0; f < R; f++) mean += a->raw[f][k];
        mean /= R;
        for (int f = 0; f < R; f++) a->raw[f][k] -= mean;
    }
    // Resample R frames -> fixed canonical length (removes speaking-rate variation).
    const int L = VDSP_CANON_LEN;
    for (int i = 0; i < L; i++) {
        float pos = (R == 1) ? 0.0f : (float)i * (R - 1) / (L - 1);
        int lo = (int)pos; float fr = pos - lo; int hi = lo + 1;
        if (hi >= R) hi = R - 1;
        for (int k = 0; k < VDSP_NMFCC; k++)
            a->canon[i][k] = a->raw[lo][k] * (1.0f - fr) + a->raw[hi][k] * fr;
    }
    // Per frame: store static MFCC + its time-derivative (delta) via a ±2-frame
    // regression. Deltas capture transitions (consonant→vowel) — strongly
    // discriminative — and are channel-invariant (the CMN constant cancels in a
    // difference). Edges clamp. Both halves quantized into the feature vector.
    for (int i = 0; i < L; i++) {
        int i2p = i + 2 < L ? i + 2 : L - 1, i1p = i + 1 < L ? i + 1 : L - 1;
        int i2m = i - 2 >= 0 ? i - 2 : 0,     i1m = i - 1 >= 0 ? i - 1 : 0;
        for (int k = 0; k < VDSP_NMFCC; k++) {
            out[i][k] = qclamp(a->canon[i][k]);
            float d = (2.0f * (a->canon[i2p][k] - a->canon[i2m][k])
                            + (a->canon[i1p][k] - a->canon[i1m][k])) / 10.0f;
            out[i][VDSP_NMFCC + k] = qclamp(d);
        }
    }
    return R;
}

// ── Banded DTW ──────────────────────────────────────────────────────────────
static inline int32_t min3(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a < b ? a : b; return m < c ? m : c;
}

int32_t vdsp_dtw(const vdsp_template A, const vdsp_template B)
{
    const int L = VDSP_CANON_LEN;
    int32_t row0[VDSP_CANON_LEN], row1[VDSP_CANON_LEN];
    for (int j = 0; j < L; j++) row0[j] = DTW_INF;
    for (int i = 0; i < L; i++) {
        for (int j = 0; j < L; j++) row1[j] = DTW_INF;
        int jlo = i - DTW_BAND; if (jlo < 0) jlo = 0;
        int jhi = i + DTW_BAND; if (jhi > L - 1) jhi = L - 1;
        for (int j = jlo; j <= jhi; j++) {
            int32_t d = 0;
            for (int k = 0; k < VDSP_NFEAT; k++) {
                int32_t diff = (int32_t)A[i][k] - (int32_t)B[j][k];
                d += diff < 0 ? -diff : diff;
            }
            int32_t best;
            if (i == 0 && j == 0)      best = 0;
            else if (i == 0)           best = (j > 0) ? row1[j - 1] : DTW_INF;
            else if (j == 0)           best = row0[0];
            else                       best = min3(row0[j], row1[j - 1], row0[j - 1]);
            if (best >= DTW_INF) continue;
            row1[j] = d + best;
        }
        memcpy(row0, row1, L * sizeof(int32_t));
    }
    int32_t cost = row0[L - 1];
    if (cost >= DTW_INF) return DTW_INF;
    return cost / L;   // per-frame-averaged L1 distance
}

void vdsp_consolidate(vdsp_template tpl, const vdsp_template obs, int alpha_q)
{
    if (alpha_q < 0) alpha_q = 0;
    if (alpha_q > 256) alpha_q = 256;
    int beta = 256 - alpha_q;
    for (int i = 0; i < VDSP_CANON_LEN; i++)
        for (int k = 0; k < VDSP_NFEAT; k++) {
            int32_t v = (int32_t)tpl[i][k] * beta + (int32_t)obs[i][k] * alpha_q + 128;
            tpl[i][k] = (int16_t)(v >> 8);
        }
}

int32_t vdsp_radius_update(int32_t radius, int32_t obs_dist)
{
    int32_t target = obs_dist * 5 / 4;             // 25% headroom over the observed match
    int32_t next   = radius + (target - radius) / 4;  // quarter-step EMA toward target
    if (next < VDSP_RADIUS_MIN) next = VDSP_RADIUS_MIN;
    if (next > VDSP_RADIUS_MAX) next = VDSP_RADIUS_MAX;
    return next;
}

// ── Test hook: stored bin span of mel filter m (used to assert the filterbank
//    never silently clamps to VDSP_MELW_MAX in the deployed config). ──────────
int vdsp_ctx_mel_len(const vdsp_ctx *c, int m)
{
    if (!c || m < 0 || m >= VDSP_NMEL) return 0;
    return c->mel_len[m];
}

// ── Test hook: log-mel of a single 400-sample window ────────────────────────
void vdsp_logmel_frame(vdsp_ctx *c, const int16_t *pcm400, float out[VDSP_NMEL])
{
    float re[VDSP_FFT_SIZE], im[VDSP_FFT_SIZE];
    float prev = 0.0f;
    for (int i = 0; i < VDSP_FRAME_LEN; i++) {
        float x = (float)pcm400[i];
        re[i] = (x - PREEMPH * prev) * c->hann[i];
        prev = x;
    }
    for (int i = VDSP_FRAME_LEN; i < VDSP_FFT_SIZE; i++) re[i] = 0.0f;
    memset(im, 0, sizeof(im));
    vdsp_fft(re, im, VDSP_FFT_SIZE);
    float peak = 0.0f;
    for (int m = 0; m < VDSP_NMEL; m++) {
        float e = 0.0f;
        int s = c->mel_start[m], len = c->mel_len[m];
        for (int j = 0; j < len; j++) {
            float p = re[s + j] * re[s + j] + im[s + j] * im[s + j];
            e += p * c->mel_w[m][j];
        }
        out[m] = e;
        if (e > peak) peak = e;
    }
    float floor_e = peak * 1e-3f + 1e-10f;
    for (int m = 0; m < VDSP_NMEL; m++)
        out[m] = logf(out[m] > floor_e ? out[m] : floor_e);
}
