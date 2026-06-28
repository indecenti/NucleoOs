// app_poker.cpp — NucleoOS "Poker": a juiced 5-card draw VIDEO POKER (Jacks or Better, category "Games").
//
// A real video-poker machine on the Cardputer: five BIG readable cards dealt face-up, then you HOLD the
// ones you keep and DRAW to replace the rest, and the hand is scored on the classic Jacks-or-Better
// paytable. The hold control is the five number keys 3 4 5 6 7 — the physical row right under the screen,
// one key per card, so the mapping is spatial: the key sits under the card it keeps. Everything is juiced
// like app_slots.cpp: cards flip when dealt/drawn (left-to-right, motion via horizontal squash), held cards
// wear a gold frame + TIENI/HOLD ribbon, winning cards pulse, a coin shower + tiered fanfare (small / big /
// JACKPOT) fire on a paying hand, the credit readout counts up.
//
// UX (project games conventions): MENU / HELP (3 pages incl. a paytable) / SETTINGS, bilingual IT/EN
// persisted to SD, lots of SFX. A TAB "Opzioni" panel holds bet/coins, fast deal, the optional hold hint
// and an audio toggle — so the table stays clean. It plays like a real machine: a win just pays and you
// keep dealing; the game ends only when the credits run out, then a keypress restarts with the base bank.
//
// Constraints honoured (same as app_slots.cpp): exclusive_flags = NX_NET_APP (dedicate RAM + free the
// shared I2S/mic line so the chiptune SFX reliably play); every bit of state is static (NO heap); drawing
// goes through the buffered `d.` path (one blit/frame — ANTI-FLICKER); 8bpp has no alpha so every glow is
// an integer channel-mix toward a solid colour. A ~30 Hz poll animates the flips and the celebration.
// LEFT/BACK reach on_back(), TAB reaches the tab handler, all other keys reach on_key(). ASCII text only.
// Never name a local `d` (it is the display macro).

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "nucleo_exclusive.h"   // NX_NET_APP: dedicate RAM + free the shared I2S line so SFX play
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define COL_FELT   rgb(12, 72, 52)
#define COL_FELT2  rgb(8, 52, 38)
#define COL_DARK   rgb(4, 26, 20)
#define COL_GOLD   rgb(255, 196, 64)
#define COL_GOLDD  rgb(150, 108, 24)
#define COL_GOLDL  rgb(255, 230, 150)
#define COL_WHITE  0xFFFF
#define COL_CREAM  rgb(255, 246, 214)
#define COL_CARD   rgb(248, 248, 240)
#define COL_CARDED rgb(60, 60, 72)
#define COL_HEART  rgb(214, 40, 48)    // red suit (hearts / diamonds)
#define COL_SPADE  rgb(26, 30, 44)     // black suit (spades / clubs)
#define COL_BACK   rgb(40, 70, 150)    // card back
#define COL_BACK2  rgb(72, 112, 200)
#define COL_BACKED rgb(20, 36, 90)
#define COL_RED    rgb(244, 70, 62)
#define COL_REDD   rgb(150, 26, 26)
#define COL_CYAN   rgb(96, 212, 236)
#define COL_GREEN  rgb(120, 232, 142)
#define COL_GREY   rgb(150, 162, 176)
#define COL_ORANGE rgb(255, 150, 50)
#define COL_DIM    rgb(70, 92, 82)
#define COL_PANEL  rgb(20, 24, 34)
#define COL_GLASS  rgb(16, 20, 30)

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
static const char *RANKS[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
static inline const char *rank_str(int r) { return RANKS[r]; }
static inline bool suit_red(int s) { return s == SU_HEART || s == SU_DIAMOND; }

// hand categories — order matters: indexes PAY[] and hand_name()
enum { HAND_NONE = 0, HAND_JACKS, HAND_TWOPAIR, HAND_TRIPS, HAND_STRAIGHT,
       HAND_FLUSH, HAND_FULL, HAND_QUADS, HAND_SF, HAND_ROYAL, NHAND };
// Jacks-or-Better 9/6 full pay, per coin. Royal gets a max-bet bonus (800/coin at 5 coins) in evaluate().
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
// 5-bit mask of the cards that actually make the scoring combo (for the win glow)
static int win_mask(const int *c, int cat)
{
    int rc[13] = {0}; for (int i = 0; i < NCARD; i++) rc[c[i] % 13]++;
    int m = 0;
    switch (cat) {
        case HAND_ROYAL: case HAND_SF: case HAND_STRAIGHT: case HAND_FLUSH: case HAND_FULL: return 0x1F;
        case HAND_QUADS:   for (int i = 0; i < NCARD; i++) if (rc[c[i] % 13] == 4) m |= 1 << i; break;
        case HAND_TRIPS:   for (int i = 0; i < NCARD; i++) if (rc[c[i] % 13] == 3) m |= 1 << i; break;
        case HAND_TWOPAIR: for (int i = 0; i < NCARD; i++) if (rc[c[i] % 13] == 2) m |= 1 << i; break;
        case HAND_JACKS:   for (int i = 0; i < NCARD; i++) { int r = c[i] % 13; if (rc[r] == 2 && r >= 9) m |= 1 << i; } break;
        default: break;
    }
    return m;
}

// ============================ state ==========================================
enum { ST_MENU = 0, ST_HELP, ST_SET, ST_PLAY, ST_OPT, ST_OVER };
// PLAY sub-phase (the deal/hold/draw/score lifecycle)
enum { PP_READY = 0, PP_DEAL, PP_HOLD, PP_DRAW, PP_RESULT };
#define START_CREDITS 200            // fresh-bank balance (restart amount when the credits run out)

static int   g_lang = 0, g_audio = 1, g_fast = 0, g_hint = 0;
static int   g_draws = 2;                            // total dealing rounds: 2 = classic (deal+1 draw), 3 = deal+2 draws
static int   s_draw_n;                               // draws taken this hand (0 after the deal)
static int   g_balance = START_CREDITS, g_best = 0, g_bet = 1;   // bet = coins 1..5

static int   s_screen;
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;

static int   s_msel, s_setsel, s_help, s_optsel;
static float s_capY;
static int   s_msg_ms; static const char *s_msg;
static char  s_msgbuf[28];           // backing store for dynamic toasts (e.g. bet auto-lowered)
static float s_disp_bal;             // eased credit readout (count-up)

// table
static int   s_deck[52], s_deckpos;
static int   s_card[NCARD];          // the 5 cards in hand (logical, updated immediately on draw)
static bool  s_hold[NCARD];
static int   s_pp;
static int   s_sugg_mask;            // suggested holds (when g_hint)
static int   s_win_mask, s_win_total, s_hand_cat, s_win_tier;   // tier 0 small,1 big,2 jackpot

// per-card flip animation: a horizontal squash that swaps the shown side at the midpoint
static int   s_fa_ms[NCARD], s_fa_delay[NCARD];   // remaining flip ms + pre-flip stagger
static bool  s_face_now[NCARD], s_face_to[NCARD];  // currently-shown side / target side (true = face up)
static int   s_id_now[NCARD], s_id_to[NCARD];      // currently-shown face id / target id
static int   s_fa_dur;                              // flip duration (depends on g_fast)

// celebration
static int   s_shake_ms, s_flash_ms, s_coinwave_ms, s_fin_age, s_shx, s_shy, s_fw_ms;
static int   s_over_ms;               // age of the game-over sequence (drives the falling cards + stamp pop)

// coin particles
#define NPART 40
struct Coin { float x, y, vx, vy; int life, max; uint16_t col; };
static Coin *s_coin = nullptr;   // heap-allocated on enter, freed on exit (zero RAM at boot)
// firework bursts (jackpot only)
#define NFW 5
struct FW { int x, y, age; bool live; uint16_t col; };
static FW s_fw[NFW];
// game-over: a deck spilling down the screen, each card tumbling (spin -> horizontal squash)
#define NFALL 11
struct Fall { float x, y, vx, vy, sp, spin; int id; };
static Fall s_fall[NFALL];

static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static void go(int s);

// ============================ persistence ====================================
#define DIRR "/sd/data/poker"
#define CFG_MAGIC 0x504B5652u   // 'PKVR'
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, fa, hi, bal, best, bet, dr; } c =
        { CFG_MAGIC, g_lang, g_audio, g_fast, g_hint, g_balance, g_best, g_bet, g_draws };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a, fa, hi, bal, best, bet, dr; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) {
        g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_fast = c.fa ? 1 : 0; g_hint = c.hi ? 1 : 0;
        g_balance = c.bal < 0 ? 0 : c.bal; g_best = c.best < 0 ? 0 : c.best;
        g_bet = (c.bet < 1 || c.bet > 5) ? 1 : c.bet;
        g_draws = (c.dr == 3) ? 3 : 2;
    }
}

// ============================ audio ==========================================
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "nav";  case 2: return "sel";  case 3: return "back"; case 4: return "holdon";
        case 5: return "holdf";case 6: return "deal"; case 7: return "draw"; case 8: return "winS";
        case 9: return "winB"; case 10:return "jack"; case 11:return "nowin";case 12:return "bet";
        case 13:return "casc"; case 14:return "chach";case 15:return "sprk";case 16:return "bonus";
        case 17:return "bust"; case 18:return "ode";
        default: return "x";
    }
}
#define NSFX 18
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1:  notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.55f; return 1;                                  // nav blip
        case 2:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.10f); return 2;   // select ding
        case 3:  notify__voice(&v[0], 587.33f, 0, 0.07f); notify__voice(&v[1], 392, 0.05f, 0.10f); return 2;       // back (down)
        case 4:  notify__voice(&v[0], 784, 0, 0.04f); notify__voice(&v[1], 1175, 0.03f, 0.06f); v[0].amp = 0.7f; return 2; // hold ON (up tick)
        case 5:  notify__voice(&v[0], 660, 0, 0.04f); notify__voice(&v[1], 440, 0.03f, 0.06f); v[0].amp = 0.7f; return 2;  // hold OFF (down tick)
        case 6:  notify__voice(&v[0], 300, 0, 0.05f); v[0].amp = 0.9f; notify__voice(&v[1], 1200, 0, 0.02f); return 2;     // deal slap
        case 7:  notify__voice(&v[0], 247, 0, 0.06f); v[0].amp = 0.9f; notify__voice(&v[1], 1400, 0, 0.02f); return 2;     // draw flip
        case 8:  notify__voice(&v[0], 880, 0, 0.06f); notify__voice(&v[1], 1174.7f, 0.05f, 0.06f);                 // small win pings
                 notify__voice(&v[2], 1567.98f, 0.10f, 0.10f); return 3;
        case 9:  notify__voice(&v[0], 523.25f, 0, 0.10f); notify__voice(&v[1], 659.25f, 0.08f, 0.10f);             // big win fanfare
                 notify__voice(&v[2], 783.99f, 0.16f, 0.12f); notify__voice(&v[3], 1046.5f, 0.24f, 0.16f);
                 notify__voice(&v[4], 1318.5f, 0.30f, 0.22f); return 5;
        case 10: notify__voice(&v[0], 523.25f, 0.00f, 0.12f); notify__voice(&v[1], 783.99f, 0.10f, 0.12f);         // JACKPOT — long, bright
                 notify__voice(&v[2], 1046.5f, 0.20f, 0.14f); notify__voice(&v[3], 1318.5f, 0.30f, 0.16f);
                 notify__voice(&v[4], 1567.98f, 0.40f, 0.20f); notify__voice(&v[5], 2093.0f, 0.50f, 0.34f); return 6;
        case 11: notify__voice(&v[0], 196, 0, 0.10f); v[0].amp = 0.4f; notify__voice(&v[1], 165, 0.06f, 0.10f); v[1].amp = 0.4f; return 2; // no-win
        case 12: notify__voice(&v[0], 880, 0, 0.04f); notify__voice(&v[1], 1318, 0.03f, 0.05f); v[0].amp = 0.7f; return 2; // bet tick
        case 13: notify__voice(&v[0], 1567.98f, 0.00f, 0.05f); notify__voice(&v[1], 1318.5f, 0.05f, 0.05f);        // coin cascade (rollup)
                 notify__voice(&v[2], 1046.5f, 0.10f, 0.05f); notify__voice(&v[3], 1318.5f, 0.16f, 0.05f);
                 notify__voice(&v[4], 1567.98f, 0.22f, 0.06f); notify__voice(&v[5], 2093.0f, 0.28f, 0.14f); return 6;
        case 14: notify__voice(&v[0], 1318.5f, 0.00f, 0.10f); notify__voice(&v[1], 1567.98f, 0.10f, 0.12f);        // cha-ching
                 notify__voice(&v[2], 1046.5f, 0.22f, 0.06f); notify__voice(&v[3], 1318.5f, 0.28f, 0.06f);
                 notify__voice(&v[4], 1760.0f, 0.34f, 0.16f); return 5;
        case 15: notify__voice(&v[0], 2093.0f, 0.00f, 0.07f); notify__voice(&v[1], 2637.02f, 0.05f, 0.10f); return 2; // sparkle
        case 16: notify__voice(&v[0], 1318.5f, 0, 0.06f); notify__voice(&v[1], 1567.98f, 0.06f, 0.08f);            // bonus / reset
                 notify__voice(&v[2], 880, 0.14f, 0.06f); notify__voice(&v[3], 1046.5f, 0.20f, 0.10f); return 4;
        case 17: notify__voice(&v[0], 392, 0.00f, 0.16f); notify__voice(&v[1], 330, 0.14f, 0.16f);                // bust — descending "game over" toll
                 notify__voice(&v[2], 262, 0.28f, 0.16f); notify__voice(&v[3], 196, 0.42f, 0.34f); v[3].amp = 0.9f; return 4;
        case 18: {   // Inno alla Gioia (Ode to Joy) — two-part polyphony (melody + bass), the win anthem
            static const float M[14] = { 659.25f,659.25f,698.46f,783.99f, 783.99f,698.46f,659.25f,587.33f, 523.25f,523.25f,587.33f,659.25f, 587.33f,523.25f };
            static const float B[14] = { 130.81f,130.81f,130.81f, 98.00f, 130.81f,130.81f, 98.00f, 98.00f,  87.31f,130.81f, 98.00f,130.81f,  98.00f,130.81f };
            float t = 0.0f; int n = 0;
            for (int i = 0; i < 14; i++) {
                float dur = (i == 13) ? 0.42f : 0.17f;
                notify__voice(&v[n], M[i], t, dur * 0.92f); v[n].amp = 0.90f; n++;     // melody
                notify__voice(&v[n], B[i], t, dur * 0.92f); v[n].amp = 0.45f; n++;     // bass under it -> polyphony
                t += dur;
            }
            return n;   // 28 voices
        }
    }
    return 0;
}
static bool sfx_important(int id) { return id == 4 || id == 5 || ((id >= 8 && id <= 18) && id != 11); }   // hold ticks + the win anthem always play crisply
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    char p[80];
    snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else { notify_voice_t v[40]; int nv = build_voices(id, v); if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return; }
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
// bump whenever a build_voices() clip changes so stale cached WAVs are rebuilt on next launch.
#define SFX_VER 3
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
    notify_voice_t v[40];
    for (int id = 1; id <= NSFX; id++) {
        char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ text helpers ===================================
static void text_at(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void text_c(int cx, int y, int sz, uint16_t col, const char *s) { text_at(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void text_r(int rx, int y, int sz, uint16_t col, const char *s) { text_at(rx - (int)strlen(s) * 6 * sz, y, sz, col, s); }

// ============================ suit pips ======================================
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
        case SU_CLUB:
            d.fillCircle(cx, cy - r / 2, r / 2, col);
            d.fillCircle(cx - r / 2, cy + r / 6, r / 2, col);
            d.fillCircle(cx + r / 2, cy + r / 6, r / 2, col);
            d.fillTriangle(cx - r / 3, cy + r, cx + r / 3, cy + r, cx, cy + r / 8, col);
            break;
    }
}

// ============================ background =====================================
static void draw_felt(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_FELT);
    for (int y = 0; y < ch; y += 6) d.drawFastHLine(0, y, W, COL_FELT2);
    for (int k = 0; k < 4; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_DARK, COL_FELT, k * 60));
}
static void bulbs(int x0, int x1, int y, int phase)
{
    int n = (x1 - x0) / 12;
    for (int i = 0; i <= n; i++) {
        int x = x0 + i * 12; bool on = ((i + phase) & 1);
        d.fillCircle(x, y, 2, on ? COL_GOLDL : COL_GOLDD);
        if (on) d.drawCircle(x, y, 3, mix(COL_GOLD, COL_FELT, 120));
    }
}

// ============================ one card =======================================
static void card_face_detail(int dx, int y, int cw, int h, int id)
{
    int rank = id % 13, suit = id / 13;
    uint16_t sc = suit_red(suit) ? COL_HEART : COL_SPADE;
    int cx = dx + cw / 2;
    const char *rs = rank_str(rank);
    text_at(dx + 3, y + 3, 1, sc, rs);                       // top-left index (rank + suit)
    draw_suit(dx + 6, y + 14, 3, suit, sc);
    text_r(dx + cw - 3, y + h - 11, 1, sc, rs);              // bottom-right index (mirrored, like a real card)
    draw_suit(dx + cw - 6, y + h - 18, 3, suit, sc);
    text_c(cx, y + 10, 3, sc, rs);                           // BIG centre rank
    draw_suit(cx, y + 45, 9, suit, sc);                      // BIG centre suit
}
static void card_back_detail(int dx, int y, int cw, int h)
{
    for (int yy = y + 5; yy < y + h - 5; yy += 6) d.drawFastHLine(dx + 4, yy, cw - 8, COL_BACK2);
    for (int xx = dx + 5; xx < dx + cw - 4; xx += 6) d.drawFastVLine(xx, y + 5, h - 10, COL_BACK2);
    draw_suit(dx + cw / 2, y + h / 2, 8, SU_SPADE, COL_GOLD);
}
static void draw_card(int slot)
{
    int x = card_x(slot) + s_shx, y = CARD_Y + s_shy, w = CARD_W, h = CARD_H;   // s_shx/s_shy = jackpot shake
    bool held = s_hold[slot];
    bool held_show = held && (s_pp == PP_HOLD || s_pp == PP_DRAW);   // keep the "held" look while the rest redeal
    bool win  = (s_pp == PP_RESULT) && (s_win_mask & (1 << slot));
    if (held_show) y -= 2;                                   // a small lift for kept cards
    if (win) { int lift = 2; if (s_fin_age < 520) lift += (int)(10.0f * sinf((float)s_fin_age / 520.0f * 3.14159f)); y -= lift; }  // winning cards JUMP up, then settle

    float scale = 1.0f;
    if (s_fa_delay[slot] <= 0 && s_fa_ms[slot] > 0) {
        float p = (float)(s_fa_dur - s_fa_ms[slot]) / s_fa_dur;  // 0..1
        scale = fabsf(1.0f - 2.0f * p);                          // 1 -> 0 (edge-on) -> 1
    }
    int cw = (int)(w * scale); if (cw < 2) cw = 2;
    int cx = x + w / 2, dx = cx - cw / 2;
    bool wide = cw > (w * 5) / 9;                                // enough width to show face/back details

    d.fillRoundRect(dx + 1, y + 3, cw, h, 5, mix(COL_FELT, COL_DARK, 150));   // drop shadow
    if (s_face_now[slot]) {
        d.fillRoundRect(dx, y, cw, h, 5, COL_CARD);
        d.drawRoundRect(dx, y, cw, h, 5, COL_CARDED);
        if (win) { int g = (s_anim & 8) ? 40 : 0; d.fillRoundRect(dx + 1, y + 1, cw - 2, h - 2, 5, mix(COL_CARD, COL_GOLDL, g)); }
        if (wide) card_face_detail(dx, y, cw, h, s_id_now[slot]);
        if (wide && held_show) {                                 // TIENI / HOLD ribbon (stays through the draw)
            d.fillRect(dx, y + h - 13, cw, 12, COL_GOLD);
            text_c(cx, y + h - 12, 1, COL_DARK, tx("TIENI", "HOLD"));
        }
    } else {
        d.fillRoundRect(dx, y, cw, h, 5, COL_BACK);
        d.drawRoundRect(dx, y, cw, h, 5, COL_BACKED);
        if (wide) card_back_detail(dx, y, cw, h);
    }
    // frames: gold for held (hold + draw phases), pulsing gold for a winning card
    if (held_show)
        for (int k = 0; k < 2; k++) d.drawRoundRect(dx - 1 - k, y - 1 - k, cw + 2 + 2 * k, h + 2 + 2 * k, 6, COL_GOLD);
    if (win) {
        int g = 120 + (int)(110 * sinf(s_anim * 0.4f));
        for (int k = 0; k < 2; k++) d.drawRoundRect(dx - 1 - k, y - 1 - k, cw + 2 + 2 * k, h + 2 + 2 * k, 6, mix(COL_GOLDD, COL_GOLDL, g));
        int ex[4] = { dx - 3, dx + cw + 3, dx + 5, dx + cw - 5 };       // twinkling sparkles framing the winning card
        int ey[4] = { y + 9, y + 7, y + h + 3, y + h - 5 };
        for (int sp = 0; sp < 4; sp++) {
            int ph = (int)s_anim + sp * 4 + slot * 7;
            if ((ph % 10) >= 4) continue;
            uint16_t sc = (ph & 1) ? COL_WHITE : COL_GOLDL;
            d.drawFastHLine(ex[sp] - 2, ey[sp], 5, sc);
            d.drawFastVLine(ex[sp], ey[sp] - 2, 5, sc);
        }
    }
}

// ============================ HUD + bottom strip =============================
static const char *hand_name(int cat)
{
    switch (cat) {
        case HAND_ROYAL:    return tx("SCALA REALE", "ROYAL FLUSH");
        case HAND_SF:       return tx("SCALA COLORE", "STRAIGHT FLUSH");
        case HAND_QUADS:    return tx("POKER", "FOUR OF A KIND");
        case HAND_FULL:     return tx("FULL", "FULL HOUSE");
        case HAND_FLUSH:    return tx("COLORE", "FLUSH");
        case HAND_STRAIGHT: return tx("SCALA", "STRAIGHT");
        case HAND_TRIPS:    return tx("TRIS", "THREE OF A KIND");
        case HAND_TWOPAIR:  return tx("DOPPIA COPPIA", "TWO PAIR");
        case HAND_JACKS:    return tx("COPPIA J+", "JACKS OR BETTER");
        default:            return tx("NESSUNA VINCITA", "NO WIN");
    }
}
static void draw_hud(void)
{
    char b[28];
    d.fillRect(0, 0, W, 16, COL_DARK);
    d.drawFastHLine(0, 15, W, COL_GOLDD);
    d.fillCircle(10, 8, 5, COL_GOLDD); d.fillCircle(10, 8, 4, COL_GOLD); d.drawCircle(10, 8, 5, COL_GOLDL);
    text_c(10, 3, 1, COL_DARK, "$");
    snprintf(b, sizeof b, "%d", (int)(s_disp_bal + 0.5f));
    bool rolling = ((int)(s_disp_bal + 0.5f) != g_balance);
    text_at(19, 1, 2, rolling && ((s_anim >> 1) & 1) ? COL_WHITE : COL_GOLDL, b);
    snprintf(b, sizeof b, "%s %d", tx("PUNT", "BET"), g_bet);
    text_r(W - 4, 4, 1, COL_CREAM, b);
}
// the bottom strip is context-aware: the 3-7 key badges while holding, a result bar on a score, prompts otherwise
static void draw_bottom(void)
{
    int y = CARD_Y + CARD_H + 3;       // 98
    if (s_pp == PP_HOLD || s_pp == PP_DEAL) {
        for (int i = 0; i < NCARD; i++) {
            int cx = card_cx(i); bool held = s_hold[i];
            bool sug = g_hint && s_pp == PP_HOLD && (s_sugg_mask & (1 << i)) && !held;
            uint16_t bg = held ? COL_GOLD : mix(COL_FELT, COL_DARK, 110);
            d.fillRoundRect(cx - 11, y, 22, 17, 4, bg);
            d.drawRoundRect(cx - 11, y, 22, 17, 4, held ? COL_GOLDL : COL_DIM);
            char kk[2] = { (char)('3' + i), 0 };
            text_c(cx, y + 2, 2, held ? COL_DARK : COL_CREAM, kk);    // the physical key digit under the card
            if (sug) d.fillTriangle(cx, y - 2, cx - 4, y - 7, cx + 4, y - 7, COL_CYAN);  // hold-hint caret
        }
        return;
    }
    if (s_pp == PP_RESULT) {
        if (s_win_total > 0 && s_win_tier >= 1) {                                       // big win: the banner shows it; here just the prompt
            const char *p = tx("SPAZIO continua", "SPACE continue");
            int wpx = (int)strlen(p) * 6 + 16;
            d.fillRoundRect(W / 2 - wpx / 2, y + 1, wpx, 16, 5, mix(COL_FELT, COL_GOLD, 60));
            text_c(W / 2, y + 5, 1, ((s_anim >> 3) & 1) ? COL_WHITE : COL_GOLDL, p);
            return;
        }
        // small win or no win — the continue prompt lives in the footer hint, so the bar never crowds
        if (s_win_total > 0) {
            d.fillRoundRect(2, y, W - 4, 19, 5, mix(COL_DARK, COL_GOLD, 95));
            d.drawRoundRect(2, y, W - 4, 19, 5, COL_GOLDL);
            text_at(8, y + 6, 1, COL_CREAM, hand_name(s_hand_cat));                     // size 1 left -> can't collide
            char b[16]; snprintf(b, sizeof b, "+%d", s_win_total);
            text_r(W - 8, y + 3, 2, COL_GOLDL, b);                                      // size 2 right -> prominent payout
        } else {
            d.fillRoundRect(2, y, W - 4, 19, 5, mix(COL_DARK, COL_REDD, 85));
            d.drawRoundRect(2, y, W - 4, 19, 5, mix(COL_REDD, COL_GREY, 90));
            text_c(W / 2, y + 4, 2, COL_GREY, hand_name(s_hand_cat));                   // "NESSUNA VINCITA" centred, nothing to overlap
        }
        return;
    }
    // PP_READY / PP_DRAW
    const char *p = s_pp == PP_DRAW ? tx("Cambio...", "Drawing...")
                                    : tx("SPAZIO per distribuire", "SPACE to deal");
    int wpx = (int)strlen(p) * 6 + 16;
    d.fillRoundRect(W / 2 - wpx / 2, y + 1, wpx, 16, 5, mix(COL_FELT, COL_DARK, 120));
    text_c(W / 2, y + 5, 1, ((s_anim >> 3) & 1) ? COL_WHITE : COL_GOLDL, p);
}

// ============================ coins / fireworks ==============================
static void coins_spawn(int n, int tier)
{
    if (!s_coin) return;
    for (int i = 0; i < NPART && n > 0; i++) {
        if (s_coin[i].life > 0) continue;
        s_coin[i].x = 20 + (int)(esp_random() % (W - 40));
        s_coin[i].y = CARD_Y + CARD_H / 2;
        s_coin[i].vx = ((int)(esp_random() % 200) - 100) / 40.0f;
        s_coin[i].vy = -2.2f - (esp_random() % 100) / 60.0f - tier * 0.5f;
        s_coin[i].max = s_coin[i].life = 30 + (int)(esp_random() % 24);
        s_coin[i].col = (esp_random() & 1) ? COL_GOLD : COL_GOLDL;
        n--;
    }
}
static void coins_step(int dt)
{
    if (!s_coin) return;
    float k = dt / 33.0f, floorY = (float)(CARD_Y + CARD_H - 2);
    for (int i = 0; i < NPART; i++) {
        if (s_coin[i].life <= 0) continue;
        s_coin[i].vy += 0.34f * k; s_coin[i].x += s_coin[i].vx * k; s_coin[i].y += s_coin[i].vy * k;
        if (s_coin[i].y > floorY && s_coin[i].vy > 0) { s_coin[i].y = floorY; s_coin[i].vy = -s_coin[i].vy * 0.5f; s_coin[i].vx *= 0.7f; }
        s_coin[i].life -= dt;
    }
}
static void coins_draw(void)
{
    if (!s_coin) return;
    for (int i = 0; i < NPART; i++) {
        if (s_coin[i].life <= 0) continue;
        int x = (int)s_coin[i].x, y = (int)s_coin[i].y;
        d.fillCircle(x, y, 3, COL_GOLDD); d.fillCircle(x, y, 2, s_coin[i].col); d.fillCircle(x - 1, y - 1, 1, COL_WHITE);
    }
}
static void fw_spawn(void)
{
    static const uint16_t C[4] = { COL_GOLD, COL_CYAN, COL_GREEN, COL_HEART };
    for (int i = 0; i < NFW; i++) {
        if (s_fw[i].live) continue;
        s_fw[i].live = true; s_fw[i].age = 0;
        s_fw[i].x = 30 + (int)(esp_random() % (W - 60));
        s_fw[i].y = CARD_Y + 8 + (int)(esp_random() % (CARD_H - 24));
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
        for (int k = 0; k < 10; k++) { float ang = k * 0.628318f; d.fillCircle(s_fw[i].x + (int)(cosf(ang) * rr), s_fw[i].y + (int)(sinf(ang) * rr), fade > 130 ? 2 : 1, c); }
        d.drawCircle(s_fw[i].x, s_fw[i].y, rr, mix(COL_FELT, COL_WHITE, fade / 2));
    }
}

// ============================ deal / hold / draw / score =====================
static void shuffle_deck(void)
{
    for (int i = 0; i < 52; i++) s_deck[i] = i;
    for (int i = 51; i > 0; i--) { int j = (int)(esp_random() % (uint32_t)(i + 1)); int t = s_deck[i]; s_deck[i] = s_deck[j]; s_deck[j] = t; }
    s_deckpos = 0;
}
static void play_hint(void);
// simplified Jacks-or-Better hold advice (a hint, not a perfect strategy): keep made hands, any pair
// (incl. low), 4-to-a-flush, else high cards.
static int suggest_holds(void)
{
    int rc[13] = {0}, sc[4] = {0};
    for (int i = 0; i < NCARD; i++) { rc[s_card[i] % 13]++; sc[s_card[i] / 13]++; }
    int cat = eval_hand(s_card);
    if (cat >= HAND_STRAIGHT) return 0x1F;                          // straight or better: hold all
    if (cat == HAND_TRIPS || cat == HAND_TWOPAIR) return win_mask(s_card, cat);
    for (int s = 0; s < 4; s++) if (sc[s] == 4) { int m = 0; for (int i = 0; i < NCARD; i++) if (s_card[i] / 13 == s) m |= 1 << i; return m; } // 4-flush
    int pm = 0; for (int r = 0; r < 13; r++) if (rc[r] == 2) for (int i = 0; i < NCARD; i++) if (s_card[i] % 13 == r) pm |= 1 << i;
    if (pm) return pm;                                             // any pair (even low)
    int hc = 0; for (int i = 0; i < NCARD; i++) if (s_card[i] % 13 >= 9) hc |= 1 << i;
    return hc;                                                      // high cards (J+), maybe 0 -> draw 5
}
static bool any_flipping(void) { for (int i = 0; i < NCARD; i++) if (s_fa_delay[i] > 0 || s_fa_ms[i] > 0) return true; return false; }
static void finish_flips(void) { for (int i = 0; i < NCARD; i++) { if (s_fa_delay[i] > 0 || s_fa_ms[i] > 0) { s_face_now[i] = s_face_to[i]; s_id_now[i] = s_id_to[i]; } s_fa_delay[i] = 0; s_fa_ms[i] = 0; } }
static void reset_table_ready(void)
{
    for (int i = 0; i < NCARD; i++) { s_face_now[i] = false; s_id_now[i] = 0; s_fa_ms[i] = 0; s_fa_delay[i] = 0; s_hold[i] = false; }
    s_win_mask = 0; s_win_total = 0; s_hand_cat = HAND_NONE; s_sugg_mask = 0;
    s_pp = PP_READY;
}
// Game over: out of credits. Spill the deck down the screen and slam a stamp in — a keypress restarts.
static void go_over(void)
{
    s_over_ms = 0; s_shake_ms = 420;
    for (int i = 0; i < NFALL; i++) {
        s_fall[i].x = 16 + (int)(esp_random() % (W - 32));
        s_fall[i].y = -24.0f - (int)(esp_random() % 150);
        s_fall[i].vx = ((int)(esp_random() % 160) - 80) / 60.0f;
        s_fall[i].vy = 0.6f + (esp_random() % 120) / 80.0f;
        s_fall[i].sp = (esp_random() % 628) / 100.0f;
        s_fall[i].spin = (s_fall[i].vx < 0 ? -1.0f : 1.0f) * (0.006f + (esp_random() % 60) / 9000.0f);
        s_fall[i].id = (int)(esp_random() % 52);
    }
    sfx(17);
    go(ST_OVER);
}
static void do_deal(void)
{
    if (g_balance < g_bet) {
        if (g_balance <= 0) { go_over(); return; }
        g_bet = g_balance;                                                             // auto-lower the bet to the affordable amount (1..4 here), then deal — no nagging
        cfg_write();
        snprintf(s_msgbuf, sizeof s_msgbuf, tx("Puntata ridotta a %d", "Bet lowered to %d"), g_bet);
        s_msg = s_msgbuf; s_msg_ms = 900; sfx(12);
    }
    g_balance -= g_bet;
    shuffle_deck();
    s_fa_dur = g_fast ? 150 : 260;
    int stag = g_fast ? 45 : 85;
    for (int i = 0; i < NCARD; i++) {
        s_card[i] = s_deck[s_deckpos++];
        s_hold[i] = false;
        s_face_now[i] = false; s_id_now[i] = 0;          // currently a back
        s_face_to[i] = true;  s_id_to[i] = s_card[i];     // flips up to the dealt card
        s_fa_delay[i] = i * stag; s_fa_ms[i] = s_fa_dur;
    }
    s_win_mask = 0; s_win_total = 0; s_hand_cat = HAND_NONE;
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_fin_age = 0;
    for (int i = 0; i < NFW; i++) s_fw[i].live = false;
    s_draw_n = 0;                                     // fresh hand: no draws taken yet
    s_pp = PP_DEAL; play_hint();
    nucleo_app_request_draw();
}
static void do_draw(void)
{
    s_fa_dur = g_fast ? 150 : 260;
    int stag = g_fast ? 45 : 85, cnt = 0;
    for (int i = 0; i < NCARD; i++) {
        if (s_hold[i]) { s_fa_delay[i] = 0; s_fa_ms[i] = 0; continue; }   // kept: no flip
        int nc = s_deck[s_deckpos++];
        s_card[i] = nc;
        s_face_to[i] = true; s_id_to[i] = nc;                            // face(old) -> face(new), swap at midpoint
        s_fa_delay[i] = cnt * stag; s_fa_ms[i] = s_fa_dur; cnt++;
    }
    s_draw_n++;                                       // one more draw taken
    s_pp = PP_DRAW; play_hint();
    nucleo_app_request_draw();
}
static void celebrate(int tier, int win)
{
    s_fin_age = 0; s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0;
    for (int i = 0; i < NFW; i++) s_fw[i].live = false;
    if (win <= 0) { sfx(11); return; }
    sfx(18);                                                                            // Inno alla Gioia (Ode to Joy) on every win
    if (tier == 2)      { s_flash_ms = 200; s_shake_ms = 600; s_coinwave_ms = 1800; s_fw_ms = 0; coins_spawn(40, 2); }
    else if (tier == 1) { s_shake_ms = 280; s_coinwave_ms = 900; coins_spawn(26, 1); }
    else                { s_coinwave_ms = 360; coins_spawn(16, 0); }
}
static void evaluate(void)
{
    int cat = eval_hand(s_card);
    s_hand_cat = cat;
    s_win_mask = win_mask(s_card, cat);
    int win = PAY[cat] * g_bet;
    if (cat == HAND_ROYAL && g_bet >= 5) win = 4000;                  // max-bet royal bonus (800/coin)
    g_balance += win;
    s_win_total = win;
    if (win > g_best) g_best = win;
    int tier = cat >= HAND_QUADS ? 2 : (cat >= HAND_STRAIGHT ? 1 : (cat >= HAND_JACKS ? 0 : -1));
    s_win_tier = tier < 0 ? 0 : tier;
    celebrate(tier, win);
    s_pp = PP_RESULT; play_hint();
    if (tier >= 1 || g_balance < g_bet) cfg_write();                  // persist notable wins / a drained bank
    nucleo_app_request_draw();
}

// ============================ screens: menu / help / settings ================
#define MENU_Y0 56
#define MENU_DY 23
static int menu_item_y(int i) { return MENU_Y0 + i * MENU_DY; }
static void draw_menu(void)
{
    draw_felt();
    bulbs(10, W - 10, 6, (s_anim / 6) & 1);
    draw_suit(20, 24, 10, SU_HEART, COL_HEART);
    draw_suit(W - 20, 24, 10, SU_SPADE, COL_CREAM);
    const char *title = "POKER";
    int lw = (int)strlen(title) * 18;
    text_at((W - lw) / 2 + 1, 11, 3, COL_GOLDD, title);
    text_at((W - lw) / 2, 10, 3, ((s_anim >> 3) & 1) ? COL_GOLDL : COL_GOLD, title);
    char bb[28]; snprintf(bb, sizeof bb, "%s %d", tx("Crediti", "Credits"), g_balance);
    text_c(W / 2, 38, 1, COL_CREAM, bb);
    const char *items[3] = { tx("Gioca", "Play"), tx("Come si gioca", "How to play"), tx("Impostazioni", "Settings") };
    int selw = (int)strlen(items[s_msel]) * 12;
    d.fillRoundRect((W - selw) / 2 - 10, (int)s_capY - 1, selw + 20, 17, 5, mix(COL_GLASS, COL_GOLD, 50));
    d.fillRoundRect((W - selw) / 2 - 6, (int)s_capY + 2, 3, 11, 1, COL_GOLDL);
    for (int i = 0; i < 3; i++) text_c(W / 2, menu_item_y(i), 2, i == s_msel ? COL_WHITE : COL_GREY, items[i]);
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    draw_felt();
    const char *titles[3] = { tx("Obiettivo", "Objective"), tx("Comandi", "Controls"), tx("Vincite", "Paytable") };
    text_c(W / 2, 6, 2, COL_GOLD, titles[s_help]);
    d.drawFastHLine(20, 26, W - 40, COL_GOLDD);
    if (s_help == 0) {
        text_at(14, 36, 1, COL_CREAM, tx("Ricevi 5 carte. TIENI quelle", "You get 5 cards. HOLD the ones"));
        text_at(14, 50, 1, COL_CREAM, tx("buone con i tasti 3-7, poi", "you like with keys 3-7, then"));
        text_at(14, 64, 1, COL_CREAM, tx("CAMBIA le altre (SPAZIO).", "DRAW the rest (SPACE)."));
        text_at(14, 82, 1, COL_GOLD, tx("Coppia di J o meglio = vinci.", "Jacks or better = you win."));
        text_at(14, 98, 1, COL_RED, tx("Scala Reale = JACKPOT!", "Royal Flush = JACKPOT!"));
    } else if (s_help == 1) {
        text_at(14, 36, 1, COL_GREY, "3 4 5 6 7");      text_at(120, 36, 1, COL_GOLDL, tx("tieni le carte", "hold cards"));
        text_at(14, 52, 1, COL_GREY, tx("SPAZIO/INVIO", "SPACE/ENTER")); text_at(120, 52, 1, COL_CYAN, tx("distribuisci/cambia", "deal / draw"));
        text_at(14, 68, 1, COL_GREY, tx("SU/GIU", "UP/DOWN"));   text_at(120, 68, 1, COL_GREEN, tx("punta +/-", "bet +/-"));
        text_at(14, 84, 1, COL_GREY, "TAB");            text_at(120, 84, 1, COL_GREEN, tx("opzioni", "options"));
        text_at(14, 100, 1, COL_GREY, "H");             text_at(120, 100, 1, COL_ORANGE, tx("suggerimento", "hold hint"));
    } else {
        const char *nm[9] = { tx("Scala Reale", "Royal Flush"), tx("Scala Colore", "Straight Flush"), tx("Poker", "Four Kind"),
                              tx("Full", "Full House"), tx("Colore", "Flush"), tx("Scala", "Straight"),
                              tx("Tris", "Three Kind"), tx("Doppia Coppia", "Two Pair"), tx("Coppia J+", "Jacks+") };
        int pv[9] = { 250, 50, 25, 9, 6, 4, 3, 2, 1 };
        for (int i = 0; i < 9; i++) {
            int col = i / 5, row = i % 5, x = 10 + col * 120, y = 32 + row * 14;
            text_at(x, y, 1, i < 2 ? COL_GOLDL : COL_CREAM, nm[i]);
            char b[10]; snprintf(b, sizeof b, "x%d", pv[i]);
            text_r(col == 0 ? 116 : 236, y, 1, i < 2 ? COL_GOLDL : COL_GOLD, b);        // keep both columns on-screen (no right-edge clip)
        }
    }
    int gap = 12, x0 = W / 2 - gap;
    for (int i = 0; i < 3; i++) d.fillCircle(x0 + i * gap, ch - 8, i == s_help ? 3 : 2, i == s_help ? COL_GOLD : COL_DIM);
}
static void draw_settings(void)
{
    draw_felt();
    text_c(W / 2, 8, 2, COL_GOLD, tx("Impostazioni", "Settings"));
    const char *names[6] = { tx("Lingua", "Language"), "Audio", tx("Distribuz. veloce", "Fast deal"), tx("Turni mano", "Draws"), tx("Azzera crediti", "Reset credits"), tx("Indietro", "Back") };
    char v[4][16];
    snprintf(v[0], 16, "%s", g_lang ? "English" : "Italiano");
    snprintf(v[1], 16, "%s", g_audio ? "On" : "Off");
    snprintf(v[2], 16, "%s", g_fast ? "On" : "Off");
    snprintf(v[3], 16, "%d", g_draws);
    d.fillRoundRect(16, (int)s_capY, W - 32, 17, 5, mix(COL_GLASS, COL_GOLD, 50));
    for (int i = 0; i < 6; i++) {
        int y = 28 + i * 17;
        text_at(26, y, 2, i == s_setsel ? COL_WHITE : COL_GREY, names[i]);
        if (i < 4) text_r(W - 18, y + 4, 1, COL_GOLD, v[i]);
    }
}
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    int shx = 0, shy = 0;
    if (s_shake_ms > 0) { int m = 3; shx = (int)((s_anim * 37) % (2 * m + 1)) - m; shy = (int)((s_anim * 53) % (2 * m + 1)) - m; }
    draw_felt();
    // a deck spilling down — each card tumbles about its vertical axis (width = |cos|, side flips edge-on)
    for (int i = 0; i < NFALL; i++) {
        float cs = cosf(s_fall[i].sp), sc = fabsf(cs);
        int w = (int)(26 * sc); if (w < 3) w = 3;
        int x = (int)s_fall[i].x + shx, yy = (int)s_fall[i].y + shy;
        if (yy < -26 || yy > ch + 26) continue;
        d.fillRoundRect(x - w / 2 + 1, yy - 15, w, 34, 4, mix(COL_FELT, COL_DARK, 150));   // shadow
        if (cs >= 0.0f) {                                                                  // face side
            d.fillRoundRect(x - w / 2, yy - 16, w, 34, 4, COL_CARD);
            d.drawRoundRect(x - w / 2, yy - 16, w, 34, 4, COL_CARDED);
            if (w > 13) { int suit = s_fall[i].id / 13; uint16_t c = suit_red(suit) ? COL_HEART : COL_SPADE; text_c(x, yy - 9, 1, c, rank_str(s_fall[i].id % 13)); draw_suit(x, yy + 6, 6, suit, c); }
        } else {                                                                           // back side
            d.fillRoundRect(x - w / 2, yy - 16, w, 34, 4, COL_BACK);
            d.drawRoundRect(x - w / 2, yy - 16, w, 34, 4, COL_BACKED);
            if (w > 13) draw_suit(x, yy, 7, SU_SPADE, COL_GOLD);
        }
    }
    // the stamp slams in (scale-up ease-out) with a flashing red/gold border
    float kf = s_over_ms < 280 ? (float)s_over_ms / 280 : 1.0f;
    kf = 1.0f - (1.0f - kf) * (1.0f - kf);
    const char *t1 = tx("CREDITI FINITI", "OUT OF CREDITS");
    int bw = (int)strlen(t1) * 12 + 26, hh = 52;
    int wpx = (int)(bw * (0.25f + 0.75f * kf)), hpx = (int)(hh * (0.25f + 0.75f * kf));
    int bx = W / 2 - wpx / 2 + shx, by = ch / 2 - hpx / 2 - 4 + shy;
    d.fillRoundRect(bx, by, wpx, hpx, 8, mix(COL_DARK, COL_REDD, 110));
    for (int k = 0; k < 3; k++) d.drawRoundRect(bx - k, by - k, wpx + 2 * k, hpx + 2 * k, 8, ((s_anim >> 2) & 1) ? COL_RED : COL_GOLD);
    if (kf > 0.6f) {
        text_c(W / 2 + shx, by + 8, 2, COL_WHITE, t1);
        char b[40]; snprintf(b, sizeof b, "%s %d", tx("Riparti con", "Restart with"), START_CREDITS);
        text_c(W / 2 + shx, by + hpx - 26, 1, COL_CREAM, b);
        text_c(W / 2 + shx, by + hpx - 14, 1, ((s_anim >> 2) & 1) ? COL_GOLDL : COL_GOLD, tx("INVIO per ricominciare", "ENTER to restart"));
    }
}

// ============================ screen: play + options panel ===================
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
// the WOW payoff: a banner that POPS in, HOLDS ~1s, then RETRACTS — so you celebrate, then get to
// admire the winning hand (the cards keep glowing once the banner is gone).
static void draw_win_banner(void)
{
    float v;
    if (s_fin_age < 220)       v = (float)s_fin_age / 220.0f;                  // pop in
    else if (s_fin_age < 1100) v = 1.0f;                                       // hold
    else if (s_fin_age < 1450) v = 1.0f - (float)(s_fin_age - 1100) / 350.0f;  // retract
    else return;                                                              // gone -> cards fully visible
    float kf = 1.0f - (1.0f - v) * (1.0f - v);                                // ease
    const char *bw = hand_name(s_hand_cat);
    int fullw = (int)strlen(bw) * 12 + 22, hh = (int)(34 * (0.45f + 0.55f * kf));
    int wpx = (int)(fullw * (0.35f + 0.65f * kf));
    if (wpx < 8 || hh < 6) return;
    int bxc = W / 2 + s_shx, by = CARD_Y + CARD_H / 2 - hh / 2 + s_shy;
    uint16_t bc = s_win_tier == 2 ? (((s_anim >> 1) & 1) ? COL_HEART : COL_GOLD) : COL_GOLD;
    d.fillRoundRect(bxc - wpx / 2, by, wpx, hh, 6, mix(COL_DARK, bc, 95));
    for (int k = 0; k < 2; k++) d.drawRoundRect(bxc - wpx / 2 - k, by - k, wpx + 2 * k, hh + 2 * k, 6, COL_GOLDL);
    if (kf > 0.55f) {
        text_c(bxc, by + hh / 2 - 13, 2, COL_WHITE, bw);
        char b[20]; snprintf(b, sizeof b, "+%d", s_win_total);
        text_c(bxc, by + hh / 2 + 3, 2, COL_GOLDL, b);
    }
}
static void draw_play(void)
{
    s_shx = s_shy = 0;
    if (s_shake_ms > 0) { int m = s_shake_ms > 350 ? 3 : 2; s_shx = (int)((s_anim * 37) % (2 * m + 1)) - m; s_shy = (int)((s_anim * 53) % (2 * m + 1)) - m; }
    bool big = (s_pp == PP_RESULT && s_win_total > 0 && s_win_tier >= 1);
    draw_felt();
    if (big) draw_rays();                                                              // golden backdrop behind the cards
    draw_hud();
    for (int i = 0; i < NCARD; i++) draw_card(i);
    draw_bottom();
    coins_draw();
    if (s_pp == PP_RESULT && s_win_tier == 2) fw_draw();
    if (s_flash_ms > 0) { int ch = nucleo_app_content_height(); d.fillRect(0, 0, W, ch, mix(COL_FELT, COL_WHITE, s_flash_ms * 256 / 200)); }
    if (s_pp == PP_RESULT && s_win_total > 0 && s_win_tier >= 1) {
        int ch = nucleo_app_content_height(), g = 110 + (int)(120 * sinf(s_anim * 0.4f));
        uint16_t gc = s_win_tier == 2 ? (((s_anim / 6) & 1) ? COL_HEART : COL_GOLD) : COL_GOLD;
        for (int k = 0; k < 3; k++) d.drawRect(k, k, W - 2 * k, ch - 2 * k, mix(COL_FELT, gc, g - k * 30));
    }
    if (big) draw_win_banner();                                                        // the popping payoff banner, on top
    if (s_msg_ms > 0) {                                                                // toast lives in the status strip (never over the cards); solid + bordered = always readable
        int ty = CARD_Y + CARD_H + 3;
        d.fillRoundRect(2, ty, W - 4, 18, 5, COL_DARK);
        d.drawRoundRect(2, ty, W - 4, 18, 5, COL_GOLD);
        text_c(W / 2, ty + 5, 1, COL_GOLDL, s_msg);
    }
}
#define NOPT 4   // Bet / Fast deal / Hint / Audio
static void draw_options(void)
{
    draw_play();
    int ch = nucleo_app_content_height();
    int px = 26, py = 8, pw = W - 52, phh = ch - 16;
    d.fillRoundRect(px, py, pw, phh, 7, COL_PANEL);
    d.drawRoundRect(px, py, pw, phh, 7, COL_GOLD);
    bulbs(px + 8, px + pw - 8, py + 5, (s_anim / 6) & 1);
    text_c(W / 2, py + 10, 2, COL_GOLD, tx("OPZIONI", "OPTIONS"));
    const char *names[NOPT] = { tx("Puntata", "Bet"), tx("Veloce", "Fast deal"), tx("Suggerim.", "Hold hint"), "Audio" };
    char vals[NOPT][12];
    snprintf(vals[0], 12, "%d", g_bet);
    snprintf(vals[1], 12, "%s", g_fast ? "On" : "Off");
    snprintf(vals[2], 12, "%s", g_hint ? "On" : "Off");
    snprintf(vals[3], 12, "%s", g_audio ? "On" : "Off");
    int y0 = py + 32;
    for (int i = 0; i < NOPT; i++) {
        int y = y0 + i * 17;
        if (i == s_optsel) d.fillRoundRect(px + 6, y - 3, pw - 12, 17, 4, mix(COL_PANEL, COL_GOLD, 60));
        text_at(px + 14, y, 1, i == s_optsel ? COL_WHITE : COL_GREY, names[i]);
        text_r(px + pw - 28, y, 1, COL_GOLDL, vals[i]);
        if (i == s_optsel) { text_at(px + pw - 22, y, 1, COL_GOLD, "<"); text_at(px + pw - 12, y, 1, COL_GOLD, ">"); }
    }
    char tb[24]; snprintf(tb, sizeof tb, "%s %d", tx("Max vinto", "Best"), g_best);
    text_c(W / 2, py + phh - 14, 1, COL_CREAM, tb);
}

// ============================ input + hints ==================================
static void play_hint(void)
{
    switch (s_pp) {
        case PP_HOLD:
            if (g_draws > 2) {
                static char hb[72];
                snprintf(hb, sizeof hb, tx("3-7 tieni  9/SPAZIO cambio %d/%d  TAB opz", "3-7 hold  9/SPACE draw %d/%d  TAB opt"), s_draw_n + 1, g_draws - 1);
                nucleo_app_set_hint(hb);
            } else nucleo_app_set_hint(tx("3-7 tieni   9/SPAZIO cambia   TAB opzioni", "3-7 hold   9/SPACE draw   TAB options"));
            break;
        case PP_DEAL:
        case PP_DRAW:   nucleo_app_set_hint(tx("SPAZIO salta animazione", "SPACE skip animation")); break;
        case PP_RESULT: nucleo_app_set_hint(tx("9/SPAZIO continua   SU/GIU punta", "9/SPACE continue   UP/DN bet")); break;
        default:        nucleo_app_set_hint(tx("9/SPAZIO distribuisci   SU/GIU punta   TAB opzioni", "9/SPACE deal   UP/DN bet   TAB options")); break;
    }
}
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU:   nucleo_app_set_hint(tx("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_HELP:   nucleo_app_set_hint(tx("SX/DX pagine  Esc indietro", "LEFT/RIGHT pages  Esc back")); break;
        case ST_SET:    nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  Esc", "UP/DN  ENTER change  Esc")); break;
        case ST_PLAY:   play_hint(); break;
        case ST_OPT:    nucleo_app_set_hint(tx("SU/GIU  SX/DX cambia  TAB chiudi", "UP/DN  LEFT/RIGHT change  TAB close")); break;
        case ST_OVER:   nucleo_app_set_hint(tx("INVIO per ricominciare", "ENTER to restart")); break;
        default: break;
    }
}
static float cap_target(void)
{
    if (s_screen == ST_MENU) return (float)menu_item_y(s_msel);
    if (s_screen == ST_SET)  return (float)(26 + s_setsel * 17);
    return s_capY;
}
static void go(int s) { s_screen = s; s_capY = cap_target(); set_hint(); nucleo_app_request_draw(); }

static void tab_handler(void)
{
    if (s_screen == ST_PLAY) { s_optsel = 0; go(ST_OPT); sfx(2); }
    else if (s_screen == ST_OPT) { go(ST_PLAY); sfx(3); }
}
static void opt_change(int dir)
{
    switch (s_optsel) {
        case 0: g_bet += dir; if (g_bet < 1) g_bet = 5; if (g_bet > 5) g_bet = 1; sfx(12); break;
        case 1: g_fast = !g_fast; sfx(2); break;
        case 2: g_hint = !g_hint; if (g_hint && s_pp == PP_HOLD) s_sugg_mask = suggest_holds(); sfx(2); break;
        case 3: g_audio = !g_audio; sfx(2); break;
        default: break;
    }
    cfg_write();
    nucleo_app_request_draw();
}
static void menu_select(void)
{
    sfx(2);
    if (s_msel == 0) { s_disp_bal = g_balance; reset_table_ready(); go(ST_PLAY); }
    else if (s_msel == 1) { s_help = 0; go(ST_HELP); }
    else { s_setsel = 0; go(ST_SET); }
}
static void settings_change(void)
{
    switch (s_setsel) {
        case 0: g_lang ^= 1; cfg_write(); set_hint(); sfx(2); break;
        case 1: g_audio ^= 1; cfg_write(); sfx(2); break;
        case 2: g_fast ^= 1; cfg_write(); sfx(2); break;
        case 3: g_draws = (g_draws == 2) ? 3 : 2; cfg_write(); sfx(2); break;       // turni mano 2 <-> 3
        case 4: g_balance = START_CREDITS; s_disp_bal = START_CREDITS; cfg_write(); sfx(16); break;
        default: sfx(3); s_msel = 2; go(ST_MENU); return;
    }
    nucleo_app_request_draw();
}
static void toggle_hold(int i)
{
    if (i < 0 || i >= NCARD) return;
    s_hold[i] = !s_hold[i];
    sfx(s_hold[i] ? 4 : 5);
    nucleo_app_request_draw();
}
static void play_bet(int dir)   // change coins from the table (only between hands)
{
    g_bet += dir; if (g_bet < 1) g_bet = 5; if (g_bet > 5) g_bet = 1;
    cfg_write(); sfx(12); nucleo_app_request_draw();
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
            if (k == NK_UP)        { s_setsel = (s_setsel + 5) % 6; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % 6; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change();
            return;
        case ST_PLAY:
            if (s_pp == PP_READY || s_pp == PP_RESULT) {
                if (ch == ' ' || ch == '9' || k == NK_ENTER) do_deal();          // '9' also (re)deals
                else if (k == NK_UP)   play_bet(+1);
                else if (k == NK_DOWN) play_bet(-1);
            } else if (s_pp == PP_HOLD) {
                if (ch >= '3' && ch <= '7') toggle_hold(ch - '3');
                else if (ch == ' ' || ch == '9' || k == NK_ENTER) do_draw();     // '9' also redeals (draw)
                else if (ch == 'h' || ch == 'H') { g_hint = !g_hint; if (g_hint) s_sugg_mask = suggest_holds(); cfg_write(); sfx(2); nucleo_app_request_draw(); }
            } else if (s_pp == PP_DEAL || s_pp == PP_DRAW) {
                if (ch == ' ' || k == NK_ENTER) { finish_flips(); nucleo_app_request_draw(); }   // skip the animation
            }
            return;
        case ST_OPT:
            if (k == NK_UP)        { s_optsel = (s_optsel + NOPT - 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_optsel = (s_optsel + 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_RIGHT || k == NK_ENTER) opt_change(+1);
            return;
        case ST_OVER:
            if (k == NK_ENTER || ch == ' ') { g_balance = START_CREDITS; s_disp_bal = g_balance; cfg_write(); sfx(16); coins_spawn(16, 1); reset_table_ready(); go(ST_PLAY); }
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
            case ST_OPT:  opt_change(-1); return true;
            case ST_PLAY: if (s_pp == PP_READY || s_pp == PP_RESULT) play_bet(-1); return true;
            default: break;
        }
    }
    switch (s_screen) {
        case ST_MENU: return false;                                   // Esc on menu -> close app
        case ST_OPT:  go(ST_PLAY); sfx(3); return true;
        default: sfx(3); s_msel = 0; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void step_flips(int dt)
{
    int half = s_fa_dur / 2;
    for (int i = 0; i < NCARD; i++) {
        if (s_fa_delay[i] > 0) {
            s_fa_delay[i] -= dt;
            if (s_fa_delay[i] <= 0) { s_fa_delay[i] = 0; if (s_fa_ms[i] > 0) sfx(s_pp == PP_DEAL ? 6 : 7); }   // tick when the flip begins
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
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        case ST_OPT:  draw_options(); break;
        case ST_OVER: draw_over(); coins_draw(); break;
        default: break;
    }
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last); if (dt < 0) dt = 0; if (dt > 200) dt = 200;
    s_last = s_now;
    bool menu_glide = false;

    s_disp_bal += ((float)g_balance - s_disp_bal) * 0.18f;
    if (fabsf((float)g_balance - s_disp_bal) < 0.5f) s_disp_bal = g_balance;
    if (s_msg_ms > 0) dec(&s_msg_ms, dt);
    coins_step(dt); fw_step(dt);

    if (s_screen == ST_PLAY || s_screen == ST_OPT) {
        if (s_pp == PP_DEAL) {
            step_flips(dt);
            if (!any_flipping()) { s_pp = PP_HOLD; if (g_hint) s_sugg_mask = suggest_holds(); play_hint(); }
        } else if (s_pp == PP_DRAW) {
            step_flips(dt);
            if (!any_flipping()) {
                if (s_draw_n < g_draws - 1) { s_pp = PP_HOLD; if (g_hint) s_sugg_mask = suggest_holds(); play_hint(); }   // more draws to go -> hold again
                else evaluate();                                                                                          // last draw -> score the hand
            }
        } else if (s_pp == PP_RESULT) {
            dec(&s_shake_ms, dt); dec(&s_flash_ms, dt); s_fin_age += dt;
            if (s_coinwave_ms > 0) { int pv = s_coinwave_ms; s_coinwave_ms -= dt; if (s_coinwave_ms / 150 != pv / 150) coins_spawn(4, 2); }
            if (s_win_tier == 2 && s_win_total > 0) { s_fw_ms -= dt; if (s_fw_ms <= 0) { fw_spawn(); s_fw_ms = 260; } }
        }
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        float tgt = cap_target();
        if (s_capY != tgt) { s_capY += (tgt - s_capY) * 0.45f; if (fabsf(tgt - s_capY) < 0.4f) s_capY = tgt; menu_glide = true; }
    } else if (s_screen == ST_OVER) {
        s_over_ms += dt; dec(&s_shake_ms, dt);
        float k = dt / 16.0f; int ch = nucleo_app_content_height();
        for (int i = 0; i < NFALL; i++) {                                  // tumble the spilled deck down, wrapping at the bottom
            s_fall[i].vy += 0.05f * k;
            s_fall[i].x  += s_fall[i].vx * k;
            s_fall[i].y  += s_fall[i].vy * k;
            s_fall[i].sp += s_fall[i].spin * dt;
            if (s_fall[i].y > ch + 26) { s_fall[i].y = -24.0f; s_fall[i].x = 16 + (int)(esp_random() % (W - 32)); s_fall[i].vy = 0.6f + (esp_random() % 120) / 80.0f; s_fall[i].id = (int)(esp_random() % 52); }
        }
    }

    if (s_screen == ST_HELP) return false;                            // fully static
    if ((s_screen == ST_MENU || s_screen == ST_SET) && !menu_glide) return false;
    if (s_now - s_frame < 33) return false;
    s_frame = s_now; s_anim++;
    return true;
}
static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    sfx_cache_check();
    presynth();
    if (!s_coin) s_coin = (Coin *)calloc(NPART, sizeof *s_coin);   // freed in on_exit; null-guarded everywhere
    else memset(s_coin, 0, NPART * sizeof *s_coin);
    memset(s_fw, 0, sizeof s_fw);
    memset(s_fall, 0, sizeof s_fall); s_over_ms = 0;
    shuffle_deck();
    reset_table_ready();
    s_shake_ms = 0; s_flash_ms = 0; s_coinwave_ms = 0; s_shx = s_shy = 0; s_fin_age = 0;
    s_msg_ms = 0; s_disp_bal = g_balance;
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
static void on_exit(void) { nucleo_audio_stop(); cfg_write(); free(s_coin); s_coin = nullptr; }

extern "C" void nucleo_register_poker(void)
{
    static const nucleo_app_def_t app = {
        "poker", "Poker", "Games", "Video poker 5 carte: tieni con 3-7 e cambia",
        'P', C_RED, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP   // dedicate RAM + free the shared I2S/mic line so the chiptune SFX reliably play
    };
    nucleo_app_register(&app);
}
