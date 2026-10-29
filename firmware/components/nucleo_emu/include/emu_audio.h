// emu_audio — the last stage of every emulator's sound, between the console's chip and the speaker.
//
// The chips are emulated as the hardware is: raw square waves, the Game Boy's DAC with its DC offset. A
// console smoothed that with its own output capacitor and a speaker the size of a coin; this board's speaker
// is just as small but sits right after a clean DAC, so the raw signal reaches it as clicks (the offset jumping
// when a channel starts, pauses or the menu opens) and as a hard, fizzy top (the squares' aliasing above
// ~8 kHz, which the tiny cone turns into distortion). Three steps per sample, integer only, on the samples the
// core has just produced:
//   1. DC block   — one-pole high-pass at ~20 Hz: the picture of a console's output capacitor
//   2. low-pass   — one-pole at ~8 kHz (32 kHz streams): the fizz goes, the tune stays
//   3. soft knee  — above ~73 % of full scale the excess is quartered, so a loud chord rounds off instead of
//                   clipping flat
// State is a few words per channel, kept in the emulator's session (not in a save state: it is not the console).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int32_t x1, y1;     // DC block: previous input / output
    int32_t lp;         // low-pass state
} emu_afilt_t;

#define EMU_AF_DC_R  32643      // pole of the DC block, Q15 (1 - 2*pi*20/32768)
#define EMU_AF_LP_A  200        // low-pass step, /256: -3 dB near 8 kHz at 32 kHz
#define EMU_AF_KNEE  24000

static inline int16_t emu_afilt_one(emu_afilt_t *f, int32_t x, int dc)
{
    int32_t y = x;
    if (dc) {
        y = x - f->x1 + ((f->y1 * EMU_AF_DC_R) >> 15);
        f->x1 = x; f->y1 = y;
    }
    f->lp += ((y - f->lp) * EMU_AF_LP_A) >> 8;
    y = f->lp;
    if (y > EMU_AF_KNEE)       y = EMU_AF_KNEE + ((y - EMU_AF_KNEE) >> 2);
    else if (y < -EMU_AF_KNEE) y = -EMU_AF_KNEE + ((y + EMU_AF_KNEE) >> 2);
    return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}

// `n` samples, `ch` channels interleaved (one filter state per channel); dc = 0 when the chip's own output
// stage already removes the offset (the Game Gear's PSG does).
static inline void emu_afilt_run(emu_afilt_t *f, int16_t *s, size_t n, int ch, int dc)
{
    for (size_t i = 0; i < n; i++) s[i] = emu_afilt_one(&f[i % (size_t)ch], s[i], dc);
}

// How much sound the speaker queues ahead of the game: three frames (~50 ms). One frame late — a busy scene, an
// SD read — then plays on instead of dropping to silence (a click), at the cost of 50 ms of latency.
#define EMU_AUDIO_QUEUE_FRAMES 3

// The speaker's queue, as the emulator sees it. The I2S DMA runs a ring of 8 buffers that never stops: what the
// emulator has not filled it plays as zeros (auto_clear), and those zeros take their place in the ring like sound.
// So the level is not "written since the first write minus elapsed time": at the open, and after any gap, the ring
// is full of zeros the next write waits behind. The anchor is the one moment the level is known: a write that had
// to WAIT returns with the ring full (its capacity, less half a buffer still draining). From an anchor on, the level
// is written minus played (elapsed time x rate; the I2S clock and the timer agree within ~1e-4), and below zero
// the ring ran out of sound: a gap (a click), counted.
typedef struct {
    int64_t  t0;        // the anchor: when the level was last known
    int64_t  last;      // the last write
    int64_t  written;   // samples in the ring at t0 plus those written since
    int32_t  rate, cap; // samples a second; the ring's size
    bool     anchored;
    uint32_t gaps;
} emu_aclock_t;

// At the open, with what was asked of nucleo_audio_pcm_open_ex (its ring: 8 buffers of 240..480 frames).
static inline void emu_aclock_open(emu_aclock_t *c, int rate, int queue_frames)
{
    int frame = (queue_frames + 7) / 8;
    if (frame < 240) frame = 240;
    if (frame > 480) frame = 480;
    c->t0 = c->last = c->written = 0;
    c->rate = rate; c->cap = 8 * frame; c->anchored = false; c->gaps = 0;
}

static inline int emu_aclock_level(const emu_aclock_t *c, int64_t now_us)
{
    if (!c->anchored) return c->cap;                    // not known yet: as full as it is at the open
    return (int)(c->written - (now_us - c->t0) * c->rate / 1000000);
}

// Before writing n samples. A write 100 ms after the last one follows a pause (the menu, a card step): the ring
// drained on purpose, so the account waits for the next anchor instead of counting it.
static inline void emu_aclock_write(emu_aclock_t *c, int64_t now_us, int n)
{
    if (now_us - c->last > 100000) c->anchored = false;
    c->last = now_us;
    if (c->anchored && emu_aclock_level(c, now_us) < 0) {            // the ring ran out: silence was played
        c->gaps++;
        c->anchored = false;                                          // (it is full of zeros again)
    }
    c->written += n;
}

// After the write: `waited_us` it blocked. A wait means the ring was full: anchor there.
static inline void emu_aclock_wrote(emu_aclock_t *c, int64_t now_us, int64_t waited_us)
{
    if (waited_us < 300) return;
    c->t0 = now_us; c->written = c->cap - 120; c->anchored = true;
}

// How many samples this frame should make: the queue held near `target` samples by its real level, not by a guess
// at how late the frame was. Below the target (a slow scene) the frame makes more, half the shortfall at a time,
// so the sound stretches instead of stopping; above it up to 1/16 fewer, so the ring never fills and the game never
// waits on the speaker. At the balance it makes exactly `nominal`: the console's clock and the speaker's agree. The
// chips render by sample count, so the change is in how long a frame's notes last, never in their pitch.
static inline int emu_aclock_size(const emu_aclock_t *c, int64_t now_us, int nominal, int target, int max)
{
    int n = nominal + (target - emu_aclock_level(c, now_us)) / 2;
    if (n < nominal - nominal / 16) n = nominal - nominal / 16;
    if (n > max) n = max;
    return n;
}
