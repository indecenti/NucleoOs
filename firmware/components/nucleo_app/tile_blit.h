// tile_blit.h — shared 8bpp RGB332 colour-key tile blitter for tile-based native games (Orde,
// Cardler, ...). One place to read/optimise instead of a copy per app.
//
// Atlas format: 16x16 tiles packed 256 B each (1 B/px, RGB332), magenta key 0xE3 — matches the
// device canvas's own 8bpp depth, so blit_op is a straight pushImage memcpy (no per-pixel 565->332
// convert every frame). Built by assets/cardler/build_atlas.mjs and
// assets/coop-rpg/repack_orde_atlas8.mjs. Callers keep their own `s_tiles` (heap-on-enter, freed on
// exit — no .bss) and pass it in; this header holds no state.
#pragma once
#include "app_gfx.h"   // 'd' = canvas
#include <stdint.h>

#define TILE_TKEY 0xE3

// opaque 16x16 tile, no transparency scan (straight 8bpp memcpy via pushImage)
static inline void tile_blit_op(uint8_t (*tiles)[256], int idx, int x, int y)
{
    d.pushImage(x, y, 16, 16, tiles[idx]);
}

// transparent 16x16 tile, per-pixel colour-key (pushImage's cross-depth key would halo on this
// canvas, so opaque pixels are drawn by hand). fl: bit0 = H mirror, bit1 = V mirror.
static inline void tile_blit_key(uint8_t (*tiles)[256], int idx, int x, int y, int fl = 0)
{
    const uint8_t *src = tiles[idx];
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++) {
            int rx = (fl & 1) ? 15 - sx : sx, ry = (fl & 2) ? 15 - sy : sy;
            uint8_t px = src[ry * 16 + rx];
            if (px != (uint8_t)TILE_TKEY) d.drawPixel(x + sx, y + sy, px);
        }
}

// scaled colour-key blit: 16x16 source -> D x D centred at (cx,cy), nearest sampling, H-mirrored if
// f. Any D (not just integer multiples), so entities can be sized freely.
static inline void tile_blit_sz(uint8_t (*tiles)[256], int idx, int f, int cx, int cy, int D)
{
    const uint8_t *src = tiles[idx];
    int ox = cx - D / 2, oy = cy - D / 2;
    for (int sy = 0; sy < 16; sy++) {
        int y0 = sy * D / 16, y1 = (sy + 1) * D / 16;
        for (int sx = 0; sx < 16; sx++) {
            uint8_t px = src[sy * 16 + sx];
            if (px == (uint8_t)TILE_TKEY) continue;      // transparent: never touch it (no halo)
            int dxs = f ? (15 - sx) : sx;
            int x0 = dxs * D / 16, x1 = (dxs + 1) * D / 16;
            d.fillRect(ox + x0, oy + y0, x1 - x0, y1 - y0, px);
        }
    }
}
