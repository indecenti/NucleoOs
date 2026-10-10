// brawler_chars.cpp — SCORRIBANDA: the 3 playable heroes + per-fighter pose composition.
//
// Owns the hero roster (stats + silhouette flavour) and turns a Fighter's current state/anim into the
// 11-joint pose the renderer inks. Poses are authored facing dir=+1; the draw loop mirrors by dir.
// HEAP-FREE: a single const table + stack-only pose math. Draws nothing (no `d` access) — it only
// composes fxanim primitives and hands the joints back.
#include "brawler.h"
#include "game_text.h"
#include "game_ui.h"
#include <math.h>

using namespace fxfig;   // Pt, FX_* joints, inkbar

#ifndef BR_PI
#define BR_PI 3.14159265f
#endif

// ------------------------------------------------------------------ hero roster
// Three archetypes, each in its own colour (COLS below) AND a DISTINCT BUILD so they read apart by shape
// too — scale = overall HEIGHT, girth = WIDTH/limb thickness (and the head tracks girth in the renderer,
// so a burly fighter is also broad-headed). Reach, speed and combo length give each a different feel in
// the hand. (`rim` is unused: every fighter gets the shell's black outline.)
//
//   OMBRA  — the technician: average height/build, fast 3-hit string, balanced everything. The baseline.
//   MOLE   — the bruiser:    tall AND wide (scale 1.14, girth 1.44), slow, only a 2-hit string but each
//                            blow is heavy; short reach (works inside). A walking wall.
//   VIPERA — the striker:    short and lean (scale 0.92, girth 0.80), very fast, long reach, a 4-hit
//                            flurry of light snaps. Fragile but slippery.
static const HeroDef HEROES[3] = {
    // name     maxhp speed  pdmg kdmg reach girth rim combo scale
    { "OMBRA",  100,  78.f,   7,  10, 30.f, 1.00f, 0, 3, 1.00f },
    { "MOLE",   150,  56.f,  12,  16, 26.f, 1.44f, 0, 2, 1.14f },
    { "VIPERA",  80, 100.f,   5,   8, 38.f, 0.80f, 0, 4, 0.92f },
};

int brawler_hero_count(void) { return 3; }

const char *brawler_hero_style(int i)
{
    return i == 1 ? GT("Potente", "Heavy") : i == 2 ? GT("Veloce", "Fast") : GT("Equilibrato", "Balanced");
}

const HeroDef *brawler_hero(int i)
{
    if (i < 0) i = 0; else if (i > 2) i = 2;   // clamp: menu cursors should never index OOB
    return &HEROES[i];
}

// ------------------------------------------------------------------ per-hero signature flavour
// Readable Capcom-style signature on every strike, so you can ID the hero by MOVEMENT alone:
//   MOLE   — swings LOW and WIDE, plants hard and leans his whole mass INTO the blow (a body-blow bruiser).
//   VIPERA — snaps HIGH and fast, stays upright and springy (a head-snap striker), almost no commit-lean.
//   OMBRA  — clean and centred, the neutral baseline.
// Returned as: hbias = target-height offset (figure units, + = higher), amt_gain = reach multiplier,
// lean = how much the upper body drives forward with the swing (MOLE big, VIPERA small).
static inline void hero_punch_flavour(int kind, float *hbias, float *amt_gain, float *lean)
{
    if (kind == 1) {            // MOLE — low, weighty, heavy committed lean
        *hbias = -0.12f; *amt_gain = 0.92f; *lean = 1.55f;
    } else if (kind == 2) {     // VIPERA — high, fast snap, light lean (recovers instantly)
        *hbias = 0.14f;  *amt_gain = 1.10f; *lean = 0.55f;
    } else {                    // OMBRA — baseline
        *hbias = 0.0f;   *amt_gain = 1.0f;  *lean = 1.0f;
    }
}

// Per-hero KICK signature: MOLE = low planted stomp (short, heavy), VIPERA = high snap kick (longer,
// chambered up), OMBRA = a balanced mid front-kick. height = foot-rise gain, ext = forward-reach gain.
static inline void hero_kick_flavour(int kind, float *height, float *ext)
{
    if (kind == 1) {            // MOLE — low, planted stomp
        *height = 0.55f; *ext = 0.86f;
    } else if (kind == 2) {     // VIPERA — high, long snap kick
        *height = 1.45f; *ext = 1.12f;
    } else {                    // OMBRA — balanced
        *height = 1.0f;  *ext = 1.0f;
    }
}

// ------------------------------------------------------------------ pose composition
// Fill out[] for fr's current state. Start from a neutral stance, then layer the move that owns the
// frame. fxanim primitives OVERRIDE only the joints they touch, so e.g. an idle keeps the base legs.
// anim is 0..1 within the action; sinf(anim*PI) gives a 0->1->0 swing for strike windup+recovery.
//
// CAPCOM CLARITY: every action must read at a glance. Punches escalate across the combo (jab -> cross ->
// finisher), the finisher visibly COMMITS (deep reach, big lean, dropped guard), kicks fully EXTEND, and
// a block is an unmistakable compact tuck. Heroes stay BLACK; enemies pose the same — silhouette is king.
void fighter_pose(const Fighter *fr, Pt out[FX_NJ])
{
    fxanim::base(out);
    const float A   = fr->anim * BR_PI;        // attack swing phase: sin(A) = 0 -> 1 -> 0
    const float sw  = sinf(A);                 // 0..1..0 windup/recovery envelope
    const bool  hero = fr->is_hero;

    switch (fr->st) {
    case BS_IDLE: {
        // a fighting stance that breathes: fists up at the chin, a slight bounce in the knees
        float t = br_now_ms() * 0.001f + fr->x * 0.01f, b = 0.02f * sinf(t * 3.4f);
        out[fxanim::PELVIS].y += b; out[fxanim::NECK].y += b; out[fxanim::HEAD].y += b;
        out[fxanim::FHAND].x = 0.22f; out[fxanim::FHAND].y = 1.24f + b;
        out[fxanim::BHAND].x = 0.10f; out[fxanim::BHAND].y = 1.30f + b;
        fxanim::ik2(out[fxanim::NECK].x, out[fxanim::NECK].y, out[fxanim::FHAND].x, out[fxanim::FHAND].y, fxanim::ARM_L1, fxanim::ARM_L2,
                    -1.0f, -0.6f, &out[fxanim::FELBOW].x, &out[fxanim::FELBOW].y);
        fxanim::ik2(out[fxanim::NECK].x, out[fxanim::NECK].y, out[fxanim::BHAND].x, out[fxanim::BHAND].y, fxanim::ARM_L1, fxanim::ARM_L2,
                    -1.0f, -0.6f, &out[fxanim::BELBOW].x, &out[fxanim::BELBOW].y);
        fxanim::ik2(out[fxanim::PELVIS].x, out[fxanim::PELVIS].y, out[fxanim::FFOOT].x, out[fxanim::FFOOT].y, fxanim::LEG_L1, fxanim::LEG_L2,
                    1.0f, 0.0f, &out[fxanim::FKNEE].x, &out[fxanim::FKNEE].y);
        fxanim::ik2(out[fxanim::PELVIS].x, out[fxanim::PELVIS].y, out[fxanim::BFOOT].x, out[fxanim::BFOOT].y, fxanim::LEG_L1, fxanim::LEG_L2,
                    1.0f, 0.0f, &out[fxanim::BKNEE].x, &out[fxanim::BKNEE].y);
        break;
    }

    case BS_WALK:
        fxanim::walk(out, fr->walkphase, 1.0f);
        break;

    case BS_PUNCH: {
        // Combo step lives in fr->var. Read it as JAB / CROSS / ... / FINISHER so the chain ESCALATES.
        int combo_len = hero ? brawler_hero(fr->kind)->combo_len : 2;
        if (combo_len < 1) combo_len = 1;
        bool is_finisher = (fr->var >= combo_len - 1);

        float hbias = 0.0f, amt_gain = 1.0f, lean = 1.0f;
        if (hero) hero_punch_flavour(fr->kind, &hbias, &amt_gain, &lean);

        if (is_finisher) {
            // FINISHER — big committed lead-hand drive, strong forward body lean, guard DROPS. The heavy hit.
            float ext = sw * amt_gain;
            fxanim::reach(out, ext, 1.30f + hbias);
            // overdrive the lead hand past the standard reach so the commitment is unmistakable
            out[fxanim::FHAND].x += 0.16f * sw;
            // forward lean of the whole upper body — scaled per hero (MOLE throws his mass, VIPERA snaps).
            out[fxanim::NECK].x  += 0.10f * sw * lean;
            out[fxanim::HEAD].x  += 0.13f * sw * lean;
            out[fxanim::PELVIS].x += 0.06f * sw * lean;
            // MOLE plants the rear foot and drives off it — the unmistakable "heavy lean" read.
            if (fr->kind == 1) {
                out[fxanim::BFOOT].x -= 0.14f * sw;
                out[fxanim::PELVIS].y -= 0.05f * sw;   // sink into the hips on the drive
            }
            // drop the rear-hand guard — fighter throws everything into it
            out[fxanim::BHAND].x  += 0.10f * sw;
            out[fxanim::BHAND].y  -= 0.30f * sw;
            out[fxanim::BELBOW].y -= 0.16f * sw;
        } else if (fr->var == 0) {
            // JAB — quick, shallow, chest-height lead poke; body stays squared, rear hand guards high.
            float ext = (0.55f + 0.20f * sw) * sw * amt_gain;
            fxanim::reach(out, ext, 1.34f + hbias);
            out[fxanim::NECK].x += 0.04f * sw * lean;   // VIPERA barely leans; MOLE drives even a jab
            out[fxanim::HEAD].x += 0.05f * sw * lean;
            out[fxanim::BHAND].y = 1.30f;               // tight high guard while jabbing
        } else {
            // CROSS (var 1, and any mid-chain link) — deeper reach, more lean, rear hand cocks BACK first.
            float ext = (0.78f + 0.18f * sw) * amt_gain;
            fxanim::reach(out, ext, 1.32f + hbias);
            out[fxanim::NECK].x += 0.05f * sw * lean;
            out[fxanim::HEAD].x += 0.07f * sw * lean;
            out[fxanim::BHAND].x = -0.10f;     // rear hand pulled back (loaded cross)
            out[fxanim::BHAND].y = 1.20f;
        }
        break;
    }

    case BS_KICK: {
        // Strong, fully-extended front kick (IK chambers then snaps out). A touch deeper than a jab's commit.
        float ext = sw;
        if (ext < 0.0f) ext = 0.0f; else if (ext > 1.0f) ext = 1.0f;
        float amt = 0.30f + 0.70f * ext;
        fxanim::kick(out, amt);
        // Per-hero kick signature: reshape the foot target (height/reach) then re-solve the knee so the
        // leg articulates correctly. MOLE = low planted stomp, VIPERA = high snap kick, OMBRA = balanced.
        if (hero) {
            float kh = 1.0f, kx = 1.0f;
            hero_kick_flavour(fr->kind, &kh, &kx);
            // foot Y above the base stance (0.08 is the chamber floor in fxanim::kick)
            out[fxanim::FFOOT].y = 0.08f + (out[fxanim::FFOOT].y - 0.08f) * kh;
            out[fxanim::FFOOT].x = 0.16f + (out[fxanim::FFOOT].x - 0.16f) * kx;
            fxanim::ik2(out[fxanim::PELVIS].x, out[fxanim::PELVIS].y,
                        out[fxanim::FFOOT].x, out[fxanim::FFOOT].y,
                        fxanim::LEG_L1, fxanim::LEG_L2, 1.0f, 0.2f,
                        &out[fxanim::FKNEE].x, &out[fxanim::FKNEE].y);
        }
        break;
    }

    case BS_JUMP:
        fxanim::airborne(out, 1.0f);
        break;

    case BS_JKICK: {
        // Overhead jump-kick: tucked airborne body + a committed leg extension that drives downward.
        // Per-hero: VIPERA snaps a high flying kick, MOLE drives a heavier diagonal stomp, OMBRA neutral.
        fxanim::airborne(out, 0.55f);
        float ak = 0.85f;
        if (fr->kind == 2) ak = 0.98f;          // VIPERA — fuller, snappier extension
        else if (fr->kind == 1) ak = 0.72f;     // MOLE — shorter, heavier drive
        fxanim::kick(out, ak);
        break;
    }

    case BS_HIT:
        fxanim::recoil(out, sw);
        break;

    case BS_DOWN:
        fxanim::collapse(out, fr->anim < 1.0f ? fr->anim : 1.0f);          // stand -> lie
        break;

    case BS_RISE:
        fxanim::collapse(out, 1.0f - (fr->anim < 1.0f ? fr->anim : 1.0f)); // lie -> stand
        break;

    case BS_GRAB:
    case BS_THROW:
        fxanim::reach(out, 1.0f, 1.20f);   // both share a full extended grab reach
        break;

    case BS_BLOCK: {
        // BLOCK — unmistakable compact defensive tuck: arms up in front of the face, slight crouch and a
        // lean BACK (away from the incoming blow). A blocking foe must read instantly across the screen.
        fxanim::guard(out);
        // tighten the guard right up against the head, both hands forward and high
        out[fxanim::FHAND].x = 0.16f; out[fxanim::FHAND].y = 1.40f;
        out[fxanim::FELBOW].x = 0.14f; out[fxanim::FELBOW].y = 1.18f;
        out[fxanim::BHAND].x = 0.10f; out[fxanim::BHAND].y = 1.36f;
        out[fxanim::BELBOW].x = 0.06f; out[fxanim::BELBOW].y = 1.16f;
        // crouch: drop the hips/torso and lean the upper body back
        out[fxanim::PELVIS].y -= 0.08f;
        out[fxanim::NECK].x   -= 0.06f; out[fxanim::NECK].y -= 0.04f;
        out[fxanim::HEAD].x   -= 0.08f; out[fxanim::HEAD].y -= 0.04f;
        // settle the stance lower so the whole body looks braced
        fxanim::ik2(out[fxanim::PELVIS].x, out[fxanim::PELVIS].y, out[fxanim::FFOOT].x, out[fxanim::FFOOT].y,
                    fxanim::LEG_L1, fxanim::LEG_L2, 1.0f, 0.0f, &out[fxanim::FKNEE].x, &out[fxanim::FKNEE].y);
        fxanim::ik2(out[fxanim::PELVIS].x, out[fxanim::PELVIS].y, out[fxanim::BFOOT].x, out[fxanim::BFOOT].y,
                    fxanim::LEG_L1, fxanim::LEG_L2, 1.0f, 0.0f, &out[fxanim::BKNEE].x, &out[fxanim::BKNEE].y);
        break;
    }

    case BS_WIN:
        fxanim::guard(out);                // hands up, victory stance
        break;

    default:
        fxanim::idle(out, br_now_ms() * 0.001f);
        break;
    }
}

// ------------------------------------------------------------------ the look of every fighter
// Each fighter is a small costumed CHARACTER, not a stick: chunky tapered limbs with 3-tone shading (a
// darker back arm/leg for depth, a lit strip from the top-left on the near parts), a 1 px dark outline
// around every part (cartoon cel look: parts overlap with an ink line between them), a rim light in the
// stage's neon on the back edge, boots, fists, a head with hair and an eye, and per character the
// costume pieces that make the silhouette alone tell them apart:
//   OMBRA  hooded indigo coat with tails, light shirt V, belt          (the technician)
//   MOLE   big bruiser: white tank top, bare arms, hand wraps, red headband, jeans
//   VIPERA lithe teal bodysuit, long yellow scarf, auburn ponytail, white sneakers
//   thug   denim jacket, backwards red cap, grey jeans
//   brute  bald, brown leather vest over bare arms, olive cargo pants, gold buckle
//   blade  knife punk: pink mohawk, black tank, purple pants, a knife in the fist
//   boss   long purple coat, red shirt, gold chain, slick hair, black shades
// Colours are RGB332 bytes (what the 8bpp canvas shows), expanded once per draw.
#define C8(r, g, b) (uint8_t)((((r) * 7 + 127) / 255) << 5 | (((g) * 7 + 127) / 255) << 2 | (((b) * 3 + 127) / 255))
enum { LK_SKIN, LK_HAIR, LK_TOP, LK_ACC, LK_PANTS, LK_BOOTS, LK_BELT, LK_N };
enum { F_COAT = 1, F_HOOD = 2, F_BARE = 4, F_SCARF = 8, F_BAND = 16, F_TAIL = 32, F_MOHAWK = 64, F_SHADES = 128,
       F_KNIFE = 256, F_CAP = 512, F_BALD = 1024, F_CHAIN = 2048, F_WRAPS = 4096 };
struct Look { uint8_t c[LK_N]; uint16_t f; };
static const Look LOOKS[7] = {
    // skin               hair              top / coat          accent             pants             boots            belt
    { { C8(255,182,170), C8(36,36,85),    C8(73,73,170),   C8(219,219,255), C8(36,36,85),   C8(73,36,0),    C8(182,146,85) }, F_COAT | F_HOOD },
    { { C8(219,146,85),  C8(36,0,0),      C8(255,255,255), C8(255,36,0),    C8(36,73,170),  C8(109,36,0),   C8(36,36,0)    }, F_BARE | F_BAND | F_WRAPS },
    { { C8(255,182,170), C8(182,73,0),    C8(0,182,170),   C8(255,219,0),   C8(36,36,85),   C8(219,219,255),C8(0,109,85)   }, F_SCARF | F_TAIL },
    { { C8(219,146,85),  C8(36,36,0),     C8(36,73,170),   C8(219,36,0),    C8(109,109,85), C8(36,36,0),    C8(36,36,0)    }, F_CAP },
    { { C8(182,109,85),  C8(36,0,0),      C8(146,73,0),    C8(255,219,85),  C8(73,73,0),    C8(36,36,0),    C8(255,219,85) }, F_BARE | F_BALD },
    { { C8(255,182,170), C8(255,73,170),  C8(36,36,85),    C8(219,219,255), C8(109,36,85),  C8(36,36,0),    C8(182,182,170)}, F_BARE | F_MOHAWK | F_KNIFE },
    { { C8(182,109,85),  C8(0,0,0),       C8(109,36,170),  C8(255,73,85),   C8(36,36,85),   C8(36,36,0),    C8(255,219,0)  }, F_COAT | F_SHADES | F_CHAIN },
};
static uint16_t c565(uint8_t c) { return gui::rgb((c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 85); }
static const Look *look_of(const Fighter *fr)
{
    int k = fr->kind < 0 ? 0 : fr->kind;
    return &LOOKS[fr->is_hero ? (k > 2 ? 2 : k) : 3 + (k > 3 ? 3 : k)];
}
uint16_t brawler_hero_color(int kind)            // HUD name / select card: the hero's costume colour, lit
{
    Fighter f; f.is_hero = true; f.kind = kind;
    const Look *l = look_of(&f);
    return gui::mix(c565(l->c[kind == 1 ? LK_ACC : LK_TOP]), 0xFFFF, 120);
}

// The pen maps figure space (feet origin, +x = facing, +y = up) to the screen, with a squash on impact.
struct Pen { float sx, fy, kx, ky; int dir; };
#define PX(p, x) ((p).sx + (p).dir * (x) * (p).kx)
#define PY(p, y) ((p).fy - (y) * (p).ky)
static const uint16_t INK = 0x0001;              // black on the 8bpp canvas (0 would read as "no colour")

// One body part: a dark 1 px outline, the fill, and (near parts) a lit strip toward the top-left.
static void part(const Pen &p, float ax, float ay, float bx, float by, float wa, float wb, uint16_t col, uint16_t lit)
{
    float x0 = PX(p, ax), y0 = PY(p, ay), x1 = PX(p, bx), y1 = PY(p, by);
    wa *= p.kx; wb *= p.kx;
    inkbar(x0, y0, x1, y1, wa + 2.0f, wb + 2.0f, INK);
    inkbar(x0, y0, x1, y1, wa, wb, col);
    if (lit && wa > 3.0f) inkbar(x0 - wa * 0.2f, y0 - wa * 0.2f, x1 - wb * 0.2f, y1 - wb * 0.2f, wa * 0.3f, wb * 0.3f, lit);
}
static void ball(const Pen &p, float x, float y, float r, uint16_t col)
{
    int R = (int)(r * p.kx + 0.5f); if (R < 1) R = 1;
    d.fillCircle((int)PX(p, x), (int)PY(p, y), R + 1, INK);
    d.fillCircle((int)PX(p, x), (int)PY(p, y), R, col);
}

// The limbs, as data: joint a (+ offset) -> joint b (+ offset) in 1/100 figure units, widths in 1/100
// (x the fighter's girth when T_G), the colour slot and its tone. Back limbs are dark (they recede), near
// ones get the lit strip. S_ARM1/S_ARM2/S_FIST resolve per costume (bare arms, coat sleeves, hand wraps).
enum { S_ARM1 = LK_N, S_ARM2, S_FIST, S_N };
enum { T_DARK = 1, T_LIT = 2, T_G = 4, T_BALL = 8 };
struct Seg { uint8_t ja, jb; int8_t ax, ay, bx, by; uint8_t wa, wb, slot, fl; };
static const Seg LIMBS[] = {
    { FX_NECK,   FX_BELBOW, 0, -4, 0, 0,  13, 11, S_ARM1,   T_DARK | T_G },          // back arm
    { FX_BELBOW, FX_BHAND,  0, 0,  0, 0,  11, 10, S_ARM2,   T_DARK | T_G },
    { FX_BHAND,  FX_BHAND,  0, 0,  0, 0,   7,  0, S_FIST,   T_DARK | T_G | T_BALL },
    { FX_PELVIS, FX_BKNEE,  0, 0,  0, 0,  18, 14, LK_PANTS, T_DARK | T_G },          // back leg + boot
    { FX_BKNEE,  FX_BFOOT,  0, 0,  0, 6,  14, 11, LK_PANTS, T_DARK | T_G },
    { FX_BFOOT,  FX_BFOOT, -4, 4, 12, 3,  10,  8, LK_BOOTS, T_DARK },
    { FX_PELVIS, FX_FKNEE,  0, 0,  0, 0,  19, 15, LK_PANTS, T_LIT | T_G },           // front leg + boot
    { FX_FKNEE,  FX_FFOOT,  0, 0,  0, 6,  15, 12, LK_PANTS, T_LIT | T_G },
    { FX_FFOOT,  FX_FFOOT, -4, 4, 13, 3,  11,  9, LK_BOOTS, T_LIT },
    { FX_NECK,   FX_FELBOW, 0, -4, 0, 0,  14, 12, S_ARM1,   T_LIT | T_G },           // front arm (drawn last)
    { FX_FELBOW, FX_FHAND,  0, 0,  0, 0,  12, 10, S_ARM2,   T_G },
};
static void limbs(const Pen &p, const Pt *j, int from, int to, const uint16_t *col, const uint16_t *dk, const uint16_t *lt, float gth)
{
    for (int i = from; i < to; i++) {
        const Seg &s = LIMBS[i];
        float k = (s.fl & T_G) ? gth * 0.01f : 0.01f;
        float ax = j[s.ja].x + s.ax * 0.01f, ay = j[s.ja].y + s.ay * 0.01f;
        uint16_t c = (s.fl & T_DARK) ? dk[s.slot] : col[s.slot];
        if (s.fl & T_BALL) { ball(p, ax, ay, s.wa * k, c); continue; }
        part(p, ax, ay, j[s.jb].x + s.bx * 0.01f, j[s.jb].y + s.by * 0.01f, s.wa * k, s.wb * k, c, (s.fl & T_LIT) ? lt[s.slot] : 0);
    }
}

// Ink fighter fr at screen (sx, feetY), scale sc. `shade` 0..256 grades every colour toward `tint` (lane
// depth / stage haze, a dimmed card); `rim` is the stage's neon for the rim light (0 = none).
void br_figure(const Fighter *fr, float sx, float feetY, float sc, int shade, uint16_t tint, uint16_t rim)
{
    Pt j[FX_NJ];
    fighter_pose(fr, j);
    const Look *L = look_of(fr);
    float gth = fr->is_hero ? brawler_hero(fr->kind)->girth : brawler_enemy(fr->kind)->girth;
    // colours: base / dark (back parts) / lit, graded toward the tint; a fresh hit flashes pure white
    uint16_t col[S_N], dk[S_N], lt[S_N];
    bool white = fr->flash > 0.88f, bare = L->f & F_BARE, coat = L->f & F_COAT;
    for (int i = 0; i < S_N; i++) {
        int src = i < LK_N ? i : i == S_ARM1 ? (bare ? LK_SKIN : LK_TOP) : i == S_ARM2 ? (coat ? LK_TOP : LK_SKIN) : LK_SKIN;
        uint16_t c = white ? 0xFFFF : gui::mix(c565(L->c[src]), tint, shade);
        if (!white && fr->flash > 0.0f) c = gui::mix(c, 0xFFFF, (int)(fr->flash * 150.0f));
        if (i == S_FIST && (L->f & F_WRAPS)) c = gui::mix(c, 0xFFFF, 150);              // hand wraps
        col[i] = c; dk[i] = gui::mix(c, INK, 95); lt[i] = gui::mix(c, 0xFFFF, 90);
    }
    if (white) rim = 0;
    Pen p = { sx, feetY, sc, sc, fr->dir };
    if (fr->st == BS_HIT && fr->anim < 0.35f) { p.kx = sc * 1.12f; p.ky = sc * 0.88f; }   // squash on impact
    if (fr->st == BS_DOWN && fr->anim > 0.55f && fr->anim < 0.85f)                       // the body bounces once
        p.fy -= sinf((fr->anim - 0.55f) / 0.30f * 3.14159f) * 0.10f * sc;
    float t = br_now_ms() * 0.001f;
    float wave = sinf(t * 7.0f + fr->x * 0.05f) * 0.03f + (fr->st == BS_WALK ? 0.03f : 0.0f);
    float hx = j[FX_HEAD].x, hy = j[FX_HEAD].y, hr = 0.15f * (0.85f + 0.15f * gth);
    const Pt &P = j[FX_PELVIS], &N = j[FX_NECK];

    // behind everything: the scarf / ponytail streaming back
    if (L->f & F_SCARF) part(p, N.x - 0.02f, N.y - 0.02f, N.x - 0.38f, N.y - 0.14f + wave * 3, 0.10f, 0.06f, col[LK_ACC], lt[LK_ACC]);
    if (L->f & F_TAIL)  part(p, hx - 0.10f, hy + 0.04f, hx - 0.30f, hy - 0.10f + wave * 2, 0.09f, 0.04f, col[LK_HAIR], 0);
    limbs(p, j, 0, 9, col, dk, lt, gth);                               // back arm, back leg, front leg
    // a coat hangs over the hips, its tails swinging
    if (coat) part(p, N.x, N.y - 0.04f, P.x - 0.06f - wave, P.y - 0.38f, 0.36f * gth, 0.44f * gth, col[LK_TOP], 0);
    // the torso: pelvis -> chest -> shoulders, then the costume details on it
    part(p, P.x, P.y + 0.02f, N.x, N.y - 0.03f, 0.29f * gth, 0.38f * gth, col[LK_TOP], lt[LK_TOP]);
    if (coat) part(p, N.x + 0.03f, N.y - 0.05f, N.x + 0.07f, N.y - 0.26f, 0.07f, 0.03f, col[LK_ACC], 0);     // shirt V
    if (!fr->is_hero && fr->kind == 1) part(p, N.x, N.y - 0.06f, P.x + 0.01f, P.y + 0.12f, 0.10f, 0.08f, col[LK_SKIN], 0);   // the vest is open
    part(p, P.x - 0.12f * gth, P.y + 0.06f, P.x + 0.12f * gth, P.y + 0.06f, 0.06f, 0.06f, col[LK_BELT], 0);    // belt
    if (L->f & F_CHAIN) part(p, N.x - 0.06f, N.y - 0.08f, N.x + 0.08f, N.y - 0.12f, 0.025f, 0.025f, col[LK_BELT], 0);
    if (rim) {                                                         // stage-neon rim light on the back edge
        float rx = -0.15f * gth;
        inkbar(PX(p, P.x + rx), PY(p, P.y + 0.08f), PX(p, N.x + rx), PY(p, N.y - 0.06f), 1.0f, 1.0f, rim);
    }
    // neck + head: skin, hair / hood / cap, an eye, the head gear
    part(p, N.x, N.y - 0.04f, hx, hy, 0.09f, 0.08f, col[LK_SKIN], 0);
    if (L->f & F_HOOD) {
        ball(p, hx - 0.03f, hy + 0.02f, hr * 1.28f, col[LK_TOP]);
        ball(p, hx + 0.05f, hy - 0.01f, hr * 0.74f, col[LK_SKIN]);
        part(p, hx - 0.02f, hy + hr * 1.1f, hx + 0.10f, hy + hr * 0.75f, 0.05f, 0.03f, dk[LK_TOP], 0);          // the hood's peak
    } else {
        ball(p, hx, hy, hr, col[LK_SKIN]);
        if (!(L->f & (F_BALD | F_MOHAWK))) {                           // hair over the back and the top
            uint16_t hc = (L->f & F_CAP) ? col[LK_ACC] : col[LK_HAIR];
            d.fillCircle((int)PX(p, hx - 0.04f), (int)PY(p, hy + 0.05f), (int)(hr * 0.92f * p.kx + 0.5f), hc);
            if (L->f & F_CAP) part(p, hx - 0.08f, hy + 0.08f, hx - 0.22f, hy + 0.05f, 0.05f, 0.04f, col[LK_ACC], 0);   // brim, backwards
            d.fillCircle((int)PX(p, hx + 0.05f), (int)PY(p, hy - 0.02f), (int)(hr * 0.66f * p.kx + 0.5f), col[LK_SKIN]);   // the face
        }
        if (L->f & F_MOHAWK) part(p, hx + 0.06f, hy + hr * 0.9f, hx - 0.10f, hy + hr * 1.1f, 0.10f, 0.06f, col[LK_HAIR], lt[LK_HAIR]);
        if (L->f & F_BALD) d.drawPixel((int)PX(p, hx - 0.03f), (int)PY(p, hy + hr * 0.6f), lt[LK_SKIN]);   // a shine
        if (L->f & F_BAND) {
            part(p, hx - hr, hy + 0.03f, hx + hr * 0.9f, hy + 0.04f, 0.045f, 0.045f, col[LK_ACC], 0);
            part(p, hx - hr, hy + 0.03f, hx - hr - 0.12f, hy - 0.04f + wave, 0.04f, 0.025f, col[LK_ACC], 0);   // the knot's tails
        }
    }
    if (L->f & F_SHADES) part(p, hx + 0.02f, hy + 0.01f, hx + hr * 1.05f, hy + 0.01f, 0.05f, 0.05f, INK, 0);
    else d.fillRect((int)PX(p, hx + hr * 0.55f), (int)PY(p, hy + 0.02f), 1, 2, INK);   // an eye
    if (L->f & F_SCARF) ball(p, N.x + 0.04f, N.y - 0.02f, 0.06f, col[LK_ACC]);           // the scarf's knot
    // the front arm on top, with a smear behind a punch in its active window, and the knife
    const Pt &E = j[FX_FELBOW], &Hd = j[FX_FHAND];
    if ((fr->st == BS_PUNCH || fr->st == BS_JKICK) && fr->anim > 0.22f && fr->anim < 0.6f) {
        uint16_t sm = gui::mix(lt[LK_TOP], tint, 60);
        for (int k = -1; k <= 1; k += 2) inkbar(PX(p, N.x + 0.05f), PY(p, Hd.y + k * 0.05f), PX(p, Hd.x - 0.06f), PY(p, Hd.y + k * 0.03f), 1.0f, 1.0f, sm);
    }
    if (fr->st == BS_KICK && fr->anim > 0.22f && fr->anim < 0.6f) {
        const Pt &F = j[FX_FFOOT];
        inkbar(PX(p, P.x + 0.05f), PY(p, F.y - 0.06f), PX(p, F.x - 0.05f), PY(p, F.y - 0.06f), 1.0f, 1.0f, lt[LK_PANTS]);
    }
    limbs(p, j, 9, 11, col, dk, lt, gth);
    if (L->f & F_KNIFE) {
        float ux = Hd.x - E.x, uy = Hd.y - E.y, l = sqrtf(ux * ux + uy * uy) + 0.001f;
        part(p, Hd.x, Hd.y, Hd.x + ux / l * 0.22f, Hd.y + uy / l * 0.22f, 0.04f, 0.015f, col[LK_ACC], 0);
    }
    ball(p, Hd.x, Hd.y, 0.075f * gth, col[S_FIST]);
}
