// app_sandgarden.cpp — NucleoOS "Giardino di Sabbia": a falling-sand cultivation game (Games).
//
// A real cellular-automaton sandbox turned into a timed score game: drop seeds on soil, pour water
// to make them sprout and climb, and they BLOOM into flowers (points + sparkles). Lava rains from
// the sky and threatens the garden — quench it with water (lava + water -> stone) and wall it off
// with stone. Grow the brightest garden before the timer runs out; best runs go on the leaderboard.
//
// Constraints: the 120x52 grid is malloc'd on enter and freed on exit (NOT permanent .bss), with
// exclusive_flags=NX_NET_APP so the RAM + the shared I2S line are free for the sim and the chiptune
// SFX. Drawing is the buffered `d.` path (one pushSprite/frame). LEFT/BACK reach on_back(); other
// keys reach on_key(). ASCII-only text (the bitmap font has no accents).

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "nucleo_exclusive.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
}

// ============================ palette ========================================
static inline uint16_t rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t mix(uint16_t a, uint16_t b, int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar + (br - ar) * t / 256) & 31) << 11) | (((ag + (bg - ag) * t / 256) & 63) << 5) | ((ab + (bb - ab) * t / 256) & 31));
}
#define COL_BG    rgb(10, 12, 20)
#define COL_WHITE 0xFFFF
#define COL_CYAN  rgb(96, 206, 232)
#define COL_AMBER rgb(255, 190, 64)
#define COL_GREEN rgb(118, 230, 140)
#define COL_RED   rgb(244, 92, 78)
#define COL_GREY  rgb(150, 160, 184)
#define COL_DIM   rgb(78, 88, 116)
#define COL_CAP   rgb(24, 52, 38)

// ============================ grid + materials ===============================
// Chunkier 3px cells (was a 2px mush that read as noise): GW*CELL = 240 keeps the play area exactly the
// screen width, and a coarser grid reads as a real garden — grains, plants and flowers are actually
// visible. Fewer cells (80x34 vs 120x52) also means less sim RAM/CPU.
#define GW 80
#define GH 34
#define CELL 3
#define PLAY_Y0 16
#define PLAY_H  (GH * CELL)              // 102; play area y = 16..118

enum { M_EMPTY = 0, M_SAND, M_WATER, M_SEED, M_PLANT, M_FLOWER, M_FIRE, M_STONE, M_STEAM, M_LAVA };

static uint8_t *s_grid;                  // GW*GH, heap (freed on exit)
static unsigned s_tick;

static int   s_score, s_time_ms, s_round_ms, s_tool, s_bx, s_by;
static int   s_brush;                       // 0 = fine, 1 = wide (S toggles) — precision vs. filling
static bool  s_pour;

// ============================ state ==========================================
enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OVER, ST_SCORES, ST_NAME };
static int   g_lang = 0, g_audio = 1, g_diff = 1, g_best = 0;
static int   s_screen, s_msel, s_setsel, s_help;
static float s_capY;
static int64_t s_now, s_last, s_frame, s_simacc;
static unsigned s_anim;
static bool  s_newbest, s_bestplayed, s_qualify;
static int   s_over_ms;
static int   s_combo, s_combo_ms;          // bloom combo multiplier + decay window
static int64_t s_lastbloom;
static int   s_weather, s_weather_ms, s_weather_next, s_banner_ms;   // 0 none, 1 rain, 2 lava storm
static const char *s_ban_it = "", *s_ban_en = "";
static int   s_flowers;                     // live flower census (HUD)
static struct { int16_t x, y; uint8_t ph; } s_mote[12];   // ambient drifting motes
static float s_bee_x, s_bee_y; static int s_bee_t;        // pollinator mascot
static int   s_pr_ms, s_pr_x, s_pr_y; static const char *s_pr_it = "", *s_pr_en = "";  // floating praise

#define NSCORES 10
struct Score { char name[12]; int score; int diff; };
static Score s_scores[NSCORES];
static char  s_name[12];
static int   s_newrank;

#define NPART 40
static struct { int16_t x, y; int8_t vx, vy; uint8_t life; uint16_t col; } s_part[NPART];

static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }

// fast LCG for the sim (seeded from the clock at start)
static uint32_t s_rng = 0x2545f491u;
static inline unsigned rr(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 9; }
static inline int rnd(int n) { return n > 0 ? (int)(rr() % (unsigned)n) : 0; }

// ============================ persistence ====================================
#define DIRG "/sd/data/giardino"
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRG, 0777); mkdir(DIRG "/sfx", 0777); mkdir(DIRG "/custom", 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRG "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, df, b; } c = { 0x47415244u, g_lang, g_audio, g_diff, g_best };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRG "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a, df, b; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == 0x47415244u) { g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_diff = (c.df < 0 || c.df > 2) ? 1 : c.df; g_best = c.b; }
}

#define SCORES_MAGIC 0x32475347u   // 'GSG2'
struct ScoresFile { uint32_t m; uint16_t v, p; char last[12]; Score e[NSCORES]; };
static void scores_load(void)
{
    for (int i = 0; i < NSCORES; i++) { snprintf(s_scores[i].name, sizeof s_scores[i].name, "%s", "---"); s_scores[i].score = 0; s_scores[i].diff = 1; }
    snprintf(s_name, sizeof s_name, "%s", "GIARDINO");
    FILE *f = fopen(DIRG "/scores.bin", "rb");
    if (!f) return;
    ScoresFile sf;
    size_t n = fread(&sf, sizeof sf, 1, f);
    fclose(f);
    if (n == 1 && sf.m == SCORES_MAGIC) {
        sf.last[11] = 0;
        snprintf(s_name, sizeof s_name, "%s", sf.last[0] ? sf.last : "GIARDINO");
        for (int i = 0; i < NSCORES; i++) { s_scores[i] = sf.e[i]; s_scores[i].name[11] = 0; if (s_scores[i].diff < 0 || s_scores[i].diff > 2) s_scores[i].diff = 1; }
    }
}
static void scores_save(void)
{
    ensure_dirs();
    ScoresFile sf;
    memset(&sf, 0, sizeof sf);
    sf.m = SCORES_MAGIC; sf.v = 1;
    snprintf(sf.last, sizeof sf.last, "%s", s_name);
    for (int i = 0; i < NSCORES; i++) sf.e[i] = s_scores[i];
    FILE *f = fopen(DIRG "/scores.tmp", "wb");
    if (!f) return;
    size_t n = fwrite(&sf, sizeof sf, 1, f);
    fclose(f);
    if (n != 1) { remove(DIRG "/scores.tmp"); return; }
    remove(DIRG "/scores.bin");
    rename(DIRG "/scores.tmp", DIRG "/scores.bin");
}
static bool score_qualifies(int sc) { return sc > 0 && sc > s_scores[NSCORES - 1].score; }
static int score_insert(const char *name, int sc, int diff)
{
    int rank = -1;
    for (int i = 0; i < NSCORES; i++) if (sc > s_scores[i].score) { rank = i; break; }
    if (rank < 0) return -1;
    for (int j = NSCORES - 1; j > rank; j--) s_scores[j] = s_scores[j - 1];
    snprintf(s_scores[rank].name, sizeof s_scores[rank].name, "%s", (name && name[0]) ? name : "GIARDINO");
    s_scores[rank].score = sc; s_scores[rank].diff = diff;
    return rank;
}

// ============================ audio ==========================================
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "move"; case 2: return "ok";   case 3: return "back"; case 4: return "pour";
        case 5: return "bloom";case 6: return "fire"; case 7: return "hiss"; case 8: return "end";
        case 9: return "start";default: return "x";
    }
}
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1: notify__voice(&v[0], 720, 0, 0.04f); v[0].amp = 0.6f; return 1;
        case 2: notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.09f); return 2;
        case 3: notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 440, 0.05f, 0.09f); return 2;
        case 4: notify__voice(&v[0], 392, 0, 0.04f); notify__voice(&v[1], 300, 0.03f, 0.05f); v[0].amp = 0.45f; v[1].amp = 0.45f; return 2;  // gentle water trickle
        case 5: notify__voice(&v[0], 1046.5f, 0, 0.05f); notify__voice(&v[1], 1318.5f, 0.04f, 0.05f); notify__voice(&v[2], 1568, 0.09f, 0.06f); notify__voice(&v[3], 2093, 0.15f, 0.18f); return 4; // bloom sparkle (rising)
        case 6: notify__voice(&v[0], 300, 0, 0.10f); notify__voice(&v[1], 200, 0.04f, 0.12f); v[0].amp = 0.7f; return 2;  // ignite whoosh
        case 7: notify__voice(&v[0], 2200, 0, 0.10f); v[0].amp = 0.45f; return 1;  // steam hiss
        case 8: notify__voice(&v[0], 523.25f, 0, 0.16f); notify__voice(&v[1], 659.25f, 0.12f, 0.16f); notify__voice(&v[2], 783.99f, 0.24f, 0.16f); notify__voice(&v[3], 1046.5f, 0.36f, 0.20f); notify__voice(&v[4], 1318.5f, 0.50f, 0.45f); return 5; // harvest fanfare
        case 9: notify__voice(&v[0], 523.25f, 0, 0.10f); notify__voice(&v[1], 659.25f, 0.08f, 0.10f); notify__voice(&v[2], 880, 0.16f, 0.22f); return 3;
    }
    return 0;
}
static bool sfx_important(int id) { return id == 8; }
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    static char p[80];
    snprintf(p, sizeof p, DIRG "/custom/%s.wav", sfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else {
        snprintf(p, sizeof p, DIRG "/sfx/%s.wav", sfx_name(id));
        f = fopen(p, "rb");
        if (f) fclose(f);
        else { notify_voice_t v[6]; int nv = build_voices(id, v); if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return; }
    }
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
#define SFX_VER 2
static void sfx_cache_check(void)
{
    int ver = 0;
    FILE *f = fopen(DIRG "/sfx/ver.bin", "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver == SFX_VER) return;
    for (int id = 1; id <= 9; id++) { char p[80]; snprintf(p, sizeof p, DIRG "/sfx/%s.wav", sfx_name(id)); remove(p); }
    f = fopen(DIRG "/sfx/ver.bin", "wb");
    if (f) { int vv = SFX_VER; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
}
static void presynth(void)
{
    if (!g_audio) return;
    notify_voice_t v[6];
    for (int id = 1; id <= 9; id++) {
        char p[80]; snprintf(p, sizeof p, DIRG "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ text helpers ===================================
static void text_at(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void text_c(int cx, int y, int sz, uint16_t col, const char *s) { text_at(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void text_c_sh(int cx, int y, int sz, uint16_t col, const char *s) { text_c(cx + 1, y + 1, sz, mix(col, COL_BG, 205), s); text_c(cx, y, sz, col, s); }

// ============================ grid helpers ===================================
static inline bool inb(int x, int y) { return x >= 0 && x < GW && y >= 0 && y < GH; }
static inline uint8_t gget(int x, int y) { return s_grid[y * GW + x]; }
static inline void gset(int x, int y, uint8_t m) { s_grid[y * GW + x] = m; }
static inline void gswap(int x1, int y1, int x2, int y2) { uint8_t t = s_grid[y1 * GW + x1]; s_grid[y1 * GW + x1] = s_grid[y2 * GW + x2]; s_grid[y2 * GW + x2] = t; }
static inline bool soft(int x, int y) { uint8_t m = gget(x, y); return m == M_EMPTY || m == M_WATER || m == M_STEAM; }   // sand/seed can sink through
static inline bool flammable(uint8_t m) { return m == M_PLANT || m == M_SEED || m == M_FLOWER; }
static bool neigh4(int x, int y, uint8_t m)
{
    if (inb(x, y - 1) && gget(x, y - 1) == m) return true;
    if (inb(x, y + 1) && gget(x, y + 1) == m) return true;
    if (inb(x - 1, y) && gget(x - 1, y) == m) return true;
    if (inb(x + 1, y) && gget(x + 1, y) == m) return true;
    return false;
}

// ============================ particles ======================================
static void spawn_burst(int px, int py, uint16_t col, int n)
{
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < NPART; i++) {
            if (s_part[i].life == 0) {
                s_part[i].x = (int16_t)px; s_part[i].y = (int16_t)py;
                s_part[i].vx = (int8_t)(rnd(5) - 2); s_part[i].vy = (int8_t)(-2 - rnd(3));
                s_part[i].life = (uint8_t)(14 + rnd(10)); s_part[i].col = col;
                break;
            }
        }
    }
}
static void particles_step(void)
{
    for (int i = 0; i < NPART; i++) {
        if (!s_part[i].life) continue;
        s_part[i].x += s_part[i].vx; s_part[i].y += s_part[i].vy;
        if (s_part[i].vy < 6) s_part[i].vy++;     // gravity
        s_part[i].life--;
    }
}

// ============================ simulation =====================================
static const int LAVA_RATE[3] = { 4, 8, 14 };    // spawn weight per tick (higher = more lava)
static const int GROW[3]      = { 16, 11, 8 };   // growth chance %

static int count_flowers(void) { int n = 0; if (s_grid) for (int i = 0; i < GW * GH; i++) if (s_grid[i] == M_FLOWER) n++; return n; }

static uint16_t flower_col(int x, int y)
{
    switch (((x * 7 + y * 13) >> 1) % 5) {
        case 0: return rgb(244, 110, 170);
        case 1: return rgb(236, 72, 72);
        case 2: return rgb(248, 212, 80);
        case 3: return rgb(186, 116, 232);
        default: return rgb(245, 245, 245);
    }
}
static bool drink_near(int x, int y)             // consume one adjacent water cell; returns true if drank
{
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        if (!dx && !dy) continue;
        if (inb(x + dx, y + dy) && gget(x + dx, y + dy) == M_WATER) { gset(x + dx, y + dy, M_EMPTY); return true; }
    }
    return false;
}
static void bloom(int x, int y)
{
    gset(x, y, M_FLOWER);
    if (s_now - s_lastbloom <= 1500) { if (s_combo < 9) s_combo++; } else s_combo = 1;
    s_lastbloom = s_now; s_combo_ms = 1700;
    s_score += 50 * (s_combo < 1 ? 1 : s_combo);
    spawn_burst(x * CELL + 1, PLAY_Y0 + y * CELL, flower_col(x, y), 4 + s_combo);
    if (s_combo >= 3) {
        s_pr_ms = 850; s_pr_x = x * CELL; s_pr_y = PLAY_Y0 + y * CELL;
        if (s_pr_x < 28) s_pr_x = 28;
        if (s_pr_x > W - 28) s_pr_x = W - 28;
        static const char *PI[4] = { "BELLO!", "FIORE!", "WOW!", "COMBO!" };
        static const char *PE[4] = { "NICE!", "BLOOM!", "WOW!", "COMBO!" };
        int kk = s_combo & 3; s_pr_it = PI[kk]; s_pr_en = PE[kk];
    }
    sfx(5);
}
static void sim(void)
{
    if (!s_grid) return;
    s_tick++;
    bool ltr = (s_tick & 1);
    int fcount = 0;
    for (int y = GH - 1; y >= 0; y--) {
        for (int xi = 0; xi < GW; xi++) {
            int x = ltr ? xi : (GW - 1 - xi);
            uint8_t m = gget(x, y);
            if (m == M_FLOWER) fcount++;
            if (m == M_EMPTY || m == M_STONE) continue;

            if (m == M_SAND) {
                if (y < GH - 1 && soft(x, y + 1)) { gswap(x, y, x, y + 1); continue; }
                int dxx = rnd(2) ? 1 : -1;
                if (y < GH - 1 && inb(x + dxx, y + 1) && soft(x + dxx, y + 1)) gswap(x, y, x + dxx, y + 1);
                else if (y < GH - 1 && inb(x - dxx, y + 1) && soft(x - dxx, y + 1)) gswap(x, y, x - dxx, y + 1);
            } else if (m == M_WATER) {
                if (neigh4(x, y, M_FIRE)) { gset(x, y, M_STEAM); continue; }
                if (neigh4(x, y, M_LAVA)) { gset(x, y, M_STEAM); continue; }
                if (y < GH - 1 && gget(x, y + 1) == M_EMPTY) { gswap(x, y, x, y + 1); continue; }
                int dxx = rnd(2) ? 1 : -1;
                if (y < GH - 1 && inb(x + dxx, y + 1) && gget(x + dxx, y + 1) == M_EMPTY) { gswap(x, y, x + dxx, y + 1); continue; }
                if (y < GH - 1 && inb(x - dxx, y + 1) && gget(x - dxx, y + 1) == M_EMPTY) { gswap(x, y, x - dxx, y + 1); continue; }
                if (inb(x + dxx, y) && gget(x + dxx, y) == M_EMPTY) { gswap(x, y, x + dxx, y); continue; }
                if (inb(x - dxx, y) && gget(x - dxx, y) == M_EMPTY) { gswap(x, y, x - dxx, y); continue; }
            } else if (m == M_SEED) {
                if (y < GH - 1 && soft(x, y + 1)) { gswap(x, y, x, y + 1); continue; }
                if (neigh4(x, y, M_WATER)) { gset(x, y, M_PLANT); drink_near(x, y); continue; }
            } else if (m == M_PLANT) {
                if (y > 0 && gget(x, y - 1) == M_EMPTY && rnd(100) < GROW[g_diff] && drink_near(x, y)) {
                    if (rnd(100) < 16) bloom(x, y - 1);
                    else gset(x, y - 1, M_PLANT);
                }
            } else if (m == M_FIRE) {
                if (neigh4(x, y, M_WATER)) { gset(x, y, M_STEAM); continue; }
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && inb(x + dx, y + dy) && flammable(gget(x + dx, y + dy)) && rnd(100) < 28) gset(x + dx, y + dy, M_FIRE);
                if (y > 0 && gget(x, y - 1) == M_EMPTY && rnd(100) < 30) { gswap(x, y, x, y - 1); continue; }
                if (rnd(100) < 22) gset(x, y, M_EMPTY);
            } else if (m == M_STEAM) {
                if (rnd(100) < 8) { gset(x, y, M_EMPTY); continue; }
                if (y > 0 && gget(x, y - 1) == M_EMPTY) gswap(x, y, x, y - 1);
            } else if (m == M_LAVA) {
                if (neigh4(x, y, M_WATER)) { gset(x, y, M_STONE); continue; }
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && inb(x + dx, y + dy) && flammable(gget(x + dx, y + dy))) gset(x + dx, y + dy, M_FIRE);
                if (rnd(100) < 55 && y < GH - 1 && soft(x, y + 1)) { gswap(x, y, x, y + 1); continue; }
            }
        }
    }
    s_flowers = fcount;
    // hazard: lava rains, intensifying through the round (and hard during a lava storm)
    int elapsed = s_round_ms ? s_time_ms * 100 / s_round_ms : 0;     // 0..100
    int rate = LAVA_RATE[g_diff] + elapsed / 8 + (s_weather == 2 ? 45 : 0);
    if (rnd(1000) < rate) { int lx = rnd(GW); if (gget(lx, 0) == M_EMPTY) { gset(lx, 0, M_LAVA); if (rnd(4) == 0) sfx(6); } }
    if (s_weather == 1) for (int k = 0; k < 3; k++) { int wx = rnd(GW); if (gget(wx, 0) == M_EMPTY) gset(wx, 0, M_WATER); }   // rain
    particles_step();
}
static void trigger_weather(void)
{
    s_weather = (rnd(100) < 55) ? 1 : 2;            // rain a bit more common than a lava storm
    s_weather_ms = 5000;
    s_weather_next = 13000 + rnd(8000);
    s_banner_ms = 1700;
    if (s_weather == 1) { s_ban_it = "PIOGGIA!"; s_ban_en = "RAIN!"; sfx(7); }
    else { s_ban_it = "TEMPESTA DI LAVA!"; s_ban_en = "LAVA STORM!"; sfx(6); }
}

// ============================ brush ==========================================
static void emit(void)
{
    if (!s_grid) return;
    uint8_t m = (s_tool == 0) ? M_WATER : (s_tool == 1) ? M_SEED : (s_tool == 2) ? M_STONE : M_EMPTY;
    int rad = (s_tool == 1) ? s_brush : (2 + s_brush * 2); // seeds drop singly/paired; others a small/big disc
    if (s_tool == 1 && (s_tick & 3)) return;               // meter seeds so they don't flood
    for (int dy = -rad; dy <= rad; dy++) for (int dx = -rad; dx <= rad; dx++) {
        if (dx * dx + dy * dy > rad * rad + 1) continue;
        int x = s_bx + dx, y = s_by + dy;
        if (!inb(x, y)) continue;
        if (m == M_EMPTY) { if (gget(x, y) != M_STONE || s_tool == 3) gset(x, y, M_EMPTY); }
        else if (m == M_WATER) { if (gget(x, y) == M_EMPTY) gset(x, y, M_WATER); }
        else if (m == M_SEED)  { if (gget(x, y) == M_EMPTY) gset(x, y, M_SEED); }
        else if (m == M_STONE) { gset(x, y, M_STONE); }
    }
}

// ============================ lifecycle ======================================
static void set_hint(void);
static void go(int s);

static void grid_alloc(void) { if (!s_grid) s_grid = (uint8_t *)heap_caps_malloc(GW * GH, MALLOC_CAP_8BIT); }
static void grid_free(void) { if (s_grid) { free(s_grid); s_grid = nullptr; } }

static void new_game(void)
{
    grid_alloc();
    if (!s_grid) { go(ST_MENU); return; }
    memset(s_grid, M_EMPTY, GW * GH);
    for (int y = GH - 4; y < GH; y++) for (int x = 0; x < GW; x++) gset(x, y, M_SAND);   // soil floor
    for (int i = 0; i < NPART; i++) s_part[i].life = 0;
    s_score = 0; s_time_ms = 0; s_round_ms = 90000; s_tool = 0; s_pour = false; s_brush = 0;
    s_bx = GW / 2; s_by = 6; s_simacc = 0;
    s_combo = 0; s_combo_ms = 0; s_lastbloom = 0; s_flowers = 0;
    s_bee_x = W / 2; s_bee_y = PLAY_Y0 + 28; s_bee_t = 0; s_pr_ms = 0;
    s_weather = 0; s_weather_ms = 0; s_weather_next = 11000; s_banner_ms = 0;
    for (int i = 0; i < 12; i++) { s_mote[i].x = (int16_t)rnd(W); s_mote[i].y = (int16_t)(PLAY_Y0 + rnd(PLAY_H)); s_mote[i].ph = (uint8_t)rnd(255); }
    s_screen = ST_PLAY;
    sfx(9);
}
static void end_round(void)
{
    s_score += count_flowers() * 20;                         // surviving flowers bonus
    s_newbest = (s_score > g_best); s_bestplayed = false;
    s_qualify = score_qualifies(s_score);
    if (s_newbest) { g_best = s_score; cfg_write(); }
    s_over_ms = 900;                                  // grid kept for the harvest backdrop; freed on leaving OVER
    for (int i = 0; i < NPART; i++) s_part[i].life = 0;
    for (int i = 0; i < 10; i++) spawn_burst(20 + rnd(W - 40), PLAY_Y0 + 10 + rnd(40), flower_col(rnd(GW), rnd(GH)), 3);
    s_screen = ST_OVER;
    set_hint();
    sfx(8);
}

// ============================ rendering ======================================
static uint16_t mat_col(uint8_t m, int x, int y)
{
    switch (m) {
        case M_SAND:  return rgb(206 - ((x + y) & 1) * 12, 178, 104);
        case M_WATER: { int s = ((x * 5 + y * 3 + (int)s_tick) >> 1) & 7; return rgb(44 + s * 3, 112 + s * 4, 206 + s * 5); }
        case M_SEED:  return rgb(120, 86, 46);
        case M_PLANT: { int s = (x * 3 + y * 7) & 3; return rgb(48 + s * 8, 150 + s * 12, 58 + s * 6); }
        case M_FLOWER:return flower_col(x, y);
        case M_FIRE:  { int s = (x + y + (int)s_tick) & 3; return rgb(255, 110 + s * 32, 18 + s * 16); }
        case M_STONE: return rgb(112, 112, 126);
        case M_STEAM: return rgb(188, 196, 210);
        case M_LAVA:  { int s = (x + y + (int)s_tick) & 3; return rgb(246, 84 + s * 22, 22 + s * 8); }
        default:      return COL_BG;
    }
}
static void draw_garden(void)
{
    int ch = nucleo_app_content_height();
    int prog = s_round_ms ? s_time_ms * 256 / s_round_ms : 0;          // 0..256 day -> dusk
    if (prog > 256) prog = 256;
    uint16_t skyTop = mix(rgb(46, 96, 156), rgb(28, 18, 52), prog);
    uint16_t skyBot = mix(rgb(120, 168, 205), rgb(116, 56, 54), prog);
    for (int by = 0; by < PLAY_H; by += 4) d.fillRect(0, PLAY_Y0 + by, W, 4, mix(skyTop, skyBot, by * 256 / PLAY_H));
    if (prog > 150)
        for (int i = 0; i < 14; i++) { int sx = (i * 53 + 11) % W, sy = PLAY_Y0 + (i * 29 + 7) % 46; if (((s_anim + i * 3) & 15) < 12) d.drawPixel(sx, sy, mix(skyTop, COL_WHITE, 60 + (prog - 150))); }
    for (int i = 0; i < 12; i++) if (((s_mote[i].ph + s_anim) & 15) < 8) d.drawPixel(s_mote[i].x, s_mote[i].y, mix(skyBot, COL_WHITE, 120));

    if (!s_grid) text_c(W / 2, PLAY_Y0 + 40, 1, COL_RED, tx("Memoria insufficiente", "Out of memory"));
    else for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
        uint8_t m = gget(x, y);
        if (m == M_EMPTY) continue;
        d.fillRect(x * CELL, PLAY_Y0 + y * CELL, CELL, CELL, mat_col(m, x, y));
        if (m == M_FLOWER && ((x * 3 + y * 5 + (int)s_anim) & 7) < 2) d.drawPixel(x * CELL, PLAY_Y0 + y * CELL, COL_WHITE);   // twinkle
    }

    if (s_weather == 1)
        for (int i = 0; i < 16; i++) { int rx = (i * 37 + (int)s_anim * 9) % W, ry = PLAY_Y0 + (i * 23 + (int)s_anim * 13) % (PLAY_H - 6); d.drawLine(rx, ry, rx - 1, ry + 5, rgb(120, 170, 230)); }

    for (int i = 0; i < NPART; i++) if (s_part[i].life) d.fillRect(s_part[i].x, s_part[i].y, 2, 2, mix(s_part[i].col, rgb(40, 50, 90), 256 - s_part[i].life * 10));

    int bcx = s_bx * CELL + 1, bcy = PLAY_Y0 + s_by * CELL;
    uint16_t tc = (s_tool == 0) ? COL_CYAN : (s_tool == 1) ? COL_GREEN : (s_tool == 2) ? COL_GREY : COL_RED;
    int rad = (s_brush ? 10 : 6) + (s_pour ? (int)((s_anim / 2) % 3) : 0);
    d.drawCircle(bcx, bcy, rad, s_pour ? COL_WHITE : tc);
    d.drawCircle(bcx, bcy, rad - 1, tc);
    d.drawFastHLine(bcx - 3, bcy, 7, tc); d.drawFastVLine(bcx, bcy - 3, 7, tc);
    if (s_pour && s_tool == 0) for (int i = 0; i < 3; i++) { int dyo = ((int)s_anim * 3 + i * 5) % 14; d.fillRect(bcx - 2 + i * 2, bcy + 4 + dyo, 1, 2, COL_CYAN); }

    { int bx = (int)s_bee_x, by = (int)s_bee_y;                          // bee mascot
      d.fillRect(bx - 1, by - 1, 3, 2, rgb(250, 210, 40));
      d.drawFastVLine(bx, by - 1, 2, rgb(40, 30, 10));
      if (s_anim & 1) { d.drawPixel(bx - 2, by - 2, COL_WHITE); d.drawPixel(bx + 2, by - 2, COL_WHITE); }
      else { d.drawPixel(bx - 2, by, COL_WHITE); d.drawPixel(bx + 2, by, COL_WHITE); } }
    if (s_pr_ms > 0) text_c_sh(s_pr_x, s_pr_y - (850 - s_pr_ms) / 45, 1, ((s_anim >> 1) & 1) ? COL_WHITE : COL_AMBER, g_lang ? s_pr_en : s_pr_it);

    if (s_combo >= 2) {
        uint16_t cc = ((s_anim >> 1) & 1) ? COL_WHITE : (s_combo >= 6 ? COL_RED : (s_combo >= 4 ? COL_AMBER : COL_GREEN));
        char cb[16]; snprintf(cb, sizeof cb, "COMBO x%d", s_combo);
        text_c_sh(W / 2, PLAY_Y0 + 2, 2, cc, cb);
    }
    if (s_banner_ms > 0 && ((s_anim >> 1) & 1)) {
        const char *w = g_lang ? s_ban_en : s_ban_it;
        int ww = (int)strlen(w) * 12 + 16;
        uint16_t bc = (s_weather == 2) ? COL_RED : COL_CYAN;
        d.fillRoundRect(W / 2 - ww / 2, PLAY_Y0 + 34, ww, 22, 5, rgb(16, 22, 30));
        d.drawRoundRect(W / 2 - ww / 2, PLAY_Y0 + 34, ww, 22, 5, bc);
        text_c(W / 2, PLAY_Y0 + 39, 2, COL_WHITE, w);
    }

    // HUD
    d.fillRect(0, 0, W, 13, COL_BG);
    char b[24];
    snprintf(b, sizeof b, "%d", s_score);
    text_at(4, 3, 1, COL_AMBER, b);
    d.fillCircle(46, 6, 2, rgb(244, 110, 170)); d.drawPixel(46, 6, COL_WHITE);
    snprintf(b, sizeof b, "%d", s_flowers);
    text_at(52, 3, 1, COL_GREEN, b);
    const char *tn[4] = { tx("ACQUA", "WATER"), tx("SEME", "SEED"), tx("PIETRA", "STONE"), tx("SCAVA", "DIG") };
    d.fillRoundRect(96, 2, 9, 9, 2, tc);
    text_at(108, 3, 1, tc, tn[s_tool]);
    int tox = 108 + (int)strlen(tn[s_tool]) * 6 + 4;
    d.drawCircle(tox + 3, 6, s_brush ? 3 : 2, COL_DIM);            // brush-size pip
    if (s_pour) text_at(tox + 9, 3, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_DIM, ">");
    int left = (s_round_ms - s_time_ms) / 1000;
    if (left < 0) left = 0;
    snprintf(b, sizeof b, "%d:%02d", left / 60, left % 60);
    uint16_t tcl = left <= 10 ? COL_RED : (left <= 30 ? COL_AMBER : COL_GREEN);
    text_at(W - 4 - (int)strlen(b) * 6, 3, 1, tcl, b);
    int barw = W - 8, fillw = s_round_ms ? barw * (s_round_ms - s_time_ms) / s_round_ms : 0;
    d.fillRect(4, 12, barw, 1, rgb(30, 36, 46));
    if (fillw > 0) d.fillRect(4, 12, fillw, 1, tcl);
    (void)ch;
}

// ============================ front-end screens ==============================
static void draw_menu(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    for (int b = 0; b < ch; b += 4) d.fillRect(0, b, W, 4, mix(rgb(16, 22, 30), rgb(28, 44, 34), b * 256 / ch));
    int lw = (int)strlen("GIARDINO") * 18;
    text_at((W - lw) / 2 + 1, 5, 3, rgb(20, 50, 24), "GIARDINO");
    text_at((W - lw) / 2, 4, 3, COL_GREEN, "GIARDINO");
    text_c(W / 2, 30, 1, COL_DIM, tx("di sabbia", "of sand"));
    for (int i = 0; i < 5; i++) { int px = (i * 47 + (int)s_anim * 2) % W, py = (i * 31 + (int)s_anim) % ch; d.drawPixel(px, py, mix(rgb(28, 44, 34), flower_col(i, i * 2), 170)); }  // drifting pollen
    { int mx = W - 24, my = 12;                                          // mascot flower by the title
      d.drawFastVLine(mx, my, 12, COL_GREEN);
      d.fillCircle(mx, my, 4, rgb(244, 110, 170)); d.fillCircle(mx, my, 2, rgb(248, 212, 80));
      d.drawPixel(mx - 4, my + 6, COL_GREEN); d.drawPixel(mx + 4, my + 5, COL_GREEN); }
    const char *items[4] = { tx("Gioca", "Play"), tx("Come si gioca", "How to play"), tx("Classifica", "Leaderboard"), tx("Impostazioni", "Settings") };
    int selw = (int)strlen(items[s_msel]) * 12;
    int capx = (W - selw) / 2 - 12;
    d.fillRoundRect(capx, (int)s_capY, selw + 24, 19, 6, COL_CAP);
    d.fillRoundRect(capx + 4, (int)s_capY + 4, 3, 11, 1, COL_GREEN);
    for (int i = 0; i < 4; i++) text_c(W / 2, 48 + i * 18, 2, i == s_msel ? COL_WHITE : COL_GREY, items[i]);
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    const char *titles[3] = { tx("Obiettivo", "Objective"), tx("Comandi", "Controls"), tx("Materiali", "Materials") };
    text_c(W / 2, 8, 2, COL_GREEN, titles[s_help]);
    d.drawFastHLine(20, 28, W - 40, COL_DIM);
    if (s_help == 0) {
        text_at(14, 40, 1, COL_WHITE, tx("Pianta semi, versa acqua:", "Plant seeds, pour water:"));
        text_at(14, 54, 1, COL_GREEN, tx("crescono e SBOCCIANO = punti.", "they grow and BLOOM = points."));
        text_at(14, 74, 1, COL_AMBER, tx("La lava piove dal cielo.", "Lava rains from the sky."));
        text_at(14, 88, 1, COL_CYAN, tx("Spegnila: acqua+lava = pietra.", "Quench it: water+lava = stone."));
    } else if (s_help == 1) {
        text_at(14, 40, 1, COL_GREY, tx("Frecce", "Arrows"));
        text_at(110, 40, 1, COL_WHITE, tx("muovi pennello", "move brush"));
        text_at(14, 56, 1, COL_GREY, "SPAZIO");
        text_at(110, 56, 1, COL_CYAN, tx("versa on/off", "pour on/off"));
        text_at(14, 72, 1, COL_GREY, "W / E");
        text_at(110, 72, 1, COL_WHITE, tx("cambia materiale", "change material"));
        text_at(14, 88, 1, COL_GREY, "1-4");
        text_at(110, 88, 1, COL_WHITE, tx("scelta diretta", "pick directly"));
    } else {
        text_at(14, 40, 1, COL_CYAN,  tx("ACQUA: nutre, spegne", "WATER: feeds, quenches"));
        text_at(14, 54, 1, COL_GREEN, tx("SEME: su terra + acqua", "SEED: on soil + water"));
        text_at(14, 68, 1, COL_GREY,  tx("PIETRA: barriera", "STONE: barrier"));
        text_at(14, 82, 1, COL_AMBER, tx("SCAVA: rimuove", "DIG: removes"));
        text_at(14, 96, 1, COL_RED,   tx("Fiori vivi a fine = bonus", "Flowers alive at end = bonus"));
    }
    int gap = 12, x0 = W / 2 - 1 * gap;
    for (int i = 0; i < 3; i++) d.fillCircle(x0 + i * gap, ch - 10, i == s_help ? 3 : 2, i == s_help ? COL_GREEN : COL_DIM);
}
static void draw_settings(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    text_c(W / 2, 8, 2, COL_GREEN, tx("Impostazioni", "Settings"));
    const char *names[4] = { tx("Lingua", "Language"), "Audio", tx("Difficolta", "Difficulty"), tx("Indietro", "Back") };
    const char *diffn[3] = { tx("Facile", "Easy"), tx("Normale", "Normal"), tx("Difficile", "Hard") };
    char v[3][16];
    snprintf(v[0], 16, "%s", g_lang ? "English" : "Italiano");
    snprintf(v[1], 16, "%s", g_audio ? "On" : "Off");
    snprintf(v[2], 16, "%s", diffn[g_diff]);
    d.fillRoundRect(16, (int)s_capY, W - 32, 20, 5, COL_CAP);
    for (int i = 0; i < 4; i++) {
        int y = 32 + i * 22;
        text_at(26, y, 2, i == s_setsel ? COL_WHITE : COL_GREY, names[i]);
        if (i < 3) text_at(W - 20 - (int)strlen(v[i]) * 6, y + 4, 1, COL_AMBER, v[i]);
    }
}
static void draw_card(int prog)
{
    if (prog > 1000) prog = 1000;
    if (prog < 0) prog = 0;
    char b[28];
    snprintf(b, sizeof b, "%s %d", tx("Punteggio", "Score"), s_score * prog / 1000);
    text_c_sh(W / 2, 80, 2, COL_WHITE, b);
    snprintf(b, sizeof b, "%s %d%s", tx("Record", "Best"), g_best * prog / 1000, s_newbest ? "  *" : "");
    text_c_sh(W / 2, 100, 1, s_newbest ? COL_GREEN : COL_AMBER, b);
}
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    if (s_grid) {
        for (int by = 0; by < PLAY_H; by += 4) d.fillRect(0, PLAY_Y0 + by, W, 4, mix(rgb(28, 18, 52), rgb(112, 56, 54), by * 256 / PLAY_H));
        for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) { uint8_t m = gget(x, y); if (m != M_EMPTY) d.fillRect(x * CELL, PLAY_Y0 + y * CELL, CELL, CELL, mix(mat_col(m, x, y), COL_BG, 70)); }
        d.fillRect(0, 0, W, PLAY_Y0, COL_BG);
        d.fillRect(0, PLAY_Y0 + PLAY_H, W, ch - PLAY_Y0 - PLAY_H, COL_BG);
    } else for (int by = 0; by < ch; by += 4) d.fillRect(0, by, W, 4, mix(rgb(16, 22, 30), rgb(28, 44, 34), by * 256 / ch));

    for (int i = 0; i < NPART; i++) if (s_part[i].life) d.fillRect(s_part[i].x, s_part[i].y, 2, 2, mix(s_part[i].col, rgb(30, 30, 50), 256 - s_part[i].life * 9));

    int sh = (int)(120 + 110 * sinf(s_anim * 0.2f));
    text_c_sh(W / 2, 18, 3, mix(COL_GREEN, COL_WHITE, sh), tx("RACCOLTO!", "HARVEST!"));
    d.fillRoundRect(28, 60, W - 56, 52, 8, rgb(18, 28, 22));
    d.drawRoundRect(28, 60, W - 56, 52, 8, COL_GREEN);
    char fb[24]; snprintf(fb, sizeof fb, "%s %d", tx("Fiori", "Flowers"), s_flowers);
    text_c(W / 2, 66, 1, COL_GREEN, fb);
    draw_card((900 - s_over_ms) * 1000 / 600);
}
static void draw_scores(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    text_c(W / 2, 4, 2, COL_GREEN, tx("Classifica", "Leaderboard"));
    d.drawFastHLine(10, 22, W - 20, COL_DIM);
    static const char DI[2][3] = { { 'F', 'N', 'D' }, { 'E', 'N', 'H' } };
    int y = 26;
    for (int i = 0; i < NSCORES; i++) {
        bool hot = (i == s_newrank);
        if (hot) d.fillRoundRect(4, y - 1, W - 8, 10, 2, COL_CAP);
        uint16_t c = hot ? COL_WHITE : (i < 3 ? COL_AMBER : COL_GREY);
        char r[6]; snprintf(r, sizeof r, "%d.", i + 1);
        text_at(6, y, 1, c, r);
        text_at(28, y, 1, c, s_scores[i].name);
        char sc[16]; snprintf(sc, sizeof sc, "%d", s_scores[i].score);
        text_at(W - 18 - (int)strlen(sc) * 6, y, 1, c, sc);
        int di = s_scores[i].diff; if (di < 0 || di > 2) di = 1;
        char dd[2] = { DI[g_lang ? 1 : 0][di], 0 };
        text_at(W - 12, y, 1, hot ? COL_GREEN : COL_DIM, dd);
        y += 9;
    }
}
static void draw_name(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    text_c(W / 2, 14, 2, COL_GREEN, tx("NUOVO RECORD!", "NEW RECORD!"));
    char b[24]; snprintf(b, sizeof b, "%s %d", tx("Punteggio", "Score"), s_score);
    text_c(W / 2, 44, 1, COL_GREY, b);
    text_c(W / 2, 60, 1, COL_GREY, tx("Scrivi il tuo nome:", "Enter your name:"));
    d.fillRoundRect(40, 74, W - 80, 22, 4, rgb(18, 30, 22));
    d.drawRoundRect(40, 74, W - 80, 22, 4, COL_GREEN);
    text_at(48, 79, 2, COL_WHITE, s_name);
    int cxp = 48 + (int)strlen(s_name) * 12;
    if ((s_anim >> 3) & 1) d.fillRect(cxp + 1, 78, 9, 16, COL_GREEN);
    text_c(W / 2, ch - 12, 1, COL_DIM, tx("INVIO conferma", "ENTER confirm"));
}
static void on_draw(void)
{
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_garden(); break;
        case ST_OVER: draw_over(); break;
        case ST_SCORES: draw_scores(); break;
        case ST_NAME: draw_name(); break;
        default: break;
    }
}

// ============================ input ==========================================
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU: nucleo_app_set_hint(tx("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_HELP: nucleo_app_set_hint(tx("SX/DX pagine  Esc indietro", "LEFT/RIGHT pages  Esc back")); break;
        case ST_SET:  nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  Esc", "UP/DN  ENTER change  Esc")); break;
        case ST_PLAY: nucleo_app_set_hint(tx("Frecce muovi  SPAZIO versa  W/E mat.  S dim.", "Arrows move  SPACE pour  W/E mat.  S size")); break;
        case ST_OVER: nucleo_app_set_hint(tx("premi un tasto", "press any key")); break;
        case ST_SCORES: nucleo_app_set_hint(tx("Esc indietro", "Esc back")); break;
        case ST_NAME: nucleo_app_set_hint(tx("Scrivi  INVIO ok  CANC canc  Esc", "Type  ENTER ok  DEL erase  Esc")); break;
        default: break;
    }
}
static float cap_target(void)
{
    if (s_screen == ST_MENU) return (float)(45 + s_msel * 18);
    if (s_screen == ST_SET)  return (float)(29 + s_setsel * 22);
    return s_capY;
}
static void go(int s) { s_screen = s; s_capY = cap_target(); set_hint(); nucleo_app_request_draw(); }

static void menu_select(void)
{
    sfx(2);
    if (s_msel == 0) new_game();
    else if (s_msel == 1) { s_help = 0; go(ST_HELP); }
    else if (s_msel == 2) { s_newrank = -1; go(ST_SCORES); }
    else { s_setsel = 0; go(ST_SET); }
    set_hint();
    nucleo_app_request_draw();
}
static void name_commit(void)
{
    if (!s_name[0]) snprintf(s_name, sizeof s_name, "%s", tx("GIARDINO", "GARDEN"));
    s_newrank = score_insert(s_name, s_score, g_diff);
    scores_save();
    sfx(2);
    go(ST_SCORES);
}
static void settings_change(void)
{
    switch (s_setsel) {
        case 0: g_lang ^= 1; cfg_write(); sfx(2); break;
        case 1: g_audio ^= 1; cfg_write(); sfx(2); break;
        case 2: g_diff = (g_diff + 1) % 3; cfg_write(); sfx(2); break;
        default: sfx(3); go(ST_MENU); return;
    }
    set_hint();
    nucleo_app_request_draw();
}
static void move_brush(int dx, int dy)
{
    s_bx += dx; s_by += dy;
    if (s_bx < 0) s_bx = 0;
    if (s_bx > GW - 1) s_bx = GW - 1;
    if (s_by < 0) s_by = 0;
    if (s_by > GH - 1) s_by = GH - 1;
    nucleo_app_request_draw();
}
static void on_key(int k, char ch)
{
    switch (s_screen) {
        case ST_MENU:
            if (k == NK_UP)        { s_msel = (s_msel + 3) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) menu_select();
            return;
        case ST_HELP:
            if (k == NK_RIGHT)      { s_help = (s_help + 1) % 3; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) { sfx(3); go(ST_MENU); }
            return;
        case ST_SET:
            if (k == NK_UP)        { s_setsel = (s_setsel + 3) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change();
            return;
        case ST_PLAY:
            if (k == NK_UP)         move_brush(0, -2);
            else if (k == NK_DOWN)  move_brush(0, 2);
            else if (k == NK_RIGHT) move_brush(2, 0);
            else if (ch == ' ')     { s_pour = !s_pour; sfx(s_pour ? 4 : 3); nucleo_app_request_draw(); }
            else if (ch == 'e' || ch == 'E') { s_tool = (s_tool + 1) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (ch == 'w' || ch == 'W') { s_tool = (s_tool + 3) % 4; sfx(1); nucleo_app_request_draw(); }
            else if (ch == 's' || ch == 'S') { s_brush ^= 1; sfx(1); nucleo_app_request_draw(); }   // brush size
            else if (ch >= '1' && ch <= '4') { s_tool = ch - '1'; sfx(1); nucleo_app_request_draw(); }
            return;
        case ST_OVER:
            (void)k; (void)ch;
            if (s_over_ms == 0) { grid_free(); if (s_qualify) go(ST_NAME); else { sfx(2); s_msel = 0; go(ST_MENU); } }
            return;
        case ST_SCORES:
            (void)k; (void)ch; sfx(3); s_msel = 0; go(ST_MENU);
            return;
        case ST_NAME:
            if (k == NK_ENTER) name_commit();
            else if (k == NK_DEL) { int l = (int)strlen(s_name); if (l > 0) s_name[l - 1] = 0; sfx(3); nucleo_app_request_draw(); }
            else if (k == NK_CHAR && ch >= 32 && ch < 127) { int l = (int)strlen(s_name); if (l < 11) { s_name[l] = ch; s_name[l + 1] = 0; sfx(1); } nucleo_app_request_draw(); }
            return;
        default: (void)k; (void)ch; return;
    }
}
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_HELP: s_help = (s_help + 2) % 3; sfx(1); nucleo_app_request_draw(); break;
            case ST_SET:  settings_change(); break;
            case ST_PLAY: move_brush(-2, 0); break;
            case ST_NAME: { int l = (int)strlen(s_name); if (l > 0) s_name[l - 1] = 0; sfx(3); nucleo_app_request_draw(); } break;
            default: break;
        }
        return true;
    }
    switch (s_screen) {
        case ST_MENU: return false;
        case ST_NAME: name_commit(); return true;
        case ST_PLAY: case ST_OVER: grid_free(); sfx(3); s_msel = 0; go(ST_MENU); return true;
        case ST_HELP: case ST_SET: case ST_SCORES: sfx(3); s_msel = 0; go(ST_MENU); return true;
        default: return false;
    }
}

// ============================ poll / lifecycle ===============================
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last);
    if (dt < 0) dt = 0;
    if (dt > 250) dt = 250;
    s_last = s_now;
    bool menu_glide = false;            // true only while the selection capsule is still easing

    if (s_screen == ST_PLAY) {
        s_simacc += dt;
        s_time_ms += dt;
        while (s_simacc >= 45) { s_simacc -= 45; if (s_pour) emit(); sim(); if (s_screen != ST_PLAY) break; }   // ~22 Hz
        dec(&s_combo_ms, dt); if (s_combo_ms == 0) s_combo = 0;
        dec(&s_banner_ms, dt);
        dec(&s_pr_ms, dt);
        if (s_weather) { dec(&s_weather_ms, dt); if (s_weather_ms == 0) s_weather = 0; }
        else { s_weather_next -= dt; if (s_weather_next <= 0) trigger_weather(); }
        if (s_time_ms >= s_round_ms) { end_round(); return true; }
    } else if (s_screen == ST_OVER) {
        dec(&s_over_ms, dt);
        if (s_over_ms == 0 && s_newbest && !s_bestplayed) { s_bestplayed = true; sfx(2); }
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        float tgt = cap_target();
        if (s_capY != tgt) {                                 // ease the capsule, then snap and rest
            s_capY += (tgt - s_capY) * 0.45f;
            if (fabsf(tgt - s_capY) < 0.4f) s_capY = tgt;
            menu_glide = true;
        }
    }

    // A settled menu is fully static: stop the ~30 Hz duplicate-frame re-blit that was the idle
    // flicker. It repaints on input (request_draw) and while the capsule glides to a new pick.
    bool menu = (s_screen == ST_MENU || s_screen == ST_SET);
    bool animated = (s_screen != ST_HELP && s_screen != ST_SCORES) && (!menu || menu_glide);
    if (!animated) return false;
    if (s_now - s_frame < 33) return false;
    s_frame = s_now;
    s_anim++;
    if (s_screen == ST_PLAY) {
        for (int i = 0; i < 12; i++) { s_mote[i].x = (int16_t)((s_mote[i].x + 1) % W); if ((s_anim & 3) == 0) { int ny = s_mote[i].y - PLAY_Y0 + 1; if (ny >= PLAY_H) ny = 0; s_mote[i].y = (int16_t)(PLAY_Y0 + ny); } }
        s_bee_t++;
        s_bee_x = W / 2 + 92.0f * sinf(s_bee_t * 0.013f);
        s_bee_y = PLAY_Y0 + 30 + 20.0f * sinf(s_bee_t * 0.022f);
        if (s_grid) { int bbx = (int)s_bee_x / CELL, bby = ((int)s_bee_y - PLAY_Y0) / CELL;
            if (inb(bbx, bby) && gget(bbx, bby) == M_PLANT && bby > 0 && gget(bbx, bby - 1) == M_EMPTY && rnd(100) < 4) bloom(bbx, bby - 1); }
    } else if (s_screen == ST_OVER) {
        if ((s_anim % 6) == 0) spawn_burst(20 + rnd(W - 40), PLAY_Y0 + 8 + rnd(50), flower_col(rnd(GW), rnd(GH)), 3);
        particles_step();
    }
    return true;
}
static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    scores_load();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    sfx_cache_check();
    presynth();
    s_rng ^= (uint32_t)esp_timer_get_time();
    s_screen = ST_MENU; s_msel = 0; s_anim = 0; s_over_ms = 900;
    s_capY = cap_target();
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint();
    sfx(9);
    nucleo_app_request_draw();
}
static void on_exit(void) { grid_free(); nucleo_audio_stop(); }

extern "C" void nucleo_register_sandgarden(void)
{
    static const nucleo_app_def_t app = {
        "giardino", "Giardino", "Games", "Falling-sand: coltiva, fai sbocciare, difendi",
        'G', C_GREEN, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP   // grid malloc + shared I2S for SFX; restored on close
    };
    nucleo_app_register(&app);
}
