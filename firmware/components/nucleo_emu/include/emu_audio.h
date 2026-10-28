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
