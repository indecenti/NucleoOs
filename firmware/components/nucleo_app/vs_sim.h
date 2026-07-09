// Mini Vampire-Survivors — portable simulation core (device-agnostic, no M5GFX).
// Step 0 proof: does the hot loop (100 enemies + projectiles + collisions) stay O(N) via the
// spatial grid, with fixed-point math and SoA pools? Compiled on the PC by tools/vs-host/vs_host.c;
// the same core is later #included by the firmware app (heap-on-enter the VS struct in Solo boot).
#ifndef VS_SIM_H
#define VS_SIM_H
#include <stdint.h>

// ---- tunables (the budget contract from docs/game-mini-vs.md) ----
#define VS_MAX_EN    100
#define VS_MAX_PROJ  64          // raised from 48: several weapons fire at once now (multi-weapon build)
#define VS_MAX_GEM   96
#define VS_MAX_WEP   6           // Vampire-Survivors-style: the player carries several auto-firing weapons

// ---- weapon archetypes (shared with the app for labels / icons / render) ----
// Each fires INDEPENDENTLY on its own cooldown, auto-targets, and levels up 1..VS_WLVL_MAX.
enum { WEP_MAGO, WEP_FRUSTA, WEP_GUARDIANO, WEP_PIROMANE, WEP_CECCHINO, WEP_COUNT };
#define VS_WLVL_MAX 5

// ---- projectile visual/behaviour kind (shared with the app so it can draw each distinctly) ----
enum { PK_BOLT, PK_WHIP, PK_NOVA, PK_SNIPE };   // Guardiano orbs are drawn from weapon state, not the pool

// ---- fixed-point (8.8): no floats in the hot loop ----
typedef int32_t vfix;
#define VS_FP    8
#define VS_TOFIX(x) ((vfix)((x) << VS_FP))
#define VS_TOINT(x) ((int)((x) >> VS_FP))

// ---- spatial grid: a camera-centred window, 16 px cells ----
#define VS_CELL  16
#define VS_GW    32           // 32*16 = 512 px window width
#define VS_GH    32

typedef struct {
    // player
    vfix ppx, ppy;
    vfix ppvx, ppvy;       // velocity (set by input; integrated + damped each frame)
    int  php, phpmax, plevel, pxp, hurt_cd;
    int  pending_up;       // unspent level-ups: the app pops the upgrade menu while >0
    uint8_t pface;         // 0 = facing right, 1 = left (set by the app; the whip needs a facing)

    // global passive stats (raised by the level-up chooser in the app). Weapons read these so ONE
    // passive card buffs every weapon at once — the Vampire-Survivors "Might / Cooldown / Area" model.
    int up_magnet, up_regen, up_garlic;
    int up_might;          // +damage to everything
    int up_haste;          // -cooldown on every weapon
    int up_area;           // +size/reach/projectile-count on every weapon
    int up_speed;          // +move speed (the app reads this to scale its input target)
    int aura_r;            // current garlic-aura radius in px (0 = none); the app draws it
    int wave_cd;           // frames to next encircle wave

    // weapons the player owns (auto-fire, each on its own cooldown)
    uint8_t wtype[VS_MAX_WEP];   // WEP_* archetype
    uint8_t wlevel[VS_MAX_WEP];  // 1..VS_WLVL_MAX
    int16_t wcd[VS_MAX_WEP];     // frames to this weapon's next shot
    int     wcount;              // number of owned weapons

    // enemies (Struct-of-Arrays)
    vfix    ex[VS_MAX_EN], ey[VS_MAX_EN];
    int16_t ehp[VS_MAX_EN];
    uint8_t etype[VS_MAX_EN];
    uint8_t ealive[VS_MAX_EN];
    uint8_t eranged[VS_MAX_EN];    // 1 = ring-wave shooter: fires bolts at the player, not melee-only
    uint8_t eelite[VS_MAX_EN];     // 1 = elite/miniboss: big, tanky, worth a burst of xp
    uint8_t eboss[VS_MAX_EN];      // 1 = minute BOSS: huge, very tanky, radial-fires, big xp burst on death
    uint8_t ehflash[VS_MAX_EN];    // hit-flash frames left: the app draws the enemy white (damage juice)
    int16_t efire_cd[VS_MAX_EN];   // frames to next shot (eranged/boss enemies)
    int     en_count;
    int     shake;                 // screen-shake frames left (the app offsets the camera) — set on big hits
    int     boss_alive;            // 1 while a boss is on the field (the app shows a boss HP bar)
    int     boss_hpmax;            // the current boss's spawn HP (denominator for its HP bar)

    // projectiles (SoA)
    vfix    px[VS_MAX_PROJ], py[VS_MAX_PROJ], pvx[VS_MAX_PROJ], pvy[VS_MAX_PROJ];
    uint8_t plife[VS_MAX_PROJ], palive[VS_MAX_PROJ], ppierce[VS_MAX_PROJ];
    uint8_t powner[VS_MAX_PROJ];   // 0 = player bolt (hits enemies), 1 = enemy bolt (hits the player)
    uint8_t pkind[VS_MAX_PROJ];    // PK_* — the app draws each kind differently
    int8_t  pdmg[VS_MAX_PROJ];     // damage this projectile deals per hit
    int     pr_count;

    // xp gems (SoA). gval = xp granted (elites drop a fat gem worth several).
    vfix    gx[VS_MAX_GEM], gy[VS_MAX_GEM];
    uint8_t galive[VS_MAX_GEM];
    uint8_t gval[VS_MAX_GEM];
    int     gem_count;

    // pickups (rare, non-xp): heal + screen-clear bomb. Small fixed pool.
    vfix    kx[8], ky[8];
    uint8_t kalive[8], ktype[8];   // ktype: 0 = heal (chicken), 1 = bomb (clears the screen)
    int     want_bomb;             // set when a bomb is collected; the app plays the flash, the sim clears

    // uniform-grid spatial hash over enemies (rebuilt each frame)
    int16_t head[VS_GW * VS_GH];
    int16_t nxt[VS_MAX_EN];
    vfix    grid_ox, grid_oy;

    uint32_t rng;
    uint32_t frame;
    int      kills;

    // instrumentation (Step 0 proof)
    uint32_t checks;       // distance comparisons this frame
    uint32_t checks_max;
} VS;

void vs_init(VS *w, uint32_t seed);
void vs_step(VS *w);       // advance exactly one game frame

// add a weapon (if new + a slot is free) or level an owned one. Returns 1 if it changed anything.
int  vs_give_weapon(VS *w, int wtype);
int  vs_has_weapon(VS *w, int wtype);    // -1 if not owned, else its level
int  vs_weapon_maxed(VS *w, int wtype);  // 1 if owned AND at VS_WLVL_MAX

#endif
