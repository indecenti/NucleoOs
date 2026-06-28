// pinball_levels.h — procedural, infinite level generator for app_pinball.cpp.
//
// Split out so the (pure, rendering-free) level math lives apart from the game/render code. A level is a
// plain config struct: theme colours, bumper/target layout, gravity, and the points goal to clear it.
// Generation is DETERMINISTIC from the level number (a small LCG seeded by n) so "level 7" always looks the
// same, and difficulty/variety rise forever. No heap, no globals — pb_gen_level() just fills the struct.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#define PBL_MAXBMP 5
#define PBL_MAXTGT 5

typedef struct {
    uint16_t field, field2, wall, wallL, accent, accent2;   // theme palette (RGB565)
    int   nbmp;  float bmpx[PBL_MAXBMP], bmpy[PBL_MAXBMP];  uint16_t bmpcol[PBL_MAXBMP];   // bumper layout
    int   ntgt;  float tgtx[PBL_MAXTGT], tgty[PBL_MAXTGT];                                  // target layout
    int   grav_e4;       // gravity * 10000 (e.g. 55 -> 0.0055 px/ms^2)
    int   bmp_score;     // per-bumper score this level
    int   goal;          // points to clear the level (advance)
    char  theme[12];     // short theme name shown on the level card
} PbLevel;

static inline uint16_t pbl_rgb(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint32_t pbl_rng(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static inline float pbl_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Fill L for level n (1-based). Playfield interior ~ lx[9..110], ly[10..232]; keep features in the upper 2/3.
static inline void pb_gen_level(int n, PbLevel *L)
{
    if (n < 1) n = 1;
    uint32_t s = (uint32_t)n * 2654435761u + 12345u;

    // ---- theme palette (6 schemes, cycling) ----
    static const char *NAMES[6] = { "COBALTO", "MAGENTA", "SMERALDO", "INFERNO", "OCEANO", "REALE" };
    int th = (n - 1) % 6;
    switch (th) {
        case 0: L->field = pbl_rgb(18,18,52);  L->field2 = pbl_rgb(30,28,92);  L->wall = pbl_rgb(70,90,200);   L->wallL = pbl_rgb(150,180,255); L->accent = pbl_rgb(96,212,236);  L->accent2 = pbl_rgb(255,122,182); break;
        case 1: L->field = pbl_rgb(40,14,50);  L->field2 = pbl_rgb(70,24,86);  L->wall = pbl_rgb(180,60,200);  L->wallL = pbl_rgb(255,150,255); L->accent = pbl_rgb(255,90,205);  L->accent2 = pbl_rgb(120,140,255); break;
        case 2: L->field = pbl_rgb(10,40,28);  L->field2 = pbl_rgb(18,66,44);  L->wall = pbl_rgb(40,170,90);   L->wallL = pbl_rgb(150,255,180); L->accent = pbl_rgb(120,232,142); L->accent2 = pbl_rgb(255,210,80);  break;
        case 3: L->field = pbl_rgb(46,16,10);  L->field2 = pbl_rgb(82,28,12);  L->wall = pbl_rgb(220,90,40);   L->wallL = pbl_rgb(255,180,90);  L->accent = pbl_rgb(255,150,50);  L->accent2 = pbl_rgb(255,84,70);   break;
        case 4: L->field = pbl_rgb(8,36,44);   L->field2 = pbl_rgb(14,60,72);  L->wall = pbl_rgb(40,150,170);  L->wallL = pbl_rgb(150,240,255); L->accent = pbl_rgb(96,236,224);  L->accent2 = pbl_rgb(120,200,255); break;
        default:L->field = pbl_rgb(26,18,52);  L->field2 = pbl_rgb(44,30,84);  L->wall = pbl_rgb(190,158,44);  L->wallL = pbl_rgb(255,220,120); L->accent = pbl_rgb(255,196,64);  L->accent2 = pbl_rgb(192,112,232); break;
    }
    snprintf(L->theme, sizeof L->theme, "%s", NAMES[th]);

    // ---- bumpers: count grows, arrangement varies by pattern ----
    int nb = 3 + ((n - 1) / 2) % 3;            // 3,3,4,4,5,5,...
    if (nb > PBL_MAXBMP) nb = PBL_MAXBMP;
    L->nbmp = nb;
    int pattern = (int)(pbl_rng(&s) % 4u);
    uint16_t bcol[4] = { L->accent, L->accent2, L->wallL, pbl_rgb(255, 210, 90) };
    for (int i = 0; i < nb; i++) {
        float t = nb > 1 ? (float)i / (nb - 1) : 0.5f, cx, cy;
        switch (pattern) {
            case 0: cx = 18 + t * 98.0f;              cy = 62 + ((i & 1) ? 16 : -6); break;       // zig-zag bank
            case 1: cx = 67 + cosf(t * 6.2831f) * 34; cy = 66 + sinf(t * 6.2831f) * 22; break;     // ring
            case 2: cx = 18 + t * 98.0f;              cy = 50 + (i == nb / 2 ? -6 : 20); break;    // shallow arc
            default: cx = 24 + (i % 3) * 34;          cy = 52 + (i / 3) * 26; break;               // grid
        }
        L->bmpx[i] = pbl_clampf(cx, 14, 120);          // full-width playfield
        L->bmpy[i] = pbl_clampf(cy, 44, 92);
        L->bmpcol[i] = bcol[i % 4];
    }
    // de-cluster: relax apart any bumpers that overlap (cleaner layout + cleaner collisions, no jitter)
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < nb; i++)
            for (int j = i + 1; j < nb; j++) {
                float dx = L->bmpx[j] - L->bmpx[i], dy = L->bmpy[j] - L->bmpy[i];
                float dist = sqrtf(dx * dx + dy * dy), mind = 24.0f;   // NB: 'd' is a system macro (app_gfx.h)
                if (dist > 0.001f && dist < mind) {
                    float push = (mind - dist) * 0.5f, ux = dx / dist, uy = dy / dist;
                    L->bmpx[i] -= ux * push; L->bmpy[i] -= uy * push;
                    L->bmpx[j] += ux * push; L->bmpy[j] += uy * push;
                }
            }
    for (int i = 0; i < nb; i++) { L->bmpx[i] = pbl_clampf(L->bmpx[i], 14, 120); L->bmpy[i] = pbl_clampf(L->bmpy[i], 44, 92); }

    // ---- targets: a top bank, count 3 or 4, in a gentle arch ----
    int nt = 3 + (int)(pbl_rng(&s) % 2u);
    if (nt > PBL_MAXTGT) nt = PBL_MAXTGT;
    L->ntgt = nt;
    for (int i = 0; i < nt; i++) {
        float u = nt > 1 ? (float)i / (nt - 1) : 0.5f;
        L->tgtx[i] = 16 + i * (100.0f / (nt > 1 ? nt - 1 : 1));     // spread across the full width
        L->tgty[i] = 24 + sinf(u * 3.14159f) * 6.0f;               // arch (centre targets sit a touch lower)
    }

    // ---- difficulty / economy ----
    int g = 272 + (n - 1) * 3; if (g > 328) g = 328;   // FIELD gravity (new physics scale), gentle per-level ramp
    L->grav_e4 = g;
    L->bmp_score = 100 + (n - 1) * 20;
    L->goal = 1800 + n * 1200;   // completable in reasonable time; the HUD meter shows progress
}
