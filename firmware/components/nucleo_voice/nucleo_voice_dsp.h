// nucleo_voice_dsp — portable speaker-dependent keyword DSP core.
//
// Pure C, depends ONLY on <math.h> — compiles byte-for-byte the same in the
// ESP-IDF firmware and in the PC host harness (tools/voice-host). No esp-dsp,
// no platform headers. The 2 s response budget makes a plain radix-2 FFT in C
// fast enough, so there is a single code path to verify.
//
// Pipeline (per spoken word / "burst"):
//   pre-emphasis -> 25ms/10ms framing -> Hann -> FFT(512) -> mel(26) -> log
//   -> DCT -> 13 MFCC -> CMN (per-utterance cepstral mean removal, channel
//   robustness) -> resample to a fixed canonical length (rate normalization)
//   -> int16 quantize. Match = banded DTW. Templates self-consolidate (EMA).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDSP_RATE_HZ     16000
#define VDSP_FRAME_LEN   400      // 25 ms window
#define VDSP_FRAME_HOP   160      // 10 ms hop
#define VDSP_FFT_SIZE    512      // next pow2 >= FRAME_LEN
#define VDSP_NBINS       (VDSP_FFT_SIZE/2 + 1)  // 257 usable spectrum bins
#define VDSP_NMEL        26       // mel filterbank size
#define VDSP_NMFCC       13       // static cepstral coefficients computed per frame (c0..c12)
#define VDSP_NFEAT       (VDSP_NMFCC * 2)  // stored feature/frame: static MFCC + delta (velocity)
#define VDSP_CANON_LEN   64       // fixed frames per stored template (~640 ms canonical)
#define VDSP_MAX_FRAMES  200      // cap raw frames per burst (~2 s)
#define VDSP_MIN_FRAMES  4        // shorter than this = rejected (silence/click)
#define VDSP_MAX_PUSH    512      // max samples per internal framing step
#define VDSP_MELW_MAX    64       // max FFT bins spanned by one mel filter
#define VDSP_Q           64       // int16 quantization scale for MFCC

// One stored/observed word: canonical-length feature matrix, int16-quantized.
// Each frame = [ static MFCC (0..NMFCC-1) | delta MFCC (NMFCC..NFEAT-1) ].
typedef int16_t vdsp_template[VDSP_CANON_LEN][VDSP_NFEAT];

// ── Precomputed tables (cache): build once, reuse for every PTT. ~7 KB heap. ──
typedef struct vdsp_ctx vdsp_ctx;
vdsp_ctx *vdsp_ctx_create(void);          // NULL on OOM
void      vdsp_ctx_free(vdsp_ctx *c);

// ── Streaming burst accumulator: ~20 KB heap, alloc on PTT press, freed on
//    release. Feed PCM as it arrives; framing/MFCC happen incrementally so the
//    raw audio is never held — only the compact MFCC frames are. ─────────────
typedef struct vdsp_acc vdsp_acc;
vdsp_acc *vdsp_acc_create(vdsp_ctx *c);   // NULL on OOM
void      vdsp_acc_free(vdsp_acc *a);
void      vdsp_acc_reset(vdsp_acc *a);    // begin a new burst
void      vdsp_acc_push(vdsp_acc *a, const int16_t *pcm, int n);  // feed samples
// Close the burst: apply CMN, resample to canonical length, quantize into `out`.
// Returns raw frame count used (>= VDSP_MIN_FRAMES), or -1 if too short.
int       vdsp_acc_finalize(vdsp_acc *a, vdsp_template out);

// ── Matching & adaptation ────────────────────────────────────────────────────
// Banded DTW. Returns a per-frame-averaged L1 distance in quantized MFCC units
// (0 = identical). Lower is more similar. Compare against a reject threshold.
int32_t   vdsp_dtw(const vdsp_template a, const vdsp_template b);

// EMA-blend an accepted observation into its matched template (frame-aligned;
// both are canonical length). alpha_q in [0,256]; ~38 (~0.15) is a gentle pull.
// This is what makes a template sharpen toward the centroid of your pronunciations.
void      vdsp_consolidate(vdsp_template tpl, const vdsp_template obs, int alpha_q);

// Self-calibrating per-template accept radius. Each accepted in-radius match
// nudges the radius (EMA) toward the observed distance + headroom, so a word
// learns its own difficulty from your real repetitions — no hand-tuned absolute
// threshold. Returns the new radius, clamped to [VDSP_RADIUS_MIN, VDSP_RADIUS_MAX].
#define VDSP_RADIUS_MIN   1500
#define VDSP_RADIUS_MAX   9000
#define VDSP_RADIUS_INIT  6000    // loose starting radius for a freshly-trained word
int32_t   vdsp_radius_update(int32_t radius, int32_t obs_dist);

// ── Test-only hooks (used by tools/voice-host) ───────────────────────────────
void      vdsp_fft(float *re, float *im, int n);                       // in-place radix-2
void      vdsp_logmel_frame(vdsp_ctx *c, const int16_t *pcm400, float out[VDSP_NMEL]);
int       vdsp_ctx_mel_len(const vdsp_ctx *c, int m);  // stored (clamped) FFT-bin span of mel filter m

#ifdef __cplusplus
}
#endif
