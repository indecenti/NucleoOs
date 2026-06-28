// nucleo_fx3d.h — a tiny, reusable pseudo-3D FX toolkit for native apps (SNES-era tricks, modernised).
//
// Two primitives, both pure-inline and HEAP-FREE (the no-hoarding rule kept by owning nothing) and
// drawn through the shared `d` target, so they composite into the off-screen frame buffer with zero
// flicker exactly like the rest of a native app's on_draw():
//
//   * fx3d::grid()       — a Mode-7 perspective plane: converging "fan" verticals + perspective-spaced,
//                          scrolling horizontals that recede to a vanishing point. No texture, no RAM —
//                          the lines are computed analytically, so it costs only a few dozen drawLine /
//                          drawFastHLine calls. This is the F-Zero / Star-Fox-surface look.
//   * fx3d::draw_model() — a flat-shaded polygon mesh (painter-sorted, fake-lit) billboarded at a screen
//                          point with a caller-supplied per-pixel scale. Star-Fox-style solid ships for a
//                          few dozen triangles a frame — the LOD "near" tier above the wireframe sprites.
//
// Colours are RGB565. Everything is namespaced so it never clashes with an app's own rgb()/shade().
#pragma once
#include "app_gfx.h"     // the movable `d` draw target (double-buffered compositing)
#include <stdint.h>
#include <math.h>

namespace fx3d {

static inline uint16_t rgb(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// scale an RGB565 colour by num/den (clamped so num<=den -> only darkens; we never brighten past source).
static inline uint16_t scl(uint16_t c, int num, int den)
{
    if (den < 1) den = 1;
    if (num < 0) num = 0; else if (num > den) num = den;
    int r = ((c >> 11) & 31) * num / den, g = ((c >> 5) & 63) * num / den, b = (c & 31) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
// linear blend a->b by t (0..255); cheap RGB565 lerp for white-hot -> tint -> ember ramps.
static inline uint16_t mix(uint16_t a, uint16_t b, int t)
{
    if (t < 0) t = 0; else if (t > 255) t = 255;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (br - ar) * t / 255) << 11) | ((ag + (bg - ag) * t / 255) << 5) | (ab + (bb - ab) * t / 255));
}

// ---------------------------------------------------------------------------- Mode-7 grid plane
struct Grid {
    int   horizon;     // screen y of the vanishing line
    int   bottom;      // screen y where the plane ends (e.g. content height - 1)
    float vanx;        // vanishing-point x (pan it with yaw / IMU tilt / the reticle)
    float scroll;      // 0..1 phase; advance it to drive the floor toward the camera
    float xspread;     // half-width (px) of the fan at `bottom` -> controls perspective width
    int   nv;          // vertical "fan" lines each side of centre
    int   nh;          // scrolling horizontal lines (0 = none -> no scanline look)
    uint16_t col;      // neon line colour (full brightness near the camera)
    uint16_t glow;     // horizon glow colour (0 = none)
    int   intensity;   // 0..255 master fade
};
static inline void grid(const Grid& c)
{
    int SW = d.width();
    int rows = c.bottom - c.horizon; if (rows < 2) return;
    int inten = c.intensity < 0 ? 0 : (c.intensity > 255 ? 255 : c.intensity);

    // (a) scrolling horizontals: perspective-spaced (dense near the horizon), sliding toward you. They
    //     enter dim at the horizon and brighten/widen as they near -> a floor moving under the camera.
    for (int i = 0; i < c.nh; i++) {
        float u = ((float)i + c.scroll) / (float)c.nh;       // 0..1 depth slot (wraps seamlessly)
        float p = u * u;                                     // ease: bunch toward the horizon
        int y = c.horizon + (int)(p * rows);
        if (y <= c.horizon || y >= c.bottom) continue;
        float w = (float)(y - c.horizon) / (float)rows;      // 0 at horizon, 1 at bottom
        int half = (int)(c.xspread * w);
        int x0 = (int)c.vanx - half, x1 = (int)c.vanx + half;
        if (x0 < 0) x0 = 0;
        if (x1 > SW - 1) x1 = SW - 1;
        if (x1 <= x0) continue;
        d.drawFastHLine(x0, y, x1 - x0 + 1, scl(c.col, (int)(40 + 215 * w) * inten / 255, 255));
    }
    // (b) converging "fan" verticals: from the vanishing point out to the bottom edge.
    for (int j = -c.nv; j <= c.nv; j++) {
        float fx = c.nv ? (float)j / (float)c.nv : 0.0f;     // -1..1
        int xb = (int)(c.vanx + fx * c.xspread);
        int a = fx < 0 ? (int)(-fx * 255) : (int)(fx * 255);
        d.drawLine((int)c.vanx, c.horizon, xb, c.bottom, scl(c.col, (60 + a / 2) * inten / 255, 255));
    }
    // (c) horizon glow line
    if (c.glow) {
        d.drawFastHLine(0, c.horizon, SW, scl(c.glow, inten, 255));
        if (c.horizon + 1 < c.bottom) d.drawFastHLine(0, c.horizon + 1, SW, scl(c.glow, inten / 2, 255));
    }
}

// ---------------------------------------------------------------------------- flat-shaded polygon mesh
struct V3  { float x, y, z; };
struct Tri { uint8_t a, b, c; };
struct Model { const V3* v; int nv; const Tri* t; int nt; };

// Rotate a model point by yaw (around Y), then pitch (around X), then bank (around Z). Trig precomputed
// by the caller. With pitch=0 this reduces EXACTLY to the old yaw+bank path (so draw_model is unchanged).
static inline void rot3(float x, float y, float z, float cy_, float sy_, float cp, float sp, float cb, float sb,
                        float* ox, float* oy, float* oz)
{
    float x1 =  x * cy_ + z * sy_;        // yaw around Y
    float z1 = -x * sy_ + z * cy_;
    float y1 =  y * cp - z1 * sp;         // pitch around X
    float z2 =  y * sp + z1 * cp;
    *ox = x1 * cb - y1 * sb;              // bank around Z
    *oy = x1 * sb + y1 * cb;
    *oz = z2;
}

// Billboarded flat-shaded mesh with FULL 3-axis rotation (yaw=Y, pitch=X, bank=Z): the caller has done
// the perspective divide for the CENTRE (screen sx,sy + per-px scale sc). Faces are painted far->near
// (painter's algorithm) and fake-lit from the rotated normal (top-lit + side gradient + rim). The
// 2-axis draw_model() below delegates here with pitch=0, so every existing caller is untouched.
static inline void draw_model_ex(const Model& m, float sx, float sy, float sc,
                                 float yaw, float pitch, float bank, uint16_t col, bool seams = true)
{
    if (m.nv > 24 || m.nt > 24 || m.nv < 3) return;
    float rx[24], ry[24], rz[24];
    float cyaw = cosf(yaw), syaw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cb = cosf(bank), sb = sinf(bank);
    for (int i = 0; i < m.nv; i++)
        rot3(m.v[i].x, m.v[i].y, m.v[i].z, cyaw, syaw, cp, sp, cb, sb, &rx[i], &ry[i], &rz[i]);
    int order[24]; for (int i = 0; i < m.nt; i++) order[i] = i;
    for (int a = 1; a < m.nt; a++) {                 // insertion sort by face depth, far (large z) first
        int k = order[a]; float zk = rz[m.t[k].a] + rz[m.t[k].b] + rz[m.t[k].c];
        int j = a - 1;
        while (j >= 0 && (rz[m.t[order[j]].a] + rz[m.t[order[j]].b] + rz[m.t[order[j]].c]) < zk) {
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = k;
    }
    // Near foes get low-poly definition: dark facet seams (toon/Star-Fox edges) + one specular hull
    // glint on the brightest face. Skipped when small so distant ships stay clean. No back-face cull on
    // purpose (hand-wound, not strictly convex -> painter order is the robust hidden-surface choice; fill
    // rate isn't the bottleneck on this board, RAM is).
    bool edges = seams && sc > 6.0f;   // seams=false -> smooth flat-shaded solid (no triangulation diagonals)
    int  bestL = 0; float gx = sx, gy = sy;
    for (int o = 0; o < m.nt; o++) {
        const Tri& tr = m.t[order[o]];
        float ax = sx + rx[tr.a] * sc, ay = sy + ry[tr.a] * sc;
        float bx = sx + rx[tr.b] * sc, by = sy + ry[tr.b] * sc;
        float cx = sx + rx[tr.c] * sc, cy = sy + ry[tr.c] * sc;
        float e1x = rx[tr.b] - rx[tr.a], e1y = ry[tr.b] - ry[tr.a], e1z = rz[tr.b] - rz[tr.a];
        float e2x = rx[tr.c] - rx[tr.a], e2y = ry[tr.c] - ry[tr.a], e2z = rz[tr.c] - rz[tr.a];
        float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        float inl = 1.0f / (sqrtf(nx * nx + ny * ny + nz * nz) + 1e-4f);
        float avgz = (rz[tr.a] + rz[tr.b] + rz[tr.c]) * (1.0f / 3.0f);
        // top-lit + side gradient + rim (edge-on faces glow) + depth tint (near/nose brighter than far).
        int light = 150 - (int)(80.0f * ny * inl) + (int)(45.0f * nx * inl)
                  + (int)(46.0f * (1.0f - fabsf(nz) * inl)) - (int)(avgz * 22.0f);
        if (light < 64) light = 64; else if (light > 255) light = 255;
        d.fillTriangle((int)ax, (int)ay, (int)bx, (int)by, (int)cx, (int)cy, scl(col, light, 255));
        if (edges) {
            uint16_t ec = scl(col, 40, 255);                 // dark facet seam
            d.drawLine((int)ax, (int)ay, (int)bx, (int)by, ec);
            d.drawLine((int)bx, (int)by, (int)cx, (int)cy, ec);
            d.drawLine((int)cx, (int)cy, (int)ax, (int)ay, ec);
            if (light > bestL) { bestL = light; gx = (ax + bx + cx) * (1.0f / 3.0f); gy = (ay + by + cy) * (1.0f / 3.0f); }
        }
    }
    if (edges && bestL >= 244) d.drawPixel((int)gx, (int)gy, 0xFFFF);   // specular hull glint
}

// 2-axis convenience (yaw + bank) — unchanged behaviour for every existing caller.
static inline void draw_model(const Model& m, float sx, float sy, float sc, float yaw, float bank, uint16_t col)
{
    draw_model_ex(m, sx, sy, sc, yaw, 0.0f, bank, col);
}

// Project ONE model-space point to the screen with the SAME rotation as draw_model_ex: returns screen
// x,y (rounded) + the rotated depth z (smaller = nearer the camera, matching the painter sort). Lets a
// caller mount decorations on a rotating model — pips on a die, hardpoints on a ship — that track it.
static inline void project(const V3& p, float sx, float sy, float sc, float yaw, float pitch, float bank,
                           int* outx, int* outy, float* outz)
{
    float rxv, ryv, rzv;
    float cyaw = cosf(yaw), syaw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cb = cosf(bank), sb = sinf(bank);
    rot3(p.x, p.y, p.z, cyaw, syaw, cp, sp, cb, sb, &rxv, &ryv, &rzv);
    *outx = (int)(sx + rxv * sc);
    *outy = (int)(sy + ryv * sc);
    *outz = rzv;
}

// Explode a model: every face becomes a shard that flies outward (along its centroid direction),
// tumbles, shrinks and cools white-hot -> tint -> ember as `progress` goes 0..1, with shards dropping
// out progressively so the burst thins instead of popping. Reuses the SAME flash model as the live
// ship, so a gorgeous model-shatter death costs ZERO extra RAM. Call over a short window per kill.
static inline void shatter(const Model& m, float sx, float sy, float sc,
                           float yaw, float bank, uint16_t col, float progress)
{
    if (m.nv > 24 || m.nt > 24 || m.nv < 3) return;
    float pr = progress; if (pr < 0.0f) pr = 0.0f; if (pr >= 1.0f) return;
    float rx[24], ry[24], rz[24];
    float cyaw = cosf(yaw), syaw = sinf(yaw), cb = cosf(bank), sb = sinf(bank);
    for (int i = 0; i < m.nv; i++) {
        float x = m.v[i].x, y = m.v[i].y, z = m.v[i].z;
        float x1 = x * cyaw + z * syaw, z1 = -x * syaw + z * cyaw;
        rx[i] = x1 * cb - y * sb; ry[i] = x1 * sb + y * cb; rz[i] = z1;
    }
    uint16_t hot = 0xFFFF, ember = rgb(86, 28, 10);
    for (int f = 0; f < m.nt; f++) {
        const Tri& tr = m.t[f];
        float ccx = (rx[tr.a] + rx[tr.b] + rx[tr.c]) * (1.0f / 3.0f);
        float ccy = (ry[tr.a] + ry[tr.b] + ry[tr.c]) * (1.0f / 3.0f);
        float ccz = (rz[tr.a] + rz[tr.b] + rz[tr.c]) * (1.0f / 3.0f);
        uint32_t h = (uint32_t)(f + 1) * 2654435761u;
        if ((int)((h >> 5) % 100) < (int)(pr * 78.0f)) continue;       // shards drop out as it burns down
        float jit = (float)((h >> 13) & 255) * (1.0f / 255.0f);
        float len = sqrtf(ccx * ccx + ccy * ccy + ccz * ccz); if (len < 1e-3f) len = 1e-3f;
        float push = pr * (1.4f + 1.6f * jit);
        float ox = ccx / len * push, oy = ccy / len * push;
        float spin = pr * (4.0f + 6.0f * jit) * (jit < 0.5f ? -1.0f : 1.0f);
        float ca = cosf(spin), sa = sinf(spin), shr = 1.0f - 0.5f * pr;
        float vsx[3], vsy[3];
        for (int k = 0; k < 3; k++) {
            int vi = (k == 0) ? tr.a : (k == 1) ? tr.b : tr.c;
            float lxr = rx[vi] - ccx, lyr = ry[vi] - ccy;
            float r2x = (lxr * ca - lyr * sa) * shr, r2y = (lxr * sa + lyr * ca) * shr;
            vsx[k] = sx + (ccx + ox + r2x) * sc;
            vsy[k] = sy + (ccy + oy + r2y) * sc;
        }
        uint16_t fc = (pr < 0.22f) ? mix(hot, col, (int)(pr / 0.22f * 255.0f))
                                   : mix(col, ember, (int)((pr - 0.22f) / 0.78f * 255.0f));
        d.fillTriangle((int)vsx[0], (int)vsy[0], (int)vsx[1], (int)vsy[1], (int)vsx[2], (int)vsy[2], fc);
    }
}

// 16-bit-style ORDERED DITHER: a checkerboard-filled disc reads as a "translucent" overlay on an opaque
// framebuffer (energy shields, hit bubbles, force fields) — the classic Genesis/SNES/PS1 fake-alpha. Cheap
// (~pi*r^2/2 plotted pixels); meant for brief flashes, not persistent fills.
static inline void dither_disc(int cx, int cy, int r, uint16_t col)
{
    if (r < 1) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy, rowx = r2 - dy * dy;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx > rowx) continue;
            if (((cx + dx + yy) & 1) == 0) continue;         // checkerboard stipple -> ~50% coverage
            d.drawPixel(cx + dx, yy, col);
        }
    }
}

} // namespace fx3d
