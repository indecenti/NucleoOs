// brawler_levels.cpp — SCORRIBANDA: the 6 levels (waves) + the "Double Dragon" gate.
//
// Fully data-driven: each level is a LevelDef with a "white paper" palette (background BR_PAPER, lines only
// in the BR_GREY_* greys, slightly bolder as the level goes up) and a wave table (WaveDef[]). Adding
// entries to LEVELS automatically extends the game (brawler_level_count() = array size). The enemies'
// blue and the blood's red live in other modules: only white and greys here.
//
// Double Dragon-style progression: heroes cannot pass g.gatex until the wave is cleared;
// once a wave is cleared, the gate slides forward and the next wave starts. Live-enemy cap BR_MAXENEMY: the
// extras stay queued and trickle into the scene as slots free up.
//
// Implements the "levels / waves" section of brawler.h:
//   brawler_level_count, brawler_level, levels_begin, levels_step, levels_is_clear
// Calls the other modules ONLY via brawler.h (brawler_spawn_enemy / brawler_live_enemies / br_*).

#include "brawler.h"
#include <math.h>

// ----------------------------------------------------------------- palette helper (white look)
// Minimal "white paper" look: background BR_PAPER, lines only in the BR_GREY_* greys. No other colors here
// (enemy blue and blood red live in other modules). Levels vary ONLY by mixing the greys.
// fx3d::mix(a,b,t) with t 0..255: t=0 -> a, t=255 -> b. We use it to move within the frozen palette.

// ----------------------------------------------------------------- wave tables
// Enemy types 0..3 (see brawler_enemies.cpp): 0=thug, 1=brawler, 2=thrower, 3=BOSS (gang leader).
// Later levels mix in tougher types; L6's last wave closes with the boss (type 3).

// L1 — The Alley: introduction, only type0/1.
static const WaveDef WAVES_L1[] = {
    { {0,0},     2 },
    { {0,1,0},   3 },
    { {1,0,1},   3 },
};
// L2 — The Market: more enemies, the thrower appears (type2).
static const WaveDef WAVES_L2[] = {
    { {0,1,0},     3 },
    { {1,2,1},     3 },
    { {0,1,2,0},   4 },
};
// L3 — The Subway: dense waves, mix 0/1/2.
static const WaveDef WAVES_L3[] = {
    { {1,0,1,0},   4 },
    { {2,1,2},     3 },
    { {0,1,2,1,0}, 5 },
    { {1,2,1,2},   4 },
};
// L4 — The Warehouses: more brawlers and throwers.
static const WaveDef WAVES_L4[] = {
    { {1,1,0,1},   4 },
    { {2,1,2,1},   4 },
    { {1,2,1,2,1}, 5 },
    { {2,2,1,1},   4 },
};
// L5 — The Rooftops: near-boss difficulty, full waves.
static const WaveDef WAVES_L5[] = {
    { {1,2,1,2},     4 },
    { {2,1,2,1,2},   5 },
    { {1,1,2,2,1,2}, 6 },
    { {2,2,1,2,1},   5 },
};
// L6 — The Harbor: the climax; the last wave is the BOSS (type3) with an escort.
static const WaveDef WAVES_L6[] = {
    { {1,2,1,2,1},   5 },
    { {2,1,2,2,1},   5 },
    { {1,2,2,1,2,1}, 6 },
    { {2,2,1,2,1,2}, 6 },
    { {3,1,2},       3 },   // boss + two henchmen
};

// ----------------------------------------------------------------- level definitions
// Palette: greys that get gradually darker and colder. (sky_top, sky_bot, build_far, build_near,
// floor_far, floor_near, floor_line). build_near darker than build_far -> depth.
// NB: NOT const — the color fields are computed at runtime (br_rgb isn't constexpr); a const would end up
// in flash/.rodata and the write would crash on the ESP32. The WaveDef pointers stay as const tables.
static LevelDef LEVELS[] = {
    // L1 — The Alley (warm night, medium greys)
    { "Il Vicolo", "The Alley", 1000.0f,
      0,  0,  0,  0,  0,  0,  0,   // placeholder palette filled in below at init (see note)
      (uint8_t)(sizeof(WAVES_L1)/sizeof(WAVES_L1[0])), WAVES_L1 },
    // L2 — The Market
    { "Il Mercato", "The Market", 1100.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L2)/sizeof(WAVES_L2[0])), WAVES_L2 },
    // L3 — The Subway
    { "La Metro", "The Subway", 1200.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L3)/sizeof(WAVES_L3[0])), WAVES_L3 },
    // L4 — The Warehouses
    { "I Magazzini", "The Warehouse", 1300.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L4)/sizeof(WAVES_L4[0])), WAVES_L4 },
    // L5 — The Rooftops
    { "I Tetti", "The Rooftops", 1400.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L5)/sizeof(WAVES_L5[0])), WAVES_L5 },
    // L6 — The Harbor (climax, boss)
    { "Il Porto", "The Docks", 1500.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L6)/sizeof(WAVES_L6[0])), WAVES_L6 },
};

// Palettes are computed at runtime (br_rgb isn't constexpr): a small lazy init writes them into the
// table's 565 slots the first time a level is requested. The table stays data-driven.
// White look: the sky is paper (BR_PAPER), the building lines are in greys (far->mid), the floor
// stays very light (paper -> barely perceptible grey) and the ground line is a soft grey. Going up
// levels we mix the greys a touch (t grows) to give depth without EVER leaving the palette.
static bool s_pal_init = false;
static void levels_palettes_init(void) {
    if (s_pal_init) return;
    s_pal_init = true;
    const int N = (int)(sizeof(LEVELS) / sizeof(LEVELS[0]));
    for (int i = 0; i < N; i++) {
        // 0..40: as it advances the greys get slightly bolder (small step, we stay "paper").
        int t = (N > 1) ? (i * 40 / (N - 1)) : 0;             // 0 (L1) .. 40 (L6) — subtle variation
        LevelDef *L = &LEVELS[i];
        // Sky: essentially paper. Full BR_PAPER at the top, a hint of the far grey at the bottom.
        L->sky_top    = BR_PAPER;
        L->sky_bot    = fx3d::mix(BR_PAPER, BR_GREY_FAR, 28 + t);   // veil of grey further up
        // Buildings (line-art): far grey in the distance, mid grey nearby; a touch bolder going up.
        L->build_far  = fx3d::mix(BR_GREY_FAR, BR_GREY_MID, t);
        L->build_near = fx3d::mix(BR_GREY_MID, BR_GREY_NEAR, t);
        // Floor: stays paper. Clean paper in the distance, a barely-hinted grey nearby.
        L->floor_far  = BR_PAPER;
        L->floor_near = fx3d::mix(BR_PAPER, BR_GREY_FAR, 40 + t);
        // Ground line: near grey but soft (mixed toward mid) -> presence without a grid.
        L->floor_line = fx3d::mix(BR_GREY_MID, BR_GREY_NEAR, 90 + t);
    }
}

// ----------------------------------------------------------------- table API
int brawler_level_count(void) { return (int)(sizeof(LEVELS) / sizeof(LEVELS[0])); }

const LevelDef *brawler_level(int i) {
    levels_palettes_init();
    int n = brawler_level_count();
    if (n <= 0) return 0;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return &LEVELS[i];
}

// ----------------------------------------------------------------- runtime state (queue/flags)
// Small static struct: "queued" enemies not yet entered because of the live-enemy cap, plus wave flags.
struct LevelRun {
    uint8_t qtype[BR_MAXENEMY * 2];  // queued types to bring in as soon as a slot frees up
    int     qn;                      // how many are queued
    int     qhead;                   // next index to spawn
    float   drip;                    // staggered-entry timer (s)
    bool    wave_done;               // all waves processed -> gate fully open
    bool    began;                   // levels_begin called
};
static LevelRun s_run = {0};

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Reference hero for positioning spawns (player 0; fallback player 1).
static Fighter *ref_hero(void) {
    Fighter *h = br_hero(0);
    if (!h || !h->on) h = br_hero(1);
    return h;
}

// ----------------------------------------------------------------- wave spawn
// Queues the current wave; whatever enters right away (under the live cap) happens here, the rest trickles in during step.
static void spawn_wave(void) {
    const LevelDef *L = brawler_level(g.level);
    if (!L) return;
    if (g.wave < 0 || g.wave >= L->nwaves) return;
    const WaveDef *w = &L->waves[g.wave];

    // Loads the wave's types into the queue.
    s_run.qn = 0;
    s_run.qhead = 0;
    s_run.drip = 0.0f;
    int cnt = w->count;
    if (cnt > BR_MAXENEMY) cnt = BR_MAXENEMY;             // WaveDef.types has BR_MAXENEMY slots
    int cap = (int)(sizeof(s_run.qtype) / sizeof(s_run.qtype[0]));
    for (int i = 0; i < cnt && s_run.qn < cap; i++) {
        s_run.qtype[s_run.qn++] = w->types[i];
    }
    // The first drip in levels_step will bring in the enemies under the live cap.
}

// Brings in the next queued enemy in front of/behind the hero, within the level bounds.
static bool drip_one(void) {
    if (s_run.qhead >= s_run.qn) return false;
    if (brawler_live_enemies() >= BR_MAXENEMY) return false;

    Fighter *h = ref_hero();
    float hx = h ? h->x : g.camx + BR_SW * 0.5f;
    float side = (br_frnd() < 0.5f) ? -1.0f : 1.0f;
    float x = hx + side * (BR_SW * 0.55f + br_frnd() * 60.0f);
    x = clampf(x, 40.0f, g.level_len - 40.0f);
    float z = 0.3f + br_frnd() * 0.6f;                    // belt depth 0.3..0.9

    int type = s_run.qtype[s_run.qhead];
    Fighter *e = brawler_spawn_enemy(type, x, z);
    if (!e) return false;                                 // no slot: retry on the next step
    s_run.qhead++;
    return true;
}

// ----------------------------------------------------------------- begin
void levels_begin(int level) {
    levels_palettes_init();
    int n = brawler_level_count();
    if (level < 0) level = 0;
    if (level >= n) level = n - 1;

    g.level = level;
    const LevelDef *L = brawler_level(level);
    g.level_len = L ? L->length : 1000.0f;
    g.camx = 0.0f;
    g.wave = 0;
    g.gatex = BR_SW * 0.9f;                               // first gate in front of the hero

    // Clear the enemy slots (leave the heroes).
    for (int i = 0; i < BR_MAXF; i++) {
        if (!g.f[i].is_hero) g.f[i].on = false;
    }

    s_run.qn = 0;
    s_run.qhead = 0;
    s_run.drip = 0.0f;
    s_run.wave_done = false;
    s_run.began = true;

    spawn_wave();                                         // queue wave 0 (will enter during step)
}

// ----------------------------------------------------------------- step
// Advances the gate when the wave is cleared; trickles queued enemies in under the live cap.
void levels_step(float dt) {
    if (!s_run.began) return;
    const LevelDef *L = brawler_level(g.level);
    if (!L) return;

    // 1) Drip: brings queued enemies on scene, staggered over time.
    if (s_run.qhead < s_run.qn) {
        s_run.drip -= dt;
        while (s_run.drip <= 0.0f && s_run.qhead < s_run.qn &&
               brawler_live_enemies() < BR_MAXENEMY) {
            if (!drip_one()) break;                       // no slot available right now: wait for the next step
            s_run.drip += 0.45f + br_frnd() * 0.5f;       // staggered entries ~0.45..0.95 s
        }
    }

    // 2) Wave advance: only when there are NO live enemies and the current queue is empty.
    bool queue_empty = (s_run.qhead >= s_run.qn);
    if (brawler_live_enemies() == 0 && queue_empty) {
        if (g.wave < L->nwaves - 1) {
            // More waves left: open the gate by one screen and launch the next one.
            g.wave++;
            g.gatex = fminf(g.gatex + (float)BR_SW, g.level_len);
            spawn_wave();
        } else {
            // Last wave cleared: gate fully open, the hero can reach the exit.
            g.gatex = g.level_len;
            s_run.wave_done = true;
        }
    }
}

// ----------------------------------------------------------------- clear
// Level cleared when the last wave is defeated (alive==0, empty queue, last wave) AND a hero has
// reached the exit (x > level_len - 40).
bool levels_is_clear(void) {
    if (!s_run.began) return false;
    const LevelDef *L = brawler_level(g.level);
    if (!L) return false;

    bool last_wave   = (g.wave >= L->nwaves - 1);
    bool queue_empty = (s_run.qhead >= s_run.qn);
    bool defeated    = (brawler_live_enemies() == 0) && last_wave && queue_empty;
    if (!defeated) return false;

    // A hero must have reached the level's right edge.
    float exitx = g.level_len - 40.0f;
    Fighter *h0 = br_hero(0);
    Fighter *h1 = br_hero(1);
    bool reached = (h0 && h0->on && h0->x > exitx) ||
                   (h1 && h1->on && h1->x > exitx);
    return reached;
}
