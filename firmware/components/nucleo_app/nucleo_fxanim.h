// nucleo_fxanim.h — REUSABLE humanoid animation primitives for the fxfig silhouette engine.
//
// fxfig draws a posed body; fxanim PRODUCES the pose. These are generic, game-agnostic building
// blocks (idle/walk/reach/kick/recoil/airborne/collapse + pose blend) that compose into any move set —
// a brawler combo, a platformer hop, a fighting-game stance. Pure-inline, HEAP-FREE, no `d` access:
// just math over an 11-joint pose array. Every fn OVERRIDES the joints it owns, so callers layer them
// (base -> walk -> reach) without resetting the rest. Keep moves out of here; this is the vocabulary.
#pragma once
#include "nucleo_fxfig.h"
#include <string.h>
#include <math.h>

namespace fxanim {

using fxfig::Pt;
// Re-export the joint indices with short names so move code reads cleanly inside fxanim/callers.
enum { PELVIS = fxfig::FX_PELVIS, NECK = fxfig::FX_NECK, HEAD = fxfig::FX_HEAD,
       BELBOW = fxfig::FX_BELBOW, BHAND = fxfig::FX_BHAND, FELBOW = fxfig::FX_FELBOW, FHAND = fxfig::FX_FHAND,
       BKNEE = fxfig::FX_BKNEE, BFOOT = fxfig::FX_BFOOT, FKNEE = fxfig::FX_FKNEE, FFOOT = fxfig::FX_FFOOT,
       NJ = fxfig::FX_NJ };

// A crumpled "on the ground" pose (figure space) — collapse() blends STAND -> LIE.
static const Pt LIE[NJ] = {
    /*PELVIS*/ { 0.10f, 0.16f }, /*NECK*/ { 0.52f, 0.22f }, /*HEAD*/ { 0.78f, 0.27f },
    /*BELBOW*/ { 0.34f, 0.30f }, /*BHAND*/ { 0.54f, 0.20f },
    /*FELBOW*/ { 0.40f, 0.10f }, /*FHAND*/ { 0.64f, 0.12f },
    /*BKNEE */ { 0.00f, 0.18f }, /*BFOOT*/ { -0.34f, 0.06f },
    /*FKNEE */ { 0.16f, 0.16f }, /*FFOOT*/ { -0.08f, 0.04f },
};

static inline void base(Pt* p)                  { memcpy(p, fxfig::STAND, sizeof(Pt) * NJ); }
static inline void copy(Pt* dst, const Pt* src) { memcpy(dst, src, sizeof(Pt) * NJ); }
static inline void blend(Pt* out, const Pt* a, const Pt* b, float t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    for (int i = 0; i < NJ; i++) { out[i].x = a[i].x + (b[i].x - a[i].x) * t; out[i].y = a[i].y + (b[i].y - a[i].y) * t; }
}

// 2-bone IK: place the knee/elbow for a 2-bone limb (l1,l2) reaching (fx,fy) from the root (hx,hy),
// bending toward (bx,by). Law-of-cosines along the chord + a perpendicular offset on the chosen side ->
// a natural, single-direction joint for ANY end position. The real cure for "reverse / backwards" joints.
static inline void ik2(float hx, float hy, float fx, float fy, float l1, float l2,
                       float bx, float by, float* kx, float* ky)
{
    float dx = fx - hx, dy = fy - hy;
    float dlen = sqrtf(dx * dx + dy * dy);   // NB: 'd' is the global gfx macro -> use dlen
    float maxd = l1 + l2 - 0.001f;
    if (dlen > maxd) dlen = maxd;
    if (dlen < 0.001f) dlen = 0.001f;
    float a  = (l1 * l1 - l2 * l2 + dlen * dlen) / (2.0f * dlen);
    float h2 = l1 * l1 - a * a; if (h2 < 0.0f) h2 = 0.0f;
    float hgt = sqrtf(h2);
    float ux = dx / dlen, uy = dy / dlen;
    float px = -uy, py = ux;
    if (px * bx + py * by < 0.0f) { px = -px; py = -py; }
    *kx = hx + ux * a + px * hgt;
    *ky = hy + uy * a + py * hgt;
}
// Limb bone lengths (figure units), sized to the STAND skeleton.
static const float LEG_L1 = 0.44f, LEG_L2 = 0.42f;   // thigh + shin
static const float ARM_L1 = 0.27f, ARM_L2 = 0.24f;   // upper arm + forearm

// Subtle breathing + a gentle hand sway — call after base() for a living idle (t = seconds). Elbows are
// IK'd so the resting arms hang with a natural bend instead of the fixed STAND angle.
static inline void idle(Pt* p, float t)
{
    float b = 0.012f * sinf(t * 2.2f);
    float sway = 0.012f * sinf(t * 1.6f);
    p[NECK].y += b; p[HEAD].y += b;
    p[FHAND].x += sway;  p[FHAND].y += b * 0.5f;
    p[BHAND].x -= sway;  p[BHAND].y += b * 0.5f;
    ik2(p[NECK].x, p[NECK].y, p[FHAND].x, p[FHAND].y, ARM_L1, ARM_L2, -1.0f, -0.3f, &p[FELBOW].x, &p[FELBOW].y);
    ik2(p[NECK].x, p[NECK].y, p[BHAND].x, p[BHAND].y, ARM_L1, ARM_L2, -1.0f, -0.3f, &p[BELBOW].x, &p[BELBOW].y);
}

// One full stride at `phase` (0..1, loops). `stride` scales the step length. Feet swing; knees solved
// by FORWARD IK so the legs articulate naturally through the whole cycle.
static inline void walk(Pt* p, float phase, float stride)
{
    float s = sinf(phase * 6.2831853f), c = cosf(phase * 6.2831853f);
    float bob = 0.025f * (c < 0 ? -c : c);
    p[PELVIS].y -= bob; p[NECK].y -= bob; p[HEAD].y -= bob;      // bob the hip FIRST (IK reads it)
    // feet swing fore/aft in opposite phase; lift whichever foot is swinging forward.
    p[FFOOT].x = 0.04f + 0.18f * s * stride; p[FFOOT].y = 0.02f + 0.12f * (s > 0 ? s : 0.0f);
    p[BFOOT].x = 0.04f - 0.18f * s * stride; p[BFOOT].y = 0.02f + 0.12f * (s < 0 ? -s : 0.0f);
    // knees: forward-bending IK from the (bobbed) hip to each foot.
    ik2(p[PELVIS].x, p[PELVIS].y, p[FFOOT].x, p[FFOOT].y, LEG_L1, LEG_L2, 1.0f, 0.0f, &p[FKNEE].x, &p[FKNEE].y);
    ik2(p[PELVIS].x, p[PELVIS].y, p[BFOOT].x, p[BFOOT].y, LEG_L1, LEG_L2, 1.0f, 0.0f, &p[BKNEE].x, &p[BKNEE].y);
    // arms counter-swing (opposite the legs): hands lead, elbows solved by IK with a backward bend so
    // the arm bends when it swings back and STRAIGHTENS when it reaches forward — alive, not a stick.
    p[FHAND].x = 0.15f - 0.17f * s; p[FHAND].y = 0.96f + 0.05f * s;
    p[BHAND].x = -0.03f + 0.17f * s; p[BHAND].y = 0.96f - 0.05f * s;
    ik2(p[NECK].x, p[NECK].y, p[FHAND].x, p[FHAND].y, ARM_L1, ARM_L2, -1.0f, -0.3f, &p[FELBOW].x, &p[FELBOW].y);
    ik2(p[NECK].x, p[NECK].y, p[BHAND].x, p[BHAND].y, ARM_L1, ARM_L2, -1.0f, -0.3f, &p[BELBOW].x, &p[BELBOW].y);
}

// Front-arm thrust (punch/jab) — `amt` 0..1 extension, `h` figure-space target height (~1.30 chest).
static inline void reach(Pt* p, float amt, float h)
{
    p[FELBOW].x = 0.16f + 0.20f * amt; p[FELBOW].y = h;
    p[FHAND].x = 0.22f + 0.58f * amt;  p[FHAND].y = h;
    p[NECK].x += 0.08f * amt; p[HEAD].x += 0.10f * amt;
    p[BHAND].x = 0.02f; p[BHAND].y = 1.28f;            // rear hand guards
}

// Front-leg kick — `amt` 0..1. Foot snaps forward (and a touch up); the knee is solved by IK so it
// chambers then extends the natural way (no reverse bend).
static inline void kick(Pt* p, float amt)
{
    p[FFOOT].x = 0.16f + 0.62f * amt; p[FFOOT].y = 0.08f + 0.42f * amt;
    ik2(p[PELVIS].x, p[PELVIS].y, p[FFOOT].x, p[FFOOT].y, LEG_L1, LEG_L2, 1.0f, 0.2f, &p[FKNEE].x, &p[FKNEE].y);
    p[NECK].x -= 0.06f * amt; p[HEAD].x -= 0.08f * amt; // counter-lean for balance
    p[BHAND].x -= 0.06f * amt;
}

// Both hands up (block / stance).
static inline void guard(Pt* p)
{
    p[FHAND].x = 0.10f; p[FHAND].y = 1.34f; p[FELBOW].x = 0.10f; p[FELBOW].y = 1.22f;
    p[BHAND].x = 0.02f; p[BHAND].y = 1.30f;
}

// Hit recoil — upper body snaps back (away from +x), arms flail. `amt` 0..1.
static inline void recoil(Pt* p, float amt)
{
    p[NECK].x -= 0.16f * amt; p[HEAD].x -= 0.22f * amt; p[PELVIS].x -= 0.05f * amt;
    p[FHAND].x += 0.05f; p[FHAND].y += 0.10f * amt; p[BHAND].x -= 0.12f * amt;
    p[FKNEE].x += 0.04f * amt; p[BKNEE].x -= 0.04f * amt;
}

// Mid-air tuck — `tuck` 0..1 (pair with a yoff jump arc owned by the game). Knees solved by IK.
static inline void airborne(Pt* p, float tuck)
{
    p[FFOOT].x = 0.16f;  p[FFOOT].y = 0.30f + 0.10f * (1.0f - tuck);
    p[BFOOT].x = -0.04f; p[BFOOT].y = 0.30f + 0.10f * (1.0f - tuck);
    ik2(p[PELVIS].x, p[PELVIS].y, p[FFOOT].x, p[FFOOT].y, LEG_L1, LEG_L2, 1.0f, 0.0f, &p[FKNEE].x, &p[FKNEE].y);
    ik2(p[PELVIS].x, p[PELVIS].y, p[BFOOT].x, p[BFOOT].y, LEG_L1, LEG_L2, 1.0f, 0.0f, &p[BKNEE].x, &p[BKNEE].y);
    p[FHAND].y = 1.22f; p[BHAND].y = 1.22f;
}

// KO crumple — `t` 0..1 from standing to lying.
static inline void collapse(Pt* p, float t) { Pt s[NJ]; base(s); blend(p, s, LIE, t); }

} // namespace fxanim
