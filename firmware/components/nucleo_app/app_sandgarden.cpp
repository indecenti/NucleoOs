// app_sandgarden.cpp — NucleoOS "Giardino": a calm merge-to-grow puzzle (Games).
//
// A 4x4 bed of soil. Every arrow press slides all the plants that way; two equal plants that meet merge
// into the next stage of growth — seed, sprout, seedling, bud, flower, sunflower, bush, flowering bush,
// young tree, cherry tree, golden tree, sacred tree — and a new seed falls into an empty plot. Grow the
// golden tree. The sky above the bed wakes from night to golden hour as the garden grows.
//
// Built for the Cardputer: four arrows (or WASD) are the whole game, one undo, the game is saved on exit
// and resumed on the next open, big tiles drawn as anti-aliased vector plants, the shared console kit
// (game_ui) for title/menu/dialogs, the five OS languages (game_text), and an arcade WAV pack
// (tools/sfx-gen/games/giardino.py) through game_sfx — no synthesis on the app task.
//
// RAM: the whole game state (board, undo, animation, petals) is one APP_RAM block (~600 B) allocated on
// open and freed on close; statics are the settings/record only. Replaced the falling-sand sandbox that
// asked for pixel-precise pouring with a tiny keyboard (2026-10).

#if __has_include("sdkconfig.h")       // the host harnesses compile this file without one
#include "sdkconfig.h"
#endif
// Built in, or (CONFIG_NUCLEO_STORE_SANDGARDEN) only as the store's native module, built with NAP_MODULE.
#if !CONFIG_NUCLEO_STORE_SANDGARDEN || defined(NAP_MODULE)
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "game_ui.h"
#include "game_text.h"
#include "app_large.h"          // two screens: the garden on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"
#include "game_sfx.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
#include "esp_timer.h"
#include "esp_random.h"
}

#define DIRG "/sd/data/giardino"
#define NB 4                                 // board side
#define TILE 29
#define GAP 3
#define BOARD (NB * TILE + (NB + 1) * GAP)   // 131
#define BX 3
#define BY 2
#define MAXT 12                              // tiers 1..12 (12 = sacred tree, beyond the 2048 golden tree)
#define WIN_T 11
#define SLIDE_MS 105
#define POP_MS 150
#define NPET 28

using gui::rgb;
using gui::mix;

// ============================ state ==========================================
struct Mv { int8_t fx, fy, tx, ty; uint8_t t; };           // a tile sliding from (fx,fy) to (tx,ty)
struct Petal { float x, y, vx, vy; int16_t life; uint16_t col; };
struct Pop { int8_t dx; int8_t y; int16_t life; int16_t val; };
struct Game {
    uint8_t b[NB * NB], ub[NB * NB];       // board + undo board (tier per cell, 0 = empty)
    uint32_t score, uscore;
    uint16_t moves;
    bool can_undo, over, won_seen;
    Mv mv[NB * NB]; uint8_t nmv;            // the last move's slides (drawn during SLIDE_MS)
    uint8_t pop[NB * NB];                   // per cell: 1 = merged this move (pops), 2 = spawned (grows)
    int16_t anim;                           // ms since the move began (<0 = idle)
    Petal pet[NPET];
    Pop fl[3];                              // floating "+score" labels
};
static Game *G;                             // APP_RAM: 0 B while the game is closed

enum { SC_MENU = 0, SC_PLAY, SC_HELP, SC_RECORD, SC_SET };
static int s_scr, s_help, s_dialog;         // dialog: 0 none, 1 pause, 2 game over, 3 golden tree
static gui::Menu s_menu, s_set;
static int s_audio = 1;
static uint32_t s_best, s_games;
static uint8_t s_best_tier;
static bool s_saved;                        // a game in progress exists on the card (menu offers Continue)
static int64_t s_last;

// ============================ text ===========================================
static const char *tier_name(int t)
{
    switch (t) {
        case 1:  return GT("Seme", "Seed");
        case 2:  return GT("Germoglio", "Sprout");
        case 3:  return GT("Piantina", "Seedling");
        case 4:  return GT("Bocciolo", "Bud");
        case 5:  return GT("Fiore", "Flower");
        case 6:  return GT("Girasole", "Sunflower");
        case 7:  return GT("Cespuglio", "Bush");
        case 8:  return GT("Rosaio", "Rose bush");
        case 9:  return GT("Alberello", "Young tree");
        case 10: return GT("Ciliegio", "Cherry tree");
        case 11: return GT("Albero d'oro", "Golden tree");
        default: return GT("Albero sacro", "Sacred tree");
    }
}

// ============================ sound ==========================================
enum { S_NAV = 1, S_SEL, S_BACK, S_SLIDE, S_M1, S_M2, S_M3, S_M4, S_UNDO, S_BEST, S_WIN, S_OVER, S_NOPE, S_N = S_NOPE };
static const char *sfx_name(int id)
{
    static const char *n[] = { "", "nav", "sel", "back", "slide", "m1", "m2", "m3", "m4", "undo", "best", "win", "over", "nope" };
    return (id > 0 && id <= S_N) ? n[id] : "nav";
}
// Fallback voices (the pack is the real sound; these only tone a cue the card does not have yet).
static int sfx_recipe(int id, notify_voice_t *v)
{
    static const float hz[] = { 0, 880, 988, 659, 330, 523, 659, 784, 1047, 440, 1319, 1568, 262, 196 };
    if (id <= 0 || id > S_N) return 0;
    notify__voice(&v[0], hz[id], 0.0f, id >= S_BEST ? 0.30f : 0.06f);
    return 1;
}
static bool sfx_important(int id) { return id >= S_M2; }
static const game_sfx_t SFX = { DIRG, sfx_name, sfx_recipe, S_N, 1, 22050, sfx_important, &s_audio };
static void sfx(int id) { game_sfx_play(&SFX, id); }

// ============================ persistence ====================================
#define SAVE_MAGIC 0x31524447u   // 'GDR1'
struct SaveFile { uint32_t m; uint8_t b[NB * NB], ub[NB * NB]; uint32_t score, uscore, best, games; uint16_t moves;
                  uint8_t best_tier, can_undo, playing, audio, won_seen, pad; };
static void save_write(void)
{
    mkdir("/sd/data", 0777); mkdir(DIRG, 0777);
    SaveFile s; memset(&s, 0, sizeof s);
    s.m = SAVE_MAGIC; s.best = s_best; s.games = s_games; s.best_tier = s_best_tier; s.audio = (uint8_t)s_audio;
    if (G && !G->over && G->moves) {        // a game worth resuming
        memcpy(s.b, G->b, sizeof s.b); memcpy(s.ub, G->ub, sizeof s.ub);
        s.score = G->score; s.uscore = G->uscore; s.moves = G->moves; s.can_undo = G->can_undo; s.won_seen = G->won_seen; s.playing = 1;
    }
    s_saved = s.playing;
    FILE *f = fopen(DIRG "/save.tmp", "wb");                       // write aside, then swap: a reset never
    if (!f) return;                                                // leaves a half-written save
    bool ok = fwrite(&s, sizeof s, 1, f) == 1;
    if (fclose(f) != 0) ok = false;
    if (ok) { remove(DIRG "/save.bin"); rename(DIRG "/save.tmp", DIRG "/save.bin"); }
}
// Load settings + record, and the game in progress into G when `game` is set. False = nothing valid.
static bool save_read(bool game)
{
    s_saved = false;                          // never a Continue left over from the last time the app was open
    FILE *f = fopen(DIRG "/save.bin", "rb");
    if (!f) return false;
    SaveFile s; bool ok = fread(&s, sizeof s, 1, f) == 1;
    fclose(f);
    if (!ok || s.m != SAVE_MAGIC) return false;
    for (int i = 0; i < NB * NB; i++) if (s.b[i] > MAXT || s.ub[i] > MAXT) return false;
    s_best = s.best; s_games = s.games; s_best_tier = s.best_tier > MAXT ? MAXT : s.best_tier; s_audio = s.audio ? 1 : 0;
    s_saved = s.playing;
    if (game && G && s.playing) {
        memcpy(G->b, s.b, sizeof s.b); memcpy(G->ub, s.ub, sizeof s.ub);
        G->score = s.score; G->uscore = s.uscore; G->moves = s.moves; G->can_undo = s.can_undo; G->won_seen = s.won_seen;
        G->over = false; G->anim = -1; G->nmv = 0; memset(G->pop, 0, sizeof G->pop);
    }
    return s.playing;
}

// ============================ rules ==========================================
static int max_tier(void) { int m = 0; for (int i = 0; i < NB * NB; i++) if (G->b[i] > m) m = G->b[i]; return m; }
static bool spawn(void)
{
    int e[NB * NB], n = 0;
    for (int i = 0; i < NB * NB; i++) if (!G->b[i]) e[n++] = i;
    if (!n) return false;
    int c = e[esp_random() % n];
    G->b[c] = (esp_random() % 10) ? 1 : 2;     // a seed, sometimes a sprout
    G->pop[c] = 2;
    return true;
}
static bool can_move(void)
{
    for (int y = 0; y < NB; y++) for (int x = 0; x < NB; x++) {
        int t = G->b[y * NB + x];
        if (!t) return true;
        if (x + 1 < NB && G->b[y * NB + x + 1] == t) return true;
        if (y + 1 < NB && G->b[(y + 1) * NB + x] == t) return true;
    }
    return false;
}
static void petals(int cx, int cy, int t)
{
    static const uint16_t pal[4] = { 0xFDB8, 0xFFE0, 0xFFFF, 0xB7F6 };
    int n = t * 2;
    for (int i = 0; i < NPET && n > 0; i++) {
        Petal &p = G->pet[i];
        if (p.life > 0) continue;
        float a = (esp_random() % 628) / 100.0f, sp = 0.03f + (esp_random() % 40) / 1000.0f;
        p.x = cx; p.y = cy; p.vx = cosf(a) * sp; p.vy = sinf(a) * sp - 0.03f;
        p.life = 600 + esp_random() % 500; p.col = t >= WIN_T ? 0xFE60 : pal[esp_random() % 4]; n--;
    }
}
static int tx_of(int x) { return BX + GAP + x * (TILE + GAP); }
static int ty_of(int y) { return BY + GAP + y * (TILE + GAP); }

// Slide every line toward (dx,dy); merge equal neighbours once per move. Returns true if anything moved.
static bool slide(int dx, int dy)
{
    uint8_t nb[NB * NB]; memset(nb, 0, sizeof nb);
    uint8_t merged[NB * NB]; memset(merged, 0, sizeof merged);
    G->nmv = 0;
    bool moved = false; uint32_t gained = 0; int top = 0;
    for (int line = 0; line < NB; line++) {
        int idx[NB];                                   // the line's cells, from the edge we slide toward
        for (int k = 0; k < NB; k++) {
            int x = dx ? (dx > 0 ? NB - 1 - k : k) : line;
            int y = dy ? (dy > 0 ? NB - 1 - k : k) : line;
            idx[k] = y * NB + x;
        }
        int out = -1;
        for (int k = 0; k < NB; k++) {
            int c = idx[k], t = G->b[c];
            if (!t) continue;
            int dst;
            if (out >= 0 && nb[idx[out]] == t && !merged[idx[out]]) {
                dst = idx[out]; nb[dst] = (uint8_t)(t + 1); merged[dst] = 1;
                gained += 1u << (t + 1); if (t + 1 > top) top = t + 1;
            } else {
                dst = idx[++out]; nb[dst] = (uint8_t)t;
            }
            if (dst != c) moved = true;
            Mv &m = G->mv[G->nmv++];
            m.fx = (int8_t)(c % NB); m.fy = (int8_t)(c / NB); m.tx = (int8_t)(dst % NB); m.ty = (int8_t)(dst / NB); m.t = (uint8_t)t;
        }
    }
    if (!moved) { G->nmv = 0; return false; }
    memcpy(G->ub, G->b, sizeof G->b); G->uscore = G->score; G->can_undo = true;
    memcpy(G->b, nb, sizeof nb);
    for (int i = 0; i < NB * NB; i++) G->pop[i] = merged[i];
    G->score += gained; G->moves++;
    if (gained) {
        for (int i = 0; i < 3; i++) if (G->fl[i].life <= 0) { G->fl[i].life = 900; G->fl[i].val = (int16_t)(gained > 32767 ? 32767 : gained); G->fl[i].y = 0; G->fl[i].dx = (int8_t)(i * 6); break; }
        for (int i = 0; i < NB * NB; i++) if (merged[i] && nb[i] >= 4) petals(tx_of(i % NB) + TILE / 2, ty_of(i / NB) + TILE / 2, nb[i]);
    }
    spawn();
    G->anim = 0;
    if (top >= WIN_T && !G->won_seen) { G->won_seen = true; s_dialog = 3; sfx(S_WIN); }
    else sfx(top >= 7 ? S_M4 : top >= 5 ? S_M3 : top >= 3 ? S_M2 : top ? S_M1 : S_SLIDE);
    if (top > s_best_tier) s_best_tier = (uint8_t)top;
    if (G->score > s_best) { if (s_best && G->score - gained <= s_best) sfx(S_BEST); s_best = G->score; }
    if (!can_move()) { G->over = true; s_games++; s_dialog = 2; sfx(S_OVER); save_write(); }
    return true;
}
static void new_game(void)
{
    memset(G, 0, sizeof *G);
    G->anim = -1;
    spawn(); spawn();
    G->anim = 0;
}

// ============================ drawing: plants ================================
// Tile colour per tier: soil-greens through blossom pinks to gold; snapped to RGB332 by gui::rgb.
static uint16_t tier_col(int t)
{
    static const uint8_t c[MAXT + 1][3] = {
        { 0, 0, 0 }, { 214, 184, 140 }, { 190, 226, 150 }, { 150, 214, 126 }, { 226, 196, 226 }, { 255, 198, 206 },
        { 255, 224, 120 }, { 118, 196, 118 }, { 206, 150, 226 }, { 130, 180, 100 }, { 255, 170, 206 }, { 255, 214, 70 }, { 170, 236, 255 } };
    t = t < 0 ? 0 : t > MAXT ? MAXT : t;
    return rgb(c[t][0], c[t][1], c[t][2]);
}
// The panel paints in clipped strip windows, where drawWideLine / drawWedgeLine widen the clip and write outside
// the window: there (s_flat) a stem is a rect and a leaf (a wedge from the stem point outward) a triangle.
static bool s_flat;
static void stem_(int x, int y0, int y1, float r, uint16_t c)
{
    if (s_flat) d.fillRect(x - (int)r, y1, 2 * (int)r + 1, y0 - y1, c); else d.drawWideLine(x, y0, x, y1, r, c);
}
static void leaf(float x, float y, float dx, float dy, float w, uint16_t col)
{
    if (!s_flat) { d.drawWedgeLine((int)x, (int)y, (int)(x + dx), (int)(y + dy), 0.5f, w, col); return; }
    float l = sqrtf(dx * dx + dy * dy) + 0.01f, px = -dy / l * w, py = dx / l * w;
    d.fillTriangle((int)x, (int)y, (int)(x + dx + px), (int)(y + dy + py), (int)(x + dx - px), (int)(y + dy - py), col);
    d.fillCircle((int)(x + dx), (int)(y + dy), (int)w, col);
}
static void flower(float cx, float cy, float r, int petalsn, uint16_t pc, uint16_t core)
{
    for (int i = 0; i < petalsn; i++) {
        float a = i * 6.2832f / petalsn - 1.5708f;
        d.fillSmoothCircle((int)(cx + cosf(a) * r * 0.62f), (int)(cy + sinf(a) * r * 0.62f), (int)(r * 0.48f + 0.5f), pc);
    }
    d.fillSmoothCircle((int)cx, (int)cy, (int)(r * 0.42f + 0.5f), core);
}
static void tree(float cx, float base, float s, uint16_t canopy, uint16_t blossom, bool gold)
{
    uint16_t bark = rgb(120, 80, 50);
    d.fillRect((int)(cx - s * 0.08f), (int)(base - s * 0.42f), (int)(s * 0.16f + 1), (int)(s * 0.42f), bark);
    float cy = base - s * 0.62f;
    d.fillSmoothCircle((int)(cx - s * 0.2f), (int)(cy + s * 0.06f), (int)(s * 0.24f), mix(canopy, rgb(0, 0, 0), 50));
    d.fillSmoothCircle((int)(cx + s * 0.2f), (int)(cy + s * 0.06f), (int)(s * 0.24f), mix(canopy, rgb(0, 0, 0), 30));
    d.fillSmoothCircle((int)cx, (int)(cy - s * 0.08f), (int)(s * 0.28f), canopy);
    if (blossom) for (int i = 0; i < 6; i++) {
        float a = i * 1.047f + 0.4f;
        d.fillSmoothCircle((int)(cx + cosf(a) * s * 0.22f), (int)(cy - s * 0.04f + sinf(a) * s * 0.16f), (int)(s * 0.06f + 1), blossom);
    }
    if (gold) { d.drawPixel((int)(cx - s * 0.3f), (int)(cy - s * 0.3f), 0xFFFF); d.drawPixel((int)(cx + s * 0.32f), (int)(cy - s * 0.22f), 0xFFFF); d.drawPixel((int)(cx + s * 0.05f), (int)(cy - s * 0.42f), 0xFFFF); }
}
// The plant for tier t, centred in a box of side s at (cx, cy).
static void plant(int t, float cx, float cy, float s)
{
    int32_t kx, ky, kw, kh; d.getClipRect(&kx, &ky, &kw, &kh);   // a panel strip: skip a plant outside it
    if (cy + s < ky || cy - s > ky + kh) return;
    uint16_t stem = rgb(60, 140, 60), lg = rgb(90, 190, 80);
    float base = cy + s * 0.36f;
    switch (t) {
        case 1:   // seed in a little dimple of soil
            d.fillEllipse((int)cx, (int)(cy + s * 0.12f), (int)(s * 0.26f), (int)(s * 0.08f), rgb(150, 110, 70));
            d.fillSmoothCircle((int)cx, (int)(cy + s * 0.02f), (int)(s * 0.12f + 1), rgb(110, 70, 40));
            d.drawPixel((int)(cx - s * 0.04f), (int)(cy - s * 0.04f), rgb(200, 160, 110));
            break;
        case 2:   // sprout: two leaves
            stem_((int)cx, (int)base, (int)(cy - s * 0.02f), s * 0.035f + 0.6f, stem);
            leaf(cx, cy, -s * 0.24f, -s * 0.16f, s * 0.09f, lg); leaf(cx, cy, s * 0.24f, -s * 0.16f, s * 0.09f, lg);
            break;
        case 3:   // seedling: four leaves
            stem_((int)cx, (int)base, (int)(cy - s * 0.18f), s * 0.035f + 0.6f, stem);
            leaf(cx, cy + s * 0.12f, -s * 0.26f, -s * 0.1f, s * 0.09f, lg); leaf(cx, cy + s * 0.12f, s * 0.26f, -s * 0.1f, s * 0.09f, lg);
            leaf(cx, cy - s * 0.08f, -s * 0.2f, -s * 0.18f, s * 0.08f, lg); leaf(cx, cy - s * 0.08f, s * 0.2f, -s * 0.18f, s * 0.08f, lg);
            break;
        case 4:   // bud
            stem_((int)cx, (int)base, (int)(cy - s * 0.1f), s * 0.035f + 0.6f, stem);
            leaf(cx, cy + s * 0.14f, -s * 0.26f, -s * 0.08f, s * 0.09f, lg); leaf(cx, cy + s * 0.14f, s * 0.26f, -s * 0.08f, s * 0.09f, lg);
            d.fillSmoothCircle((int)cx, (int)(cy - s * 0.2f), (int)(s * 0.14f + 1), rgb(220, 90, 150));
            leaf(cx, cy - s * 0.08f, -s * 0.1f, -s * 0.16f, s * 0.06f, lg); leaf(cx, cy - s * 0.08f, s * 0.1f, -s * 0.16f, s * 0.06f, lg);
            break;
        case 5:   // flower
            stem_((int)cx, (int)base, (int)(cy - s * 0.1f), s * 0.035f + 0.6f, stem);
            leaf(cx, cy + s * 0.16f, -s * 0.24f, -s * 0.06f, s * 0.08f, lg);
            flower(cx, cy - s * 0.16f, s * 0.3f, 5, rgb(240, 70, 110), rgb(255, 220, 60));
            break;
        case 6:   // sunflower
            stem_((int)cx, (int)base, (int)(cy - s * 0.1f), s * 0.04f + 0.6f, stem);
            leaf(cx, cy + s * 0.14f, s * 0.26f, -s * 0.08f, s * 0.09f, lg);
            flower(cx, cy - s * 0.14f, s * 0.36f, 8, rgb(255, 210, 0), rgb(120, 70, 30));
            break;
        case 7:   // bush
            d.fillSmoothCircle((int)(cx - s * 0.2f), (int)(cy + s * 0.12f), (int)(s * 0.2f), rgb(60, 150, 70));
            d.fillSmoothCircle((int)(cx + s * 0.2f), (int)(cy + s * 0.12f), (int)(s * 0.2f), rgb(50, 140, 60));
            d.fillSmoothCircle((int)cx, (int)(cy - s * 0.04f), (int)(s * 0.24f), rgb(80, 180, 80));
            break;
        case 8:   // flowering bush
            plant(7, cx, cy, s);
            for (int i = 0; i < 5; i++) { float a = i * 1.2566f; d.fillSmoothCircle((int)(cx + cosf(a) * s * 0.2f), (int)(cy + s * 0.04f + sinf(a) * s * 0.14f), (int)(s * 0.06f + 1), i & 1 ? rgb(255, 255, 255) : rgb(230, 90, 200)); }
            break;
        case 9:   tree(cx, base, s, rgb(70, 160, 70), 0, false); break;
        case 10:  tree(cx, base, s, rgb(255, 170, 210), rgb(255, 255, 255), false); break;
        case 11:  tree(cx, base, s, rgb(255, 200, 40), rgb(255, 250, 180), true); break;
        default:  tree(cx, base, s, rgb(150, 230, 255), rgb(255, 255, 255), true);
                  d.drawCircle((int)cx, (int)(cy - s * 0.12f), (int)(s * 0.46f), rgb(220, 250, 255)); break;
    }
}
// A tile: rounded card in the tier colour, light top edge, darker bottom edge, the plant, the value.
static void tile(int t, float cx, float cy, float scale)
{
    int s = (int)(TILE * scale + 0.5f); if (s < 4) return;
    int x = (int)(cx - s / 2.0f), y = (int)(cy - s / 2.0f);
    uint16_t c = tier_col(t);
    d.fillSmoothRoundRect(x, y, s, s, s / 5, c);
    d.drawFastHLine(x + s / 5, y + 1, s - 2 * (s / 5), mix(c, rgb(255, 255, 255), 120));
    d.drawFastHLine(x + s / 5, y + s - 2, s - 2 * (s / 5), mix(c, rgb(0, 0, 0), 70));
    plant(t, cx, cy - s * 0.06f, s * 0.92f);
    if (scale > 0.9f) {
        char v[8]; snprintf(v, sizeof v, "%u", 1u << t);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(textdatum_t::bottom_right);
        d.setTextColor(mix(c, rgb(0, 0, 0), 150)); d.drawString(v, x + s - 2, y + s - 1);
        d.setTextDatum(textdatum_t::top_left);
    }
}

// ============================ drawing: scenes ================================
// The sky wakes with the garden: night -> dawn -> day -> golden hour -> enchanted.
static void sky_cols(int t, uint16_t *top, uint16_t *bot)
{
    static const uint8_t K[5][6] = {
        { 8, 12, 40, 40, 44, 90 }, { 60, 70, 150, 250, 160, 120 }, { 60, 140, 235, 190, 225, 255 },
        { 80, 50, 140, 255, 150, 70 }, { 30, 20, 80, 180, 120, 230 } };
    float p = t <= 1 ? 0.0f : (t - 1) / 10.0f * 4.0f; if (p > 4) p = 4;
    int k = (int)p; float f = p - k; if (k >= 4) { k = 3; f = 1.0f; }
    int a[6]; for (int i = 0; i < 6; i++) a[i] = (int)(K[k][i] + (K[k + 1][i] - K[k][i]) * f);
    *top = rgb(a[0], a[1], a[2]); *bot = rgb(a[3], a[4], a[5]);
}
// The board drawn at origin (s_bx, s_by) with s_ts-px plots s_gp apart: the compact frame (BX, BY, TILE, GAP) or
// the panel's native 230 px board (lg_paint sets the view, then puts it back).
static int s_bx = BX, s_by = BY, s_ts = TILE, s_gp = GAP;
static int vx_of(int x) { return s_bx + s_gp + x * (s_ts + s_gp); }
static int vy_of(int y) { return s_by + s_gp + y * (s_ts + s_gp); }
static void draw_board(void)
{
    const int bs = NB * s_ts + (NB + 1) * s_gp;
    const float k = s_ts / (float)TILE, h = s_ts / 2.0f;
    gui::panel(s_bx, s_by, bs, bs, rgb(96, 64, 40), rgb(150, 110, 70));
    for (int y = 0; y < NB; y++) for (int x = 0; x < NB; x++) {
        int px = vx_of(x), py = vy_of(y);
        d.fillRoundRect(px, py, s_ts, s_ts, s_ts > TILE ? s_ts / 6 : 5, rgb(70, 46, 28));
        d.drawFastHLine(px + 5, py + 1, s_ts - 10, rgb(50, 32, 20));                 // the plot's inner shadow
    }
    float q0 = G->anim < 0 ? 1.0f : G->anim / (float)SLIDE_MS; if (q0 > 1) q0 = 1;
    float e = 1 - (1 - q0) * (1 - q0) * (1 - q0);                                    // ease-out cubic
    if (q0 < 1.0f) {
        for (int i = 0; i < G->nmv; i++) {                                           // tiles in flight (old tiers)
            const Mv &m = G->mv[i];
            float cx = vx_of(m.fx) + (vx_of(m.tx) - vx_of(m.fx)) * e + h;
            float cy = vy_of(m.fy) + (vy_of(m.ty) - vy_of(m.fy)) * e + h;
            tile(m.t, cx, cy, k);
        }
        return;
    }
    int pt = G->anim < 0 ? POP_MS : G->anim - SLIDE_MS;                               // after the slide: pops and growth
    float q = pt >= POP_MS ? 1.0f : pt / (float)POP_MS;
    for (int i = 0; i < NB * NB; i++) {
        if (!G->b[i]) continue;
        float sc = 1.0f;
        if (G->pop[i] == 1 && q < 1) sc = 1.0f + 0.18f * sinf(q * 3.1416f);          // merge: a bounce
        if (G->pop[i] == 2 && q < 1) sc = 0.25f + 0.75f * q;                         // new seed: grows in
        tile(G->b[i], vx_of(i % NB) + h, vy_of(i / NB) + h, sc * k);
    }
}
static void draw_side(void)
{
    int x0 = BX + BOARD + 4, w = W - x0 - 2;
    gui::panel(x0, 2, w, 44, rgb(14, 20, 34), rgb(80, 130, 90));
    gui::text(GT("PUNTI", "SCORE"), x0 + w / 2, 5, 1, gui::F_SMALL, rgb(150, 200, 160), rgb(0, 0, 0));
    char b[16]; snprintf(b, sizeof b, "%lu", (unsigned long)G->score);
    gui::text(b, x0 + w / 2, 20, 1, gui::text_width(b, gui::F_BIG) <= w - 6 ? gui::F_BIG : gui::F_BODY, rgb(255, 255, 255), rgb(0, 0, 0));
    snprintf(b, sizeof b, "%s %lu", GT("Rec", "Best"), (unsigned long)s_best);
    gui::text(b, x0 + w / 2, 49, 1, gui::F_SMALL, rgb(255, 230, 150), rgb(0, 0, 0));
    int mt = max_tier();
    gui::panel(x0, 64, w, 52, rgb(14, 20, 34), tier_col(mt));                       // the best plant grown so far
    plant(mt, x0 + w / 2.0f, 84, 32);
    const char *nm = tier_name(mt);
    if (gui::text_width(nm, gui::F_SMALL) <= w - 4) gui::text(nm, x0 + w / 2, 99, 1, gui::F_SMALL, rgb(255, 255, 255), rgb(0, 0, 0));
    else { d.setTextColor(rgb(255, 255, 255)); d.setTextDatum(textdatum_t::top_center); d.drawString(nm, x0 + w / 2, 103); d.setTextDatum(textdatum_t::top_left); }
    gui::text(GT("Z annulla", "Z undo"), x0 + w / 2, 118, 1, gui::F_SMALL, G->can_undo ? rgb(200, 210, 230) : rgb(90, 100, 120), rgb(0, 0, 0));
    for (int i = 0; i < 3; i++) if (G->fl[i].life > 0) {                             // floating "+N" over the score
        char p[12]; snprintf(p, sizeof p, "+%d", G->fl[i].val);
        int a = G->fl[i].life * 256 / 900;
        gui::text(p, x0 + w - 4 - G->fl[i].dx, 18 - G->fl[i].y, 2, gui::F_SMALL, mix(rgb(0, 0, 0), rgb(255, 240, 120), a), rgb(0, 0, 0));
    }
}
static void draw_play(void)
{
    uint16_t top, bot; sky_cols(max_tier(), &top, &bot);
    gui::vgradient(0, 0, W, H, top, bot);
    draw_board();
    for (int i = 0; i < NPET; i++) if (G->pet[i].life > 0) d.fillRect((int)G->pet[i].x, (int)G->pet[i].y, 2, 2, G->pet[i].col);
    draw_side();
    if (s_dialog == 1) gui::dialog(GT("Pausa", "Paused"), GT("La partita e' salvata", "Your game is saved"), nullptr, GT("INVIO riprendi  Esc menu", "ENTER resume  Esc menu"), rgb(120, 220, 140));
    if (s_dialog == 2) {
        char l1[32]; snprintf(l1, sizeof l1, "%s %lu", GT("Punti", "Score"), (unsigned long)G->score);
        gui::dialog(GT("Giardino pieno", "Garden full"), l1, tier_name(max_tier()), G->can_undo ? GT("INVIO nuova  Z annulla", "ENTER new  Z undo") : GT("INVIO nuova  Esc menu", "ENTER new  Esc menu"), rgb(255, 200, 80));
    }
    if (s_dialog == 3) gui::dialog(GT("Albero d'oro!", "Golden tree!"), GT("Hai fatto crescere il 2048", "You grew the 2048"), GT("Continua per l'albero sacro", "Go on for the sacred tree"), GT("INVIO continua", "ENTER continue"), rgb(255, 214, 70));
}
enum { MI_CONT = 0, MI_NEW, MI_HELP, MI_REC, MI_SET };
static const char *menu_items[5];
static uint8_t menu_ids[5];
static int menu_build(void)
{
    int n = 0;
    if (s_saved) { menu_ids[n] = MI_CONT; menu_items[n++] = GT("Continua", "Continue"); }
    menu_ids[n] = MI_NEW;  menu_items[n++] = GT("Nuova partita", "New game");
    menu_ids[n] = MI_HELP; menu_items[n++] = GT("Come si gioca", "How to play");
    menu_ids[n] = MI_REC;  menu_items[n++] = GT("Record", "Records");
    menu_ids[n] = MI_SET;  menu_items[n++] = GT("Impostazioni", "Settings");
    return n;
}
static void draw_menu(void)
{
    int y = gui::title(GT("Giardino", "Garden"), GT("Unisci le piante, fai crescere l'albero", "Merge plants, grow the golden tree"), rgb(120, 220, 140));
    int n = menu_build();
    gui::menu(s_menu, menu_items, n, y, nucleo_app_content_height(), rgb(120, 220, 140));
}
// The guide's text, shared by the compact page and the panel page: line i of page pg (nullptr past the end).
static const char *help_line(int pg, int i)
{
    static const uint8_t N[3] = { 2, 1, 5 };
    if (i >= N[pg]) return nullptr;
    if (pg == 0) return i ? GT("Due piante uguali si uniscono", "Two equal plants merge") : GT("Le frecce spostano TUTTE le piante", "The arrows slide EVERY plant");
    if (pg == 1) return GT("Dal seme all'albero sacro", "From seed to sacred tree");
    switch (i) {
        case 0:  return GT("Ogni mossa cade un seme", "Each move drops a seed");
        case 1:  return GT("Giardino pieno = fine", "Full garden = game over");
        case 2:  return GT("Z / DEL annulla l'ultima mossa", "Z / DEL undoes the last move");
        case 3:  return GT("Esc: la partita resta salvata", "Esc: your game stays saved");
        default: return GT("Obiettivo: l'albero d'oro (2048)", "Goal: the golden tree (2048)");
    }
}
static uint16_t help_col(int pg, int i) { return pg < 2 || i < 2 ? rgb(255, 255, 255) : i < 4 ? rgb(255, 230, 150) : rgb(255, 214, 70); }
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    gui::vgradient(0, 0, W, ch, rgb(20, 40, 30), rgb(0, 0, 0));
    char head[32]; snprintf(head, sizeof head, "%s  %d/3", GT("Come si gioca", "How to play"), s_help + 1);
    gui::text(head, W / 2, 2, 1, gui::F_BODY, rgb(120, 220, 140), rgb(0, 0, 0));
    static const uint8_t Y[3] = { 28, 104, 26 };
    for (int i = 0; help_line(s_help, i); i++)
        gui::text(help_line(s_help, i), W / 2, Y[s_help] + i * 18, 1, gui::F_SMALL, help_col(s_help, i), rgb(0, 0, 0));
    if (s_help == 0) {
        tile(2, 36, 88, 1.0f); tile(2, 70, 88, 1.0f);
        d.fillTriangle(90, 82, 90, 94, 98, 88, rgb(255, 230, 120));
        tile(3, 118, 88, 1.0f);
        gui::text(GT("...e crescono", "...and grow"), 186, 81, 1, gui::F_SMALL, rgb(255, 230, 120), rgb(0, 0, 0));
    } else if (s_help == 1) {
        for (int t = 1; t <= MAXT; t++) tile(t, 22 + (t - 1) % 6 * 39, 40 + (t - 1) / 6 * 40, 1.0f);   // the family, 6 per row
    }
}
static void draw_record(void)
{
    int y = gui::title(GT("Record", "Records"), nullptr, rgb(255, 214, 70));
    char b[40];
    snprintf(b, sizeof b, "%s  %lu", GT("Punteggio migliore", "Best score"), (unsigned long)s_best);
    gui::text(b, W / 2, y + 4, 1, gui::F_BODY, rgb(255, 255, 255), rgb(0, 0, 0));
    snprintf(b, sizeof b, "%s  %lu", GT("Partite", "Games"), (unsigned long)s_games);
    gui::text(b, W / 2, y + 26, 1, gui::F_SMALL, rgb(200, 210, 230), rgb(0, 0, 0));
    if (s_best_tier) {
        tile(s_best_tier, 40, y + 62, 1.0f);
        gui::text(GT("Pianta piu' alta", "Tallest plant"), 64, y + 50, 0, gui::F_SMALL, rgb(150, 200, 160), rgb(0, 0, 0));
        gui::text(tier_name(s_best_tier), 64, y + 66, 0, gui::F_BODY, rgb(255, 255, 255), rgb(0, 0, 0));
    }
}
static const char *set_items[2];
static void draw_settings(void)
{
    int y = gui::title(GT("Impostazioni", "Settings"), nullptr, rgb(120, 220, 140));
    static char a[32];
    snprintf(a, sizeof a, "%s: %s", GT("Suoni", "Sound"), s_audio ? GT("Si", "On") : GT("No", "Off"));
    set_items[0] = a; set_items[1] = GT("Azzera record", "Reset records");
    gui::menu(s_set, set_items, 2, y, nucleo_app_content_height(), rgb(120, 220, 140));
}
static void draw_game(void)
{
    switch (s_scr) {
        case SC_PLAY:   draw_play(); break;
        case SC_HELP:   draw_help(); break;
        case SC_RECORD: draw_record(); break;
        case SC_SET:    draw_settings(); break;
        default:        draw_menu(); break;
    }
}

/// ===== two screens: the garden NATIVE on the 320x240 panel, the Cardputer as a companion =================
// Every screen is laid out for 320x240 (lg::frame) and touched directly: in play SWIPE the board to slide the
// plants (a drag pad, one move per swipe) or TAP the board on the side they should go to; tap the undo card to
// take a move back, the chevron pauses. Menus and dialogs: tap a row to pick it, tap it again to open it.
#define GRN rgb(120, 220, 140)
#define LW 320
#define LH 240
#define LB0 5                                   // the panel board: 4 x 50 px plots, 6 px gaps = 230 px square
#define LBS 230
#define LSX 241                                 // the side column
static int s_lsel, s_ldlg;                      // the picked row of a panel dialog, the dialog it belongs to
static int64_t s_tmove;                         // the last swiped move (one move per swipe)
static void lt(int x, int y, int f, uint16_t c, const char *s, uint8_t dt = top_left)
{
    lg::font(f); d.setTextDatum(dt); d.setTextColor(c); d.drawString(s, x, y); d.setTextDatum(top_left);
}
static void lchev(int x, int y, uint16_t c)
{
    for (int k = 0; k < 2; k++) { d.drawLine(x + 6 + k, y, x + k, y + 7, c); d.drawLine(x + k, y + 7, x + 6 + k, y + 14, c); }
}
static void lhead(const char *title)            // the header band: the chevron (tap = Back) and the title
{
    gui::vgradient(0, 0, LW, 30, rgb(20, 50, 30), rgb(10, 16, 26));
    d.drawFastHLine(0, 29, LW, rgb(80, 130, 90));
    lchev(10, 8, GRN); nucleo_touch_add(0, 0, 140, 30, NK_BACK, 0);
    lt(28, 7, 1, GRN, title);
}
// n rows from y, rh apart, the picked one lit; row i taps as NUCLEO_TOUCH_ROW + i.
static void lrows(int y, int rh, const char *const *lab, int n, int sel, int x, int w)
{
    for (int i = 0; i < n; i++) {
        const int ry = y + i * rh, h = rh - 6; const bool s = i == sel;
        d.fillRoundRect(x, ry, w, h, 10, s ? mix(rgb(10, 16, 26), GRN, 110) : rgb(24, 34, 44));
        d.drawRoundRect(x, ry, w, h, 10, s ? GRN : rgb(60, 80, 70));
        char b[40]; lg::font(1); lg::fit(b, sizeof b, lab[i], w - 16);
        lt(x + w / 2, ry + h / 2 + 1, 1, s ? 0xFFFF : rgb(190, 200, 190), b, middle_center);
        nucleo_touch_add(x, ry, w, h, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
}
// The open dialog's rows: 0 = ENTER (resume / new / continue), 1 = undo, 2 = Back (menu).
static int dlg_rows(int *id, const char **lab)
{
    if (s_dialog != s_ldlg) { s_ldlg = s_dialog; s_lsel = 0; }             // a new dialog starts on its first row
    int n = 0;
    if (s_dialog == 2) { id[n] = 0; lab[n++] = GT("Nuova partita", "New game"); if (G->can_undo) { id[n] = 1; lab[n++] = GT("Annulla", "Undo"); } }
    else { id[n] = 0; lab[n++] = s_dialog == 1 ? GT("Riprendi", "Resume") : GT("Continua", "Continue"); }
    if (s_dialog != 3) { id[n] = 2; lab[n++] = GT("Menu", "Menu"); }
    return n;
}
static void lp_dialog(void)
{
    char b[32]; const char *t, *l1, *lab[3]; int id[3], n = dlg_rows(id, lab);
    if (s_dialog == 1) { t = GT("Pausa", "Paused"); l1 = GT("La partita e' salvata", "Your game is saved"); }
    else if (s_dialog == 2) { t = GT("Giardino pieno", "Garden full"); snprintf(b, sizeof b, "%s %lu", GT("Punti", "Score"), (unsigned long)G->score); l1 = b; }
    else { t = GT("Albero d'oro!", "Golden tree!"); l1 = GT("Hai fatto crescere il 2048", "You grew the 2048"); }
    const uint16_t c = s_dialog == 1 ? GRN : rgb(255, 214, 70);
    gui::panel(30, 24, LW - 60, 196, rgb(10, 16, 26), c);
    lt(LW / 2, 36, 2, c, t, top_center);
    lt(LW / 2, 64, 0, 0xFFFF, l1, top_center);
    lrows(92, 42, lab, n, s_lsel, 60, LW - 120);
}
static void lp_play(void)
{
    uint16_t top, bot; sky_cols(max_tier(), &top, &bot);
    gui::vgradient(0, 0, LW, LH, top, bot);
    s_bx = LB0; s_by = LB0; s_ts = 50; s_gp = 6;
    draw_board();
    s_bx = BX; s_by = BY; s_ts = TILE; s_gp = GAP;
    for (int i = 0; i < NPET; i++) if (G->pet[i].life > 0)                         // petals live in compact units
        d.fillRect(LB0 + ((int)G->pet[i].x - BX) * LBS / BOARD, LB0 + ((int)G->pet[i].y - BY) * LBS / BOARD, 3, 3, G->pet[i].col);
    const int w = LW - LSX - 4, cx = LSX + w / 2;
    char b[24];
    lchev(LSX + 6, 10, 0xFFFF); lt(LSX + 20, 9, 0, 0xFFFF, GT("Menu", "Menu"));
    gui::panel(LSX, 36, w, 64, rgb(14, 20, 34), rgb(80, 130, 90));                 // score + record
    lt(cx, 41, 4, rgb(150, 200, 160), GT("PUNTI", "SCORE"), top_center);
    snprintf(b, sizeof b, "%lu", (unsigned long)G->score);
    lg::font(2); lt(cx, 54, d.textWidth(b) <= w - 6 ? 2 : 0, 0xFFFF, b, top_center);
    snprintf(b, sizeof b, "%s %lu", GT("Rec", "Best"), (unsigned long)s_best);
    lt(cx, 82, 4, rgb(255, 230, 150), b, top_center);
    for (int i = 0; i < 3; i++) if (G->fl[i].life > 0) {                            // floating "+N" over the score
        snprintf(b, sizeof b, "+%d", G->fl[i].val);
        lt(LSX + w - 3 - G->fl[i].dx, 40 - G->fl[i].y, 4, mix(rgb(0, 0, 0), rgb(255, 240, 120), G->fl[i].life * 256 / 900), b, top_right);
    }
    const int mt = max_tier();
    gui::panel(LSX, 104, w, 74, rgb(14, 20, 34), tier_col(mt));                    // the best plant grown so far
    plant(mt, cx, 132, 46);
    lg::font(4); lg::fit(b, sizeof b, tier_name(mt), w - 4); lt(cx, 165, 4, 0xFFFF, b, top_center);
    const uint16_t uc = G->can_undo ? rgb(200, 210, 230) : rgb(90, 100, 120);       // the undo card
    gui::panel(LSX, 182, w, 54, rgb(14, 20, 34), uc);
    d.fillArc(cx, 204, 11, 8, 200, 470, uc);
    d.fillTriangle(cx - 16, 199, cx - 4, 199, cx - 10, 207, uc);
    lt(cx, 218, 0, uc, GT("Annulla", "Undo"), top_center);
    if (s_dialog) { lp_dialog(); return; }
    nucleo_touch_add(LSX, 0, w + 4, 34, NK_BACK, 0);
    lg::field(LB0, LB0, LBS, LBS); lg::field(LSX, 182, w + 4, 58);
    nucleo_touch_add_pad(LB0, LB0, LBS, LBS, 30);
}
static void lp_menu(void)
{
    gui::vgradient(0, 0, LW, LH, rgb(8, 12, 40), rgb(40, 60, 50));
    lt(LW / 2, 6, 3, rgb(255, 230, 150), GT("Giardino", "Garden"), top_center);
    char b[48]; lg::font(0); lg::fit(b, sizeof b, GT("Unisci le piante, fai crescere l'albero", "Merge plants, grow the golden tree"), LW - 20);
    lt(LW / 2, 38, 0, GRN, b, top_center);
    tile(5, 26, 22, 1.2f); tile(11, LW - 26, 22, 1.2f);
    const int n = menu_build();
    const int rh = n < 5 ? 42 : 36;
    lrows(58 + (182 - n * rh) / 2, rh, menu_items, n, s_menu.sel, 50, LW - 100);
}
static void lp_help(void)
{
    gui::vgradient(0, 0, LW, LH, rgb(20, 40, 30), rgb(0, 0, 0));
    char b[48]; snprintf(b, sizeof b, "%s  %d/3", GT("Come si gioca", "How to play"), s_help + 1);
    lhead(b);
    for (int i = 0; help_line(s_help, i); i++) {
        lg::font(0); lg::fit(b, sizeof b, help_line(s_help, i), LW - 16);
        lt(LW / 2, (s_help == 1 ? 196 : 44) + i * 26, 0, help_col(s_help, i), b, top_center);
    }
    if (s_help == 0) {
        tile(2, 60, 140, 1.7f); tile(2, 120, 140, 1.7f);
        d.fillTriangle(156, 130, 156, 150, 170, 140, rgb(255, 230, 120));
        tile(3, 210, 140, 1.7f);
        lt(LW / 2, 190, 0, rgb(255, 230, 120), GT("...e crescono", "...and grow"), top_center);
    } else if (s_help == 1) {
        for (int t = 1; t <= MAXT; t++) tile(t, 30 + (t - 1) % 6 * 52, 74 + (t - 1) / 6 * 58, 1.6f);
    }
    lg::field(0, 30, LW, LH - 30);
}
static void lp_record(void)
{
    gui::vgradient(0, 0, LW, LH, rgb(30, 26, 10), rgb(0, 0, 0));
    lhead(GT("Record", "Records"));
    char b[24]; snprintf(b, sizeof b, "%lu", (unsigned long)s_best);
    lt(LW / 2, 44, 0, rgb(255, 230, 150), GT("Punteggio migliore", "Best score"), top_center);
    lt(LW / 2, 64, 3, 0xFFFF, b, top_center);
    snprintf(b, sizeof b, "%s  %lu", GT("Partite", "Games"), (unsigned long)s_games);
    lt(LW / 2, 104, 0, rgb(200, 210, 230), b, top_center);
    if (!s_best_tier) return;
    tile(s_best_tier, 80, 178, 2.2f);
    lt(130, 158, 0, rgb(150, 200, 160), GT("Pianta piu' alta", "Tallest plant"));
    lt(130, 178, 1, 0xFFFF, tier_name(s_best_tier));
}
static void lp_set(void)
{
    gui::vgradient(0, 0, LW, LH, rgb(20, 40, 30), rgb(0, 0, 0));
    lhead(GT("Impostazioni", "Settings"));
    const char *lab[2] = { GT("Suoni", "Sound"), GT("Azzera record", "Reset records") };
    lrows(50, 54, lab, 2, s_set.sel, 20, LW - 40);
    const int sx = LW - 84, vy = 50 + 24;                                          // the sound switch is its value
    d.fillRoundRect(sx, vy - 12, 46, 24, 12, s_audio ? GRN : rgb(60, 70, 80));
    d.fillCircle(s_audio ? sx + 34 : sx + 12, vy, 9, 0xFFFF);
}
static void lg_paint(void *)
{
    s_flat = true;
    switch (s_scr) {
        case SC_PLAY:   lp_play(); break;
        case SC_HELP:   lp_help(); break;
        case SC_RECORD: lp_record(); break;
        case SC_SET:    lp_set(); break;
        default:        lp_menu(); break;
    }
    s_flat = false;
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}
static bool on_back(int key);
static void on_key(int k, char ch);
static void play_dir(int dir);
static void undo(void);
// A touch on the panel (a row or a field tap): true when handled.
static bool lg_key(int k, char ch)
{
    int tx = 0, ty = 0;
    const bool fld = lg::tap(k, ch, &tx, &ty);
    const int ri = nucleo_touch_is_row(k, ch) ? ch - NUCLEO_TOUCH_ROW : -1;
    if (!fld && ri < 0) return false;
    if (s_scr == SC_PLAY && s_dialog) {
        const char *lab[3]; int id[3], n = dlg_rows(id, lab);
        if (ri < 0 || ri >= n) return true;
        if (s_lsel != ri) { s_lsel = ri; sfx(S_NAV); nucleo_app_request_draw(); return true; }
        if (id[ri] == 2) on_back(NK_BACK); else on_key(id[ri] ? NK_CHAR : NK_ENTER, id[ri] ? 'z' : '\n');
        return true;
    }
    if (s_scr == SC_PLAY) {
        if (!fld) return true;
        if (tx >= LSX) { undo(); return true; }
        const int dx = tx - (LB0 + LBS / 2), dy = ty - (LB0 + LBS / 2);           // the side of the board tapped
        play_dir(dx * dx > dy * dy ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0));
        return true;
    }
    if (s_scr == SC_HELP) { if (fld) on_key(NK_RIGHT, 0); return true; }
    gui::Menu *m = s_scr == SC_MENU ? &s_menu : s_scr == SC_SET ? &s_set : nullptr;
    if (!m || ri < 0 || ri >= (s_scr == SC_MENU ? menu_build() : 2)) return true;
    if (m->sel == ri) on_key(NK_ENTER, '\n'); else { m->sel = (int8_t)ri; sfx(S_NAV); nucleo_app_request_draw(); }
    return true;
}
// A drag on the board delivers arrows every 30 px: one move per swipe (a key held on the Cardputer is down,
// a swipe's arrows are not).
static bool swipe_ok(char c)
{
    if (nucleo_kbd_char_down(c)) return true;
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - s_tmove < 300) return false;
    s_tmove = now; return true;
}
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), hc = nucleo_app_content_height();
    d.fillRect(0, top, W, hc, BG);
    int y = lg::comp_head("GIARDINO", GRN);
    char b[32];
    if (s_scr == SC_PLAY && G) {
        gui::text(GT("Punti", "Score"), 8, y + 2, 0, gui::F_SMALL, rgb(150, 200, 160), 0);
        snprintf(b, sizeof b, "%lu", (unsigned long)G->score); gui::text(b, 8, y + 13, 0, gui::F_BIG, 0xFFFF, 0);
        snprintf(b, sizeof b, GT("Rec %lu", "Best %lu"), (unsigned long)s_best); gui::text(b, W - 8, y + 18, 2, gui::F_SMALL, rgb(255, 230, 150), 0);
        int mt = max_tier();
        snprintf(b, sizeof b, GT("Pianta: %s", "Plant: %s"), tier_name(mt));
        gui::text(b, 8, y + 44, 0, gui::F_BODY, tier_col(mt), 0);
        const char *st = s_dialog == 1 ? GT("In pausa", "Paused")
                       : s_dialog == 2 ? GT("Giardino pieno", "Garden full")
                       : s_dialog == 3 ? GT("Albero d'oro!", "Golden tree!") : nullptr;
        if (st) gui::text(st, W - 8, y + 44, 2, gui::F_SMALL, rgb(255, 200, 80), 0);
        lg::comp_keys(GT("Frecce muovono  Z annulla  Esc pausa", "Arrows move  Z undo  Esc pause"));
        return;
    }
    const char *st = s_scr == SC_SET ? GT("Impostazioni", "Settings")
                   : s_scr == SC_HELP ? GT("Come si gioca", "How to play")
                   : s_scr == SC_RECORD ? GT("Record", "Records") : GT("Menu", "Menu");
    gui::text(st, 8, y + 6, 0, gui::F_BODY, 0xFFFF, 0);
    snprintf(b, sizeof b, GT("Record %lu", "Best %lu"), (unsigned long)s_best); gui::text(b, 8, y + 34, 0, gui::F_SMALL, rgb(255, 230, 150), 0);
    lg::comp_keys(GT("Tocca le righe sul pannello", "Tap the rows on the panel"));
}
static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    draw_game();
}

// ============================ flow ===========================================
static void set_hint(void)
{
    switch (s_scr) {
        case SC_MENU: case SC_SET: nucleo_app_set_hint(GT("SU/GIU  INVIO ok  Esc esci", "UP/DN  ENTER ok  Esc quit")); break;
        case SC_HELP: nucleo_app_set_hint(GT("SX/DX pagine  Esc indietro", "L/R pages  Esc back")); break;
        default:      nucleo_app_set_hint(GT("Esc indietro", "Esc back")); break;
    }
}
static void go(int s)
{
    s_scr = s; s_dialog = 0;
    nucleo_app_set_fullscreen(s == SC_PLAY);
    set_hint();
    nucleo_app_request_draw();
}
// One move per PRESS: the keyboard driver auto-repeats a held arrow every 90 ms, which would fire a burst
// of moves. A repeat of the key still physically down is ignored; poll() forgets it on release.
static char s_held;
static const char DIR_KEY[4] = { ';', '/', '.', ',' };
static void play_dir(int dir)                 // 0 up, 1 right, 2 down, 3 left
{
    if (!G || s_dialog || G->over) return;
    if (s_held == DIR_KEY[dir]) return;
    if (nucleo_kbd_char_down(DIR_KEY[dir])) s_held = DIR_KEY[dir];
    if (G->anim >= 0) { G->anim = -1; G->nmv = 0; memset(G->pop, 0, sizeof G->pop); }   // a press mid-animation lands it
                                                                                         // at once: fast taps are never lost
    static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
    if (!slide(DX[dir], DY[dir])) sfx(S_NOPE);
    nucleo_app_request_draw();
}
static void undo(void)
{
    if (!G || !G->can_undo || s_dialog) { sfx(S_NOPE); return; }
    memcpy(G->b, G->ub, sizeof G->b); G->score = G->uscore; G->can_undo = false; G->over = false;
    G->nmv = 0; G->anim = -1; memset(G->pop, 0, sizeof G->pop);
    sfx(S_UNDO); nucleo_app_request_draw();
}
static void start_game(bool resume)
{
    if (!resume || !save_read(true)) new_game();
    sfx(S_SEL); go(SC_PLAY);
}
static void on_key(int k, char ch)
{
    if (lg_key(k, ch)) return;
    if (s_scr == SC_PLAY) {
        if (s_dialog) {
            if (s_dialog == 2 && (ch == 'z' || ch == 'Z' || k == NK_DEL) && G->can_undo) {   // take back the last move
                s_dialog = 0; if (s_games) s_games--; undo(); return;
            }
            if (k == NK_ENTER) {
                if (s_dialog == 2) { new_game(); s_dialog = 0; sfx(S_SEL); }
                else { s_dialog = 0; sfx(S_SEL); }
                nucleo_app_request_draw();
            }
            return;
        }
        if (k == NK_UP || ch == 'w' || ch == 'W')         { if (k != NK_UP || swipe_ok(';')) play_dir(0); }
        else if (k == NK_RIGHT || ch == 'd' || ch == 'D') { if (k != NK_RIGHT || swipe_ok('/')) play_dir(1); }
        else if (k == NK_DOWN || ch == 's' || ch == 'S')  { if (k != NK_DOWN || swipe_ok('.')) play_dir(2); }
        else if (ch == 'a' || ch == 'A')                  play_dir(3);
        else if (ch == 'z' || ch == 'Z' || k == NK_DEL)   undo();
        return;
    }
    if (s_scr == SC_MENU) {
        int n = menu_build();
        if (gui::menu_key(s_menu, k, n)) { sfx(S_NAV); nucleo_app_request_draw(); return; }
        if (k != NK_ENTER) return;
        switch (menu_ids[s_menu.sel < n ? s_menu.sel : 0]) {
            case MI_CONT: start_game(true); break;
            case MI_NEW:  start_game(false); break;
            case MI_HELP: s_help = 0; sfx(S_SEL); go(SC_HELP); break;
            case MI_REC:  sfx(S_SEL); go(SC_RECORD); break;
            default:      s_set.sel = 0; s_set.pos = 0; sfx(S_SEL); go(SC_SET); break;
        }
        return;
    }
    if (s_scr == SC_SET) {
        if (gui::menu_key(s_set, k, 2)) { sfx(S_NAV); nucleo_app_request_draw(); return; }
        if (k == NK_ENTER || k == NK_RIGHT) {
            if (s_set.sel == 0) { s_audio ^= 1; sfx(S_SEL); }
            else { s_best = 0; s_games = 0; s_best_tier = 0; sfx(S_BACK); }
            save_write(); nucleo_app_request_draw();
        }
        return;
    }
    if (s_scr == SC_HELP) {
        if (k == NK_RIGHT || k == NK_ENTER) { s_help = (s_help + 1) % 3; sfx(S_NAV); nucleo_app_request_draw(); }
        return;
    }
}
static bool on_back(int key)
{
    if (key == NK_LEFT) {                     // LEFT is a move in play, a page in the guide
        if (s_scr == SC_PLAY) { if (swipe_ok(',')) play_dir(3); }
        else if (s_scr == SC_HELP) { s_help = (s_help + 2) % 3; sfx(S_NAV); nucleo_app_request_draw(); }
        return true;
    }
    if (s_scr == SC_PLAY) {
        if (s_dialog == 0) { s_dialog = 1; sfx(S_BACK); save_write(); nucleo_app_request_draw(); return true; }
        if (s_dialog == 3) { s_dialog = 0; nucleo_app_request_draw(); return true; }
        save_write(); sfx(S_BACK); s_menu.sel = 0; s_menu.pos = 0; go(SC_MENU); return true;
    }
    if (s_scr == SC_MENU) return false;       // Esc on the title screen leaves the app
    sfx(S_BACK); go(SC_MENU);
    return true;
}
static bool poll(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    int dt = (int)(now - s_last); if (dt < 0) dt = 0; if (dt > 60) dt = 60;
    s_last = now;
    bool want = false;
    if (s_scr == SC_MENU) return gui::menu_tick(s_menu, dt);
    if (s_scr == SC_SET) return gui::menu_tick(s_set, dt);
    if (s_scr != SC_PLAY || !G) return false;
    if (s_held && !nucleo_kbd_char_down(s_held)) s_held = 0;
    if (G->anim >= 0) {
        G->anim += dt; want = true;
        if (G->anim >= SLIDE_MS + POP_MS) {
            G->anim = -1; G->nmv = 0; memset(G->pop, 0, sizeof G->pop);
        }
    }
    for (int i = 0; i < NPET; i++) {
        Petal &p = G->pet[i];
        if (p.life <= 0) continue;
        p.vy += 0.00012f * dt; p.vx *= 0.995f;
        p.x += p.vx * dt + sinf((now + i * 97) * 0.006f) * 0.08f; p.y += p.vy * dt; p.life -= dt;
        want = true;
    }
    for (int i = 0; i < 3; i++) if (G->fl[i].life > 0) { G->fl[i].life -= dt; G->fl[i].y = (int8_t)((900 - G->fl[i].life) / 60); want = true; }
    return want;
}
static void on_enter(void)
{
    game_text_open("giardino");
    s_scr = SC_MENU; s_dialog = 0; s_menu.sel = 0; s_menu.pos = 0;
    save_read(false);
    game_sfx_ensure(&SFX);
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(80);
    s_last = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint();
    nucleo_app_request_draw();
}
static void on_exit(void)
{
    save_write();
    nucleo_audio_stop();
    game_text_close();
}

static const nucleo_app_ram_t APP_RAM[] = { { (void **)&G, sizeof(Game) }, { nullptr, 0 } };

extern "C" void nucleo_register_sandgarden(void)
{
    static const nucleo_app_def_t app = {
        "giardino", "Giardino", "Games", "Unisci le piante e fai crescere l'albero d'oro",
        'G', C_GREEN, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP,   // the shared I2S line for the sound pack; restored on close
        APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
#endif  // !CONFIG_NUCLEO_STORE_SANDGARDEN || NAP_MODULE
