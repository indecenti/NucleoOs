// nucleo_fxfig.h — articulated SILHOUETTE-figure extension to the fx3d toolkit.
//
// A belt-scroll brawler (Cadillacs & Dinosaurs, stylised) needs posable humanoids drawn as clean
// solid silhouettes — near-black heroes, one-colour enemies with a crisp outline — reading crisply on
// a minimal WHITE "paper" backdrop and shrinking with depth, plus the few support primitives that
// style needs (depth mapping, grey gradients). Like nucleo_fx3d.h this
// is PURE-INLINE and HEAP-FREE (owns nothing) and draws through the shared `d` target, so figures
// composite into the off-screen frame buffer with zero flicker exactly like the rest of on_draw().
// It builds ON fx3d (rgb/scl/mix reused) — the same toolkit family, one step further: from rigid
// flat-shaded ships to articulated, animated bodies.
#pragma once
#include "nucleo_fx3d.h"

namespace fxfig {

using fx3d::rgb;
using fx3d::scl;
using fx3d::mix;

// ---------------------------------------------------------------- belt-scroll depth plane
// The "belt" is the walkable strip with depth: z in [0..1], 0 = far edge (small, high on screen),
// 1 = near edge (big, toward the camera at the bottom). Feet sit on belt_y(z); the figure is drawn
// at belt_s(z) scale. This is the cheap 2.5D the genre lives on — no perspective divide per vertex.
struct Belt { float horizonY, frontY, farS, nearS; };
static inline float belt_y(const Belt& b, float z) { return b.horizonY + (b.frontY - b.horizonY) * z; }
static inline float belt_s(const Belt& b, float z) { return b.farS + (b.nearS - b.farS) * z; }

// ---------------------------------------------------------------- skeleton
// A compact side-view humanoid. Shoulders ride the NECK joint, hips ride the PELVIS joint, so 11
// points describe a whole body. Positions are FIGURE SPACE: origin at the feet, +x = the way the
// fighter faces, +y = up. The app fills these per animation frame; the renderer just inks them.
enum { FX_PELVIS = 0, FX_NECK, FX_HEAD, FX_BELBOW, FX_BHAND, FX_FELBOW, FX_FHAND,
       FX_BKNEE, FX_BFOOT, FX_FKNEE, FX_FFOOT, FX_NJ };

struct Pt { float x, y; };

// A neutral standing pose — the app copies this and bends it into walk/punch/kick/etc.
static const Pt STAND[FX_NJ] = {
    /*PELVIS*/ { 0.00f, 0.86f },
    /*NECK  */ { 0.02f, 1.42f },
    /*HEAD  */ { 0.05f, 1.60f },
    /*BELBOW*/ { -0.07f, 1.18f },
    /*BHAND */ { -0.04f, 0.96f },
    /*FELBOW*/ { 0.12f, 1.18f },
    /*FHAND */ { 0.16f, 0.95f },
    /*BKNEE */ { -0.02f, 0.46f },
    /*BFOOT */ { -0.09f, 0.02f },
    /*FKNEE */ { 0.09f, 0.45f },
    /*FFOOT */ { 0.12f, 0.02f },
};

// A tapered, round-capped "ink stroke" between two screen points — the silhouette building block.
// Filled body (a quad split into two triangles) + a disc at each end welds joints seamlessly so the
// body reads as ONE solid shape with no gaps. Widths are in PIXELS (caller already applied scale).
static inline void inkbar(float ax, float ay, float bx, float by, float wa, float wb, uint16_t col)
{
    float dx = bx - ax, dy = by - ay;
    float L = sqrtf(dx * dx + dy * dy);
    float ha = wa * 0.5f, hb = wb * 0.5f;
    if (L < 0.5f) { d.fillCircle((int)ax, (int)ay, (int)(ha + 0.5f), col); return; }
    float nx = -dy / L, ny = dx / L;                                   // unit normal
    float x0 = ax + nx * ha, y0 = ay + ny * ha, x1 = ax - nx * ha, y1 = ay - ny * ha;
    float x2 = bx - nx * hb, y2 = by - ny * hb, x3 = bx + nx * hb, y3 = by + ny * hb;
    d.fillTriangle((int)x0, (int)y0, (int)x1, (int)y1, (int)x2, (int)y2, col);
    d.fillTriangle((int)x0, (int)y0, (int)x2, (int)y2, (int)x3, (int)y3, col);
    // Round caps weld joints into one shape. ALWAYS draw at least a 1px disc so far/shrunken figures
    // (sub-pixel limb widths) don't crack apart between segments — the depth-clean guarantee.
    int ra = (int)(ha + 0.5f); if (ra < 1) ra = 1;
    int rb = (int)(hb + 0.5f); if (rb < 1) rb = 1;
    d.fillCircle((int)ax, (int)ay, ra, col);
    d.fillCircle((int)bx, (int)by, rb, col);
}

// Ink the whole limb/torso skeleton in ONE colour at a width bias. The silhouette is built bottom-up
// (far limbs -> torso -> head -> near limbs) so the camera-side arm/leg overlaps correctly whichever
// way the fighter faces. `wbias` fattens every segment & the head by a few px: pass 0 for the body
// pass, a small positive value for the outline pass underneath. Iconic, simple proportions — a clean
// shape on white, not a blob. Head is drawn by the caller (figure) so the outline can ring it too.
// Three depth shades: cB = back limbs (recede, darker), cM = torso, cF = front limbs (pop, lighter).
// Pass the same colour for all three to draw flat (e.g. the outline pass). `wbias` fattens for outlines.
static inline void figbody(const Pt* j, float sx, float feetY, float sc, int dir,
                           float g, uint16_t cB, uint16_t cM, uint16_t cF, float wbias)
{
#define FXX(i) (sx + dir * j[i].x * sc)
#define FXY(i) (feetY - j[i].y * sc)
    // back leg + back arm (away from camera) — darker so they recede; lean limbs ("magri")
    inkbar(FXX(FX_PELVIS), FXY(FX_PELVIS), FXX(FX_BKNEE), FXY(FX_BKNEE), 0.115f * g + wbias, 0.085f * g + wbias, cB);
    inkbar(FXX(FX_BKNEE),  FXY(FX_BKNEE),  FXX(FX_BFOOT), FXY(FX_BFOOT), 0.085f * g + wbias, 0.050f * g + wbias, cB);
    inkbar(FXX(FX_NECK),   FXY(FX_NECK),   FXX(FX_BELBOW),FXY(FX_BELBOW),0.085f * g + wbias, 0.065f * g + wbias, cB);
    inkbar(FXX(FX_BELBOW), FXY(FX_BELBOW), FXX(FX_BHAND), FXY(FX_BHAND), 0.065f * g + wbias, 0.045f * g + wbias, cB);
    // torso — the iconic core: a slim waist flaring to broad-ish shoulders (V-taper)
    inkbar(FXX(FX_PELVIS), FXY(FX_PELVIS), FXX(FX_NECK), FXY(FX_NECK), 0.165f * g + wbias, 0.235f * g + wbias, cM);
    // front leg + front arm (toward camera) — lighter so they pop forward
    inkbar(FXX(FX_PELVIS), FXY(FX_PELVIS), FXX(FX_FKNEE), FXY(FX_FKNEE), 0.12f * g + wbias, 0.085f * g + wbias, cF);
    inkbar(FXX(FX_FKNEE),  FXY(FX_FKNEE),  FXX(FX_FFOOT), FXY(FX_FFOOT), 0.085f * g + wbias, 0.050f * g + wbias, cF);
    inkbar(FXX(FX_NECK),   FXY(FX_NECK),   FXX(FX_FELBOW),FXY(FX_FELBOW),0.085f * g + wbias, 0.065f * g + wbias, cF);
    inkbar(FXX(FX_FELBOW), FXY(FX_FELBOW), FXX(FX_FHAND), FXY(FX_FHAND), 0.065f * g + wbias, 0.045f * g + wbias, cF);
#undef FXX
#undef FXY
}

// Ink a whole humanoid. `j` = 11 figure-space joints; placed at screen (sx, feetY) with per-unit
// scale `sc`, facing `dir` (+1 right / -1 left, mirrors x). `body` = silhouette colour, `rim` = a
// CLEAN OUTLINE colour (0 = none): when set, the entire silhouette is stroked first as a slightly
// fatter shape in `rim`, then the body is filled on top — a crisp coloured edge on the white "paper"
// for enemies, while heroes (rim==0) stay pure ink silhouettes. `girth` scales limb thickness (Mole =
// burly >1, Vipera = lean <1).
static inline void figure(const Pt* j, float sx, float feetY, float sc, int dir,
                          uint16_t body, uint16_t rim, float girth)
{
    float g = girth * sc;
    float hr = 0.15f * sc * (0.80f + 0.20f * girth);   // head tracks build: burly -> bigger, lean -> smaller
    float hx = sx + dir * j[FX_HEAD].x * sc;
    float hy = feetY - j[FX_HEAD].y * sc;
    // Depth shading — subtle "same colour, slightly different" shades so arms & legs read apart and the
    // body gains volume: back limbs recede (darker), torso at the base, front limbs pop (lighter), head a
    // touch lighter still. Works for the near-black heroes and each enemy hue alike.
    uint16_t dark = scl(body, 158, 255);
    uint16_t lite = mix(body, 0xFFFF, 64);
    uint16_t hedc = mix(body, 0xFFFF, 30);
    if (rim) {
        // OUTLINE PASS: same shape, fattened ~2px, drawn underneath in the flat rim colour. Thickness
        // eases with depth so far figures don't drown in their own edge (min ~1px, up to ~2px near).
        float ow = 1.0f + 0.012f * sc;
        if (ow > 2.2f) ow = 2.2f;
        d.fillCircle((int)hx, (int)hy, (int)(hr + ow + 0.5f), rim);
        figbody(j, sx, feetY, sc, dir, g, rim, rim, rim, ow);
    }
    // BODY PASS: shaded silhouette (back dark -> torso base -> front light).
    figbody(j, sx, feetY, sc, dir, g, dark, body, lite, 0.0f);
    d.fillCircle((int)hx, (int)hy, (int)(hr + 0.5f), hedc);
}

// ---------------------------------------------------------------- backdrop helpers
// Vertical grey gradient band (sky / wall). top->bot RGB565 lerp, one HLine per row.
static inline void vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bot)
{
    if (h < 1) return;
    for (int i = 0; i < h; i++)
        d.drawFastHLine(x, y + i, w, mix(top, bot, i * 255 / (h > 1 ? h - 1 : 1)));
}

// A flattened "puddle" ellipse on the floor (blood pool / shadow). Cheap, ground-hugging.
static inline void puddle(int cx, int cy, int rx, int ry, uint16_t col)
{
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    d.fillEllipse(cx, cy, rx, ry, col);
}

} // namespace fxfig
