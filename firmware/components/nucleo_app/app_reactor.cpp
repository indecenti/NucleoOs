// app_reactor.cpp — NucleoOS "Reattore": a reactor-control arcade game (category "Games").
//
// Front-end (MENU / HELP / SETTINGS / PLAY / GAME-OVER), bilingual IT/EN persisted to SD, and a
// heavily juiced smartwatch-OS presentation: spring-driven ring gauges, directional rod sparks,
// a heat-scaled danger telegraph (visual + accelerating heartbeat), a SCRAM cool-flash, score
// popups, demand-rise warnings, death-specific game-over animations, ambient embers and an eased
// selection capsule. All feedback doubles as readability.
//
// Constraints honoured: exclusive_flags = 0; every bit of state is static (NO heap); drawing goes
// through the buffered `d.` path (the run loop blits the shared canvas once — ANTI-FLICKER tech 1);
// 8bpp has no alpha, so every fade/glow is an integer channel-mix toward another solid colour. A
// ~30 Hz poll handler animates and steps the sim at a fixed 10 Hz timestep. LEFT/BACK reach
// on_back(), all other keys reach on_key(). Text is ASCII only. Never name a local `d` (display macro).

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
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
// integer channel mix in RGB565 space: t in 0..256 (0 = a, 256 = b)
static uint16_t mix(uint16_t a, uint16_t b, int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar + (br - ar) * t / 256) & 31) << 11) | (((ag + (bg - ag) * t / 256) & 63) << 5) | ((ab + (bb - ab) * t / 256) & 31));
}
#define COL_BG     rgb(10, 12, 20)
#define COL_TRACK  rgb(30, 34, 50)
#define COL_WHITE  0xFFFF
#define COL_CYAN   rgb(96, 206, 232)
#define COL_AMBER  rgb(255, 190, 64)
#define COL_GREEN  rgb(118, 230, 140)
#define COL_RED    rgb(244, 92, 78)
#define COL_GREY   rgb(150, 160, 184)
#define COL_DIM    rgb(78, 88, 116)
#define COL_CAP    rgb(26, 40, 78)

// ============================ model ==========================================
#define NROD      6
#define ROD_MAX   5
#define POWER_PER_R 10
#define POWER_MAX (NROD * ROD_MAX * POWER_PER_R)   // 300
#define STEP_MS   100

enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OVER, ST_SCORES, ST_NAME };

static int   g_lang = 0, g_audio = 1, g_diff = 1, g_best = 0;

static int   s_screen;
static int64_t s_now, s_last, s_frame;
static int   s_acc;
static unsigned s_anim;

static int   s_msel, s_setsel, s_help;
static float s_capY;                       // eased selection-capsule y

static uint8_t s_rod[NROD];
static int   s_cur;
static float s_heat, s_stab;
static int   s_power, s_demand;
static int   s_score, s_time_ms, s_over_reason;
static float s_dpow, s_dheat, s_dstab;     // sprung display values
static float s_vpow, s_vheat, s_vstab;     // spring velocities

// juice timers / one-shots (all static, ms)
static int   s_rodfx_ms, s_rodfx_dir;
static int   s_scram_ms, s_dem_ms, s_pop_ms, s_popN, s_lastscore, s_milestone;
static int   s_alarm_ms, s_over_ms, s_over_total;
static bool  s_newbest, s_bestplayed, s_qualify;

#define NSCORES 10
struct Score { char name[12]; int score; int diff; };
static Score s_scores[NSCORES];      // leaderboard, sorted descending
static char  s_name[12];             // current / last-entered pilot name
static int   s_newrank;              // row to highlight on the board (-1 = none)

static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ============================ persistence (cfg) ==============================
#define DIRR "/sd/data/reattore"
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); mkdir(DIRR "/custom", 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, df, b; } c = { 0x52454143u, g_lang, g_audio, g_diff, g_best };
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
    if (n == 1 && c.m == 0x52454143u) { g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_diff = (c.df < 0 || c.df > 2) ? 1 : c.df; g_best = c.b; }
}

// ---- leaderboard (top 10) ---------------------------------------------------
#define SCORES_MAGIC 0x32435352u   // 'RSC2'
struct ScoresFile { uint32_t m; uint16_t v, p; char last[12]; Score e[NSCORES]; };
static void scores_load(void)
{
    for (int i = 0; i < NSCORES; i++) { snprintf(s_scores[i].name, sizeof s_scores[i].name, "%s", "---"); s_scores[i].score = 0; s_scores[i].diff = 1; }
    snprintf(s_name, sizeof s_name, "%s", "PILOTA");
    FILE *f = fopen(DIRR "/scores.bin", "rb");
    if (!f) return;
    ScoresFile sf;
    size_t n = fread(&sf, sizeof sf, 1, f);
    fclose(f);
    if (n == 1 && sf.m == SCORES_MAGIC) {
        sf.last[11] = 0;
        snprintf(s_name, sizeof s_name, "%s", sf.last[0] ? sf.last : "PILOTA");
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
    FILE *f = fopen(DIRR "/scores.tmp", "wb");
    if (!f) return;
    size_t n = fwrite(&sf, sizeof sf, 1, f);
    fclose(f);
    if (n != 1) { remove(DIRR "/scores.tmp"); return; }
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
    snprintf(s_scores[rank].name, sizeof s_scores[rank].name, "%s", (name && name[0]) ? name : "PILOTA");
    s_scores[rank].score = sc; s_scores[rank].diff = diff;
    return rank;
}

// ============================ audio ==========================================
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "move"; case 2: return "ok";   case 3: return "back";  case 4: return "out";
        case 5: return "in";   case 6: return "scram";case 7: return "alarm"; case 8: return "boom";
        case 9: return "start";case 10:return "heart";case 11:return "rise"; case 12:return "klaxon"; default: return "x";
    }
}
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1:  notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.6f; return 1;
        case 2:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.09f); return 2;
        case 3:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 440, 0.05f, 0.09f); return 2;
        case 4:  notify__voice(&v[0], 880, 0, 0.05f); notify__voice(&v[1], 1174.7f, 0.03f, 0.06f); return 2;
        case 5:  notify__voice(&v[0], 587.33f, 0, 0.05f); notify__voice(&v[1], 440, 0.03f, 0.06f); return 2;
        case 6:  notify__voice(&v[0], 392, 0, 0.18f); notify__voice(&v[1], 261.63f, 0.10f, 0.22f); return 2;
        case 7:  notify__voice(&v[0], 988, 0, 0.10f); notify__voice(&v[1], 988, 0.14f, 0.10f); return 2;
        case 8:  notify__voice(&v[0], 150, 0, 0.78f); v[0].amp = 1.0f; notify__voice(&v[1], 82, 0.02f, 0.9f); v[1].amp = 1.0f;
                 notify__voice(&v[2], 240, 0, 0.30f); notify__voice(&v[3], 64, 0.32f, 0.6f); v[3].amp = 1.0f; return 4;   // big layered detonation
        case 9:  notify__voice(&v[0], 523.25f, 0, 0.14f); notify__voice(&v[1], 659.25f, 0.12f, 0.14f); notify__voice(&v[2], 783.99f, 0.24f, 0.30f); return 3;
        case 10: notify__voice(&v[0], 110, 0, 0.07f); v[0].amp = 0.5f; return 1;                       // heartbeat thud
        case 11: notify__voice(&v[0], 698, 0, 0.05f); notify__voice(&v[1], 1046, 0.04f, 0.07f); return 2; // demand-rise blip
        case 12: notify__voice(&v[0], 988, 0, 0.09f); notify__voice(&v[1], 740, 0.08f, 0.13f); v[0].amp = 1.0f; v[1].amp = 1.0f; return 2; // panic klaxon (nee-naw)
    }
    return 0;
}
static bool sfx_important(int id) { return id == 6 || id == 8; }   // only SCRAM and meltdown interrupt
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    static char p[80];
    snprintf(p, sizeof p, DIRR "/custom/%s.wav", sfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else {
        snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        f = fopen(p, "rb");
        if (f) fclose(f);
        else { notify_voice_t v[6]; int nv = build_voices(id, v); if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return; }
    }
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
// bump whenever a build_voices() clip changes, so stale cached WAVs get rebuilt on next launch.
#define SFX_VER 2
static void sfx_cache_check(void)
{
    int ver = 0;
    FILE *f = fopen(DIRR "/sfx/ver.bin", "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver == SFX_VER) return;
    for (int id = 1; id <= 12; id++) { char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); remove(p); }
    f = fopen(DIRR "/sfx/ver.bin", "wb");
    if (f) { int vv = SFX_VER; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
}
// synthesize every clip to SD up-front (RAM is free under exclusive mode) so playback during the
// game never has to synth mid-frame, and any SD problem surfaces at open rather than as silence.
static void presynth(void)
{
    if (!g_audio) return;
    notify_voice_t v[6];
    for (int id = 1; id <= 12; id++) {
        char p[80];
        snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ draw helpers ===================================
// transparent text (single-arg colour) — avoids opaque glyph boxes punching holes in rings/bars
static void text_at(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void text_c(int cx, int y, int sz, uint16_t col, const char *s) { text_at(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
// centred text with a soft 1px drop shadow — adds depth to the big readouts
static void text_c_sh(int cx, int y, int sz, uint16_t col, const char *s) { text_c(cx + 1, y + 1, sz, mix(col, COL_BG, 205), s); text_c(cx, y, sz, col, s); }

static uint16_t heat_col(int pct)
{
    if (pct < 45) return COL_GREEN;
    if (pct < 70) return COL_AMBER;
    if (pct < 85) return rgb(255, 130, 50);
    return COL_RED;
}
static void ring(int cx, int cy, int r, int pct, uint16_t col, bool redzone)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d.fillArc(cx, cy, r - 9, r, 0, 360, COL_TRACK);
    if (redzone) d.fillArc(cx, cy, r - 9, r, 360 * 85 / 100, 360, rgb(96, 32, 30));
    if (pct > 0) d.fillArc(cx, cy, r - 9, r, 0, 360 * pct / 100, col);
    d.drawCircle(cx, cy, r, mix(col, COL_BG, 165));        // crisp coloured outer rim
    d.drawCircle(cx, cy, r - 9, rgb(22, 26, 40));          // inner rim
}
static void radial_tick(int cx, int cy, int r0, int r1, int pct, uint16_t col)
{
    float a = (float)pct * 3.6f * 0.01745329f;
    float ca = cosf(a), sa = sinf(a);
    d.drawLine(cx + (int)(ca * r0), cy + (int)(sa * r0), cx + (int)(ca * r1), cy + (int)(sa * r1), col);
}
static void hbar(int x, int y, int w, int h, int pct, uint16_t col)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d.fillRoundRect(x, y, w, h, 2, COL_TRACK);
    if (pct > 0) d.fillRoundRect(x, y, w * pct / 100, h, 2, col);
}
static void pager_dots(int cx, int y, int n, int sel)
{
    int gap = 12, x0 = cx - (n - 1) * gap / 2;
    for (int i = 0; i < n; i++) d.fillCircle(x0 + i * gap, y, i == sel ? 3 : 2, i == sel ? COL_CYAN : COL_DIM);
}
static void draw_core(int cx, int cy, int base, uint16_t col)
{
    int pulse = base + (int)(3 * sinf(s_anim * 0.12f));
    d.drawCircle(cx, cy, base + 14, mix(col, COL_BG, 180));
    for (int r = pulse + 9; r > pulse; r -= 2) d.drawCircle(cx, cy, r, mix(col, COL_BG, 140));
    d.fillCircle(cx, cy, pulse, col);
    d.fillCircle(cx - pulse / 3, cy - pulse / 3, pulse / 3, COL_WHITE);
    for (int e = 0; e < 4; e++) {                                   // orbiting embers
        float a = s_anim * 0.06f + e * 1.57f;
        int rr = base + 11 + (e & 1) * 4;
        d.fillCircle(cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr), 1, rgb(255, 180, 90));
    }
}

// ============================ simulation =====================================
static const int COOL[3]      = { 20, 16, 12 };
static const int DEM_START[3] = { 60, 70, 85 };
static const int DEM_STEP[3]  = { 16, 24, 32 };
static const int DEM_EVERY[3] = { 14, 10, 7 };

static int reactivity(void) { int r = 0; for (int i = 0; i < NROD; i++) r += ROD_MAX - s_rod[i]; return r; }

static void new_game(void)
{
    for (int i = 0; i < NROD; i++) s_rod[i] = 3;
    s_cur = 0; s_heat = 18; s_stab = 100; s_score = 0; s_time_ms = 0; s_acc = 0;
    s_demand = DEM_START[g_diff];
    s_power = reactivity() * POWER_PER_R;
    s_dpow = s_power; s_dheat = s_heat; s_dstab = s_stab;
    s_vpow = s_vheat = s_vstab = 0;
    s_rodfx_ms = s_scram_ms = s_dem_ms = s_pop_ms = s_popN = 0;
    s_lastscore = 0; s_milestone = 0; s_alarm_ms = 0;
    s_screen = ST_PLAY;
    sfx(9);
}
static void game_over(int reason)
{
    s_over_reason = reason; s_screen = ST_OVER;
    s_over_total = s_over_ms = (reason == 0) ? 1800 : 700;        // meltdown earns a long spectacle
    s_newbest = (s_score > g_best); s_bestplayed = false;
    s_qualify = score_qualifies(s_score);
    if (s_newbest) { g_best = s_score; cfg_write(); }
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
    if ((s_time_ms / 1000) > 0 && (s_time_ms % (DEM_EVERY[g_diff] * 1000)) < STEP_MS) { s_demand += DEM_STEP[g_diff]; s_dem_ms = 500; sfx(11); }

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
static void draw_menu(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    draw_core(W / 2, 14, 4, heat_col(40 + (int)(20 * sinf(s_anim * 0.05f))));
    int lw = (int)strlen("REATTORE") * 18;
    text_at((W - lw) / 2 + 1, 5, 3, rgb(70, 20, 20), "REATTORE");
    text_at((W - lw) / 2, 4, 3, COL_AMBER, "REATTORE");

    const char *items[4] = { tx("Gioca", "Play"), tx("Come si gioca", "How to play"), tx("Classifica", "Leaderboard"), tx("Impostazioni", "Settings") };
    int selw = (int)strlen(items[s_msel]) * 12;
    int capx = (W - selw) / 2 - 12;
    d.fillRoundRect(capx, (int)s_capY, selw + 24, 19, 6, COL_CAP);
    d.fillRoundRect(capx + 4, (int)s_capY + 4, 3, 11, 1, COL_AMBER);   // focus accent rail
    for (int i = 0; i < 4; i++) text_c(W / 2, 36 + i * 22, 2, i == s_msel ? COL_WHITE : COL_GREY, items[i]);
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    const char *titles[3] = { tx("Obiettivo", "Objective"), tx("Comandi", "Controls"), tx("Pericoli", "Hazards") };
    text_c(W / 2, 8, 2, COL_CYAN, titles[s_help]);
    d.drawFastHLine(20, 28, W - 40, COL_DIM);
    if (s_help == 0) {
        text_at(16, 40, 1, COL_WHITE, tx("Tieni la POTENZA pari alla", "Keep POWER equal to the"));
        text_at(16, 54, 1, COL_WHITE, tx("DOMANDA della rete...", "grid's DEMAND..."));
        text_at(16, 74, 1, COL_AMBER, tx("...senza fondere il nucleo.", "...without melting the core."));
        text_at(16, 92, 1, COL_GREY, tx("Resisti il piu' a lungo!", "Survive as long as you can!"));
    } else if (s_help == 1) {
        text_at(16, 40, 1, COL_GREY, tx("SX / DX", "LEFT / RIGHT"));
        text_at(120, 40, 1, COL_WHITE, tx("scegli barra", "pick rod"));
        text_at(16, 56, 1, COL_GREY, tx("SU / W", "UP / W"));
        text_at(120, 56, 1, COL_GREEN, tx("estrai: +potenza", "pull out: +power"));
        text_at(16, 72, 1, COL_GREY, tx("GIU / E", "DOWN / E"));
        text_at(120, 72, 1, COL_CYAN, tx("inserisci: raffredda", "push in: cools"));
        text_at(16, 88, 1, COL_GREY, "SPAZIO");
        text_at(120, 88, 1, COL_AMBER, "SCRAM");
    } else {
        text_at(16, 40, 1, COL_WHITE, tx("Il CALORE sale con la potenza.", "HEAT rises with power."));
        text_at(16, 56, 1, COL_RED, tx("Al 100% e' FUSIONE.", "At 100% it is MELTDOWN."));
        text_at(16, 76, 1, COL_WHITE, tx("Potenza sotto la domanda", "Power below demand for"));
        text_at(16, 90, 1, COL_RED, tx("troppo a lungo = BLACKOUT.", "too long = BLACKOUT."));
    }
    pager_dots(W / 2, ch - 10, 3, s_help);
}
static void draw_settings(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    text_c(W / 2, 8, 2, COL_CYAN, tx("Impostazioni", "Settings"));
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
        if (i < 3) text_at(W - 20 - (int)strlen(v[i]) * 6, y + 4, 1, COL_AMBER, v[i]);   // value at size 1 → never collides with the size-2 name
    }
}
static void draw_play(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    char b[24];

    // top strip: time + score (with brighten-pop) + floating +N
    int sec = s_time_ms / 1000;
    snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    text_at(6, 2, 1, COL_GREY, b);
    snprintf(b, sizeof b, "%d", s_score);
    text_at(W - 6 - (int)strlen(b) * 6, 2, 1, s_pop_ms > 300 ? COL_WHITE : COL_AMBER, b);
    if (s_pop_ms > 0) {
        char pb[12]; snprintf(pb, sizeof pb, "+%d", s_popN);
        int rise = (420 - s_pop_ms) / 42;
        text_at(W - 6 - (int)strlen(pb) * 6, 12 - rise, 1, mix(COL_GREEN, COL_BG, (420 - s_pop_ms) * 256 / 420), pb);
    }

    bool met = s_power >= s_demand;
    int hpct = (int)s_dheat;
    int dz = s_dheat < 82 ? 0 : (int)((s_dheat - 82) * 100 / 18);
    if (dz > 100) dz = 100;

    // POWER ring (left) — calm white shimmer when locked on
    int px = 56, py = 46, pr = 30;
    int ppct = (int)(s_dpow * 100 / POWER_MAX);
    int sh = (int)(40 * (0.5f + 0.5f * sinf(s_anim * 0.18f)));
    uint16_t pcol = met ? mix(COL_GREEN, COL_WHITE, sh) : COL_AMBER;
    ring(px, py, pr, ppct, pcol, false);
    radial_tick(px, py, pr - 11, pr + 2 + (met ? sh / 20 : 0), s_demand * 100 / POWER_MAX, COL_WHITE);
    snprintf(b, sizeof b, "%d", (int)(s_dpow + 0.5f));
    text_c_sh(px, py - 15, 3, COL_WHITE, b);
    text_c(px, py + 10, 1, COL_GREY, "MW");

    // CORE heat ring (right) — heat-tinted big number, glow when critical
    int hx = 184, hy = 46, hr = 30;
    ring(hx, hy, hr, hpct, (dz >= 45 && (s_anim & 1)) ? COL_WHITE : heat_col(hpct), true);   // strobes in panic
    if (dz > 50) d.drawCircle(hx, hy, hr + 1, rgb(120, 30, 30));
    snprintf(b, sizeof b, "%d", hpct);
    text_c_sh(hx, hy - 15, 3, dz > 0 ? rgb(255, 130 - dz, 70 - dz) : heat_col(hpct), b);
    text_c(hx, hy + 10, 1, COL_GREY, "%");

    // centre: demand (with rise-telegraph) + grid stability
    text_c(W / 2, 14, 1, COL_GREY, tx("DOMANDA", "DEMAND"));
    snprintf(b, sizeof b, "%d", s_demand);
    text_c_sh(W / 2, 24, 2, s_dem_ms > 0 ? COL_WHITE : COL_AMBER, b);
    if (s_dem_ms > 0) { int dw = (int)strlen(b) * 12; d.drawFastHLine(W / 2 - dw / 2, 41, dw * s_dem_ms / 500, COL_AMBER); }
    text_c(W / 2, 52, 1, COL_GREY, tx("RETE", "GRID"));
    hbar(W / 2 - 26, 62, 52, 7, (int)s_dstab, s_stab > 35 ? COL_CYAN : COL_RED);

    // rod bank — recessed slots inside a console panel
    int ty = 88, th = 26, pitch = 36, x0 = (W - (NROD - 1) * pitch - 24) / 2;
    d.fillRoundRect(3, ty - 4, W - 6, th + 8, 5, rgb(15, 18, 30));
    d.drawRoundRect(3, ty - 4, W - 6, th + 8, 5, rgb(34, 40, 60));
    for (int i = 0; i < NROD; i++) {
        int x = x0 + i * pitch;
        bool sel = (i == s_cur);
        d.fillRoundRect(x, ty, 24, th, 4, rgb(20, 24, 38));              // recessed slot
        int usable = th - 6;
        int hyh = ty + 3 + s_rod[i] * usable / ROD_MAX;
        uint16_t fuel = sel ? rgb(255, 206, 130) : heat_col(hpct);
        int fh = hyh - (ty + 3);
        if (fh > 0) {
            d.fillRoundRect(x + 3, ty + 3, 18, fh, 2, fuel);            // exposed fuel column
            d.drawFastVLine(x + 4, ty + 4, fh > 2 ? fh - 2 : 1, mix(fuel, COL_WHITE, 95));  // gloss
        }
        d.fillRect(x + 3, hyh, 18, (ty + th - 3) - hyh, rgb(44, 48, 66)); // inserted rod
        d.fillRoundRect(x + 2, hyh - 1, 20, 3, 1, sel ? COL_WHITE : rgb(200, 205, 220)); // handle
        d.drawRoundRect(x, ty, 24, th, 4, sel ? COL_AMBER : rgb(50, 56, 78));            // slot frame (amber when picked)
        if (sel) {
            d.fillTriangle(x + 12, ty - 9, x + 8, ty - 4, x + 16, ty - 4, COL_AMBER);
            if (s_rodfx_ms > 0) {                                       // directional spark
                if (s_rodfx_dir > 0) { for (int k = -1; k <= 1; k++) d.drawLine(x + 12, ty - 5, x + 12 + k * 3, ty - 11, k ? COL_CYAN : COL_WHITE); }
                else d.fillRect(x + 6, hyh + 2, 12, 4, COL_CYAN);
            }
        }
    }

    // SCRAM cool-flash + downward wipe
    if (s_scram_ms > 0) {
        if (s_scram_ms > 180) d.fillRect(0, 0, W, ch, mix(rgb(180, 230, 255), COL_BG, (220 - s_scram_ms) * 256 / 40));
        int yw = (220 - s_scram_ms) * ch / 220;
        if (yw - 2 >= 0) d.drawFastHLine(0, yw - 2, W, rgb(60, 120, 150));
        d.drawFastHLine(0, yw, W, COL_CYAN);
    }
    // continuous danger vignette — soft red glow fading inward, breathing with the heat
    if (dz > 0) {
        int amp = (int)(dz * (0.55f + 0.45f * sinf(s_anim * 0.4f)));
        for (int k = 0; k < 4; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, rgb(28 + amp * (4 - k) / 3, 12, 12));
    }
    if (s_dheat >= 90 && ((s_anim >> 1) & 1)) {                          // PANIC banner (blinks; clears at the screen top, between time & score)
        const char *w = tx("! CRITICO !", "! CRITICAL !");
        int ww = (int)strlen(w) * 6 + 14;
        d.fillRoundRect(W / 2 - ww / 2, 0, ww, 12, 2, COL_RED);
        text_c(W / 2, 2, 1, COL_WHITE, w);
    }
}
static void draw_card(int ch, int prog)   // score + best count-up, shared by both deaths
{
    (void)ch;
    if (prog > 1000) prog = 1000;
    if (prog < 0) prog = 0;
    char b[28];
    snprintf(b, sizeof b, "%s %d", tx("Punteggio", "Score"), s_score * prog / 1000);
    text_c_sh(W / 2, 84, 2, COL_WHITE, b);
    snprintf(b, sizeof b, "%s %d%s", tx("Record", "Best"), g_best * prog / 1000, s_newbest ? "  *" : "");
    text_c_sh(W / 2, 102, 1, s_newbest ? COL_GREEN : COL_AMBER, b);
}
static void draw_blackout(int ch, int e)
{
    d.fillRect(0, 0, W, ch, COL_BG);
    draw_core(W / 2, 34, 14, rgb(60, 80, 150));
    if (e >= 500 || ((s_anim >> 1) & 3)) text_c_sh(W / 2, 56, 3, COL_CYAN, "BLACKOUT");   // flicker in/out first 500ms
    draw_card(ch, e > 120 ? (e - 120) * 1000 / 480 : 0);
}
static void draw_meltdown(int ch, int e)
{
    int cx = W / 2, cyc = ch / 2;
    float k = s_over_total ? (float)s_over_ms / s_over_total : 0;            // 1 at blast -> 0 as it settles
    int shake = (int)(7 * k);
    int ox = shake ? ((int)(s_anim * 37) % (2 * shake + 1) - shake) : 0;
    int oy = shake ? ((int)(s_anim * 53) % (2 * shake + 1) - shake) : 0;

    for (int y = 0; y < ch; y += 3) {                                       // roiling plasma, fading to dark
        int n = ((y * 7 + (int)s_anim * 5) % 23);
        int hh = 40 + n * 7;
        d.fillRect(0, y, W, 3, mix(rgb(hh, hh / 4, 8), COL_BG, (int)((1.0f - k) * 220)));
    }
    int swEnd = s_over_total * 6 / 10;                                      // expanding shockwaves
    if (e < swEnd) {
        int maxr = e * W / swEnd;
        for (int s = 0; s < 3; s++) { int r = maxr - s * 12; if (r > 2) d.drawCircle(cx + ox, cyc + oy, r, s == 0 ? COL_WHITE : (s == 1 ? rgb(255, 170, 60) : COL_RED)); }
    }
    for (int i = 0; i < 18; i++) {                                          // radial debris with motion trails
        float a = i * 0.349f;
        int dist = (int)(e * (0.10f + (i % 5) * 0.03f));
        int x = cx + (int)(cosf(a) * dist) + ox, y = cyc + (int)(sinf(a) * dist) + oy;
        d.drawLine(cx + (int)(cosf(a) * (dist - 7)) + ox, cyc + (int)(sinf(a) * (dist - 7)) + oy, x, y, rgb(255, 120, 40));
        d.fillCircle(x, y, 1 + (i & 1), (i & 1) ? rgb(255, 210, 90) : COL_WHITE);
    }
    if (e < 700) { int rr = 11 - e / 70; if (rr > 0) d.fillCircle(cx + ox, cyc + oy, rr, mix(COL_WHITE, COL_RED, e * 256 / 700)); }
    if (e < 160) d.fillRect(0, 0, W, ch, mix(COL_WHITE, COL_BG, e * 256 / 160));   // detonation whiteout
    if (e > 240) {                                                          // title slams in, strobing
        uint16_t tc = ((s_anim >> 1) & 1) ? COL_WHITE : COL_RED;
        text_c(cx + ox + 2, 42 + oy, 3, rgb(110, 18, 18), tx("FUSIONE", "MELTDOWN"));
        text_c(cx + ox, 40 + oy, 3, tc, tx("FUSIONE", "MELTDOWN"));
    }
    if (e > s_over_total - 650) draw_card(ch, (e - (s_over_total - 650)) * 1000 / 600);   // card as chaos settles
}
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    int e = s_over_total - s_over_ms;
    if (s_over_reason == 0) draw_meltdown(ch, e);
    else draw_blackout(ch, e);
}
static void draw_scores(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    text_c(W / 2, 4, 2, COL_CYAN, tx("Classifica", "Leaderboard"));
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
        int di = s_scores[i].diff;
        if (di < 0 || di > 2) di = 1;
        char dd[2] = { DI[g_lang ? 1 : 0][di], 0 };
        text_at(W - 12, y, 1, hot ? COL_CYAN : COL_DIM, dd);
        y += 9;
    }
}
static void draw_name(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    draw_core(W / 2, 26, 7, COL_AMBER);
    text_c(W / 2, 14, 2, COL_AMBER, tx("NUOVO RECORD!", "NEW RECORD!"));
    char b[24]; snprintf(b, sizeof b, "%s %d", tx("Punteggio", "Score"), s_score);
    text_c(W / 2, 46, 1, COL_GREY, b);
    text_c(W / 2, 60, 1, COL_GREY, tx("Scrivi il tuo nome:", "Enter your name:"));
    d.fillRoundRect(40, 74, W - 80, 22, 4, rgb(20, 24, 38));
    d.drawRoundRect(40, 74, W - 80, 22, 4, COL_CYAN);
    text_at(48, 79, 2, COL_WHITE, s_name);
    int cxp = 48 + (int)strlen(s_name) * 12;
    if ((s_anim >> 3) & 1) d.fillRect(cxp + 1, 78, 9, 16, COL_CYAN);
    text_c(W / 2, ch - 12, 1, COL_DIM, tx("INVIO conferma", "ENTER confirm"));
}

// ============================ input + hint ===================================
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU: nucleo_app_set_hint(tx("SU/GIU scegli   INVIO ok   Esc esci", "UP/DN pick   ENTER ok   Esc quit")); break;
        case ST_HELP: nucleo_app_set_hint(tx("SX/DX pagine   Esc indietro", "LEFT/RIGHT pages   Esc back")); break;
        case ST_SET:  nucleo_app_set_hint(tx("SU/GIU   INVIO cambia   Esc", "UP/DN   ENTER change   Esc")); break;
        case ST_PLAY: nucleo_app_set_hint(tx("SX/DX barra  SU/W GIU/E  SPAZIO scram", "L/R rod  UP/W DN/E  SPACE scram")); break;
        case ST_OVER: nucleo_app_set_hint(tx("premi un tasto", "press any key")); break;
        case ST_SCORES: nucleo_app_set_hint(tx("Esc indietro", "Esc back")); break;
        case ST_NAME:   nucleo_app_set_hint(tx("Scrivi  INVIO ok  CANC canc  Esc", "Type  ENTER ok  DEL erase  Esc")); break;
        default: break;
    }
}
static float cap_target(void)
{
    if (s_screen == ST_MENU) return (float)(33 + s_msel * 22);
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
    if (!s_name[0]) snprintf(s_name, sizeof s_name, "%s", tx("PILOTA", "PILOT"));
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
            if (k == NK_RIGHT)     { s_cur = (s_cur + 1) % NROD; sfx(1); }
            else if (k == NK_UP || ch == 'w' || ch == 'W') rod_adjust(+1);
            else if (k == NK_DOWN || ch == 'e' || ch == 'E') rod_adjust(-1);
            else if (ch == ' ' || ch == 'x' || ch == 'X') scram();
            nucleo_app_request_draw();
            return;
        case ST_OVER:
            (void)k; (void)ch;
            if (s_over_total - s_over_ms >= 700) {
                if (s_qualify) go(ST_NAME);                       // made the top 10 -> enter a name
                else { sfx(2); s_msel = 0; go(ST_MENU); }
            }
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
            case ST_PLAY: s_cur = (s_cur + NROD - 1) % NROD; sfx(1); nucleo_app_request_draw(); break;
            case ST_NAME: { int l = (int)strlen(s_name); if (l > 0) s_name[l - 1] = 0; sfx(3); nucleo_app_request_draw(); } break;
            default: break;
        }
        return true;
    }
    switch (s_screen) {
        case ST_MENU: return false;
        case ST_NAME: name_commit(); return true;
        case ST_PLAY: case ST_HELP: case ST_SET: case ST_OVER: case ST_SCORES: sfx(3); s_msel = 0; go(ST_MENU); return true;
        default: return false;
    }
}

// ============================ draw / poll / lifecycle ========================
static void on_draw(void)
{
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        case ST_OVER: draw_over(); break;
        case ST_SCORES: draw_scores(); break;
        case ST_NAME: draw_name(); break;
        default: break;
    }
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last);
    if (dt < 0) dt = 0;
    if (dt > 250) dt = 250;
    s_last = s_now;

    if (s_screen == ST_PLAY) {
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
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        s_capY += (cap_target() - s_capY) * 0.45f;
    }

    bool animated = (s_screen != ST_HELP);
    if (!animated) return false;
    if (s_now - s_frame < 33) return false;
    s_frame = s_now;
    s_anim++;
    return true;
}
static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    scores_load();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);   // never start the game inaudibly low
    sfx_cache_check();                                             // rebuild cached clips if their sounds changed
    presynth();                                                    // cache every clip (RAM is free under exclusive)
    s_screen = ST_MENU; s_msel = 0; s_anim = 0;
    s_capY = cap_target();
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint();
    sfx(9);                                                        // opening sting — also confirms audio is alive
    nucleo_app_request_draw();
}
static void on_exit(void) { nucleo_audio_stop(); }

extern "C" void nucleo_register_reactor(void)
{
    static const nucleo_app_def_t app = {
        "reactor", "Reattore", "Games", "Tieni la potenza, evita la fusione",
        'R', C_RED, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP   // dedicate RAM + free the shared I2S/mic line so the chiptune SFX reliably play
    };
    nucleo_app_register(&app);
}
