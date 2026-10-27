// app_reactor.cpp — NucleoOS "Reattore": a reactor-control arcade game (category "Games").
//
// Front-end (MENU / SETTINGS / HELP / SCORES / PLAY / PAUSE / GAME-OVER / NAME) on the shared console kit
// (game_ui.h), text in the OS language (GT), and a heavily juiced presentation: spring-driven ring gauges,
// directional rod sparks, a heat-scaled danger telegraph (visual + accelerating heartbeat), a SCRAM
// cool-flash, score popups, demand-rise warnings, death-specific game-over animations, ambient embers.
// All feedback doubles as readability.
//
// Constraints honoured: exclusive_flags = NX_NET_APP (dedicated RAM + the I2S line free for the SFX);
// state is static except the leaderboard (APP_RAM, 0 B while closed); drawing goes through the buffered
// `d.` path; 8bpp has no alpha, so every fade/glow is an integer channel-mix toward another solid colour.
// A ~30 Hz poll animates and steps the sim at a fixed 10 Hz timestep; still screens are not redrawn.
// LEFT/BACK reach on_back(), all other keys reach on_key(). Text is ASCII only. Never name a local `d`.

#if __has_include("sdkconfig.h")       // the host harnesses compile this file without one
#include "sdkconfig.h"
#endif
// Built in, or (CONFIG_NUCLEO_STORE_REACTOR) only as the store's native module, built with NAP_MODULE.
#if !CONFIG_NUCLEO_STORE_REACTOR || defined(NAP_MODULE)
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "game_text.h"
#include "game_ui.h"
#include "app_large.h"          // two screens: the reactor gauges on the 320x240 panel, the Cardputer companion
#include "nucleo_touch.h"
#include "nucleo_exclusive.h"   // NX_NET_APP: dedicate RAM + free the shared I2S line so SFX play
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
#include "esp_timer.h"
}

// ============================ palette ========================================
static inline uint16_t rgb(int r, int g, int b) { return gui::rgb(r, g, b); }     // RGB332-exact
static inline uint16_t mix(uint16_t a, uint16_t b, int t) { return gui::mix(a, b, t); }
#define COL_BG     rgb(0, 0, 0)
#define COL_TRACK  rgb(36, 36, 85)
#define COL_WHITE  rgb(255, 255, 255)
#define COL_CYAN   rgb(109, 219, 255)
#define COL_AMBER  rgb(255, 182, 85)
#define COL_GREEN  rgb(109, 219, 170)
#define COL_RED    rgb(255, 73, 85)
#define COL_GREY   rgb(146, 146, 170)
#define COL_DIM    rgb(73, 73, 85)
#define ACC        COL_AMBER
#define CH         s_vh                  // the view height: the compact frame, or the panel for the game-over

// ============================ model ==========================================
#define NROD      6
#define ROD_MAX   5
#define POWER_PER_R 10
#define POWER_MAX (NROD * ROD_MAX * POWER_PER_R)   // 300
#define STEP_MS   100

enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OVER, ST_SCORES, ST_NAME, ST_PAUSE };

static int   g_audio = 1, g_diff = 1, g_best = 0;

static int   s_screen, s_lsel;             // s_lsel: the picked row of the panel pause card
static int   s_vw = W, s_vh = H - HINT;    // the view the game-over spectacle is drawn in (lp_over: 320x240)
static int64_t s_now, s_last, s_frame;
static int   s_acc;
static unsigned s_anim;

static gui::Menu s_menu;                   // title / settings list cursor
static int   s_help;

static uint8_t s_rod[NROD];
static int   s_cur;
static float s_heat, s_stab;
static int   s_power, s_demand;
static int   s_score, s_time_ms, s_over_reason;
static float s_dpow, s_dheat, s_dstab;     // sprung display values
static float s_vpow, s_vheat, s_vstab;     // spring velocities

// juice timers / one-shots (ms)
static int   s_rodfx_ms, s_rodfx_dir;
static int   s_scram_ms, s_dem_ms, s_pop_ms, s_popN, s_lastscore, s_milestone;
static int   s_dem_n; static bool s_dem_dip;          // demand changes so far / the last one was an off-peak dip
static int   s_alarm_ms, s_over_ms, s_over_total;
static bool  s_newbest, s_bestplayed, s_qualify;

#define NSCORES 10
struct Score { char name[12]; int score; int diff; };
static Score *s_scores;              // NSCORES entries, sorted descending: APP_RAM (0 B while closed)
static char  s_name[12];             // current / last-entered pilot name
static int   s_newrank;              // row to highlight on the board (-1 = none)

static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static const char *diff_name(int df) { return df == 0 ? GT("Facile", "Easy") : df == 2 ? GT("Difficile", "Hard") : GT("Normale", "Normal"); }

// ============================ persistence (cfg) ==============================
#define DIRR "/sd/data/reattore"
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, df, b; } c = { 0x52454143u, 0, g_audio, g_diff, g_best };   // l: retired language slot
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a, df, b; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == 0x52454143u) { g_audio = c.a ? 1 : 0; g_diff = (c.df < 0 || c.df > 2) ? 1 : c.df; g_best = c.b; }
}

// ---- leaderboard (top 10) ---------------------------------------------------
#define SCORES_MAGIC 0x32435352u   // 'RSC2'
struct ScoresHdr { uint32_t m; uint16_t v, p; char last[12]; };   // then NSCORES x Score
static void scores_load(void)
{
    for (int i = 0; i < NSCORES; i++) { snprintf(s_scores[i].name, sizeof s_scores[i].name, "%s", "---"); s_scores[i].score = 0; s_scores[i].diff = 1; }
    snprintf(s_name, sizeof s_name, "%s", GT("PILOTA", "PILOT"));
    FILE *f = fopen(DIRR "/scores.bin", "rb");
    if (!f) return;
    ScoresHdr h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.m == SCORES_MAGIC && fread(s_scores, sizeof(Score), NSCORES, f) == NSCORES;
    fclose(f);
    if (!ok) { for (int i = 0; i < NSCORES; i++) { s_scores[i].score = 0; snprintf(s_scores[i].name, 12, "%s", "---"); } return; }
    h.last[11] = 0;
    if (h.last[0]) snprintf(s_name, sizeof s_name, "%s", h.last);
    for (int i = 0; i < NSCORES; i++) { s_scores[i].name[11] = 0; if (s_scores[i].diff < 0 || s_scores[i].diff > 2) s_scores[i].diff = 1; }
}
static void scores_save(void)
{
    ensure_dirs();
    ScoresHdr h;
    memset(&h, 0, sizeof h);
    h.m = SCORES_MAGIC; h.v = 1;
    snprintf(h.last, sizeof h.last, "%s", s_name);
    FILE *f = fopen(DIRR "/scores.tmp", "wb");
    if (!f) return;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(s_scores, sizeof(Score), NSCORES, f) == NSCORES;
    fclose(f);
    if (!ok) { remove(DIRR "/scores.tmp"); return; }
    remove(DIRR "/scores.bin");
    rename(DIRR "/scores.tmp", DIRR "/scores.bin");
}
static bool score_qualifies(int sc) { return sc > 0 && sc > s_scores[NSCORES - 1].score; }
static int score_insert(const char *name, int sc, int diff)
{
    int rank = -1;
    for (int i = 0; i < NSCORES; i++) if (sc > s_scores[i].score) { rank = i; break; }
    if (rank < 0) return -1;
    for (int j = NSCORES - 1; j > rank; j--) s_scores[j] = s_scores[j - 1];
    snprintf(s_scores[rank].name, sizeof s_scores[rank].name, "%s", (name && name[0]) ? name : GT("PILOTA", "PILOT"));
    s_scores[rank].score = sc; s_scores[rank].diff = diff;
    return rank;
}

// ============================ audio ==========================================
// Every cue is a WAV of the game's pack, rendered on the PC (tools/sfx-gen/games/reactor.py ->
// /sd/data/reattore/pack); a user WAV in /sd/data/reattore/custom/ overrides it. Nothing is synthesized on
// the device (synthesizing all 12 cues on open stalled the app task). No WAV -> an important cue is a tone.
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "move"; case 2: return "ok";   case 3: return "back";  case 4: return "out";
        case 5: return "in";   case 6: return "scram";case 7: return "alarm"; case 8: return "boom";
        case 9: return "start";case 10:return "heart";case 11:return "rise"; case 12:return "klaxon"; default: return "x";
    }
}
static bool sfx_important(int id) { return id == 6 || id == 8 || id == 9 || id == 12; }   // SCRAM, meltdown, start, klaxon interrupt
static bool wav_ok(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 44; }
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    bool imp = sfx_important(id);
    if (!imp && nucleo_audio_is_playing()) return;                     // small cues never stack
    char p[48];
    snprintf(p, sizeof p, DIRR "/custom/%s.wav", sfx_name(id));
    if (!wav_ok(p)) snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id));
    if (!wav_ok(p)) { if (imp) nucleo_audio_tone(id == 8 ? 90 : 660, 60, 60); return; }
    if (imp) nucleo_audio_stop();
    nucleo_audio_play(p);
}

// ============================ draw helpers ===================================
// The panel paints in clipped strip windows (lg::frame), where drawWideLine widens the clip and writes outside
// the window: there (s_flat) the demand needle is two plain lines.
static bool s_flat;
// transparent text (single-arg colour) — avoids opaque glyph boxes punching holes in rings/bars
static void text_at(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); d.setTextSize(1); }
static void text_c(int cx, int y, int sz, uint16_t col, const char *s) { text_at(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }

static uint16_t heat_col(int pct)
{
    if (pct < 45) return COL_GREEN;
    if (pct < 70) return COL_AMBER;
    if (pct < 85) return rgb(255, 146, 0);
    return COL_RED;
}
static void ring(int cx, int cy, int r, int pct, uint16_t col, bool redzone)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d.fillArc(cx, cy, r - 9, r, 0, 360, COL_TRACK);
    if (redzone) d.fillArc(cx, cy, r - 9, r, 360 * 85 / 100, 360, rgb(109, 0, 0));
    if (pct > 0) d.fillArc(cx, cy, r - 9, r, 0, 360 * pct / 100, col);
    d.drawCircle(cx, cy, r, mix(col, COL_BG, 140));         // crisp coloured outer rim
    d.drawCircle(cx, cy, r - 9, rgb(36, 36, 85));          // inner rim
}
static void radial_tick(int cx, int cy, int r0, int r1, int pct, uint16_t col)
{
    float a = (float)pct * 3.6f * 0.01745329f;
    float ca = cosf(a), sa = sinf(a);
    if (s_flat) { for (int k = 0; k < 2; k++) d.drawLine(cx + (int)(ca * r0) + k, cy + (int)(sa * r0), cx + (int)(ca * r1) + k, cy + (int)(sa * r1), col); return; }
    d.drawWideLine(cx + (int)(ca * r0), cy + (int)(sa * r0), cx + (int)(ca * r1), cy + (int)(sa * r1), 1.0f, col);
}
static void draw_core(int cx, int cy, int base, uint16_t col)
{
    int pulse = base + (int)(3 * sinf(s_anim * 0.12f));
    d.drawCircle(cx, cy, base + 14, mix(col, COL_BG, 180));
    for (int r = pulse + 9; r > pulse; r -= 2) d.drawCircle(cx, cy, r, mix(col, COL_BG, 140));
    d.fillSmoothCircle(cx, cy, pulse, col);
    d.fillCircle(cx - pulse / 3, cy - pulse / 3, pulse / 3, COL_WHITE);
    for (int e = 0; e < 4; e++) {                                   // orbiting embers
        float a = s_anim * 0.06f + e * 1.57f;
        int rr = base + 11 + (e & 1) * 4;
        d.fillCircle(cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr), 1, rgb(255, 182, 85));
    }
}

// ============================ simulation =====================================
static const int8_t  COOL[3]      = { 18, 16, 14 };
static const uint8_t DEM_START[3] = { 60, 70, 80 };
static const int8_t  DEM_STEP[3]  = { 16, 24, 28 };
static const int8_t  DEM_EVERY[3] = { 15, 14, 11 };

static int reactivity(void) { int r = 0; for (int i = 0; i < NROD; i++) r += ROD_MAX - s_rod[i]; return r; }

static void set_hint(void);
static void go(int s);
static void new_game(void)
{
    for (int i = 0; i < NROD; i++) s_rod[i] = 3;
    s_cur = 0; s_heat = 18; s_stab = 100; s_score = 0; s_time_ms = 0; s_acc = 0;
    s_demand = DEM_START[g_diff];
    s_power = reactivity() * POWER_PER_R;
    s_dpow = s_power; s_dheat = s_heat; s_dstab = s_stab;
    s_vpow = s_vheat = s_vstab = 0;
    s_rodfx_ms = s_scram_ms = s_dem_ms = s_pop_ms = s_popN = 0;
    s_lastscore = 0; s_milestone = 0; s_alarm_ms = 0; s_dem_n = 0; s_dem_dip = false;
    go(ST_PLAY);
    sfx(9);
}
static void game_over(int reason)
{
    s_over_reason = reason;
    s_over_total = s_over_ms = (reason == 0) ? 1800 : 700;        // meltdown earns a long spectacle
    s_newbest = (s_score > g_best); s_bestplayed = false;
    s_qualify = score_qualifies(s_score);
    if (s_newbest) { g_best = s_score; cfg_write(); }
    go(ST_OVER);
    sfx(8);
}
static void sim_step(void)
{
    int R = reactivity();
    s_power = R * POWER_PER_R;
    float dt = STEP_MS / 1000.0f;
    s_heat += (R * 1.4f - COOL[g_diff]) * dt;
    if (s_heat < 0) s_heat = 0;

    if (s_power >= s_demand) { s_stab += 1.4f; if (s_stab > 100) s_stab = 100; }
    else { s_stab -= (1.0f + (s_demand - s_power) / 22.0f); if (s_stab < 0) s_stab = 0; }

    int paid = s_power < s_demand ? s_power : s_demand;
    s_score += paid / 24;
    int gain = s_score - s_lastscore;
    if (gain > 0) { s_popN = gain; s_pop_ms = 420; s_lastscore = s_score; }
    if (s_score / 100 != s_milestone) { s_milestone = s_score / 100; sfx(2); }

    s_time_ms += STEP_MS;
    // The grid's demand: three rises, then an off-peak dip of two steps — the trend still climbs (the end
    // always comes), but a steady hand gets a breather to cool the core instead of a wall at ~50 s.
    if ((s_time_ms / 1000) > 0 && (s_time_ms % (DEM_EVERY[g_diff] * 1000)) < STEP_MS) {
        s_dem_dip = (++s_dem_n % 4) == 0;
        s_demand += s_dem_dip ? -2 * DEM_STEP[g_diff] : DEM_STEP[g_diff];
        if (s_demand < DEM_START[g_diff]) s_demand = DEM_START[g_diff];
        s_dem_ms = 500; sfx(s_dem_dip ? 5 : 11);
    }

    if (s_heat >= 100) { game_over(0); return; }
    if (s_stab <= 0)   { game_over(1); return; }

    if (s_heat >= 90) {                                              // PANIC: urgent fast klaxon
        int period = 460 - (int)((s_heat - 90) * 22);               // ~460ms @90% -> ~240ms near meltdown
        s_alarm_ms += STEP_MS;
        if (s_alarm_ms >= period) { s_alarm_ms = 0; sfx(12); }
    } else if (s_heat >= 82) {                                       // warning: accelerating heartbeat
        int dz = (int)((s_heat - 82) * 100 / 18);
        int period = 900 - dz * 6;
        s_alarm_ms += STEP_MS;
        if (s_alarm_ms >= period) { s_alarm_ms = 0; sfx(10); }
    } else s_alarm_ms = 0;
}
static void scram(void) { for (int i = 0; i < NROD; i++) s_rod[i] = ROD_MAX; s_scram_ms = 220; sfx(6); }
static void rod_adjust(int delta)
{
    int v = (int)s_rod[s_cur] - delta;                              // delta +1 = pull OUT
    if (v < 0) v = 0;
    if (v > ROD_MAX) v = ROD_MAX;
    if (v != s_rod[s_cur]) { s_rod[s_cur] = (uint8_t)v; s_rodfx_ms = 120; s_rodfx_dir = delta; sfx(delta > 0 ? 4 : 5); }
}

// ============================ screens ========================================
#define NMENU 3
#define NSET  3
static void draw_menu(void)
{
    char sub[32]; snprintf(sub, sizeof sub, GT("Record %d", "Best %d"), g_best);
    int y = gui::title(GT("REATTORE", "REACTOR"), sub, ACC);
    draw_core(W - 22, 18, 4, heat_col(40 + (int)(20 * sinf(s_anim * 0.05f))));
    const char *items[NMENU] = { GT("Gioca", "Play"), GT("Classifica", "Leaderboard"), GT("Impostazioni", "Settings") };
    gui::menu(s_menu, items, NMENU, y, CH, ACC);
}
// The guide's five lines of page pg and their colours (shared by the compact page and the panel page).
static void help_lines(int pg, const char **l, uint16_t *c)
{
    for (int i = 0; i < 5; i++) c[i] = COL_WHITE;
    if (pg == 0) {
        l[0] = GT("Tieni la POTENZA (MW)", "Keep the POWER (MW)");
        l[1] = GT("pari alla DOMANDA della rete", "equal to the grid DEMAND");
        l[2] = GT("senza fondere il nucleo.", "without melting the core.");
        l[3] = GT("La domanda sale col tempo:", "Demand rises over time:");
        l[4] = GT("resisti il piu' a lungo!", "survive as long as you can!");
        c[2] = COL_AMBER; c[4] = COL_GREEN;
    } else if (pg == 1) {
        l[0] = GT("SX/DX o 1-6: scegli la barra", "LEFT/RIGHT or 1-6: pick a rod");
        l[1] = GT("SU o W: estrai = +potenza", "UP or W: pull out = +power");
        l[2] = GT("GIU o E: inserisci = raffredda", "DOWN or E: push in = cools");
        l[3] = GT("SPAZIO: SCRAM, tutte dentro", "SPACE: SCRAM, all rods in");
        l[4] = GT("Esc: pausa", "Esc: pause");
        c[1] = COL_GREEN; c[2] = COL_CYAN; c[3] = COL_AMBER; c[4] = COL_GREY;
    } else {
        l[0] = GT("Il CALORE sale con la potenza.", "HEAT rises with the power.");
        l[1] = GT("Al 100% e' FUSIONE.", "At 100% it is MELTDOWN.");
        l[2] = GT("Potenza sotto la domanda:", "Power below the demand:");
        l[3] = GT("la RETE cede = BLACKOUT.", "the GRID fails = BLACKOUT.");
        l[4] = GT("Il cuore batte: rallenta!", "A heartbeat: slow down!");
        c[1] = COL_RED; c[3] = COL_RED; c[4] = COL_AMBER;
    }
}
static const char *help_title(int pg) { return pg == 0 ? GT("Obiettivo", "Objective") : pg == 1 ? GT("Comandi", "Controls") : GT("Pericoli", "Hazards"); }
static void draw_help(void)
{
    int y = gui::title(help_title(s_help), nullptr, ACC);
    for (int i = 0; i < 3; i++) d.fillCircle(W - 26 + i * 8, 14, i == s_help ? 3 : 2, i == s_help ? ACC : COL_DIM);   // page dots
    const char *l[5]; uint16_t c[5];
    help_lines(s_help, l, c);
    for (int i = 0; i < 5; i++) {
        if (gui::text_width(l[i], gui::F_SMALL) <= W - 16) gui::text(l[i], 8, y + i * 16, 0, gui::F_SMALL, c[i], COL_BG);
        else text_at(8, y + i * 16 + 4, 1, c[i], l[i]);           // a long translation: the 6x8 face
    }
}
static void draw_settings(void)
{
    int y = gui::title(GT("Impostazioni", "Settings"), nullptr, ACC);
    char a[32], df[40];
    snprintf(a, sizeof a, "%s: %s", GT("Audio", "Audio"), g_audio ? GT("Si", "On") : GT("No", "Off"));
    snprintf(df, sizeof df, "%s: %s", GT("Difficolta", "Difficulty"), diff_name(g_diff));
    const char *items[NSET] = { a, df, GT("Come si gioca", "How to play") };
    gui::menu(s_menu, items, NSET, y, CH, ACC);
}
static void draw_play(void)
{
    d.fillRect(0, 0, W, CH, COL_BG);
    char b[24];

    // top strip: time + score (with brighten-pop) + floating +N
    int sec = s_time_ms / 1000;
    snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    gui::text(b, 4, 0, 0, gui::F_SMALL, COL_GREY, COL_BG);
    snprintf(b, sizeof b, "%d", s_score);
    gui::text(b, W - 4, 0, 2, gui::F_SMALL, s_pop_ms > 300 ? COL_WHITE : COL_AMBER, COL_BG);
    if (s_pop_ms > 0) {
        char pb[12]; snprintf(pb, sizeof pb, "+%d", s_popN);
        int rise = (420 - s_pop_ms) / 42;
        text_at(W - 4 - (int)strlen(pb) * 6, 16 - rise, 1, mix(COL_GREEN, COL_BG, (420 - s_pop_ms) * 256 / 420), pb);
    }

    bool met = s_power >= s_demand;
    int hpct = (int)s_dheat;
    int dz = s_dheat < 82 ? 0 : (int)((s_dheat - 82) * 100 / 18);
    if (dz > 100) dz = 100;

    // POWER ring (left) — calm white shimmer when locked on; the demand is the white needle
    int px = 50, py = 48, pr = 31;
    int ppct = (int)(s_dpow * 100 / POWER_MAX);
    int sh = (int)(40 * (0.5f + 0.5f * sinf(s_anim * 0.18f)));
    uint16_t pcol = met ? mix(COL_GREEN, COL_WHITE, sh) : COL_AMBER;
    ring(px, py, pr, ppct, pcol, false);
    radial_tick(px, py, pr - 11, pr + 2 + (met ? sh / 20 : 0), s_demand * 100 / POWER_MAX, COL_WHITE);
    snprintf(b, sizeof b, "%d", (int)(s_dpow + 0.5f));
    gui::text(b, px, py - 15, 1, gui::F_BIG, COL_WHITE, COL_DIM);
    text_c(px, py + 11, 1, COL_GREY, "MW");

    // CORE heat ring (right) — heat-tinted big number, strobing when critical
    int hx = 190, hy = 48, hr = 31;
    ring(hx, hy, hr, hpct, (dz >= 45 && (s_anim & 1)) ? COL_WHITE : heat_col(hpct), true);
    if (dz > 50) d.drawCircle(hx, hy, hr + 1, rgb(146, 36, 0));
    snprintf(b, sizeof b, "%d", hpct);
    gui::text(b, hx, hy - 15, 1, gui::F_BIG, dz > 0 ? rgb(255, 109 + (100 - dz), 85) : heat_col(hpct), COL_DIM);
    text_c(hx, hy + 11, 1, COL_GREY, "%");

    // centre: demand (with rise-telegraph) + grid stability
    gui::text(GT("DOMANDA", "DEMAND"), W / 2, 16, 1, gui::F_SMALL, COL_GREY, COL_BG);
    snprintf(b, sizeof b, "%d", s_demand);
    gui::text(b, W / 2, 31, 1, gui::F_BODY, s_dem_ms > 0 ? COL_WHITE : COL_AMBER, COL_DIM);
    if (s_dem_ms > 0) {                                              // a rise (amber, up) or an off-peak dip (cyan, down)
        uint16_t tc = s_dem_dip ? COL_CYAN : COL_AMBER;
        d.drawFastHLine(W / 2 - 18, 50, 36 * s_dem_ms / 500, tc);
        int ax = W / 2 + 26, ay = 38;
        if (s_dem_dip) d.fillTriangle(ax - 4, ay - 3, ax + 4, ay - 3, ax, ay + 3, tc);
        else           d.fillTriangle(ax - 4, ay + 3, ax + 4, ay + 3, ax, ay - 3, tc);
    }
    gui::text(GT("RETE", "GRID"), W / 2, 53, 1, gui::F_SMALL, COL_GREY, COL_BG);
    int sw = (int)s_dstab * 50 / 100;
    d.fillRoundRect(W / 2 - 26, 70, 52, 7, 2, COL_TRACK);
    if (sw > 0) d.fillRoundRect(W / 2 - 25, 71, sw, 5, 2, s_stab > 35 ? COL_CYAN : COL_RED);

    // rod bank — recessed slots inside a console panel
    int ty = 88, th = 26, pitch = 36, x0 = (W - (NROD - 1) * pitch - 24) / 2;
    d.fillRoundRect(3, ty - 4, W - 6, th + 8, 5, rgb(0, 0, 85));
    d.drawRoundRect(3, ty - 4, W - 6, th + 8, 5, rgb(36, 73, 170));
    for (int i = 0; i < NROD; i++) {
        int x = x0 + i * pitch;
        bool sel = (i == s_cur);
        d.fillRoundRect(x, ty, 24, th, 4, rgb(0, 0, 0));                 // recessed slot
        int usable = th - 6;
        int hyh = ty + 3 + s_rod[i] * usable / ROD_MAX;
        uint16_t fuel = sel ? rgb(255, 219, 170) : heat_col(hpct);
        int fh = hyh - (ty + 3);
        if (fh > 0) {
            d.fillRoundRect(x + 3, ty + 3, 18, fh, 2, fuel);            // exposed fuel column
            d.drawFastVLine(x + 4, ty + 4, fh > 2 ? fh - 2 : 1, mix(fuel, COL_WHITE, 95));  // gloss
        }
        d.fillRect(x + 3, hyh, 18, (ty + th - 3) - hyh, rgb(73, 73, 85)); // inserted rod
        d.fillRoundRect(x + 2, hyh - 1, 20, 3, 1, sel ? COL_WHITE : rgb(182, 182, 170)); // handle
        d.drawRoundRect(x, ty, 24, th, 4, sel ? COL_AMBER : rgb(36, 73, 170));          // slot frame (amber when picked)
        if (sel) {
            d.fillTriangle(x + 12, ty - 9, x + 8, ty - 4, x + 16, ty - 4, COL_AMBER);
            if (s_rodfx_ms > 0) {                                       // directional spark
                if (s_rodfx_dir > 0) { for (int k = -1; k <= 1; k++) d.drawLine(x + 12, ty - 5, x + 12 + k * 3, ty - 11, k ? COL_CYAN : COL_WHITE); }
                else d.fillRect(x + 6, hyh + 2, 12, 4, COL_CYAN);
            }
        } else {
            char n[2] = { (char)('1' + i), 0 };                         // its quick-pick digit
            text_c(x + 12, ty - 10, 1, COL_DIM, n);
        }
    }

    // SCRAM cool-flash + downward wipe
    if (s_scram_ms > 0) {
        if (s_scram_ms > 180) d.fillRect(0, 0, W, CH, mix(rgb(182, 219, 255), COL_BG, (220 - s_scram_ms) * 256 / 40));
        int yw = (220 - s_scram_ms) * CH / 220;
        if (yw - 2 >= 0) d.drawFastHLine(0, yw - 2, W, rgb(36, 109, 170));
        d.drawFastHLine(0, yw, W, COL_CYAN);
    }
    // continuous danger vignette — a red glow fading inward, breathing with the heat
    if (dz > 0) {
        int amp = (int)(dz * (0.55f + 0.45f * sinf(s_anim * 0.4f)));
        for (int k = 0; k < 4; k++) d.drawRect(k, k, W - 2 * k, CH - 2 * k, rgb(36 + amp * (4 - k) / 3, 0, 0));
    }
    if (s_dheat >= 90 && ((s_anim >> 1) & 1)) {                          // PANIC banner (blinks, between time & score)
        const char *w = GT("! CRITICO !", "! CRITICAL !");
        int ww = gui::text_width(w, gui::F_SMALL) + 14;
        d.fillRoundRect(W / 2 - ww / 2, 0, ww, 16, 3, COL_RED);
        gui::text(w, W / 2, 0, 1, gui::F_SMALL, COL_WHITE, rgb(109, 0, 0));
    }
}
static void draw_card(int prog)   // score + best count-up, shared by both deaths
{
    const int W = s_vw;
    if (prog > 1000) prog = 1000;
    if (prog < 0) prog = 0;
    char b[32];
    snprintf(b, sizeof b, GT("Punteggio %d", "Score %d"), s_score * prog / 1000);
    gui::text(b, W / 2, CH * 63 / 100, 1, gui::F_BODY, COL_WHITE, COL_BG);
    snprintf(b, sizeof b, GT("Record %d%s", "Best %d%s"), g_best * prog / 1000, s_newbest ? "  *" : "");
    gui::text(b, W / 2, CH * 81 / 100, 1, gui::F_SMALL, s_newbest ? COL_GREEN : COL_AMBER, COL_BG);
}
static void draw_blackout(int e)
{
    const int W = s_vw;
    d.fillRect(0, 0, W, CH, COL_BG);
    draw_core(W / 2, CH / 4, 12, rgb(73, 73, 170));
    if (e >= 500 || ((s_anim >> 1) & 3)) gui::text("BLACKOUT", W / 2, CH * 2 / 5, 1, gui::F_TITLE, COL_CYAN, rgb(0, 36, 85));   // flickers in
    draw_card(e > 120 ? (e - 120) * 1000 / 480 : 0);
}
static void draw_meltdown(int e)
{
    const int W = s_vw;
    int cx = W / 2, cyc = CH / 2;
    float k = s_over_total ? (float)s_over_ms / s_over_total : 0;            // 1 at blast -> 0 as it settles
    int shake = (int)(7 * k);
    int ox = shake ? ((int)(s_anim * 37) % (2 * shake + 1) - shake) : 0;
    int oy = shake ? ((int)(s_anim * 53) % (2 * shake + 1) - shake) : 0;

    for (int y = 0; y < CH; y += 3) {                                       // roiling plasma, fading to dark
        int n = ((y * 7 + (int)s_anim * 5) % 23);
        int hh = 40 + n * 7;
        d.fillRect(0, y, W, 3, mix(rgb(hh, hh / 4, 0), COL_BG, (int)((1.0f - k) * 220)));
    }
    int swEnd = s_over_total * 6 / 10;                                      // expanding shockwaves
    if (e < swEnd) {
        int maxr = e * W / swEnd;
        for (int s = 0; s < 3; s++) { int r = maxr - s * 12; if (r > 2) d.drawCircle(cx + ox, cyc + oy, r, s == 0 ? COL_WHITE : (s == 1 ? rgb(255, 182, 85) : COL_RED)); }
    }
    for (int i = 0; i < 18; i++) {                                          // radial debris with motion trails
        float a = i * 0.349f;
        int dist = (int)(e * (0.10f + (i % 5) * 0.03f));
        float ca = cosf(a), sa = sinf(a);
        int x = cx + (int)(ca * dist) + ox, y = cyc + (int)(sa * dist) + oy;
        d.drawLine(cx + (int)(ca * (dist - 7)) + ox, cyc + (int)(sa * (dist - 7)) + oy, x, y, rgb(255, 109, 0));
        d.fillCircle(x, y, 1 + (i & 1), (i & 1) ? rgb(255, 219, 85) : COL_WHITE);
    }
    if (e < 700) { int rr = 11 - e / 70; if (rr > 0) d.fillCircle(cx + ox, cyc + oy, rr, mix(COL_WHITE, COL_RED, e * 256 / 700)); }
    if (e < 160) d.fillRect(0, 0, W, CH, mix(COL_WHITE, COL_BG, e * 256 / 160));   // detonation whiteout
    if (e > 240) {                                                          // title slams in, strobing
        uint16_t tc = ((s_anim >> 1) & 1) ? COL_WHITE : COL_RED;
        gui::text(GT("FUSIONE", "MELTDOWN"), cx + ox, CH * 3 / 10 + oy, 1, gui::F_TITLE, tc, rgb(109, 0, 0));
    }
    if (e > s_over_total - 650) draw_card((e - (s_over_total - 650)) * 1000 / 600);   // card as chaos settles
}
static void draw_over(void)
{
    int e = s_over_total - s_over_ms;
    const bool clip = s_vw == W;                 // (the panel strip is already clipped: never widen it)
    if (clip) d.setClipRect(0, 0, W, CH);        // shockwaves + debris fly off-screen: keep them above the footer
    if (s_over_reason == 0) draw_meltdown(e);
    else draw_blackout(e);
    if (clip) d.clearClipRect();
}
static void draw_scores(void)
{
    int y = gui::title(GT("Classifica", "Leaderboard"), nullptr, ACC);
    for (int i = 0; i < NSCORES; i++) {
        int x = 4 + (i / 5) * 118, ry = y + (i % 5) * 16;
        bool hot = (i == s_newrank);
        if (hot) d.fillRoundRect(x - 2, ry - 1, 116, 15, 4, rgb(73, 36, 0));
        uint16_t c = hot ? COL_WHITE : (i < 3 ? COL_AMBER : COL_GREY);
        char r[16]; snprintf(r, sizeof r, "%2d", i + 1);
        text_at(x, ry + 3, 1, c, r);
        if (!s_scores[i].score) { text_at(x + 20, ry + 3, 1, COL_DIM, "-"); continue; }
        d.fillRect(x + 14, ry + 5, 3, 3, s_scores[i].diff == 0 ? COL_GREEN : s_scores[i].diff == 2 ? COL_RED : COL_AMBER);   // difficulty pip
        snprintf(r, sizeof r, "%.10s", s_scores[i].name);
        text_at(x + 20, ry + 3, 1, c, r);
        snprintf(r, sizeof r, "%d", s_scores[i].score);
        text_at(x + 114 - (int)strlen(r) * 6, ry + 3, 1, c, r);
    }
}
static void draw_name(void)
{
    char sub[32]; snprintf(sub, sizeof sub, GT("Punteggio %d", "Score %d"), s_score);
    gui::title(GT("Nuovo record!", "New record!"), sub, ACC);
    draw_core(W - 22, 18, 4, COL_AMBER);
    gui::text(GT("Il tuo nome:", "Your name:"), W / 2, 58, 1, gui::F_SMALL, COL_GREY, COL_BG);
    gui::panel(40, 78, W - 80, 24, rgb(0, 0, 85), COL_CYAN);
    text_at(48, 83, 2, COL_WHITE, s_name);
    int cxp = 48 + (int)strlen(s_name) * 12;
    if ((s_anim >> 3) & 1) d.fillRect(cxp + 1, 82, 9, 16, COL_CYAN);
}

// ============================ input + hint ===================================
// A sideways arrow from the panel's rod-bank drag (no key down): ignored, so a drag only slides the rod (a tap
// picks the rod). The Cardputer's arrow keys are physically down and keep stepping the selection.
static bool pad_side(char c) { return nucleo_app_two_screens() && !nucleo_kbd_char_down(c); }
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU:   nucleo_app_set_hint(GT("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_HELP:   nucleo_app_set_hint(GT("SX/DX pagine  Esc indietro", "LEFT/RIGHT pages  Esc back")); break;
        case ST_SET:    nucleo_app_set_hint(GT("SU/GIU  INVIO cambia  Esc menu", "UP/DN  ENTER change  Esc menu")); break;
        case ST_PLAY:   nucleo_app_set_hint(GT("SX/DX barra  SU/GIU  SPAZIO scram", "L/R rod  UP/DN  SPACE scram")); break;
        case ST_PAUSE:  nucleo_app_set_hint(GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave")); break;
        case ST_OVER:   nucleo_app_set_hint(s_qualify ? GT("INVIO scrivi il tuo nome", "ENTER enter your name")
                                                      : GT("INVIO rigioca  Esc menu", "ENTER replay  Esc menu")); break;
        case ST_SCORES: nucleo_app_set_hint(GT("INVIO/Esc menu", "ENTER/Esc menu")); break;
        case ST_NAME:   nucleo_app_set_hint(GT("Scrivi  INVIO ok  CANC cancella", "Type  ENTER ok  DEL erase")); break;
        default: break;
    }
}
static void go(int s)
{
    if (s == ST_MENU || s == ST_SET) { s_menu.sel = 0; s_menu.pos = 0; }
    s_screen = s; s_lsel = 0; set_hint(); nucleo_app_request_draw();
}

static void menu_select(void)
{
    sfx(2);
    if (s_menu.sel == 0) new_game();
    else if (s_menu.sel == 1) { s_newrank = -1; go(ST_SCORES); }
    else go(ST_SET);
}
static void name_commit(void)
{
    if (!s_name[0]) snprintf(s_name, sizeof s_name, "%s", GT("PILOTA", "PILOT"));
    s_newrank = score_insert(s_name, s_score, g_diff);
    s_qualify = false;
    scores_save();
    sfx(2);
    go(ST_SCORES);
}
static void settings_change(int key)
{
    if (s_menu.sel == 0) g_audio ^= 1;
    else if (s_menu.sel == 1) g_diff = (g_diff + (key == NK_LEFT ? 2 : 1)) % 3;
    else { if (key == NK_ENTER) { sfx(2); s_help = 0; go(ST_HELP); } return; }
    cfg_write(); sfx(2); nucleo_app_request_draw();
}
// The result card: a top-10 run always goes to the name entry first (ENTER or Esc), else ENTER replays and
// Esc returns to the menu. Keys are ignored while the death animation is still in its first 700 ms.
static void over_key(bool enter)
{
    if (s_over_total - s_over_ms < 700) return;
    if (s_qualify) { go(ST_NAME); return; }
    if (enter) new_game(); else { sfx(3); go(ST_MENU); }
}
static bool lg_key(int k, char ch);
static void on_key(int k, char ch)
{
    if (lg_key(k, ch)) return;
    switch (s_screen) {
        case ST_MENU:
            if (gui::menu_key(s_menu, k, NMENU)) { sfx(1); nucleo_app_request_draw(); }
            else if (ch >= '1' && ch < '1' + NMENU) { s_menu.sel = (int8_t)(ch - '1'); menu_select(); }   // 1-3 quick pick
            else if (k == NK_ENTER || k == NK_RIGHT) menu_select();
            return;
        case ST_HELP:
            if (k == NK_RIGHT || k == NK_ENTER) { s_help = (s_help + 1) % 3; sfx(1); nucleo_app_request_draw(); }
            return;
        case ST_SET:
            if (gui::menu_key(s_menu, k, NSET)) { sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change(k);
            return;
        case ST_PLAY:
            if (k == NK_RIGHT)     { if (!pad_side('/')) { s_cur = (s_cur + 1) % NROD; sfx(1); } }
            else if (ch >= '1' && ch <= '0' + NROD) { s_cur = ch - '1'; sfx(1); }      // 1-6 jump straight to a rod
            else if (k == NK_UP || ch == 'w' || ch == 'W') rod_adjust(+1);
            else if (k == NK_DOWN || ch == 'e' || ch == 'E') rod_adjust(-1);
            else if (ch == ' ' || ch == 'x' || ch == 'X') scram();
            nucleo_app_request_draw();
            return;
        case ST_PAUSE:
            if (k == NK_ENTER) { s_last = esp_timer_get_time() / 1000; sfx(2); go(ST_PLAY); }
            return;
        case ST_OVER:
            if (k == NK_ENTER || ch == ' ') over_key(true);
            return;
        case ST_SCORES:
            if (k == NK_ENTER) { sfx(3); go(ST_MENU); }
            return;
        case ST_NAME:
            if (k == NK_ENTER) name_commit();
            else if (k == NK_DEL) { int l = (int)strlen(s_name); if (l > 0) s_name[l - 1] = 0; sfx(3); nucleo_app_request_draw(); }
            else if (k == NK_CHAR && ch >= 32 && ch < 127) { int l = (int)strlen(s_name); if (l < 11) { s_name[l] = ch; s_name[l + 1] = 0; sfx(1); } nucleo_app_request_draw(); }
            return;
        default: return;
    }
}
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_HELP: s_help = (s_help + 2) % 3; sfx(1); nucleo_app_request_draw(); break;
            case ST_SET:  if (s_menu.sel < 2) settings_change(key); break;
            case ST_PLAY: if (!pad_side(',')) { s_cur = (s_cur + NROD - 1) % NROD; sfx(1); nucleo_app_request_draw(); } break;
            case ST_NAME: { int l = (int)strlen(s_name); if (l > 0) s_name[l - 1] = 0; sfx(3); nucleo_app_request_draw(); } break;
            default: break;
        }
        return true;
    }
    switch (s_screen) {
        case ST_MENU:  return false;
        case ST_NAME:  name_commit(); return true;
        case ST_PLAY:  sfx(3); go(ST_PAUSE); return true;          // the first Esc pauses...
        case ST_OVER:  over_key(false); return true;
        case ST_HELP:  sfx(3); go(ST_SET); s_menu.sel = 2; s_menu.pos = 2; return true;
        default:       sfx(3); go(ST_MENU); return true;           // ...a second one (in the pause) leaves
    }
}

// ============================ draw / poll / lifecycle ========================
static void draw_game(void)
{
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        case ST_PAUSE: {
            draw_play();
            char l1[40]; int sec = s_time_ms / 1000;
            snprintf(l1, sizeof l1, GT("%d:%02d  Punti %d", "%d:%02d  Score %d"), sec / 60, sec % 60, s_score);
            gui::dialog(GT("PAUSA", "PAUSED"), l1, diff_name(g_diff), GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave"), ACC);
            break;
        }
        case ST_OVER: draw_over(); break;
        case ST_SCORES: draw_scores(); break;
        case ST_NAME: draw_name(); break;
        default: break;
    }
}

/// ===== two screens: the control room NATIVE on the 320x240 panel, the Cardputer as a companion ==========
// Every screen is laid out for 320x240 (lg::frame) and touched directly: tap a row to pick it, tap it again to
// open it; the chevron is Back. In play the rod bank is six sliders: tap a rod at a height to set it there, drag
// up / down to pull it out / push it in; tap the core gauge to SCRAM. The game-over spectacle runs full panel
// (the same renderer at 320x240) and a tap goes on. The companion shows the live numbers and the key map.
#define LW 320
#define LH 240
#define LRY 150                                 // the rod bank: slots LRY..LRY+LRH, 40 px wide, 52 px apart
#define LRH 82
#define LRX 10
static void lt(int x, int y, int f, uint16_t c, const char *s, uint8_t dt = top_left)
{
    lg::font(f); d.setTextDatum(dt); d.setTextColor(c); d.drawString(s, x, y); d.setTextDatum(top_left);
}
static void lchev(int x, int y, uint16_t c)
{
    for (int k = 0; k < 2; k++) { d.drawLine(x + 6 + k, y, x + k, y + 7, c); d.drawLine(x + k, y + 7, x + 6 + k, y + 14, c); }
}
static void lhead(const char *title, const char *right)   // the header band: the chevron (tap = Back), title, value
{
    gui::vgradient(0, 0, LW, 30, rgb(73, 36, 0), COL_BG);
    d.drawFastHLine(0, 29, LW, mix(ACC, COL_BG, 120));
    lchev(10, 8, ACC); nucleo_touch_add(0, 0, 140, 30, NK_BACK, 0);
    lt(28, 7, 1, ACC, title);
    if (right) lt(LW - 10, 8, 0, COL_GREY, right, top_right);
}
// n rows from y, rh apart, the picked one lit; row i taps as NUCLEO_TOUCH_ROW + i. `val` = a right-hand value.
static void lrows(int y, int rh, const char *const *lab, const char *const *val, int n, int sel, int x, int w)
{
    for (int i = 0; i < n; i++) {
        const int ry = y + i * rh, h = rh - 6; const bool s = i == sel;
        d.fillRoundRect(x, ry, w, h, 8, s ? rgb(73, 36, 0) : rgb(0, 0, 36));
        d.drawRoundRect(x, ry, w, h, 8, s ? ACC : COL_TRACK);
        if (val && val[i]) { lt(x + 16, ry + h / 2 + 1, 1, s ? COL_WHITE : COL_GREY, lab[i], middle_left); lt(x + w - 16, ry + h / 2 + 1, 1, ACC, val[i], middle_right); }
        else lt(x + w / 2, ry + h / 2 + 1, 1, s ? COL_WHITE : COL_GREY, lab[i], middle_center);
        nucleo_touch_add(x, ry, w, h, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
}
static void lp_menu(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    lt(LW / 2, 8, 3, ACC, GT("REATTORE", "REACTOR"), top_center);
    char b[24]; snprintf(b, sizeof b, GT("Record %d", "Best %d"), g_best);
    lt(LW / 2, 38, 0, COL_GREY, b, top_center);
    draw_core(LW / 2, 86, 14, heat_col(40 + (int)(20 * sinf(s_anim * 0.05f))));
    const char *items[NMENU] = { GT("Gioca", "Play"), GT("Classifica", "Leaderboard"), GT("Impostazioni", "Settings") };
    lrows(126, 38, items, nullptr, NMENU, s_menu.sel, 50, LW - 100);
}
static void lp_set(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    lhead(GT("Impostazioni", "Settings"), nullptr);
    const char *lab[NSET] = { GT("Audio", "Audio"), GT("Difficolta", "Difficulty"), GT("Come si gioca", "How to play") };
    const char *val[NSET] = { g_audio ? GT("Si", "On") : GT("No", "Off"), diff_name(g_diff), nullptr };
    lrows(44, 50, lab, val, NSET, s_menu.sel, 16, LW - 32);
}
static void lp_help(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    lhead(help_title(s_help), nullptr);
    for (int i = 0; i < 3; i++) d.fillCircle(LW - 34 + i * 12, 15, i == s_help ? 4 : 3, i == s_help ? ACC : COL_DIM);   // page dots
    const char *l[5]; uint16_t c[5];
    help_lines(s_help, l, c);
    for (int i = 0; i < 5; i++) { char b[48]; lg::font(0); lg::fit(b, sizeof b, l[i], LW - 28); lt(14, 46 + i * 36, 0, c[i], b); }
    lg::field(0, 30, LW, LH - 30);
}
static void lp_scores(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    lhead(GT("Classifica", "Leaderboard"), nullptr);
    char b[16];
    for (int i = 0; i < NSCORES; i++) {
        const int y = 33 + i * 20; const bool hot = i == s_newrank;
        if (hot) d.fillRoundRect(6, y, LW - 12, 19, 5, rgb(73, 36, 0));
        const uint16_t c = hot ? COL_WHITE : (i < 3 ? COL_AMBER : COL_GREY);
        snprintf(b, sizeof b, "%d", i + 1); lt(34, y + 2, 0, c, b, top_right);
        if (!s_scores[i].score) { lt(60, y + 2, 0, COL_DIM, "-"); continue; }
        d.fillCircle(48, y + 10, 3, s_scores[i].diff == 0 ? COL_GREEN : s_scores[i].diff == 2 ? COL_RED : COL_AMBER);
        lt(60, y + 2, 0, c, s_scores[i].name);
        snprintf(b, sizeof b, "%d", s_scores[i].score); lt(LW - 16, y + 2, 0, c, b, top_right);
    }
}
static void lp_name(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    char b[24]; snprintf(b, sizeof b, GT("Punteggio %d", "Score %d"), s_score);
    lhead(GT("Nuovo record!", "New record!"), b);
    lt(LW / 2, 50, 0, COL_GREY, GT("Scrivi il nome sulla tastiera", "Type the name on the keyboard"), top_center);
    gui::panel(30, 76, LW - 60, 44, rgb(0, 0, 85), COL_CYAN);
    lt(44, 98, 2, COL_WHITE, s_name, middle_left);
    lg::font(2); const int cx = 46 + d.textWidth(s_name);
    if ((s_anim >> 3) & 1) d.fillRect(cx, 86, 10, 24, COL_CYAN);
    const char *ok[1] = { "OK" };
    lrows(150, 52, ok, nullptr, 1, 0, 80, LW - 160);
}
// The rod a panel x falls on (the slots and the gaps right of them).
static int lrod_at(int x) { int i = (x - LRX + 6) / 52; return i < 0 ? 0 : i >= NROD ? NROD - 1 : i; }
static void lp_play(void)
{
    d.fillRect(0, 0, LW, LH, COL_BG);
    char b[24];
    const int sec = s_time_ms / 1000, hpct = (int)s_dheat;
    int dz = s_dheat < 82 ? 0 : (int)((s_dheat - 82) * 100 / 18); if (dz > 100) dz = 100;
    gui::vgradient(0, 0, LW, 30, rgb(0, 0, 36), COL_BG);                            // the top band: pause, time, score
    lchev(10, 8, ACC);
    snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60); lt(26, 7, 1, COL_GREY, b);
    snprintf(b, sizeof b, "%d", s_score); lt(LW - 10, 5, 2, s_pop_ms > 300 ? COL_WHITE : COL_AMBER, b, top_right);
    if (s_dheat >= 90 && ((s_anim >> 1) & 1)) {                                       // PANIC banner
        d.fillRoundRect(LW / 2 - 60, 3, 120, 24, 5, COL_RED);
        lt(LW / 2, 15, 1, COL_WHITE, GT("! CRITICO !", "! CRITICAL !"), middle_center);
    }
    const bool met = s_power >= s_demand;                                              // POWER gauge, demand needle
    const int sh = (int)(40 * (0.5f + 0.5f * sinf(s_anim * 0.18f)));
    const uint16_t pcol = met ? mix(COL_GREEN, COL_WHITE, sh) : COL_AMBER;
    ring(62, 88, 46, (int)(s_dpow * 100 / POWER_MAX), pcol, false);
    radial_tick(62, 88, 33, 49, s_demand * 100 / POWER_MAX, COL_WHITE);
    snprintf(b, sizeof b, "%d", (int)(s_dpow + 0.5f)); lt(62, 86, 3, COL_WHITE, b, middle_center);
    lt(62, 104, 4, COL_GREY, "MW", top_center);
    ring(258, 88, 46, hpct, (dz >= 45 && (s_anim & 1)) ? COL_WHITE : heat_col(hpct), true);   // CORE heat = SCRAM
    snprintf(b, sizeof b, "%d%%", hpct); lt(258, 86, 3, dz > 0 ? rgb(255, 109 + (100 - dz), 85) : heat_col(hpct), b, middle_center);
    lt(258, 104, 4, COL_RED, "SCRAM", top_center);
    lt(LW / 2, 38, 4, COL_GREY, GT("DOMANDA", "DEMAND"), top_center);                  // centre: demand + grid
    snprintf(b, sizeof b, "%d", s_demand); lt(LW / 2, 50, 2, s_dem_ms > 0 ? COL_WHITE : COL_AMBER, b, top_center);
    if (s_dem_ms > 0) {
        const uint16_t tc = s_dem_dip ? COL_CYAN : COL_AMBER; const int ax = LW / 2 + 30, ay = 60;
        if (s_dem_dip) d.fillTriangle(ax - 5, ay - 4, ax + 5, ay - 4, ax, ay + 4, tc);
        else           d.fillTriangle(ax - 5, ay + 4, ax + 5, ay + 4, ax, ay - 4, tc);
    }
    lt(LW / 2, 88, 4, COL_GREY, GT("RETE", "GRID"), top_center);
    d.fillRoundRect(LW / 2 - 32, 100, 64, 10, 3, COL_TRACK);
    const int sw = (int)s_dstab * 60 / 100;
    if (sw > 0) d.fillRoundRect(LW / 2 - 30, 102, sw, 6, 2, s_stab > 35 ? COL_CYAN : COL_RED);
    d.fillRoundRect(3, LRY - 8, LW - 6, LRH + 14, 6, rgb(0, 0, 85));                   // the rod bank: six sliders
    d.drawRoundRect(3, LRY - 8, LW - 6, LRH + 14, 6, rgb(36, 73, 170));
    const int usable = LRH - 8;
    for (int i = 0; i < NROD; i++) {
        const int x = LRX + i * 52, hy = LRY + 4 + s_rod[i] * usable / ROD_MAX; const bool sel = i == s_cur;
        d.fillRoundRect(x, LRY, 40, LRH, 5, COL_BG);
        const uint16_t fuel = sel ? rgb(255, 219, 170) : heat_col(hpct);
        if (hy - LRY - 4 > 0) d.fillRoundRect(x + 4, LRY + 4, 32, hy - LRY - 4, 3, fuel);
        d.fillRect(x + 4, hy, 32, LRY + LRH - 4 - hy, rgb(73, 73, 85));
        d.fillRoundRect(x + 2, hy - 2, 36, 5, 2, sel ? COL_WHITE : rgb(182, 182, 170)); // the handle
        d.drawRoundRect(x, LRY, 40, LRH, 5, sel ? COL_AMBER : rgb(36, 73, 170));
    }
    if (s_scram_ms > 180) d.fillRect(0, 0, LW, LH, mix(rgb(182, 219, 255), COL_BG, (220 - s_scram_ms) * 256 / 40));   // SCRAM flash
    if (dz > 0) {                                                                       // the danger vignette
        const int amp = (int)(dz * (0.55f + 0.45f * sinf(s_anim * 0.4f)));
        for (int k = 0; k < 5; k++) d.drawRect(k, k, LW - 2 * k, LH - 2 * k, rgb(36 + amp * (5 - k) / 4, 0, 0));
    }
    if (s_screen == ST_PAUSE) {
        gui::panel(40, 40, LW - 80, 170, COL_BG, ACC);
        lt(LW / 2, 50, 2, ACC, GT("PAUSA", "PAUSED"), top_center);
        snprintf(b, sizeof b, "%d:%02d  %d  %s", sec / 60, sec % 60, s_score, diff_name(g_diff));
        lt(LW / 2, 78, 0, COL_GREY, b, top_center);
        const char *rows[2] = { GT("Riprendi", "Resume"), GT("Menu", "Menu") };
        lrows(104, 50, rows, nullptr, 2, s_lsel, 60, LW - 120);
        return;
    }
    nucleo_touch_add(0, 0, 120, 30, NK_BACK, 0);
    lg::field(206, 36, 110, 104);                                                      // the core gauge: SCRAM
    lg::field(0, LRY - 8, LW, LH - LRY + 8);                                           // the rods: tap = set
    nucleo_touch_add_pad(0, LRY - 8, LW, LH - LRY + 8, 16);                            // drag = pull / push
}
static void lp_over(void)
{
    d.fillRect(0, LH - 36, LW, 36, COL_BG);
    s_vw = LW; s_vh = LH - 36;                                                         // the spectacle above the hint
    draw_over();
    s_vw = W; s_vh = H - HINT;
    lchev(10, 8, COL_GREY); nucleo_touch_add(0, 0, 60, 40, NK_BACK, 0);
    if (s_over_total - s_over_ms < 700) return;
    const char *t = s_qualify ? GT("Salva record", "Save score") : GT("Rigioca", "Replay");
    lg::font(1); const int w = d.textWidth(t) + 40;
    d.fillRoundRect(LW / 2 - w / 2, LH - 33, w, 30, 15, ACC);
    lt(LW / 2, LH - 17, 1, COL_BG, t, middle_center);
    lg::field(0, 40, LW, LH - 40);
}
static void lg_paint(void *)
{
    s_flat = true;
    switch (s_screen) {
        case ST_MENU:   lp_menu(); break;
        case ST_HELP:   lp_help(); break;
        case ST_SET:    lp_set(); break;
        case ST_PLAY: case ST_PAUSE: lp_play(); break;
        case ST_OVER:   lp_over(); break;
        case ST_SCORES: lp_scores(); break;
        case ST_NAME:   lp_name(); break;
        default: break;
    }
    s_flat = false;
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}
// A touch on the panel (a row or a field tap): true when handled.
static bool lg_key(int k, char ch)
{
    int tx = 0, ty = 0;
    const bool fld = lg::tap(k, ch, &tx, &ty);
    const int ri = nucleo_touch_is_row(k, ch) ? ch - NUCLEO_TOUCH_ROW : -1;
    if (!fld && ri < 0) return false;
    switch (s_screen) {
        case ST_MENU: case ST_SET: {
            const int n = s_screen == ST_MENU ? NMENU : NSET;
            if (ri < 0 || ri >= n) return true;
            if (s_menu.sel != ri) { s_menu.sel = (int8_t)ri; sfx(1); nucleo_app_request_draw(); }
            else if (s_screen == ST_MENU) menu_select(); else settings_change(NK_ENTER);
            return true;
        }
        case ST_HELP:  if (fld) on_key(NK_RIGHT, 0); return true;
        case ST_PAUSE:
            if (ri < 0 || ri > 1) return true;
            if (s_lsel != ri) { s_lsel = ri; sfx(1); nucleo_app_request_draw(); }
            else if (ri == 0) on_key(NK_ENTER, 0); else on_back(NK_BACK);
            return true;
        case ST_PLAY:
            if (!fld) return true;
            if (ty < LRY - 8) { if (tx >= 206) scram(); }
            else {                                                                      // a rod set at the tapped height
                s_cur = lrod_at(tx);
                const int v = ((ty - LRY - 4) * ROD_MAX + (LRH - 8) / 2) / (LRH - 8);
                rod_adjust((int)s_rod[s_cur] - (v < 0 ? 0 : v > ROD_MAX ? ROD_MAX : v));
            }
            nucleo_app_request_draw();
            return true;
        case ST_OVER:  if (fld) over_key(true); return true;
        case ST_NAME:  if (ri == 0) name_commit(); return true;
        default:       return true;
    }
}
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), hc = nucleo_app_content_height();
    d.fillRect(0, top, W, hc, COL_BG);
    int y = lg::comp_head("REATTORE", ACC);
    char b[40];
    if (s_screen == ST_PLAY || s_screen == ST_PAUSE) {
        int sec = s_time_ms / 1000, pwr = (int)(s_dpow + 0.5f), hpct = (int)s_dheat;
        snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60); gui::text(b, 8, y + 2, 0, gui::F_SMALL, COL_GREY, 0);
        snprintf(b, sizeof b, "%d", s_score); gui::text(b, W - 8, y + 2, 2, gui::F_SMALL, COL_AMBER, 0);
        snprintf(b, sizeof b, GT("Potenza %d  Domanda %d", "Power %d  Demand %d"), pwr, s_demand);
        gui::text(b, 8, y + 18, 0, gui::F_SMALL, s_power >= s_demand ? COL_GREEN : COL_AMBER, 0);
        snprintf(b, sizeof b, GT("Calore %d%%  Rete %d%%", "Heat %d%%  Grid %d%%"), hpct, (int)s_dstab);
        gui::text(b, 8, y + 34, 0, gui::F_SMALL, heat_col(hpct), 0);
        snprintf(b, sizeof b, GT("Barra %d di %d", "Rod %d of %d"), s_cur + 1, NROD);
        gui::text(b, 8, y + 52, 0, gui::F_BODY, COL_WHITE, 0);
        lg::comp_keys(s_screen == ST_PAUSE ? GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave")
                                           : GT("SX/DX barra  SU/GIU  SPAZIO scram", "L/R rod  UP/DN  SPACE scram"));
        return;
    }
    const char *st = s_screen == ST_MENU ? GT("Menu", "Menu")
                   : s_screen == ST_SET  ? GT("Impostazioni", "Settings")
                   : s_screen == ST_HELP ? GT("Come si gioca", "How to play")
                   : s_screen == ST_SCORES ? GT("Classifica", "Leaderboard")
                   : s_screen == ST_NAME ? GT("Nuovo record", "New record")
                   : s_over_reason == 0 ? GT("Fusione", "Meltdown") : "Blackout";
    gui::text(st, 8, y + 6, 0, gui::F_BODY, 0xFFFF, 0);
    snprintf(b, sizeof b, GT("Record %d", "Best %d"), g_best); gui::text(b, 8, y + 34, 0, gui::F_SMALL, COL_AMBER, 0);
    lg::comp_keys(s_screen == ST_NAME ? GT("Scrivi il nome  INVIO ok", "Type the name  ENTER ok")
                                      : GT("Tocca le righe sul pannello", "Tap the rows on the panel"));
}
static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    draw_game();
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last);
    if (dt < 0) dt = 0;
    if (dt > 250) dt = 250;
    s_last = s_now;
    bool animated = false;

    if (s_screen == ST_PLAY) {
        animated = true;
        s_acc += dt;
        while (s_acc >= STEP_MS) { s_acc -= STEP_MS; sim_step(); if (s_screen != ST_PLAY) break; }
        float f;
        f = (s_power - s_dpow) * 0.55f;  s_vpow  = (s_vpow + f) * 0.65f;  s_dpow  += s_vpow;
        f = (s_heat - s_dheat) * 0.55f;  s_vheat = (s_vheat + f) * 0.65f; s_dheat += s_vheat;
        f = (s_stab - s_dstab) * 0.55f;  s_vstab = (s_vstab + f) * 0.65f; s_dstab += s_vstab;
        s_dpow = clampf(s_dpow, 0, POWER_MAX);
        s_dheat = clampf(s_dheat, 0, 110);
        s_dstab = clampf(s_dstab, 0, 100);
        dec(&s_rodfx_ms, dt); dec(&s_scram_ms, dt); dec(&s_dem_ms, dt); dec(&s_pop_ms, dt);
    } else if (s_screen == ST_OVER) {
        int e0 = s_over_total - s_over_ms;
        dec(&s_over_ms, dt);
        int e1 = s_over_total - s_over_ms;
        if (s_over_reason == 0 && e0 < 620 && e1 >= 620) sfx(8);          // aftershock rumble
        if (s_over_ms == 0 && s_newbest && !s_bestplayed) { s_bestplayed = true; sfx(2); }
        animated = e0 < s_over_total + 200;                               // the spectacle, then a still card
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        animated = gui::menu_tick(s_menu, dt) || s_screen == ST_MENU;     // the title core pulses
    } else if (s_screen == ST_NAME) animated = true;                      // the blinking caret

    // A still screen (help, scores, pause, a settled list) is not redrawn: the run loop pushes nothing.
    if (!animated) return false;
    if (s_now - s_frame < 33) return false;
    s_frame = s_now;
    s_anim++;
    return true;
}
static void on_enter(void)
{
    game_text_open("reactor");
    ensure_dirs();
    cfg_read();
    scores_load();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);   // never start the game inaudibly low
    s_screen = ST_MENU; s_anim = 0; s_menu.sel = 0; s_menu.pos = 0; s_newrank = -1;
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint();
    sfx(9);                                                        // opening sting — also confirms audio is alive
    nucleo_app_request_draw();
}
static void on_exit(void) { nucleo_audio_stop(); game_text_close(); }

static const nucleo_app_ram_t APP_RAM[] = { { (void **)&s_scores, sizeof(Score) * NSCORES }, { nullptr, 0 } };

extern "C" void nucleo_register_reactor(void)
{
    static const nucleo_app_def_t app = {
        "reactor", "Reattore", "Games", "Tieni la potenza, evita la fusione",
        'R', C_RED, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP,  // dedicate RAM + free the shared I2S/mic line so the chiptune SFX reliably play
        APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
#endif  // !CONFIG_NUCLEO_STORE_REACTOR || NAP_MODULE
