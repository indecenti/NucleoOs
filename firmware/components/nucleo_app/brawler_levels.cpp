// brawler_levels.cpp — SCORRIBANDA: the 6 levels (waves) + the "Double Dragon" gate.
//
// Fully data-driven: each level is a LevelDef (street length + a wave table). Adding entries to LEVELS
// automatically extends the game (brawler_level_count() = array size); give the new level a name in
// brawler_level_name().
//
// Double Dragon-style progression: heroes cannot pass g.gatex until the wave is cleared;
// once a wave is cleared, the gate slides forward and the next wave starts. Live-enemy cap BR_MAXENEMY: the
// extras stay queued and trickle into the scene as slots free up.
//
// Implements the "levels / waves" section of brawler.h:
//   brawler_level_count, brawler_level, levels_begin, levels_step, levels_is_clear
// Calls the other modules ONLY via brawler.h (brawler_spawn_enemy / brawler_live_enemies / br_*).

#include "brawler.h"
#include "game_text.h"
#include <math.h>

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
// Length + waves only (const, in flash). The street's grey shade is derived from the level index by
// scene_draw(); the name comes from brawler_level_name() in the OS language.
#define NW(w) (uint8_t)(sizeof(w) / sizeof(w[0])), w
static const LevelDef LEVELS[] = {
    { 1000.0f, NW(WAVES_L1) },   // L1 the alley
    { 1100.0f, NW(WAVES_L2) },   // L2 the market
    { 1200.0f, NW(WAVES_L3) },   // L3 the subway
    { 1300.0f, NW(WAVES_L4) },   // L4 the warehouses
    { 1400.0f, NW(WAVES_L5) },   // L5 the rooftops
    { 1500.0f, NW(WAVES_L6) },   // L6 the docks (climax, boss)
};

// ----------------------------------------------------------------- table API
int brawler_level_count(void) { return (int)(sizeof(LEVELS) / sizeof(LEVELS[0])); }

const LevelDef *brawler_level(int i) {
    int n = brawler_level_count();
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return &LEVELS[i];
}

const char *brawler_level_name(int i) {
    switch (i) {
        case 0:  return GT("Il Vicolo", "The Alley");
        case 1:  return GT("Il Mercato", "The Market");
        case 2:  return GT("La Metro", "The Subway");
        case 3:  return GT("I Magazzini", "The Warehouse");
        case 4:  return GT("I Tetti", "The Rooftops");
        default: return GT("Il Porto", "The Docks");
    }
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

    // Enter just past a screen edge so a foe always walks IN (a spawn clamped to the street start used to
    // pop up in plain view). No room behind (street start) -> come from ahead, and vice versa.
    float off = 16.0f + br_frnd() * 50.0f;
    float x = (br_frnd() < 0.5f) ? g.camx - off : g.camx + BR_SW + off;
    if (x < 8.0f) x = g.camx + BR_SW + off;
    else if (x > g.level_len - 8.0f) x = g.camx - off;
    float z = 0.3f + br_frnd() * 0.6f;                    // belt depth 0.3..0.9

    int type = s_run.qtype[s_run.qhead];
    Fighter *e = brawler_spawn_enemy(type, x, z);
    if (!e) return false;                                 // no slot: retry on the next step
    s_run.qhead++;
    return true;
}

// ----------------------------------------------------------------- begin
void levels_begin(int level) {
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
    g.banner = 0; g.banner_t = 2.2f;                      // the stage card slides in over the street
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
            if (!s_run.wave_done) bsfx(BSFX_GO);          // the street is open: GO (the HUD arrow blinks)
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
