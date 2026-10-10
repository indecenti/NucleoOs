// tile_blit.h — shared 8bpp RGB332 colour-key tile atlas + blitters for the tile-based native games (Orde,
// Cardler). One place to read/optimise instead of a copy per app.
//
// Atlas format: 16x16 tiles packed 256 B each (1 B/px, RGB332), magenta key 0xE3 — the device canvas's own
// 8bpp format. Built by assets/cardler/build_atlas.mjs and assets/coop-rpg/repack_orde_atlas8.mjs.
//
// RAM: the atlas is read into 4 KB heap chunks (TILE_CHUNK tiles each) held in a TileAtlas the game keeps
// (32 B, no .bss buffer), so it never needs one big free block (Cardler's 86 tiles were one 22 KB block).
// Free it on exit with tile_atlas_free().
//
// CPU: while a frame is composited into the shared 8bpp canvas (always, except the direct-to-panel
// fallback) the blitters write RGB332 bytes straight into its buffer — a row memcpy / memset instead of a
// GFX call per pixel (a 32 px sprite was 256 fillRects). The fallback keeps the GFX path; both draw the
// same pixels (tools/native-host/games/orde.cpp checks it).
//
// Also shared: the two games' settings (sound + the ADV tilt controller: one file format, one settings
// list) — TileCfg below. Everything is `inline` with external linkage, so the firmware keeps ONE copy for
// both games (the linker folds the identical definitions) while each game still builds on its own.
#pragma once
#include "app_gfx.h"   // 'd' = canvas
#include "game_ui.h"   // gui:: settings list
#include "game_text.h" // the OS language
#include "nucleo_imu.h"
#include "nucleo_kbd.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TILE_TKEY     0xE3
#define TILE_CHUNK    16                  // tiles per heap block (4 KB)
#define TILE_MAXCHUNK 8                   // up to 128 tiles

typedef struct { uint8_t (*c[TILE_MAXCHUNK])[256]; } TileAtlas;

inline const uint8_t *tile_px(const TileAtlas *a, int idx) { return a->c[idx / TILE_CHUNK][idx % TILE_CHUNK]; }

inline void tile_atlas_free(TileAtlas *a)
{
    for (int i = 0; i < TILE_MAXCHUNK; i++) { free(a->c[i]); a->c[i] = nullptr; }
}
// Read n tiles from a raw atlas file. False — and nothing held — on any failure (missing, short, OOM).
inline bool tile_atlas_load(TileAtlas *a, const char *path, int n)
{
    tile_atlas_free(a);
    FILE *f = fopen(path, "rb");
    bool ok = f && n > 0 && n <= TILE_CHUNK * TILE_MAXCHUNK;
    for (int i = 0; ok && i * TILE_CHUNK < n; i++) {
        int k = n - i * TILE_CHUNK; if (k > TILE_CHUNK) k = TILE_CHUNK;
        a->c[i] = (uint8_t (*)[256])malloc((size_t)k * 256);
        ok = a->c[i] && fread(a->c[i], 256, (size_t)k, f) == (size_t)k;
    }
    if (f) fclose(f);
    if (!ok) tile_atlas_free(a);
    return ok;
}

// The 8bpp RGB332 frame being composited (+ its clip rect), or NULL on the direct-to-panel fallback.
struct TileFb { uint8_t *px; int w, x0, y0, x1, y1; };
inline bool tile_fb(TileFb *f)
{
    if (!nucleo_app_is_buffered() || d.getColorDepth() != lgfx::rgb332_1Byte || d.hasPalette()) return false;
    f->px = (uint8_t *)static_cast<lgfx::LGFX_Sprite *>(nucleo_app_gfx())->getBuffer();
    if (!f->px) return false;
    int32_t cx, cy, cw, ch; d.getClipRect(&cx, &cy, &cw, &ch);
    f->w = d.width(); f->x0 = cx; f->y0 = cy; f->x1 = cx + cw; f->y1 = cy + ch;
    return true;
}

// opaque 16x16 tile
inline void tile_blit_op(const uint8_t *src, int x, int y)
{
    TileFb f;
    if (!tile_fb(&f)) { d.pushImage(x, y, 16, 16, src); return; }
    int a = x < f.x0 ? f.x0 : x, b = x + 16 > f.x1 ? f.x1 : x + 16;
    if (a >= b) return;
    for (int sy = 0; sy < 16; sy++) {
        int yy = y + sy;
        if (yy >= f.y0 && yy < f.y1) memcpy(f.px + yy * f.w + a, src + sy * 16 + (a - x), (size_t)(b - a));
    }
}

// transparent 16x16 tile, per-pixel colour-key. fl: bit0 = H mirror, bit1 = V mirror.
inline void tile_blit_key(const uint8_t *src, int x, int y, int fl = 0)
{
    TileFb f; bool fb = tile_fb(&f);
    for (int sy = 0; sy < 16; sy++) {
        int yy = y + sy, ry = (fl & 2) ? 15 - sy : sy;
        if (fb && (yy < f.y0 || yy >= f.y1)) continue;
        for (int sx = 0; sx < 16; sx++) {
            uint8_t px = src[ry * 16 + ((fl & 1) ? 15 - sx : sx)];
            if (px == (uint8_t)TILE_TKEY) continue;
            int xx = x + sx;
            if (!fb) d.drawPixel(xx, yy, px);
            else if (xx >= f.x0 && xx < f.x1) f.px[yy * f.w + xx] = px;
        }
    }
}

// RGB332 byte from 8-bit channels (the atlas / canvas pixel format)
#define TILE_RGB332(r, g, b) ((uint8_t)(((r) & 0xE0) | (((g) >> 3) & 0x1C) | ((b) >> 6)))

// scaled colour-key blit: 16x16 source -> D x D centred at (cx,cy), nearest sampling, H-mirrored if f.
// Any D (not just integer multiples), so entities can be sized freely. ol >= 0: first paint a 1 px outline
// of that RGB332 colour around the silhouette, so a sprite pops off busy ground (canvas path only).
inline void tile_blit_sz(const uint8_t *src, int f, int cx, int cy, int D, int ol = -1)
{
    TileFb fb; bool direct = tile_fb(&fb);
    int ox = cx - D / 2, oy = cy - D / 2;
    for (int pass = (direct && ol >= 0) ? 0 : 1; pass < 2; pass++) {
        int g = pass ? 0 : 1;                            // the outline pass grows every source pixel by 1 px
        for (int sy = 0; sy < 16; sy++) {
            int y0 = oy + sy * D / 16 - g, y1 = oy + (sy + 1) * D / 16 + g;
            if (direct) { if (y0 < fb.y0) y0 = fb.y0; if (y1 > fb.y1) y1 = fb.y1; if (y0 >= y1) continue; }
            for (int sx = 0; sx < 16; sx++) {
                uint8_t px = src[sy * 16 + sx];
                if (px == (uint8_t)TILE_TKEY) continue;      // transparent: never touch it (no halo)
                int dxs = f ? (15 - sx) : sx;
                int x0 = ox + dxs * D / 16 - g, x1 = ox + (dxs + 1) * D / 16 + g;
                if (!direct) { d.fillRect(x0, y0, x1 - x0, y1 - y0, px); continue; }
                if (x0 < fb.x0) x0 = fb.x0;
                if (x1 > fb.x1) x1 = fb.x1;
                if (!pass) px = (uint8_t)ol;
                for (int yy = y0; yy < y1 && x0 < x1; yy++) memset(fb.px + yy * fb.w + x0, px, (size_t)(x1 - x0));
            }
        }
    }
}

// A 50 % checkerboard-stippled ellipse of an RGB332 colour: reads as translucent on 8bpp (soft shadows
// under sprites, torch light pools) at the cost of a few byte stores. The direct-to-panel fallback paints
// a solid ellipse instead.
inline void tile_dither_ellipse(int cx, int cy, int rx, int ry, uint8_t col)
{
    if (rx < 1 || ry < 1) return;
    TileFb f;
    if (!tile_fb(&f)) { d.fillEllipse(cx, cy, rx, ry, col); return; }
    for (int dy = -ry; dy <= ry; dy++) {
        int yy = cy + dy;
        if (yy < f.y0 || yy >= f.y1) continue;
        int hw = (int)(rx * sqrtf(1.0f - (float)(dy * dy) / (float)(ry * ry)));   // one sqrt per row
        uint8_t *row = f.px + yy * f.w;
        for (int xx = cx - hw + ((cx - hw + yy) & 1); xx <= cx + hw; xx += 2)
            if (xx >= f.x0 && xx < f.x1) row[xx] = col;
    }
}

// ---- the tile games' settings: sound + the ADV tilt controller, persisted as a tiny JSON ----
// The tilt neutral is angle-based (atan2 roll/pitch of the gravity vector), so it stays symmetric at any
// holding angle; Ctrl / Space / C recentre it. The keyboard always wins: tilt only moves you with no key held.
struct TileCfg { int audio, tilt, sens, inv_x, inv_y; bool dirty, recenter, rc_prev; int settle; float roll0, pitch0; };
enum { TCFG_AUDIO, TCFG_TILT, TCFG_SENS, TCFG_INVX, TCFG_INVY };
inline int tile_cfg_rows(void) { return nucleo_imu_present() ? 5 : 1; }   // the tilt rows only where the sensor is

inline void tile_cfg_load(TileCfg *c, const char *path)
{
    int t = 1, se = 10, ix = 1, iy = 0, au = 1;
    FILE *f = fopen(path, "rb");
    if (f) {
        char b[128]; int n = (int)fread(b, 1, sizeof b - 1, f); fclose(f); b[n < 0 ? 0 : n] = 0;
        sscanf(b, "{\"tilt\":%d,\"sens\":%d,\"ix\":%d,\"iy\":%d,\"au\":%d}", &t, &se, &ix, &iy, &au);
    }
    c->tilt = t != 0; c->sens = se < 1 ? 1 : se > 20 ? 20 : se; c->inv_x = ix != 0; c->inv_y = iy != 0; c->audio = au != 0;
    c->dirty = false; c->recenter = true; c->rc_prev = false; c->settle = 0;
}
inline void tile_cfg_save(TileCfg *c, const char *path)              // only when something changed
{
    if (!c->dirty) return;
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "{\"tilt\":%d,\"sens\":%d,\"ix\":%d,\"iy\":%d,\"au\":%d}\n", c->tilt, c->sens, c->inv_x, c->inv_y, c->audio);
    fclose(f);
    c->dirty = false;
}
inline void tile_cfg_adjust(TileCfg *c, int row, int dir)
{
    switch (row) {
        case TCFG_AUDIO: c->audio = !c->audio; break;
        case TCFG_TILT:  c->tilt = !c->tilt; break;
        case TCFG_SENS:  c->sens += dir; if (c->sens < 1) c->sens = 1; if (c->sens > 20) c->sens = 20; break;
        case TCFG_INVX:  c->inv_x = !c->inv_x; break;
        case TCFG_INVY:  c->inv_y = !c->inv_y; break;
    }
    c->dirty = true;
}
inline void tile_cfg_draw(const TileCfg *c, const gui::Menu &m, uint16_t accent)
{
    char r[5][32];
    const char *yes = GT("SI", "YES"), *no = GT("NO", "NO");
    snprintf(r[0], 32, "%s: %s", GT("Suoni", "Sound"), c->audio ? "ON" : "OFF");
    snprintf(r[1], 32, "%s: %s", GT("Giroscopio", "Tilt sensor"), c->tilt ? "ON" : "OFF");
    snprintf(r[2], 32, "%s: %d", GT("Sensibilita", "Sensitivity"), c->sens);
    snprintf(r[3], 32, "%s: %s", GT("Inverti X", "Invert X"), c->inv_x ? yes : no);
    snprintf(r[4], 32, "%s: %s", GT("Inverti Y", "Invert Y"), c->inv_y ? yes : no);
    const char *rows[5] = { r[0], r[1], r[2], r[3], r[4] };
    int y = gui::title(GT("Impostazioni", "Settings"), tile_cfg_rows() > 1 ? GT("Spazio o C azzera lo zero", "Space or C recenters the tilt") : nullptr, accent);
    gui::menu(m, rows, tile_cfg_rows(), y, nucleo_app_content_height(), accent);
}
// Ctrl / Space / C pressed (rising edge): recentre the tilt neutral. True on the press.
inline bool tile_cfg_recenter_key(TileCfg *c)
{
    bool rc = ((nucleo_kbd_mods() & NK_MOD_CTRL) != 0) || nucleo_kbd_char_down(' ') || nucleo_kbd_char_down('c');
    bool edge = rc && !c->rc_prev;
    c->rc_prev = rc;
    if (edge) c->recenter = true;
    return edge;
}
// Tilt deflection, each axis -1..1 (dead-zoned, ~34 deg = full), or false without a sensor / tilt off.
inline bool tile_cfg_tilt(TileCfg *c, float *mx, float *my)
{
    float gx, gy, gz;
    if (!c->tilt || !nucleo_imu_present() || !nucleo_imu_gravity_screen(&gx, &gy, &gz)) return false;
    float roll = atan2f(gx, gz), pitch = atan2f(gy, gz);
    if (c->recenter) { c->roll0 = roll; c->pitch0 = pitch; c->settle = 8; c->recenter = false; }
    float dr = roll - c->roll0, dp = pitch - c->pitch0;
    if (dr > (float)M_PI) dr -= 2 * (float)M_PI; else if (dr < -(float)M_PI) dr += 2 * (float)M_PI;
    if (dp > (float)M_PI) dp -= 2 * (float)M_PI; else if (dp < -(float)M_PI) dp += 2 * (float)M_PI;
    const float SPAN = 0.60f;
    float DZ = fmaxf(0.015f, 0.16f - (float)c->sens * 0.007f);       // rad; high sensitivity = tiny dead-zone
    // Just after a recentre, re-baseline for a few frames so the settling low-pass can't bake a bias; inside
    // the dead-zone, slowly pull the neutral to the current pose so a residual drift decays to zero.
    if (c->settle > 0) { c->roll0 += dr * 0.6f; c->pitch0 += dp * 0.6f; c->settle--; dr = 0; dp = 0; }
    else if (fabsf(dr) < DZ && fabsf(dp) < DZ) { c->roll0 += dr * 0.05f; c->pitch0 += dp * 0.05f; }
    if (c->inv_x) dr = -dr;
    if (c->inv_y) dp = -dp;
    *mx = fabsf(dr) > DZ ? fminf(fmaxf(dr, -SPAN), SPAN) / SPAN : 0.0f;
    *my = fabsf(dp) > DZ ? fminf(fmaxf(dp, -SPAN), SPAN) / SPAN : 0.0f;
    return true;
}

// A 50 % stippled rectangle of an RGB332 colour: a translucent tint / darkening (screen-edge flashes, a
// backing for text over busy art). The direct-to-panel fallback skips it (no flicker-prone solid box).
inline void tile_dither_rect(int x, int y, int w, int h, uint8_t col)
{
    TileFb f;
    if (!tile_fb(&f)) return;
    int x0 = x < f.x0 ? f.x0 : x, x1 = x + w > f.x1 ? f.x1 : x + w;
    for (int yy = y < f.y0 ? f.y0 : y; yy < y + h && yy < f.y1; yy++) {
        uint8_t *row = f.px + yy * f.w;
        for (int xx = x0 + ((x0 + yy) & 1); xx < x1; xx += 2) row[xx] = col;
    }
}
// RGB565 -> the canvas's RGB332 byte
inline uint8_t tile_c332(uint16_t c) { return (uint8_t)((((c >> 13) & 7) << 5) | (((c >> 8) & 7) << 2) | ((c >> 3) & 3)); }
