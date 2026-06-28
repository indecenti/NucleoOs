// brawler_fx.cpp — SCORRIBANDA: blood spray + ground pools (the noir's one bright colour).
//
// Built on fxfx (Field = ballistic droplets, PoolRing = ground decals). RED is the only saturated
// hue in the whole game; everything else is black-on-grey, so a hit literally splashes colour onto
// the scene. Droplets fly under gravity (BR_GRAV), settle on a per-particle floor (the belt feet-y
// for the victim's depth), and each landing leaves a dark pool. HEAP-FREE: two fixed static pools.
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

// Particle stores. The two Fields (droplets + sparks, ~3.8 KB together) are HEAP buffers allocated on
// app enter via brfx_reset() and freed on app exit via brfx_shutdown(), so they hold ZERO static RAM at
// boot when the game is closed. The tiny PoolRing stays inline. Every access path is null-guarded so a
// failed alloc (or use before enter) degrades to no-op rather than dereferencing null.
static fxfx::Field<72>   *s_drops  = nullptr;   // 72 droplets in flight
static fxfx::PoolRing<28> s_pools;              // 28 lingering ground pools (inline, tiny)

// Impact sparks: a separate field of short-lived bright pips with little/no gravity. These are NOT blood
// (no colour reserve broken — the caller passes white / a duller grey for blocks), they just make a
// connect READ as a stamp. They never land (landY parked off-screen); they die by life only.
static fxfx::Field<48>   *s_sparks = nullptr;
static const float SPARK_GRAV = 90.0f;   // a faint pull so the burst arcs slightly, far below BR_GRAV

// Landing callback: a settled droplet becomes a small dark pool on the floor. Sizes jitter so pools
// read organic rather than stamped. Fires from Field::step when a falling particle reaches landY.
static void onLand(float x, float y)
{
    s_pools.add(x, y, 3 + (int)(br_frnd() * 3), 1 + (int)(br_frnd() * 2), BRFX_POOLDARK);
}

// Clear every store — called on app enter and on each level (re)start so blood/sparks don't carry across
// scenes. Doubles as the heap-alloc site: the two Fields are created here on first use (idempotent — a
// re-entry that already holds them just clears). If an alloc fails the pointer stays null and every
// access path below skips cleanly.
void brfx_reset(void)
{
    if (!s_drops)  s_drops  = (fxfx::Field<72>*)calloc(1, sizeof *s_drops);
    if (!s_sparks) s_sparks = (fxfx::Field<48>*)calloc(1, sizeof *s_sparks);
    if (s_drops)  s_drops->clear();
    if (s_sparks) s_sparks->clear();
    s_pools.clear();
}

// Free the heap Fields — called on app exit so they hold ZERO RAM while the game is closed.
void brfx_shutdown(void)
{
    free(s_drops);  s_drops  = nullptr;
    free(s_sparks); s_sparks = nullptr;
}

// Impact spark: a brief radial burst of small bright pips at (worldx, screeny). `col` is the spark hue
// (white for a clean connect, a duller grey for a blocked hit per the combat module); `n` scales with
// the move's weight (bigger for finishers/kicks). Short life + a faint gravity = a quick crisp flash,
// not a lingering shower. Parks landY off-screen so step() never converts a spark into a ground pool.
void brfx_spark(float worldx, float screeny, uint16_t col, int n)
{
    if (!s_sparks) return;
    if (n < 1) n = 1; else if (n > 16) n = 16;
    const float parkY = screeny + 4000.0f;        // never reached -> dies by life, leaves no decal
    for (int i = 0; i < n; i++) {
        float ang = br_frnd() * 6.2831853f;        // full radial spread
        float spd = 60.0f + br_frnd() * 110.0f;    // snappy outward speed
        float vx  = cosf(ang) * spd;
        float vy  = sinf(ang) * spd - 30.0f;       // slight upward bias so the burst lifts off the hit
        float life = 0.10f + br_frnd() * 0.12f;    // short: 0.10..0.22s
        s_sparks->spawn(worldx, screeny, vx, vy, life, parkY, col);
    }
}

// Emit a directional blood burst from (worldx, screeny) biased by `dir` (the attacker's facing).
// `amount` droplets; landY = the belt feet-y where they'll settle (caller passes scene_floor_y(z)).
// Field::spray is a template over two RNG callables (0..1 and -1..1); we hand it the shared br_frnd /
// br_frnd2 via tiny lambdas so the spread stays deterministic-friendly and owns no RNG state.
void brfx_blood(float worldx, float screeny, int dir, int amount, float landY)
{
    if (!s_drops) return;
    s_drops->spray(worldx, screeny, dir, amount, 130.0f, landY, BRFX_RED,
                   [] { return br_frnd(); },
                   [] { return br_frnd2(); });
}

// Advance droplets under gravity (settled ones spawn pools via onLand) and sparks under a faint pull
// (no land callback -> they vanish when their short life runs out, leaving the floor clean).
void brfx_step(float dt)
{
    if (s_drops)  s_drops->step(dt, BR_GRAV, &onLand);
    if (s_sparks) s_sparks->step(dt, SPARK_GRAV, NULL);
}

// Ground decals — drawn behind the fighters. Camera + shake folded into the x offset.
void brfx_draw_pools(void)
{
    s_pools.draw(-g.camx + g.shake.ox());
}

// Airborne droplets + impact sparks — drawn in front of the fighters. Sparks paint last so the bright
// pip sits on top of the body it just struck. Both fold camera scroll + shake into the x offset.
void brfx_draw_drops(void)
{
    float xoff = -g.camx + g.shake.ox();
    if (s_drops)  s_drops->draw(xoff);
    if (s_sparks) s_sparks->draw(xoff);
}
