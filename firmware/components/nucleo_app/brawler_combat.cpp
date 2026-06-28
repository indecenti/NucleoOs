// brawler_combat.cpp — SCORRIBANDA combat core: physics, hitboxes, damage, knockback, combos, KO.
//
// The fight kernel. Integrates every fighter each tick (jump arc, knockback drift, depth/x clamps,
// action animation + state recovery, hit-flash decay), opens attacks (punch/kick/jump-kick, hero combo
// chaining), and resolves active-frame hitboxes into damage + red blood. Calls other modules only
// through brawler.h. HEAP-FREE: nothing but the shared `g` and fixed arrays.
#include "brawler.h"
#include <math.h>

// --------------------------------------------------------------------- helpers (local only)
static inline float clampf_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// True while a fighter is locked in an action (mid-attack / hurt / floored / recovering / grappling /
// guarding). A guard (BS_BLOCK) is a committed state: you can't move or strike until it drops.
bool fighter_busy(const Fighter *fr)
{
    switch (fr->st) {
        case BS_PUNCH: case BS_KICK: case BS_JKICK:
        case BS_HIT:   case BS_DOWN: case BS_RISE:
        case BS_GRAB:  case BS_THROW: case BS_BLOCK:
            return true;
        default:
            return false;
    }
}

// Kick off a strike (or raise a guard). Sets the action state, resets anim/hit window, picks the swing
// speed by move; for a hero punch it walks the combo chain (var wraps by combo_len) so repeated J reads
// as a 1-2-3 string — var 0 = jab, var = combo_len-1 = FINISHER, the in-betweens are crosses.
void combat_begin_attack(Fighter *fr, BrState kind)
{
    bool link = (fr->st == BS_PUNCH);   // already mid-punch -> this tap CHAINS the string; else it's fresh
    fr->st       = kind;
    fr->anim     = 0.0f;
    fr->hit_done = false;
    fr->aspd     = (kind == BS_PUNCH) ? 1.0f / 0.26f
                 : (kind == BS_KICK)  ? 1.0f / 0.34f
                 : (kind == BS_BLOCK) ? 1.0f / 0.45f    // a guard held briefly, then dropped
                                      : 1.0f / 0.30f;   // jump-kick
    // Hero punch combo: a FRESH string opens on the JAB (var 0); only a genuine chain-link advances the
    // step (var 1 = cross ... var == combo_len-1 = FINISHER). Pre-incrementing skipped the jab and made
    // MOLE's first tap a knockdown finisher.
    if (fr->is_hero && kind == BS_PUNCH) {
        int len = brawler_hero(fr->kind)->combo_len;
        if (len < 1) len = 1;
        fr->var = link ? (fr->var + 1) % len : 0;
    }
    // A raised guard isn't a swing — no whiff cue.
    if (kind != BS_BLOCK) bsfx(BSFX_WHIFF);
}

// Apply a connected hit to `victim` from `src`: damage, knockback, flash, state, a red blood spray at
// chest height, plus hitstop + screen-shake + the right impact cue.
void fighter_take_hit(Fighter *victim, Fighter *src, int dmg, float knock)
{
    victim->hp      -= dmg;
    victim->vx      += src->dir * knock;
    victim->flash    = 1.0f;
    victim->st       = (victim->hp <= 0) ? BS_DOWN : BS_HIT;
    victim->anim     = 0.0f;
    victim->aspd     = 1.0f / 0.30f;
    victim->hit_done = true;
    if (victim->is_hero && victim->hp <= 0) victim->cool = 0.7f;   // KO lie-time; the shell resolves life/respawn

    // Blood: spray from the chest, settling on the belt under the victim.
    float fy    = scene_floor_y(victim->z);
    float chest = fy - 1.10f * scene_scale(victim->z);
    brfx_blood(victim->x, chest, src->dir, (victim->hp <= 0 ? 14 : 6) + dmg / 3, fy);

    g.hitstop = (victim->hp <= 0) ? 0.09f : 0.05f;
    g.shake.add(victim->hp <= 0 ? 5.0f : 3.0f);
    bsfx(victim->hp <= 0 ? BSFX_KO : (victim->is_hero ? BSFX_HURT : BSFX_HIT));
}

// Integrate every live fighter: jumps, knockback drift + damping, depth/x clamps, action animation with
// state recovery (attacks end, the hurt fall down or shake it off, the downed rise or die), flash decay.
void combat_step(float dt)
{
    for (int i = 0; i < BR_MAXF; i++) {
        Fighter *fr = &g.f[i];
        if (!fr->on) continue;

        // --- jump arc ---
        if (fr->yoff > 0.0f || fr->vy != 0.0f) {
            fr->yoff += fr->vy * dt;
            fr->vy   -= BR_GRAV * dt;
            if (fr->yoff <= 0.0f) {
                fr->yoff = 0.0f;
                fr->vy   = 0.0f;
                if (fr->st == BS_JUMP || fr->st == BS_JKICK) fr->st = BS_IDLE;
            }
        }

        // --- knockback / drift ---
        fr->x += fr->vx * dt;
        fr->z += fr->vz * dt;
        fr->vx *= expf(-9.0f * dt);

        // --- clamps ---
        fr->z = clampf_(fr->z, 0.18f, 1.0f);
        if (fr->is_hero) fr->x = clampf_(fr->x, g.camx + 10.0f, g.gatex - 10.0f);
        else             fr->x = clampf_(fr->x, 0.0f, g.level_len);

        // --- action animation + recovery (only the timed action states; JUMP recovers via the arc) ---
        switch (fr->st) {
            case BS_PUNCH: case BS_KICK: case BS_JKICK:
            case BS_HIT:   case BS_DOWN: case BS_RISE: case BS_BLOCK: {
                fr->anim += fr->aspd * dt;
                if (fr->anim >= 1.0f) {
                    fr->anim = 1.0f;
                    switch (fr->st) {
                        case BS_PUNCH: case BS_KICK:
                            fr->st = BS_IDLE;                 // moving-state unknown here -> idle
                            break;
                        case BS_JKICK:
                            // landing is the jump arc's job; if still airborne stay a plain jump
                            fr->st = (fr->yoff > 0.0f) ? BS_JUMP : BS_IDLE;
                            break;
                        case BS_HIT:
                            if (fr->hp <= 0) { fr->st = BS_DOWN; fr->anim = 0.0f; fr->aspd = 1.0f / 0.30f; }
                            else             { fr->st = BS_IDLE; }
                            break;
                        case BS_DOWN:
                            // Knockdown resolution. A LIVE fighter (knocked down by a finisher/kick with
                            // hp still > 0) must get back up; only a dead enemy despawns + scores; a KO'd
                            // hero stays floored for the shell's life system.
                            if (fr->hp > 0) {
                                fr->st = BS_RISE; fr->anim = 0.0f; fr->aspd = 1.0f / 0.50f;
                            } else if (fr->is_hero) {
                                fr->anim = 1.0f;                 // KO'd hero: stay down, the shell respawns/ends
                            } else {
                                fr->on = false; g.score += brawler_enemy(fr->kind)->score;  // dead enemy: clear + score
                            }
                            break;
                        case BS_RISE:
                            fr->st = BS_IDLE;
                            break;
                        case BS_BLOCK:
                            fr->st = BS_IDLE;                 // guard drops -> back to neutral
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            default:
                break;   // IDLE / WALK / JUMP / GRAB / THROW / WIN: not advanced by combat
        }

        // --- hit-flash decay ---
        fr->flash -= dt * 3.0f;
        if (fr->flash < 0.0f) fr->flash = 0.0f;
    }
}

// Per-move profile: damage scale, knockback, whether it KNOCKS DOWN, and the feedback weight (0 light ..
// 1 heavy) that drives hitstop/shake/spark size. Resolved from the attacker's move + (for hero punches)
// the combo step: jab (var 0) light, cross (mid links) medium, FINISHER (last var) heavy + knockdown;
// kick / jump-kick are heavy + knockdown by design.
struct MoveProfile { int dmg; float knock; bool knockdown; float weight; };

static MoveProfile move_profile(const Fighter *att)
{
    MoveProfile mp = { 1, 95.0f, false, 0.0f };

    if (att->is_hero) {
        const HeroDef *h = brawler_hero(att->kind);
        int len = h->combo_len; if (len < 1) len = 1;
        if (att->st == BS_KICK || att->st == BS_JKICK) {
            // Strong single kick / overhead jump-kick: heavier, long knockback, floors the target.
            mp.dmg       = h->kdmg;
            mp.knock     = (att->st == BS_JKICK) ? 200.0f : 175.0f;
            mp.knockdown = true;
            mp.weight    = 1.0f;
        } else if (att->var >= len - 1) {
            // FINISHER (last link of the string): heaviest punch, large push, KNOCKDOWN.
            mp.dmg       = (h->pdmg * 9) / 5;     // ~1.8x the jab
            mp.knock     = 165.0f;
            mp.knockdown = true;
            mp.weight    = 0.95f;
        } else if (att->var == 0) {
            // Jab: fast, light, small push, brief stun.
            mp.dmg    = h->pdmg;
            mp.knock  = 55.0f;
            mp.weight = 0.0f;
        } else {
            // Cross / mid links: medium dmg + push, brief stun.
            mp.dmg    = (h->pdmg * 13) / 10;      // ~1.3x the jab
            mp.knock  = 100.0f;
            mp.weight = 0.4f;
        }
    } else {
        // Enemies throw a single committed blow (their AI only opens BS_PUNCH); difficulty scales dmg.
        float k = (g.diff == 0) ? 0.7f : (g.diff == 2 ? 1.3f : 1.0f);
        mp.dmg       = (int)(brawler_enemy(att->kind)->dmg * k);
        mp.knock     = (att->st == BS_KICK) ? 150.0f : 90.0f;
        mp.knockdown = (att->st == BS_KICK || att->st == BS_JKICK);
        mp.weight    = (att->st == BS_KICK) ? 0.9f : 0.35f;
    }
    if (mp.dmg < 1) mp.dmg = 1;
    return mp;
}

// True if `vic` is guarding (BS_BLOCK) and roughly facing `att` (the strike comes from the front).
static bool blocking_against(const Fighter *vic, const Fighter *att)
{
    if (vic->st != BS_BLOCK) return false;
    return (att->x - vic->x) * (float)vic->dir > 0.0f;   // attacker on the side the victim faces
}

// Active-frame hitbox checks: for each attacker in its strike window, test every opposing-team victim in
// reach + depth band and land the first one. Branches by reaction: a guard turns the hit into chip-only,
// a finisher/kick knocks the victim DOWN, everything else applies a brief stun. Every connect throws an
// impact spark + weight-scaled hitstop/shake; heroes feed the combo + score juice on a clean hit.
void combat_resolve(float dt)
{
    (void)dt;
    for (int a = 0; a < BR_MAXF; a++) {
        Fighter *att = &g.f[a];
        if (!att->on || att->hit_done) continue;
        if (att->st != BS_PUNCH && att->st != BS_KICK && att->st != BS_JKICK) continue;
        if (att->anim < 0.28f || att->anim > 0.52f) continue;   // active-frame window

        float reach = (att->is_hero ? brawler_hero(att->kind)->reach
                                    : brawler_enemy(att->kind)->reach)
                    * (0.7f + 0.5f * scene_scale(att->z) / 34.0f);

        MoveProfile mp = move_profile(att);

        for (int v = 0; v < BR_MAXF; v++) {
            Fighter *vic = &g.f[v];
            if (vic == att || !vic->on) continue;
            if (vic->is_hero == att->is_hero) continue;   // only the opposite team
            if (vic->st == BS_DOWN) continue;             // can't hit a floored body

            float dx = (vic->x - att->x) * att->dir;      // ahead, in facing direction
            float dz = fabsf(vic->z - att->z);
            if (dx < 4.0f || dx > reach || dz >= 0.13f) continue;

            // Impact point: roughly chest height of the victim, in WORLD-x (the fx layer folds camera).
            float ix = vic->x;
            float iy = scene_floor_y(vic->z) - 1.10f * scene_scale(vic->z);

            if (blocking_against(vic, att)) {
                // BLOCKED: chip damage only, no stun, a tiny push, a duller (non-white) spark. The guard
                // is NOT broken (it keeps timing out in combat_step), so the victim's state is untouched.
                int chip = mp.dmg / 4; if (chip < 1) chip = 1;
                vic->hp -= chip;
                vic->vx += att->dir * 30.0f;
                vic->flash = 0.35f;                       // a faint pop, less than a clean hit
                brfx_spark(ix, iy, BR_GREY_NEAR, 3);      // dull grey sparks read as "parried", not blood
                g.hitstop = 0.025f;
                g.shake.add(1.5f);
                bsfx(BSFX_WHIFF);                         // a guard "thud", not the meaty hit cue
                if (vic->hp <= 0) {                       // chip finished a guard-down foe: floor it cleanly
                    vic->st   = BS_DOWN;
                    vic->anim = 0.0f;
                    vic->aspd = 1.0f / 0.30f;
                    if (vic->is_hero) vic->cool = 0.7f;   // hand a KO'd hero to the shell's life system
                }
            } else {
                // CLEAN HIT: damage + knockback + flash + blood (fighter_take_hit owns those), then we
                // refine the reaction. fighter_take_hit set BS_HIT (or BS_DOWN if it killed); for a
                // knockdown move on a SURVIVOR we override to BS_DOWN so they get floored, then rise.
                fighter_take_hit(vic, att, mp.dmg, mp.knock);
                if (mp.knockdown && vic->hp > 0) {
                    vic->st   = BS_DOWN;
                    vic->anim = 0.0f;
                    vic->aspd = 1.0f / 0.30f;
                }
                // Weight-scaled juice (overrides fighter_take_hit's defaults so each move READS apart).
                int   spn = 4 + (int)(mp.weight * 8.0f);  // 4 (jab) .. ~12 (finisher/kick)
                g.hitstop = 0.03f + mp.weight * 0.06f;    // jab ~0.03s .. heavy ~0.09s
                g.shake.add(2.5f + mp.weight * 4.0f);     // light tap .. solid jolt
                brfx_spark(ix, iy, 0xFFFF, spn);          // bright WHITE impact stamp

                if (att->is_hero) {
                    g.combo++;
                    g.combo_t = 1.2f;
                    g.score  += (long)mp.dmg * 10;
                }
            }
            att->hit_done = true;
            break;   // one hit per swing
        }
    }
}
