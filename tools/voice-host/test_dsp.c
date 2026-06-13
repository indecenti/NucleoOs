// Host verification for nucleo_voice_dsp — the real firmware DSP core, compiled
// on the PC. Validates the MATH end-to-end (FFT, mel, MFCC, CMN, DTW, rate
// normalization, EMA consolidation, and synthetic-word discrimination).
//
// "Words" are sequences of phoneme-like tone complexes, so the spectrum changes
// over time the way speech does — a stationary tone would (correctly) be erased
// by CMN. What this CANNOT prove: real-voice recognition rate. That proof is the
// on-device test with your own voice.
#include "../../firmware/components/nucleo_voice/nucleo_voice_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// A phoneme = a set of simultaneous formant-like tones.
typedef struct { const float *f; int nf; } phon;

// A "word" = nseg phonemes played in sequence (a spectral trajectory).
static int gen_word(int16_t *buf, int nsamp, const phon *seq, int nseg,
                    float amp, float noise)
{
    for (int n = 0; n < nsamp; n++) {
        float t = (float)n * nseg / nsamp;
        int seg = (int)t;
        if (seg >= nseg) seg = nseg - 1;
        const phon *p = &seq[seg];
        float s = 0.0f;
        for (int k = 0; k < p->nf; k++)
            s += sinf(2.0f * (float)M_PI * p->f[k] * n / VDSP_RATE_HZ);
        s /= p->nf;
        float env = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (nsamp - 1));
        float v = amp * s * env;
        if (noise > 0.0f) v += ((rand() % 2001) - 1000) / 1000.0f * noise;
        if (v >  32000.0f) v =  32000.0f;
        if (v < -32000.0f) v = -32000.0f;
        buf[n] = (int16_t)v;
    }
    return nsamp;
}

static int extract(vdsp_ctx *c, const int16_t *pcm, int n, vdsp_template out)
{
    vdsp_acc *a = vdsp_acc_create(c);
    vdsp_acc_reset(a);
    int off = 0;
    while (off < n) {            // odd chunk sizes exercise the streaming framer
        int m = (n - off) < 313 ? (n - off) : 313;
        vdsp_acc_push(a, pcm + off, m);
        off += m;
    }
    int r = vdsp_acc_finalize(a, out);
    vdsp_acc_free(a);
    return r;
}

int main(void)
{
    srand(12345);
    vdsp_ctx *c = vdsp_ctx_create();
    if (!c) { printf("ctx alloc failed\n"); return 2; }

    // ── T1: FFT — a pure bin-64 sinusoid peaks at bin 64 ─────────────────────
    {
        float re[VDSP_FFT_SIZE], im[VDSP_FFT_SIZE];
        for (int i = 0; i < VDSP_FFT_SIZE; i++) {
            re[i] = sinf(2.0f * (float)M_PI * 64 * i / VDSP_FFT_SIZE);
            im[i] = 0.0f;
        }
        vdsp_fft(re, im, VDSP_FFT_SIZE);
        int peak = 0; float best = -1;
        for (int k = 1; k < VDSP_FFT_SIZE / 2; k++) {
            float mag = re[k]*re[k] + im[k]*im[k];
            if (mag > best) { best = mag; peak = k; }
        }
        CHECK(peak == 64, "T1 FFT peak bin = %d (want 64)", peak);
    }

    // ── T2: a 1000 Hz tone lights up the mel filter near 1000 Hz ─────────────
    {
        int16_t fr[VDSP_FRAME_LEN];
        for (int i = 0; i < VDSP_FRAME_LEN; i++)
            fr[i] = (int16_t)(12000 * sinf(2.0f * (float)M_PI * 1000.0f * i / VDSP_RATE_HZ));
        float lm[VDSP_NMEL];
        vdsp_logmel_frame(c, fr, lm);
        int peak = 0; float best = -1e30f;
        for (int m = 0; m < VDSP_NMEL; m++) if (lm[m] > best) { best = lm[m]; peak = m; }
        CHECK(peak >= 5 && peak <= 14, "T2 mel peak filter = %d (want ~6-13 for 1kHz)", peak);
    }

    // Phoneme inventory and two distinct word trajectories.
    static const float p1[] = {300, 870, 2500}, p2[] = {600, 1200, 2900};
    static const float p3[] = {450, 1700, 3300}, p4[] = {350, 1000, 2200};
    phon wordA[] = { {p1,3}, {p2,3}, {p3,3} };   // A: p1 -> p2 -> p3
    phon wordB[] = { {p3,3}, {p4,3}, {p2,3} };   // B: p3 -> p4 -> p2 (different)

    int16_t buf[VDSP_RATE_HZ];
    vdsp_template A, A2, Aq, As, B;

    CHECK(extract(c, buf, gen_word(buf, 9600, wordA, 3, 9000, 0), A) > 0, "T3 extract A");

    // ── T3: self-distance is zero ────────────────────────────────────────────
    CHECK(vdsp_dtw(A, A) == 0, "T3 dtw(A,A) = %ld (want 0)", (long)vdsp_dtw(A, A));

    extract(c, buf, gen_word(buf, 9600, wordB, 3, 9000, 0),   B);    // different word
    extract(c, buf, gen_word(buf, 9600, wordA, 3, 9000, 500), A2);   // A + noise
    extract(c, buf, gen_word(buf, 9600, wordA, 3, 3000, 0),   Aq);   // A at 1/3 gain
    extract(c, buf, gen_word(buf, 14400, wordA, 3, 9000, 0),  As);   // A spoken slower (0.9 s)

    int32_t dAB = vdsp_dtw(A, B);
    printf("  [info] dtw A-B(inter)=%ld  A-noisy=%ld  A-quiet=%ld  A-slow=%ld\n",
           (long)dAB, (long)vdsp_dtw(A, A2), (long)vdsp_dtw(A, Aq), (long)vdsp_dtw(A, As));

    // ── T4: noise robustness — noisy A closer to A than B is ─────────────────
    CHECK(vdsp_dtw(A, A2) < dAB, "T4 intra(A,noisy)=%ld must be < inter(A,B)=%ld",
          (long)vdsp_dtw(A, A2), (long)dAB);
    // ── T5: CMN/gain robustness — A at 1/3 amplitude still an intra-match ────
    CHECK(vdsp_dtw(A, Aq) < dAB, "T5 intra(A,quiet)=%ld must be < inter(A,B)=%ld",
          (long)vdsp_dtw(A, Aq), (long)dAB);
    // ── T6: rate invariance — A spoken slower still an intra-match ───────────
    CHECK(vdsp_dtw(A, As) < dAB, "T6 intra(A,slow)=%ld must be < inter(A,B)=%ld",
          (long)vdsp_dtw(A, As), (long)dAB);

    // ── T7: EMA consolidation pulls the template toward a new observation ────
    {
        vdsp_template T; memcpy(T, A, sizeof(T));
        int32_t before = vdsp_dtw(T, B);
        vdsp_consolidate(T, B, 80);          // blend ~31% of B
        int32_t after = vdsp_dtw(T, B);
        CHECK(after < before, "T7 consolidate: dist %ld -> %ld (must decrease)",
              (long)before, (long)after);
        CHECK(after > 0, "T7 consolidate must be partial, not a full overwrite");
    }

    // ── T8: too-short input is rejected ──────────────────────────────────────
    {
        vdsp_template S;
        CHECK(extract(c, buf, 200, S) == -1, "T8 short burst must reject");
    }

    // ── T9: margin gate — a known (noisy) word is classified more decisively
    //        than an unknown one (store = {A, B}). ─────────────────────────────
    {
        static const float p5[] = {520, 1400, 3000};
        phon wordD[] = { {p2,3}, {p5,3}, {p4,3} };       // out-of-vocabulary word
        vdsp_template D;
        extract(c, buf, gen_word(buf, 9600, wordD, 3, 9000, 0), D);

        int32_t kA = vdsp_dtw(A2, A), kB = vdsp_dtw(A2, B);
        int32_t kbest = kA < kB ? kA : kB, ksec = kA < kB ? kB : kA;
        int32_t dA = vdsp_dtw(D, A), dB = vdsp_dtw(D, B);
        int32_t dbest = dA < dB ? dA : dB, dsec = dA < dB ? dB : dA;
        int rk = (int)((int64_t)kbest * 100 / (ksec ? ksec : 1));
        int ru = (int)((int64_t)dbest * 100 / (dsec ? dsec : 1));
        printf("  [info] margin ratio: known=%d%%  unknown=%d%%\n", rk, ru);
        CHECK(rk < ru, "T9 known must be more decisive than unknown (%d%% vs %d%%)", rk, ru);
        CHECK((int64_t)kbest * 4 <= (int64_t)ksec * 3,
              "T9 known passes margin gate (best=%ld second=%ld)", (long)kbest, (long)ksec);
    }

    // ── T10: self-calibrating radius tightens toward observed distances + clamps ─
    {
        int32_t r = VDSP_RADIUS_INIT;
        for (int i = 0; i < 12; i++) r = vdsp_radius_update(r, 2000);
        printf("  [info] radius after 12x obs=2000: %ld\n", (long)r);
        CHECK(r > 2000 && r < 3500, "T10 radius converges near 1.25x obs (got %ld)", (long)r);
        CHECK(vdsp_radius_update(100, 100) >= VDSP_RADIUS_MIN, "T10 radius clamps to MIN");
        CHECK(vdsp_radius_update(9999, 999999) <= VDSP_RADIUS_MAX, "T10 radius clamps to MAX");
    }

    // ── T11: mel filterbank invariant — in the deployed geometry (26 mel bands over
    //        257 FFT bins) NO filter spans more than VDSP_MELW_MAX bins, so the width
    //        clamp at ctx-build time never silently drops spectral energy. Locks it. ──
    {
        int maxlen = 0;
        for (int m = 0; m < VDSP_NMEL; m++) {
            int len = vdsp_ctx_mel_len(c, m);
            if (len > maxlen) maxlen = len;
        }
        printf("  [info] widest mel filter = %d bins (cap %d)\n", maxlen, VDSP_MELW_MAX);
        CHECK(maxlen < VDSP_MELW_MAX, "T11 no mel filter is clamped (widest=%d, cap=%d)", maxlen, VDSP_MELW_MAX);
    }

    // ── T12: resample at the SHORT boundary — a word that yields exactly the minimum
    //        frame count (R≈VDSP_MIN_FRAMES) must still extract and self-match (heavy
    //        4→64 upsample, the R==1 / hi-clamp branches in vdsp_acc_finalize). ───────
    {
        vdsp_template S;
        int r = extract(c, buf, gen_word(buf, 1000, wordA, 3, 9000, 0), S);   // ~4 raw frames
        CHECK(r >= VDSP_MIN_FRAMES, "T12 short word extracts (R=%d, min=%d)", r, VDSP_MIN_FRAMES);
        CHECK(vdsp_dtw(S, S) == 0, "T12 short self-distance = %ld (want 0)", (long)vdsp_dtw(S, S));
    }

    // ── T13: resample at the LONG boundary — a >2 s word saturates VDSP_MAX_FRAMES and
    //        downsamples (~200→64). Exercises the frame cap + the hi-clamp tail. ──────
    {
        int n = 33000;                                  // > 400 + 200*160 → hits the MAX_FRAMES cap
        int16_t *big = (int16_t *)malloc((size_t)n * sizeof(int16_t));
        CHECK(big != NULL, "T13 alloc");
        if (big) {
            vdsp_template Lg;
            int r = extract(c, big, gen_word(big, n, wordA, 3, 9000, 0), Lg);
            CHECK(r == VDSP_MAX_FRAMES, "T13 long word saturates frame cap (R=%d, cap=%d)", r, VDSP_MAX_FRAMES);
            CHECK(vdsp_dtw(Lg, Lg) == 0, "T13 long self-distance = %ld (want 0)", (long)vdsp_dtw(Lg, Lg));
            free(big);
        }
    }

    // ── T14: delta (velocity) features are non-trivial AND don't saturate. Zeroing the
    //        delta half of A must change the DTW distance (proves Δ contributes), and no
    //        stored delta hits the int16 rail (no quantization overflow). ─────────────
    {
        vdsp_template Az; memcpy(Az, A, sizeof(Az));
        for (int i = 0; i < VDSP_CANON_LEN; i++)
            for (int k = VDSP_NMFCC; k < VDSP_NFEAT; k++) Az[i][k] = 0;
        CHECK(vdsp_dtw(A, Az) > 0, "T14 delta features contribute to DTW (dist=%ld)", (long)vdsp_dtw(A, Az));
        int saturated = 0, dmax = 0;
        for (int i = 0; i < VDSP_CANON_LEN; i++)
            for (int k = VDSP_NMFCC; k < VDSP_NFEAT; k++) {
                int v = A[i][k] < 0 ? -A[i][k] : A[i][k];
                if (v > dmax) dmax = v;
                if (A[i][k] == 32767 || A[i][k] == -32768) saturated = 1;
            }
        printf("  [info] max |delta| = %d\n", dmax);
        CHECK(!saturated, "T14 no delta saturates the int16 rail");
    }

    // ── T15: banded DTW actually enforces its band. A copy of A time-shifted by 12
    //        frames (> DTW_BAND=8) cannot be aligned and must cost MORE than one shifted
    //        by 4 frames (within the band). Validates the band, not just the diagonal. ─
    {
        const int L = VDSP_CANON_LEN;
        vdsp_template sh4, sh12;
        for (int i = 0; i < L; i++) {
            int j4 = i - 4 >= 0 ? i - 4 : 0;
            int j12 = i - 12 >= 0 ? i - 12 : 0;
            for (int k = 0; k < VDSP_NFEAT; k++) { sh4[i][k] = A[j4][k]; sh12[i][k] = A[j12][k]; }
        }
        int32_t d4 = vdsp_dtw(A, sh4), d12 = vdsp_dtw(A, sh12);
        printf("  [info] band shift: +4=%ld  +12=%ld\n", (long)d4, (long)d12);
        CHECK(d12 > d4, "T15 out-of-band shift (+12=%ld) must cost more than in-band (+4=%ld)", (long)d12, (long)d4);
    }

    vdsp_ctx_free(c);
    printf("\n%s  —  %d passed, %d failed\n", g_fail ? "RESULT: FAIL" : "RESULT: OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
