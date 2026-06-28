// brawler_chars.cpp — SCORRIBANDA: the 3 playable heroes + per-fighter pose composition.
//
// Owns the hero roster (stats + silhouette flavour) and turns a Fighter's current state/anim into the
// 11-joint pose the renderer inks. Poses are authored facing dir=+1; the draw loop mirrors by dir.
// HEAP-FREE: a single const table + stack-only pose math. Draws nothing (no `d` access) — it only
// composes fxanim primitives and hands the joints back.
#include "brawler.h"
#include <math.h>

#ifndef BR_PI
#define BR_PI 3.14159265f
#endif

// ------------------------------------------------------------------ hero roster
// Three archetypes, one silhouette language: same near-black BR_INK body, but a DISTINCT BUILD so they
// read apart by shape alone — scale = overall HEIGHT, girth = WIDTH/limb thickness (and the head tracks
// girth in the renderer, so a burly fighter is also broad-headed). Reach, speed and combo length give
// each a different feel in the hand. On the white-paper look heroes are pure black silhouettes and need
// NO outline — rim is 0 for all (the renderer treats 0 as "no outline"). They read apart by build.
//
//   OMBRA  — the technician: average height/build, fast 3-hit string, balanced everything. The baseline.
//   MOLE   — the bruiser:    tall AND wide (scale 1.14, girth 1.44), slow, only a 2-hit string but each
//                            blow is heavy; short reach (works inside). A walking wall.
//   VIPERA — the striker:    short and lean (scale 0.92, girth 0.80), very fast, long reach, a 4-hit
//                            flurry of light snaps. Fragile but slippery.
static const HeroDef HEROES[3] = {
    // name      style_it                     style_en                     maxhp speed  pdmg kdmg reach girth rim combo scale
    { "OMBRA",  "Equilibrato", "Balanced", 100,  78.f,   7,  10, 30.f, 1.00f, 0, 3, 1.00f },
    { "MOLE",   "Devastante",  "Heavy",    150,  56.f,  12,  16, 26.f, 1.44f, 0, 2, 1.14f },
    { "VIPERA", "Velocissima", "Fast",      80, 100.f,   5,   8, 38.f, 0.80f, 0, 4, 0.92f },
};

int brawler_hero_count(void) { return 3; }

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
void fighter_pose(const Fighter *fr, fxfig::Pt out[fxfig::FX_NJ])
{
    fxanim::base(out);
    const float A   = fr->anim * BR_PI;        // attack swing phase: sin(A) = 0 -> 1 -> 0
    const float sw  = sinf(A);                 // 0..1..0 windup/recovery envelope
    const bool  hero = fr->is_hero;

    switch (fr->st) {
    case BS_IDLE:
        fxanim::idle(out, br_now_ms() * 0.001f);
        break;

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

// ------------------------------------------------------------------ per-type enemy hue
// One distinct hue PER enemy TYPE so foes read apart at a glance (the user's complaint: all the same
// blue). These are the FROZEN BR_EN_* palette from brawler.h — never the legacy steel-blue roster body,
// never red (that's blood). kind: 0 thug, 1 brute, 2 blade, 3 boss.
static inline uint16_t enemy_hue(int kind)
{
    switch (kind) {
    case 0:  return BR_EN_THUG;    // steel blue
    case 1:  return BR_EN_BRUTE;   // ochre / khaki
    case 2:  return BR_EN_BLADE;   // teal green
    case 3:  return BR_EN_BOSS;    // purple
    default: return BR_EN_THUG;
    }
}

// ------------------------------------------------------------------ silhouette colour
// Body ink for fr: heroes are pure BR_INK (near-black on white paper); enemies use their PER-TYPE hue so
// the roster reads apart at a glance. A non-zero flash folds in a white hit-pop (0..1 -> up to ~86% toward
// white) so a connect reads as a bright stamp on the frame. Palette-strict: only BR_INK / a BR_EN_* hue /
// white ever appear here.
uint16_t fighter_body(const Fighter *fr)
{
    uint16_t base = fr->is_hero ? BR_INK : enemy_hue(fr->kind);
    if (fr->flash > 0.0f) {
        int t = (int)(fr->flash * 220.0f);
        return fx3d::mix(base, 0xFFFF, t);
    }
    return base;
}
