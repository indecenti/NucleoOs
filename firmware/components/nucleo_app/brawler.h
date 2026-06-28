// brawler.h — SCORRIBANDA: the single shared contract for the noir belt-scroll beat'em up.
//
// This is the one source of cross-module truth. Every brawler_*.cpp implements the functions declared
// for its section and calls other sections only through here, so the modules build in parallel and link
// cleanly. Game state lives in ONE BrCtx `g`; rendering is silhouettes via the reusable fx* toolkit.
//
//   app_brawler.cpp     shell: lifecycle, input routing, main loop, `g`, br_* helpers, registration
//   brawler_chars.cpp   3 heroes + per-fighter pose composition (fighter_pose)
//   brawler_enemies.cpp enemy roster + AI brains
//   brawler_combat.cpp  hitboxes, damage, knockback, combos, KO
//   brawler_levels.cpp  6 levels of waves + the Double-Dragon gate/progression
//   brawler_scene.cpp   per-level parallax backdrop + belt
//   brawler_fx.cpp      blood spray + ground pools (built on fxfx)
//   brawler_menu.cpp    title / character-select / options / pause / game-over / HUD
//   brawler_sfx.cpp     procedural sound effects
//   brawler_net.cpp     2-player co-op over ESP-NOW (host-authoritative)
#pragma once
#include "nucleo_fxfig.h"
#include "nucleo_fxanim.h"
#include "nucleo_fxfx.h"
#include "nucleo_fxscene.h"
#include <stdint.h>

// ------------------------------------------------------------------ tuning / limits
#define BR_SW        240
#define BR_SH        135
#define BR_MAXF      8          // heroes (1-2) + on-screen enemies
#define BR_MAXENEMY  6          // simultaneous live enemies
#define BR_GRAV      900.0f     // px/s^2 for jumps & blood

// ------------------------------------------------------------------ STRICT palette (minimal, C&D-on-white)
// The whole game uses ONLY these. White "paper" backdrop, grey LINE-ART shapes, black heroes, ONE colour
// for enemies (steel blue), red ONLY for blood. Introduce no other hue anywhere. (br_rgb is declared
// below; these macros expand at the call site, exactly like the menu's MN_* colours.)
#define BR_PAPER     br_rgb(243, 243, 238)   // background base — warm white
#define BR_GREY_FAR  br_rgb(214, 216, 219)   // faint far backdrop lines
#define BR_GREY_MID  br_rgb(176, 179, 184)   // mid backdrop lines
#define BR_GREY_NEAR br_rgb(132, 136, 142)   // near backdrop lines / ground shade
#define BR_INK       br_rgb(22, 22, 26)      // heroes — near-black silhouette + strongest outlines
#define BR_ENEMY     br_rgb(36, 92, 150)     // (legacy) generic enemy blue
#define BR_ENEMY_DK  br_rgb(22, 60, 104)     // (legacy) generic enemy outline
#define BR_BLOOD     br_rgb(206, 26, 28)     // blood — reserved exclusively for blood (NEVER an enemy hue)

// Enemy VARIETY (user-requested): one distinct hue PER TYPE so foes read apart at a glance. None may be
// red (that's blood). Heroes stay BR_INK (black); the backdrop stays white + greys. The per-fighter
// outline is just the darker shade of the body (computed at draw time), so the colour pops on paper.
#define BR_EN_THUG   br_rgb(36, 92, 150)     // type0 teppista  — steel blue
#define BR_EN_BRUTE  br_rgb(150, 110, 40)    // type1 bruto     — ochre / khaki
#define BR_EN_BLADE  br_rgb(34, 140, 96)     // type2 lama      — teal green
#define BR_EN_BOSS   br_rgb(120, 60, 150)    // type3 capobanda — purple

// ------------------------------------------------------------------ fighters
enum BrState {
    BS_IDLE, BS_WALK, BS_PUNCH, BS_KICK, BS_JUMP, BS_JKICK,
    BS_HIT, BS_DOWN, BS_RISE, BS_GRAB, BS_THROW, BS_BLOCK, BS_WIN
};

struct Fighter {
    bool    on;
    bool    is_hero;
    int     kind;        // hero index (chars) or enemy type (enemies)
    float   x, z;        // world x (px) along the street, depth 0..1 (0 far .. 1 near)
    float   vx, vz;      // drift / knockback velocity
    float   yoff, vy;    // jump height above the belt (px) + vertical velocity
    int     dir;         // facing +1 / -1
    int     hp, maxhp;
    BrState st;
    float   anim;        // 0..1 progress within the current action
    float   aspd;        // anim speed (1/duration s)
    int     var;         // attack variant / combo step
    bool    hit_done;    // current attack already connected (one hit per swing)
    float   flash;       // white hit-flash 0..1
    float   cool;        // AI cooldown (s)
    float   walkphase;   // running stride phase
    uint8_t player;      // co-op: 0 or 1 for heroes; ignored for enemies
};

// ------------------------------------------------------------------ screens / context
enum BrScreen { SC_MENU, SC_SEL, SC_PLAY, SC_PAUSE, SC_OVER, SC_CLEAR, SC_HELP, SC_OPT,
                SC_COOP,    // co-op role pick: Host or Join
                SC_LOBBY }; // co-op pairing: waiting for the other Cardputer over ESP-NOW

struct BrCtx {
    BrScreen   screen;
    int        sel;            // generic menu cursor
    int        hero_pick[2];   // chosen hero per player
    int        nplayers;       // 1 or 2
    int        level;          // 0..count-1
    int        wave;           // current wave index in the level
    Fighter    f[BR_MAXF];     // f[0] (and f[1] in co-op) = heroes; rest = enemies
    float      camx;           // camera world x (left edge)
    float      gatex;          // right barrier: heroes can't pass until the wave is cleared
    float      level_len;      // world length of the current level
    int        lives;
    long       score;
    int        combo;          // hero combo count (HUD juice)
    float      combo_t;        // combo decay timer (s)
    float      hitstop;        // freeze timer (s): sim pauses, draw continues
    fxfx::Shake shake;
    uint32_t   now;            // ms (refreshed each poll)
    float      floorscroll;    // belt scroll phase
    bool       audio;
    int        lang;           // 0 = it, 1 = en
    int        diff;           // 0 easy, 1 normal, 2 hard
    bool       net;            // co-op session active
    bool       is_host;
    bool       paused;
};
extern BrCtx g;                // defined in app_brawler.cpp

// ------------------------------------------------------------------ shared helpers (app_brawler.cpp)
uint32_t br_now_ms(void);
uint16_t br_rgb(int r, int g, int b);
uint32_t br_rnd(void);                  // xorshift
float    br_frnd(void);                 // 0..1
float    br_frnd2(void);                // -1..1
float    br_screen_x(float worldx);     // worldx - camx + shake.ox()
Fighter *br_hero(int player);           // heroes only (NULL if absent)
void     br_reset_fighters(void);

// ------------------------------------------------------------------ characters (brawler_chars.cpp)
struct HeroDef {
    const char *name;
    const char *style_it, *style_en;
    int      maxhp;
    float    speed;        // px/s
    int      pdmg, kdmg;   // punch / kick damage
    float    reach;        // px
    float    girth;        // limb thickness multiplier (silhouette build)
    uint16_t rim;          // edge-light accent
    int      combo_len;    // max punch chain
    float    scale;        // overall body-size (height) multiplier for distinct builds; 0/unset -> 1.0
};
int            brawler_hero_count(void);
const HeroDef *brawler_hero(int i);
// Fill the 11-joint pose for fr's current state/anim (composes fxanim + per-character flavour).
void           fighter_pose(const Fighter *fr, fxfig::Pt out[fxfig::FX_NJ]);
// Silhouette body colour for fr (folds in hit-flash). Used by the draw loop.
uint16_t       fighter_body(const Fighter *fr);

// ------------------------------------------------------------------ enemies (brawler_enemies.cpp)
struct EnemyDef {
    const char *name_it, *name_en;
    int      maxhp;
    float    speed;
    int      dmg;
    float    reach;
    float    girth;
    uint16_t body;
    int      score;
    float    scale;        // overall body-size multiplier (boss big, lama small); 0/unset -> 1.0
};
int             brawler_enemy_count(void);
const EnemyDef *brawler_enemy(int t);
Fighter        *brawler_spawn_enemy(int type, float x, float z);
int             brawler_live_enemies(void);
void            enemies_ai(float dt);

// ------------------------------------------------------------------ levels / waves (brawler_levels.cpp)
struct WaveDef { uint8_t types[BR_MAXENEMY]; uint8_t count; };
struct LevelDef {
    const char *name_it, *name_en;
    float    length;                                   // world px
    uint16_t sky_top, sky_bot, build_far, build_near;  // backdrop palette
    uint16_t floor_far, floor_near, floor_line;
    uint8_t  nwaves;
    const WaveDef *waves;
};
int             brawler_level_count(void);
const LevelDef *brawler_level(int i);
void            levels_begin(int level);   // load level: reset camera/gate/wave, spawn wave 0
void            levels_step(float dt);     // advance gate, spawn next wave when cleared
bool            levels_is_clear(void);     // all waves of the level defeated

// ------------------------------------------------------------------ combat (brawler_combat.cpp)
// COMBAT DESIGN (Capcom belt-scroll feel, the bar to hit):
//  - Hero light string J,J,J: hits 1-2 = fast jabs (light dmg, small push, brief hitstun); hit 3 =
//    FINISHER (heavier, KNOCKS DOWN -> victim BS_DOWN even if still alive, then rises). K = a strong
//    single kick (more range/knockback). Air J/K = overhead jump-kick.
//  - Knockback scales with the move (jab small, finisher/kick large) and is directional (attacker dir).
//  - A non-lethal KNOCKDOWN is BS_DOWN with hp>0 -> the fighter must RISE (only hp<=0 enemies despawn,
//    hp<=0 heroes stay floored for the shell's life system). Fix combat_step so a knocked-down LIVE
//    enemy gets up instead of vanishing.
//  - Enemies may BLOCK (BS_BLOCK): a blocked light hit deals chip + no stun (the brute/boss use it).
//  - Feedback on every connect: white hit-spark (brfx_spark), hitstop scaled by weight, screen-shake,
//    white body-flash, blood. Make hits READ.
void  combat_step(float dt);                 // integrate fighters: knockback, jumps, flash, KO, recovery
void  combat_begin_attack(Fighter *fr, BrState kind);  // start punch/kick/jumpkick (sets anim/var/cooldown)
void  combat_resolve(float dt);              // active-frame hitbox checks -> fighter_take_hit + blood
void  fighter_take_hit(Fighter *victim, Fighter *src, int dmg, float knock);
bool  fighter_busy(const Fighter *fr);       // mid-attack / hit / down (input locked)

// ------------------------------------------------------------------ fx glue: blood (brawler_fx.cpp)
void  brfx_reset(void);        // clear stores; allocates the heap particle Fields on first use (app enter)
void  brfx_shutdown(void);     // free the heap particle Fields (app exit) -> zero static RAM when closed
void  brfx_blood(float worldx, float screeny, int dir, int amount, float landY);
void  brfx_step(float dt);
void  brfx_draw_pools(void);   // ground decals (drawn behind fighters)
void  brfx_draw_drops(void);   // airborne droplets (drawn in front)
void  brfx_spark(float worldx, float screeny, uint16_t col, int n);  // brief impact spark burst (hit feedback)

// ------------------------------------------------------------------ scene / backdrop (brawler_scene.cpp)
extern const fxfig::Belt BR_BELT;
float scene_floor_y(float z);  // belt feet-y for a depth (handy for blood landing + shadows)
float scene_scale(float z);    // figure scale for a depth
void  scene_draw(void);        // sky + parallax skyline + belt for g.level

// ------------------------------------------------------------------ menu / HUD / screens (brawler_menu.cpp)
void  menu_draw(void);                 // renders the current non-play screen
void  hud_draw(void);                  // health/score/lives/combo over the action
bool  menu_key(int key, char ch);      // consume a key on menu screens; true if handled
void  menu_goto(BrScreen s);           // switch screen (resets cursors, plays a cue)
void  menu_coop_start(void);           // co-op: both peers paired -> configure heroes + enter SC_PLAY
                                       // (host sims + streams; guest renders the host snapshot)

// ------------------------------------------------------------------ sound (brawler_sfx.cpp)
enum { BSFX_NAV = 1, BSFX_SEL, BSFX_BACK, BSFX_WHIFF, BSFX_HIT, BSFX_KO, BSFX_HURT, BSFX_JUMP, BSFX_CLEAR, BSFX_OVER };
void  bsfx_presynth(void);   // pre-generate WAVs to SD on app open (async-safe play later)
void  bsfx(int id);          // play a cue (no-op if g.audio off or busy)

// ------------------------------------------------------------------ co-op net (brawler_net.cpp)
bool  bnet_start(void);      // bring up ESP-NOW for this app
void  bnet_stop(void);
void  bnet_poll(void);       // drain packets; host streams state, guest streams input
bool  bnet_available(void);  // true once a peer is paired
