// app_pinball.cpp — NucleoOS "Flipper": a PORTRAIT pinball machine (category "Games").
//
// The Cardputer panel is fixed landscape (240x135), but a pinball table wants to be TALL. So the whole
// playfield is authored in portrait logical space (PW=135 across x PH=240 down) and a 90' transform maps
// it onto the landscape panel — the player holds the device vertically (rotate so the screen's RIGHT edge
// points up). Gravity pulls the ball down the long axis. Menus stay in normal landscape (readable text);
// only the live table is portrait + fullscreen.
//
// The score and the pop-up DMD are drawn with a real 5x7 DOT-MATRIX font (each pixel a little glowing dot),
// which both looks like a true pinball Dot Matrix Display AND sidesteps rotated-text entirely — a rotated
// grid of dots is trivial. The DMD is "a comparsa": hidden normally, it slides in for events (SHOOT AGAIN,
// BONUS, JACKPOT, TILT, BALL n, GAME OVER...) with animation, then retracts.
//
// Constraints (same as the other games): exclusive_flags = NX_NET_APP (dedicate RAM + free the I2S line so
// SFX play); all state static (NO heap); buffered `d.` drawing (one blit/frame ANTI-FLICKER); 8bpp has no
// alpha so glows are integer channel-mixes. ~50 Hz poll runs the physics. Flippers HOLD up while '1'/'0'
// (or FN/OPT) are down, via nucleo_kbd_char_down (live pressed-key state). ASCII only. Never name a local `d`.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "nucleo_exclusive.h"
#include "pinball_levels.h"
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
static uint16_t mix(uint16_t a, uint16_t b, int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar + (br - ar) * t / 256) & 31) << 11) | (((ag + (bg - ag) * t / 256) & 63) << 5) | ((ab + (bb - ab) * t / 256) & 31));
}
#define COL_BG     rgb(10, 12, 28)
#define COL_FIELD  rgb(22, 20, 56)
#define COL_FIELD2 rgb(34, 26, 84)
#define COL_DARK   rgb(6, 8, 18)
#define COL_WALL   rgb(70, 90, 200)
#define COL_WALLL  rgb(150, 180, 255)
#define COL_STEEL  rgb(150, 162, 180)
#define COL_STEELL rgb(225, 235, 250)
#define COL_WHITE  0xFFFF
#define COL_CREAM  rgb(255, 246, 214)
#define COL_GOLD   rgb(255, 196, 64)
#define COL_GOLDL  rgb(255, 230, 150)
#define COL_RED    rgb(244, 70, 62)
#define COL_REDD   rgb(150, 26, 26)
#define COL_CYAN   rgb(96, 212, 236)
#define COL_GREEN  rgb(120, 232, 142)
#define COL_PURPLE rgb(192, 112, 232)
#define COL_PINK   rgb(255, 122, 182)
#define COL_ORANGE rgb(255, 150, 50)
#define COL_AMBER  rgb(255, 176, 36)     // DMD lit dot
#define COL_AMBERD rgb(70, 42, 6)        // DMD unlit dot
#define COL_GREY   rgb(150, 162, 176)
#define COL_DIM    rgb(60, 66, 92)

// ============================ portrait transform =============================
// Logical portrait: lx in [0,PW-1] (across), ly in [0,PH-1] (down). Screen 240x135.
// 90' rotation: screen_x = (PH-1) - ly, screen_y = lx.  Right-handed (no mirror).
#define PW 135
#define PH 240
static inline int SX(float ly) { return (PH - 1) - (int)(ly + 0.5f); }
static inline int SY(float lx) { return (int)(lx + 0.5f); }
static void pcircle(float lx, float ly, int r, uint16_t c) { d.fillCircle(SX(ly), SY(lx), r, c); }
static void pcirc_o(float lx, float ly, int r, uint16_t c) { d.drawCircle(SX(ly), SY(lx), r, c); }
static void ptri(float ax, float ay, float bx, float by, float cx, float cy, uint16_t c) { d.fillTriangle(SX(ay), SY(ax), SX(by), SY(bx), SX(cy), SY(cx), c); }
// logical axis-aligned box (lx,ly,w,h) -> screen rect (dims swap under the rotation)
static void pbox(float lx, float ly, float w, float h, uint16_t c) { d.fillRect(SX(ly + h), SY(lx), (int)h + 1, (int)w + 1, c); }
static void pbox_round(float lx, float ly, float w, float h, int r, uint16_t c) { d.fillRoundRect(SX(ly + h), SY(lx), (int)h + 1, (int)w + 1, r, c); }
// thick logical segment (capsule-ish) as two triangles of half-width hw
static void pthick(float ax, float ay, float bx, float by, float hw, uint16_t c)
{
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy); if (l < 0.001f) l = 0.001f;
    float nx = -dy / l * hw, ny = dx / l * hw;
    ptri(ax + nx, ay + ny, ax - nx, ay - ny, bx + nx, by + ny, c);
    ptri(ax - nx, ay - ny, bx + nx, by + ny, bx - nx, by - ny, c);
}

// ============================ 5x7 dot-matrix font ============================
static const char *FCH = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.!:+/";
static const uint8_t FONT[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31}, // 0-3
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03}, // 4-7
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E}, // 8-9
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C}, // A-D
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F}, // E-H
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40}, // I-L
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06}, // M-P
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01}, // Q-T
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63}, // U-X
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}, // Y-Z
    {0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x36,0x36,0x00,0x00}, // - . ! :
    {0x08,0x08,0x3E,0x08,0x08},{0x20,0x10,0x08,0x04,0x02}, // + /
};
static int glyph_idx(char c) { if (c >= 'a' && c <= 'z') c -= 32; const char *p = strchr(FCH, c); return p ? (int)(p - FCH) : 0; }
// draw a dot-matrix string in LOGICAL coords; columns advance along +lx, rows along +ly. pitch=dot spacing.
static int dmd_text(float lx, float ly, const char *s, int pitch, int dotr, uint16_t on, int litbg, uint16_t off)
{
    float x = lx;
    for (; *s; s++) {
        int gi = glyph_idx(*s);
        for (int c = 0; c < 5; c++) {
            uint8_t col = FONT[gi][c];
            for (int r = 0; r < 7; r++) {
                bool lit = (col >> r) & 1;
                if (lit) pcircle(x + c * pitch, ly + r * pitch, dotr, on);
                else if (litbg) pcircle(x + c * pitch, ly + r * pitch, dotr, off);
            }
        }
        x += 6 * pitch;
    }
    return (int)(x - lx);
}
static int dmd_width(const char *s, int pitch) { return (int)strlen(s) * 6 * pitch; }

// ============================ landscape text (menus) =========================
static void ltext(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void ltext_c(int cx, int y, int sz, uint16_t col, const char *s) { ltext(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void ltext_r(int rx, int y, int sz, uint16_t col, const char *s) { ltext(rx - (int)strlen(s) * 6 * sz, y, sz, col, s); }

// ============================ state ==========================================
enum { ST_MENU = 0, ST_SCORES, ST_HELP, ST_SET, ST_PLAY, ST_OPT, ST_OVER };
enum { BP_READY = 0, BP_PLAY, BP_DRAIN };          // ball phase within ST_PLAY
#define START_BALLS 3

static int   g_lang = 0, g_audio = 1, g_nudge = 1;
static unsigned g_hi = 0;

static int   s_screen, s_msel, s_setsel, s_help, s_optsel;
static float s_capY;
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;

// game
static int   s_bp, s_balls;
static unsigned s_score, s_disp_score;
static int   s_mult;                                // current playfield multiplier 1..5
static int   s_combo, s_combo_ms;                   // bumper combo counter
static int   s_tilt_warn, s_tilt;                   // nudge abuse -> tilt

// ball
static float bx, by, vx, vy;
static float s_plunge;                              // 0..1 plunger charge (visual)
static int   s_save_ms;                             // ball-save grace after launch
static int   s_stuck_ms;                            // anti-stuck timer

// flippers: pivot P, rest tip R, up tip U, swing 0..1, swing velocity, held (hold-to-flip)
struct Flip { float px, py, rx, ry, ux, uy, sw, sv; bool held; };
static Flip s_fl, s_fr;
static int  s_ltap_ms, s_rtap_ms;        // tap timers (key '1' / RIGHT) — drive a momentary flip
static uint8_t s_mods_prev;              // last modifier bitmask (OPT hold + CTRL tilt edge)
// static guide rails (inlane/outlane funnels) — line segments the ball bounces off
struct Seg { float ax, ay, bx2, by2; };
#define NWALL 2
static Seg s_wall[NWALL];

// bumpers / slingshots (active round obstacles with a kick)
struct Bump { float x, y; int r; int lit_ms; int kick; uint16_t col; int score; };
#define NBMP 5
#define NSLG 2
static Bump s_bmp[NBMP];
static Bump s_slg[NSLG];
static int  s_nbmp = 3;                 // active bumpers this level (3..5)
#define NPOST 2
static Bump s_post[NPOST];              // fixed mid-field kicker posts (extra interactive bumpers)
// spinner — scores + spins when the ball passes through it (mid-field)
#define SPIN_X 67
#define SPIN_Y 106
static float s_spin_a, s_spin_av;       // spinner angle + angular velocity
static int   s_spin_cool;               // debounce: one pass = one score burst
// ball trail (fading motion blur)
#define NTRAIL 11
static float s_trx[NTRAIL], s_try[NTRAIL]; static int s_trn;
// shockwaves (expanding rings emitted on impacts)
#define NSHOCK 6
struct Shock { float x, y; int age, max; uint16_t col; };
static Shock s_shock[NSHOCK];
// power-ups (timed) + which one the next target-bank grants
static int   s_2x_ms, s_slow_ms, s_pw_rot;

// top rollover targets (light all -> bonus + multiplier up)
#define NTGT 5
struct Tgt { float x, y; bool lit; int hit_ms; };
static Tgt s_tgt[NTGT];
static int  s_ntgt = 3;                 // active targets this level (3..4)

// ---- procedural levels (pinball_levels.h) ----
static PbLevel  s_lv;                    // current level config
static int      s_level = 1;             // current level (1-based, infinite)
static unsigned s_lv_score;              // points banked toward this level's goal
static float    s_grav = 0.0055f;        // gravity for the current level
static uint16_t th_field, th_field2, th_wall, th_wallL, th_accent, th_accent2;   // active theme colours
static int      s_xfade_ms, s_xfade_age; // fullscreen level-up transition

// ---- plunger ----
static bool     s_charging, s_inchute, s_go_held;   // charging / ball in the (invisible) launch chute / GO held
static int      s_launch_ms, s_atmax;               // launch streak timer / ms held at full charge (overheat)

// ---- leaderboard (top-10, persisted) ----
#define NTOP 10
static unsigned g_top[NTOP];

// spark particles
#define NSPK 28
struct Spark { float x, y, vx, vy; int life, max; uint16_t col; };
static Spark *s_spk = nullptr;   // heap-allocated on enter, freed on exit (zero .bss at boot)

// DMD pop-up
static int   s_dmd_ms;                              // remaining time the DMD is shown
static int   s_dmd_age;                             // age (drives slide-in + animation)
static char  s_dmd_line[20];                        // big line
static char  s_dmd_sub[20];                         // small line (optional)
static uint16_t s_dmd_col;

static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static void dec(int *v, int n) { *v -= n; if (*v < 0) *v = 0; }
static void go(int s);

// ============================ persistence ====================================
#define DIRR "/sd/data/pinball"
#define CFG_MAGIC 0x50494E43u   // 'PINC' (v2: adds the top-10 leaderboard)
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); }
static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m, hi; int l, a, n; unsigned top[NTOP]; } c = { CFG_MAGIC, g_hi, g_lang, g_audio, g_nudge, { 0 } };
    for (int i = 0; i < NTOP; i++) c.top[i] = g_top[i];
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m, hi; int l, a, n; unsigned top[NTOP]; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) {
        g_hi = c.hi; g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_nudge = c.n ? 1 : 0;
        for (int i = 0; i < NTOP; i++) g_top[i] = c.top[i];
    }
}
static void leaderboard_add(unsigned sc)        // insert sc into the sorted-desc top-10
{
    if (sc == 0) return;
    int pos = NTOP;
    for (int i = 0; i < NTOP; i++) if (sc > g_top[i]) { pos = i; break; }
    if (pos >= NTOP) return;
    for (int i = NTOP - 1; i > pos; i--) g_top[i] = g_top[i - 1];
    g_top[pos] = sc;
}
static bool leaderboard_qualifies(unsigned sc) { return sc > 0 && sc > g_top[NTOP - 1]; }

// ============================ audio ==========================================
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "nav";   case 2: return "sel";   case 3: return "back";  case 4: return "launch";
        case 5: return "flip";  case 6: return "bump";  case 7: return "sling";  case 8: return "tgt";
        case 9: return "drain"; case 10:return "jack";  case 11:return "bonus";  case 12:return "tilt";
        case 13:return "extra"; case 14:return "over";
        case 15:return "bC";    case 16:return "bD";    case 17:return "bE";     case 18:return "bG";  case 19:return "bA";
        case 20:return "plg";   case 21:return "lvl";   case 22:return "tw";
        case 23:return "coin";  case 24:return "x2";    case 25:return "slow";   case 26:return "blast";
        case 27:return "spin";  case 28:return "rail";
        default: return "x";
    }
}
#define NSFX 28
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1:  notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.55f; return 1;
        case 2:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.10f); return 2;
        case 3:  notify__voice(&v[0], 587.33f, 0, 0.07f); notify__voice(&v[1], 392, 0.05f, 0.10f); return 2;
        case 4:  notify__voice(&v[0], 160, 0, 0.05f); notify__voice(&v[1], 320, 0.04f, 0.06f);                 // launch — rising whoosh
                 notify__voice(&v[2], 520, 0.09f, 0.07f); notify__voice(&v[3], 780, 0.15f, 0.10f); return 4;
        case 5:  notify__voice(&v[0], 240, 0, 0.03f); v[0].amp = 0.8f; notify__voice(&v[1], 900, 0, 0.02f); return 2;  // flipper clack
        case 6:  notify__voice(&v[0], 523.25f, 0, 0.05f); notify__voice(&v[1], 784, 0.02f, 0.05f); v[0].amp = 0.9f; return 2; // bumper pop
        case 7:  notify__voice(&v[0], 300, 0, 0.04f); notify__voice(&v[1], 1200, 0, 0.02f); v[0].amp = 0.9f; return 2; // slingshot snap
        case 8:  notify__voice(&v[0], 1046.5f, 0, 0.04f); notify__voice(&v[1], 1318.5f, 0.03f, 0.06f); return 2;       // target ding
        case 9:  notify__voice(&v[0], 392, 0, 0.10f); notify__voice(&v[1], 294, 0.08f, 0.12f); v[0].amp = 0.6f;        // drain — sad slide
                 notify__voice(&v[2], 196, 0.18f, 0.20f); return 3;
        case 10: notify__voice(&v[0], 523.25f, 0.00f, 0.10f); notify__voice(&v[1], 659.25f, 0.07f, 0.10f);            // jackpot fanfare
                 notify__voice(&v[2], 783.99f, 0.14f, 0.12f); notify__voice(&v[3], 1046.5f, 0.21f, 0.16f);
                 notify__voice(&v[4], 1318.5f, 0.28f, 0.22f); return 5;
        case 11: notify__voice(&v[0], 880, 0, 0.05f); notify__voice(&v[1], 1174.7f, 0.05f, 0.06f);                    // bonus chime
                 notify__voice(&v[2], 1567.98f, 0.10f, 0.10f); return 3;
        case 12: notify__voice(&v[0], 110, 0, 0.16f); v[0].amp = 0.9f; notify__voice(&v[1], 104, 0.05f, 0.16f); v[1].amp = 0.9f; return 2; // tilt buzz
        case 13: notify__voice(&v[0], 659.25f, 0, 0.06f); notify__voice(&v[1], 987.77f, 0.05f, 0.06f);                // extra ball
                 notify__voice(&v[2], 1318.5f, 0.10f, 0.12f); return 3;
        case 14: notify__voice(&v[0], 392, 0.00f, 0.14f); notify__voice(&v[1], 330, 0.13f, 0.14f);                    // game over toll
                 notify__voice(&v[2], 262, 0.26f, 0.14f); notify__voice(&v[3], 196, 0.39f, 0.30f); return 4;
        case 15: notify__voice(&v[0], 523.25f, 0, 0.05f); notify__voice(&v[1], 1046.5f, 0.02f, 0.04f); v[0].amp = 0.9f; return 2;  // bumper pops (pentatonic: C D E G A)
        case 16: notify__voice(&v[0], 587.33f, 0, 0.05f); notify__voice(&v[1], 1174.7f, 0.02f, 0.04f); v[0].amp = 0.9f; return 2;
        case 17: notify__voice(&v[0], 659.25f, 0, 0.05f); notify__voice(&v[1], 1318.5f, 0.02f, 0.04f); v[0].amp = 0.9f; return 2;
        case 18: notify__voice(&v[0], 783.99f, 0, 0.05f); notify__voice(&v[1], 1568.0f, 0.02f, 0.04f); v[0].amp = 0.9f; return 2;
        case 19: notify__voice(&v[0], 880.00f, 0, 0.05f); notify__voice(&v[1], 1760.0f, 0.02f, 0.04f); v[0].amp = 0.9f; return 2;
        case 20: notify__voice(&v[0], 180, 0, 0.06f); notify__voice(&v[1], 360, 0.05f, 0.07f);                        // power launch — strong rising whoosh
                 notify__voice(&v[2], 620, 0.11f, 0.08f); notify__voice(&v[3], 980, 0.18f, 0.13f); v[3].amp = 0.9f; return 4;
        case 21: notify__voice(&v[0], 659.25f, 0, 0.10f); notify__voice(&v[1], 880, 0.08f, 0.10f);                    // level-up fanfare (ascending)
                 notify__voice(&v[2], 1046.5f, 0.16f, 0.12f); notify__voice(&v[3], 1318.5f, 0.24f, 0.14f);
                 notify__voice(&v[4], 1760.0f, 0.32f, 0.24f); return 5;
        case 22: notify__voice(&v[0], 140, 0, 0.08f); v[0].amp = 0.8f; notify__voice(&v[1], 130, 0.04f, 0.08f); v[1].amp = 0.8f; return 2; // tilt warning
        case 23: notify__voice(&v[0], 988, 0, 0.06f); notify__voice(&v[1], 1319, 0.06f, 0.10f); return 2;                              // coin / arcade insert
        case 24: notify__voice(&v[0], 784, 0, 0.05f); notify__voice(&v[1], 988, 0.05f, 0.05f);                                         // 2X — bright ascending arcade run
                 notify__voice(&v[2], 1319, 0.10f, 0.06f); notify__voice(&v[3], 1568, 0.16f, 0.13f); return 4;
        case 25: notify__voice(&v[0], 880, 0, 0.10f); notify__voice(&v[1], 660, 0.09f, 0.10f);                                         // slow-mo — descending warble
                 notify__voice(&v[2], 440, 0.18f, 0.18f); return 3;
        case 26: notify__voice(&v[0], 90, 0, 0.18f); v[0].amp = 1.0f; notify__voice(&v[1], 160, 0.02f, 0.10f);                         // blast — low boom + bright crack
                 notify__voice(&v[2], 1200, 0, 0.05f); notify__voice(&v[3], 1800, 0.04f, 0.08f); return 4;
        case 27: notify__voice(&v[0], 600, 0, 0.05f); v[0].amp = 1.0f; notify__voice(&v[1], 950, 0, 0.03f); v[1].amp = 0.7f; return 2;   // spinner ratchet tick (bright, full)
        case 28: notify__voice(&v[0], 360, 0, 0.03f); v[0].amp = 0.6f; notify__voice(&v[1], 1500, 0, 0.015f); return 2;     // rail/deflector tick
    }
    return 0;
}
static bool sfx_important(int id) { return id == 4 || id == 9 || id == 10 || id == 11 || id == 14 || id == 20 || id == 21 || (id >= 23 && id <= 26); }   // fanfares/launch/power-ups always play
static bool sfx_snappy(int id) { return id == 5 || id == 6 || id == 7 || id == 8 || (id >= 15 && id <= 19) || id == 27 || id == 28; }  // brief impacts: retrigger crisply, never pile up
static int64_t s_sfx_guard = 0;   // ms: until then a fanfare owns the channel and quick hits won't cut it
static void sfx(int id)
{
    if (!g_audio || id <= 0) return;
    int64_t now = esp_timer_get_time() / 1000;
    bool important = sfx_important(id), snappy = sfx_snappy(id);
    if (!important) {
        if (now < s_sfx_guard) return;                         // a fanfare is sounding -> leave it intact
        if (nucleo_audio_is_playing() && !snappy) return;      // non-snappy & busy -> don't pile up
    }
    const char *nm = sfx_name(id);
    char p[80];
    snprintf(p, sizeof p, DIRR "/pack/%s.wav", nm);
    FILE *f = fopen(p, "rb");
    if (!f) {
        fprintf(stderr, "[sfx] NOT FOUND: %s\n", p);          // debug: log missing files
        return;
    }
    fclose(f);
    if (important || snappy) nucleo_audio_stop();              // grab the channel for an immediate, punchy hit
    nucleo_audio_play(p);
    if (important) s_sfx_guard = now + 700;                    // protect the fanfare for ~its length
}
#define SFX_VER 5
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
        char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ DMD events =====================================
static void dmd_show(const char *line, const char *sub, uint16_t col, int ms)
{
    snprintf(s_dmd_line, sizeof s_dmd_line, "%s", line);
    snprintf(s_dmd_sub, sizeof s_dmd_sub, "%s", sub ? sub : "");
    s_dmd_col = col; s_dmd_ms = ms; s_dmd_age = 0;
}

// ============================ sparks =========================================
static void spark_burst(float x, float y, int n, uint16_t col)
{
    if (!s_spk) return;
    for (int i = 0; i < NSPK && n > 0; i++) {
        if (s_spk[i].life > 0) continue;
        float a = (esp_random() % 628) / 100.0f, sp = 0.4f + (esp_random() % 100) / 120.0f;
        s_spk[i].x = x; s_spk[i].y = y;
        s_spk[i].vx = cosf(a) * sp; s_spk[i].vy = sinf(a) * sp;
        s_spk[i].max = s_spk[i].life = 14 + (esp_random() % 14);
        s_spk[i].col = col; n--;
    }
}
static void sparks_step(int dt)
{
    if (!s_spk) return;
    float k = dt / 16.0f;
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        s_spk[i].x += s_spk[i].vx * k; s_spk[i].y += s_spk[i].vy * k; s_spk[i].vy += 0.02f * k;
        s_spk[i].life -= dt;
    }
}
static void sparks_draw(void)
{
    if (!s_spk) return;
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        int f = s_spk[i].life * 256 / (s_spk[i].max > 0 ? s_spk[i].max : 1);
        pcircle(s_spk[i].x, s_spk[i].y, f > 130 ? 2 : 1, mix(COL_FIELD, s_spk[i].col, f));
    }
}
// ---- shockwaves: expanding rings on impacts ----
static void shock_spawn(float x, float y, int life, uint16_t col)
{
    for (int i = 0; i < NSHOCK; i++)
        if (s_shock[i].age >= s_shock[i].max) { s_shock[i].x = x; s_shock[i].y = y; s_shock[i].age = 0; s_shock[i].max = life; s_shock[i].col = col; return; }
}
static void shock_step(int dt) { for (int i = 0; i < NSHOCK; i++) if (s_shock[i].age < s_shock[i].max) s_shock[i].age += dt; }
static void shock_draw(void)
{
    for (int i = 0; i < NSHOCK; i++) {
        if (s_shock[i].age >= s_shock[i].max) continue;
        int a = s_shock[i].age, m = s_shock[i].max, rr = a * 26 / m, fade = 256 - a * 256 / m;
        pcirc_o(s_shock[i].x, s_shock[i].y, rr, mix(th_field, s_shock[i].col, fade));
        pcirc_o(s_shock[i].x, s_shock[i].y, rr + 1, mix(th_field, s_shock[i].col, fade / 2));
    }
}
// ---- ball trail (fading motion blur) ----
static void trail_push(void) { s_trx[s_trn % NTRAIL] = bx; s_try[s_trn % NTRAIL] = by; s_trn++; }
static void trail_draw(void)
{
    for (int k = 1; k < 4; k++) {                       // just a short faint wisp (no long comet)
        int idx = s_trn - 1 - k;
        if (idx < 0) break;
        pcircle(s_trx[idx % NTRAIL], s_try[idx % NTRAIL], 1, mix(th_field, COL_STEELL, 80 - k * 22));
    }
}

// ============================ table layout ===================================
// playfield interior: lx [LWALL..RWALL], ly [TWALL..BWALL]; plunger lane on the right
#define LWALL 5
#define RWALL 130       // full-width playfield (uses nearly every pixel; centre stays at 67.5)
#define TWALL 20        // top wall sits BELOW the score HUD (no blue border over the score)
#define BWALL 235
#ifdef BR
#undef BR               // some board/SoC headers also define BR; ours is the ball radius
#endif
#define BR    3.0f      // ball radius
#define DRAINX0 62
#define DRAINX1 74      // drain gap = the real opening between the two flipper tips at rest
#define CHX     (RWALL - 7)   // invisible launch-chute centre x; the ball is locked here while launching
#define CHTOP   80            // the ball enters the field only when it rises above this — weak shots fall back

// Build the table for level n from its procedural config: theme, bumpers, targets, gravity. The
// slingshots, flippers and guide rails keep a fixed (tuned) geometry across levels.
static void apply_level(int n)
{
    s_level = n;
    pb_gen_level(n, &s_lv);
    th_field = s_lv.field; th_field2 = s_lv.field2; th_wall = s_lv.wall; th_wallL = s_lv.wallL;
    th_accent = s_lv.accent; th_accent2 = s_lv.accent2;
    s_grav = s_lv.grav_e4 / 10000.0f;
    s_nbmp = s_lv.nbmp; s_ntgt = s_lv.ntgt;
    for (int i = 0; i < s_nbmp; i++) s_bmp[i] = { s_lv.bmpx[i], s_lv.bmpy[i], 9, 0, 0, s_lv.bmpcol[i], s_lv.bmp_score };
    for (int i = 0; i < s_ntgt; i++) { s_tgt[i].x = s_lv.tgtx[i]; s_tgt[i].y = s_lv.tgty[i]; s_tgt[i].lit = false; s_tgt[i].hit_ms = 0; }
    s_slg[0] = { 34, 180, 8, 0, 0, s_lv.accent2, 50 };          // mirror about x=68 (flipper centre)
    s_slg[1] = { 102, 180, 8, 0, 0, s_lv.accent2, 50 };
    // mid-field kicker posts (extra interactive bumpers), themed
    s_post[0] = { 24, 140, 7, 0, 0, s_lv.wallL, 75 };
    s_post[1] = { 112, 140, 7, 0, 0, s_lv.wallL, 75 };
    s_spin_a = 0; s_spin_av = 0; s_spin_cool = 0;
    // flippers — wide REAL gap at rest (ball drains down the middle); raised flippers close it
    s_fl.px = 38; s_fl.py = 214; s_fl.rx = 57; s_fl.ry = 226; s_fl.ux = 66; s_fl.uy = 208; s_fl.sw = 0; s_fl.sv = 0; s_fl.held = false;
    s_fr.px = 98; s_fr.py = 214; s_fr.rx = 79; s_fr.ry = 226; s_fr.ux = 70; s_fr.uy = 208; s_fr.sw = 0; s_fr.sv = 0; s_fr.held = false;
    // outlane deflectors: short rails that slope down toward each flipper, narrowing the drain.
    // SYMMETRIC about x=68 (the flipper centre): they mirror each other and neither touches the side
    // border. The right rail stays just inside the launch chute so it never blocks the shot.
    s_wall[0].ax = 17;  s_wall[0].ay = 192; s_wall[0].bx2 = 35;  s_wall[0].by2 = 211;
    s_wall[1].ax = 119; s_wall[1].ay = 192; s_wall[1].bx2 = 101; s_wall[1].by2 = 211;
}
static void ball_to_lane(void)             // park the ball at the bottom of the (invisible) launch chute
{
    bx = CHX; by = BWALL - 7; vx = 0; vy = 0; s_plunge = 0; s_inchute = false; s_charging = false; s_atmax = 0; s_bp = BP_READY;
}

// ============================ game flow ======================================
static void add_score(unsigned pts)
{
    unsigned g = pts * (unsigned)s_mult * (s_2x_ms > 0 ? 2u : 1u);   // 2X power-up doubles
    s_score += g; s_lv_score += g;                  // banked toward the level goal too
    if (s_score > g_hi) g_hi = s_score;
}
static void apply_level(int n);                     // fwd
static void new_game(void)
{
    s_score = 0; s_disp_score = 0; s_balls = START_BALLS; s_mult = 1; s_combo = 0; s_combo_ms = 0;
    s_tilt = 0; s_tilt_warn = 0;
    s_ltap_ms = 0; s_rtap_ms = 0; s_mods_prev = nucleo_kbd_mods();    // ignore a modifier already held at kickoff
    s_level = 1; s_lv_score = 0; s_xfade_ms = 0; s_charging = false; s_launch_ms = 0;
    apply_level(1);
    if (s_spk) memset(s_spk, 0, sizeof(Spark) * NSPK);
    memset(s_shock, 0, sizeof s_shock);
    s_trn = 0; s_2x_ms = 0; s_slow_ms = 0; s_pw_rot = 0; s_stuck_ms = 0; s_go_held = false;
    ball_to_lane();
    s_dmd_ms = 0;
    dmd_show(tx("FLIPPER", "PINBALL"), s_lv.theme, COL_CYAN, 1600);
    sfx(23);                                  // arcade "insert coin"
}
static void launch_ball(float power)
{
    if (s_bp != BP_READY) return;
    if (power < 0.04f) power = 0.04f;
    if (power > 1.0f) power = 1.0f;
    vy = -(1.2f + power * 2.6f);            // up the chute; only a strong charge clears the top
    vx = 0;
    s_bp = BP_PLAY; s_inchute = true; s_save_ms = 2600; s_launch_ms = 280;
    s_charging = false; s_plunge = 0; s_atmax = 0;
    sfx(power > 0.6f ? 20 : 4);             // soft / hard launch
    // Random lateral vx added at chute exit (step_ball) so it's not reset to 0
}
static void level_up(void)
{
    s_level++; s_lv_score = 0;
    apply_level(s_level);               // new theme / layout / gravity (balls + total score persist)
    s_xfade_ms = 1700; s_xfade_age = 0; // fullscreen wow transition
    sfx(21);                            // level-up fanfare
    ball_to_lane();
    char b[16]; snprintf(b, sizeof b, "%s %d", tx("LIVELLO", "LEVEL"), s_level);
    dmd_show(b, s_lv.theme, COL_GOLDL, 1900);
}
static void lose_ball(void)
{
    s_bp = BP_DRAIN;
    sfx(9);
    s_balls--;
    s_mult = 1;
    for (int i = 0; i < s_ntgt; i++) s_tgt[i].lit = false;
    if (s_balls <= 0) {
        bool hs = leaderboard_qualifies(s_score);
        leaderboard_add(s_score);
        cfg_write();
        dmd_show(tx("FINE PARTITA", "GAME OVER"), hs ? tx("IN CLASSIFICA!", "TOP 10!") : NULL, hs ? COL_GOLD : COL_RED, 100);
        sfx(14);
        return;
    }
    char b[20]; snprintf(b, sizeof b, "%s %d", tx("PALLA", "BALL"), START_BALLS - s_balls + 1);
    dmd_show(b, tx("LANCIA", "SHOOT"), COL_AMBER, 1400);
}

// ============================ physics ========================================
// hold-to-flip: sw rises fast while held, eases down when released; sv>0 means it's actively swinging up
static void flipper_step(Flip *f, int dt)
{
    float k = dt / 16.0f, old = f->sw;
    if (f->held) f->sw += 0.55f * k; else f->sw -= 0.22f * k;   // snappier swing up, prompt return
    if (f->sw > 1.0f) f->sw = 1.0f;
    if (f->sw < 0.0f) f->sw = 0.0f;
    f->sv = f->sw - old;
}
static void flip_tip(const Flip *f, float *tx_, float *ty_) { *tx_ = f->rx + (f->ux - f->rx) * f->sw; *ty_ = f->ry + (f->uy - f->ry) * f->sw; }
// closest point on segment AB to P
static void closest_seg(float ax, float ay, float bx2, float by2, float px, float py, float *cx, float *cy)
{
    float dx = bx2 - ax, dy = by2 - ay, l2 = dx * dx + dy * dy;
    float t = l2 > 0.0001f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *cx = ax + dx * t; *cy = ay + dy * t;
}
static void hit_flipper(Flip *f)
{
    float tx2, ty2; flip_tip(f, &tx2, &ty2);
    float cx, cy; closest_seg(f->px, f->py, tx2, ty2, bx, by, &cx, &cy);
    float dx = bx - cx, dy = by - cy, dist = sqrtf(dx * dx + dy * dy);
    float rr = BR + 2.8f;                                          // slightly thicker bat -> reliably catches the ball (no slip-through)
    if (dist > rr || dist < 0.0001f) return;
    float nx = dx / dist, ny = dy / dist;
    float vn = vx * nx + vy * ny;
    if (vn < 0) { vx -= 1.72f * vn * nx; vy -= 1.72f * vn * ny; }  // reflect off the bat face
    // contact position along the bat: 0 at the pivot, 1 at the tip — the tip swings fastest, so it hits hardest
    float sx = tx2 - f->px, sy = ty2 - f->py, sl2 = sx * sx + sy * sy;
    float t = sl2 > 0.0001f ? ((cx - f->px) * sx + (cy - f->py) * sy) / sl2 : 0.5f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float kick = f->sw * 0.45f + (f->sv > 0.008f ? (1.15f + 1.4f * t) : 0.0f);   // live flip, stronger toward the tip
    vy -= kick; vx += (f->ux - f->rx) > 0 ? 0.3f : -0.3f;
    bx = cx + nx * (rr + 0.35f); by = cy + ny * (rr + 0.35f);     // push fully clear so the ball never sinks into the bat
    if (vy < -3.9f) vy = -3.9f;
}
static void hit_round(Bump *o, int kick_sfx, bool is_sling)
{
    float dx = bx - o->x, dy = by - o->y, dist = sqrtf(dx * dx + dy * dy), rr = BR + o->r;
    if (dist > rr || dist < 0.0001f) return;
    float nx = dx / dist, ny = dy / dist;
    float vn = vx * nx + vy * ny;
    float rest = is_sling ? 1.7f : 1.55f;                        // slings stay lively; bumpers/posts less bouncy
    if (vn < 0) { vx -= rest * vn * nx; vy -= rest * vn * ny; }
    float boost = is_sling ? 0.45f : 0.28f;                      // a touch less added kick (credible, not pinballing)
    vx += nx * boost; vy += ny * boost;
    if (is_sling && (vx * vx + vy * vy) < 1.0f) { vx = nx * 1.3f; vy = ny * 1.3f - 0.3f; }   // never let a ball rest on the rubber -> always eject
    bx = o->x + nx * (rr + 0.3f); by = o->y + ny * (rr + 0.3f);
    o->lit_ms = 180;
    add_score(o->score);
    spark_burst(o->x + nx * o->r, o->y + ny * o->r, 5, o->col);
    shock_spawn(o->x, o->y, 340, o->col);
    sfx(kick_sfx);
    if (!is_sling) {
        s_combo++; s_combo_ms = 1400;
        if (s_combo == 5) { dmd_show("COMBO X5", NULL, COL_PURPLE, 1100); add_score(500); }
        else if (s_combo == 10) { dmd_show(tx("SUPER COMBO", "SUPER COMBO"), NULL, COL_GOLD, 1300); add_score(2000); sfx(10); }
    }
}
static void blast(void)        // BONUS BLAST power-up: pop every bumper + post at once
{
    for (int i = 0; i < s_nbmp; i++) { s_bmp[i].lit_ms = 240; add_score(s_bmp[i].score); spark_burst(s_bmp[i].x, s_bmp[i].y, 6, s_bmp[i].col); shock_spawn(s_bmp[i].x, s_bmp[i].y, 420, s_bmp[i].col); }
    for (int i = 0; i < NPOST; i++) { s_post[i].lit_ms = 240; add_score(s_post[i].score); shock_spawn(s_post[i].x, s_post[i].y, 420, s_post[i].col); }
    shock_spawn(67, 130, 640, COL_WHITE);
    sfx(26);
}
static void grant_powerup(void)    // escalating reward each time the target bank is cleared (2X -> slow -> blast)
{
    s_pw_rot = (s_pw_rot + 1) % 3;
    if (s_pw_rot == 1)      { s_2x_ms = 9000;   dmd_show(tx("PUNTI X2", "2X SCORE"), tx("9 SEC", "9 SEC"), COL_GOLD, 1700); sfx(24); }
    else if (s_pw_rot == 2) { s_slow_ms = 6000; dmd_show(tx("RALLENTA", "SLOW-MO"),  tx("6 SEC", "6 SEC"), COL_CYAN, 1700); sfx(25); }
    else                    { dmd_show(tx("ESPLOSIONE", "BLAST"), NULL, COL_RED, 1500); blast(); }
}
static void check_targets(void)
{
    for (int i = 0; i < s_ntgt; i++) {
        if (s_tgt[i].lit) continue;
        float dx = bx - s_tgt[i].x, dy = by - s_tgt[i].y;
        if (dx * dx + dy * dy < (BR + 5) * (BR + 5)) {
            s_tgt[i].lit = true; s_tgt[i].hit_ms = 220; add_score(150); sfx(8);
            spark_burst(s_tgt[i].x, s_tgt[i].y, 4, COL_GOLD);
            bool all = true; for (int k = 0; k < s_ntgt; k++) if (!s_tgt[k].lit) all = false;
            if (all) {
                for (int k = 0; k < s_ntgt; k++) s_tgt[k].lit = false;
                if (s_mult < 5) s_mult++;
                add_score(1000);
                grant_powerup();        // clearing the bank grants an escalating power-up
            }
        }
    }
}
static void nudge(int dir)
{
    if (!g_nudge || s_tilt > 0 || s_bp != BP_PLAY) return;
    vx += dir * 0.15f; vy -= 0.06f;
    s_tilt_warn += 2;
    if (s_tilt_warn >= 7) { s_tilt = 1500; sfx(12); dmd_show("TILT", NULL, COL_RED, 1500); }
    else sfx(7);
}
static void step_ball(int dt)
{
    if (s_bp != BP_PLAY) return;
    trail_push();
    float kk = dt / 16.0f;
    if (s_inchute) {                                     // FX3-style launch: ball locked in the (invisible) rail
        int n = (int)(kk * 2.0f) + 1; if (n > 10) n = 10;
        float ks = kk / n;
        for (int s = 0; s < n; s++) {
            vy += 0.022f * ks;                           // chute gravity decelerates the rising ball
            by += vy * ks;
            bx = CHX; vx = 0;                            // never leaves the rail
            if (by <= CHTOP) {                           // cleared the top -> SHOOT into the field
                s_inchute = false;
                float r = (esp_random() % 1000) / 1000.0f;
                vx = -0.6f + (r - 0.5f) * 0.3f;             // base -0.6 + random ±0.15 lateral wiggle
                float sp = sqrtf(vx * vx + vy * vy);
                if (sp > 3.0f) { vx *= 3.0f / sp; vy *= 3.0f / sp; }
                s_launch_ms = 280;
                dmd_show(tx("IN GIOCO", "BALL LIVE"), NULL, COL_GREEN, 1100);
                break;
            }
            if (by >= BWALL - 7 && vy >= 0) { ball_to_lane(); return; }   // too weak -> rolled back, re-rack
        }
        return;
    }
    // timers once per frame
    if (s_save_ms > 0) dec(&s_save_ms, dt);
    if (s_tilt > 0) { dec(&s_tilt, dt); if (s_tilt == 0) s_tilt_warn = 0; }
    if (s_combo_ms > 0) { dec(&s_combo_ms, dt); if (s_combo_ms == 0) s_combo = 0; }
    float g = s_slow_ms > 0 ? s_grav * 0.5f : s_grav;   // per-level gravity (slow-mo halves it)
    int nsub = (int)(kk * 2.0f) + 1; if (nsub > 9) nsub = 9;     // SUB-STEP so each move stays small (no tunneling, esp. fast balls)
    float k = kk / nsub;
    bool drained = false;
    for (int sub = 0; sub < nsub; sub++) {
        vy += g * k;
        if (s_tilt > 0) vx *= 0.996f;                            // tilt: lose control
        bx += vx * k; by += vy * k;
        if (bx < LWALL + BR) { bx = LWALL + BR; vx = -vx * 0.56f; }   // walls absorb more now (less pinballing)
        if (bx > RWALL - BR) { bx = RWALL - BR; vx = -vx * 0.56f; }
        if (by < TWALL + BR) { by = TWALL + BR; vy = -vy * 0.60f; }
        if (by > BWALL - BR) { lose_ball(); drained = true; break; }   // past the flipper line -> DRAINED (the flippers were the only save)
        if (s_tilt == 0) {
            for (int i = 0; i < NWALL; i++) {                                            // guide-rail bounces
                float cx, cy; closest_seg(s_wall[i].ax, s_wall[i].ay, s_wall[i].bx2, s_wall[i].by2, bx, by, &cx, &cy);
                float dx = bx - cx, dy = by - cy, dist = sqrtf(dx * dx + dy * dy);
                if (dist < BR + 1.2f && dist > 0.0001f) {
                    float nx = dx / dist, ny = dy / dist, vn = vx * nx + vy * ny;
                    if (vn < 0) { vx -= 1.55f * vn * nx; vy -= 1.55f * vn * ny; if (vn < -0.6f) sfx(28); }   // rails a touch less bouncy; tick on a firm hit only
                    bx = cx + nx * (BR + 1.5f); by = cy + ny * (BR + 1.5f);   // firm push-out (no re-penetration)
                }
            }
            for (int i = 0; i < s_nbmp; i++) hit_round(&s_bmp[i], 15 + (i % 5), false);
            for (int i = 0; i < NPOST; i++) hit_round(&s_post[i], 17, false);
            for (int i = 0; i < NSLG; i++) hit_round(&s_slg[i], 7, true);
            hit_flipper(&s_fl); hit_flipper(&s_fr);
            check_targets();
            { float dxs = bx - SPIN_X, dys = by - SPIN_Y;                                // spinner pass-through (bigger catch zone)
              if (dxs * dxs + dys * dys < 150.0f && s_spin_cool <= 0) {
                  float sp = sqrtf(vx * vx + vy * vy);
                  s_spin_av += sp * 0.8f + 0.6f; add_score(50); sfx(27); s_spin_cool = 160;   // more spin energy -> longer audible ratchet
              } }
        }
    }
    if (drained) return;
    vx *= 0.990f; vy *= 0.994f;                                                          // a bit more friction -> the ball settles sooner (credible, less endless bouncing)
    // anti-stuck: a ball loitering in a pocket (deflectors/slings/outlanes) gets nudged out fast — but NOT one
    // sitting in the flipper/drain channel (that should just drain or be flipped, never be auto-launched).
    bool inFlipZone = (bx > 36 && bx < 100 && by > 205);
    if (vx * vx + vy * vy < 0.06f && !inFlipZone) {
        s_stuck_ms += dt;
        if (s_stuck_ms > 600) { vx += (bx < 67 ? 0.85f : -0.85f); vy -= 0.9f; s_stuck_ms = 0; }
    } else s_stuck_ms = 0;
    float sp = sqrtf(vx * vx + vy * vy), vmax = s_slow_ms > 0 ? 2.0f : 3.6f;             // speed cap (a touch livelier)
    if (sp > vmax) { vx *= vmax / sp; vy *= vmax / sp; }
}

// ============================ rendering: table ===============================
// Neon frame OPEN at the bottom: the bottom IS the drain, so there's no floor to draw (a bottom
// rail reads as a wall the ball should bounce off). Top + the two side rails only.
static void draw_open_frame(int e, int th, uint16_t c)
{
    int x = LWALL - 3 - e, y = TWALL - 3 - e;
    int w = RWALL - LWALL + 6 + 2 * e, h = BWALL - TWALL + 6 + 2 * e;
    pbox(x, y, w, th, c);                 // top rail
    pbox(x, y, th, h, c);                 // left rail
    pbox(x + w - th, y, th, h, c);        // right rail
}
// Per-theme background MOTIF: each level's theme gets a distinct SHAPE (not just colour), kept faint
// so it never fights the ball/objects. Every motif stays inside the playfield (LWALL..RWALL, TWALL..BWALL).
static void draw_bg_motif(int theme)
{
    unsigned a = s_anim;
    uint16_t c = mix(th_field2, th_accent, 24);
    switch (((theme % 6) + 6) % 6) {
    case 0:  // scrolling horizontal scan lines
        for (int ly = TWALL + (int)(a % 16); ly < BWALL; ly += 16)
            pbox(LWALL, ly, RWALL - LWALL, 1, mix(th_field2, th_accent, 16 + (int)(12 * sinf((ly + a) * 0.07f))));
        break;
    case 1:  // drifting diagonals (clipped to the field)
        for (int o = LWALL - (BWALL - TWALL) + (int)(a % 20); o < RWALL; o += 20) {
            float y0 = TWALL, y1 = BWALL, ya = TWALL + (LWALL - o), yb = TWALL + (RWALL - o);
            if (ya > y0) y0 = ya;
            if (yb < y1) y1 = yb;
            if (y0 < y1) pthick(o + (y0 - TWALL), y0, o + (y1 - TWALL), y1, 0.5f, c);
        }
        break;
    case 2:  // shimmering vertical columns
        for (int lx = LWALL + 3; lx < RWALL; lx += 16)
            pbox(lx, TWALL, 1, BWALL - TWALL, mix(th_field2, th_accent, 12 + (int)(12 * sinf((lx + a) * 0.11f))));
        break;
    case 3:  // gentle horizontal heat-waves
        for (int ly = TWALL + 6; ly < BWALL; ly += 14) {
            int dx = (int)(4 * sinf((ly + a) * 0.13f));
            pbox(LWALL + 3 + dx, ly, RWALL - LWALL - 6, 1, c);
        }
        break;
    case 4:  // diamond cross-hatch (both diagonals, clipped)
        for (int o = LWALL - (BWALL - TWALL) + (int)(a % 32); o < RWALL + (BWALL - TWALL); o += 32) {
            { float y0 = TWALL, y1 = BWALL, ya = TWALL + (LWALL - o), yb = TWALL + (RWALL - o);
              if (ya > y0) y0 = ya;
              if (yb < y1) y1 = yb;
              if (y0 < y1) pthick(o + (y0 - TWALL), y0, o + (y1 - TWALL), y1, 0.4f, c); }
            { float y0 = TWALL, y1 = BWALL, ya = TWALL + (o - RWALL), yb = TWALL + (o - LWALL);
              if (ya > y0) y0 = ya;
              if (yb < y1) y1 = yb;
              if (y0 < y1) pthick(o - (y0 - TWALL), y0, o - (y1 - TWALL), y1, 0.4f, c); }
        }
        break;
    default: // 5: dotted lattice
        for (int ly = TWALL + 8; ly < BWALL; ly += 16)
            for (int lx = LWALL + 8 + (((ly / 16) & 1) ? 8 : 0); lx < RWALL - 2; lx += 16)
                pcircle(lx, ly, 1, c);
        break;
    }
}
static void draw_field(void)
{
    d.fillRect(0, 0, 240, 135, COL_BG);
    // themed gradient base
    for (int ly = TWALL; ly < BWALL; ly += 4) {
        int t = 80 + 130 * (ly - TWALL) / (BWALL - TWALL);
        pbox(LWALL, ly, RWALL - LWALL, 4, mix(th_field, th_field2, t));
    }
    draw_bg_motif(s_level - 1);   // per-level background SHAPE (varies with the theme), woven faintly into the table
    // pulsing radial glow from the lower-centre
    int gc = 26 + (int)(22 * sinf(s_anim * 0.06f));
    for (int r = 0; r < 4; r++) pcirc_o(67, 130, 28 + r * 18, mix(th_field, th_accent, gc - r * 5 > 0 ? gc - r * 5 : 0));
    // drifting energy motes rising up the table (computed, zero storage)
    for (int i = 0; i < 9; i++) {
        float mx = LWALL + 6 + ((i * 41) % (RWALL - LWALL - 14));
        float my = BWALL - 2 - ((s_anim + i * 53) % (unsigned)(BWALL - TWALL));
        pcircle(mx, my, 1, mix(th_field2, th_wallL, 85));   // faint (kept non-invasive alongside the motif)
    }
    // neon side+top rails (double-stroke, themed) — OPEN at the bottom: that's the drain, no floor
    draw_open_frame(0, 3, th_wall);
    draw_open_frame(-1, 1, th_wallL);
    // guide rails
    for (int i = 0; i < NWALL; i++) {
        pthick(s_wall[i].ax, s_wall[i].ay, s_wall[i].bx2, s_wall[i].by2, 1.7f, COL_STEEL);
        pthick(s_wall[i].ax, s_wall[i].ay, s_wall[i].bx2, s_wall[i].by2, 0.7f, COL_STEELL);
    }
}
static void draw_targets(void)
{
    for (int i = 0; i < s_ntgt; i++) {
        uint16_t c = s_tgt[i].lit ? th_accent : COL_DIM;
        if (s_tgt[i].hit_ms > 0) c = COL_WHITE;
        pbox_round(s_tgt[i].x - 8, s_tgt[i].y - 4, 16, 8, 2, COL_DARK);                       // socket
        pbox_round(s_tgt[i].x - 7, s_tgt[i].y - 3, 14, 6, 2, mix(COL_DARK, c, s_tgt[i].lit ? 220 : 110));
        pbox(s_tgt[i].x - 6, s_tgt[i].y - 2, 12, 1, mix(c, COL_WHITE, 160));                  // top gleam
    }
}
static void draw_bumper(const Bump *o)
{
    int glow = o->lit_ms > 0 ? 120 + o->lit_ms : 50 + (int)(36 * sinf(s_anim * 0.2f));
    if (glow > 255) glow = 255;
    pcircle(o->x, o->y, o->r + 4, mix(th_field, o->col, glow / 4));        // outer glow
    pcirc_o(o->x, o->y, o->r + 3, mix(o->col, COL_WHITE, glow / 2));       // rim ring
    pcircle(o->x, o->y, o->r, mix(o->col, COL_DARK, 70));                  // body
    pcircle(o->x, o->y, o->r - 2, mix(o->col, COL_WHITE, glow));           // lit cap
    pcircle(o->x - o->r / 3, o->y - o->r / 3, o->r / 3, COL_WHITE);        // highlight
}
static void draw_sling(const Bump *o)
{
    int glow = o->lit_ms > 0 ? 220 : 80;
    bool left = o->x < PW / 2;
    float ax = o->x + (left ? -7 : 7), ay = o->y - 12, bx2 = o->x + (left ? -7 : 7), by2 = o->y + 12, cxp = o->x + (left ? 7 : -7), cyp = o->y;
    ptri(ax, ay, bx2, by2, cxp, cyp, mix(COL_STEEL, o->col, glow));        // metal body
    float bw = o->lit_ms > 0 ? 2.6f : 1.4f;                               // rubber band snaps wide on a kick
    pthick(ax, ay, bx2, by2, bw, mix(o->col, COL_WHITE, glow));            // rubber band on the kicking face
    pcircle(ax, ay, 1, COL_STEELL); pcircle(bx2, by2, 1, COL_STEELL);      // posts
}
static void draw_flipper(const Flip *f, bool left)
{
    float tx2, ty2; flip_tip(f, &tx2, &ty2);
    uint16_t col = left ? th_accent : th_accent2;
    if (f->sw > 0.05f) pthick(f->px, f->py, tx2, ty2, 6.2f, mix(th_field, col, (int)(f->sw * 70)));  // colored glow halo while raised
    pthick(f->px, f->py, tx2, ty2, 4.8f, COL_DARK);                                   // dark outline (contrast vs field)
    pthick(f->px, f->py, tx2, ty2, 3.9f, col);                                        // bold themed body
    pthick(f->px, f->py, tx2, ty2, 1.8f, mix(COL_WHITE, col, 60 + (int)(f->sw * 90)));// bright core, flashes white on flip
    pcircle(f->px, f->py, 4, COL_STEELL); pcircle(f->px, f->py, 2, COL_DARK);         // pivot hub
    pcircle(tx2, ty2, 2, COL_WHITE);                                                  // tip cap
}
static void draw_ball(void)
{
    pcircle(bx + 1, by + 2, BR, mix(th_field2, COL_DARK, 150));   // soft drop shadow (offset = floats above table)
    pcircle(bx, by, BR, COL_STEEL);                              // chrome body
    pcircle(bx - 0.7f, by - 0.7f, BR - 1, COL_STEELL);           // lit crescent up-left = roundness
    pcircle(bx - 1, by - 1, 1, COL_WHITE);                       // specular highlight
}
// score strip along the very top of the table (dot-matrix), always visible
static void draw_score_strip(void)
{
    // HUD bar: dark backing + top sheen
    pbox(0, 0, PW, 18, COL_DARK);
    pbox(0, 0, PW, 1, mix(COL_DARK, th_wallL, 80));
    // level-goal progress meter (replaces the plain underline): shows how close you are to clearing the level
    unsigned cap = s_lv_score < (unsigned)s_lv.goal ? s_lv_score : (unsigned)s_lv.goal;
    int fillw = s_lv.goal > 0 ? (int)((long)PW * cap / s_lv.goal) : 0;
    pbox(0, 18, PW, 2, mix(COL_DARK, th_wall, 200));
    if (fillw > 0) pbox(0, 18, fillw, 2, mix(th_accent, COL_WHITE, fillw >= PW ? 160 : 0));
    // left readout: level + multiplier (mult goes green when active)
    char lm[14]; snprintf(lm, sizeof lm, "L%d X%d", s_level, s_mult);
    int lw = dmd_width(lm, 1);
    dmd_text(3, 5, lm, 1, 1, s_mult > 1 ? COL_GREEN : th_accent, 0, 0);
    // right readout: balls remaining
    char bl[8]; snprintf(bl, sizeof bl, "B%d", s_balls);
    int bw = dmd_width(bl, 1);
    dmd_text(PW - bw - 3, 5, bl, 1, 1, th_wallL, 0, 0);
    // centre: score, AUTO-SHRINKING so a long number never collides with the side readouts
    char b[16]; snprintf(b, sizeof b, "%lu", (unsigned long)s_disp_score);
    int avail = PW - (lw + 7) - (bw + 7);
    int sp = (dmd_width(b, 2) <= avail) ? 2 : 1;                    // big pitch normally, small pitch when it grows
    int w = dmd_width(b, sp);
    float cx0 = (lw + 7) + (avail - w) / 2.0f;
    if (cx0 < lw + 7) cx0 = lw + 7;
    uint16_t sc = (s_disp_score != s_score && (s_anim & 2)) ? COL_WHITE : COL_AMBER;  // flash white while rolling up
    dmd_text(cx0, sp == 2 ? 2 : 5, b, sp, 1, sc, 0, 0);
}
// the fullscreen WOW transition shown on a level-up: theme flash, expanding rings, big LEVEL n
static void draw_xfade(void)
{
    if (s_xfade_ms <= 0) return;
    int age = s_xfade_age, cx = 120, cy = 67;
    if (age < 200) d.fillRect(0, 0, 240, 135, mix(th_field, th_wallL, age * 256 / 200));   // opening flash
    else for (int y = (age & 1); y < 135; y += 2) d.drawFastHLine(0, y, 240, COL_BG);       // screen-door dim
    int fade = s_xfade_ms < 300 ? s_xfade_ms * 256 / 300 : 256;
    for (int r = 0; r < 6; r++) {
        int rr = (age - 170) / 3 - r * 16;
        if (rr > 2 && rr < 280) { uint16_t c = mix(COL_BG, (r & 1) ? th_accent : th_wallL, fade); d.drawCircle(cx, cy, rr, c); d.drawCircle(cx, cy, rr + 1, c); }
    }
    char b[16]; snprintf(b, sizeof b, "%s %d", tx("LIV", "LVL"), s_level);
    int wn = dmd_width(b, 3);
    dmd_text((PW - wn) / 2.0f, PH / 2 - 28, b, 3, 1, ((s_anim >> 1) & 1) ? th_accent : COL_WHITE, 0, 0);
    int wt = dmd_width(s_lv.theme, 2);
    dmd_text((PW - wt) / 2.0f, PH / 2 + 8, s_lv.theme, 2, 1, th_accent2, 0, 0);
}
// render one string's LIT dots into the DMD grid at row gridRow; centres if it fits, else marquee-scrolls
static void dmd_grid_line(float px, float py, int P, int cols, int rows, int gridRow, const char *str, unsigned anim, uint16_t on)
{
    int len = (int)strlen(str), tw = len * 6, cx0;
    if (tw <= cols) cx0 = (cols - tw) / 2;
    else cx0 = cols - (int)(anim % (unsigned)(tw + cols));      // scroll in from the right (DMD marquee)
    for (int ci = 0; ci < len; ci++) {
        int gi = glyph_idx(str[ci]);
        for (int c = 0; c < 5; c++) {
            uint8_t colb = FONT[gi][c];
            for (int r = 0; r < 7; r++)
                if ((colb >> r) & 1) {
                    int gx = cx0 + ci * 6 + c, gy = gridRow + r;
                    if (gx >= 0 && gx < cols && gy >= 0 && gy < rows) pcircle(px + 3 + gx * P, py + 3 + gy * P, 1, on);
                }
        }
    }
}
// the pop-up DMD panel — a real dot-matrix display: slides in, shows the message, retracts
static void draw_dmd(void)
{
    if (s_dmd_ms <= 0) return;
    float in = s_dmd_age < 160 ? (float)s_dmd_age / 160 : 1.0f;
    if (s_dmd_ms < 200) in = (float)s_dmd_ms / 200;
    in = 1.0f - (1.0f - in) * (1.0f - in);
    int P = 2, cols = 64, rows = 17;                    // a 64x17 dot matrix spanning the FULL display width
    int panelW = cols * P + 6, panelH = rows * P + 6;
    float px = (PW - panelW) / 2.0f;                    // centred panel
    float py = TWALL + 8 - (1.0f - in) * 56;            // slides in from above
    pbox_round(px - 2, py - 2, panelW + 4, panelH + 4, 4, rgb(48, 34, 8));   // amber bezel
    pbox_round(px, py, panelW, panelH, 3, COL_DARK);                         // black DMD glass
    for (int gy = 0; gy < rows; gy++)                                         // the FULL dim-amber dot grid (single pixels = cheap)
        for (int gx = 0; gx < cols; gx++)
            d.drawPixel(SX(py + 3 + gy * P), SY(px + 3 + gx * P), COL_AMBERD);
    uint16_t on = ((s_anim >> 2) & 1) ? s_dmd_col : mix(s_dmd_col, COL_WHITE, 90);   // lit dots, gentle flicker
    if (s_dmd_sub[0]) {
        dmd_grid_line(px, py, P, cols, rows, 1, s_dmd_line, s_anim / 2, on);
        dmd_grid_line(px, py, P, cols, rows, 10, s_dmd_sub, s_anim / 2, COL_AMBER);
    } else {
        dmd_grid_line(px, py, P, cols, rows, 5, s_dmd_line, s_anim / 2, on);
    }
}
static void draw_spinner(void)
{
    float spd = s_spin_av;
    uint16_t glow = spd > 0.15f ? COL_WHITE : th_accent;
    pcirc_o(SPIN_X, SPIN_Y, 11, mix(th_field2, th_wallL, 100));                         // bezel ring (always)
    if (spd > 0.18f) for (int i = 1; i <= 2; i++) pcirc_o(SPIN_X, SPIN_Y, 11 + i * 2, mix(th_field, glow, 70 / i));   // whirl streaks only when spinning
    pthick(SPIN_X - 10, SPIN_Y, SPIN_X + 10, SPIN_Y, 0.7f, mix(th_field2, COL_STEEL, 120));   // horizontal axle
    pcircle(SPIN_X - 10, SPIN_Y, 2, COL_STEEL); pcircle(SPIN_X + 10, SPIN_Y, 2, COL_STEEL);    // bearing posts
    // foreshortened paddle: fast sin/cos approximation avoids expensive trig
    float a = s_spin_a;
    float ca = cosf(a);
    float sa = sinf(a);
    int hw = 9, hh = 1 + (int)(8.5f * fabsf(ca));
    uint16_t face = mix(ca >= 0 ? th_accent : th_accent2, glow, (int)(70 + 150 * fabsf(ca)));
    pbox_round(SPIN_X - hw, SPIN_Y - hh, hw * 2, hh * 2, 1, face);  // reduced corner radius (1 vs 2)
    if (ca > 0.3f) pbox(SPIN_X - hw, SPIN_Y, hw * 2, 1, mix(face, COL_WHITE, 140));   // edge gleam only when front-facing
    pcircle(SPIN_X, SPIN_Y, 2, COL_STEELL);                                             // hub
}
static void draw_play(void)
{
    draw_field();
    draw_targets();
    for (int i = 0; i < NSLG; i++) draw_sling(&s_slg[i]);
    for (int i = 0; i < s_nbmp; i++) draw_bumper(&s_bmp[i]);
    for (int i = 0; i < NPOST; i++) draw_bumper(&s_post[i]);
    draw_spinner();
    draw_flipper(&s_fl, true); draw_flipper(&s_fr, false);
    shock_draw();
    sparks_draw();
    if (s_bp == BP_PLAY) trail_draw();                                  // fading motion trail behind the ball
    if (s_launch_ms > 0 && s_bp == BP_PLAY)                              // launch motion-streak behind the ball
        pthick(bx, by, bx, by + 16, 2, mix(th_field, COL_WHITE, s_launch_ms * 220 / 280));
    draw_ball();
    if (s_bp == BP_READY) {                                             // FX3 power gauge along the (invisible) chute
        int GH = BWALL - 20 - CHTOP, gx = CHX - 9;
        pbox(gx, CHTOP, 4, GH, mix(COL_DARK, th_field2, 150));          // gauge track
        int bh = (int)(s_plunge * GH);
        pbox(gx, CHTOP + (GH - bh), 4, bh, mix(COL_GREEN, COL_RED, (int)(s_plunge * 256)));   // fill green->red as it charges
        pbox(gx - 1, CHTOP - 1, 6, 2, s_plunge >= 0.99f ? ((s_anim & 2) ? COL_WHITE : COL_RED) : COL_DIM);   // MAX marker (flashes when full)
        const char *pp = s_charging ? tx("RILASCIA!", "RELEASE!") : tx("TIENI CTRL: CARICA", "HOLD CTRL: CHARGE");
        uint16_t ac = ((s_anim >> 2) & 1) ? COL_GOLDL : COL_GOLD;
        dmd_text((PW - dmd_width(pp, 1)) / 2.0f, BWALL - 26, pp, 1, 1, s_charging ? COL_RED : ac, 0, 0);
    }
    draw_score_strip();
    if (s_2x_ms > 0 || s_slow_ms > 0) {                                 // active power-up badge + shrinking timer bar
        const char *pl = s_2x_ms > 0 ? "X2" : "SLOW";
        int rem = s_2x_ms > 0 ? s_2x_ms : s_slow_ms, full = s_2x_ms > 0 ? 9000 : 6000;
        uint16_t pc = s_2x_ms > 0 ? COL_GOLD : COL_CYAN;
        dmd_text(LWALL + 1, 13, pl, 1, 1, ((s_anim >> 2) & 1) ? pc : COL_WHITE, 0, 0);
        pbox(LWALL + 1, 21, rem * 26 / full, 2, pc);
    }
    if (s_tilt > 0)
        for (int k = 0; k < 3; k++) draw_open_frame(k, 1, ((s_anim >> 1) & 1) ? COL_RED : COL_REDD);
    draw_dmd();
    draw_xfade();
}

// ============================ rendering: menus (landscape) ===================
static void felt(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_BG);
    for (int y = 0; y < ch; y += 5) d.drawFastHLine(0, y, W, mix(COL_BG, COL_FIELD, 40));
}
static void mini_table(int cx, int cy)            // little pinball glyph for the menu
{
    d.fillRoundRect(cx - 14, cy - 22, 28, 44, 6, COL_FIELD2);
    d.drawRoundRect(cx - 14, cy - 22, 28, 44, 6, COL_WALLL);
    d.fillCircle(cx - 6, cy - 8, 4, COL_CYAN); d.fillCircle(cx + 6, cy - 10, 4, COL_PINK); d.fillCircle(cx, cy + 2, 4, COL_GREEN);
    d.drawLine(cx - 9, cy + 16, cx - 1, cy + 12, COL_STEELL); d.drawLine(cx + 9, cy + 16, cx + 1, cy + 12, COL_STEELL);
    d.fillCircle(cx + 8, cy + 8, 2, COL_STEELL);
}
#define MENU_Y0 50
#define MENU_DY 17
#define NMENU 4
static int menu_item_y(int i) { return MENU_Y0 + i * MENU_DY; }
static void draw_menu(void)
{
    felt();
    mini_table(26, 56); mini_table(W - 26, 56);
    const char *title = tx("FLIPPER", "PINBALL");
    int lw = (int)strlen(title) * 18;
    ltext((W - lw) / 2 + 1, 9, 3, COL_WALL, title);
    ltext((W - lw) / 2, 8, 3, ((s_anim >> 3) & 1) ? COL_WALLL : COL_CYAN, title);
    char hb[28]; snprintf(hb, sizeof hb, "%s %lu", tx("Record", "High"), (unsigned long)g_hi);
    ltext_c(W / 2, 34, 1, COL_GOLDL, hb);
    const char *items[NMENU] = { tx("Gioca", "Play"), tx("Classifica", "Scores"), tx("Come si gioca", "How to play"), tx("Impostazioni", "Settings") };
    int selw = (int)strlen(items[s_msel]) * 12;
    d.fillRoundRect((W - selw) / 2 - 10, (int)s_capY - 1, selw + 20, 16, 5, mix(COL_DARK, COL_CYAN, 50));
    for (int i = 0; i < NMENU; i++) ltext_c(W / 2, menu_item_y(i), 2, i == s_msel ? COL_WHITE : COL_GREY, items[i]);
}
static void draw_scores(void)
{
    felt();
    ltext_c(W / 2, 8, 2, COL_GOLD, tx("CLASSIFICA", "HIGH SCORES"));
    d.drawFastHLine(20, 28, W - 40, COL_WALL);
    for (int i = 0; i < NTOP; i++) {
        int col = i / 5, row = i % 5, x = 14 + col * 118, y = 36 + row * 16;
        char r[6]; snprintf(r, sizeof r, "%2d", i + 1);
        ltext(x, y, 1, i < 3 ? COL_GOLDL : COL_GREY, r);
        char s[14]; snprintf(s, sizeof s, "%lu", (unsigned long)g_top[i]);
        ltext(x + 18, y, 1, g_top[i] ? (i < 3 ? COL_CREAM : COL_GREY) : COL_DIM, g_top[i] ? s : "---");
    }
}
static void draw_help(void)
{
    int ch = nucleo_app_content_height();
    felt();
    const char *titles[3] = { tx("Obiettivo", "Objective"), tx("Comandi", "Controls"), tx("Punti", "Scoring") };
    ltext_c(W / 2, 6, 2, COL_CYAN, titles[s_help]);
    d.drawFastHLine(20, 26, W - 40, COL_WALL);
    if (s_help == 0) {
        ltext(12, 34, 1, COL_CREAM, tx("Tieni il device IN VERTICALE", "Hold the device VERTICAL"));
        ltext(12, 48, 1, COL_CREAM, tx("(ruota: bordo destro in alto).", "(rotate: right edge up)."));
        ltext(12, 66, 1, COL_CREAM, tx("Lancia la palla, colpisci respingenti", "Launch the ball, hit bumpers"));
        ltext(12, 80, 1, COL_CREAM, tx("e bersagli. Non farla cadere!", "and targets. Don't let it drain!"));
        ltext(12, 98, 1, COL_GOLD, tx("3 bersagli accesi = BONUS + molt.", "3 targets lit = BONUS + mult."));
    } else if (s_help == 1) {
        ltext(12, 34, 1, COL_GREY, tx("1  /  FN", "1  /  FN"));          ltext(150, 34, 1, COL_CYAN, tx("flipper sx (tieni)", "left flip (hold)"));
        ltext(12, 50, 1, COL_GREY, tx("0  /  OPT", "0  /  OPT"));         ltext(150, 50, 1, COL_CYAN, tx("flipper dx (tieni)", "right flip (hold)"));
        ltext(12, 66, 1, COL_GREY, tx("CTRL / GO", "CTRL / GO"));        ltext(150, 66, 1, COL_GREEN, tx("tieni: carica lancio", "hold: charge"));
        ltext(12, 82, 1, COL_GREY, "SPAZIO");                            ltext(150, 82, 1, COL_GOLD, tx("lancio pieno", "full launch"));
        ltext(12, 98, 1, COL_GREY, tx("ALT / TAB", "ALT / TAB"));        ltext(150, 98, 1, COL_ORANGE, tx("tilt / opzioni", "tilt / options"));
    } else {
        ltext(14, 36, 1, COL_CREAM, tx("Respingente", "Bumper"));      ltext_r(W - 14, 36, 1, COL_GOLD, "100");
        ltext(14, 52, 1, COL_CREAM, tx("Slingshot", "Slingshot"));     ltext_r(W - 14, 52, 1, COL_GOLD, "50");
        ltext(14, 68, 1, COL_CREAM, tx("Bersaglio", "Target"));        ltext_r(W - 14, 68, 1, COL_GOLD, "150");
        ltext(14, 84, 1, COL_CREAM, tx("Bonus 3/3", "Bonus 3/3"));     ltext_r(W - 14, 84, 1, COL_GOLDL, "1000");
        ltext(14, 100, 1, COL_PURPLE, tx("Combo respingenti", "Bumper combo")); ltext_r(W - 14, 100, 1, COL_GOLDL, "x");
    }
    int gap = 12, x0 = W / 2 - gap;
    for (int i = 0; i < 3; i++) d.fillCircle(x0 + i * gap, ch - 8, i == s_help ? 3 : 2, i == s_help ? COL_CYAN : COL_DIM);
}
static void draw_settings(void)
{
    felt();
    ltext_c(W / 2, 8, 2, COL_CYAN, tx("Impostazioni", "Settings"));
    const char *names[4] = { tx("Lingua", "Language"), "Audio", tx("Colpetto", "Nudge"), tx("Indietro", "Back") };
    char v[3][16];
    snprintf(v[0], 16, "%s", g_lang ? "English" : "Italiano");
    snprintf(v[1], 16, "%s", g_audio ? "On" : "Off");
    snprintf(v[2], 16, "%s", g_nudge ? "On" : "Off");
    d.fillRoundRect(16, (int)s_capY, W - 32, 18, 5, mix(COL_DARK, COL_CYAN, 50));
    for (int i = 0; i < 4; i++) {
        int y = 32 + i * 20;
        ltext(26, y, 2, i == s_setsel ? COL_WHITE : COL_GREY, names[i]);
        if (i < 3) ltext_r(W - 18, y + 4, 1, COL_GOLD, v[i]);
    }
}
static void draw_over(void)
{
    int ch = nucleo_app_content_height();
    felt();
    bool hs = (s_score >= g_hi && s_score > 0);
    ltext_c(W / 2, 12, 2, hs ? COL_GOLD : COL_RED, hs ? tx("NUOVO RECORD!", "HIGH SCORE!") : tx("FINE PARTITA", "GAME OVER"));
    char b[28]; snprintf(b, sizeof b, "%lu", (unsigned long)s_score);
    ltext_c(W / 2, 44, 3, COL_WHITE, b);
    char hb[28]; snprintf(hb, sizeof hb, "%s %lu", tx("Record", "Best"), (unsigned long)g_hi);
    ltext_c(W / 2, 78, 1, COL_GOLDL, hb);
    ltext_c(W / 2, ch - 14, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_CYAN, tx("INVIO per rigiocare", "ENTER to play again"));
}

// ============================ input + hint ===================================
static void set_hint(void)
{
    switch (s_screen) {
        case ST_MENU: nucleo_app_set_hint(tx("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_SCORES: nucleo_app_set_hint(tx("INVIO/Esc indietro", "ENTER/Esc back")); break;
        case ST_HELP: nucleo_app_set_hint(tx("SX/DX pagine  Esc indietro", "LEFT/RIGHT pages  Esc back")); break;
        case ST_SET:  nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  Esc", "UP/DN  ENTER change  Esc")); break;
        case ST_OPT:  nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  TAB chiudi", "UP/DN  ENTER change  TAB close")); break;
        case ST_OVER: nucleo_app_set_hint(tx("INVIO rigioca  Esc menu", "ENTER replay  Esc menu")); break;
        default: break;   // ST_PLAY is fullscreen portrait: no landscape footer
    }
}
static float cap_target(void)
{
    if (s_screen == ST_MENU) return (float)menu_item_y(s_msel);
    if (s_screen == ST_SET)  return (float)(32 + s_setsel * 20);
    return s_capY;
}
static void go(int s)
{
    s_screen = s;
    nucleo_app_set_fullscreen(s == ST_PLAY);     // the table owns the whole panel (portrait); menus keep the footer
    s_capY = cap_target();
    set_hint();
    nucleo_app_request_draw();
}
static void tab_handler(void)
{
    if (s_screen == ST_PLAY) { s_optsel = 0; go(ST_OPT); sfx(2); }
    else if (s_screen == ST_OPT) { go(ST_PLAY); sfx(3); }
}
static void opt_change(void)
{
    switch (s_optsel) {
        case 0: g_audio ^= 1; sfx(2); break;
        case 1: g_nudge ^= 1; sfx(2); break;
        case 2: g_lang ^= 1; set_hint(); sfx(2); break;
        default: break;
    }
    cfg_write(); nucleo_app_request_draw();
}
static void draw_options(void)                    // a small landscape panel over a frozen table is awkward; keep it simple/landscape
{
    felt();
    ltext_c(W / 2, 10, 2, COL_CYAN, tx("OPZIONI", "OPTIONS"));
    const char *names[3] = { "Audio", tx("Colpetto", "Nudge"), tx("Lingua", "Language") };
    char v[3][16];
    snprintf(v[0], 16, "%s", g_audio ? "On" : "Off");
    snprintf(v[1], 16, "%s", g_nudge ? "On" : "Off");
    snprintf(v[2], 16, "%s", g_lang ? "English" : "Italiano");
    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 20;
        if (i == s_optsel) d.fillRoundRect(16, y - 3, W - 32, 18, 4, mix(COL_DARK, COL_CYAN, 60));
        ltext(26, y, 2, i == s_optsel ? COL_WHITE : COL_GREY, names[i]);
        ltext_r(W - 22, y + 4, 1, COL_GOLDL, v[i]);
    }
    ltext_c(W / 2, 110, 1, COL_DIM, tx("TAB torna al tavolo", "TAB back to table"));
}
static void menu_select(void)
{
    sfx(2);
    if (s_msel == 0) { new_game(); go(ST_PLAY); }
    else if (s_msel == 1) go(ST_SCORES);
    else if (s_msel == 2) { s_help = 0; go(ST_HELP); }
    else { s_setsel = 0; go(ST_SET); }
}
static void settings_change(void)
{
    switch (s_setsel) {
        case 0: g_lang ^= 1; cfg_write(); set_hint(); sfx(2); break;
        case 1: g_audio ^= 1; cfg_write(); sfx(2); break;
        case 2: g_nudge ^= 1; cfg_write(); sfx(2); break;
        default: sfx(3); s_msel = 2; go(ST_MENU); return;
    }
    nucleo_app_request_draw();
}
static void on_key(int k, char ch)
{
    switch (s_screen) {
        case ST_MENU:
            if (k == NK_UP)        { s_msel = (s_msel + NMENU - 1) % NMENU; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % NMENU; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) menu_select();
            return;
        case ST_SCORES:
            if (k == NK_ENTER || k == NK_RIGHT) { sfx(3); go(ST_MENU); }
            return;
        case ST_HELP:
            if (k == NK_RIGHT)      { s_help = (s_help + 1) % 3; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) { sfx(3); go(ST_MENU); }
            return;
        case ST_SET:
            if (k == NK_UP)        { s_setsel = (s_setsel + 3) % 4; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % 4; s_capY = cap_target(); sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_change();
            return;
        case ST_OPT:
            if (k == NK_UP)        { s_optsel = (s_optsel + 2) % 3; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_optsel = (s_optsel + 1) % 3; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) opt_change();
            return;
        case ST_PLAY:
            if (s_bp == BP_READY && (ch == ' ' || k == NK_ENTER || k == NK_DOWN || k == NK_UP)) launch_ball(1.0f);   // tap = full-power launch
            else if (ch == '1') { s_ltap_ms = 150; sfx(5); }                    // '1' -> left flipper (tap)
            else if (k == NK_RIGHT) { s_rtap_ms = 150; sfx(5); }                // RIGHT -> right flipper (tap; OPT is the hold)
            else if (ch == 'n' || ch == 'N') nudge((s_anim & 1) ? 1 : -1);
            else if ((ch == ' ' || k == NK_ENTER) && s_bp == BP_DRAIN && s_balls > 0) ball_to_lane();
            return;
        case ST_OVER:
            if (k == NK_ENTER || ch == ' ') { new_game(); go(ST_PLAY); }
            return;
        default: return;
    }
}
static bool on_back(int key)
{
    if (key == NK_LEFT) {                          // LEFT routes here (back handler) — use it for the left flipper in play
        if (s_screen == ST_PLAY) { s_ltap_ms = 150; sfx(5); return true; }
        if (s_screen == ST_HELP) { s_help = (s_help + 2) % 3; sfx(1); nucleo_app_request_draw(); return true; }
        if (s_screen == ST_SET)  { settings_change(); return true; }
    }
    switch (s_screen) {                            // Esc / Back
        case ST_MENU: return false;                // close app
        case ST_OPT:  go(ST_PLAY); sfx(3); return true;
        case ST_PLAY: sfx(3); s_msel = 0; go(ST_MENU); return true;
        default: sfx(3); s_msel = 0; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void on_draw(void)
{
    switch (s_screen) {
        case ST_MENU: draw_menu(); break;
        case ST_SCORES: draw_scores(); break;
        case ST_HELP: draw_help(); break;
        case ST_SET:  draw_settings(); break;
        case ST_PLAY: draw_play(); break;
        case ST_OPT:  draw_options(); break;
        case ST_OVER: draw_over(); break;
        default: break;
    }
}
static bool poll(void)
{
    s_now = esp_timer_get_time() / 1000;
    int dt = (int)(s_now - s_last); if (dt < 0) dt = 0; if (dt > 60) dt = 60;
    s_last = s_now;
    bool menu_glide = false;

    // eased score readout
    if (s_disp_score != s_score) {
        unsigned step = (s_score - s_disp_score) / 6 + 1;
        s_disp_score += step; if (s_disp_score > s_score) s_disp_score = s_score;
    }
    if (s_dmd_ms > 0) { s_dmd_age += dt; dec(&s_dmd_ms, dt); }
    sparks_step(dt);
    shock_step(dt);
    for (int i = 0; i < s_nbmp; i++) dec(&s_bmp[i].lit_ms, dt);
    for (int i = 0; i < NPOST; i++) dec(&s_post[i].lit_ms, dt);
    for (int i = 0; i < NSLG; i++) dec(&s_slg[i].lit_ms, dt);
    for (int i = 0; i < s_ntgt; i++) dec(&s_tgt[i].hit_ms, dt);
    if (s_spin_cool > 0) dec(&s_spin_cool, dt);
    if (s_spin_av > 0) {                                                                // spinner coasts down, ticking points each turn
        int rev0 = (int)(s_spin_a / 6.2832f);
        s_spin_a += s_spin_av * (dt / 16.0f);
        int rev1 = (int)(s_spin_a / 6.2832f);
        if (rev1 != rev0 && s_bp == BP_PLAY) { add_score(25); sfx(27); }   // spinner ratchet: a tick EVERY turn (slows as it winds down)
    }
    s_spin_av *= 0.975f; if (s_spin_av < 0.01f) s_spin_av = 0;

    if (s_screen == ST_PLAY) {
        uint8_t m = nucleo_kbd_mods();
        // NOTE: the Cardputer's physical labels are offset from the driver's modifier bits.
        // Physical FN -> NK_MOD_CTRL, OPT -> NK_MOD_ALT, CTRL -> NK_MOD_FN, ALT -> NK_MOD_GUI.
        static bool s_l1_prev = false, s_r0_prev = false;
        bool inplay    = (s_bp == BP_PLAY);
        bool leftKey   = nucleo_kbd_char_down('1');  // '1' held -> left flipper STAYS up (printable keys are reliable)
        bool rightKey  = nucleo_kbd_char_down('0');  // '0' held -> right flipper STAYS up
        bool leftMod   = (m & NK_MOD_CTRL) != 0;     // physical FN   -> left flipper (HOLD), alternative
        bool rightMod  = (m & NK_MOD_ALT)  != 0;     // physical OPT  -> right flipper (HOLD), alternative
        bool chargeMod = (m & NK_MOD_FN)   != 0;     // physical CTRL -> charge the plunger
        bool leftHeld  = leftKey  || leftMod;
        bool rightHeld = rightKey || rightMod;
        if (inplay && leftHeld  && !s_l1_prev) sfx(5);                           // flipper clack on press (either input)
        if (inplay && rightHeld && !s_r0_prev) sfx(5);
        s_l1_prev = leftHeld; s_r0_prev = rightHeld;
        if ((m & NK_MOD_GUI) && !(s_mods_prev & NK_MOD_GUI)) nudge((s_anim & 1) ? 1 : -1);   // physical ALT -> tilt (rising edge)
        if (s_bp == BP_READY) {                                  // FX3 plunger: hold CTRL (or GO) to charge, release to fire
            bool want = chargeMod || s_go_held;
            if (want) {
                if (s_plunge < 1.0f) { s_plunge += 0.020f * (dt / 16.0f); if (s_plunge > 1.0f) { s_plunge = 1.0f; s_atmax = 0; } }
                else { s_atmax += dt; if (s_atmax > 1600) { s_plunge = 0; s_atmax = 0; } }   // held at max too long -> overheat reset
                s_charging = true;
            } else if (s_charging) launch_ball(s_plunge);        // released -> fire at the charged power
        }
        s_mods_prev = m;
        if (s_launch_ms > 0) dec(&s_launch_ms, dt);
        if (s_ltap_ms > 0) dec(&s_ltap_ms, dt);
        if (s_rtap_ms > 0) dec(&s_rtap_ms, dt);
        s_fl.held = inplay && (leftHeld  || s_ltap_ms > 0);                      // '1'/FN held (or LEFT tap) -> left flipper STAYS up
        s_fr.held = inplay && (rightHeld || s_rtap_ms > 0);                      // '0'/OPT held (or RIGHT tap) -> right flipper STAYS up
        flipper_step(&s_fl, dt); flipper_step(&s_fr, dt);
        step_ball(dt);
        if (s_bp == BP_PLAY) { if (s_2x_ms > 0) dec(&s_2x_ms, dt); if (s_slow_ms > 0) dec(&s_slow_ms, dt); }
        if (s_xfade_ms > 0) { s_xfade_age += dt; dec(&s_xfade_ms, dt); }
        if (s_bp == BP_PLAY && s_xfade_ms <= 0 && s_lv_score >= (unsigned)s_lv.goal) level_up();  // reached the level goal -> advance
        if (s_bp == BP_DRAIN && s_balls <= 0 && s_dmd_ms <= 0) { go(ST_OVER); return true; }   // game over screen after the DMD toll
        if (s_bp == BP_DRAIN && s_balls > 0 && s_dmd_ms <= 0) ball_to_lane();                   // auto-serve the next ball
    } else if (s_screen == ST_MENU || s_screen == ST_SET) {
        float tgt = cap_target();
        if (s_capY != tgt) { s_capY += (tgt - s_capY) * 0.45f; if (fabsf(tgt - s_capY) < 0.5f) s_capY = tgt; menu_glide = true; }
    }

    if (s_screen == ST_HELP || s_screen == ST_OPT || s_screen == ST_SCORES) return false;   // static
    if ((s_screen == ST_MENU || s_screen == ST_SET) && !menu_glide && s_dmd_ms <= 0) return false;
    if (s_now - s_frame < 20) return false;                            // ~50 Hz for smooth physics
    s_frame = s_now; s_anim++;
    return true;
}
static void ptt_fn(bool on) { s_go_held = on; }     // GO button held -> charge the plunger
static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(85);
    sfx_cache_check();
    presynth();
    if (!s_spk) s_spk = (Spark *)calloc(NSPK, sizeof *s_spk);   // freed in on_exit; null = sparks no-op
    s_level = 1; s_lv_score = 0; s_xfade_ms = 0; s_charging = false; s_launch_ms = 0;
    apply_level(1);
    if (s_spk) memset(s_spk, 0, sizeof(Spark) * NSPK);
    s_score = s_disp_score = 0; s_balls = START_BALLS; s_mult = 1; s_dmd_ms = 0;
    s_bp = BP_READY; ball_to_lane();
    s_screen = ST_MENU; s_msel = 0; s_anim = 0;
    s_capY = cap_target();
    s_now = s_last = s_frame = esp_timer_get_time() / 1000;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);
    nucleo_app_set_ptt_handler(ptt_fn);      // hold the GO button to charge the plunger (alternative to SHIFT)
    nucleo_app_set_fullscreen(false);
    set_hint();
    sfx(2);
    nucleo_app_request_draw();
}
static void on_exit(void) { nucleo_audio_stop(); cfg_write(); free(s_spk); s_spk = nullptr; }

extern "C" void nucleo_register_pinball(void)
{
    static const nucleo_app_def_t app = {
        "pinball", "Flipper", "Games", "Flipper verticale: tieni il device in verticale",
        'F', C_BLUE, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP
    };
    nucleo_app_register(&app);
}
