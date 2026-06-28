// nucleo_fxscene.h — REUSABLE parallax backdrop primitives (generic, two looks).
//
// Two families, both pure-inline and HEAP-FREE, drawn through the shared `d`:
//   * filled noir   — sky()/skyline()/floor(): grey gradients, building silhouettes, Mode-7 belt.
//   * minimal line  — ridge()/props()/clean_floor(): white-paper aesthetic, OUTLINE strokes only,
//                     multi-layer parallax, optional gentle sway — no grid, lots of white space.
// The caller composes layers per level with its own parallax factors. Few colours by design — perfect
// for the no-PSRAM board and a high-contrast silhouette game.
#pragma once
#include "nucleo_fxfig.h"   // -> nucleo_fx3d.h (grid), vgrad, Belt

namespace fxscene {

// A full sky/wall gradient band.
static inline void sky(int x, int y, int w, int h, uint16_t top, uint16_t bot) { fxfig::vgrad(x, y, w, h, top, bot); }

// Procedural building silhouettes for one parallax layer. Buildings tile in world space; `parallax`
// (0=locked to camera, 1=full scroll) and `camx` slide them; `seed` decorrelates layers; heights vary
// per column. A sparse scatter of dim "windows" reads as a night skyline without extra colours.
static inline void skyline(int baseY, int minH, int maxH, int spacing, uint16_t col, uint16_t win,
                           float parallax, float camx, uint32_t seed)
{
    int SW = d.width();
    if (spacing < 6) spacing = 6;
    float off = camx * parallax;
    int i0 = (int)floorf(off / spacing) - 1;
    for (int i = i0;; i++) {
        int bx = (int)(i * spacing - off);
        if (bx > SW) break;
        if (bx + spacing < 0) continue;
        uint32_t h = (uint32_t)i * 2654435761u ^ seed; h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16;
        int rng = maxH - minH; if (rng < 1) rng = 1;
        int bh = minH + (int)(h % (uint32_t)rng);
        int bw = spacing - 2;
        int by = baseY - bh;
        d.fillRect(bx, by, bw, bh, col);
        if (win) {
            for (int wy = by + 4; wy < baseY - 3; wy += 7) {
                for (int wx = bx + 2; wx < bx + bw - 1; wx += 6) {
                    uint32_t r = ((uint32_t)(wx * 73856093) ^ (uint32_t)(wy * 19349663) ^ seed);
                    if ((r & 7u) == 0u) d.fillRect(wx, wy, 2, 2, win);
                }
            }
        }
    }
}

// The walkable street: a near->far gradient plus faint receding lines (fx3d Mode-7 grid). `scroll`
// (0..1) animates the floor toward the camera as the fighters advance.
static inline void floor(const fxfig::Belt& b, uint16_t far_, uint16_t near_, uint16_t line,
                         float vanx, float scroll)
{
    int SW = d.width();
    int top = (int)b.horizonY, bot = (int)b.frontY;
    fxfig::vgrad(0, top, SW, bot - top + 1, far_, near_);
    fx3d::Grid gr{ top, bot, vanx, scroll, (float)SW * 0.95f, 7, 8, line, 0, 80 };
    fx3d::grid(gr);
}

// ---------------------------------------------------------------- minimal LINE-ART primitives
// A clean white-paper aesthetic (Cadillacs & Dinosaurs, stylised): everything below is OUTLINE only —
// thin grey strokes on lots of white — never filled boxes. Heap-free, drawn through `d`. Layers compose
// back-to-front with their own parallax factor; pass a phase (ms*k) to sway/drift the near ones.
// Small deterministic hash for procedural, parallax-stable variety (no RNG state kept).
static inline uint32_t sc_hash(int i, uint32_t seed)
{
    uint32_t h = (uint32_t)i * 2654435761u ^ seed;
    h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16;
    return h;
}

// ridge(): a grey skyline drawn as a STROKED polyline (a horizon of roofs), not filled rectangles.
// Procedural step heights from `seed`; parallax = camx*parallax. `fill` (0 = none) paints a faint band
// just under the line so distant ridges read as soft mass without adding a second hue. Outline only.
static inline void ridge(int baseY, int minH, int maxH, int spacing, uint16_t col, uint16_t fill,
                         float parallax, float camx, uint32_t seed, float drift)
{
    int SW = d.width();
    if (spacing < 6) spacing = 6;
    int rng = maxH - minH; if (rng < 1) rng = 1;
    float off = camx * parallax - drift;
    int i0 = (int)floorf(off / spacing) - 1;
    int prevx = -10000, prevy = baseY;
    for (int i = i0;; i++) {
        int bx = (int)(i * spacing - off);
        if (bx - spacing > SW) break;
        uint32_t h = sc_hash(i, seed);
        int by = baseY - (minH + (int)(h % (uint32_t)rng));
        if (prevx > -9999) {
            // faint mass under the segment (a hair darker than paper), then the crisp stroke on top.
            if (fill) {
                int x0 = prevx < 0 ? 0 : prevx;
                int x1 = bx > SW ? SW : bx;
                int topy = prevy < by ? prevy : by;
                for (int xx = x0; xx <= x1; xx++)
                    d.drawFastVLine(xx, topy, baseY - topy + 1, fill);
            }
            d.drawLine(prevx, prevy, bx, by, col);          // roofline
            d.drawLine(bx, prevy, bx, by, col);             // riser between roofs -> a city silhouette
        }
        prevx = bx; prevy = by;
    }
}

// props(): simple stylised line shapes tiled in world space — lamp posts, arches, window frames, a tuft
// of foliage — drawn as OUTLINES in grey. `parallax`+`camx` slide them; `phase` (radians) lets the
// caller sway the tops a little (small idle animation). `kind` picks the silhouette so a level can mix
// near/far furniture from the same call. Few strokes by design.
static inline void props(int baseY, int spacing, uint16_t col, float parallax, float camx,
                         uint32_t seed, float phase, int kind)
{
    int SW = d.width();
    if (spacing < 12) spacing = 12;
    float off = camx * parallax;
    int i0 = (int)floorf(off / spacing) - 1;
    for (int i = i0;; i++) {
        int x = (int)(i * spacing - off);
        if (x - spacing > SW) break;
        if (x + spacing < 0) continue;
        uint32_t h = sc_hash(i, seed);
        int variant = (kind >= 0) ? kind : (int)(h & 3u);
        int hh = 24 + (int)(h % 22u);                       // prop height, varies per tile
        int sway = (int)(sinf(phase + i * 0.7f) * 2.0f);    // gentle top drift (animation)
        int topy = baseY - hh;
        if (variant == 0) {
            // lamp post: a thin upright + a little arm + a ring "lamp" (outline circle).
            d.drawFastVLine(x, topy, hh, col);
            d.drawLine(x, topy, x + 6 + sway, topy + 2, col);
            d.drawCircle(x + 7 + sway, topy + 4, 2, col);
        } else if (variant == 1) {
            // arch / doorway: two posts + a lintel, slightly taller.
            int w = 12;
            d.drawFastVLine(x, topy, hh, col);
            d.drawFastVLine(x + w, topy, hh, col);
            d.drawLine(x, topy, x + w, topy, col);
            d.drawLine(x + 1, topy + 3, x + w - 1, topy + 3, col);
        } else if (variant == 2) {
            // window frame: a rectangle outline with a cross mullion (storefront read).
            int w = 14, hwin = hh > 20 ? 20 : hh;
            d.drawRect(x, topy, w, hwin, col);
            d.drawFastVLine(x + w / 2, topy, hwin, col);
            d.drawFastHLine(x, topy + hwin / 2, w, col);
        } else {
            // foliage: a stem + a small open canopy of strokes that sways at the tips.
            int cx = x, cy = topy;
            d.drawFastVLine(cx, cy, hh, col);
            d.drawLine(cx, cy, cx - 5 + sway, cy - 4, col);
            d.drawLine(cx, cy, cx + 5 + sway, cy - 4, col);
            d.drawLine(cx, cy + 4, cx - 6 + sway, cy + 1, col);
            d.drawLine(cx, cy + 4, cx + 6 + sway, cy + 1, col);
        }
    }
}

// clean_floor(): NO grid. A subtle grey ground band (a hair darker than paper) under the horizon plus a
// single faint horizon stroke. Walkable depth is read from figure SCALE, not floor lines, so the look
// stays minimal. `ground` should be only slightly off paper; `horizon` is the thin seam colour.
static inline void clean_floor(const fxfig::Belt& b, uint16_t ground, uint16_t horizon)
{
    int SW = d.width();
    int top = (int)b.horizonY, bot = (int)b.frontY;
    if (bot < top) bot = top;
    d.fillRect(0, top, SW, bot - top + 1, ground);
    d.drawFastHLine(0, top, SW, horizon);                   // the one faint horizon line
}

} // namespace fxscene
