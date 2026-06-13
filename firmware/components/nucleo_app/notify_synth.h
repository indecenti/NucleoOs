// Polyphonic notification chimes, synthesized at runtime to a mono WAV on the SD card.
//
// The Cardputer speaker is a single mono I2S stream — no real-time polyphony. The trick:
// pre-synthesize the chord ADDITIVELY (sum of voices that ring together) into one WAV, then
// play it as a normal file. Real polyphony, paid for ONCE, in CPU, with ~zero RAM: samples are
// computed on the fly and streamed straight to disk — there is no PCM buffer, only ~4 voices of
// floats on the stack. (CPU is cheap on the S3; RAM/SD is the scarce resource — see the project's
// RAM-budget notes.) The file is cached on SD, so the synth runs at most once per level, ever.
//
// Pure C: only stdio + math, no ESP/FreeRTOS deps, so the host harness compiles and exercises it.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum {
    NOTIFY_SND_INFO = 0, NOTIFY_SND_SUCCESS, NOTIFY_SND_WARN, NOTIFY_SND_CRITICAL,
    NOTIFY_SND_COUNT
} notify_snd_t;

// Source identity: each gets a short, recognizable "signature" flourish that plays BEFORE the level
// chord, so the same sound tells you WHO (source) and HOW URGENT (level) without looking. A learnable
// auditory language — the earcon.
typedef enum {
    NOTIFY_SRC_SYSTEM = 0, NOTIFY_SRC_CALENDAR, NOTIFY_SRC_ANIMA, NOTIFY_SRC_VOICE,
    NOTIFY_SRC_RECORDER, NOTIFY_SRC_OTA, NOTIFY_SRC_APP,
    NOTIFY_SRC_COUNT
} notify_src_t;

// The level chord starts this long after the signature begins (s) — a brief overlap reads as one
// musical phrase, not two beeps.
#define NOTIFY_SIG_LEN 0.16f

// One ringing voice: frequency, onset (s), duration (s), relative amplitude.
typedef struct { float hz, t0, dur, amp; } notify_voice_t;

// A consonant, pretty motif per level — voices overlap (true polyphony) with a small onset
// stagger for a gentle "roll". Designed to be audible but never harsh: soft attack, exponential
// release, plus an octave overtone for a music-box/glockenspiel timbre. Returns the voice count.
static inline void notify__voice(notify_voice_t *v, float hz, float t0, float dur)
{ v->hz = hz; v->t0 = t0; v->dur = dur; v->amp = 1.0f; }

static inline int notify_chord(notify_snd_t lvl, notify_voice_t *v, int max)
{
    notify_voice_t s[4]; int n = 0;
    switch (lvl) {
    case NOTIFY_SND_SUCCESS:   // DO maggiore ascendente — "fatto!"
        notify__voice(&s[0], 523.25f, 0.00f, 0.55f);
        notify__voice(&s[1], 659.25f, 0.06f, 0.55f);
        notify__voice(&s[2], 783.99f, 0.12f, 0.60f); n = 3; break;
    case NOTIFY_SND_WARN:      // RE5 + LA5 — più attento, ancora morbido
        notify__voice(&s[0], 587.33f, 0.00f, 0.52f);
        notify__voice(&s[1], 880.00f, 0.05f, 0.52f); n = 2; break;
    case NOTIFY_SND_CRITICAL:  // motivo a 3 note insistente ma musicale (mai buzzer)
        notify__voice(&s[0], 659.25f, 0.00f, 0.42f);
        notify__voice(&s[1], 659.25f, 0.18f, 0.42f);
        notify__voice(&s[2], 987.77f, 0.32f, 0.55f); n = 3; break;
    case NOTIFY_SND_INFO:
    default:                   // quinta morbida MI5 + SI5
        notify__voice(&s[0], 659.25f, 0.00f, 0.50f);
        notify__voice(&s[1], 987.77f, 0.04f, 0.50f); n = 2; break;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) v[i] = s[i];
    return n;
}

// The per-source signature: 1-2 short notes that identify WHO is calling. Kept brief and a touch
// quieter than the level chord so the body (urgency) still leads. Voices start at t0 in [0, ~0.13).
static inline int notify_signature(notify_src_t src, notify_voice_t *v, int max)
{
    notify_voice_t s[2]; int n = 0;
    switch (src) {
    case NOTIFY_SRC_ANIMA:      // sparkle: due grazie acute, brillante — identità "AI"
        notify__voice(&s[0], 2093.00f, 0.00f, 0.10f);    // DO7
        notify__voice(&s[1], 2637.02f, 0.05f, 0.12f);    // MI7
        n = 2; break;
    case NOTIFY_SRC_CALENDAR:   // rintocco caldo singolo — promemoria
        notify__voice(&s[0], 783.99f, 0.00f, 0.16f);     // SOL5
        n = 1; break;
    case NOTIFY_SRC_OTA:        // coppia ascendente — aggiornamento in salita
        notify__voice(&s[0], 523.25f, 0.00f, 0.10f);     // DO5
        notify__voice(&s[1], 783.99f, 0.07f, 0.12f);     // SOL5
        n = 2; break;
    case NOTIFY_SRC_RECORDER:   // coppia discendente — "nastro"
        notify__voice(&s[0], 880.00f, 0.00f, 0.10f);     // LA5
        notify__voice(&s[1], 698.46f, 0.07f, 0.12f);     // FA5
        n = 2; break;
    case NOTIFY_SRC_VOICE:      // singolo medio caldo
        notify__voice(&s[0], 659.25f, 0.00f, 0.14f);     // MI5
        n = 1; break;
    case NOTIFY_SRC_APP:        // singolo discreto
        notify__voice(&s[0], 587.33f, 0.00f, 0.12f);     // RE5
        n = 1; break;
    case NOTIFY_SRC_SYSTEM:
    default:                    // neutro basso singolo
        notify__voice(&s[0], 523.25f, 0.00f, 0.13f);     // DO5
        n = 1; break;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { v[i] = s[i]; v[i].amp = 0.9f; }
    return n;
}

static inline void notify__put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void notify__put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

// Chime content tops out ~2 kHz (octave of the highest note), so 12 kHz (Nyquist 6 kHz) is plenty
// and ~25% lighter than 16 kHz on SD bytes, synth CPU, and I2S read bandwidth.
#define NOTIFY_SND_RATE 12000

// One sample's RAW (un-normalized) value at index `i` — shared by both synthesis passes so the
// math lives in exactly one place.
static inline float notify__sample(const notify_voice_t *v, int nv, int rate, int i)
{
    float t = (float)i / rate, s = 0.0f;
    for (int j = 0; j < nv; j++) {
        float lt = t - v[j].t0;
        if (lt < 0.0f || lt > v[j].dur) continue;
        const float atk = 0.012f;                                    // ~12 ms soft attack
        float env = lt < atk ? lt / atk : expf(-(lt - atk) * 4.5f);  // exponential release
        float ph = 2.0f * (float)M_PI * v[j].hz * lt;
        float tone = sinf(ph) + 0.30f * sinf(2.0f * ph);             // + octave overtone = carillon
        s += v[j].amp * env * tone;
    }
    return s;
}

// Stream a set of overlapping VOICES to a normalized mono 16-bit PCM WAV at `rate` Hz (<=0 ->
// NOTIFY_SND_RATE). TWO PASSES, ZERO BUFFER: pass 1 finds the signal peak, pass 2 writes it normalized
// to a fixed target loudness — the sound lands at the same audible level regardless of voice overlap and
// never clips. Streamed straight to disk; no PCM buffer is ever held (CPU spent to spare RAM, the
// device's trade). Shared by the notification earcons AND any app wanting a custom polyphonic cue (e.g.
// the recorder's GO start/stop tones). Returns 0 on success, non-zero on a file error.
static inline int notify_synth_voices_wav(const notify_voice_t *v, int nv, const char *path, int rate)
{
    if (rate <= 0) rate = NOTIFY_SND_RATE;
    float end = 0.0f;
    for (int i = 0; i < nv; i++) { float e = v[i].t0 + v[i].dur; if (e > end) end = e; }
    int total = (int)(end * rate) + 1;

    // Pass 1: peak of the raw signal (recomputed in pass 2 — no array, RAM stays flat).
    float peak = 0.0f;
    for (int i = 0; i < total; i++) { float a = notify__sample(v, nv, rate, i); if (a < 0.0f) a = -a; if (a > peak) peak = a; }
    float norm = peak > 1e-6f ? 0.82f / peak : 0.0f;                 // target ~82% FS: audible, with headroom

    uint32_t data_len = (uint32_t)total * 2;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint8_t h[44];
    memcpy(h, "RIFF", 4);       notify__put32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);   memcpy(h + 12, "fmt ", 4);
    notify__put32(h + 16, 16);  notify__put16(h + 20, 1);            // PCM
    notify__put16(h + 22, 1);   notify__put32(h + 24, (uint32_t)rate);
    notify__put32(h + 28, (uint32_t)rate * 2);                       // byte rate (mono 16-bit)
    notify__put16(h + 32, 2);   notify__put16(h + 34, 16);
    memcpy(h + 36, "data", 4);  notify__put32(h + 40, data_len);
    fwrite(h, 1, 44, f);

    // Pass 2: stream normalized samples.
    for (int i = 0; i < total; i++) {
        float s = notify__sample(v, nv, rate, i) * norm;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        int16_t pcm = (int16_t)(s * 32767.0f);
        uint8_t b[2]; notify__put16(b, (uint16_t)pcm);
        fwrite(b, 1, 2, f);
    }
    fclose(f);
    return 0;
}

// Synthesize the earcon for (source, level): the source SIGNATURE (who) followed by the level CHORD (how
// urgent), fused into one short polyphonic sound. Cached on SD, so it runs at most once per (src,lvl).
// Returns 0 on success, non-zero on a file error.
static inline int notify_synth_wav(notify_src_t src, notify_snd_t lvl, const char *path, int rate)
{
    notify_voice_t v[6];
    int nv = notify_signature(src, v, 6);             // who (signature, at t0~0)
    notify_voice_t c[4];
    int nc = notify_chord(lvl, c, 4);                 // how urgent (level chord, shifted after it)
    for (int i = 0; i < nc && nv < 6; i++) { c[i].t0 += NOTIFY_SIG_LEN; v[nv++] = c[i]; }
    return notify_synth_voices_wav(v, nv, path, rate);
}
