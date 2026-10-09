// app_poker.cpp — NucleoOS "Poker": a 5-card draw VIDEO POKER (Jacks or Better 9/6, category "Games").
//
// A real video-poker machine on the Cardputer: five big readable cards dealt face-up, you HOLD the ones
// you keep and DRAW to replace the rest, and the hand is paid on the classic Jacks-or-Better paytable
// (royal flush 800/coin at max bet). The hold control is the five number keys 3 4 5 6 7 — the physical
// row right under the screen, one key per card. Cards flip when dealt/drawn (horizontal squash), held
// cards wear a gold frame + HOLD ribbon, winning cards jump and pulse, coins shower, the credit readout
// counts up, a tiered fanfare plays (small / big / jackpot).
//
// Console conventions (game_ui): title menu, three help pages (incl. the paytable), one settings list
// (from the menu, or TAB at the table), Esc at the table pauses and a second Esc leaves — a hand left
// half-way is played as it stands and paid (never swallowed), also when the app is closed. The game ends
// only when the credits run out; ENTER then restarts with the base bank. Text in five languages (GT).
// Sound: the PC-rendered pack (tools/sfx-gen/games/poker.py), never synthesized here.
// Never name a local `d` (it is the display macro).

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "nucleo_exclusive.h"   // NX_NET_APP: dedicate RAM + free the shared I2S line so SFX play
#include "game_text.h"
#include "game_ui.h"
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
static const uint16_t COL_FELT = RGB(0, 73, 36), COL_FELT2 = RGB(0, 36, 0), COL_DARK = RGB(0, 36, 0),
    COL_GOLD = RGB(255, 182, 85), COL_GOLDD = RGB(146, 109, 0), COL_GOLDL = RGB(255, 219, 170), COL_WHITE = 0xFFFF,
    COL_CREAM = RGB(255, 255, 170), COL_CARD = 0xFFFF, COL_CARDED = RGB(73, 73, 85), COL_HEART = RGB(219, 36, 0),
    COL_SPADE = RGB(36, 36, 85), COL_BACK = RGB(36, 73, 170), COL_BACK2 = RGB(73, 109, 255), COL_BACKED = RGB(0, 36, 85),
    COL_RED = RGB(255, 73, 85), COL_REDD = RGB(146, 36, 0), COL_CYAN = RGB(109, 219, 255), COL_GREEN = RGB(146, 255, 170),
    COL_GREY = RGB(146, 146, 170), COL_DIM = RGB(73, 109, 85), COL_SHADOW = RGB(0, 36, 0);
static inline uint16_t mix(uint16_t a, uint16_t b, int t) { return gui::mix(a, b, t); }

// ============================ geometry =======================================
#define NCARD   5
#define CARD_W  44
#define CARD_GAP 5
#define CARD_Y  19
#define CARD_H  76
static inline int card_x(int i)  { return i * (CARD_W + CARD_GAP); }   // 0,49,98,147,196
static inline int card_cx(int i) { return card_x(i) + CARD_W / 2; }    // 22,71,120,169,218

// ============================ cards / scoring ================================
// card id 0..51: suit = id/13 (0 spade,1 heart,2 diamond,3 club), rank = id%13 (0=2 .. 8=10,9=J,10=Q,11=K,12=A)
enum { SU_SPADE = 0, SU_HEART, SU_DIAMOND, SU_CLUB };
static const char *const RANKS[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
static inline bool suit_red(int s) { return s == SU_HEART || s == SU_DIAMOND; }

// hand categories — order matters: indexes PAY[] and hand_name()
enum { HAND_NONE = 0, HAND_JACKS, HAND_TWOPAIR, HAND_TRIPS, HAND_STRAIGHT,
       HAND_FLUSH, HAND_FULL, HAND_QUADS, HAND_SF, HAND_ROYAL, NHAND };
// Jacks-or-Better 9/6 full pay, per coin. Royal gets a max-bet bonus (800/coin at 5 coins) in pay_of().
static const int PAY[NHAND] = { 0, 1, 2, 3, 4, 6, 9, 25, 50, 250 };

static int eval_hand(const int *c)
{
    int rc[13] = {0}, sc[4] = {0};
    for (int i = 0; i < NCARD; i++) { rc[c[i] % 13]++; sc[c[i] / 13]++; }
    bool flush = false;
    for (int s = 0; s < 4; s++) if (sc[s] == NCARD) flush = true;
    bool straight = false; int hi = -1;
    for (int lo = 0; lo <= 8; lo++) {                       // five consecutive ranks
        bool ok = true;
        for (int k = 0; k < 5; k++) if (rc[lo + k] == 0) { ok = false; break; }
        if (ok) { straight = true; hi = lo + 4; }
    }
    if (!straight && rc[12] && rc[0] && rc[1] && rc[2] && rc[3]) { straight = true; hi = 3; } // wheel A-2-3-4-5
    int pairs = 0, trips = 0, quads = 0, jbPair = 0;
    for (int r = 0; r < 13; r++) {
        if (rc[r] == 2) { pairs++; if (r >= 9) jbPair = 1; }      // r>=9 -> J,Q,K,A
        else if (rc[r] == 3) trips++;
        else if (rc[r] == 4) quads++;
    }
    if (flush && straight) return hi == 12 ? HAND_ROYAL : HAND_SF; // ace-high straight flush = royal
    if (quads) return HAND_QUADS;
    if (trips && pairs) return HAND_FULL;
    if (flush) return HAND_FLUSH;
    if (straight) return HAND_STRAIGHT;
    if (trips) return HAND_TRIPS;
    if (pairs == 2) return HAND_TWOPAIR;
    if (pairs == 1 && jbPair) return HAND_JACKS;
    return HAND_NONE;
}
// 5-bit mask of the cards that actually make the scoring combo (for the win glow / the hold advice)
static int win_mask(const int *c, int cat)
{
    int rc[13] = {0}; for (int i = 0; i < NCARD; i++) rc[c[i] % 13]++;
    int m = 0, need = cat == HAND_QUADS ? 4 : cat == HAND_TRIPS ? 3 : 2;
    if (cat >= HAND_STRAIGHT && cat != HAND_QUADS) return 0x1F;
    for (int i = 0; i < NCARD; i++) { int r = c[i] % 13; if (rc[r] == need && (cat != HAND_JACKS || r >= 9)) m |= 1 << i; }
    return cat ? m : 0;
}

// ============================ state ==========================================
enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OVER };
// PLAY sub-phase (the deal/hold/draw/score lifecycle)
enum { PP_READY = 0, PP_DEAL, PP_HOLD, PP_DRAW, PP_RESULT };
#define START_CREDITS 200            // fresh-bank balance (restart amount when the credits run out)

static int   g_audio = 1, g_fast = 0, g_hint = 0;
static int   g_draws = 2;                            // dealing rounds: 2 = classic (deal + 1 draw), 3 = deal + 2 draws
static int   s_draw_n;                               // draws taken this hand (0 after the deal)
static int   g_balance = START_CREDITS, g_best = 0, g_bet = 1;   // bet = coins 1..5

static int   s_screen, s_set_from;                   // s_set_from: the screen settings return to (menu / table)
static bool  s_paused, s_confirm;                    // pause card at the table / "reset credits?" card
static gui::Menu s_menu, s_setm;
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;
static int   s_help;
static int   s_msg_ms; static char s_msg[28];       // short toast in the bottom strip (e.g. bet auto-lowered)
static float s_disp_bal;                             // eased credit readout (count-up)

// table
static int   s_card[NCARD];          // the 5 cards in hand (logical, updated immediately on draw)
static bool  s_hold[NCARD];
static int   s_pp, s_deckpos;
static int   s_sugg_mask;            // suggested holds (when g_hint)
static int   s_win_mask, s_win_total, s_hand_cat, s_win_tier;   // tier 0 small,1 big,2 jackpot

// per-card flip animation: a horizontal squash that swaps the shown side at the midpoint
static int   s_fa_ms[NCARD], s_fa_delay[NCARD];   // remaining flip ms + pre-flip stagger
static bool  s_face_now[NCARD], s_face_to[NCARD];  // currently-shown side / target side (true = face up)
static int   s_id_now[NCARD], s_id_to[NCARD];      // currently-shown face id / target id
static int   s_fa_dur;                              // flip duration (depends on g_fast)

// celebration
static int   s_shake_ms, s_flash_ms, s_coinwave_ms, s_fin_age, s_shx, s_shy, s_fw_ms;
static int   s_over_ms;               // age of the game-over sequence (drives the falling cards + the card)

// Working RAM (APP_RAM: allocated on open, freed on close — 0 B while the game is closed)
#define NPART 40
#define NFW 5
#define NFALL 11
struct Coin { float x, y, vx, vy; int life; uint16_t col; };
struct FW { int x, y, age; bool live; uint16_t col; };
struct Fall { float x, y, vx, vy, sp, spin; int id; };   // game over: a deck spilling down, each card tumbling
struct Fx { Coin coin[NPART]; FW fw[NFW]; Fall fall[NFALL]; uint8_t deck[52]; };
static Fx *s_fx;

static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static void go(int s);

// ============================ persistence ====================================
#define DIRR "/sd/data/poker"
#define CFG_MAGIC 0x504B5652u   // 'PKVR'
struct Cfg { uint32_t m; int lang_unused, a, fa, hi, bal, best, bet, dr; };   // layout kept: old saves still load
static void cfg_write(void)
{
    mkdir("/sd/data", 0777); mkdir(DIRR, 0777);
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    Cfg c = { CFG_MAGIC, 0, g_audio, g_fast, g_hint, g_balance, g_best, g_bet, g_draws };
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
        g_audio = c.a ? 1 : 0; g_fast = c.fa ? 1 : 0; g_hint = c.hi ? 1 : 0;
        g_balance = c.bal < 0 ? 0 : c.bal; g_best = c.best < 0 ? 0 : c.best;
        g_bet = (c.bet < 1 || c.bet > 5) ? 1 : c.bet;
        g_draws = (c.dr == 3) ? 3 : 2;
    }
}

// ============================ audio ==========================================
// The deployed pack (tools/sfx-gen/games/poker.py -> /sd/data/poker/pack). Never synthesized on the device
// (that ran on this task, past the 8 s Task-WDT); a cue whose WAV is missing plays a short tone instead.
enum { SFX_NAV = 1, SFX_SEL, SFX_BACK, SFX_HOLDON, SFX_HOLDOFF, SFX_DEAL, SFX_DRAW, SFX_WINS, SFX_WINB, SFX_ODE,
       SFX_NOWIN, SFX_BET, SFX_BONUS, SFX_BUST, NSFX = SFX_BUST };
static const char *sfx_name(int id)
{
    static const char *const N[NSFX + 1] = { "x", "nav", "sel", "back", "holdon", "holdf", "deal", "draw", "winS", "winB",
                                             "ode", "nowin", "bet", "bonus", "bust" };
    return id > 0 && id <= NSFX ? N[id] : "x";
}
static const uint16_t SFX_HZ[NSFX + 1] = { 0, 760, 988, 392, 1175, 440, 300, 247, 1568, 1046, 659, 196, 1318, 1568, 196 };
static bool sfx_important(int id) { return id == SFX_HOLDON || id == SFX_HOLDOFF || (id >= SFX_WINS && id != SFX_NOWIN && id != SFX_BET); }
static void sfx(int id)
{
    if (!g_audio || id <= 0 || id > NSFX) return;
    bool imp = sfx_important(id);
    if (!imp && nucleo_audio_is_playing()) return;
    char p[48]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id));
    struct stat st;
    if (imp) nucleo_audio_stop();
    if (stat(p, &st) != 0 || st.st_size <= 44 || nucleo_audio_play(p) != ESP_OK) nucleo_audio_tone(SFX_HZ[id], 45, 55);
}

// ============================ drawing helpers ================================
static int txt(const char *s, int x, int y, int datum, int font, uint16_t col) { return gui::text(s, x, y, datum, font, col, 0); }
static void draw_suit(int cx, int cy, int r, int suit, uint16_t col)
{
    switch (suit) {
        case SU_HEART:
            d.fillCircle(cx - r / 2, cy - r / 4, r / 2, col);
            d.fillCircle(cx + r / 2, cy - r / 4, r / 2, col);
            d.fillTriangle(cx - r, cy - r / 8, cx + r, cy - r / 8, cx, cy + r, col);
            break;
        case SU_DIAMOND:
            d.fillTriangle(cx, cy - r, cx - r * 3 / 4, cy, cx + r * 3 / 4, cy, col);
            d.fillTriangle(cx, cy + r, cx - r * 3 / 4, cy, cx + r * 3 / 4, cy, col);
            break;
        case SU_SPADE:
            d.fillTriangle(cx - r, cy + r / 6, cx + r, cy + r / 6, cx, cy - r, col);
            d.fillCircle(cx - r / 2, cy + r / 6, r / 2, col);
            d.fillCircle(cx + r / 2, cy + r / 6, r / 2, col);
            d.fillTriangle(cx - r / 3, cy + r, cx + r / 3, cy + r, cx, cy + r / 8, col);
            break;
        default:
            d.fillCircle(cx, cy - r / 2, r / 2, col);
            d.fillCircle(cx - r / 2, cy + r / 6, r / 2, col);
            d.fillCircle(cx + r / 2, cy + r / 6, r / 2, col);
            d.fillTriangle(cx - r / 3, cy + r, cx + r / 3, cy + r, cx, cy + r / 8, col);
            break;
    }
}
// the table: a dithered felt with a dark vignette edge
static void draw_felt(void)
{
    int ch = nucleo_app_content_height();
    gui::vgradient(0, 0, W, ch, RGB(0, 109, 36), COL_FELT2);
    for (int k = 0; k < 3; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(0, COL_FELT, k * 80));
}
// One card at x,y, cw wide (cw < CARD_W while it flips), face `id` or its back; details only when wide enough.
static void card_shape(int dx, int y, int cw, int h, bool face, int id, uint16_t tint)
{
    d.fillRoundRect(dx + 2, y + 3, cw, h, 5, COL_SHADOW);                                // drop shadow
    d.fillSmoothRoundRect(dx, y, cw, h, 5, face ? tint : COL_BACK);
    d.drawRoundRect(dx, y, cw, h, 5, face ? COL_CARDED : COL_BACKED);
    if (cw < h * 2 / 5) return;
    if (!face) {                                                                       // back: lattice + gold spade
        for (int yy = y + 5; yy < y + h - 5; yy += 6) d.drawFastHLine(dx + 4, yy, cw - 8, COL_BACK2);
        for (int xx = dx + 5; xx < dx + cw - 4; xx += 6) d.drawFastVLine(xx, y + 5, h - 10, COL_BACK2);
        draw_suit(dx + cw / 2, y + h / 2, 8, SU_SPADE, COL_GOLD);
        return;
    }
    d.drawFastHLine(dx + 5, y + 1, cw - 10, COL_WHITE);                                 // top highlight
    int rank = id % 13, suit = id / 13, cx = dx + cw / 2;
    uint16_t sc = suit_red(suit) ? COL_HEART : COL_SPADE;
    gui::text(RANKS[rank], dx + 3, y + 1, 0, gui::F_SMALL, sc, tint);                   // corner index
    draw_suit(dx + 7, y + 20, 3, suit, sc);
    draw_suit(dx + cw - 7, y + h - 9, 3, suit, sc);
    gui::text(RANKS[rank], cx, y + 23, 1, gui::F_TITLE, sc, tint);                      // big centre rank
    draw_suit(cx, y + 56, 9, suit, sc);                                                 // big centre suit
}
static void draw_card(int slot)
{
    int x = card_x(slot) + s_shx, y = CARD_Y + s_shy, w = CARD_W, h = CARD_H;   // s_shx/s_shy = jackpot shake
    bool held_show = s_hold[slot] && (s_pp == PP_HOLD || s_pp == PP_DRAW);   // keep the "held" look while the rest redeal
    bool win  = (s_pp == PP_RESULT) && (s_win_mask & (1 << slot));
    if (held_show) y -= 2;                                   // a small lift for kept cards
    if (win) { int lift = 2; if (s_fin_age < 520) lift += (int)(10.0f * sinf((float)s_fin_age / 520.0f * 3.14159f)); y -= lift; }  // winning cards JUMP up, then settle

    float scale = 1.0f;
    if (s_fa_delay[slot] <= 0 && s_fa_ms[slot] > 0) scale = fabsf(1.0f - 2.0f * (float)(s_fa_dur - s_fa_ms[slot]) / s_fa_dur);   // 1 -> edge-on -> 1
    int cw = (int)(w * scale); if (cw < 2) cw = 2;
    int cx = x + w / 2, dx = cx - cw / 2;
    card_shape(dx, y, cw, h, s_face_now[slot], s_id_now[slot], win && (s_anim & 8) ? COL_CREAM : COL_CARD);
    if (held_show && cw > w / 2) {                           // HOLD ribbon (stays through the draw)
        d.fillRect(dx + 1, y + h - 13, cw - 2, 12, COL_GOLD);
        txt(GT("TIENI", "HOLD"), cx, y + h - 15, 1, gui::F_SMALL, COL_DARK);
    }
    // frames: gold for held, pulsing gold + twinkles for a winning card
    uint16_t fc = held_show ? COL_GOLD : win ? mix(COL_GOLDD, COL_GOLDL, 120 + (int)(110 * sinf(s_anim * 0.4f))) : 0;
    if (fc) for (int k = 0; k < 2; k++) d.drawRoundRect(dx - 1 - k, y - 1 - k, cw + 2 + 2 * k, h + 2 + 2 * k, 6, fc);
    if (win) {
        int ex[4] = { dx - 3, dx + cw + 3, dx + 5, dx + cw - 5 }, ey[4] = { y + 9, y + 7, y + h + 3, y + h - 5 };
        for (int sp = 0; sp < 4; sp++) {
            int ph = (int)s_anim + sp * 4 + slot * 7;
            if ((ph % 10) >= 4) continue;
            uint16_t c = (ph & 1) ? COL_WHITE : COL_GOLDL;
            d.drawFastHLine(ex[sp] - 2, ey[sp], 5, c); d.drawFastVLine(ex[sp], ey[sp] - 2, 5, c);
        }
    }
}

// ============================ HUD + bottom strip =============================
static const char *hand_name(int cat)
{
    switch (cat) {
        case HAND_ROYAL:    return GT("SCALA REALE", "ROYAL FLUSH");
        case HAND_SF:       return GT("SCALA COLORE", "STRAIGHT FLUSH");
        case HAND_QUADS:    return GT("POKER", "FOUR OF A KIND");
        case HAND_FULL:     return GT("FULL", "FULL HOUSE");
        case HAND_FLUSH:    return GT("COLORE", "FLUSH");
        case HAND_STRAIGHT: return GT("SCALA", "STRAIGHT");
        case HAND_TRIPS:    return GT("TRIS", "THREE OF A KIND");
        case HAND_TWOPAIR:  return GT("DOPPIA COPPIA", "TWO PAIR");
        case HAND_JACKS:    return GT("COPPIA J+", "JACKS OR BETTER");
        default:            return GT("NESSUNA VINCITA", "NO WIN");
    }
}
static void draw_hud(void)
{
    char b[28];
    d.fillRect(0, 0, W, 16, COL_DARK);
    d.drawFastHLine(0, 15, W, COL_GOLDD);
    d.fillSmoothCircle(9, 8, 6, COL_GOLD); d.drawCircle(9, 8, 6, COL_GOLDD); d.drawCircle(9, 8, 3, COL_GOLDD);   // a coin
    snprintf(b, sizeof b, "%d", (int)(s_disp_bal + 0.5f));
    bool rolling = ((int)(s_disp_bal + 0.5f) != g_balance);
    txt(b, 19, -1, 0, gui::F_BODY, rolling && ((s_anim >> 1) & 1) ? COL_WHITE : COL_GOLDL);
    if (g_draws > 2 && s_pp == PP_HOLD) { snprintf(b, sizeof b, GT("Cambio %d/%d", "Draw %d/%d"), s_draw_n + 1, g_draws - 1); txt(b, W / 2 + 20, 0, 1, gui::F_SMALL, COL_CYAN); }
    snprintf(b, sizeof b, GT("Puntata %d", "Bet %d"), g_bet);
    txt(b, W - 4, 0, 2, gui::F_SMALL, COL_CREAM);
}
static void pill(const char *p)              // a prompt in the bottom strip, blinking gently
{
    int y = CARD_Y + CARD_H + 4, wpx = gui::text_width(p, gui::F_SMALL) + 18;
    d.fillSmoothRoundRect(W / 2 - wpx / 2, y, wpx, 17, 6, mix(COL_FELT, 0, 110));
    txt(p, W / 2, y, 1, gui::F_SMALL, ((s_now >> 9) & 1) ? COL_WHITE : COL_GOLDL);
}
// the bottom strip is context-aware: the 3-7 key badges while holding, a result bar on a score, prompts otherwise
static void draw_bottom(void)
{
    int y = CARD_Y + CARD_H + 3;       // 98
    if (s_msg_ms > 0) {                                                                // toast: solid + bordered = always readable
        gui::panel(2, y, W - 6, 19, COL_DARK, COL_GOLD);
        txt(s_msg, W / 2, y + 1, 1, gui::F_SMALL, COL_GOLDL);
        return;
    }
    if (s_pp == PP_HOLD || s_pp == PP_DEAL) {
        for (int i = 0; i < NCARD; i++) {
            int cx = card_cx(i); bool held = s_hold[i];
            d.fillSmoothRoundRect(cx - 11, y, 22, 18, 5, held ? COL_GOLD : mix(COL_FELT, 0, 110));
            d.drawRoundRect(cx - 11, y, 22, 18, 5, held ? COL_GOLDL : COL_DIM);
            char kk[2] = { (char)('3' + i), 0 };
            txt(kk, cx, y - 1, 1, gui::F_BODY, held ? COL_DARK : COL_CREAM);          // the physical key under the card
            if (g_hint && s_pp == PP_HOLD && (s_sugg_mask & (1 << i)) && !held) d.fillTriangle(cx, y - 2, cx - 4, y - 7, cx + 4, y - 7, COL_CYAN);  // hold-hint caret
        }
        return;
    }
    if (s_pp == PP_RESULT) {
        if (s_win_total > 0 && s_win_tier >= 1) { pill(GT("SPAZIO continua", "SPACE continue")); return; }   // big win: the banner tells
        bool won = s_win_total > 0;
        gui::panel(2, y, W - 6, 20, won ? mix(COL_DARK, COL_GOLD, 95) : mix(COL_DARK, COL_REDD, 85), won ? COL_GOLDL : COL_REDD);
        if (won) {
            txt(hand_name(s_hand_cat), 8, y + 2, 0, gui::F_SMALL, COL_CREAM);
            char b[16]; snprintf(b, sizeof b, "+%d", s_win_total);
            txt(b, W - 10, y, 2, gui::F_BODY, COL_GOLDL);
        } else txt(hand_name(s_hand_cat), W / 2, y, 1, gui::F_BODY, COL_GREY);
        return;
    }
    pill(s_pp == PP_DRAW ? GT("Cambio...", "Drawing...") : GT("SPAZIO per distribuire", "SPACE to deal"));
}

// ============================ coins / fireworks ==============================
static void coins_spawn(int n, int tier)
{
    for (int i = 0; i < NPART && n > 0; i++) {
        Coin &c = s_fx->coin[i];
        if (c.life > 0) continue;
        c.x = 20 + (int)(esp_random() % (W - 40));
        c.y = CARD_Y + CARD_H / 2;
        c.vx = ((int)(esp_random() % 200) - 100) / 40.0f;
        c.vy = -2.2f - (esp_random() % 100) / 60.0f - tier * 0.5f;
        c.life = 30 + (int)(esp_random() % 24);
        c.col = (esp_random() & 1) ? COL_GOLD : COL_GOLDL;
        n--;
    }
}
static void coins_step(int dt)
{
    float k = dt / 33.0f, floorY = (float)(CARD_Y + CARD_H - 2);
    for (int i = 0; i < NPART; i++) {
        Coin &c = s_fx->coin[i];
        if (c.life <= 0) continue;
        c.vy += 0.34f * k; c.x += c.vx * k; c.y += c.vy * k;
        if (c.y > floorY && c.vy > 0) { c.y = floorY; c.vy = -c.vy * 0.5f; c.vx *= 0.7f; }
        c.life -= dt;
    }
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
    static const uint16_t C[4] = { COL_GOLD, COL_CYAN, COL_GREEN, COL_HEART };
    for (int i = 0; i < NFW; i++) {
        FW &f = s_fx->fw[i];
        if (f.live) continue;
        f.live = true; f.age = 0;
        f.x = 30 + (int)(esp_random() % (W - 60));
        f.y = CARD_Y + 8 + (int)(esp_random() % (CARD_H - 24));
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

// ============================ deal / hold / draw / score =====================
static void shuffle_deck(void)
{
    uint8_t *k = s_fx->deck;
    for (int i = 0; i < 52; i++) k[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) { int j = (int)(esp_random() % (uint32_t)(i + 1)); uint8_t t = k[i]; k[i] = k[j]; k[j] = t; }
    s_deckpos = 0;
}
static void play_hint(void);
// Hold advice: the textbook Jacks-or-Better order, compacted — pat royal/straight flush/quads/full house;
// 4 to a royal; any other pat hand; two pair / trips; 4 to a straight flush; a high pair; 3 to a royal;
// 4 to a flush; a low pair; 4 to an outside straight; two suited high cards; the two lowest high cards;
// one high card; else draw five.
static int suggest_holds(void)
{
    int rc[13] = {0}, sm[4] = {0}, cat = eval_hand(s_card);
    for (int i = 0; i < NCARD; i++) { rc[s_card[i] % 13]++; sm[s_card[i] / 13] |= 1 << i; }
    if (cat >= HAND_FULL) return cat == HAND_QUADS ? win_mask(s_card, cat) : 0x1F;
    int royal = 0, rn = 0;                                           // most cards toward one royal (10..A suited)
    for (int s = 0; s < 4; s++) {
        int m = 0, n = 0; for (int i = 0; i < NCARD; i++) if ((sm[s] >> i & 1) && s_card[i] % 13 >= 8) { m |= 1 << i; n++; }
        if (n > rn) { rn = n; royal = m; }
    }
    if (rn == 4) return royal;
    if (cat >= HAND_STRAIGHT) return 0x1F;
    if (cat == HAND_TRIPS || cat == HAND_TWOPAIR) return win_mask(s_card, cat);
    for (int s = 0; s < 4; s++) {                                    // 4 to a straight flush (spread <= 4, or A-2-3-4-5)
        if (__builtin_popcount(sm[s]) != 4) continue;
        int lo = 13, hi = -1, lo2 = 13, hi2 = -1;
        for (int i = 0; i < NCARD; i++) if (sm[s] >> i & 1) { int r = s_card[i] % 13, r2 = r == 12 ? -1 : r; lo = r < lo ? r : lo; hi = r > hi ? r : hi; lo2 = r2 < lo2 ? r2 : lo2; hi2 = r2 > hi2 ? r2 : hi2; }
        if (hi - lo <= 4 || hi2 - lo2 <= 4) return sm[s];
    }
    if (cat == HAND_JACKS) return win_mask(s_card, cat);
    if (rn == 3) return royal;
    for (int s = 0; s < 4; s++) if (__builtin_popcount(sm[s]) == 4) return sm[s];
    for (int r = 0; r < 13; r++) if (rc[r] == 2) { int m = 0; for (int i = 0; i < NCARD; i++) if (s_card[i] % 13 == r) m |= 1 << i; return m; }
    for (int lo = 0; lo <= 7; lo++) {                                // 4 to an outside straight: 2-3-4-5 .. 9-10-J-Q
        if (!(rc[lo] && rc[lo + 1] && rc[lo + 2] && rc[lo + 3])) continue;
        int m = 0; for (int r = lo; r < lo + 4; r++) for (int i = 0; i < NCARD; i++) if (s_card[i] % 13 == r) { m |= 1 << i; break; }
        return m;
    }
    int hm = 0, n = 0;
    for (int i = 0; i < NCARD; i++) if (s_card[i] % 13 >= 9) { hm |= 1 << i; n++; }
    for (int s = 0; s < 4; s++) if (__builtin_popcount(sm[s] & hm) == 2) return sm[s] & hm;
    while (n > 2) {                                                  // keep the two lowest high cards
        int hi = -1; for (int i = 0; i < NCARD; i++) if ((hm >> i & 1) && (hi < 0 || s_card[i] % 13 > s_card[hi] % 13)) hi = i;
        hm &= ~(1 << hi); n--;
    }
    return hm;
}
static bool any_flipping(void) { for (int i = 0; i < NCARD; i++) if (s_fa_delay[i] > 0 || s_fa_ms[i] > 0) return true; return false; }
static void finish_flips(void) { for (int i = 0; i < NCARD; i++) { if (s_fa_delay[i] > 0 || s_fa_ms[i] > 0) { s_face_now[i] = s_face_to[i]; s_id_now[i] = s_id_to[i]; } s_fa_delay[i] = 0; s_fa_ms[i] = 0; } }
static void reset_table_ready(void)
{
    for (int i = 0; i < NCARD; i++) { s_face_now[i] = false; s_id_now[i] = 0; s_fa_ms[i] = 0; s_fa_delay[i] = 0; s_hold[i] = false; }
    s_win_mask = 0; s_win_total = 0; s_hand_cat = HAND_NONE; s_sugg_mask = 0;
    s_pp = PP_READY;
}
// Game over: out of credits. Spill the deck down the screen; the card with the restart prompt drops in.
static void go_over(void)
{
    s_over_ms = 0; s_shake_ms = 420;
    for (int i = 0; i < NFALL; i++) {
        Fall &f = s_fx->fall[i];
        f.x = 16 + (int)(esp_random() % (W - 32));
        f.y = -24.0f - (int)(esp_random() % 150);
        f.vx = ((int)(esp_random() % 160) - 80) / 60.0f;
        f.vy = 0.6f + (esp_random() % 120) / 80.0f;
        f.sp = (esp_random() % 628) / 100.0f;
        f.spin = (f.vx < 0 ? -1.0f : 1.0f) * (0.006f + (esp_random() % 60) / 9000.0f);
        f.id = (int)(esp_random() % 52);
    }
    sfx(SFX_BUST);
    go(ST_OVER);
}
// flip the unheld cards to new ones from the deck (the deal flips all five from their backs)
static void flip_in(bool deal)
{
    s_fa_dur = g_fast ? 150 : 260;
    int stag = g_fast ? 45 : 85, cnt = 0;
    for (int i = 0; i < NCARD; i++) {
        if (!deal && s_hold[i]) { s_fa_delay[i] = 0; s_fa_ms[i] = 0; continue; }   // kept: no flip
        if (deal) { s_hold[i] = false; s_face_now[i] = false; }
        s_card[i] = s_fx->deck[s_deckpos++];
        s_face_to[i] = true; s_id_to[i] = s_card[i];                     // swap at the midpoint (edge-on)
        s_fa_delay[i] = cnt * stag; s_fa_ms[i] = s_fa_dur; cnt++;
    }
}
static void do_deal(void)
{
    if (g_balance < g_bet) {
        if (g_balance <= 0) { go_over(); return; }
        g_bet = g_balance;                                                             // auto-lower the bet to what is affordable
        snprintf(s_msg, sizeof s_msg, GT("Puntata ridotta a %d", "Bet lowered to %d"), g_bet);
        s_msg_ms = 1100; sfx(SFX_BET);
    }
    g_balance -= g_bet;
    shuffle_deck();
    flip_in(true);
    s_win_mask = 0; s_win_total = 0; s_hand_cat = HAND_NONE;
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_fin_age = 0;
    for (int i = 0; i < NFW; i++) s_fx->fw[i].live = false;
    s_draw_n = 0;                                     // fresh hand: no draws taken yet
    s_pp = PP_DEAL; play_hint();
    nucleo_app_request_draw();
}
static void do_draw(void)
{
    flip_in(false);
    s_draw_n++;
    s_pp = PP_DRAW; play_hint();
    nucleo_app_request_draw();
}
// what the hand pays at the current bet (max-bet royal: 800 per coin)
static int pay_of(int cat) { return cat == HAND_ROYAL && g_bet >= 5 ? 4000 : PAY[cat] * g_bet; }
static void evaluate(void)
{
    int cat = eval_hand(s_card);
    s_hand_cat = cat;
    s_win_mask = win_mask(s_card, cat);
    int win = pay_of(cat);
    g_balance += win;
    s_win_total = win;
    if (win > g_best) g_best = win;
    int tier = cat >= HAND_QUADS ? 2 : (cat >= HAND_STRAIGHT ? 1 : (cat >= HAND_JACKS ? 0 : -1));
    s_win_tier = tier < 0 ? 0 : tier;
    s_fin_age = 0; s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0;
    for (int i = 0; i < NFW; i++) s_fx->fw[i].live = false;
    if (win <= 0)       sfx(SFX_NOWIN);
    else if (tier == 2) { sfx(SFX_ODE); s_flash_ms = 200; s_shake_ms = 600; s_coinwave_ms = 1800; s_fw_ms = 0; coins_spawn(40, 2); }   // jackpot: the anthem
    else if (tier == 1) { sfx(SFX_WINB); s_shake_ms = 280; s_coinwave_ms = 900; coins_spawn(26, 1); }
    else                { sfx(SFX_WINS); s_coinwave_ms = 360; coins_spawn(16, 0); }
    s_pp = PP_RESULT; play_hint();
    if (tier >= 1 || g_balance < g_bet) cfg_write();                  // persist notable wins / a drained bank
    nucleo_app_request_draw();
}
// Leaving the table (Esc, closing the app) with a hand in progress: it is played as it stands — the held
// cards kept, every remaining draw taken — and paid, so a bet is never swallowed.
static void settle_hand(void)
{
    if (s_pp == PP_READY || s_pp == PP_RESULT) return;
    finish_flips();
    while (s_draw_n < g_draws - 1) { flip_in(false); finish_flips(); s_draw_n++; }
    int win = pay_of(eval_hand(s_card));
    g_balance += win; if (win > g_best) g_best = win;
    reset_table_ready();
    cfg_write();
}

// ============================ screens: menu / help / settings ================
static void draw_menu(void)
{
    char sub[32]; snprintf(sub, sizeof sub, GT("Crediti %d", "Credits %d"), g_balance);
    int y = gui::title("POKER", sub, COL_GOLD);
    draw_suit(22, 15, 10, SU_HEART, COL_HEART); draw_suit(W - 22, 15, 10, SU_SPADE, COL_CREAM);
    const char *items[3] = { GT("Gioca", "Play"), GT("Come si gioca", "How to play"), GT("Impostazioni", "Settings") };
    gui::menu(s_menu, items, 3, y, nucleo_app_content_height(), COL_GOLD);
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    const char *titles[3] = { GT("Obiettivo", "Objective"), GT("Comandi", "Controls"), GT("Vincite", "Paytable") };
    int y = gui::title(titles[s_help], nullptr, COL_GOLD);
    if (s_help == 0) {
        txt(GT("Ricevi 5 carte. TIENI le buone", "You get 5 cards. HOLD the good"), 8, y, 0, gui::F_SMALL, COL_CREAM);
        txt(GT("con i tasti 3-7, poi CAMBIA", "ones with keys 3-7, then DRAW"), 8, y + 16, 0, gui::F_SMALL, COL_CREAM);
        txt(GT("le altre (SPAZIO).", "the rest (SPACE)."), 8, y + 32, 0, gui::F_SMALL, COL_CREAM);
        txt(GT("Coppia di J o meglio: vinci!", "Jacks or better: you win!"), 8, y + 50, 0, gui::F_SMALL, COL_GOLD);
    } else if (s_help == 1) {
        const char *k[4] = { "3 4 5 6 7", GT("SPAZIO", "SPACE"), GT("SU/GIU", "UP/DN"), "TAB  H" };
        const char *v[4] = { GT("tieni le carte", "hold cards"), GT("distribuisci/cambia", "deal / draw"), GT("puntata +/-", "bet +/-"), GT("opzioni  consiglio", "setup  hint") };
        for (int i = 0; i < 4; i++) { txt(k[i], 8, y + i * 17, 0, gui::F_SMALL, COL_GREY); txt(v[i], 92, y + i * 17, 0, gui::F_SMALL, COL_GOLDL); }
    } else {
        for (int i = 0; i < 9; i++) {
            int cat = HAND_ROYAL - i, col = i / 5, row = i % 5, x = 6 + col * 118, yy = y - 2 + row * 15;
            char b[8]; snprintf(b, sizeof b, "%d", PAY[cat]);
            d.setTextColor(i < 2 ? COL_GOLDL : COL_CREAM); d.setCursor(x, yy + 4); d.print(hand_name(cat));
            txt(b, x + 112, yy, 2, gui::F_SMALL, i < 2 ? COL_GOLDL : COL_GOLD);
        }
    }
    for (int i = 0; i < 3; i++) d.fillCircle(W / 2 - 12 + i * 12, ch - 5, i == s_help ? 3 : 2, i == s_help ? COL_GOLD : COL_DIM);
}
#define NSET 6   // Bet / Draws / Fast deal / Hold hint / Audio / Reset credits
static void draw_settings(void)
{
    char r[5][28];
    const char *on = GT("Si", "On"), *off = GT("No", "Off");
    snprintf(r[0], 28, "%s: %d", GT("Puntata", "Bet"), g_bet);
    snprintf(r[1], 28, "%s: %d", GT("Cambi", "Draws"), g_draws - 1);
    snprintf(r[2], 28, "%s: %s", GT("Veloce", "Fast deal"), g_fast ? on : off);
    snprintf(r[3], 28, "%s: %s", GT("Consiglio", "Hold hint"), g_hint ? on : off);
    snprintf(r[4], 28, "Audio: %s", g_audio ? on : off);
    const char *items[NSET] = { r[0], r[1], r[2], r[3], r[4], GT("Azzera crediti", "Reset credits") };
    int y = gui::title(GT("Impostazioni", "Settings"), nullptr, COL_GOLD);
    gui::menu(s_setm, items, NSET, y, nucleo_app_content_height(), COL_GOLD);
    if (s_confirm) {
        char l1[28]; snprintf(l1, sizeof l1, GT("Riparti con %d crediti", "Restart with %d credits"), START_CREDITS);
        gui::dialog(GT("Azzerare i crediti?", "Reset the credits?"), l1, nullptr, GT("INVIO si  Esc no", "ENTER yes  Esc no"), COL_RED);
    }
}
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    int shx = 0, shy = 0;
    if (s_shake_ms > 0) { shx = (int)((s_anim * 37) % 7) - 3; shy = (int)((s_anim * 53) % 7) - 3; }
    draw_felt();
    for (int i = 0; i < NFALL; i++) {                          // a deck spilling down, each card tumbling
        const Fall &f = s_fx->fall[i];
        float cs = cosf(f.sp);
        int w = (int)(26 * fabsf(cs)); if (w < 3) w = 3;
        int x = (int)f.x + shx, yy = (int)f.y + shy;
        if (yy < -26 || yy > ch + 26) continue;
        d.fillRoundRect(x - w / 2 + 2, yy - 14, w, 34, 4, COL_SHADOW);
        d.fillRoundRect(x - w / 2, yy - 16, w, 34, 4, cs >= 0 ? COL_CARD : COL_BACK);
        d.drawRoundRect(x - w / 2, yy - 16, w, 34, 4, cs >= 0 ? COL_CARDED : COL_BACKED);
        if (w > 13) { int suit = f.id / 13; draw_suit(x, yy, 6, cs >= 0 ? suit : SU_SPADE, cs < 0 ? COL_GOLD : suit_red(suit) ? COL_HEART : COL_SPADE); }
    }
    if (s_over_ms < 300) return;                               // the card drops in after the spill starts
    char l1[32]; snprintf(l1, sizeof l1, GT("Riparti con %d crediti", "Restart with %d credits"), START_CREDITS);
    gui::dialog(GT("CREDITI FINITI", "OUT OF CREDITS"), l1, nullptr, GT("INVIO ricomincia  Esc menu", "ENTER restart  Esc menu"), ((s_anim >> 2) & 1) ? COL_RED : COL_GOLD);
}

// ============================ screen: play ===================================
// rotating golden light rays behind the table — the big/jackpot celebration backdrop
static void draw_rays(void)
{
    int cx = W / 2, cy = CARD_Y + CARD_H / 2, R = 240;
    float rot = s_anim * 0.05f;
    for (int i = 0; i < 12; i += 2) {
        float a0 = rot + i * 0.5235988f, a1 = a0 + 0.34f;
        d.fillTriangle(cx, cy, cx + (int)(cosf(a0) * R), cy + (int)(sinf(a0) * R),
                       cx + (int)(cosf(a1) * R), cy + (int)(sinf(a1) * R), mix(COL_FELT, COL_GOLD, 52));
    }
}
// the WOW payoff: a banner that POPS in, HOLDS ~1s, then RETRACTS, so the winning hand can be admired
static void draw_win_banner(void)
{
    float v;
    if (s_fin_age < 220)       v = (float)s_fin_age / 220.0f;                  // pop in
    else if (s_fin_age < 1100) v = 1.0f;                                       // hold
    else if (s_fin_age < 1450) v = 1.0f - (float)(s_fin_age - 1100) / 350.0f;  // retract
    else return;
    float kf = 1.0f - (1.0f - v) * (1.0f - v);                                // ease
    const char *bw = hand_name(s_hand_cat);
    int fullw = gui::text_width(bw, gui::F_BODY) + 26, hh = (int)(42 * (0.45f + 0.55f * kf));
    int wpx = (int)(fullw * (0.35f + 0.65f * kf));
    if (wpx < 8 || hh < 6) return;
    int bxc = W / 2 + s_shx, by = CARD_Y + CARD_H / 2 - hh / 2 + s_shy;
    uint16_t bc = s_win_tier == 2 && ((s_anim >> 1) & 1) ? COL_HEART : COL_GOLD;
    d.fillSmoothRoundRect(bxc - wpx / 2, by, wpx, hh, 7, mix(0, bc, 80));
    for (int k = 0; k < 2; k++) d.drawRoundRect(bxc - wpx / 2 - k, by - k, wpx + 2 * k, hh + 2 * k, 7, COL_GOLDL);
    if (kf > 0.55f) {
        txt(bw, bxc, by + 1, 1, gui::F_BODY, COL_WHITE);
        char b[16]; snprintf(b, sizeof b, "+%d", s_win_total);
        txt(b, bxc, by + 19, 1, gui::F_BODY, COL_GOLDL);
    }
}
static void draw_play(void)
{
    s_shx = s_shy = 0;
    if (s_shake_ms > 0) { int m = s_shake_ms > 350 ? 3 : 2; s_shx = (int)((s_anim * 37) % (2 * m + 1)) - m; s_shy = (int)((s_anim * 53) % (2 * m + 1)) - m; }
    bool big = (s_pp == PP_RESULT && s_win_total > 0 && s_win_tier >= 1);
    int ch = nucleo_app_content_height();
    draw_felt();
    if (big) draw_rays();                                                              // golden backdrop behind the cards
    draw_hud();
    for (int i = 0; i < NCARD; i++) draw_card(i);
    draw_bottom();
    coins_draw();
    if (s_pp == PP_RESULT && s_win_tier == 2) fw_draw();
    if (s_flash_ms > 0) d.fillRect(0, 0, W, ch, mix(COL_FELT, COL_WHITE, s_flash_ms * 256 / 200));
    if (big) {
        int g = 110 + (int)(120 * sinf(s_anim * 0.4f));
        uint16_t gc = s_win_tier == 2 && ((s_anim / 6) & 1) ? COL_HEART : COL_GOLD;
        for (int k = 0; k < 3; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_FELT, gc, g - k * 30));
        draw_win_banner();                                                             // the popping payoff banner, on top
    }
    if (s_paused) gui::dialog(GT("PAUSA", "PAUSED"), s_pp == PP_READY || s_pp == PP_RESULT ? nullptr : GT("Uscendo, la mano si gioca", "Leaving plays the hand"),
                              nullptr, GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave"), COL_GOLD);
}

// ============================ input + hints ==================================
static void play_hint(void)
{
    const char *h;
    if (s_paused) h = GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave");
    else switch (s_pp) {
        case PP_HOLD:   h = GT("3-7 tieni  SPAZIO cambia  TAB opz.", "3-7 hold  SPACE draw  TAB setup"); break;
        case PP_DEAL:
        case PP_DRAW:   h = GT("SPAZIO salta", "SPACE skip"); break;
        default:        h = GT("SPAZIO distribuisci  su/giu punta", "SPACE deal  UP/DN bet  TAB setup"); break;
    }
    nucleo_app_set_hint(h);
}
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU: nucleo_app_set_hint(GT("su/giu  INVIO scegli  Esc esci", "UP/DN  ENTER pick  Esc quit")); break;
        case ST_HELP: nucleo_app_set_hint(GT("sx/dx pagine  Esc indietro", "L/R pages  Esc back")); break;
        case ST_SET:  nucleo_app_set_hint(GT("su/giu  sx/dx cambia  Esc ok", "UP/DN  L/R change  Esc done")); break;
        case ST_PLAY: play_hint(); break;
        default:      nucleo_app_set_hint(GT("INVIO ricomincia  Esc menu", "ENTER restart  Esc menu")); break;
    }
}
static void go(int s) { s_screen = s; s_paused = false; s_confirm = false; set_hint(); nucleo_app_request_draw(); }

static void open_settings(int from) { s_set_from = from; s_setm = {}; go(ST_SET); sfx(SFX_SEL); }
static void tab_handler(void)
{
    if (s_screen == ST_PLAY && !s_paused) open_settings(ST_PLAY);
    else if (s_screen == ST_SET && s_set_from == ST_PLAY && !s_confirm) { go(ST_PLAY); sfx(SFX_BACK); }
}
static void bet_step(int dir) { g_bet += dir; if (g_bet < 1) g_bet = 5; if (g_bet > 5) g_bet = 1; sfx(SFX_BET); }
static void settings_change(int dir)
{
    switch (s_setm.sel) {
        case 0: bet_step(dir); break;
        case 1: g_draws = g_draws == 2 ? 3 : 2; sfx(SFX_SEL); break;
        case 2: g_fast = !g_fast; sfx(SFX_SEL); break;
        case 3: g_hint = !g_hint; if (g_hint && s_pp == PP_HOLD) s_sugg_mask = suggest_holds(); sfx(SFX_SEL); break;
        case 4: g_audio = !g_audio; sfx(SFX_SEL); break;
        default: if (dir > 0) { s_confirm = true; sfx(SFX_SEL); } break;
    }
    cfg_write();
    nucleo_app_request_draw();
}
static void toggle_hold(int i)
{
    s_hold[i] = !s_hold[i];
    sfx(s_hold[i] ? SFX_HOLDON : SFX_HOLDOFF);
    nucleo_app_request_draw();
}
static void on_key(int k, char ch)
{
    nucleo_app_request_draw();
    switch (s_screen) {
        case ST_MENU:
            if (gui::menu_key(s_menu, k, 3)) sfx(SFX_NAV);
            else if (k == NK_ENTER || k == NK_RIGHT) {
                sfx(SFX_SEL);
                if (s_menu.sel == 0) { s_disp_bal = g_balance; reset_table_ready(); go(ST_PLAY); }
                else if (s_menu.sel == 1) { s_help = 0; go(ST_HELP); }
                else open_settings(ST_MENU);
            }
            return;
        case ST_HELP:
            if (k == NK_RIGHT)      { s_help = (s_help + 1) % 3; sfx(SFX_NAV); }
            else if (k == NK_ENTER) { sfx(SFX_BACK); go(ST_MENU); }
            return;
        case ST_SET:
            if (s_confirm) {
                if (k == NK_ENTER) { g_balance = START_CREDITS; s_disp_bal = START_CREDITS; s_confirm = false; cfg_write(); sfx(SFX_BONUS); }
                return;
            }
            if (gui::menu_key(s_setm, k, NSET)) sfx(SFX_NAV);
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change(+1);
            return;
        case ST_PLAY:
            if (s_paused) { if (k == NK_ENTER) { s_paused = false; play_hint(); } return; }
            if (s_pp == PP_READY || s_pp == PP_RESULT) {
                if (ch == ' ' || ch == '9' || k == NK_ENTER) do_deal();
                else if (k == NK_UP)   { bet_step(+1); cfg_write(); }
                else if (k == NK_DOWN) { bet_step(-1); cfg_write(); }
            } else if (s_pp == PP_HOLD) {
                if (ch >= '3' && ch <= '7') toggle_hold(ch - '3');
                else if (ch == ' ' || ch == '9' || k == NK_ENTER) do_draw();
                else if (ch == 'h' || ch == 'H') { g_hint = !g_hint; if (g_hint) s_sugg_mask = suggest_holds(); cfg_write(); sfx(SFX_SEL); }
            } else if (ch == ' ' || k == NK_ENTER) finish_flips();                     // skip the animation
            return;
        case ST_OVER:
            if (k == NK_ENTER || ch == ' ') { g_balance = START_CREDITS; s_disp_bal = g_balance; cfg_write(); sfx(SFX_BONUS); coins_spawn(16, 1); reset_table_ready(); go(ST_PLAY); }
            return;
        default: return;
    }
}
static bool on_back(int key)
{
    nucleo_app_request_draw();
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_HELP: s_help = (s_help + 2) % 3; sfx(SFX_NAV); return true;
            case ST_SET:  if (!s_confirm) settings_change(-1); return true;
            case ST_PLAY: if (!s_paused && (s_pp == PP_READY || s_pp == PP_RESULT)) { bet_step(-1); cfg_write(); } return true;
            default: return s_screen != ST_MENU;
        }
    }
    switch (s_screen) {
        case ST_MENU: return false;                                                    // Esc on the menu -> close the app
        case ST_SET:  if (s_confirm) s_confirm = false; else { go(s_set_from); sfx(SFX_BACK); } return true;
        case ST_PLAY:
            if (!s_paused) { s_paused = true; play_hint(); sfx(SFX_BACK); return true; }   // first Esc: the pause card
            settle_hand();                                                             // second: leave (the hand is played out)
            [[fallthrough]];
        default: sfx(SFX_BACK); s_menu = {}; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void step_flips(int dt)
{
    int half = s_fa_dur / 2;
    for (int i = 0; i < NCARD; i++) {
        if (s_fa_delay[i] > 0) {
            s_fa_delay[i] -= dt;
            if (s_fa_delay[i] <= 0) { s_fa_delay[i] = 0; if (s_fa_ms[i] > 0) sfx(s_pp == PP_DEAL ? SFX_DEAL : SFX_DRAW); }   // tick when the flip begins
            continue;
        }
        if (s_fa_ms[i] > 0) {
            int before = s_fa_ms[i];
            s_fa_ms[i] -= dt; if (s_fa_ms[i] < 0) s_fa_ms[i] = 0;
            if (before > half && s_fa_ms[i] <= half) { s_face_now[i] = s_face_to[i]; s_id_now[i] = s_id_to[i]; }  // swap side edge-on
        }
    }
}
static void on_draw(void)
{
    d.setClipRect(0, 0, W, nucleo_app_content_height());          // rays / spilled cards never paint the footer
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        default:      draw_over(); coins_draw(); break;
    }
    d.clearClipRect();
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last); if (dt < 0) dt = 0; if (dt > 200) dt = 200;
    s_last = s_now;
    if (s_screen == ST_MENU) return gui::menu_tick(s_menu, dt);                     // still menus push nothing
    if (s_screen == ST_SET)  return gui::menu_tick(s_setm, dt);
    if (s_screen == ST_HELP || s_paused) return false;

    float before = s_disp_bal;
    s_disp_bal += ((float)g_balance - s_disp_bal) * 0.18f;
    if (fabsf((float)g_balance - s_disp_bal) < 0.5f) s_disp_bal = g_balance;
    bool live = s_disp_bal != before || s_msg_ms > 0;
    if (s_msg_ms > 0) dec(&s_msg_ms, dt);
    coins_step(dt); fw_step(dt);
    for (int i = 0; i < NPART && !live; i++) live = s_fx->coin[i].life > 0;

    if (s_screen == ST_PLAY) {
        if (s_pp == PP_DEAL || s_pp == PP_DRAW) {
            step_flips(dt); live = true;
            if (!any_flipping()) {
                if (s_pp == PP_DEAL || s_draw_n < g_draws - 1) { s_pp = PP_HOLD; if (g_hint) s_sugg_mask = suggest_holds(); play_hint(); }   // (more) draws to go
                else evaluate();                                                                                                            // last draw -> score
            }
        } else if (s_pp == PP_RESULT && s_win_total > 0) {
            dec(&s_shake_ms, dt); dec(&s_flash_ms, dt); s_fin_age += dt;
            if (s_coinwave_ms > 0) { int pv = s_coinwave_ms; s_coinwave_ms -= dt; if (s_coinwave_ms / 150 != pv / 150) coins_spawn(4, 2); }
            if (s_win_tier == 2) { s_fw_ms -= dt; if (s_fw_ms <= 0) { fw_spawn(); s_fw_ms = 260; } }
            live = live || s_fin_age < 2500;                                           // the jump, the banner, the pulse
        }
    } else {                                                                           // ST_OVER
        s_over_ms += dt; dec(&s_shake_ms, dt); live = true;
        float k = dt / 16.0f; int ch = nucleo_app_content_height();
        for (int i = 0; i < NFALL; i++) {                                  // tumble the spilled deck down, wrapping at the bottom
            Fall &f = s_fx->fall[i];
            f.vy += 0.05f * k; f.x += f.vx * k; f.y += f.vy * k; f.sp += f.spin * dt;
            if (f.y > ch + 26) { f.y = -24.0f; f.x = 16 + (int)(esp_random() % (W - 32)); f.vy = 0.6f + (esp_random() % 120) / 80.0f; f.id = (int)(esp_random() % 52); }
        }
    }
    // ~30 fps while something moves; a waiting table only blinks its prompt (4 fps); else nothing is pushed
    int every = live ? 33 : s_screen == ST_PLAY && (s_pp == PP_READY || s_pp == PP_RESULT) ? 256 : 0;
    if (!every || s_now - s_frame < every) return false;
    s_frame = s_now; s_anim++;
    return true;
}
static void on_enter(void)
{
    game_text_open("poker");
    cfg_read();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    shuffle_deck();
    reset_table_ready();
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_shx = s_shy = 0; s_fin_age = 0; s_over_ms = 0;
    s_msg_ms = 0; s_disp_bal = g_balance; s_anim = 0; s_menu = {};
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);
    go(ST_MENU);
    sfx(SFX_SEL);
}
static void on_exit(void) { nucleo_audio_stop(); settle_hand(); cfg_write(); game_text_close(); }

static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_fx, sizeof(Fx) },
    { nullptr, 0 }
};

extern "C" void nucleo_register_poker(void)
{
    static const nucleo_app_def_t app = {
        "poker", "Poker", "Games", "Video poker 5 carte: tieni con 3-7 e cambia",
        'P', C_RED, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP,  // dedicate RAM + free the shared I2S/mic line so the SFX reliably play
        APP_RAM
    };
    nucleo_app_register(&app);
}
