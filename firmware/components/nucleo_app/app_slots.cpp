// app_slots.cpp — NucleoOS "Slot": a juiced 3x3 slot machine (category "Games").
//
// A professional fruit-machine feel on the Cardputer: a gold cabinet with a blinking marquee,
// three reels that spin with motion-blur and stop left-to-right with a weighted bounce, a
// last-reel ANTICIPATION (spotlight + rising tone when two high symbols already landed), flashing
// paylines, a coin shower, a smooth credit count-up and tiered win fanfares (small / big / JACKPOT).
//
// UX (per the project's games conventions): MENU / HELP (with a paytable page) / SETTINGS, bilingual
// IT/EN persisted to SD, big readable vector symbols, lots of SFX. It plays like a real slot — a win
// just pays and you keep spinning; the game only ends when the credits run out, then a keypress
// restarts a fresh game with the base balance (no name prompt, no leaderboard).
// The PLAY screen is deliberately minimal — only credits, bet and a spin prompt; everything else
// (lines, bet/line, autospin, turbo) lives in a TAB "Puntate" panel so the game view stays clean.
//
// Constraints honoured (same as app_reactor.cpp): exclusive_flags = NX_NET_APP (dedicate RAM + free
// the shared I2S/mic line so the chiptune SFX reliably play); every bit of state is static (NO heap);
// drawing goes through the buffered `d.` path (one blit/frame — ANTI-FLICKER); 8bpp has no alpha so
// every fade/glow is an integer channel-mix toward a solid colour. A ~30 Hz poll handler animates and
// steps the reel physics. LEFT/BACK reach on_back(), TAB reaches the tab handler, all other keys reach
// on_key(). Text is ASCII only. Never name a local `d` (it is the display macro).

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
#include "esp_random.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
#define COL_FELT   rgb(10, 64, 44)
#define COL_FELT2  rgb(6, 46, 32)
#define COL_DARK   rgb(4, 28, 20)
#define COL_GOLD   rgb(255, 196, 64)
#define COL_GOLDD  rgb(150, 108, 24)
#define COL_GOLDL  rgb(255, 230, 150)
#define COL_WHITE  0xFFFF
#define COL_CREAM  rgb(255, 246, 214)
#define COL_RED    rgb(244, 70, 62)
#define COL_REDD   rgb(150, 26, 26)
#define COL_CYAN   rgb(96, 212, 236)
#define COL_GREEN  rgb(120, 232, 142)
#define COL_PURPLE rgb(192, 112, 232)
#define COL_YELLOW rgb(255, 224, 72)
#define COL_ORANGE rgb(255, 150, 50)
#define COL_PINK   rgb(255, 122, 182)
#define COL_GREY   rgb(150, 162, 176)
#define COL_DIM    rgb(70, 92, 82)
#define COL_GLASS  rgb(16, 20, 30)
#define COL_GLASS2 rgb(28, 34, 50)
#define COL_PANEL  rgb(20, 24, 34)

// ============================ geometry =======================================
#define NREEL 3
#define NROW  3
#define SLEN  30
#define REEL_W 62
#define REEL_GAP 7
#define GRID_W (NREEL * REEL_W + (NREEL - 1) * REEL_GAP)   // 200
#define GRID_X ((W - GRID_W) / 2)                          // 20
#define CELL_H 33
#define GRID_Y 18
#define GRID_H (NROW * CELL_H)                             // 99  -> window 18..117 (fills down to the system footer)
#define SYM_S  15                                          // symbol half-size (px)

// ============================ symbols / paytable =============================
enum { SY_CHERRY = 0, SY_LEMON, SY_PLUM, SY_BELL, SY_BAR, SY_DIAMOND, SY_SEVEN, SY_WILD, NSYM };
// per-reel frequency (sums to SLEN = 30): sevens & wilds are rare
static const uint8_t SYM_W[NSYM] = { 6, 6, 5, 4, 4, 3, 1, 1 };

// 3-of-a-kind multiplier of the per-line bet; cherry also pays for 2.
static int paymul(int sym, int run)
{
    if (run >= 3) {
        switch (sym) {
            case SY_CHERRY:  return 5;
            case SY_LEMON:   return 8;
            case SY_PLUM:    return 12;
            case SY_BELL:    return 20;
            case SY_BAR:     return 40;
            case SY_DIAMOND: return 80;
            case SY_SEVEN:   return 200;
            case SY_WILD:    return 500;
        }
    }
    if (run == 2 && sym == SY_CHERRY) return 2;
    return 0;
}
static uint16_t sym_color(int id)
{
    switch (id) {
        case SY_CHERRY:  return COL_RED;
        case SY_LEMON:   return COL_YELLOW;
        case SY_PLUM:    return COL_PURPLE;
        case SY_BELL:    return COL_GOLD;
        case SY_BAR:     return COL_CYAN;
        case SY_DIAMOND: return COL_CYAN;
        case SY_SEVEN:   return COL_RED;
        case SY_WILD:    return COL_GOLD;
    }
    return COL_WHITE;
}

// paylines (row index per reel) + their accent colour. lines 1 -> {0}; 3 -> {0,1,2}; 5 -> all.
static const uint8_t PAYLINE[5][NREEL] = {
    { 1, 1, 1 },   // middle
    { 0, 0, 0 },   // top
    { 2, 2, 2 },   // bottom
    { 0, 1, 2 },   // diagonal down
    { 2, 1, 0 },   // diagonal up
};
static const uint16_t LINECOL[5] = { COL_GOLD, COL_CYAN, COL_GREEN, COL_PINK, COL_ORANGE };

static const int LINES_OPT[3] = { 1, 3, 5 };
static const int BET_OPT[5]   = { 1, 2, 5, 10, 25 };
#define NBET 4   // bet-panel rows: Lines / Bet-per-line / Auto / Turbo (TAB or Esc closes)

// ============================ state ==========================================
enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_BET, ST_OVER };
#define START_CREDITS 1000   // fresh-game balance (restart amount when the credits run out)
// PLAY sub-phase (the spin/win lifecycle)
enum { PP_IDLE = 0, PP_SPIN, PP_WIN };

static int   g_lang = 0, g_audio = 1, g_turbo = 0;
static int   g_balance = START_CREDITS, g_best_win = 0;
static int   g_lines_idx = 2, g_bet_idx = 1;   // default: 5 lines, 2/line

static int   s_screen;
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;

static int   s_msel, s_setsel, s_help, s_betsel;
static float s_capY;
static int   s_msg_ms;                          // transient toast (e.g. "lower the bet")
static const char *s_msg;

// reels
static uint8_t s_strip[NREEL][SLEN];
static float r_pos[NREEL];                       // continuous top-index
static float r_start[NREEL], r_target[NREEL];
static int   r_phase[NREEL];                      // 0 done/idle, 1 spin, 2 stop, 3 bounce
static int   r_t[NREEL];
static int   r_delay[NREEL];
static int   r_top[NREEL];                         // strip index of the resting top row (result)

// spin / win lifecycle
static int   s_pp;
static int   s_spin_ms;
static bool  s_antic, s_antic_checked;
static int   s_win_total, s_win_ms, s_win_tier;   // tier 0 small,1 big,2 mega
static bool  s_cellwin[NREEL][NROW];
static bool  s_linewin[5];
static float s_disp_bal;                           // eased credit readout (count-up)
static int   s_auto_ms;                            // autospin inter-round delay
static bool  s_auto;                               // autospin armed
// juice: sequential payline reveal + mega-win impact
static int   r_flash_ms[NREEL];                    // per-reel "just landed" white flash
static int   s_win_lines[5], s_win_run[5], s_win_nlines;   // winning lines collected by evaluate()
static int   s_win_shown, s_win_step_ms;           // how many revealed so far + countdown to next
static int   s_finale_ms;                          // beat before the finale fires (after last line shown)
static bool  s_finale_done;                        // finale (coin fountain + tier sound) has fired
static int   s_shake_ms, s_flash_ms, s_coinwave_ms;// mega screen-shake, white flash, jackpot coin waves
static int   s_shx, s_shy;                          // current shake offset (set each frame in draw_play)
static bool  s_quickstop;                           // player slammed the spin button to stop the reels early
static int   s_fin_age;                             // ms since the finale fired (drives banner pop + edge glow)

// coin particles
#define NPART 40
struct Coin { float x, y, vx, vy; int life, max; uint16_t col; };
static Coin s_coin[NPART];

// firework bursts (mega/jackpot celebration only)
#define NFW 5
struct FW { int x, y, age; bool live; uint16_t col; };
static FW s_fw[NFW];
static int s_fw_ms;                                  // mega: countdown to the next burst

static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static int   total_bet(void) { return LINES_OPT[g_lines_idx] * BET_OPT[g_bet_idx]; }
static int   active_lines(void) { return LINES_OPT[g_lines_idx]; }
static void  go(int s);                                        // screen switch (defined below)

// ============================ persistence ====================================
#define DIRR "/sd/data/slots"
#define CFG_MAGIC 0x534C4F54u   // 'SLOT'
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, t, bal, best, li, bi; } c =
        { CFG_MAGIC, g_lang, g_audio, g_turbo, g_balance, g_best_win, g_lines_idx, g_bet_idx };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a, t, bal, best, li, bi; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) {
        g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_turbo = c.t ? 1 : 0;
        g_balance = c.bal < 0 ? 0 : c.bal; g_best_win = c.best < 0 ? 0 : c.best;
        g_lines_idx = (c.li < 0 || c.li > 2) ? 2 : c.li;
        g_bet_idx   = (c.bi < 0 || c.bi > 4) ? 1 : c.bi;
    }
}

// ============================ audio ==========================================
// id -> short cache name
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "move";  case 2: return "sel";   case 3: return "back";  case 4: return "bet";
        case 5: return "spin";  case 6: return "stop1";  case 7: return "stop2"; case 8: return "stop3";
        case 9: return "winS";  case 10:return "winB";   case 11:return "jack";  case 12:return "antic";
        case 13:return "nowin"; case 14:return "near";   case 15:return "bonus";
        case 16:return "ln1";   case 17:return "ln2";    case 18:return "ln3";   case 19:return "ln4";
        case 20:return "ln5";   case 21:return "casc";   case 22:return "chach"; case 23:return "sprk";
        case 24:return "drum";  default: return "x";
    }
}
#define NSFX 24
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1:  notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.55f; return 1;                                    // nav blip
        case 2:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.10f); return 2;     // select ding
        case 3:  notify__voice(&v[0], 587.33f, 0, 0.07f); notify__voice(&v[1], 392, 0.05f, 0.10f); return 2;         // back (down)
        case 4:  notify__voice(&v[0], 880, 0, 0.04f); notify__voice(&v[1], 1318, 0.03f, 0.05f); v[0].amp = 0.7f; return 2; // bet tick
        case 5:  notify__voice(&v[0], 130, 0, 0.22f); v[0].amp = 0.8f;                                               // lever / spin whoosh
                 notify__voice(&v[1], 330, 0, 0.06f); notify__voice(&v[2], 440, 0.05f, 0.06f);
                 notify__voice(&v[3], 587, 0.10f, 0.07f); return 4;
        case 6:  notify__voice(&v[0], 196, 0, 0.07f); v[0].amp = 0.9f; notify__voice(&v[1], 1100, 0, 0.02f); return 2; // reel clunk (low)
        case 7:  notify__voice(&v[0], 220, 0, 0.07f); v[0].amp = 0.9f; notify__voice(&v[1], 1250, 0, 0.02f); return 2;
        case 8:  notify__voice(&v[0], 247, 0, 0.08f); v[0].amp = 1.0f; notify__voice(&v[1], 1400, 0, 0.02f); return 2;
        case 9:  notify__voice(&v[0], 880, 0, 0.06f); notify__voice(&v[1], 1174.7f, 0.05f, 0.06f);                    // small win — coin pings
                 notify__voice(&v[2], 1567.98f, 0.10f, 0.10f); return 3;
        case 10: notify__voice(&v[0], 523.25f, 0, 0.10f); notify__voice(&v[1], 659.25f, 0.08f, 0.10f);               // big win fanfare
                 notify__voice(&v[2], 783.99f, 0.16f, 0.12f); notify__voice(&v[3], 1046.5f, 0.24f, 0.16f);
                 notify__voice(&v[4], 1318.5f, 0.30f, 0.22f); return 5;
        case 11: notify__voice(&v[0], 523.25f, 0.00f, 0.12f); notify__voice(&v[1], 783.99f, 0.10f, 0.12f);           // JACKPOT — long, bright, siren tail
                 notify__voice(&v[2], 1046.5f, 0.20f, 0.14f); notify__voice(&v[3], 1318.5f, 0.30f, 0.16f);
                 notify__voice(&v[4], 1567.98f, 0.40f, 0.20f); notify__voice(&v[5], 2093.0f, 0.50f, 0.34f); return 6;
        case 12: notify__voice(&v[0], 440, 0.00f, 0.16f); notify__voice(&v[1], 554, 0.14f, 0.18f);                   // anticipation — rising tension
                 notify__voice(&v[2], 659, 0.30f, 0.30f); return 3;
        case 13: notify__voice(&v[0], 196, 0, 0.10f); v[0].amp = 0.4f; notify__voice(&v[1], 165, 0.06f, 0.10f); v[1].amp = 0.4f; return 2; // no-win
        case 14: notify__voice(&v[0], 659.25f, 0, 0.10f); notify__voice(&v[1], 523.25f, 0.10f, 0.10f);               // near miss "aww"
                 notify__voice(&v[2], 392, 0.20f, 0.18f); return 3;
        case 15: notify__voice(&v[0], 1318.5f, 0, 0.06f); notify__voice(&v[1], 1567.98f, 0.06f, 0.08f);              // bonus — cash register
                 notify__voice(&v[2], 880, 0.14f, 0.06f); notify__voice(&v[3], 1046.5f, 0.20f, 0.10f); return 4;
        // --- ascending payline-reveal chimes (one per winning line, rising pitch) ---
        case 16: notify__voice(&v[0], 523.25f, 0, 0.10f); notify__voice(&v[1], 783.99f, 0.04f, 0.13f); return 2;    // C+G
        case 17: notify__voice(&v[0], 659.25f, 0, 0.10f); notify__voice(&v[1], 987.77f, 0.04f, 0.13f); return 2;    // E+B
        case 18: notify__voice(&v[0], 783.99f, 0, 0.10f); notify__voice(&v[1], 1174.7f, 0.04f, 0.13f); return 2;    // G+D
        case 19: notify__voice(&v[0], 987.77f, 0, 0.10f); notify__voice(&v[1], 1479.98f, 0.04f, 0.13f); return 2;   // B+F#
        case 20: notify__voice(&v[0], 1174.7f, 0, 0.11f); notify__voice(&v[1], 1760.0f, 0.04f, 0.15f); return 2;    // D+A
        case 21: notify__voice(&v[0], 1567.98f, 0.00f, 0.05f); notify__voice(&v[1], 1318.5f, 0.05f, 0.05f);         // coin cascade (rollup)
                 notify__voice(&v[2], 1046.5f, 0.10f, 0.05f); notify__voice(&v[3], 1318.5f, 0.16f, 0.05f);
                 notify__voice(&v[4], 1567.98f, 0.22f, 0.06f); notify__voice(&v[5], 2093.0f, 0.28f, 0.14f); return 6;
        case 22: notify__voice(&v[0], 1318.5f, 0.00f, 0.10f); notify__voice(&v[1], 1567.98f, 0.10f, 0.12f);         // cha-ching (cash register)
                 notify__voice(&v[2], 1046.5f, 0.22f, 0.06f); notify__voice(&v[3], 1318.5f, 0.28f, 0.06f);
                 notify__voice(&v[4], 1760.0f, 0.34f, 0.16f); return 5;
        case 23: notify__voice(&v[0], 2093.0f, 0.00f, 0.07f); notify__voice(&v[1], 2637.02f, 0.05f, 0.10f); return 2; // sparkle (7 / WILD lands)
        case 24: notify__voice(&v[0], 98, 0.00f, 0.04f); v[0].amp = 0.9f;                                            // drumroll build (anticipation)
                 notify__voice(&v[1], 98, 0.07f, 0.04f); v[1].amp = 0.9f; notify__voice(&v[2], 110, 0.13f, 0.04f); v[2].amp = 0.95f;
                 notify__voice(&v[3], 110, 0.18f, 0.04f); v[3].amp = 0.95f; notify__voice(&v[4], 123, 0.22f, 0.05f);
                 notify__voice(&v[5], 147, 0.27f, 0.06f); notify__voice(&v[6], 196, 0.33f, 0.12f); return 7;
    }
    return 0;
}
static bool sfx_important(int id) { return id >= 5 && id <= 24 && id != 13 && id != 14; }
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    char p[80];
    snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else { notify_voice_t v[8]; int nv = build_voices(id, v); if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return; }
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
// bump whenever a build_voices() clip changes so stale cached WAVs are rebuilt on next launch.
#define SFX_VER 2
static void sfx_cache_check(void)
{
    int ver = 0;
    FILE *f = fopen(DIRR "/sfx/ver.bin", "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver == SFX_VER) return;
    for (int id = 1; id <= NSFX; id++) { char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); remove(p); }
    f = fopen(DIRR "/sfx/ver.bin", "wb");
    if (f) { int vv = SFX_VER; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
}
static void presynth(void)
{
    if (!g_audio) return;
    notify_voice_t v[8];
    for (int id = 1; id <= NSFX; id++) {
        char p[80];
        snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ reels: build / read ============================
static void build_strips(void)
{
    for (int r = 0; r < NREEL; r++) {
        int idx = 0;
        for (int s = 0; s < NSYM; s++) for (int c = 0; c < SYM_W[s]; c++) s_strip[r][idx++] = (uint8_t)s;
        for (int i = SLEN - 1; i > 0; i--) {                         // Fisher-Yates, hardware RNG
            int j = (int)(esp_random() % (uint32_t)(i + 1));
            uint8_t t = s_strip[r][i]; s_strip[r][i] = s_strip[r][j]; s_strip[r][j] = t;
        }
    }
}
static inline int wrap(int i) { i %= SLEN; if (i < 0) { i += SLEN; } return i; }

// ============================ draw helpers ===================================
static void text_at(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void text_c(int cx, int y, int sz, uint16_t col, const char *s) { text_at(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void text_c_sh(int cx, int y, int sz, uint16_t col, const char *s) { text_c(cx + 1, y + 1, sz, mix(col, COL_DARK, 200), s); text_c(cx, y, sz, col, s); }
static void text_r(int rx, int y, int sz, uint16_t col, const char *s) { text_at(rx - (int)strlen(s) * 6 * sz, y, sz, col, s); }

static void star(int cx, int cy, int ro, int ri, uint16_t col)
{
    float px[10], py[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.57079633f + i * 0.62831853f;
        int r = (i & 1) ? ri : ro;
        px[i] = cx + cosf(a) * r; py[i] = cy + sinf(a) * r;
    }
    for (int i = 0; i < 10; i++) {
        int j = (i + 1) % 10;
        d.fillTriangle(cx, cy, (int)px[i], (int)py[i], (int)px[j], (int)py[j], col);
    }
}

// One slot symbol, centred at (cx,cy), half-size s, with a 0..255 highlight pulse `glow`.
static void draw_sym(int cx, int cy, int s, int id, int glow)
{
    uint16_t hl = mix(sym_color(id), COL_WHITE, glow);
    switch (id) {
        case SY_CHERRY: {
            d.drawLine(cx + 1, cy - s + 2, cx - s / 2, cy + s / 3, COL_GREEN);   // stems
            d.drawLine(cx + 1, cy - s + 2, cx + s / 2, cy + s / 3, COL_GREEN);
            d.fillTriangle(cx + 1, cy - s + 2, cx + s / 2 + 2, cy - s + 1, cx + s - 2, cy - s / 2, COL_GREEN); // leaf
            int rr = s * 5 / 9;
            d.fillCircle(cx - s / 2, cy + s / 3, rr, COL_REDD);
            d.fillCircle(cx - s / 2, cy + s / 3, rr - 1, mix(COL_RED, COL_WHITE, glow));
            d.fillCircle(cx - s / 2 - rr / 3, cy + s / 3 - rr / 3, rr / 3, COL_CREAM);
            d.fillCircle(cx + s / 2, cy + s / 3 + 1, rr, COL_REDD);
            d.fillCircle(cx + s / 2, cy + s / 3 + 1, rr - 1, mix(COL_RED, COL_REDD, 80));
            d.fillCircle(cx + s / 2 - rr / 3, cy + s / 3 + 1 - rr / 3, rr / 3, COL_CREAM);
            break;
        }
        case SY_LEMON: {
            d.fillEllipse(cx, cy, s, s * 3 / 4, COL_GOLDD);
            d.fillEllipse(cx, cy, s - 1, s * 3 / 4 - 1, hl);
            d.fillEllipse(cx - s / 3, cy - s / 4, s / 3, s / 5, COL_CREAM);          // sheen
            d.fillCircle(cx + s - 1, cy, 1, COL_GREEN);                              // nib
            break;
        }
        case SY_PLUM: {
            d.fillCircle(cx, cy + 1, s - 1, mix(COL_PURPLE, COL_DARK, 110));
            d.fillCircle(cx, cy + 1, s - 2, hl);
            d.fillCircle(cx - s / 3, cy - s / 3 + 1, s / 3, mix(COL_PURPLE, COL_WHITE, 120 + glow / 2)); // sheen
            d.drawLine(cx, cy - s + 1, cx + 2, cy - s + 4, COL_GREEN);
            d.fillTriangle(cx + 1, cy - s + 2, cx + s / 2 + 2, cy - s + 1, cx + s - 3, cy - s / 2 + 1, COL_GREEN);
            break;
        }
        case SY_BELL: {
            d.fillTriangle(cx, cy - s + 2, cx - s + 1, cy + s - 3, cx + s - 1, cy + s - 3, COL_GOLDD);   // body
            d.fillTriangle(cx, cy - s + 3, cx - s + 3, cy + s - 4, cx + s - 3, cy + s - 4, hl);
            d.fillRoundRect(cx - s + 1, cy + s - 5, 2 * s - 2, 4, 2, COL_GOLDD);    // rim
            d.fillRoundRect(cx - s + 2, cy + s - 5, 2 * s - 4, 2, 1, COL_GOLDL);
            d.fillCircle(cx, cy - s + 1, 2, COL_GOLDL);                              // knob
            d.fillCircle(cx, cy + s - 1, 2, COL_GOLDD);                              // clapper
            d.drawLine(cx - s / 3, cy - s + 6, cx - s / 2, cy + s - 6, COL_CREAM);   // shine
            break;
        }
        case SY_BAR: {
            d.fillRoundRect(cx - s, cy - s / 2 - 1, 2 * s, s + 2, 3, COL_GOLDD);
            d.fillRoundRect(cx - s + 1, cy - s / 2, 2 * s - 2, s, 3, mix(COL_CYAN, COL_WHITE, glow / 2));
            text_c(cx, cy - 3, 1, COL_DARK, "BAR");
            break;
        }
        case SY_DIAMOND: {
            int t = cy - s / 2;                                                       // table line
            d.fillTriangle(cx - s + 2, t, cx + s - 2, t, cx, cy + s, mix(COL_CYAN, COL_DARK, 80)); // body
            d.fillTriangle(cx - s + 3, t + 1, cx + s - 3, t + 1, cx, cy + s - 2, hl);
            d.drawLine(cx - s + 2, t, cx, cy + s, mix(COL_WHITE, COL_CYAN, 120));     // facets
            d.drawLine(cx + s - 2, t, cx, cy + s, mix(COL_WHITE, COL_CYAN, 120));
            d.drawLine(cx - s + 2, t, cx + s - 2, t, COL_WHITE);
            d.drawLine(cx - s / 2 + 1, t, cx, cy + s - 3, mix(COL_WHITE, COL_CYAN, 60));
            d.fillTriangle(cx - s / 2, t + 1, cx - 1, t + 1, cx - s / 4, t + s / 2, COL_WHITE); // sparkle facet
            break;
        }
        case SY_SEVEN: {
            uint16_t c = mix(COL_RED, COL_WHITE, glow);
            for (int o = 1; o >= 0; o--) {                                           // o=1 dark outline, o=0 face
                uint16_t cc = o ? COL_REDD : c;
                int dx = o, dy = o;
                d.fillRect(cx - s + 2 + dx, cy - s + 1 + dy, 2 * s - 4, 4, cc);       // top bar
                d.fillTriangle(cx + s - 8 + dx, cy - s + 1 + dy, cx + s - 2 + dx, cy - s + 1 + dy, cx + 2 + dx, cy + s + dy, cc);
                d.fillTriangle(cx + s - 8 + dx, cy - s + 1 + dy, cx + 2 + dx, cy + s + dy, cx - 4 + dx, cy + s + dy, cc);
            }
            d.drawLine(cx - s + 3, cy - s + 1, cx + s - 4, cy - s + 1, COL_GOLDL);    // gold gleam on the bar
            break;
        }
        case SY_WILD: {
            star(cx, cy, s, s * 2 / 5, COL_GOLDD);
            star(cx, cy, s - 1, s * 2 / 5, mix(COL_GOLD, COL_WHITE, glow));
            d.fillCircle(cx, cy, s / 3 + 1, mix(COL_REDD, COL_RED, 120));
            text_c(cx, cy - 3, 1, COL_CREAM, "W");
            break;
        }
    }
}

// ============================ background / cabinet ===========================
static void draw_felt(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_FELT);
    for (int y = 0; y < ch; y += 6) d.drawFastHLine(0, y, W, COL_FELT2);            // subtle weave
    for (int k = 0; k < 5; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_DARK, COL_FELT, k * 50)); // vignette
}
// blinking marquee bulbs along a horizontal span
static void bulbs(int x0, int x1, int y, int phase)
{
    int n = (x1 - x0) / 12;
    for (int i = 0; i <= n; i++) {
        int x = x0 + i * 12;
        bool on = ((i + phase) & 1);
        d.fillCircle(x, y, 2, on ? COL_GOLDL : COL_GOLDD);
        if (on) d.drawCircle(x, y, 3, mix(COL_GOLD, COL_FELT, 120));
    }
}
static void draw_cabinet(void)
{
    // triple gold frame hugging the reel window (sits below the top HUD, above the bottom HUD)
    d.drawRoundRect(GRID_X - 3, GRID_Y - 3, GRID_W + 6, GRID_H + 6, 5, COL_GOLDD);
    d.drawRoundRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4, 4, COL_GOLD);
    d.drawRoundRect(GRID_X - 1, GRID_Y - 1, GRID_W + 2, GRID_H + 2, 3, COL_GOLDL);
    // corner bolts
    int bx[4] = { GRID_X - 2, GRID_X + GRID_W + 1, GRID_X - 2, GRID_X + GRID_W + 1 };
    int by[4] = { GRID_Y - 2, GRID_Y - 2, GRID_Y + GRID_H + 1, GRID_Y + GRID_H + 1 };
    for (int i = 0; i < 4; i++) { d.fillCircle(bx[i], by[i], 2, COL_GOLDL); d.drawCircle(bx[i], by[i], 2, COL_GOLDD); }
}

// ============================ reels render ===================================
static void draw_reel_one(int reel)
{
    int rx = GRID_X + reel * (REEL_W + REEL_GAP);
    // glass background (vertical sheen)
    for (int y = 0; y < GRID_H; y++) {
        int t = 60 + (int)(40 * sinf((y * 3.14159f) / GRID_H));
        d.drawFastHLine(rx, GRID_Y + y, REEL_W, mix(COL_GLASS, COL_GLASS2, t));
    }
    float pos = r_pos[reel];
    int base = (int)floorf(pos);
    float frac = pos - base;
    bool fast = (r_phase[reel] == 1) || (r_phase[reel] == 2 && r_t[reel] < 140);
    int cx = rx + REEL_W / 2 + s_shx;
    for (int k = -1; k <= NROW; k++) {
        int sy = GRID_Y + (int)((k - frac) * CELL_H) + CELL_H / 2 + s_shy;
        if (sy < GRID_Y - CELL_H || sy > GRID_Y + GRID_H + CELL_H) continue;
        int id = s_strip[reel][wrap(base + k)];
        int glow = 0;
        // resting winning cell pulses; a just-landed symbol gets a white flash that fades out
        if (r_phase[reel] == 0 && k >= 0 && k < NROW) {
            if (s_cellwin[reel][k] && s_pp == PP_WIN) glow = 90 + (int)(90 * sinf(s_anim * 0.4f));
            if (r_flash_ms[reel] > 0) { int fg = r_flash_ms[reel] * 255 / 150; if (fg > glow) glow = fg; }
            if (glow > 255) glow = 255;
        }
        if (fast) {
            // motion blur: dim + horizontal streaks, symbol squashed slightly
            uint16_t sc = mix(sym_color(id), COL_GLASS, 150);
            d.drawFastHLine(rx + 4, sy - SYM_S / 2, REEL_W - 8, sc);
            d.drawFastHLine(rx + 4, sy, REEL_W - 8, mix(sym_color(id), COL_GLASS, 90));
            d.drawFastHLine(rx + 4, sy + SYM_S / 2, REEL_W - 8, sc);
            draw_sym(cx, sy, SYM_S - 3, id, 0);
        } else {
            int ssz = SYM_S;
            if (r_phase[reel] == 0 && k >= 0 && k < NROW && s_cellwin[reel][k] && s_pp == PP_WIN) {
                int amp = s_win_tier == 2 ? 4 : (s_win_tier == 1 ? 2 : 1);             // win symbols throb — bigger for bigger wins
                ssz = SYM_S + (int)(amp * (0.5f + 0.5f * sinf(s_anim * 0.4f)));
            }
            draw_sym(cx, sy, ssz, id, glow);
        }
    }
    // payline guides on the resting reel (faint), main line a touch brighter
    if (r_phase[reel] == 0 && s_pp != PP_WIN) {
        d.drawFastHLine(rx, GRID_Y + CELL_H + CELL_H / 2, REEL_W, mix(COL_GLASS2, COL_GOLD, 40));
    }
}
static void draw_reels(void)
{
    d.setClipRect(GRID_X, GRID_Y, GRID_W, GRID_H);
    for (int r = 0; r < NREEL; r++) draw_reel_one(r);
    d.clearClipRect();
    // gold posts between reels
    for (int r = 0; r < NREEL - 1; r++) {
        int x = GRID_X + r * (REEL_W + REEL_GAP) + REEL_W + REEL_GAP / 2;
        d.drawFastVLine(x, GRID_Y, GRID_H, COL_GOLDD);
    }
    // anticipation: dim the settled reels with a screen-door, spotlight the still-spinning last reel
    if (s_antic && r_phase[NREEL - 1] != 0) {
        for (int rr = 0; rr < NREEL - 1; rr++) {
            int dx = GRID_X + rr * (REEL_W + REEL_GAP);
            for (int yy = GRID_Y; yy < GRID_Y + GRID_H; yy += 2) d.drawFastHLine(dx, yy, REEL_W, COL_DARK);
        }
        int rx = GRID_X + (NREEL - 1) * (REEL_W + REEL_GAP);
        int a = 70 + (int)(80 * sinf(s_anim * 0.5f));
        for (int k = 0; k < 3; k++) d.drawRoundRect(rx - 2 - k, GRID_Y - 2 - k, REEL_W + 4 + 2 * k, GRID_H + 4 + 2 * k, 4, mix(COL_GLASS, COL_GOLDL, a - k * 18));
    }
}

// ============================ HUD (minimal) ==================================
static void draw_hud(void)
{
    char b[28];
    // single top strip (0..16): credits (left) + bet/lines (right) + AUTO badge — the only on-screen HUD;
    // controls live in the system footer (set in set_hint). The reels fill everything below to y117.
    d.fillRect(0, 0, W, 17, COL_DARK);
    d.drawFastHLine(0, 16, W, COL_GOLDD);
    d.fillCircle(11, 8, 5, COL_GOLDD); d.fillCircle(11, 8, 4, COL_GOLD); d.drawCircle(11, 8, 5, COL_GOLDL);
    text_c(11, 3, 1, COL_DARK, "C");                                                  // coin glyph
    snprintf(b, sizeof b, "%d", (int)(s_disp_bal + 0.5f));
    bool rolling = ((int)(s_disp_bal + 0.5f) != g_balance);                            // credits counting up after a win
    text_at(20, 1, 2, rolling && ((s_anim >> 1) & 1) ? COL_WHITE : COL_GOLDL, b);
    snprintf(b, sizeof b, "%s %d  L%d", tx("PUNT", "BET"), total_bet(), active_lines());
    text_r(W - 6, 5, 1, COL_CREAM, b);
    if (s_pp == PP_IDLE && (s_auto || s_auto_ms > 0)) {                                // autospin indicator
        int aw = 4 * 6 + 8;
        d.fillRoundRect(W / 2 - aw / 2, 2, aw, 12, 3, mix(COL_DARK, COL_CYAN, 70));
        text_c(W / 2, 4, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_CYAN, "AUTO");
    }
}

// ============================ coin particles =================================
static void coins_spawn(int n, int tier)
{
    for (int i = 0; i < NPART && n > 0; i++) {
        if (s_coin[i].life > 0) continue;
        s_coin[i].x = GRID_X + 10 + (int)(esp_random() % (GRID_W - 20));
        s_coin[i].y = GRID_Y + GRID_H / 2;
        s_coin[i].vx = ((int)(esp_random() % 200) - 100) / 40.0f;
        s_coin[i].vy = -2.2f - (esp_random() % 100) / 60.0f - tier * 0.5f;
        s_coin[i].max = s_coin[i].life = 30 + (int)(esp_random() % 24);
        s_coin[i].col = (esp_random() & 1) ? COL_GOLD : COL_GOLDL;
        n--;
    }
}
static void coins_step(int dt)
{
    float k = dt / 33.0f;
    float floorY = (float)(GRID_Y + GRID_H - 3);
    for (int i = 0; i < NPART; i++) {
        if (s_coin[i].life <= 0) continue;
        s_coin[i].vy += 0.34f * k;
        s_coin[i].x += s_coin[i].vx * k;
        s_coin[i].y += s_coin[i].vy * k;
        if (s_coin[i].y > floorY && s_coin[i].vy > 0) {                                // bounce on the reel floor
            s_coin[i].y = floorY; s_coin[i].vy = -s_coin[i].vy * 0.5f; s_coin[i].vx *= 0.7f;
        }
        s_coin[i].life -= dt;
    }
}
static void coins_draw(void)
{
    for (int i = 0; i < NPART; i++) {
        if (s_coin[i].life <= 0) continue;
        int x = (int)s_coin[i].x, y = (int)s_coin[i].y;
        d.fillCircle(x, y, 3, COL_GOLDD);
        d.fillCircle(x, y, 2, s_coin[i].col);
        d.fillCircle(x - 1, y - 1, 1, COL_WHITE);                                      // shine
    }
}
// fireworks: a coloured ring of sparks that expands and fades — the jackpot-only flourish
static void fw_spawn(void)
{
    static const uint16_t C[4] = { COL_GOLD, COL_CYAN, COL_PINK, COL_GREEN };
    for (int i = 0; i < NFW; i++) {
        if (s_fw[i].live) continue;
        s_fw[i].live = true; s_fw[i].age = 0;
        s_fw[i].x = 30 + (int)(esp_random() % (W - 60));
        s_fw[i].y = GRID_Y + 8 + (int)(esp_random() % (GRID_H - 24));
        s_fw[i].col = C[esp_random() & 3];
        return;
    }
}
static void fw_step(int dt) { for (int i = 0; i < NFW; i++) if (s_fw[i].live) { s_fw[i].age += dt; if (s_fw[i].age > 460) s_fw[i].live = false; } }
static void fw_draw(void)
{
    for (int i = 0; i < NFW; i++) {
        if (!s_fw[i].live) continue;
        int a = s_fw[i].age, rr = a * 24 / 460, fade = 256 - a * 256 / 460;
        uint16_t c = mix(COL_FELT, s_fw[i].col, fade);
        for (int k = 0; k < 10; k++) {
            float ang = k * 0.628318f;
            d.fillCircle(s_fw[i].x + (int)(cosf(ang) * rr), s_fw[i].y + (int)(sinf(ang) * rr), fade > 130 ? 2 : 1, c);
        }
        d.drawCircle(s_fw[i].x, s_fw[i].y, rr, mix(COL_FELT, COL_WHITE, fade / 2));
    }
}

// ============================ spin / evaluate ================================
static void reset_win_marks(void)
{
    for (int r = 0; r < NREEL; r++) for (int w = 0; w < NROW; w++) s_cellwin[r][w] = false;
    for (int i = 0; i < 5; i++) s_linewin[i] = false;
}
static void do_spin(void)
{
    int bet = total_bet();
    if (g_balance < bet) {
        if (g_balance <= 0) { sfx(3); go(ST_OVER); return; }
        s_msg = tx("Riduci la puntata", "Lower the bet"); s_msg_ms = 1100; sfx(13); nucleo_app_request_draw(); return;
    }
    g_balance -= bet;
    reset_win_marks();
    s_pp = PP_SPIN; s_spin_ms = 0; s_antic = false; s_antic_checked = false;
    s_win_total = 0; s_auto_ms = 0;
    s_win_nlines = 0; s_win_shown = 0; s_finale_done = false; s_fin_age = 0;
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_quickstop = false;
    for (int i = 0; i < NFW; i++) s_fw[i].live = false;
    for (int r = 0; r < NREEL; r++) {
        r_phase[r] = 1; r_t[r] = 0; r_flash_ms[r] = 0;
        r_delay[r] = g_turbo ? (240 + r * 150) : (520 + r * 300);
    }
    sfx(5);
    nucleo_app_request_draw();
}
// Slam-stop (pro-slot feel): the spin button pressed mid-spin drops every still-spinning reel into its
// stop now, in a quick left-to-right snap. The landing symbols are computed at stop time exactly as in a
// natural stop, so the outcome stays fair/random — you only skip the wait.
static void quick_stop(void)
{
    if (s_quickstop) return;
    s_quickstop = true;
    s_antic = false; s_antic_checked = true;                  // cancel any anticipation hold
    int off = 0;
    for (int r = 0; r < NREEL; r++) {
        if (r_phase[r] == 1) { r_delay[r] = s_spin_ms + off; off += 90; }
    }
    sfx(2);
    nucleo_app_request_draw();
}
// would a third matching high symbol pay? checked from reels 0,1 already-known results.
static bool antic_possible(void)
{
    for (int l = 0; l < active_lines(); l++) {
        int a = s_strip[0][wrap(r_top[0] + PAYLINE[l][0])];
        int b = s_strip[1][wrap(r_top[1] + PAYLINE[l][1])];
        int base = a;
        if (base == SY_WILD) base = (b != SY_WILD) ? b : SY_WILD;
        bool m0 = (a == base || a == SY_WILD), m1 = (b == base || b == SY_WILD);
        if (m0 && m1 && base >= SY_BELL) return true;     // two high symbols lined up -> tension
    }
    return false;
}
// Reveal the i-th winning line: light it, mark its symbols (so the reels glow), flash those reels
// and play an ascending chime — the classic "ding... ding... ding..." that makes a win feel earned.
#define REVEAL_MS 340
static void win_reveal(int i)
{
    if (i < 0 || i >= s_win_nlines) return;
    int l = s_win_lines[i];
    s_linewin[l] = true;
    for (int r = 0; r < s_win_run[i]; r++) { s_cellwin[r][PAYLINE[l][r]] = true; r_flash_ms[r] = 170; }
    sfx(16 + (i < 5 ? i : 4));                                  // rising chime, one per line revealed
}
// All lines shown -> the payoff: coin fountain + a tier-scaled finale (cascade / cha-ching / JACKPOT
// siren with a white flash, screen shake and a continuous coin fountain).
static void win_finale(void)
{
    int tier = s_win_tier;
    s_fin_age = 0;
    coins_spawn(tier == 2 ? 34 : (tier == 1 ? 20 : 10), tier);
    if (tier == 2)      { sfx(11); s_flash_ms = 200; s_shake_ms = 650; s_coinwave_ms = 1700; s_fw_ms = 0; }
    else if (tier == 1) { sfx(22); s_shake_ms = 300; s_coinwave_ms = 700; }
    else                  sfx(21);
}
static void evaluate(void)
{
    int total = 0, lines = active_lines(), bpl = BET_OPT[g_bet_idx];
    bool jackpotish = false;
    s_win_nlines = 0;
    for (int l = 0; l < lines; l++) {
        int base = s_strip[0][wrap(r_top[0] + PAYLINE[l][0])];
        if (base == SY_WILD) {
            int s1 = s_strip[1][wrap(r_top[1] + PAYLINE[l][1])];
            int s2 = s_strip[2][wrap(r_top[2] + PAYLINE[l][2])];
            base = (s1 != SY_WILD) ? s1 : (s2 != SY_WILD ? s2 : SY_WILD);
        }
        int run = 0;
        for (int r = 0; r < NREEL; r++) {
            int sv = s_strip[r][wrap(r_top[r] + PAYLINE[l][r])];
            if (sv == base || sv == SY_WILD) run++; else break;
        }
        int mul = paymul(base, run);
        if (mul > 0) {
            total += mul * bpl;
            s_win_lines[s_win_nlines] = l; s_win_run[s_win_nlines] = run; s_win_nlines++;
            if (base == SY_SEVEN || base == SY_WILD) jackpotish = true;
        }
    }
    if (total > 0) {
        g_balance += total;
        s_win_total = total;
        int ratio = total / (bpl > 0 ? bpl : 1);
        int tier = (jackpotish || ratio >= 60) ? 2 : (ratio >= 15 ? 1 : 0);
        s_win_tier = tier;
        s_win_ms = (tier == 2) ? 2200 : (tier == 1 ? 1300 : 800);   // HOLD time after the finale fires
        if (total > g_best_win) g_best_win = total;
        s_win_shown = 0; s_win_step_ms = 140; s_finale_ms = 0; s_finale_done = false;
        s_pp = PP_WIN;                                              // poll() now drives the reveal sequence
        if (tier >= 1) cfg_write();                                // persist only notable wins (spare the SD; on_exit always saves)
    } else {
        // near-miss earcon: two sevens/wilds on any active line but the third missed
        bool near = false;
        for (int l = 0; l < lines && !near; l++) {
            int c = 0;
            for (int r = 0; r < NREEL; r++) { int sv = s_strip[r][wrap(r_top[r] + PAYLINE[l][r])]; if (sv == SY_SEVEN || sv == SY_WILD) c++; }
            if (c == 2) near = true;
        }
        sfx(near ? 14 : 13);
        s_pp = PP_IDLE;
        if (g_balance < total_bet()) cfg_write();                                     // persist a drained balance
        if (s_auto) s_auto_ms = 500;
    }
    nucleo_app_request_draw();
}

// ============================ screens: menu/help/set/scores/name/bonus =======
// menu item baseline (size-2 rows); the capsule eases to MENU_Y(sel)
#define MENU_Y0 54
#define MENU_DY 23
static int menu_item_y(int i) { return MENU_Y0 + i * MENU_DY; }
static void draw_menu(void)
{
    draw_felt();
    bulbs(10, W - 10, 6, (s_anim / 6) & 1);
    // flanking decorative symbols that cycle, for slot-machine flavour
    draw_sym(22, 22, 11, (int)((s_anim / 16) % NSYM), 30 + (int)(30 * sinf(s_anim * 0.25f)));
    draw_sym(W - 22, 22, 11, (int)((s_anim / 16 + 4) % NSYM), 30 + (int)(30 * cosf(s_anim * 0.25f)));
    const char *title = "SLOT";
    int lw = (int)strlen(title) * 18;
    text_at((W - lw) / 2 + 1, 11, 3, COL_GOLDD, title);
    text_at((W - lw) / 2, 10, 3, ((s_anim >> 3) & 1) ? COL_GOLDL : COL_GOLD, title);
    char bb[28]; snprintf(bb, sizeof bb, "%s %d", tx("Crediti", "Credits"), g_balance);
    text_c(W / 2, 36, 1, COL_CREAM, bb);

    const char *items[3] = { tx("Gioca", "Play"), tx("Come si gioca", "How to play"), tx("Impostazioni", "Settings") };
    int selw = (int)strlen(items[s_msel]) * 12;
    d.fillRoundRect((W - selw) / 2 - 10, (int)s_capY - 1, selw + 20, 17, 5, mix(COL_GLASS, COL_GOLD, 50));
    d.fillRoundRect((W - selw) / 2 - 6, (int)s_capY + 2, 3, 11, 1, COL_GOLDL);          // focus rail
    for (int i = 0; i < 3; i++) text_c(W / 2, menu_item_y(i), 2, i == s_msel ? COL_WHITE : COL_GREY, items[i]);
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    draw_felt();
    const char *titles[3] = { tx("Obiettivo", "Objective"), tx("Comandi", "Controls"), tx("Simboli", "Symbols") };
    text_c(W / 2, 6, 2, COL_GOLD, titles[s_help]);
    d.drawFastHLine(20, 26, W - 40, COL_GOLDD);
    if (s_help == 0) {
        text_at(14, 36, 1, COL_CREAM, tx("Allinea 3 simboli uguali su", "Match 3 equal symbols on an"));
        text_at(14, 50, 1, COL_CREAM, tx("una linea attiva per vincere.", "active line to win."));
        text_at(14, 68, 1, COL_GOLD, tx("Il JOLLY (W) sostituisce tutti.", "The WILD (W) substitutes all."));
        text_at(14, 86, 1, COL_RED, tx("Tre 7 = JACKPOT!", "Three 7s = JACKPOT!"));
    } else if (s_help == 1) {
        text_at(14, 36, 1, COL_GREY, tx("SPAZIO / INVIO", "SPACE / ENTER"));
        text_at(140, 36, 1, COL_GOLDL, tx("gira", "spin"));
        text_at(14, 52, 1, COL_GREY, "TAB");
        text_at(140, 52, 1, COL_CYAN, tx("puntate", "bets"));
        text_at(14, 68, 1, COL_GREY, tx("SU/W  GIU/E", "UP/W  DN/E"));
        text_at(140, 68, 1, COL_GREEN, tx("punta +/-", "bet +/-"));
        text_at(14, 84, 1, COL_GREY, tx("SX/DX", "LEFT/RIGHT"));
        text_at(140, 84, 1, COL_GREEN, tx("linee -/+", "lines -/+"));
        text_at(14, 100, 1, COL_GREY, "A");
        text_at(140, 100, 1, COL_ORANGE, tx("auto", "auto"));
    } else {
        // paytable: mini symbol + 3x payout
        int order[8] = { SY_WILD, SY_SEVEN, SY_DIAMOND, SY_BAR, SY_BELL, SY_PLUM, SY_LEMON, SY_CHERRY };
        for (int i = 0; i < 8; i++) {
            int col = i / 4, row = i % 4;
            int x = 24 + col * 120, y = 36 + row * 18;
            draw_sym(x, y + 6, 8, order[i], 0);
            char b[12]; snprintf(b, sizeof b, "x%d", paymul(order[i], 3));
            text_at(x + 16, y + 2, 1, order[i] == SY_SEVEN || order[i] == SY_WILD ? COL_GOLDL : COL_CREAM, b);
        }
    }
    int gap = 12, x0 = W / 2 - gap;
    for (int i = 0; i < 3; i++) d.fillCircle(x0 + i * gap, ch - 8, i == s_help ? 3 : 2, i == s_help ? COL_GOLD : COL_DIM);
}
static void draw_settings(void)
{
    draw_felt();
    text_c(W / 2, 8, 2, COL_GOLD, tx("Impostazioni", "Settings"));
    const char *names[5] = { tx("Lingua", "Language"), "Audio", tx("Giro veloce", "Turbo spin"), tx("Azzera crediti", "Reset credits"), tx("Indietro", "Back") };
    char v[3][16];
    snprintf(v[0], 16, "%s", g_lang ? "English" : "Italiano");
    snprintf(v[1], 16, "%s", g_audio ? "On" : "Off");
    snprintf(v[2], 16, "%s", g_turbo ? "On" : "Off");
    d.fillRoundRect(16, (int)s_capY, W - 32, 18, 5, mix(COL_GLASS, COL_GOLD, 50));
    for (int i = 0; i < 5; i++) {
        int y = 30 + i * 18;
        text_at(26, y, 2, i == s_setsel ? COL_WHITE : COL_GREY, names[i]);
        if (i < 3) text_r(W - 18, y + 4, 1, COL_GOLD, v[i]);
    }
}
// Out of credits: the game is over. A keypress restarts a fresh game with the base balance.
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    draw_felt();
    star(W / 2, 26, 13, 5, COL_GOLD);
    text_c(W / 2, 16, 2, COL_RED, tx("CREDITI FINITI", "OUT OF CREDITS"));
    text_c(W / 2, 48, 1, COL_CREAM, tx("Partita terminata", "Game over"));
    char b[40]; snprintf(b, sizeof b, "%s %d %s", tx("Riparti con", "Restart with"), START_CREDITS, tx("crediti", "credits"));
    text_c(W / 2, 64, 1, COL_GOLD, b);
    text_c(W / 2, ch - 14, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_GOLD, tx("INVIO per ricominciare", "ENTER to restart"));
}

// ============================ screen: play ===================================
static void draw_win_overlay(void)
{
    // winning paylines — revealed progressively (s_linewin flags grow as each line is shown)
    int bright = 120 + (int)(120 * sinf(s_anim * 0.5f));
    for (int l = 0; l < 5; l++) {
        if (!s_linewin[l]) continue;
        uint16_t lc = mix(LINECOL[l], COL_WHITE, bright);
        int px = -1, py = -1;
        for (int r = 0; r < NREEL; r++) {
            int cx = GRID_X + r * (REEL_W + REEL_GAP) + REEL_W / 2 + s_shx;
            int cy = GRID_Y + PAYLINE[l][r] * CELL_H + CELL_H / 2 + s_shy;
            if (px >= 0) { d.drawLine(px, py, cx, cy, lc); d.drawLine(px, py + 1, cx, cy + 1, lc); }
            px = cx; py = cy;
        }
    }
    if (!s_finale_done) return;                                                        // payoff text appears on the finale, after the reveal
    int bxc = W / 2 + s_shx;
    if (s_win_tier == 0) {                                                             // SMALL: a modest "+N" that floats up — no banner
        char b[16]; snprintf(b, sizeof b, "+%d", s_win_total);
        int rise = s_fin_age < 600 ? s_fin_age / 55 : 10;
        text_c_sh(bxc, GRID_Y + GRID_H / 2 - 4 - rise, 2, COL_GOLDL, b);
        return;
    }
    // BIG / JACKPOT: the banner pops in (box grows with an ease-out), text + total snap in once wide enough
    const char *bw = s_win_tier == 2 ? "JACKPOT!" : tx("BELLA VINCITA!", "BIG WIN!");
    float kf = s_fin_age < 180 ? (float)s_fin_age / 180 : 1.0f;
    kf = 1.0f - (1.0f - kf) * (1.0f - kf);
    int fullw = (int)strlen(bw) * 12 + 16, hh = (int)(24 * (0.5f + 0.5f * kf));
    int wpx = (int)(fullw * (0.35f + 0.65f * kf));
    int by = GRID_Y + GRID_H / 2 - hh / 2 + s_shy;
    uint16_t bc = s_win_tier == 2 ? (((s_anim >> 1) & 1) ? COL_RED : COL_GOLD) : COL_GOLD;
    d.fillRoundRect(bxc - wpx / 2, by, wpx, hh, 5, mix(COL_DARK, bc, 90));
    d.drawRoundRect(bxc - wpx / 2, by, wpx, hh, 5, COL_GOLDL);
    if (kf > 0.6f) {
        text_c_sh(bxc, by + hh / 2 - 8, 2, COL_WHITE, bw);
        char b[20]; snprintf(b, sizeof b, "+%d", s_win_total);
        text_c_sh(bxc, by + hh + 1, 1, COL_GOLDL, b);
    }
}
// rotating golden light rays behind the machine on a big/mega win
static void draw_rays(void)
{
    int cx = W / 2, cy = GRID_Y + GRID_H / 2, R = 220;
    float rot = s_anim * 0.05f;
    for (int i = 0; i < 12; i += 2) {
        float a0 = rot + i * 0.5235988f, a1 = a0 + 0.32f;
        d.fillTriangle(cx, cy, cx + (int)(cosf(a0) * R), cy + (int)(sinf(a0) * R),
                       cx + (int)(cosf(a1) * R), cy + (int)(sinf(a1) * R), mix(COL_FELT, COL_GOLD, 55));
    }
}
static void draw_play(void)
{
    s_shx = s_shy = 0;
    if (s_shake_ms > 0) {                                                              // mega-win screen shake
        int m = s_shake_ms > 350 ? 3 : 2;
        s_shx = (int)((s_anim * 37) % (2 * m + 1)) - m;
        s_shy = (int)((s_anim * 53) % (2 * m + 1)) - m;
    }
    draw_felt();
    if (s_pp == PP_WIN && s_finale_done && s_win_tier >= 1) draw_rays();
    draw_cabinet();
    draw_reels();
    draw_hud();
    coins_draw();
    if (s_pp == PP_WIN && s_win_tier == 2) fw_draw();                                   // jackpot-only fireworks
    if (s_flash_ms > 0) { int ch = nucleo_app_content_height(); d.fillRect(0, 0, W, ch, mix(COL_FELT, COL_WHITE, s_flash_ms * 256 / 200)); }   // jackpot white burst
    if (s_pp == PP_WIN && s_finale_done && s_win_tier >= 1) {                           // pulsing win frame: steady gold (big) / colour-cycling (jackpot)
        int ch = nucleo_app_content_height();
        int g = 110 + (int)(120 * sinf(s_anim * 0.4f));
        uint16_t gc = s_win_tier == 2 ? LINECOL[(s_anim / 6) % 5] : COL_GOLD;
        for (int k = 0; k < 3; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_FELT, gc, g - k * 30));
    }
    if (s_pp == PP_WIN) draw_win_overlay();
    if (s_msg_ms > 0) {
        int wpx = (int)strlen(s_msg) * 6 + 14;
        d.fillRoundRect(W / 2 - wpx / 2, GRID_Y + GRID_H / 2 - 6, wpx, 14, 3, COL_REDD);
        text_c(W / 2, GRID_Y + GRID_H / 2 - 3, 1, COL_WHITE, s_msg);
    }
}

// ============================ screen: bet panel ==============================
static void draw_bet(void)
{
    draw_play();                                                                       // live slot stays visible around the panel edges
    int ch = nucleo_app_content_height();
    int px = 22, py = 8, pw = W - 44, phh = ch - 14;                                    // panel 8..114
    d.fillRoundRect(px, py, pw, phh, 7, COL_PANEL);
    d.drawRoundRect(px, py, pw, phh, 7, COL_GOLD);
    bulbs(px + 8, px + pw - 8, py + 5, (s_anim / 6) & 1);
    text_c(W / 2, py + 10, 2, COL_GOLD, tx("PUNTATE", "BET SETUP"));

    const char *names[NBET] = { tx("Linee", "Lines"), tx("Punt./linea", "Bet/line"), "Auto", "Turbo" };
    char vals[NBET][12];
    snprintf(vals[0], 12, "%d", active_lines());
    snprintf(vals[1], 12, "%d", BET_OPT[g_bet_idx]);
    snprintf(vals[2], 12, "%s", s_auto ? "On" : "Off");
    snprintf(vals[3], 12, "%s", g_turbo ? "On" : "Off");
    int y0 = py + 30;
    for (int i = 0; i < NBET; i++) {
        int y = y0 + i * 16;
        if (i == s_betsel) d.fillRoundRect(px + 6, y - 3, pw - 12, 16, 4, mix(COL_PANEL, COL_GOLD, 60));
        text_at(px + 14, y, 1, i == s_betsel ? COL_WHITE : COL_GREY, names[i]);
        text_r(px + pw - 30, y, 1, COL_GOLDL, vals[i]);
        if (i == s_betsel) { text_at(px + pw - 24, y, 1, COL_GOLD, "<"); text_at(px + pw - 12, y, 1, COL_GOLD, ">"); }
    }
    char tb[24]; snprintf(tb, sizeof tb, "%s %d", tx("Tot", "Total"), total_bet());
    text_c(W / 2, py + phh - 16, 2, COL_GOLDL, tb);
}

// ============================ input + hint ===================================
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU:   nucleo_app_set_hint(tx("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_HELP:   nucleo_app_set_hint(tx("SX/DX pagine  Esc indietro", "LEFT/RIGHT pages  Esc back")); break;
        case ST_SET:    nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  Esc", "UP/DN  ENTER change  Esc")); break;
        case ST_PLAY:   nucleo_app_set_hint(tx("SPAZIO gira/ferma   TAB puntate   W/E punta", "SPACE spin/stop   TAB bets   W/E bet")); break;
        case ST_BET:    nucleo_app_set_hint(tx("SU/GIU  SX/DX cambia  TAB chiudi", "UP/DN  LEFT/RIGHT change  TAB close")); break;
        case ST_OVER:   nucleo_app_set_hint(tx("INVIO per ricominciare", "ENTER to restart")); break;
        default: break;
    }
}
static float cap_target(void)
{
    if (s_screen == ST_MENU) return (float)menu_item_y(s_msel);
    if (s_screen == ST_SET)  return (float)(28 + s_setsel * 18);
    return s_capY;
}
static void go(int s)
{
    s_screen = s;
    s_capY = cap_target();
    set_hint();
    nucleo_app_request_draw();
}

static void tab_handler(void)
{
    if (s_screen == ST_PLAY) { s_betsel = 0; go(ST_BET); sfx(2); }
    else if (s_screen == ST_BET) { go(ST_PLAY); sfx(3); }
}
static void bet_change(int dir)
{
    switch (s_betsel) {
        case 0: g_lines_idx = (g_lines_idx + 3 + dir) % 3; sfx(4); break;
        case 1: g_bet_idx = (g_bet_idx + 5 + dir) % 5; sfx(4); break;
        case 2: s_auto = !s_auto; sfx(2); break;
        case 3: g_turbo = !g_turbo; sfx(2); break;
        default: break;
    }
    cfg_write();
    nucleo_app_request_draw();
}
static void menu_select(void)
{
    sfx(2);
    if (s_msel == 0) { s_pp = PP_IDLE; s_disp_bal = g_balance; go(ST_PLAY); }
    else if (s_msel == 1) { s_help = 0; go(ST_HELP); }
    else { s_setsel = 0; go(ST_SET); }
}
static void settings_change(void)
{
    switch (s_setsel) {
        case 0: g_lang ^= 1; cfg_write(); set_hint(); sfx(2); break;
        case 1: g_audio ^= 1; cfg_write(); sfx(2); break;
        case 2: g_turbo ^= 1; cfg_write(); sfx(2); break;
        case 3: g_balance = START_CREDITS; s_disp_bal = START_CREDITS; cfg_write(); sfx(15); break;
        default: sfx(3); s_msel = 2; go(ST_MENU); return;
    }
    nucleo_app_request_draw();
}
static void on_key(int k, char ch)
{
    switch (s_screen) {
        case ST_MENU:
            if (k == NK_UP)        { s_msel = (s_msel + 2) % 3; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % 3; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) menu_select();
            return;
        case ST_HELP:
            if (k == NK_RIGHT)      { s_help = (s_help + 1) % 3; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) { sfx(3); go(ST_MENU); }
            return;
        case ST_SET:
            if (k == NK_UP)        { s_setsel = (s_setsel + 4) % 5; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % 5; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change();
            return;
        case ST_PLAY:
            if (s_pp == PP_IDLE) {
                if (ch == ' ' || k == NK_ENTER)               do_spin();
                else if (k == NK_UP || ch == 'w' || ch == 'W') { g_bet_idx = (g_bet_idx + 1) % 5; cfg_write(); sfx(4); nucleo_app_request_draw(); }
                else if (k == NK_DOWN || ch == 'e' || ch == 'E') { g_bet_idx = (g_bet_idx + 4) % 5; cfg_write(); sfx(4); nucleo_app_request_draw(); }
                else if (k == NK_RIGHT)                        { g_lines_idx = (g_lines_idx + 1) % 3; cfg_write(); sfx(4); nucleo_app_request_draw(); }
                else if (ch == 'a' || ch == 'A')              { s_auto = !s_auto; if (s_auto) { s_auto_ms = 200; } sfx(2); nucleo_app_request_draw(); }
            } else if (s_pp == PP_SPIN) {
                if (ch == ' ' || k == NK_ENTER) quick_stop();                          // slam-stop the reels
            }
            return;
        case ST_BET:
            if (k == NK_UP)        { s_betsel = (s_betsel + NBET - 1) % NBET; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_betsel = (s_betsel + 1) % NBET; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_RIGHT || k == NK_ENTER) bet_change(+1);
            return;
        case ST_OVER:
            if (k == NK_ENTER || ch == ' ') { g_balance = START_CREDITS; s_disp_bal = g_balance; cfg_write(); sfx(15); coins_spawn(16, 1); go(ST_PLAY); }
            return;
        default: return;
    }
}
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_HELP: s_help = (s_help + 2) % 3; sfx(1); nucleo_app_request_draw(); return true;
            case ST_SET:  settings_change(); return true;
            case ST_PLAY: if (s_pp == PP_IDLE) { g_lines_idx = (g_lines_idx + 2) % 3; cfg_write(); sfx(4); nucleo_app_request_draw(); } return true;
            case ST_BET:  bet_change(-1); return true;
            default: break;
        }
    }
    switch (s_screen) {
        case ST_MENU: return false;                                                    // Esc on menu -> close app
        case ST_BET:  go(ST_PLAY); sfx(3); return true;
        default: sfx(3); s_msel = 0; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void step_reels(int dt)
{
    const float SPD = 0.034f;                                                          // symbols per ms
    const int   STOP_MS = s_quickstop ? 150 : (g_turbo ? 240 : 360), BOUNCE_MS = 140, TRAVEL = 5;
    const float BOUNCE_AMP = 0.16f;
    s_spin_ms += dt;
    for (int r = 0; r < NREEL; r++) {
        if (r_phase[r] == 1) {                                                          // spinning
            r_pos[r] += SPD * dt;
            // last reel anticipation: decide once, from reels 0,1 already-known results
            if (r == NREEL - 1 && !s_antic_checked && s_spin_ms >= r_delay[r]) {
                s_antic_checked = true;
                if (antic_possible()) { r_delay[r] += g_turbo ? 450 : 900; s_antic = true; sfx(24); }   // drumroll build
            }
            if (s_spin_ms >= r_delay[r]) {
                r_start[r] = r_pos[r];
                int land = (int)ceilf(r_pos[r]) + TRAVEL;
                r_target[r] = (float)land;
                r_top[r] = wrap(land);
                r_phase[r] = 2; r_t[r] = 0;
            }
        } else if (r_phase[r] == 2) {                                                   // easing to stop
            r_t[r] += dt;
            float u = (float)r_t[r] / STOP_MS;
            if (u >= 1.0f) {
                r_pos[r] = r_target[r]; r_phase[r] = 3; r_t[r] = 0; r_flash_ms[r] = 150;
                bool hi = false;
                for (int l = 0; l < active_lines(); l++) { int sv = s_strip[r][wrap(r_top[r] + PAYLINE[l][r])]; if (sv == SY_SEVEN || sv == SY_WILD) { hi = true; break; } }
                sfx(hi ? 23 : (6 + r));                                                 // sparkle when a 7/WILD lands, else mechanical clunk
            }
            else { float f = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u); r_pos[r] = r_start[r] + (r_target[r] - r_start[r]) * f; }
        } else if (r_phase[r] == 3) {                                                   // settle bounce
            r_t[r] += dt;
            float u = (float)r_t[r] / BOUNCE_MS;
            if (u >= 1.0f) { r_pos[r] = r_target[r]; r_phase[r] = 0; }
            else r_pos[r] = r_target[r] + sinf((float)M_PI * u) * BOUNCE_AMP * (1.0f - u);
        }
    }
    bool all_done = true;
    for (int r = 0; r < NREEL; r++) if (r_phase[r] != 0) all_done = false;
    if (all_done) evaluate();
}
static void on_draw(void)
{
    switch (s_screen) {
        case ST_MENU:   draw_menu(); break;
        case ST_HELP:   draw_help(); break;
        case ST_SET:    draw_settings(); break;
        case ST_PLAY:   draw_play(); break;
        case ST_BET:    draw_bet(); break;
        case ST_OVER:   draw_over(); coins_draw(); break;
        default: break;
    }
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last);
    if (dt < 0) dt = 0;
    if (dt > 200) dt = 200;
    s_last = s_now;

    // eased credit count-up/down (the "rolling" balance)
    s_disp_bal += ((float)g_balance - s_disp_bal) * 0.18f;
    if (fabsf((float)g_balance - s_disp_bal) < 0.5f) s_disp_bal = g_balance;

    if (s_msg_ms > 0) dec(&s_msg_ms, dt);
    coins_step(dt);
    fw_step(dt);
    for (int r = 0; r < NREEL; r++) dec(&r_flash_ms[r], dt);

    if (s_screen == ST_PLAY || s_screen == ST_BET) {
        if (s_pp == PP_SPIN) step_reels(dt);
        else if (s_pp == PP_WIN) {
            dec(&s_shake_ms, dt); dec(&s_flash_ms, dt);
            if (s_win_shown < s_win_nlines) {                          // reveal one winning line at a time
                s_win_step_ms -= dt;
                if (s_win_step_ms <= 0) {
                    win_reveal(s_win_shown);
                    s_win_shown++;
                    s_win_step_ms = REVEAL_MS;
                    if (s_win_shown >= s_win_nlines) s_finale_ms = 240;
                }
            } else if (!s_finale_done) {                              // a beat, then the payoff
                s_finale_ms -= dt;
                if (s_finale_ms <= 0) { win_finale(); s_finale_done = true; }
            } else {
                s_fin_age += dt;
                if (s_coinwave_ms > 0) { int pv = s_coinwave_ms; s_coinwave_ms -= dt; if (s_coinwave_ms / 150 != pv / 150) coins_spawn(4, 2); }   // jackpot fountain
                if (s_win_tier == 2) { s_fw_ms -= dt; if (s_fw_ms <= 0) { fw_spawn(); s_fw_ms = 260; } }                                          // jackpot fireworks
                dec(&s_win_ms, dt);
                if (s_win_ms == 0) { s_pp = PP_IDLE; if (s_auto) s_auto_ms = 500; }   // keep playing — a win never ends the game
            }
        } else if (s_pp == PP_IDLE && s_screen == ST_PLAY && s_auto && s_auto_ms > 0) {
            dec(&s_auto_ms, dt);
            if (s_auto_ms == 0) do_spin();
        }
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        s_capY += (cap_target() - s_capY) * 0.45f;
    }

    // HELP and SCORES are fully static (no s_anim/timer): don't re-composite them every frame.
    // Key presses there call nucleo_app_request_draw() for a one-shot redraw (mirrors app_reactor).
    if (s_screen == ST_HELP) return false;
    if (s_now - s_frame < 33) return false;
    s_frame = s_now;
    s_anim++;
    return true;
}
static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    build_strips();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    sfx_cache_check();
    presynth();
    memset(s_coin, 0, sizeof s_coin);
    memset(s_fw, 0, sizeof s_fw);
    reset_win_marks();
    for (int r = 0; r < NREEL; r++) { r_phase[r] = 0; r_top[r] = (int)(esp_random() % SLEN); r_pos[r] = r_top[r]; r_flash_ms[r] = 0; }
    s_win_nlines = 0; s_win_shown = 0; s_finale_done = false; s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_shx = s_shy = 0;
    s_quickstop = false; s_fin_age = 0;
    s_pp = PP_IDLE; s_auto = false; s_auto_ms = 0; s_msg_ms = 0;
    s_disp_bal = g_balance;
    s_screen = ST_MENU; s_msel = 0; s_anim = 0;
    s_capY = cap_target();
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);
    set_hint();
    sfx(2);
    nucleo_app_request_draw();
}
static void on_exit(void) { nucleo_audio_stop(); cfg_write(); }

extern "C" void nucleo_register_slots(void)
{
    static const nucleo_app_def_t app = {
        "slots", "Slot", "Games", "Slot machine: gira e vinci il jackpot",
        '7', C_YELLOW, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP   // dedicate RAM + free the shared I2S/mic line so the chiptune SFX reliably play
    };
    nucleo_app_register(&app);
}
