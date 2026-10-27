// app_slots.cpp — NucleoOS "Slot": a 3x3 fruit machine with 5 paylines (category "Games").
//
// A gold cabinet with three reels that spin with motion blur and stop left-to-right with a weighted
// bounce, a last-reel ANTICIPATION (drumroll + spotlight when two high symbols already lined up), the
// winning lines revealed one by one with rising chimes, a coin shower, a counting-up credit readout and
// tiered finales (small / big / JACKPOT with fireworks). It plays like a real slot: a win just pays and
// you keep spinning; the game ends when the credits run out, then ENTER restarts with the base bank.
//
// FAIR: like a real machine the result is drawn from the hardware RNG when the spin starts (each reel
// stop uniform over its 30-symbol strip); the animation and the slam-stop only reveal it. A spin left
// half-way (Esc, closing the app) is settled, never swallowed. Paytable return ~91% (5 lines).
//
// Console conventions (game_ui): title menu, three help pages (incl. the paytable), one settings list
// (from the menu, or TAB at the machine), Esc at the machine pauses and a second Esc leaves. Text in five
// languages (GT). Sound: the PC-rendered pack in /sd/data/slots/pack, never synthesized on the device.
// Never name a local `d` (it is the display macro).

#include "nucleo_app.h"
#include "nucleo_touch.h"       // tap the bet / AUTO labels
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "nucleo_exclusive.h"   // NX_NET_APP: dedicate RAM + free the shared I2S line so SFX play
#include "game_text.h"
#include "game_ui.h"
#include "app_large.h"          // two screens: the cabinet on the 320x240 panel, the Cardputer as companion
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

// ============================ palette (RGB332-exact: the canvas is 8bpp) =====
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t COL_FELT = RGB(0, 73, 36), COL_DARK = RGB(0, 36, 0), COL_GOLD = RGB(255, 182, 85),
    COL_GOLDD = RGB(146, 109, 0), COL_GOLDL = RGB(255, 219, 170), COL_WHITE = 0xFFFF, COL_CREAM = RGB(255, 255, 170),
    COL_RED = RGB(255, 73, 85), COL_REDD = RGB(146, 36, 0), COL_CYAN = RGB(109, 219, 255), COL_GREEN = RGB(146, 255, 170),
    COL_PURPLE = RGB(182, 109, 255), COL_YELLOW = RGB(255, 219, 85), COL_ORANGE = RGB(255, 146, 85), COL_PINK = RGB(255, 109, 170),
    COL_GREY = RGB(146, 146, 170), COL_GLASS = RGB(0, 0, 85), COL_GLASS2 = RGB(36, 36, 85), COL_GLASS3 = RGB(36, 73, 85);
static inline uint16_t mix(uint16_t a, uint16_t b, int t) { return gui::mix(a, b, t); }

// ============================ geometry =======================================
// The view: the compact 240-wide frame, or the native 320x240 panel (view_set(true), two screens). The painters
// read W, the content height and the reel geometry from here, so one set draws both screens at their own size.
static bool s_big;
static int  s_vw = 240, s_vh = 119, s_reelw = 62, s_reelgap = 7, s_cellh = 33, s_gridy = 18, s_syms = 15, s_gridx = 20;
#define W      s_vw
#define NREEL 3
#define NROW  3
#define SLEN  30
#define REEL_W   s_reelw
#define REEL_GAP s_reelgap
#define GRID_W (NREEL * REEL_W + (NREEL - 1) * REEL_GAP)   // 200 (panel 238)
#define GRID_X s_gridx                                     // 20 (panel 30: the lever stands on the right)
#define CELL_H s_cellh
#define GRID_Y s_gridy
#define GRID_H (NROW * CELL_H)                             // 99  -> window 18..117 (fills down to the system footer)
#define SYM_S  s_syms                                      // symbol half-size (px)
static void view_set(bool big)
{
    s_big = big; s_vw = big ? 320 : 240; s_vh = big ? 240 : nucleo_app_content_height();
    s_reelw = big ? 74 : 62; s_reelgap = big ? 8 : 7; s_cellh = big ? 44 : 33; s_gridy = big ? 40 : 18; s_syms = big ? 19 : 15;
    s_gridx = big ? 30 : 20;
}

// ============================ symbols / paytable =============================
enum { SY_CHERRY = 0, SY_LEMON, SY_PLUM, SY_BELL, SY_BAR, SY_DIAMOND, SY_SEVEN, SY_WILD, NSYM };
// per-reel frequency (sums to SLEN = 30): sevens & wilds are rare
static const uint8_t SYM_W[NSYM] = { 6, 6, 5, 4, 4, 3, 1, 1 };
// 3-of-a-kind multiplier of the per-line bet (a WILD stands in for anything); two cherries pay CHERRY2
static const int16_t PAY3[NSYM] = { 5, 8, 12, 20, 40, 80, 200, 500 };
#define CHERRY2 3
static int paymul(int sym, int run) { return run >= 3 ? PAY3[sym] : (run == 2 && sym == SY_CHERRY) ? CHERRY2 : 0; }
static uint16_t sym_color(int id)
{
    static const uint16_t C[NSYM] = { COL_RED, COL_YELLOW, COL_PURPLE, COL_GOLD, COL_CYAN, COL_CYAN, COL_RED, COL_GOLD };
    return C[id];
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

// ============================ state ==========================================
enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OVER };
#define START_CREDITS 1000   // fresh-game balance (restart amount when the credits run out)
enum { PP_IDLE = 0, PP_SPIN, PP_WIN };   // PLAY sub-phase (the spin/win lifecycle)

static int   g_audio = 1, g_turbo = 0;
static int   g_balance = START_CREDITS, g_best_win = 0;
static int   g_lines_idx = 2, g_bet_idx = 1;   // default: 5 lines, 2/line

static int   s_screen, s_set_from;              // s_set_from: the screen the settings return to
static bool  s_paused, s_confirm;               // pause card at the machine / "reset credits?" card
static gui::Menu s_menu, s_setm;
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;
static int   s_help;
static int   s_msg_ms; static char s_msg[28];   // transient toast (e.g. "bet auto-lowered")

// reels
static uint8_t (*s_strip)[SLEN];                  // NREEL rows, APP_RAM (rebuilt by build_strips() in on_enter)
static float r_pos[NREEL];                       // continuous top-index
static float r_start[NREEL], r_target[NREEL];
static int   r_phase[NREEL];                      // 0 done/idle, 1 spin, 2 stop, 3 bounce
static int   r_t[NREEL];
static int   r_delay[NREEL];
static int   r_top[NREEL];                         // strip index of the top row of the result (drawn at spin start)

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
static int   r_flash_ms[NREEL];                    // per-reel "just landed" white flash
static int   s_win_lines[5], s_win_run[5], s_win_nlines;   // winning lines collected by line_wins()
static int   s_win_shown, s_win_step_ms;           // how many revealed so far + countdown to next
static int   s_finale_ms;                          // beat before the finale fires (after last line shown)
static bool  s_finale_done;                        // finale (coin fountain + tier sound) has fired
static int   s_shake_ms, s_flash_ms, s_coinwave_ms;// mega screen-shake, white flash, jackpot coin waves
static int   s_shx, s_shy;                          // current shake offset (set each frame in draw_play)
static bool  s_quickstop;                           // player slammed the spin button to stop the reels early
static int   s_fin_age;                             // ms since the finale fired (drives banner pop + edge glow)

// coin particles + firework bursts (jackpot): APP_RAM, 0 B while the game is closed
#define NPART 40
#define NFW 5
struct Coin { float x, y, vx, vy; int life; uint16_t col; };
struct FW { int x, y, age; bool live; uint16_t col; };
struct Fx { Coin coin[NPART]; FW fw[NFW]; };
static Fx *s_fx;
static int s_fw_ms;                                  // mega: countdown to the next burst

static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static int   total_bet(void) { return LINES_OPT[g_lines_idx] * BET_OPT[g_bet_idx]; }
static int   active_lines(void) { return LINES_OPT[g_lines_idx]; }
static void  go(int s);
static void  on_key(int k, char ch);
static bool  on_back(int key);
static bool  lg_key(int k, char ch);

// ============================ persistence ====================================
#define DIRR "/sd/data/slots"
#define CFG_MAGIC 0x534C4F54u   // 'SLOT'
struct Cfg { uint32_t m; int lang_unused, a, t, bal, best, li, bi; };   // layout kept: old saves still load
static void cfg_write(void)
{
    mkdir("/sd/data", 0777); mkdir(DIRR, 0777);
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    Cfg c = { CFG_MAGIC, 0, g_audio, g_turbo, g_balance, g_best_win, g_lines_idx, g_bet_idx };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    Cfg c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) {
        g_audio = c.a ? 1 : 0; g_turbo = c.t ? 1 : 0;
        g_balance = c.bal < 0 ? 0 : c.bal; g_best_win = c.best < 0 ? 0 : c.best;
        g_lines_idx = (c.li < 0 || c.li > 2) ? 2 : c.li;
        g_bet_idx   = (c.bi < 0 || c.bi > 4) ? 1 : c.bi;
    }
}

// ============================ audio ==========================================
// The deployed pack (/sd/data/slots/pack, tools/sfx-gen). Never synthesized here (that blocked the UI task
// past the 8 s Task-WDT); a missing WAV plays a short tone instead.
enum { SFX_MOVE = 1, SFX_SEL, SFX_BACK, SFX_BET, SFX_SPIN, SFX_STOP1, SFX_NOWIN = SFX_STOP1 + 3, SFX_NEAR, SFX_BONUS,
       SFX_LN1, SFX_CASC = SFX_LN1 + 5, SFX_CHACH, SFX_SPRK, SFX_DRUM, SFX_JACK, NSFX = SFX_JACK };
static const char *sfx_name(int id)
{
    static const char *const N[NSFX + 1] = { "x", "move", "sel", "back", "bet", "spin", "stop1", "stop2", "stop3", "nowin", "near",
                                             "bonus", "ln1", "ln2", "ln3", "ln4", "ln5", "casc", "chach", "sprk", "drum", "jack" };
    return id > 0 && id <= NSFX ? N[id] : "x";
}
static const uint16_t SFX_HZ[NSFX + 1] = { 0, 760, 988, 392, 880, 330, 196, 220, 247, 196, 523, 1318, 523, 659, 784, 988, 1175, 1568, 1318, 2093, 98, 1046 };
static void sfx(int id)
{
    if (!g_audio || id <= 0 || id > NSFX) return;
    bool imp = id >= SFX_SPIN && id != SFX_NOWIN && id != SFX_NEAR;
    if (!imp && nucleo_audio_is_playing()) return;
    char p[48]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id));
    struct stat st;
    if (imp) nucleo_audio_stop();
    if (stat(p, &st) != 0 || st.st_size <= 44 || nucleo_audio_play(p) != ESP_OK) nucleo_audio_tone(SFX_HZ[id], 45, 55);
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
static int cell(int r, int row) { return s_strip[r][wrap(r_top[r] + row)]; }

// ============================ draw helpers ===================================
static int txt(const char *s, int x, int y, int datum, int font, uint16_t col) { return gui::text(s, x, y, datum, font, col, 0); }

static void star(int cx, int cy, int ro, int ri, uint16_t col)
{
    static const int8_t UX[10] = { 0, 59, 95, 95, 59, 0, -59, -95, -95, -59 }, UY[10] = { -100, -81, -31, 31, 81, 100, 81, 31, -31, -81 };   // x100
    for (int i = 0; i < 10; i++) {
        int j = (i + 1) % 10, ri0 = (i & 1) ? ri : ro, rj = (j & 1) ? ri : ro;
        d.fillTriangle(cx, cy, cx + UX[i] * ri0 / 100, cy + UY[i] * ri0 / 100, cx + UX[j] * rj / 100, cy + UY[j] * rj / 100, col);
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
            d.fillSmoothCircle(cx - s / 2, cy + s / 3, rr, COL_REDD);
            d.fillCircle(cx - s / 2, cy + s / 3, rr - 1, mix(COL_RED, COL_WHITE, glow));
            d.fillCircle(cx - s / 2 - rr / 3, cy + s / 3 - rr / 3, rr / 3, COL_CREAM);
            d.fillSmoothCircle(cx + s / 2, cy + s / 3 + 1, rr, COL_REDD);
            d.fillCircle(cx + s / 2, cy + s / 3 + 1, rr - 1, mix(COL_RED, COL_REDD, 80));
            d.fillCircle(cx + s / 2 - rr / 3, cy + s / 3 + 1 - rr / 3, rr / 3, COL_CREAM);
            break;
        }
        case SY_LEMON:
            d.fillEllipse(cx, cy, s, s * 3 / 4, COL_GOLDD);
            d.fillEllipse(cx, cy, s - 1, s * 3 / 4 - 1, hl);
            d.fillEllipse(cx - s / 3, cy - s / 4, s / 3, s / 5, COL_CREAM);          // sheen
            d.fillCircle(cx + s - 1, cy, 1, COL_GREEN);                              // nib
            break;
        case SY_PLUM:
            d.fillSmoothCircle(cx, cy + 1, s - 1, mix(COL_PURPLE, COL_DARK, 110));
            d.fillCircle(cx, cy + 1, s - 2, hl);
            d.fillCircle(cx - s / 3, cy - s / 3 + 1, s / 3, mix(COL_PURPLE, COL_WHITE, 120 + glow / 2)); // sheen
            d.drawLine(cx, cy - s + 1, cx + 2, cy - s + 4, COL_GREEN);
            d.fillTriangle(cx + 1, cy - s + 2, cx + s / 2 + 2, cy - s + 1, cx + s - 3, cy - s / 2 + 1, COL_GREEN);
            break;
        case SY_BELL:
            d.fillTriangle(cx, cy - s + 2, cx - s + 1, cy + s - 3, cx + s - 1, cy + s - 3, COL_GOLDD);   // body
            d.fillTriangle(cx, cy - s + 3, cx - s + 3, cy + s - 4, cx + s - 3, cy + s - 4, hl);
            d.fillRoundRect(cx - s + 1, cy + s - 5, 2 * s - 2, 4, 2, COL_GOLDD);    // rim
            d.fillRoundRect(cx - s + 2, cy + s - 5, 2 * s - 4, 2, 1, COL_GOLDL);
            d.fillCircle(cx, cy - s + 1, 2, COL_GOLDL);                              // knob
            d.fillCircle(cx, cy + s - 1, 2, COL_GOLDD);                              // clapper
            d.drawLine(cx - s / 3, cy - s + 6, cx - s / 2, cy + s - 6, COL_CREAM);   // shine
            break;
        case SY_BAR:
            d.fillRoundRect(cx - s, cy - s / 2 - 1, 2 * s, s + 2, 3, COL_GOLDD);
            d.fillRoundRect(cx - s + 1, cy - s / 2, 2 * s - 2, s, 3, mix(COL_CYAN, COL_WHITE, glow / 2));
            d.setTextColor(COL_DARK); d.setCursor(cx - 8, cy - 3); d.print("BAR");
            break;
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
                d.fillRect(cx - s + 2 + o, cy - s + 1 + o, 2 * s - 4, 4, cc);         // top bar
                d.fillTriangle(cx + s - 8 + o, cy - s + 1 + o, cx + s - 2 + o, cy - s + 1 + o, cx + 2 + o, cy + s + o, cc);
                d.fillTriangle(cx + s - 8 + o, cy - s + 1 + o, cx + 2 + o, cy + s + o, cx - 4 + o, cy + s + o, cc);
            }
            d.drawLine(cx - s + 3, cy - s + 1, cx + s - 4, cy - s + 1, COL_GOLDL);    // gold gleam on the bar
            break;
        }
        default:                                                                      // WILD: a gold star
            star(cx, cy, s, s * 2 / 5, COL_GOLDD);
            star(cx, cy, s - 1, s * 2 / 5, mix(COL_GOLD, COL_WHITE, glow));
            d.fillSmoothCircle(cx, cy, s / 3 + 1, mix(COL_REDD, COL_RED, 120));
            d.setTextColor(COL_CREAM); d.setCursor(cx - 2, cy - 3); d.print("W");
            break;
    }
}

// ============================ background / cabinet ===========================
static void draw_felt(void) { gui::vgradient(0, 0, W, s_vh, RGB(0, 109, 36), COL_DARK); }
// Where payline tab l sits: lines 1-3 on the left at their rows, 4-5 on the right where they end.
static void tab_xy(int l, int *x, int *y)
{
    const int right = l >= 3, row = right ? PAYLINE[l][2] : PAYLINE[l][0];
    *x = right ? GRID_X + GRID_W + 4 : GRID_X - (s_big ? 24 : 18);
    *y = GRID_Y + row * CELL_H + CELL_H / 2 - (s_big ? 9 : 6);
}
static void draw_cabinet(void)
{
    // triple gold frame hugging the reel window, corner bolts
    d.drawRoundRect(GRID_X - 3, GRID_Y - 3, GRID_W + 6, GRID_H + 6, 5, COL_GOLDD);
    d.drawRoundRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4, 4, COL_GOLD);
    d.drawRoundRect(GRID_X - 1, GRID_Y - 1, GRID_W + 2, GRID_H + 2, 3, COL_GOLDL);
    for (int i = 0; i < 4; i++) { int bx = i & 1 ? GRID_X + GRID_W + 1 : GRID_X - 2, by = i & 2 ? GRID_Y + GRID_H + 1 : GRID_Y - 2; d.fillSmoothCircle(bx, by, 2, COL_GOLDL); }
    // payline tabs on the cabinet edges: lines 1-3 on the left at their rows, 4-5 on the right where they end
    const int tw = s_big ? 20 : 14, th = s_big ? 18 : 12;
    for (int l = 0; l < 5; l++) {
        bool on = l < active_lines();
        int x, y; tab_xy(l, &x, &y);
        uint16_t c = s_linewin[l] ? COL_WHITE : on ? LINECOL[l] : RGB(36, 73, 36);
        d.fillSmoothRoundRect(x, y, tw, th, 3, c);
        char b[2] = { (char)('1' + l), 0 };
        d.setTextColor(on ? COL_DARK : RGB(73, 109, 73)); d.setCursor(x + tw / 2 - 2, y + th / 2 - 4); d.print(b);
    }
    if (!s_big) return;
    // the panel: the machine's lever on the right (tap it, or the reels, to spin); pulled down while they turn
    const int lx = 304, ky = s_pp == PP_SPIN ? GRID_Y + GRID_H - 22 : GRID_Y + 14;
    d.fillRoundRect(lx - 4, GRID_Y + 6, 8, GRID_H - 12, 4, COL_DARK);
    d.drawRoundRect(lx - 4, GRID_Y + 6, 8, GRID_H - 12, 4, COL_GOLDD);
    const int py = GRID_Y + GRID_H / 2;
    d.fillRect(lx - 2, ky < py ? ky : py, 4, ky < py ? py - ky : ky - py, COL_GREY);
    d.fillSmoothCircle(lx, py, 6, COL_GOLDD);
    d.fillSmoothCircle(lx, ky, 11, COL_REDD); d.fillSmoothCircle(lx - 1, ky - 1, 9, COL_RED); d.fillCircle(lx - 4, ky - 4, 3, COL_PINK);
}

// ============================ reels render ===================================
static void draw_reel_one(int reel)
{
    int rx = GRID_X + reel * (REEL_W + REEL_GAP);
    // the drum: dark at the top and bottom edge, lit in the middle — a parabola over 4 RGB332 levels with
    // alternate-row dithering at each step (integer maths, one line per row)
    static const uint16_t LV[4] = { 0, COL_GLASS, COL_GLASS2, COL_GLASS3 };
    for (int y = 0; y < GRID_H; y++) {
        int v = (24 * y * (GRID_H - 1 - y) / ((GRID_H - 1) * (GRID_H - 1)) + (y & 1)) / 2;
        d.drawFastHLine(rx, GRID_Y + y, REEL_W, LV[v > 3 ? 3 : v]);
    }
    float pos = r_pos[reel];
    int base = (int)floorf(pos);
    float frac = pos - base;
    bool fast = (r_phase[reel] == 1) || (r_phase[reel] == 2 && r_t[reel] < 140);
    int cx = rx + REEL_W / 2 + s_shx;
    float pulse = 0.5f + 0.5f * sinf(s_anim * 0.4f);
    for (int k = -1; k <= NROW; k++) {
        int sy = GRID_Y + (int)((k - frac) * CELL_H) + CELL_H / 2 + s_shy;
        int id = s_strip[reel][wrap(base + k)];
        if (fast) {                                                                   // motion blur: streaks + a squashed symbol
            uint16_t sc = mix(sym_color(id), COL_GLASS, 150);
            d.drawFastHLine(rx + 4, sy - SYM_S / 2, REEL_W - 8, sc);
            d.drawFastHLine(rx + 4, sy, REEL_W - 8, mix(sym_color(id), COL_GLASS, 90));
            d.drawFastHLine(rx + 4, sy + SYM_S / 2, REEL_W - 8, sc);
            draw_sym(cx, sy, SYM_S - 3, id, 0);
            continue;
        }
        int glow = 0, ssz = SYM_S;
        if (r_phase[reel] == 0 && k >= 0 && k < NROW) {                               // resting: winners throb, a fresh stop flashes
            if (s_cellwin[reel][k] && s_pp == PP_WIN) { glow = 90 + (int)(180 * pulse); ssz += (int)((s_win_tier + 1) * pulse); }
            if (r_flash_ms[reel] > 0) { int fg = r_flash_ms[reel] * 255 / 150; if (fg > glow) glow = fg; }
            if (glow > 255) glow = 255;
        }
        draw_sym(cx, sy, ssz, id, glow);
    }
    if (r_phase[reel] == 0 && s_pp != PP_WIN) d.drawFastHLine(rx, GRID_Y + CELL_H + CELL_H / 2, REEL_W, mix(COL_GLASS2, COL_GOLD, 40));   // main line guide
}
static void draw_reels(void)
{
    int32_t cx, cy, cw, ch;                       // clip to the window, inside the current clip (the panel's strip)
    d.getClipRect(&cx, &cy, &cw, &ch);
    const int x0 = GRID_X > cx ? GRID_X : cx, y0 = GRID_Y > cy ? GRID_Y : cy;
    const int x1 = GRID_X + GRID_W < cx + cw ? GRID_X + GRID_W : cx + cw, y1 = GRID_Y + GRID_H < cy + ch ? GRID_Y + GRID_H : cy + ch;
    if (x1 > x0 && y1 > y0) { d.setClipRect(x0, y0, x1 - x0, y1 - y0); for (int r = 0; r < NREEL; r++) draw_reel_one(r); }
    d.setClipRect(cx, cy, cw, ch);
    for (int r = 0; r < NREEL - 1; r++) d.drawFastVLine(GRID_X + r * (REEL_W + REEL_GAP) + REEL_W + REEL_GAP / 2, GRID_Y, GRID_H, COL_GOLDD);   // gold posts
    // anticipation: dim the settled reels with a screen-door, spotlight the still-spinning last reel
    if (s_antic && r_phase[NREEL - 1] != 0) {
        for (int rr = 0; rr < NREEL - 1; rr++) {
            int dx = GRID_X + rr * (REEL_W + REEL_GAP);
            for (int yy = GRID_Y; yy < GRID_Y + GRID_H; yy += 2) d.drawFastHLine(dx, yy, REEL_W, COL_DARK);
        }
        int rx = GRID_X + (NREEL - 1) * (REEL_W + REEL_GAP), a = 70 + (int)(80 * sinf(s_anim * 0.5f));
        for (int k = 0; k < 3; k++) d.drawRoundRect(rx - 2 - k, GRID_Y - 2 - k, REEL_W + 4 + 2 * k, GRID_H + 4 + 2 * k, 4, mix(COL_GLASS, COL_GOLDL, a - k * 18));
    }
}

// ============================ HUD ============================================
// The panel's back chevron (tap = Back, like Esc).
static void lchev(void)
{
    for (int k = 0; k < 2; k++) { d.drawLine(16 + k, 8, 10 + k, 15, COL_GOLDL); d.drawLine(10 + k, 15, 16 + k, 22, COL_GOLDL); }
    nucleo_touch_add(0, 0, 70, 30, NK_BACK, 0);
}
static void draw_hud(void)
{
    char b[28];
    const int hh = s_big ? 30 : 16, cx = s_big ? 36 : 9, ty = s_big ? 7 : 0, ay = hh / 2 - 6;
    d.fillRect(0, 0, W, hh, COL_DARK);
    d.drawFastHLine(0, hh - 1, W, COL_GOLDD);
    if (s_big) lchev();
    d.fillSmoothCircle(cx, hh / 2, 6, COL_GOLD); d.drawCircle(cx, hh / 2, 6, COL_GOLDD); d.drawCircle(cx, hh / 2, 3, COL_GOLDD);   // a coin
    snprintf(b, sizeof b, "%d", (int)(s_disp_bal + 0.5f));
    bool rolling = ((int)(s_disp_bal + 0.5f) != g_balance);                            // credits counting up after a win
    txt(b, cx + 10, s_big ? 3 : -1, 0, s_big ? gui::F_TITLE : gui::F_BODY, rolling && ((s_anim >> 1) & 1) ? COL_WHITE : COL_GOLDL);
    snprintf(b, sizeof b, GT("Puntata %d", "Bet %d"), total_bet());
    txt(b, W - 6, ty, 2, gui::F_SMALL, COL_CREAM);
    if (s_auto) {                                                                       // autospin indicator
        d.fillSmoothRoundRect(W / 2 - 2, ay, 34, 12, 4, mix(COL_DARK, COL_CYAN, 90));
        d.setTextColor(((s_anim >> 2) & 1) ? COL_WHITE : COL_CYAN); d.setCursor(W / 2 + 3, ay + 2); d.print("AUTO");
    }
}
// The panel's bet row under the machine: the bet per line as five coins (tap one = that bet) and the AUTO lamp.
static void draw_bet_row(void)
{
    const bool idle = s_pp == PP_IDLE;
    txt(GT("Puntata/linea", "Bet/line"), 12, 180, 0, gui::F_SMALL, COL_GOLDL);
    for (int i = 0; i < 5; i++) {
        const bool on = i == g_bet_idx; const int x = 10 + i * 50;
        d.fillSmoothRoundRect(x, 198, 44, 34, 8, on ? COL_GOLD : mix(COL_DARK, COL_FELT, idle ? 120 : 40));
        d.drawRoundRect(x, 198, 44, 34, 8, on ? COL_GOLDL : COL_GOLDD);
        char b[4]; snprintf(b, sizeof b, "%d", BET_OPT[i]);
        txt(b, x + 22, 205, 1, gui::F_BODY, on ? COL_DARK : idle ? COL_CREAM : COL_GREY);
    }
    d.fillSmoothRoundRect(262, 186, 50, 46, 10, s_auto ? mix(COL_DARK, COL_CYAN, 140) : COL_DARK);
    d.drawRoundRect(262, 186, 50, 46, 10, COL_CYAN);
    txt("AUTO", 287, 201, 1, gui::F_SMALL, s_auto && ((s_anim >> 2) & 1) ? COL_WHITE : COL_CYAN);
}
// A tap on the machine (the field below the header).
static void machine_tap(int x, int y)
{
    if (s_pp == PP_SPIN) { on_key(NK_ENTER, 0); return; }                              // slam-stop
    if (s_pp != PP_IDLE) return;
    if (y >= GRID_Y - 6 && y < GRID_Y + GRID_H + 6) {
        for (int l = 0; l < 5; l++) {                                                  // a payline tab: play up to that line
            int tx, ty; tab_xy(l, &tx, &ty);
            if (x >= tx - 4 && x < tx + 24 && y >= ty - 6 && y < ty + 24) { g_lines_idx = l == 0 ? 0 : l < 3 ? 1 : 2; sfx(SFX_BET); return; }
        }
        on_key(NK_ENTER, 0);                                                           // the reels / the lever: spin
    } else if (y >= 186 && x >= 258) on_key(NK_CHAR, 'a');                             // the AUTO lamp
    else if (y >= 190 && x < 256) { const int i = (x - 8) / 50; if (i >= 0 && i < 5 && i != g_bet_idx) { g_bet_idx = i; sfx(SFX_BET); } }
}

// ============================ coin particles / fireworks =====================
static void coins_spawn(int n, int tier)
{
    for (int i = 0; i < NPART && n > 0; i++) {
        Coin &c = s_fx->coin[i];
        if (c.life > 0) continue;
        c.x = GRID_X + 10 + (int)(esp_random() % (GRID_W - 20));
        c.y = GRID_Y + GRID_H / 2;
        c.vx = ((int)(esp_random() % 200) - 100) / 40.0f;
        c.vy = -2.2f - (esp_random() % 100) / 60.0f - tier * 0.5f;
        c.life = 30 + (int)(esp_random() % 24);
        c.col = (esp_random() & 1) ? COL_GOLD : COL_GOLDL;
        n--;
    }
}
static bool coins_step(int dt)
{
    float k = dt / 33.0f, floorY = (float)(GRID_Y + GRID_H - 3);
    bool any = false;
    for (int i = 0; i < NPART; i++) {
        Coin &c = s_fx->coin[i];
        if (c.life <= 0) continue;
        c.vy += 0.34f * k; c.x += c.vx * k; c.y += c.vy * k;
        if (c.y > floorY && c.vy > 0) { c.y = floorY; c.vy = -c.vy * 0.5f; c.vx *= 0.7f; }   // bounce on the reel floor
        c.life -= dt; any = true;
    }
    return any;
}
static void coins_draw(void)
{
    for (int i = 0; i < NPART; i++) {
        const Coin &c = s_fx->coin[i];
        if (c.life <= 0) continue;
        int x = (int)c.x, y = (int)c.y;
        d.fillCircle(x, y, 3, COL_GOLDD); d.fillCircle(x, y, 2, c.col); d.drawPixel(x - 1, y - 1, COL_WHITE);
    }
}
static void fw_spawn(void)
{
    static const uint16_t C[4] = { COL_GOLD, COL_CYAN, COL_PINK, COL_GREEN };
    for (int i = 0; i < NFW; i++) {
        FW &f = s_fx->fw[i];
        if (f.live) continue;
        f.live = true; f.age = 0;
        f.x = 30 + (int)(esp_random() % (W - 60));
        f.y = GRID_Y + 8 + (int)(esp_random() % (GRID_H - 24));
        f.col = C[esp_random() & 3];
        return;
    }
}
static void fw_step(int dt) { for (int i = 0; i < NFW; i++) { FW &f = s_fx->fw[i]; if (f.live) { f.age += dt; if (f.age > 460) f.live = false; } } }
static void fw_draw(void)
{
    static const int8_t DX[10] = { 10, 8, 3, -3, -8, -10, -8, -3, 3, 8 }, DY[10] = { 0, 6, 10, 10, 6, 0, -6, -10, -10, -6 };   // unit ring x10
    for (int i = 0; i < NFW; i++) {
        const FW &f = s_fx->fw[i];
        if (!f.live) continue;
        int rr = f.age * 24 / 460, fade = 256 - f.age * 256 / 460;
        uint16_t c = mix(COL_FELT, f.col, fade);
        for (int k = 0; k < 10; k++) d.fillCircle(f.x + DX[k] * rr / 10, f.y + DY[k] * rr / 10, fade > 130 ? 2 : 1, c);
        d.drawCircle(f.x, f.y, rr, mix(COL_FELT, COL_WHITE, fade / 2));
    }
}

// ============================ spin / evaluate ================================
// Auto-fit the wager to the bankroll: the LARGEST affordable total bet (<= balance), more lines on a tie.
static void fit_bet(void)
{
    int bestLi = g_lines_idx, bestBi = g_bet_idx, bestTot = -1;
    for (int li = 0; li < 3; li++)
        for (int bi = 0; bi < 5; bi++) {
            int t = LINES_OPT[li] * BET_OPT[bi];
            if (t > g_balance) continue;
            if (t > bestTot || (t == bestTot && LINES_OPT[li] > LINES_OPT[bestLi])) { bestTot = t; bestLi = li; bestBi = bi; }
        }
    if (bestTot > 0) { g_lines_idx = bestLi; g_bet_idx = bestBi; }
}
// What the stops in r_top pay on the active lines: collects the winning lines, returns the total.
static int line_wins(bool *jackpotish)
{
    int total = 0, bpl = BET_OPT[g_bet_idx];
    s_win_nlines = 0; *jackpotish = false;
    for (int l = 0; l < active_lines(); l++) {
        int s0 = cell(0, PAYLINE[l][0]), s1 = cell(1, PAYLINE[l][1]), s2 = cell(2, PAYLINE[l][2]);
        int base = s0 != SY_WILD ? s0 : s1 != SY_WILD ? s1 : s2;                // a leading WILD takes the first real symbol
        int run = 0;
        for (int r = 0; r < NREEL; r++) { int sv = cell(r, PAYLINE[l][r]); if (sv == base || sv == SY_WILD) run++; else break; }
        int mul = paymul(base, run);
        if (!mul) continue;
        total += mul * bpl;
        s_win_lines[s_win_nlines] = l; s_win_run[s_win_nlines] = run; s_win_nlines++;
        if (base >= SY_SEVEN) *jackpotish = true;
    }
    return total;
}
static void do_spin(void)
{
    int bet = total_bet();
    if (g_balance < bet) {
        if (g_balance <= 0) { s_auto = false; sfx(SFX_BACK); go(ST_OVER); return; }
        fit_bet();                                                                     // auto-lower to what is affordable
        bet = total_bet();
        snprintf(s_msg, sizeof s_msg, GT("Puntata ridotta a %d", "Bet lowered to %d"), bet);
        s_msg_ms = 1100; sfx(SFX_BET);
    }
    g_balance -= bet;
    for (int r = 0; r < NREEL; r++) for (int w = 0; w < NROW; w++) s_cellwin[r][w] = false;
    for (int i = 0; i < 5; i++) s_linewin[i] = false;
    s_pp = PP_SPIN; s_spin_ms = 0; s_antic = false; s_antic_checked = false;
    s_win_total = 0; s_auto_ms = 0;
    s_win_nlines = 0; s_win_shown = 0; s_finale_done = false; s_fin_age = 0;
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_quickstop = false;
    for (int i = 0; i < NFW; i++) s_fx->fw[i].live = false;
    for (int r = 0; r < NREEL; r++) {
        r_top[r] = (int)(esp_random() % SLEN);                                         // the result, drawn now
        r_phase[r] = 1; r_t[r] = 0; r_flash_ms[r] = 0;
        r_delay[r] = g_turbo ? (240 + r * 150) : (520 + r * 300);
    }
    sfx(SFX_SPIN);
    nucleo_app_request_draw();
}
// Slam-stop (pro-slot feel): the spin button pressed mid-spin drops every still-spinning reel into its stop
// now, left to right. The result was drawn at the spin: this only skips the wait.
static void quick_stop(void)
{
    if (s_quickstop) return;
    s_quickstop = true;
    s_antic = false; s_antic_checked = true;                  // cancel any anticipation hold
    for (int r = 0, off = 0; r < NREEL; r++) if (r_phase[r] == 1) { r_delay[r] = s_spin_ms + off; off += 90; }
    sfx(SFX_SEL);
    nucleo_app_request_draw();
}
// would a third matching high symbol pay? checked from reels 0,1 already stopped.
static bool antic_possible(void)
{
    for (int l = 0; l < active_lines(); l++) {
        int a = cell(0, PAYLINE[l][0]), b = cell(1, PAYLINE[l][1]);
        int base = a == SY_WILD ? b : a;
        if ((a == base || a == SY_WILD) && (b == base || b == SY_WILD) && base >= SY_BELL) return true;
    }
    return false;
}
// Reveal the i-th winning line: light it, mark its symbols, flash those reels, an ascending chime.
#define REVEAL_MS 340
static void win_reveal(int i)
{
    int l = s_win_lines[i];
    s_linewin[l] = true;
    for (int r = 0; r < s_win_run[i]; r++) { s_cellwin[r][PAYLINE[l][r]] = true; r_flash_ms[r] = 170; }
    sfx(SFX_LN1 + (i < 5 ? i : 4));
}
// All lines shown -> the payoff: coin fountain + a tier-scaled finale.
static void win_finale(void)
{
    s_fin_age = 0;
    coins_spawn(s_win_tier == 2 ? 34 : (s_win_tier == 1 ? 20 : 10), s_win_tier);
    if (s_win_tier == 2)      { sfx(SFX_JACK); s_flash_ms = 200; s_shake_ms = 650; s_coinwave_ms = 1700; s_fw_ms = 0; }
    else if (s_win_tier == 1) { sfx(SFX_CHACH); s_shake_ms = 300; s_coinwave_ms = 700; }
    else                        sfx(SFX_CASC);
}
static void evaluate(void)
{
    bool jackpotish;
    int total = line_wins(&jackpotish), bpl = BET_OPT[g_bet_idx];
    if (total > 0) {
        g_balance += total;
        s_win_total = total;
        int ratio = total / bpl;
        s_win_tier = (jackpotish || ratio >= 60) ? 2 : (ratio >= 15 ? 1 : 0);
        s_win_ms = s_win_tier == 2 ? 2200 : (s_win_tier == 1 ? 1300 : 800);   // HOLD time after the finale fires
        if (total > g_best_win) g_best_win = total;
        s_win_shown = 0; s_win_step_ms = 140; s_finale_ms = 0; s_finale_done = false;
        s_pp = PP_WIN;                                              // poll() now drives the reveal sequence
        if (s_win_tier >= 1) cfg_write();                          // persist notable wins (on_exit always saves)
    } else {
        bool near = false;                                          // near-miss earcon: two 7s/WILDs on a line
        for (int l = 0; l < active_lines() && !near; l++) {
            int c = 0;
            for (int r = 0; r < NREEL; r++) c += cell(r, PAYLINE[l][r]) >= SY_SEVEN;
            near = c == 2;
        }
        sfx(near ? SFX_NEAR : SFX_NOWIN);
        s_pp = PP_IDLE;
        if (g_balance < total_bet()) cfg_write();                   // persist a drained balance
        if (s_auto) s_auto_ms = 500;
    }
    nucleo_app_request_draw();
}
// Leaving mid-spin (Esc, closing the app): the drawn result is shown at once and paid — never swallowed.
static void settle_spin(void)
{
    if (s_pp != PP_SPIN) { s_pp = PP_IDLE; return; }
    for (int r = 0; r < NREEL; r++) { r_phase[r] = 0; r_pos[r] = (float)r_top[r]; }
    bool j; int win = line_wins(&j);
    g_balance += win; if (win > g_best_win) g_best_win = win;
    s_pp = PP_IDLE; s_auto = false;
    cfg_write();
}

// ============================ screens: menu / help / settings / over =========
// The shared title / menu / dialog, drawn natively on the panel (touch rows, a back chevron, a tappable card).
static int title_bar(const char *name, const char *sub)
{
    if (!s_big) return gui::title(name, sub, COL_GOLD);
    gui::vgradient(0, 0, W, s_vh, mix(0, COL_GOLD, 60), COL_DARK);
    d.fillRect(0, 0, W, 30, COL_DARK); d.drawFastHLine(0, 29, W, COL_GOLDD);
    lchev();
    txt(name, W / 2, 4, 1, gui::F_TITLE, COL_GOLDL);
    if (sub) txt(sub, W / 2, 34, 1, gui::F_SMALL, COL_CREAM);
    return sub ? 58 : 38;
}
static void menu_rows(const gui::Menu &m, const char *const *it, int n, int y0)
{
    if (!s_big) { gui::menu(m, it, n, y0, s_vh, COL_GOLD); return; }
    int rh = (s_vh - y0 - 4) / n; if (rh > 54) rh = 54;
    for (int i = 0; i < n; i++) {
        const int y = y0 + i * rh; const bool sel = i == m.sel;
        d.fillSmoothRoundRect(28, y + 3, W - 56, rh - 6, 10, sel ? mix(0, COL_GOLD, 150) : mix(COL_DARK, 0, 60));
        d.drawRoundRect(28, y + 3, W - 56, rh - 6, 10, sel ? COL_GOLDL : COL_GOLDD);
        txt(it[i], W / 2, y + rh / 2 - 7, 1, gui::F_BODY, sel ? COL_WHITE : COL_CREAM);
        if (!s_confirm) nucleo_touch_add(0, y, W, rh, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
}
// tkeys == nullptr on the panel: a Yes / No pair (rows 0 / 1); else the card itself is the tap target.
static void dlg(const char *head, const char *l1, const char *keys, const char *tkeys, uint16_t acc)
{
    if (!s_big) { gui::dialog(head, l1, nullptr, keys, acc); return; }
    const int x = 30, y = 62, w = 260, h = 116;
    gui::panel(x, y, w, h, COL_DARK, acc);
    txt(head, W / 2, y + 10, 1, gui::F_TITLE, acc);
    if (l1) txt(l1, W / 2, y + 42, 1, gui::F_SMALL, COL_WHITE);
    if (tkeys) { txt(tkeys, W / 2, y + 80, 1, gui::F_SMALL, COL_GOLDL); lg::field(x, y, w, h); return; }
    for (int i = 0; i < 2; i++) {
        const int bx = x + 20 + i * 116;
        d.fillSmoothRoundRect(bx, y + 70, 104, 34, 8, i ? mix(COL_DARK, COL_GREY, 90) : COL_RED);
        txt(i ? "No" : GT("Si", "Yes"), bx + 52, y + 77, 1, gui::F_BODY, COL_WHITE);
        nucleo_touch_add(bx, y + 70, 104, 34, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
}
static void draw_menu(void)
{
    char sub[32]; snprintf(sub, sizeof sub, GT("Crediti %d", "Credits %d"), g_balance);
    int y = title_bar("SLOT", sub);
    const int sx = s_big ? 112 : 22;
    draw_sym(sx, 16, 11, SY_SEVEN, 40); draw_sym(W - sx, 16, 11, SY_CHERRY, 40);
    const char *items[3] = { GT("Gioca", "Play"), GT("Come si gioca", "How to play"), GT("Impostazioni", "Settings") };
    menu_rows(s_menu, items, 3, y);
}
static void draw_help(void)
{
    int ch = s_vh;
    const char *titles[3] = { GT("Obiettivo", "Objective"), GT("Comandi", "Controls"), GT("Simboli", "Symbols") };
    int y = title_bar(titles[s_help], nullptr);
    const int lp = s_big ? 26 : 17, fb = s_big ? gui::F_BODY : gui::F_SMALL;   // the panel: bigger type, airier lines
    if (s_big) { lg::field(0, 30, W, s_vh - 30); y += 10; }                    // tap the left / right half: page back / on
    if (s_help == 0) {
        txt(GT("Allinea 3 simboli uguali su", "Line up 3 equal symbols on an"), 8, y, 0, fb, COL_CREAM);
        txt(GT("una linea attiva per vincere.", "active line to win."), 8, y + lp, 0, fb, COL_CREAM);
        txt(GT("Il JOLLY (W) vale per tutti.", "The WILD (W) stands for all."), 8, y + 2 * lp + 2, 0, fb, COL_GOLD);
        txt(GT("Tre 7 = JACKPOT!", "Three 7s = JACKPOT!"), 8, y + 3 * lp + 2, 0, fb, COL_RED);
    } else if (s_help == 1) {
        const char *k[4] = { s_big ? GT("Leva, rulli", "Lever, reels") : GT("SPAZIO", "SPACE"), s_big ? "1 2 5 10 25" : GT("SU/GIU", "UP/DN"), s_big ? "1 - 5" : GT("SX/DX", "L/R"), s_big ? "AUTO" : "A  TAB" };
        const char *v[4] = { GT("gira / ferma", "spin / stop"), GT("puntata per linea", "bet per line"), GT("linee 1 3 5", "lines 1 3 5"), GT("auto  impostazioni", "auto  setup") };
        for (int i = 0; i < 4; i++) { txt(k[i], 8, y + i * lp, 0, fb, COL_GREY); txt(v[i], s_big ? 136 : 84, y + i * lp, 0, fb, COL_GOLDL); }
    } else {
        for (int i = 0; i < NSYM; i++) {                                             // paytable: mini symbol + 3x payout
            int sym = NSYM - 1 - i, x = 14 + (i % 4) * (s_big ? 78 : 58), yy = y + 2 + (i / 4) * (s_big ? 40 : 24), ss = s_big ? 13 : 8;
            draw_sym(x + ss - 8, yy + ss, ss, sym, 0);
            char b[12]; snprintf(b, sizeof b, "x%d", PAY3[sym]);
            txt(b, x + 2 * ss - 5, yy + ss - 8, 0, gui::F_SMALL, sym >= SY_SEVEN ? COL_GOLDL : COL_CREAM);
        }
        char b[32]; snprintf(b, sizeof b, GT("2 ciliegie x%d", "2 cherries x%d"), CHERRY2);
        const int cy = y + (s_big ? 96 : 58);
        draw_sym(14, cy, 8, SY_CHERRY, 0); draw_sym(30, cy, 8, SY_CHERRY, 0);
        txt(b, 44, cy - 8, 0, fb, COL_RED);
    }
    for (int i = 0; i < 3; i++) d.fillCircle(W / 2 - 12 + i * 12, ch - 5, i == s_help ? 3 : 2, i == s_help ? COL_GOLD : RGB(73, 109, 85));
}
#define NSET 6   // Lines / Bet per line / Auto / Turbo / Audio / Reset credits
static void draw_settings(void)
{
    char r[5][28];
    const char *on = GT("Si", "On"), *off = GT("No", "Off");
    snprintf(r[0], 28, "%s: %d", GT("Linee", "Lines"), active_lines());
    snprintf(r[1], 28, "%s: %d", GT("Puntata/linea", "Bet/line"), BET_OPT[g_bet_idx]);
    snprintf(r[2], 28, "Auto: %s", s_auto ? on : off);
    snprintf(r[3], 28, "Turbo: %s", g_turbo ? on : off);
    snprintf(r[4], 28, "Audio: %s", g_audio ? on : off);
    const char *items[NSET] = { r[0], r[1], r[2], r[3], r[4], GT("Azzera crediti", "Reset credits") };
    int y = title_bar(GT("Impostazioni", "Settings"), nullptr);
    menu_rows(s_setm, items, NSET, y);
    if (s_confirm) {
        char l1[28]; snprintf(l1, sizeof l1, GT("Riparti con %d crediti", "Restart with %d credits"), START_CREDITS);
        dlg(GT("Azzerare i crediti?", "Reset the credits?"), l1, GT("INVIO si  Esc no", "ENTER yes  Esc no"), nullptr, COL_RED);
    }
}
static void draw_over(void)
{
    draw_felt();
    char l1[32]; snprintf(l1, sizeof l1, GT("Riparti con %d crediti", "Restart with %d credits"), START_CREDITS);
    if (s_big) lchev();
    dlg(GT("CREDITI FINITI", "OUT OF CREDITS"), l1, GT("INVIO ricomincia  Esc menu", "ENTER restart  Esc menu"), GT("Tocca: ricomincia", "Tap: restart"), ((s_anim >> 2) & 1) ? COL_RED : COL_GOLD);
}

// ============================ screen: play ===================================
static void draw_win_overlay(void)
{
    int bright = 120 + (int)(120 * sinf(s_anim * 0.5f));
    for (int l = 0; l < 5; l++) {                                                      // revealed winning paylines
        if (!s_linewin[l]) continue;
        uint16_t lc = mix(LINECOL[l], COL_WHITE, bright);
        for (int r = 1; r < NREEL; r++) {
            int x0 = GRID_X + (r - 1) * (REEL_W + REEL_GAP) + REEL_W / 2 + s_shx, y0 = GRID_Y + PAYLINE[l][r - 1] * CELL_H + CELL_H / 2 + s_shy;
            int x1 = x0 + REEL_W + REEL_GAP, y1 = GRID_Y + PAYLINE[l][r] * CELL_H + CELL_H / 2 + s_shy;
            for (int o = -1; o <= 1; o++) d.drawLine(x0, y0 + o, x1, y1 + o, lc);   // (drawWideLine ignores the strip clip)
        }
    }
    if (!s_finale_done) return;                                                        // payoff text after the reveal
    int bxc = W / 2 + s_shx;
    char b[16]; snprintf(b, sizeof b, "+%d", s_win_total);
    if (s_win_tier == 0) {                                                             // SMALL: a "+N" that floats up
        int rise = s_fin_age < 600 ? s_fin_age / 55 : 10;
        gui::text(b, bxc, GRID_Y + GRID_H / 2 - 12 - rise, 1, gui::F_BIG, COL_GOLDL, COL_DARK);
        return;
    }
    // BIG / JACKPOT: the banner pops in (ease-out), text once wide enough
    const char *bw = s_win_tier == 2 ? "JACKPOT!" : GT("GRANDE VINCITA!", "BIG WIN!");
    float kf = s_fin_age < 180 ? (float)s_fin_age / 180 : 1.0f;
    kf = 1.0f - (1.0f - kf) * (1.0f - kf);
    int fullw = gui::text_width(bw, gui::F_BODY) + 24, hh = (int)(44 * (0.5f + 0.5f * kf)), wpx = (int)(fullw * (0.35f + 0.65f * kf));
    int by = GRID_Y + GRID_H / 2 - hh / 2 + s_shy;
    uint16_t bc = s_win_tier == 2 && ((s_anim >> 1) & 1) ? COL_RED : COL_GOLD;
    d.fillSmoothRoundRect(bxc - wpx / 2, by, wpx, hh, 7, mix(0, bc, 80));
    d.drawRoundRect(bxc - wpx / 2, by, wpx, hh, 7, COL_GOLDL);
    if (kf > 0.6f) { txt(bw, bxc, by + 1, 1, gui::F_BODY, COL_WHITE); txt(b, bxc, by + 20, 1, gui::F_BODY, COL_GOLDL); }
}
static void draw_rays(void)                       // rotating golden light rays behind the machine on a big/mega win
{
    int cx = W / 2, cy = GRID_Y + GRID_H / 2, R = 220;
    float rot = s_anim * 0.05f;
    for (int i = 0; i < 12; i += 2) {
        float a0 = rot + i * 0.5235988f, a1 = a0 + 0.32f;
        d.fillTriangle(cx, cy, cx + (int)(cosf(a0) * R), cy + (int)(sinf(a0) * R), cx + (int)(cosf(a1) * R), cy + (int)(sinf(a1) * R), mix(COL_FELT, COL_GOLD, 55));
    }
}
static void draw_play(void)
{
    s_shx = s_shy = 0;
    if (s_shake_ms > 0) { int m = s_shake_ms > 350 ? 3 : 2; s_shx = (int)((s_anim * 37) % (2 * m + 1)) - m; s_shy = (int)((s_anim * 53) % (2 * m + 1)) - m; }
    int ch = s_vh;
    bool big = s_pp == PP_WIN && s_finale_done && s_win_tier >= 1;
    draw_felt();
    if (big) draw_rays();
    draw_cabinet();
    draw_reels();
    draw_hud();
    // Touch: the bet label raises the bet, the middle of the bar toggles autospin; a tap on the reels (or
    // anywhere else) is Enter: spin, or slam-stop while they turn. Drag right/left changes the lines. On the panel
    // the machine under the header is one field (machine_tap): the reels / lever, the payline tabs, the coins, AUTO.
    if (s_big) { draw_bet_row(); if (!s_paused) lg::field(0, 30, W, s_vh - 30); }
    else if (!s_paused && s_pp == PP_IDLE && !nucleo_app_two_screens()) {
        nucleo_touch_add(W - 80, 0, 80, 16, NK_UP, 0);
        nucleo_touch_add(W / 2 - 24, 0, 52, 16, NK_CHAR, 'a');
    }
    coins_draw();
    if (s_pp == PP_WIN && s_win_tier == 2) fw_draw();                                   // jackpot-only fireworks
    if (s_flash_ms > 0) d.fillRect(0, 0, W, ch, mix(COL_FELT, COL_WHITE, s_flash_ms * 256 / 200));   // jackpot white burst
    if (big) {                                                                         // pulsing win frame
        int g = 110 + (int)(120 * sinf(s_anim * 0.4f));
        uint16_t gc = s_win_tier == 2 ? LINECOL[(s_anim / 6) % 5] : COL_GOLD;
        for (int k = 0; k < 3; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_FELT, gc, g - k * 30));
    }
    if (s_pp == PP_WIN) draw_win_overlay();
    if (s_msg_ms > 0) { gui::panel(30, GRID_Y + GRID_H / 2 - 10, W - 60, 20, COL_REDD, COL_GOLD); txt(s_msg, W / 2, GRID_Y + GRID_H / 2 - 9, 1, gui::F_SMALL, COL_WHITE); }
    if (s_paused) dlg(GT("PAUSA", "PAUSED"), nullptr, GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave"), GT("Tocca: riprendi  < esci", "Tap: resume  < leave"), COL_GOLD);
}

// ============================ input + hint ===================================
static void set_hint(void)
{
    const char *h;
    switch (s_screen) {
        case ST_MENU: h = GT("su/giu  INVIO scegli  Esc esci", "UP/DN  ENTER pick  Esc quit"); break;
        case ST_HELP: h = GT("sx/dx pagine  Esc indietro", "L/R pages  Esc back"); break;
        case ST_SET:  h = GT("su/giu  sx/dx cambia  Esc ok", "UP/DN  L/R change  Esc done"); break;
        case ST_PLAY: h = s_paused ? GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave")
                                   : GT("SPAZIO gira  su/giu punta  TAB", "SPACE spin  UP/DN bet  TAB setup"); break;
        default:      h = GT("INVIO ricomincia  Esc menu", "ENTER restart  Esc menu"); break;
    }
    nucleo_app_set_hint(h);
}
static void go(int s) { s_screen = s; s_paused = false; s_confirm = false; set_hint(); nucleo_app_request_draw(); }

static void open_settings(int from) { s_set_from = from; s_setm = {}; go(ST_SET); sfx(SFX_SEL); }
static void tab_handler(void)
{
    if (s_screen == ST_PLAY && !s_paused && s_pp == PP_IDLE) open_settings(ST_PLAY);
    else if (s_screen == ST_SET && s_set_from == ST_PLAY && !s_confirm) { go(ST_PLAY); sfx(SFX_BACK); }
}
static void bet_change(int row, int dir)
{
    switch (row) {
        case 0: g_lines_idx = (g_lines_idx + 3 + dir) % 3; sfx(SFX_BET); break;
        case 1: g_bet_idx = (g_bet_idx + 5 + dir) % 5; sfx(SFX_BET); break;
        case 2: s_auto = !s_auto; s_auto_ms = s_auto ? 400 : 0; sfx(SFX_SEL); break;
        case 3: g_turbo = !g_turbo; sfx(SFX_SEL); break;
        case 4: g_audio = !g_audio; sfx(SFX_SEL); break;
        default: if (dir > 0) { s_confirm = true; sfx(SFX_SEL); } return;
    }
    cfg_write();
}
static void on_key(int k, char ch)
{
    if (lg_key(k, ch)) return;                                                         // a touch on the panel
    nucleo_app_request_draw();
    switch (s_screen) {
        case ST_MENU:
            if (gui::menu_key(s_menu, k, 3)) sfx(SFX_MOVE);
            else if (k == NK_ENTER || k == NK_RIGHT) {
                sfx(SFX_SEL);
                if (s_menu.sel == 0) { s_disp_bal = g_balance; go(ST_PLAY); }
                else if (s_menu.sel == 1) { s_help = 0; go(ST_HELP); }
                else open_settings(ST_MENU);
            }
            return;
        case ST_HELP:
            if (k == NK_RIGHT)      { s_help = (s_help + 1) % 3; sfx(SFX_MOVE); }
            else if (k == NK_ENTER) { sfx(SFX_BACK); go(ST_MENU); }
            return;
        case ST_SET:
            if (s_confirm) { if (k == NK_ENTER) { g_balance = START_CREDITS; s_disp_bal = START_CREDITS; s_confirm = false; cfg_write(); sfx(SFX_BONUS); } return; }
            if (gui::menu_key(s_setm, k, NSET)) sfx(SFX_MOVE);
            else if (k == NK_ENTER || k == NK_RIGHT) bet_change(s_setm.sel, +1);
            return;
        case ST_PLAY:
            if (s_paused) { if (k == NK_ENTER) { s_paused = false; set_hint(); } return; }
            if (s_pp == PP_IDLE) {
                if (ch == ' ' || k == NK_ENTER)               do_spin();
                else if (k == NK_UP || ch == 'w' || ch == 'W')   bet_change(1, +1);
                else if (k == NK_DOWN || ch == 'e' || ch == 'E') bet_change(1, -1);
                else if (k == NK_RIGHT)                          bet_change(0, +1);
                else if (ch == 'a' || ch == 'A')                 bet_change(2, +1);
            } else if (s_pp == PP_SPIN && (ch == ' ' || k == NK_ENTER)) quick_stop();  // slam-stop the reels
            return;
        default:
            if (k == NK_ENTER || ch == ' ') { g_balance = START_CREDITS; s_disp_bal = g_balance; s_auto = false; cfg_write(); sfx(SFX_BONUS); coins_spawn(16, 1); go(ST_PLAY); }
            return;
    }
}
static bool on_back(int key)
{
    nucleo_app_request_draw();
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_HELP: s_help = (s_help + 2) % 3; sfx(SFX_MOVE); return true;
            case ST_SET:  if (!s_confirm) bet_change(s_setm.sel, -1); return true;
            case ST_PLAY: if (!s_paused && s_pp == PP_IDLE) bet_change(0, -1); return true;
            default: return s_screen != ST_MENU;
        }
    }
    switch (s_screen) {
        case ST_MENU: return false;                                                    // Esc on the menu -> close the app
        case ST_SET:  if (s_confirm) s_confirm = false; else { go(s_set_from); sfx(SFX_BACK); } return true;
        case ST_PLAY:
            if (!s_paused) { s_paused = true; set_hint(); sfx(SFX_BACK); return true; }   // first Esc: the pause card
            settle_spin();                                                             // second: leave (a spin is paid out)
            [[fallthrough]];
        default: sfx(SFX_BACK); s_menu = {}; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void step_reels(int dt)
{
    const float SPD = 0.034f;                                                          // symbols per ms
    const int   STOP_MS = s_quickstop ? 150 : (g_turbo ? 240 : 360), BOUNCE_MS = 140, TRAVEL = 5;
    s_spin_ms += dt;
    for (int r = 0; r < NREEL; r++) {
        if (r_phase[r] == 1) {                                                          // spinning
            r_pos[r] += SPD * dt;
            if (r == NREEL - 1 && !s_antic_checked && s_spin_ms >= r_delay[r]) {        // last reel: anticipation, decided once
                s_antic_checked = true;
                if (antic_possible()) { r_delay[r] += g_turbo ? 450 : 900; s_antic = true; sfx(SFX_DRUM); }
            }
            if (s_spin_ms >= r_delay[r]) {
                // ease into the drawn stop: the strip jumps by whole symbols while it is a blur, so the reel
                // always travels the same 5 symbols at the same speed whatever the result
                int land = (int)ceilf(r_pos[r]) + TRAVEL, k;
                int delta = wrap(r_top[r] - land);
                r_pos[r] += delta; land += delta;
                k = (land / SLEN) * SLEN; r_pos[r] -= k; land -= k;
                r_start[r] = r_pos[r]; r_target[r] = (float)land;
                r_phase[r] = 2; r_t[r] = 0;
            }
        } else if (r_phase[r] == 2) {                                                   // easing to stop
            r_t[r] += dt;
            float u = (float)r_t[r] / STOP_MS;
            if (u >= 1.0f) {
                r_pos[r] = r_target[r]; r_phase[r] = 3; r_t[r] = 0; r_flash_ms[r] = 150;
                bool hi = false;
                for (int l = 0; l < active_lines(); l++) hi |= cell(r, PAYLINE[l][r]) >= SY_SEVEN;
                sfx(hi ? SFX_SPRK : SFX_STOP1 + r);                                     // sparkle when a 7/WILD lands, else a clunk
            } else { float f = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u); r_pos[r] = r_start[r] + (r_target[r] - r_start[r]) * f; }
        } else if (r_phase[r] == 3) {                                                   // settle bounce
            r_t[r] += dt;
            float u = (float)r_t[r] / BOUNCE_MS;
            if (u >= 1.0f) { r_pos[r] = r_target[r]; r_phase[r] = 0; }
            else r_pos[r] = r_target[r] + sinf(3.14159f * u) * 0.16f * (1.0f - u);
        }
    }
    for (int r = 0; r < NREEL; r++) if (r_phase[r] != 0) return;
    evaluate();
}
static void draw_game(void)
{
    if (!s_big) d.setClipRect(0, 0, W, s_vh);          // rays never paint the footer
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        default:      draw_over(); coins_draw(); break;
    }
    if (!s_big) d.clearClipRect();                     // (on the panel the strip window owns the clip)
}

// ===== two screens: the whole game NATIVE on the 320x240 panel, the Cardputer as a companion =============
// view_set(true) lays every screen out for 320x240 (bigger reels, the lever, the bet coins under the machine),
// composed off-screen in strips (lg::frame). Touch is direct: tap a menu / settings row to pick it, again to open or
// change it; tap the reels or the lever to spin (again to slam-stop), a payline tab for the lines, a coin for the
// bet per line, the AUTO lamp; tap the card (pause, game over) to go on; the header chevron is Back.
static void lg_paint(void *) { draw_game(); }
static void draw_large(void)
{
    view_set(true);
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}
static bool lg_key(int k, char ch)
{
    int x = 0, y = 0;
    const bool fld = lg::tap(k, ch, &x, &y);
    const int ri = nucleo_touch_is_row(k, ch) ? ch - NUCLEO_TOUCH_ROW : -1;
    if (!fld && ri < 0) return false;
    gui::Menu *m = s_screen == ST_MENU ? &s_menu : s_screen == ST_SET && !s_confirm ? &s_setm : nullptr;
    if (m && ri >= 0) {                                                                 // a row: pick it, again = open / change
        if (m->sel == ri) on_key(NK_ENTER, 0); else { m->sel = (int8_t)ri; m->pos = ri; sfx(SFX_MOVE); }
    } else if (s_screen == ST_SET && ri >= 0) { if (ri == 0) on_key(NK_ENTER, 0); else on_back(NK_BACK); }   // Yes / No
    else if (s_screen == ST_HELP && fld) { if (x < W / 2) on_back(NK_LEFT); else on_key(NK_RIGHT, 0); }   // page back / on
    else if (s_screen == ST_PLAY && fld && !s_paused) machine_tap(x, y);
    else if (fld) on_key(NK_ENTER, 0);                                                  // the pause / game-over card
    nucleo_app_request_draw();
    return true;
}
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = s_vh;
    d.fillRect(0, top, W, h, BG);
    int y = lg::comp_head("SLOT", COL_GOLD);
    char b[40];
    gui::text(GT("Crediti", "Credits"), 8, y + 2, 0, gui::F_SMALL, COL_GREY, 0);
    snprintf(b, sizeof b, "%d", g_balance); gui::text(b, 8, y + 13, 0, gui::F_BIG, COL_GOLDL, 0);
    snprintf(b, sizeof b, GT("Punt %d  Linee %d", "Bet %d  Lines %d"), total_bet(), active_lines());
    gui::text(b, W - 8, y + 20, 2, gui::F_SMALL, COL_CREAM, 0);
    const char *st;
    if (s_screen == ST_MENU)      st = GT("Menu", "Menu");
    else if (s_screen == ST_HELP) st = GT("Guida", "Help");
    else if (s_screen == ST_SET)  st = GT("Impostazioni", "Settings");
    else if (s_screen == ST_OVER) st = GT("Crediti finiti", "Out of credits");
    else if (s_paused)            st = GT("Pausa", "Paused");
    else if (s_pp == PP_SPIN)     st = GT("Girano...", "Spinning...");
    else if (s_pp == PP_WIN)      st = s_win_tier == 2 ? "JACKPOT!" : GT("Vincita!", "Win!");
    else if (s_auto)              st = GT("Auto attivo", "Auto on");
    else                          st = GT("Pronto a girare", "Ready to spin");
    gui::text(st, 8, y + 50, 0, gui::F_BODY, COL_WHITE, 0);
    if (s_screen == ST_PLAY && s_pp == PP_WIN && s_win_total > 0) { snprintf(b, sizeof b, "+%d", s_win_total); gui::text(b, W - 8, y + 50, 2, gui::F_BODY, COL_GOLDL, 0); }
    const char *keys = s_screen != ST_PLAY ? GT("Tocca il pannello", "Tap the panel")
                     : s_paused            ? GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave")
                     : s_pp == PP_SPIN     ? GT("SPAZIO ferma i rulli", "SPACE stop the reels")
                     : GT("SPAZIO gira  su/giu punta", "SPACE spin  up/dn bet");
    lg::comp_keys(keys);
}
static void on_draw(void)
{
    view_set(false);
    if (nucleo_app_two_screens()) { draw_companion(); view_set(true); return; }
    draw_game();
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last);
    if (dt < 0) dt = 0;
    if (dt > 200) dt = 200;
    s_last = s_now;
    if (s_screen == ST_MENU) return gui::menu_tick(s_menu, dt);                     // still menus push nothing
    if (s_screen == ST_SET)  return gui::menu_tick(s_setm, dt);
    if (s_screen == ST_HELP || s_paused) return false;

    float before = s_disp_bal;                                                         // eased credit count-up/down
    s_disp_bal += ((float)g_balance - s_disp_bal) * 0.18f;
    if (fabsf((float)g_balance - s_disp_bal) < 0.5f) s_disp_bal = g_balance;
    bool live = s_disp_bal != before || s_msg_ms > 0 || s_auto;
    if (s_msg_ms > 0) dec(&s_msg_ms, dt);
    live |= coins_step(dt);
    fw_step(dt);
    for (int r = 0; r < NREEL; r++) { live |= r_flash_ms[r] > 0; dec(&r_flash_ms[r], dt); }

    if (s_screen == ST_PLAY) {
        if (s_pp == PP_SPIN) { step_reels(dt); live = true; }
        else if (s_pp == PP_WIN) {
            live = true;
            dec(&s_shake_ms, dt); dec(&s_flash_ms, dt);
            if (s_win_shown < s_win_nlines) {                          // reveal one winning line at a time
                s_win_step_ms -= dt;
                if (s_win_step_ms <= 0) {
                    win_reveal(s_win_shown++);
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
        } else if (s_auto && s_auto_ms > 0) {
            dec(&s_auto_ms, dt);
            if (s_auto_ms == 0) do_spin();
        }
    } else live = true;                                                                // ST_OVER: the blinking card
    // ~30 fps while something moves; else nothing is pushed
    if (!live || s_now - s_frame < (s_screen == ST_OVER ? 120 : 33)) return false;
    s_frame = s_now;
    s_anim++;
    return true;
}
static void on_enter(void)
{
    game_text_open("slots");
    cfg_read();
    view_set(nucleo_app_two_screens());
    build_strips();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    for (int r = 0; r < NREEL; r++) { r_phase[r] = 0; r_top[r] = (int)(esp_random() % SLEN); r_pos[r] = r_top[r]; r_flash_ms[r] = 0; }
    for (int i = 0; i < 5; i++) s_linewin[i] = false;
    memset(s_cellwin, 0, sizeof s_cellwin);
    s_win_nlines = 0; s_win_shown = 0; s_finale_done = false; s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_shx = s_shy = 0;
    s_quickstop = false; s_fin_age = 0;
    s_pp = PP_IDLE; s_auto = false; s_auto_ms = 0; s_msg_ms = 0;
    s_disp_bal = g_balance; s_anim = 0; s_menu = {};
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);
    go(ST_MENU);
    sfx(SFX_SEL);
}
static void on_exit(void) { nucleo_audio_stop(); settle_spin(); cfg_write(); game_text_close(); }

// Working RAM: allocated (zeroed) by the framework before on_enter, freed after on_exit. Foreground-only.
static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_fx, sizeof(Fx) },
    { (void **)&s_strip, sizeof(uint8_t) * NREEL * SLEN },
    { nullptr, 0 }
};

extern "C" void nucleo_register_slots(void)
{
    static const nucleo_app_def_t app = {
        "slots", "Slot", "Games", "Slot machine: gira e vinci il jackpot",
        '7', C_YELLOW, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP | NX_SOLO,  // NX_SOLO: reboot into a FRESH unfragmented heap (the 32KB canvas couldn't be
                               // re-acquired inline on the fragmented heap -> OOM/Task-WDT). NX_NET_APP bits are
                               // no-ops in Solo (services already down at boot) but kept defensively.
        APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
