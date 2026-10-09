// brawler_fx.cpp — SCORRIBANDA: blood spray, impact sparks + ground pools.
//
// Built on fxfx (Field = ballistic particles, PoolRing = ground decals). Droplets fly under gravity
// (BR_GRAV), settle on a per-particle floor (the belt feet-y for the victim's depth), and each landing
// leaves a dark pool. One heap block while the game is open (see BrFx), nothing static.
//
// Draw order, set by the shell: pools BEHIND the fighters (ground decals), drops IN FRONT (airborne).
// Both fold camera scroll + shake into their x offset via -g.camx + g.shake.ox(), so positions stored
// in WORLD-x stay camera-independent — same convention the rest of the fx toolkit uses.

#include "brawler.h"
#include <math.h>
#include <stdlib.h>

// Palette: the only loud colour in the game.
static const uint16_t BRFX_RED      = BR_BLOOD;                       // fresh airborne blood (palette source)
static const uint16_t BRFX_POOLDARK = fx3d::scl(BR_BLOOD, 155, 255);  // congealed ground pool — clearly red, a touch deep

// Particle stores: blood droplets, impact sparks (bright pips with little gravity that die by life only —
// they never land: landY parked off-screen) and the ground pools, all in ONE heap block allocated on app
// enter (brfx_reset) and freed on exit (brfx_shutdown): zero static RAM while the game is closed. Both
// fields share one size so the particle code is compiled once. Null-guarded: a failed alloc = no fx.
struct BrFx { fxfx::Field<60> drops, sparks; fxfx::PoolRing<28> pools; };
static BrFx *s_fx = nullptr;
static const float SPARK_GRAV = 90.0f;   // a faint pull so the burst arcs slightly, far below BR_GRAV

// Landing callback: a settled droplet becomes a small dark pool on the floor. Sizes jitter so pools
// read organic rather than stamped. Fires from Field::step when a falling particle reaches landY.
static void onLand(float x, float y)
{
    s_fx->pools.add(x, y, 3 + (int)(br_frnd() * 3), 1 + (int)(br_frnd() * 2), BRFX_POOLDARK);
}

// Clear every store — called on app enter and on each level (re)start so blood/sparks don't carry across
// scenes. Doubles as the heap-alloc site (idempotent: a re-entry that already holds the block just clears).
void brfx_reset(void)
{
    if (!s_fx) s_fx = (BrFx *)calloc(1, sizeof *s_fx);
    if (!s_fx) return;
    s_fx->drops.clear(); s_fx->sparks.clear(); s_fx->pools.clear();
}

// Free the heap block — called on app exit so it holds ZERO RAM while the game is closed.
void brfx_shutdown(void)
{
    free(s_fx); s_fx = nullptr;
}

// Impact spark: a brief radial burst of small bright pips at (worldx, screeny). `col` is the spark hue
// (white for a clean connect, a duller grey for a blocked hit per the combat module); `n` scales with
// the move's weight (bigger for finishers/kicks). Short life + a faint gravity = a quick crisp flash,
// not a lingering shower. Parks landY off-screen so step() never converts a spark into a ground pool.
void brfx_spark(float worldx, float screeny, uint16_t col, int n)
{
    if (!s_fx) return;
    if (n < 1) n = 1; else if (n > 16) n = 16;
    const float parkY = screeny + 4000.0f;        // never reached -> dies by life, leaves no decal
    for (int i = 0; i < n; i++) {
        float ang = br_frnd() * 6.2831853f;        // full radial spread
        float spd = 60.0f + br_frnd() * 110.0f;    // snappy outward speed
        float vx  = cosf(ang) * spd;
        float vy  = sinf(ang) * spd - 30.0f;       // slight upward bias so the burst lifts off the hit
        float life = 0.10f + br_frnd() * 0.12f;    // short: 0.10..0.22s
        s_fx->sparks.spawn(worldx, screeny, vx, vy, life, parkY, col);
    }
}

// Emit a directional blood burst from (worldx, screeny) biased by `dir` (the attacker's facing).
// `amount` droplets; landY = the belt feet-y where they'll settle (caller passes scene_floor_y(z)).
// Field::spray is a template over two RNG callables (0..1 and -1..1); we hand it the shared br_frnd /
// br_frnd2 via tiny lambdas so the spread stays deterministic-friendly and owns no RNG state.
void brfx_blood(float worldx, float screeny, int dir, int amount, float landY)
{
    if (!s_fx) return;
    s_fx->drops.spray(worldx, screeny, dir, amount, 130.0f, landY, BRFX_RED,
                   [] { return br_frnd(); },
                   [] { return br_frnd2(); });
}

// Advance droplets under gravity (settled ones spawn pools via onLand) and sparks under a faint pull
// (no land callback -> they vanish when their short life runs out, leaving the floor clean).
void brfx_step(float dt)
{
    if (!s_fx) return;
    s_fx->drops.step(dt, BR_GRAV, &onLand);
    s_fx->sparks.step(dt, SPARK_GRAV, NULL);
}

// Ground decals — drawn behind the fighters. Camera + shake folded into the x offset.
void brfx_draw_pools(void)
{
    if (s_fx) s_fx->pools.draw(-g.camx + g.shake.ox());
}

// Airborne droplets + impact sparks — drawn in front of the fighters. Sparks paint last so the bright
// pip sits on top of the body it just struck. Both fold camera scroll + shake into the x offset.
void brfx_draw_drops(void)
{
    float xoff = -g.camx + g.shake.ox();
    if (!s_fx) return;
    s_fx->drops.draw(xoff);
    s_fx->sparks.draw(xoff);
}
