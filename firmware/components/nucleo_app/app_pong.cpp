// app_pong.cpp — NucleoOS "Pong": a juiced, procedurally-evolving Pong — playable 1v1 over the air
// between two Cardputers (ESP-NOW) or solo vs an offline CPU. Category "Games".
//
// Not a bare Pong: every level regenerates a themed arena (hue, mid-field obstacles, ball speed) and
// power-ups spawn on the field (grow/shrink paddles, slow/fast ball, instant point). Glow, ball trail,
// particle bursts, screen-shake, an animated countdown and synthesised sound effects give it arcade feel,
// and a top-10 leaderboard + best-level persist to SD.
//
// Networking — HOST-AUTHORITATIVE (same shape as the web Game Center). The room CREATOR is the host
// (left paddle): it owns ball physics, scoring, level progression and power-up spawning, and broadcasts
// a compact state snapshot ~40 Hz. The JOINER is the guest (right paddle): it sends only its paddle
// position and renders the host's snapshots. Crucially the arena is NOT streamed — the host sends a
// 32-bit seed + level and the guest REGENERATES the identical themed arena deterministically, so two
// devices show the same obstacles for almost no bandwidth. Cosmetic juice (trail/particles/shake/sound)
// is computed locally on each side. A dropped frame is simply superseded by the next (nucleo_pnet is
// best-effort, no ACKs). Offline, the same physics runs locally with a beatable CPU on the right paddle.
//
// Pairing / channel: nucleo_pnet rides the current Wi-Fi channel when on a network (STA or SoftAP) — so
// two Cardputers on the same Wi-Fi meet automatically — else both park on channel 1 for a local match.
//
// Constraints: exclusive_flags = NX_NET_APP (free ~70KB; Wi-Fi STA, which ESP-NOW rides, stays up); ALL
// state static (NO heap — PSRAM-less chip); buffered `d.` drawing (one blit/frame); the keyboard has no
// key-up and only auto-repeats after 350ms, so paddle control is momentum-based (a hold glides, a tap
// nudges). ASCII only. Never name a local `d`. LEFT/Back route to the back handler (framework rule).

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_pnet.h"
#include "nucleo_audio.h"
#include "esp_timer.h"
#include "esp_random.h"
}

// ============================ palette / color helpers ========================
static inline uint16_t rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t mix(uint16_t a, uint16_t b, int t) {       // t 0..256
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar + (br - ar) * t / 256) & 31) << 11) |
                      (((ag + (bg - ag) * t / 256) & 63) << 5) |
                       ((ab + (bb - ab) * t / 256) & 31));
}
static uint16_t hsv(float h, float sa, float v) {          // h 0..360, sa/v 0..1
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = v * sa, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60)      { r = c; g = x; }
    else if (h < 120){ r = x; g = c; }
    else if (h < 180){ g = c; b = x; }
    else if (h < 240){ g = x; b = c; }
    else if (h < 300){ r = x; b = c; }
    else             { r = c; b = x; }
    return rgb((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}
#define COL_WHITE 0xFFFF
#define COL_PL    rgb(96, 212, 236)     // left paddle / player 1 (cyan — stable identity)
#define COL_PR    rgb(255, 150, 80)     // right paddle / player 2 (orange — stable identity)
#define COL_DIM   rgb(96, 106, 140)
#define COL_MUT   rgb(150, 160, 190)
#define COL_GOLD  rgb(255, 200, 80)
#define COL_RED   rgb(244, 84, 72)
#define COL_GREEN rgb(120, 232, 142)

// ============================ geometry =======================================
#define COURT_TOP  22
#define COURT_BOT  133
#define PAD_W      5
#define BASE_PH    24                  // base paddle height
#define BIG_PH     42
#define SMALL_PH   14
#define PADL_FACE  12                  // right edge x of the left paddle
#define PADR_FACE  (W - 12)            // left  edge x of the right paddle
#ifdef BR
#undef BR                              // xtensa specreg.h also defines BR; ours is the ball radius
#endif
#define BR         3
#define TARGET     7                   // first to 7 points wins the match
#define MAXLEVEL   12

#define BASE_SPD   0.118f              // ball speed at level 1 (px/ms)
#define MAX_SPD    0.360f
#define PAD_SPD    0.175f              // human paddle speed (px/ms)
#define HOLD_MS    380                 // motion window per key event (> the 350ms kbd repeat delay)

// ============================ protocol (over nucleo_pnet) ====================
#define PG_M0 'P'
#define PG_M1 'G'
#define PG_VER 2
enum { PG_HELLO = 1, PG_JOIN, PG_ACCEPT, PG_REJECT, PG_STATE, PG_INPUT, PG_BYE };
enum { HS_IDLE = 0, HS_HOSTING };

typedef struct __attribute__((packed)) { uint8_t m0, m1, ver, type; uint8_t status; char name[22]; } pg_hello_t;
typedef struct __attribute__((packed)) { uint8_t m0, m1, ver, type; char name[22]; }              pg_join_t;
typedef struct __attribute__((packed)) { uint8_t m0, m1, ver, type; uint32_t session; uint8_t target; } pg_accept_t;
typedef struct __attribute__((packed)) { uint8_t m0, m1, ver, type; uint32_t seq; float pr; }     pg_input_t;
typedef struct __attribute__((packed)) {
    uint8_t  m0, m1, ver, type;
    uint32_t seq;
    float    bx, by, vx, vy;          // ball
    float    pl, pr, plh, prh;        // paddle centre-y + heights
    uint16_t sl, sr;                  // scores
    uint8_t  phase, cnt, winner, level;
    uint8_t  pu_type, lasthit, pad0, pad1;
    float    pu_x, pu_y;              // active power-up pickup (pu_type 0 = none)
    uint32_t seed;                    // arena seed (guest regenerates the level from seed+level)
} pg_state_t;

// ============================ state ==========================================
enum { ST_MENU = 0, ST_PLAY, ST_OVER, ST_HELP, ST_SCORES, ST_HOST, ST_BROWSE, ST_OPT };
enum { MODE_AI = 0, MODE_HOST, MODE_GUEST };
enum { PH_COUNT = 0, PH_PLAY, PH_POINT, PH_OVER };
enum { PU_NONE = 0, PU_GROW, PU_SHRINK, PU_SLOW, PU_FAST, PU_POINT, PU_N };

static int    s_screen, s_mode;
static int    s_msel;                  // menu selection
static int    s_optsel;                // settings-screen selection
static int    s_diff = 1;              // 0 easy, 1 normal, 2 hard
static int    g_lang = 0;              // 0 = Italiano, 1 = English (bilingual via tx())
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;

// game
static float  bx, by, vx, vy;          // ball
static float  pl, pr;                   // paddle centre-y
static float  s_lh, s_rh;               // current paddle heights (eased)
static int    s_lh_t, s_rh_t;           // height-effect expiry (ms; 0 = none -> base)
static int    sl, sr;                   // scores
static int    s_phase, s_cnt, s_winner, s_level, s_lasthit;
static int    s_phtimer;                // ms left in COUNT/POINT
static int    s_servedir;
static uint32_t s_seed;

// power-up pickup on the field
static bool   s_pu_on;
static int    s_pu_type;
static float  s_pu_x, s_pu_y;
static int    s_pu_next;                // host: ms clock until next spawn

// procedural arena
#define NOBS 6
struct Obs { float x, y, w, h; };
static Obs    s_obs[NOBS];
static int    s_nobs;
static uint16_t th_field, th_field2, th_net, th_obs, th_ball, th_glow;
static uint32_t s_rng;

// cosmetics (local only)
#define NSPK 26
struct Spark { float x, y, vx, vy; int life, max; uint16_t col; };
static Spark  s_spk[NSPK];
#define NTRAIL 9
static float  s_tx[NTRAIL], s_ty[NTRAIL];
static int    s_ti;
static float  s_shake;
static int    s_hitl, s_hitr;           // paddle hit-flash timers (ms)
#define NWAVE 6                          // expanding shock-rings on every bounce
struct Wave { float x, y, r, max; int life, lmax; uint16_t col; };
static Wave   s_wave[NWAVE];
static int    s_flash;                   // full-field flash timer on a scored point (ms)
static uint16_t s_flashcol;

// local input (momentum)

// net
static uint8_t  s_peer[6];
static bool     s_haspeer;
static uint32_t s_session, s_txseq, s_rxseq;
static int64_t  s_last_rx, s_last_tx, s_last_hello, s_state_ms, s_join_t0;
static bool     s_join_pending, s_netlost, s_peerleft;
static char     s_joinname[22];         // room being joined — named feedback for the guest while connecting

struct Host { uint8_t mac[6]; char name[22]; int64_t seen; };
#define NHOST 6
static Host   s_hosts[NHOST];
static int    s_nhost, s_bsel;

// persistence / settings
static int    g_audio = 1;
static unsigned g_best_level = 0;
#define NTOP 8
static unsigned g_top[NTOP];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static float   frnd(float a, float b) { return a + (b - a) * ((esp_random() & 0xFFFF) / 65535.0f); }
static uint32_t xr(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static float    xrf(void) { return (xr() & 0xFFFF) / 65535.0f; }

// ============================ text helpers ===================================
static void txt(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static const char *tx(const char *it, const char *en) { return g_lang ? en : it; }   // bilingual string pick
static void txt_c(int cx, int y, int sz, uint16_t col, const char *s) { txt(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void txt_r(int rx, int y, int sz, uint16_t col, const char *s) { txt(rx - (int)strlen(s) * 6 * sz, y, sz, col, s); }

// ============================ audio (synth -> SD cache, like Flipper) ========
#define DIRR "/sd/data/pong"
#define NSFX 13
static const char *sfx_name(int id) {
    switch (id) {
        case 1: return "nav";   case 2: return "sel";   case 3: return "back";  case 4: return "count";
        case 5: return "serve"; case 6: return "wall";  case 7: return "hit";   case 8: return "obst";
        case 9: return "power"; case 10:return "score"; case 11:return "win";   case 12:return "lose";
        case 13:return "level"; default: return "x";
    }
}
static int build_voices(int id, notify_voice_t *v) {
    switch (id) {
        case 1:  notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.5f; return 1;
        case 2:  notify__voice(&v[0], 659.25f, 0, 0.06f); notify__voice(&v[1], 987.77f, 0.04f, 0.09f); return 2;
        case 3:  notify__voice(&v[0], 520, 0, 0.06f); notify__voice(&v[1], 350, 0.05f, 0.09f); return 2;
        case 4:  notify__voice(&v[0], 880, 0, 0.05f); v[0].amp = 0.6f; return 1;                    // countdown blip
        case 5:  notify__voice(&v[0], 300, 0, 0.04f); notify__voice(&v[1], 600, 0.03f, 0.06f); return 2; // serve whoosh
        case 6:  notify__voice(&v[0], 420, 0, 0.025f); v[0].amp = 0.7f; return 1;                   // wall tick
        case 7:  notify__voice(&v[0], 540, 0, 0.03f); v[0].amp = 0.9f; notify__voice(&v[1], 820, 0, 0.02f); return 2; // paddle pock
        case 8:  notify__voice(&v[0], 240, 0, 0.04f); notify__voice(&v[1], 360, 0.02f, 0.04f); return 2; // obstacle thunk
        case 9:  notify__voice(&v[0], 880, 0, 0.05f); notify__voice(&v[1], 1174.7f, 0.05f, 0.06f);  // power-up sparkle
                 notify__voice(&v[2], 1568.0f, 0.10f, 0.10f); return 3;
        case 10: notify__voice(&v[0], 523.25f, 0, 0.07f); notify__voice(&v[1], 392, 0.06f, 0.10f); return 2; // point
        case 11: notify__voice(&v[0], 523.25f, 0, 0.10f); notify__voice(&v[1], 659.25f, 0.08f, 0.10f);   // win fanfare
                 notify__voice(&v[2], 783.99f, 0.16f, 0.12f); notify__voice(&v[3], 1046.5f, 0.24f, 0.18f); return 4;
        case 12: notify__voice(&v[0], 392, 0, 0.12f); notify__voice(&v[1], 294, 0.10f, 0.14f);          // lose toll
                 notify__voice(&v[2], 196, 0.22f, 0.22f); return 3;
        case 13: notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 988, 0.06f, 0.08f);      // level-up
                 notify__voice(&v[2], 1318.5f, 0.13f, 0.13f); return 3;
    }
    return 0;
}
static bool sfx_important(int id) { return id == 5 || id == 9 || id == 10 || id == 11 || id == 12 || id == 13; }
static void sfx(int id) {
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else { notify_voice_t v[8]; int nv = build_voices(id, v); if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return; }
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); }
static void presynth(void) {
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

// ============================ persistence ====================================
#define CFG_MAGIC 0x504F4E47u   // 'PONG'
static void cfg_write(void) {
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int audio, diff, lang; unsigned best; unsigned top[NTOP]; } c = { CFG_MAGIC, g_audio, s_diff, g_lang, g_best_level, { 0 } };
    for (int i = 0; i < NTOP; i++) c.top[i] = g_top[i];
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void) {
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int audio, diff, lang; unsigned best; unsigned top[NTOP]; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) {
        g_audio = c.audio ? 1 : 0; s_diff = (c.diff >= 0 && c.diff <= 2) ? c.diff : 1;
        g_lang = c.lang ? 1 : 0;
        g_best_level = c.best;
        for (int i = 0; i < NTOP; i++) g_top[i] = c.top[i];
    }
}
static void leaderboard_add(unsigned sc) {
    if (sc == 0) return;
    int pos = NTOP;
    for (int i = 0; i < NTOP; i++) if (sc > g_top[i]) { pos = i; break; }
    if (pos >= NTOP) return;
    for (int i = NTOP - 1; i > pos; i--) g_top[i] = g_top[i - 1];
    g_top[pos] = sc;
}

// ============================ cosmetics ======================================
static void shake(float a) { if (a > s_shake) s_shake = a; }
static void spark_burst(float x, float y, int n, uint16_t col) {
    for (int i = 0; i < NSPK && n > 0; i++) {
        if (s_spk[i].life > 0) continue;
        float a = frnd(0, 6.283f), sp = frnd(0.25f, 1.4f);
        s_spk[i].x = x; s_spk[i].y = y;
        s_spk[i].vx = cosf(a) * sp; s_spk[i].vy = sinf(a) * sp;
        s_spk[i].max = s_spk[i].life = 12 + (int)frnd(0, 16);
        s_spk[i].col = col; n--;
    }
}
static void sparks_step(int dt) {
    float k = dt / 16.0f;
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        s_spk[i].x += s_spk[i].vx * k; s_spk[i].y += s_spk[i].vy * k; s_spk[i].vy += 0.012f * k;
        s_spk[i].life -= dt;
    }
}
static void wave_spawn(float x, float y, float maxr, uint16_t col) {
    for (int i = 0; i < NWAVE; i++) {
        if (s_wave[i].life > 0) continue;
        s_wave[i].x = x; s_wave[i].y = y; s_wave[i].r = 2; s_wave[i].max = maxr;
        s_wave[i].life = s_wave[i].lmax = 260; s_wave[i].col = col;
        return;
    }
}
static void waves_step(int dt) {
    for (int i = 0; i < NWAVE; i++) {
        if (s_wave[i].life <= 0) continue;
        s_wave[i].life -= dt;
        float p = 1.0f - (float)s_wave[i].life / s_wave[i].lmax;
        s_wave[i].r = 2 + (s_wave[i].max - 2) * p;
    }
}
static void flash(uint16_t col, int ms) { s_flash = ms; s_flashcol = col; }

// ============================ procedural arena ===============================
static const char *pu_glyph(int t) {
    switch (t) { case PU_GROW: return "+"; case PU_SHRINK: return "-"; case PU_SLOW: return "S";
                 case PU_FAST: return "F"; case PU_POINT: return "*"; default: return "?"; }
}
static uint16_t pu_color(int t) {
    switch (t) { case PU_GROW: return COL_GREEN; case PU_SHRINK: return COL_RED; case PU_SLOW: return rgb(120,180,255);
                 case PU_FAST: return COL_GOLD; case PU_POINT: return rgb(255,120,220); default: return COL_WHITE; }
}
// Build level `lv` deterministically from s_seed: theme hue, mirrored mid-field obstacles, ball tint.
static void regen_level(int lv) {
    s_level = lv;
    s_rng = s_seed ^ (0x9E3779B1u * (uint32_t)(lv + 1));
    if (!s_rng) s_rng = 0xC0FFEEu;
    float hue = fmodf((s_seed % 360) + lv * 47.0f, 360.0f);
    th_field  = hsv(hue, 0.55f, 0.16f);
    th_field2 = hsv(hue, 0.60f, 0.09f);
    th_net    = hsv(hue, 0.35f, 0.45f);
    th_obs    = hsv(hue + 30.0f, 0.65f, 0.70f);
    th_ball   = hsv(hue + 180.0f, 0.25f, 1.0f);
    th_glow   = hsv(hue + 180.0f, 0.50f, 0.55f);
    // mirrored obstacle pairs (fair for 1v1); count grows with level
    int pairs = (lv - 1) / 2; if (pairs > NOBS / 2) pairs = NOBS / 2;
    s_nobs = 0;
    for (int i = 0; i < pairs; i++) {
        float w = 6 + xrf() * 6, h = 16 + xrf() * 26;
        float x = 42 + xrf() * 26;                                  // left-half x in [42..68]
        float y = COURT_TOP + 12 + xrf() * (COURT_BOT - COURT_TOP - 24 - h);
        s_obs[s_nobs++] = { x, y, w, h };
        s_obs[s_nobs++] = { W - x - w, y, w, h };                   // mirror to the right half
    }
}

// ============================ game flow ======================================
static float lvl_speed(void) { float s = BASE_SPD + (s_level - 1) * 0.011f; return s > MAX_SPD ? MAX_SPD : s; }
static void serve(void) {
    bx = W / 2.0f; by = (COURT_TOP + COURT_BOT) / 2.0f;
    float sp = lvl_speed();
    vx = s_servedir * sp;
    vy = frnd(-0.05f, 0.05f);
    s_lasthit = 0; s_ti = 0;
    for (int i = 0; i < NTRAIL; i++) { s_tx[i] = bx; s_ty[i] = by; }
}
static void new_match(int mode) {
    s_mode = mode;
    sl = sr = 0; s_winner = 0;
    pl = pr = (COURT_TOP + COURT_BOT) / 2.0f;
    s_lh = s_rh = BASE_PH; s_lh_t = s_rh_t = 0;
    s_servedir = (esp_random() & 1) ? 1 : -1;
    s_seed = esp_random();
    regen_level(1);
    s_pu_on = false; s_pu_type = PU_NONE; s_pu_next = 5000 + (int)frnd(0, 4000);
    s_phase = PH_COUNT; s_phtimer = 1800; s_cnt = 3;
    s_netlost = s_peerleft = false;
    s_last_rx = s_last_tx = s_state_ms = now_ms();
    memset(s_spk, 0, sizeof s_spk);
    memset(s_wave, 0, sizeof s_wave);
    s_shake = 0; s_hitl = s_hitr = 0; s_flash = 0;
    serve();
}
static void score_point(int who) {                 // who = 1 left, 2 right
    if (who == 1) sl++; else sr++;
    s_servedir = (who == 1) ? 1 : -1;
    shake(6.0f); sfx(10);
    flash(who == 1 ? COL_PL : COL_PR, 170);
    wave_spawn(who == 1 ? (float)W : 0.0f, by, 64, who == 1 ? COL_PL : COL_PR);
    if (sl >= TARGET || sr >= TARGET) {
        s_phase = PH_OVER; s_winner = who; s_phtimer = 0;
        sfx((s_mode == MODE_AI) ? (who == 1 ? 11 : 12) : 11);
        return;
    }
    int nl = (sl + sr) / 2 + 1; if (nl > MAXLEVEL) nl = MAXLEVEL;
    if (nl != s_level) { regen_level(nl); sfx(13); shake(7.0f); }
    s_pu_on = false;                               // clear any stale pickup between points
    s_phase = PH_POINT; s_phtimer = 700;
}

// ============================ power-ups ======================================
static void apply_pu(int t, int hitter) {          // hitter = 1 left, 2 right (0 = nobody -> ignore)
    if (hitter == 0) return;
    switch (t) {
        case PU_GROW:   if (hitter == 1) s_lh_t = 8000; else s_rh_t = 8000; break;     // grower handled in ease
        case PU_SHRINK: if (hitter == 1) s_rh_t = -8000; else s_lh_t = -8000; break;   // negative = shrink opp
        case PU_SLOW:   vx *= 0.7f; vy *= 0.7f; break;
        case PU_FAST: {
            float cur = sqrtf(vx * vx + vy * vy), m = 1.32f;
            if (cur * m > MAX_SPD) m = MAX_SPD / (cur + 0.0001f);
            vx *= m; vy *= m;
        } break;
        case PU_POINT:  score_point(hitter); return;                                   // instant point (also ends rally)
        default: break;
    }
    sfx(9); shake(4.0f);
    flash(pu_color(t), 110);
    wave_spawn(s_pu_x, s_pu_y, 30, pu_color(t));
    spark_burst(s_pu_x, s_pu_y, 12, pu_color(t));
}
// ease paddle heights toward their effect target (timers in s_lh_t/s_rh_t; sign encodes grow/shrink)
static void heights_step(int dt) {
    int *T[2] = { &s_lh_t, &s_rh_t };
    float *Hc[2] = { &s_lh, &s_rh };
    for (int s = 0; s < 2; s++) {
        float tgt = BASE_PH;
        if (*T[s] > 0)      { tgt = BIG_PH;   *T[s] -= dt; if (*T[s] < 0) *T[s] = 0; }
        else if (*T[s] < 0) { tgt = SMALL_PH; *T[s] += dt; if (*T[s] > 0) *T[s] = 0; }
        *Hc[s] += (tgt - *Hc[s]) * (dt / 120.0f);
    }
}

// ============================ physics (host + AI) ============================
static void reflect_paddle(int side) {             // side 1 left, 2 right
    float pad = (side == 1) ? pl : pr, ph = (side == 1) ? s_lh : s_rh;
    float rel = (by - pad) / (ph / 2.0f);
    if (rel < -1.2f) rel = -1.2f;
    if (rel > 1.2f) rel = 1.2f;
    float sp = sqrtf(vx * vx + vy * vy) * 1.06f; if (sp > MAX_SPD) sp = MAX_SPD;
    vx = (side == 1) ? fabsf(vx) : -fabsf(vx);
    vy += rel * 0.10f;
    float n = sqrtf(vx * vx + vy * vy); if (n > 0.0001f) { vx = vx / n * sp; vy = vy / n * sp; }
    bx = (side == 1) ? (PADL_FACE + BR + 0.5f) : (PADR_FACE - BR - 0.5f);
    s_lasthit = side;
    if (side == 1) s_hitl = 160; else s_hitr = 160;
    spark_burst(bx, by, 6, (side == 1) ? COL_PL : COL_PR);
    wave_spawn(bx, by, 26, (side == 1) ? COL_PL : COL_PR);
    shake(3.0f);
    sfx(7);
}
static void phys_step(float dt) {
    if (s_phase == PH_COUNT) {
        int prev = s_cnt;
        s_phtimer -= (int)dt;
        s_cnt = s_phtimer / 600 + 1; if (s_cnt < 1) s_cnt = 1;
        if (s_cnt != prev) sfx(4);
        if (s_phtimer <= 0) { s_phase = PH_PLAY; sfx(5); }
        return;
    }
    if (s_phase == PH_POINT) {
        s_phtimer -= (int)dt;
        if (s_phtimer <= 0) { serve(); s_phase = PH_PLAY; sfx(5); }
        return;
    }
    if (s_phase != PH_PLAY) return;

    bx += vx * dt; by += vy * dt;

    if (by < COURT_TOP + BR) { by = COURT_TOP + BR; vy = -vy; sfx(6); spark_burst(bx, by, 3, th_glow); wave_spawn(bx, by, 15, th_glow); }
    if (by > COURT_BOT - BR) { by = COURT_BOT - BR; vy = -vy; sfx(6); spark_burst(bx, by, 3, th_glow); wave_spawn(bx, by, 15, th_glow); }

    if (vx < 0 && bx - BR <= PADL_FACE && bx - BR >= PADL_FACE - 8 &&
        by >= pl - s_lh / 2 - BR && by <= pl + s_lh / 2 + BR) reflect_paddle(1);
    if (vx > 0 && bx + BR >= PADR_FACE && bx + BR <= PADR_FACE + 8 &&
        by >= pr - s_rh / 2 - BR && by <= pr + s_rh / 2 + BR) reflect_paddle(2);

    // obstacles (AABB reflect on the smaller penetration axis)
    for (int i = 0; i < s_nobs; i++) {
        Obs *o = &s_obs[i];
        if (bx + BR < o->x || bx - BR > o->x + o->w || by + BR < o->y || by - BR > o->y + o->h) continue;
        float penL = (bx + BR) - o->x, penR = (o->x + o->w) - (bx - BR);
        float penT = (by + BR) - o->y, penB = (o->y + o->h) - (by - BR);
        float mnx = penL < penR ? penL : penR, mny = penT < penB ? penT : penB;
        if (mnx < mny) { vx = -vx; bx += (penL < penR) ? -mnx : mnx; }
        else           { vy = -vy; by += (penT < penB) ? -mny : mny; }
        sfx(8); spark_burst(bx, by, 4, th_obs); wave_spawn(bx, by, 18, th_obs); shake(2.0f);
        break;
    }

    // power-up pickup collision
    if (s_pu_on) {
        float dx = bx - s_pu_x, dy = by - s_pu_y;
        if (dx * dx + dy * dy < (BR + 7) * (BR + 7)) { s_pu_on = false; apply_pu(s_pu_type, s_lasthit); }
    }

    if (bx < -3)         { score_point(2); return; }
    else if (bx > W + 3) { score_point(1); return; }

    // clamp crazy speeds
    float sp = sqrtf(vx * vx + vy * vy);
    if (sp > MAX_SPD) { vx *= MAX_SPD / sp; vy *= MAX_SPD / sp; }
}
// Host owns power-up spawning (guest receives it in the snapshot).
static void pu_spawn_step(int dt) {
    if (s_phase != PH_PLAY) return;
    if (s_pu_on) return;
    s_pu_next -= dt;
    if (s_pu_next > 0) return;
    int unlocked = 2 + s_level / 2; if (unlocked > PU_N - 1) unlocked = PU_N - 1;
    s_pu_type = 1 + (int)(esp_random() % (uint32_t)unlocked);
    s_pu_x = frnd(78, W - 78);
    s_pu_y = frnd(COURT_TOP + 16, COURT_BOT - 16);
    s_pu_on = true;
    s_pu_next = 6000 + (int)frnd(0, 6000);
}

static void ai_step(float dt) {
    static const float SPD[3] = { 0.090f, 0.130f, 0.168f };
    static const float DZ[3]  = { 11.0f, 6.0f, 3.0f };
    float spd = SPD[s_diff], dz = DZ[s_diff];
    float target = (s_phase == PH_PLAY && vx > 0) ? by : (COURT_TOP + COURT_BOT) / 2.0f;
    float dy = target - pr, step = spd * dt;
    if (fabsf(dy) > dz) pr += (dy > 0 ? 1 : -1) * (fabsf(dy) < step ? fabsf(dy) : step);
    float lo = COURT_TOP + s_rh / 2, hi = COURT_BOT - s_rh / 2;
    if (pr < lo) pr = lo;
    if (pr > hi) pr = hi;
}
static void move_local_paddle(float dt) {
    // smooth continuous movement from physically-held keys (no jittery momentum):
    // arrows ; (up) / . (down), plus Z (up) / N (down)
    int dir = 0;
    if (nucleo_kbd_char_down(';') || nucleo_kbd_char_down('z')) dir -= 1;
    if (nucleo_kbd_char_down('.') || nucleo_kbd_char_down('n')) dir += 1;
    bool guest = (s_mode == MODE_GUEST);
    float *p = guest ? &pr : &pl;
    float ph = guest ? s_rh : s_lh;
    *p += dir * PAD_SPD * dt;
    float lo = COURT_TOP + ph / 2, hi = COURT_BOT - ph / 2;
    if (*p < lo) *p = lo;
    if (*p > hi) *p = hi;
}

// ============================ net send / recv ================================
static void send_state(void) {
    pg_state_t s = { PG_M0, PG_M1, PG_VER, PG_STATE, ++s_txseq,
                     bx, by, vx, vy, pl, pr, s_lh, s_rh, (uint16_t)sl, (uint16_t)sr,
                     (uint8_t)s_phase, (uint8_t)s_cnt, (uint8_t)s_winner, (uint8_t)s_level,
                     (uint8_t)(s_pu_on ? s_pu_type : PU_NONE), (uint8_t)s_lasthit, 0, 0,
                     s_pu_x, s_pu_y, s_seed };
    pnet_send(s_peer, &s, sizeof s);
    s_last_tx = s_now;
}
static void send_input(void) {
    pg_input_t in = { PG_M0, PG_M1, PG_VER, PG_INPUT, ++s_txseq, pr };
    pnet_send(s_peer, &in, sizeof in);
    s_last_tx = s_now;
}
static void send_hello(int status) {
    pg_hello_t h = { PG_M0, PG_M1, PG_VER, PG_HELLO, (uint8_t)status, { 0 } };
    snprintf(h.name, sizeof h.name, "%s", pnet_name());
    pnet_send(NULL, &h, sizeof h);
}
static void send_bye(void) { if (s_haspeer) { uint8_t b[4] = { PG_M0, PG_M1, PG_VER, PG_BYE }; pnet_send(s_peer, b, 4); } }

static void host_add(const uint8_t *mac, const char *name) {
    for (int i = 0; i < s_nhost; i++)
        if (!memcmp(s_hosts[i].mac, mac, 6)) { s_hosts[i].seen = s_now; snprintf(s_hosts[i].name, 22, "%s", name); return; }
    if (s_nhost >= NHOST) return;
    memcpy(s_hosts[s_nhost].mac, mac, 6);
    snprintf(s_hosts[s_nhost].name, 22, "%s", (name && name[0]) ? name : "?");
    s_hosts[s_nhost].seen = s_now; s_nhost++;
}
static void hosts_prune(void) {
    for (int i = 0; i < s_nhost; ) {
        if (s_now - s_hosts[i].seen > 4000) { for (int k = i; k < s_nhost - 1; k++) s_hosts[k] = s_hosts[k + 1]; s_nhost--; }
        else i++;
    }
    if (s_bsel >= s_nhost) s_bsel = s_nhost > 0 ? s_nhost - 1 : 0;
}

static void go(int s);

static void net_handle(const pnet_pkt_t *p) {
    if (p->len < 4 || p->buf[0] != PG_M0 || p->buf[1] != PG_M1 || p->buf[2] != PG_VER) return;
    int type = p->buf[3];

    if (s_screen == ST_BROWSE) {
        if (type == PG_HELLO && p->len >= (int)sizeof(pg_hello_t)) {
            const pg_hello_t *h = (const pg_hello_t *)p->buf;
            if (h->status == HS_HOSTING) host_add(p->mac, h->name);
        } else if (type == PG_ACCEPT && s_join_pending && p->len >= (int)sizeof(pg_accept_t)) {
            const pg_accept_t *a = (const pg_accept_t *)p->buf;
            memcpy(s_peer, p->mac, 6); s_haspeer = true; s_session = a->session;
            s_join_pending = false;
            new_match(MODE_GUEST);
            go(ST_PLAY);
        }
        return;
    }
    if (s_screen == ST_HOST) {
        if (type == PG_JOIN && p->len >= (int)sizeof(pg_join_t)) {
            memcpy(s_peer, p->mac, 6); s_haspeer = true;
            s_session = (esp_random() | 1);
            pg_accept_t a = { PG_M0, PG_M1, PG_VER, PG_ACCEPT, s_session, TARGET };
            pnet_send(s_peer, &a, sizeof a);
            new_match(MODE_HOST);
            go(ST_PLAY);
        }
        return;
    }
    if (s_screen == ST_PLAY || s_screen == ST_OVER) {
        if (!s_haspeer || memcmp(p->mac, s_peer, 6)) return;
        if (type == PG_BYE) { s_peerleft = true; return; }
        if (s_mode == MODE_HOST && type == PG_INPUT && p->len >= (int)sizeof(pg_input_t)) {
            const pg_input_t *in = (const pg_input_t *)p->buf;
            if (in->seq >= s_rxseq) { s_rxseq = in->seq; pr = in->pr; s_last_rx = s_now; }
        } else if (s_mode == MODE_GUEST && type == PG_STATE && p->len >= (int)sizeof(pg_state_t)) {
            const pg_state_t *s = (const pg_state_t *)p->buf;
            if (s->seq < s_rxseq) return;
            s_rxseq = s->seq;
            if (s->seed != s_seed || (int)s->level != s_level) { s_seed = s->seed; regen_level(s->level); }
            int pl_score = sl, pr_score = sr;
            bx = s->bx; by = s->by; vx = s->vx; vy = s->vy; pl = s->pl;
            s_lh = s->plh; s_rh = s->prh;
            sl = s->sl; sr = s->sr; s_phase = s->phase; s_cnt = s->cnt; s_winner = s->winner;
            s_pu_on = (s->pu_type != PU_NONE); s_pu_type = s->pu_type; s_pu_x = s->pu_x; s_pu_y = s->pu_y;
            s_lasthit = s->lasthit;
            if (sl != pl_score || sr != pr_score) {                            // local juice on a scored point
                int who = (sl != pl_score) ? 1 : 2;
                shake(6.0f); sfx(10); flash(who == 1 ? COL_PL : COL_PR, 170);
                wave_spawn(who == 1 ? (float)W : 0.0f, by, 64, who == 1 ? COL_PL : COL_PR);
            }
            s_last_rx = s_state_ms = s_now;
        }
    }
}

// ============================ rendering: play ================================
// neon paddle: an outer glow whose size/brightness pulses with ball proximity, a bright core, a hot edge.
static void draw_paddle(int facex, bool left, float cy, float h, int flsh, int glow) {
    int x = left ? facex - PAD_W : facex;
    int y = (int)(cy - h / 2), ih = (int)h;
    uint16_t base = left ? COL_PL : COL_PR;
    int gw = 2 + glow / 55;
    int gmix = 205 - glow; if (gmix < 40) gmix = 40;
    d.fillRoundRect(x - gw, y - gw, PAD_W + 2 * gw, ih + 2 * gw, 3, mix(base, rgb(4, 5, 12), gmix));
    uint16_t c = flsh > 0 ? COL_WHITE : base;
    d.fillRoundRect(x, y, PAD_W, ih, 2, c);
    d.fillRect(x + (left ? 1 : PAD_W - 2), y + 2, 1, ih - 4, mix(c, COL_WHITE, 170));
}
// synthwave depth grid behind the court (perspective fan + scrolling floor lines), tinted by the level.
static void grid_bg(int ox, int oy) {
    int ft = s_flash > 0 ? (s_flash * 90 / 170) : 0;
    uint16_t base = ft ? mix(rgb(4, 5, 12), s_flashcol, ft) : rgb(4, 5, 12);
    d.fillRect(0, 0, W, H, base);
    int hy = COURT_TOP + 1;
    uint16_t gcol = mix(th_net, base, 120);
    for (int i = -7; i <= 7; i++) {
        int xt = W / 2 + i * 5 + ox, xb = W / 2 + i * 54 + ox;
        d.drawLine(xt, hy + oy, xb, COURT_BOT + oy, gcol);
    }
    float ph = (s_anim % 48) / 48.0f;
    for (int k = 1; k <= 8; k++) {
        float f = (k - ph) / 8.0f;
        if (f <= 0.02f) continue;
        int y = hy + (int)((COURT_BOT - hy) * f * f);
        if (y <= hy || y >= COURT_BOT) continue;
        int t = (int)(40 + 200 * f); if (t > 256) t = 256;
        d.drawFastHLine(0, y + oy, W, mix(gcol, th_glow, t));
    }
    d.drawFastHLine(0, COURT_TOP + oy, W, mix(th_net, COL_WHITE, 70));
    d.drawFastHLine(0, COURT_BOT + oy, W, mix(th_net, COL_WHITE, 70));
}
static void draw_play(void) {
    int ox = 0, oy = 0;
    if (s_shake > 0.4f) { ox = (int)frnd(-s_shake, s_shake); oy = (int)frnd(-s_shake, s_shake); }
    grid_bg(ox, oy);
    // reactive centre net — segments flare as the ball passes their height
    for (int y = COURT_TOP + 4; y < COURT_BOT; y += 11) {
        int prox = (int)fabsf((float)(y + 3) - by);
        int g = prox < 28 ? (96 - prox * 3) : 0;
        d.fillRect(W / 2 - 1 + ox, y + oy, 2, 6, mix(th_net, COL_WHITE, 40 + g));
    }
    // obstacles (neon blocks)
    for (int i = 0; i < s_nobs; i++) {
        Obs *o = &s_obs[i];
        d.fillRoundRect((int)o->x - 1 + ox, (int)o->y - 1 + oy, (int)o->w + 2, (int)o->h + 2, 2, mix(th_obs, rgb(4, 5, 12), 150));
        d.fillRoundRect((int)o->x + ox, (int)o->y + oy, (int)o->w, (int)o->h, 2, th_obs);
        d.fillRect((int)o->x + 1 + ox, (int)o->y + 1 + oy, (int)o->w - 2, 1, mix(th_obs, COL_WHITE, 130));
    }
    // shock-rings
    for (int i = 0; i < NWAVE; i++) {
        if (s_wave[i].life <= 0) continue;
        int a = s_wave[i].life * 256 / s_wave[i].lmax;
        int X = (int)s_wave[i].x + ox, Y = (int)s_wave[i].y + oy, R = (int)s_wave[i].r;
        d.drawCircle(X, Y, R, mix(rgb(4, 5, 12), s_wave[i].col, a));
        if (R > 3) d.drawCircle(X, Y, R - 1, mix(rgb(4, 5, 12), s_wave[i].col, a / 2));
    }
    // power-up pickup: pulsing orb + rotating ring + glyph
    if (s_pu_on) {
        uint16_t pc = pu_color(s_pu_type);
        int X = (int)s_pu_x + ox, Y = (int)s_pu_y + oy, pr0 = 7 + ((s_anim >> 1) & 3);
        d.fillCircle(X, Y, pr0 + 2, mix(pc, rgb(4, 5, 12), 170));
        d.drawCircle(X, Y, pr0, pc);
        float a = s_anim * 0.4f;
        d.fillCircle(X + (int)(cosf(a) * pr0), Y + (int)(sinf(a) * pr0), 1, COL_WHITE);
        d.fillCircle(X, Y, 4, mix(pc, COL_WHITE, 90));
        txt_c(X, Y - 3, 1, rgb(8, 10, 18), pu_glyph(s_pu_type));
    }
    // paddles — glow pulses with how near the ball is to each side
    int gl = (int)(120 - fabsf(bx - PADL_FACE)); if (gl < 20) gl = 20;
    int gr = (int)(120 - fabsf(bx - PADR_FACE)); if (gr < 20) gr = 20;
    draw_paddle(PADL_FACE + ox, true, pl + oy, s_lh, s_hitl, gl);
    draw_paddle(PADR_FACE + ox, false, pr + oy, s_rh, s_hitr, gr);
    // ball — a plasma comet: fading trail, CRT chromatic split, white-hot core
    if (s_phase == PH_PLAY) {
        float rbx = bx, rby = by;
        if (s_mode == MODE_GUEST) { float age = (float)(s_now - s_state_ms); if (age > 60) age = 60; rbx = bx + vx * age; rby = by + vy * age; }
        for (int i = 0; i < NTRAIL; i++) {
            int idx = (s_ti - 1 - i + NTRAIL * 2) % NTRAIL;
            int t = 220 - i * 24; if (t < 0) t = 0;
            d.fillCircle((int)s_tx[idx] + ox, (int)s_ty[idx] + oy, ((BR + 1) * (NTRAIL - i)) / NTRAIL, mix(rgb(4, 5, 12), th_glow, t));
        }
        if (rbx < 1) rbx = 1;
        if (rbx > W - 1) rbx = W - 1;
        int X = (int)rbx + ox, Y = (int)rby + oy;
        d.fillCircle(X - 2, Y, BR + 1, rgb(255, 40, 60));      // red fringe
        d.fillCircle(X + 2, Y, BR + 1, rgb(40, 170, 255));     // cyan fringe
        d.fillCircle(X, Y, BR + 1, th_glow);
        d.fillCircle(X, Y, BR, COL_WHITE);
    }
    // particles
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        int f = s_spk[i].life * 256 / (s_spk[i].max > 0 ? s_spk[i].max : 1);
        d.fillCircle((int)s_spk[i].x + ox, (int)s_spk[i].y + oy, f > 130 ? 2 : 1, mix(rgb(4, 5, 12), s_spk[i].col, f));
    }
    // HUD: scores + level (crisp, on top)
    char b[8];
    snprintf(b, sizeof b, "%d", sl); txt_c(W / 2 - 36, 2, 3, COL_PL, b);
    snprintf(b, sizeof b, "%d", sr); txt_c(W / 2 + 36, 2, 3, COL_PR, b);
    char lv[12]; snprintf(lv, sizeof lv, "LIV %d", s_level); txt_c(W / 2, 4, 1, COL_GOLD, lv);
    if (s_phase == PH_COUNT) {
        snprintf(b, sizeof b, "%d", s_cnt);
        int rr = 26 - (s_phtimer % 600) / 30;
        d.drawCircle(W / 2 + ox, H / 2 + oy, rr, th_glow);
        d.drawCircle(W / 2 + ox, H / 2 + oy, rr + 6, mix(th_glow, rgb(4, 5, 12), 150));
        txt_c(W / 2 + ox, H / 2 - 14 + oy, 4, ((s_anim >> 1) & 1) ? COL_WHITE : COL_GOLD, b);
    }
    const char *who = s_mode == MODE_AI ? tx("VS CPU", "VS CPU") : s_mode == MODE_HOST ? "HOST" : tx("OSPITE", "GUEST");
    txt(4, COURT_TOP + 2, 1, COL_DIM, who);
    if (s_netlost) txt_c(W / 2, H - 12, 1, COL_RED, "segnale debole...");
}

// ============================ rendering: menus ===============================
static void felt(int ch) {
    d.fillRect(0, 0, W, ch, rgb(8, 10, 20));
    for (int y = 0; y < ch; y += 6) d.drawFastHLine(0, y, W, rgb(14, 18, 34));
}
static void mini_court(int cx, int cy) {
    d.drawRoundRect(cx - 20, cy - 13, 40, 26, 3, rgb(54, 64, 104));
    for (int y = cy - 11; y < cy + 12; y += 6) d.fillRect(cx - 1, y, 2, 3, rgb(54, 64, 104));
    d.fillRect(cx - 17, cy - 4, 2, 8, COL_PL);
    d.fillRect(cx + 15, cy - 1, 2, 8, COL_PR);
    d.fillCircle(cx + 3, cy, 2, COL_WHITE);
}
#define NMENU 6
static const char *menu_label(int i) {
    switch (i) {
        case 0: return tx("Gioca vs CPU", "Play vs CPU");
        case 1: return tx("Crea stanza", "Create room");
        case 2: return tx("Entra in stanza", "Join room");
        case 3: return tx("Impostazioni", "Settings");
        case 4: return tx("Record", "Scores");
        default: return tx("Come si gioca", "How to play");
    }
}
static void draw_menu(int ch) {
    felt(ch);
    mini_court(22, 18); mini_court(W - 22, 18);
    txt_c(W / 2 + 1, 5, 4, mix(COL_PL, rgb(8, 10, 20), 110), "PONG");
    txt_c(W / 2, 3, 4, ((s_anim >> 3) & 1) ? COL_WHITE : COL_PL, "PONG");
    // windowed list (selection + two neighbours each side), ALL at size 2 (bigger, legible), wrapped
    const int cy = 46, rowh = 17;
    for (int dlt = -2; dlt <= 2; dlt++) {
        int i = (s_msel + dlt + NMENU) % NMENU, y = cy + (dlt + 2) * rowh;
        if (dlt == 0) {
            d.fillRoundRect(12, y - 1, W - 24, rowh + 1, 5, rgb(28, 40, 70));
            d.drawRoundRect(12, y - 1, W - 24, rowh + 1, 5, ((s_anim >> 3) & 1) ? rgb(120, 150, 220) : rgb(70, 92, 150));
            d.fillRect(15, y + 1, 3, rowh - 3, COL_GOLD);
            txt_c(W / 2 + 4, y + 1, 2, COL_WHITE, menu_label(i));
        } else {
            int fade = (dlt == -2 || dlt == 2) ? 120 : 55;
            txt_c(W / 2, y + 1, 2, mix(COL_MUT, rgb(12, 16, 30), fade), menu_label(i));
        }
    }
}
static const char *opt_label(int i) {
    switch (i) {
        case 0: return tx("Difficolta", "Difficulty");
        case 1: return "Audio";
        default: return tx("Lingua", "Language");
    }
}
#define NOPT 3
static void opt_value(int i, char *out, int cap, uint16_t *col) {
    *col = COL_GOLD;
    switch (i) {
        case 0: snprintf(out, cap, "%s", s_diff == 0 ? tx("Facile", "Easy") : s_diff == 1 ? tx("Normale", "Normal") : tx("Difficile", "Hard")); break;
        case 1: snprintf(out, cap, "%s", g_audio ? "On" : "Off"); if (!g_audio) *col = COL_DIM; break;
        default: snprintf(out, cap, "%s", g_lang ? "English" : "Italiano"); break;
    }
}
static void draw_options(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, mix(COL_GOLD, rgb(8, 10, 20), 130), tx("IMPOSTAZIONI", "SETTINGS"));
    txt_c(W / 2, 6, 3, COL_GOLD, tx("IMPOSTAZIONI", "SETTINGS"));
    d.drawFastHLine(14, 30, W - 28, rgb(54, 64, 104));
    int y = 40;
    for (int i = 0; i < NOPT; i++) {
        bool sel = (i == s_optsel);
        char vb[16]; uint16_t vc; opt_value(i, vb, sizeof vb, &vc);
        if (sel) {
            d.fillRoundRect(12, y, W - 24, 22, 5, rgb(28, 40, 70));
            d.drawRoundRect(12, y, W - 24, 22, 5, ((s_anim >> 3) & 1) ? rgb(120, 150, 220) : rgb(70, 92, 150));
            d.fillRect(15, y + 3, 3, 16, COL_GOLD);
            txt(22, y + 4, 2, COL_WHITE, opt_label(i));
            char vv[20]; snprintf(vv, sizeof vv, "< %s >", vb); txt_r(W - 18, y + 7, 1, vc, vv);
            y += 26;
        } else {
            txt(22, y + 3, 2, COL_MUT, opt_label(i));
            txt_r(W - 18, y + 6, 1, mix(vc, rgb(8, 10, 20), 60), vb);
            y += 22;
        }
    }
    txt_c(W / 2, ch - 9, 1, COL_DIM, tx("SU/GIU scegli  SX/DX cambia  Esc esci", "UP/DN pick  L/R change  Esc back"));
}
static void draw_host(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, mix(COL_PL, rgb(8, 10, 20), 90), tx("Crea stanza", "Create room"));
    txt_c(W / 2, 6, 3, COL_PL, tx("Crea stanza", "Create room"));
    d.drawFastHLine(14, 30, W - 28, rgb(54, 64, 104));
    d.fillRoundRect(20, 36, W - 40, 30, 6, rgb(24, 30, 50));
    char nm[24]; snprintf(nm, sizeof nm, "%.11s", pnet_name()); txt_c(W / 2, 40, 2, COL_GOLD, nm);
    char cc[28]; snprintf(cc, sizeof cc, tx("canale %d", "channel %d"), pnet_channel()); txt_c(W / 2, 56, 1, COL_GREEN, cc);
    char dots[5] = "    "; for (int i = 0; i < (int)((s_anim >> 2) % 4); i++) dots[i] = '.';
    char l[48]; snprintf(l, sizeof l, "%s%s", tx("Attendo sfidante", "Waiting for rival"), dots);
    txt_c(W / 2, 80, 2, COL_WHITE, l);
    txt_c(W / 2, 104, 1, COL_DIM, tx("sull'altro: Entra in stanza, stesso canale", "on the other: Join room, same channel"));
}
static void draw_browse(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 2, mix(rgb(8, 10, 18), COL_PR, 90), tx("Entra in stanza", "Join room"));
    txt_c(W / 2, 6, 2, COL_PR, tx("Entra in stanza", "Join room"));
    char cc[28]; snprintf(cc, sizeof cc, tx("canale %d", "channel %d"), pnet_channel());
    txt_r(W - 8, 10, 1, COL_GREEN, cc);
    d.drawFastHLine(10, 26, W - 20, rgb(54, 64, 104));
    if (s_join_pending) {                                // joining: name the room + animate so the guest gets clear feedback
        char dots[5] = "    "; for (int i = 0; i < (int)((s_anim >> 2) % 4); i++) dots[i] = '.';
        txt_c(W / 2, 46, 1, COL_DIM, tx("Entro nella stanza", "Joining room"));
        char rn[16]; snprintf(rn, sizeof rn, "%.10s", s_joinname); txt_c(W / 2 + 1, 61, 2, rgb(8, 10, 18), rn); txt_c(W / 2, 60, 2, COL_GOLD, rn);
        char w[24]; snprintf(w, sizeof w, "%s%s", tx("contatto", "reaching"), dots); txt_c(W / 2, 86, 1, COL_DIM, w);
        return;
    }
    if (s_nhost == 0) {
        txt_c(W / 2, 50, 1, COL_WHITE, tx("Nessuna stanza trovata", "No rooms found"));
        txt_c(W / 2, 66, 1, COL_DIM, tx("Sull'altro Cardputer apri Pong", "On the other Cardputer open Pong"));
        txt_c(W / 2, 78, 1, COL_DIM, tx("e scegli Crea stanza.", "and choose Create room."));
        return;
    }
    txt(12, 30, 1, COL_DIM, tx("Scegli una stanza:", "Pick a room:"));
    int y = 42;
    for (int i = 0; i < s_nhost; i++) {
        bool f = (i == s_bsel);
        if (f) {
            d.fillRoundRect(10, y, W - 20, 22, 5, rgb(40, 30, 24)); d.drawRoundRect(10, y, W - 20, 22, 5, COL_PR);
            d.fillRect(12, y + 2, 3, 18, COL_PR);
            char nm[24]; snprintf(nm, sizeof nm, "%.11s", s_hosts[i].name); txt(20, y + 4, 2, COL_WHITE, nm);
            txt_r(W - 16, y + 8, 1, ((s_anim >> 2) & 1) ? COL_GREEN : COL_WHITE, "INVIO>");
            y += 26;
        } else {
            char nm[24]; snprintf(nm, sizeof nm, "%.18s", s_hosts[i].name); txt(20, y + 3, 1, COL_MUT, nm);
            y += 16;
        }
        if (y > ch - 14) break;
    }
}
static void draw_scores(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, mix(COL_GOLD, rgb(8, 10, 20), 130), tx("RECORD", "SCORES"));
    txt_c(W / 2, 6, 3, COL_GOLD, tx("RECORD", "SCORES"));
    char bl[28]; snprintf(bl, sizeof bl, "%s %u", tx("Livello max:", "Best level:"), g_best_level);
    txt_c(W / 2, 32, 1, COL_GREEN, bl);
    d.drawFastHLine(20, 44, W - 40, rgb(54, 64, 104));
    for (int i = 0; i < NTOP; i++) {
        int col = i / 4, row = i % 4, x = 16 + col * 116, y = 50 + row * 16;
        char r[5]; snprintf(r, sizeof r, "%d.", i + 1);
        txt(x, y, 1, i < 3 ? COL_GOLD : COL_DIM, r);
        char s[14]; snprintf(s, sizeof s, "%u", g_top[i]);
        txt(x + 18, y, 1, g_top[i] ? (i < 3 ? COL_WHITE : COL_MUT) : COL_DIM, g_top[i] ? s : "---");
    }
}
static void draw_over(int ch) {
    felt(ch);
    bool iwon = (s_mode == MODE_GUEST) ? (s_winner == 2) : (s_winner == 1);
    bool good = (s_mode == MODE_AI) ? (s_winner == 1) : iwon;
    const char *t = (s_mode == MODE_AI) ? (s_winner == 1 ? tx("HAI VINTO!", "YOU WIN!") : tx("VINCE LA CPU", "CPU WINS")) : (iwon ? tx("HAI VINTO!", "YOU WIN!") : tx("HAI PERSO", "YOU LOSE"));
    txt_c(W / 2 + 1, 15, 3, rgb(8, 10, 18), t); txt_c(W / 2, 14, 3, good ? COL_GOLD : COL_RED, t);
    char sc[16]; snprintf(sc, sizeof sc, "%d  -  %d", sl, sr);
    txt_c(W / 2, 46, 4, COL_WHITE, sc);
    char lv[20]; snprintf(lv, sizeof lv, "%s %d", tx("Livello", "Level"), s_level);
    txt_c(W / 2, 84, 1, COL_GREEN, lv);
    if (s_peerleft) txt_c(W / 2, 100, 1, COL_RED, tx("Avversario uscito", "Opponent left"));
    else if (s_mode == MODE_GUEST) txt_c(W / 2, 100, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_DIM, tx("in attesa dell'host...", "waiting for host..."));
    else txt_c(W / 2, 100, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_DIM, tx("INVIO rigioca   Esc menu", "ENTER replay   Esc menu"));
}
static void draw_help(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, mix(COL_PL, rgb(8, 10, 20), 110), tx("Come si gioca", "How to play"));
    txt_c(W / 2, 6, 3, COL_PL, tx("Come si gioca", "How to play"));
    d.drawFastHLine(14, 30, W - 28, rgb(54, 64, 104));
    txt(14, 36, 1, COL_WHITE, tx("SU/GIU o Z (su) / N (giu)", "UP/DN or Z (up) / N (down)"));
    txt(14, 50, 1, COL_WHITE, tx("Primo a 7 punti vince.", "First to 7 points wins."));
    txt(14, 66, 1, COL_GOLD,  tx("Power-up sul campo:", "Power-ups on court:"));
    txt(14, 78, 1, COL_GREEN, tx("+ ingrandisci   - rimpicc. avv.", "+ grow self   - shrink rival"));
    txt(14, 90, 1, rgb(120, 180, 255), tx("S lento  F veloce  * punto", "S slow  F fast  * point"));
    txt(14, 104, 1, COL_DIM, tx("Livelli: arena e colori cambiano.", "Levels: arena and colours change."));
}

// ============================ input / hint ===================================
static void set_hint(void) {
    switch (s_screen) {
        case ST_MENU:   nucleo_app_set_hint(tx("SU/GIU  INVIO scegli  Esc esci", "UP/DN  ENTER pick  Esc quit")); break;
        case ST_OPT:    nucleo_app_set_hint(tx("SU/GIU  SX/DX cambia  Esc esci", "UP/DN  L/R change  Esc back")); break;
        case ST_HOST:   nucleo_app_set_hint(tx("In attesa...  Esc annulla", "Waiting...  Esc cancel")); break;
        case ST_BROWSE: nucleo_app_set_hint(tx("SU/GIU  INVIO entra  Esc indietro", "UP/DN  ENTER join  Esc back")); break;
        case ST_OVER:   nucleo_app_set_hint(tx("INVIO rigioca  Esc menu", "ENTER replay  Esc menu")); break;
        case ST_SCORES: case ST_HELP: nucleo_app_set_hint(tx("Esc indietro", "Esc back")); break;
        default: break;
    }
}
static void go(int s) {
    s_screen = s;
    nucleo_app_set_fullscreen(s == ST_PLAY);
    set_hint();
    nucleo_app_request_draw();
}
static void leave_to_menu(void) {
    send_bye();
    s_haspeer = false; s_join_pending = false;
    s_msel = 0; go(ST_MENU);
}
static void record_match(void) {                   // store a vs-CPU result to the leaderboard
    if (s_mode != MODE_AI) return;
    if ((unsigned)s_level > g_best_level) g_best_level = s_level;
    unsigned score = (unsigned)s_level * 100 + (unsigned)sl * 10 + (s_winner == 1 ? 100 : 0);
    leaderboard_add(score);
    cfg_write();
}

static void opt_change(int i, int dir) {
    switch (i) {
        case 0: s_diff = (s_diff + (dir < 0 ? 2 : 1)) % 3; break;
        case 1: g_audio ^= 1; break;
        default: g_lang ^= 1; set_hint(); break;     // language flips the whole UI -> refresh the hint bar too
    }
    cfg_write(); sfx(2); nucleo_app_request_draw();
}
static void on_key(int k, char ch) {
    (void)ch;
    switch (s_screen) {
        case ST_MENU:
            if (k == NK_UP)        { s_msel = (s_msel + NMENU - 1) % NMENU; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % NMENU; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) {
                if (s_msel == 0)      { sfx(2); new_match(MODE_AI); go(ST_PLAY); }
                else if (s_msel == 1) { sfx(2); s_nhost = 0; s_haspeer = false; go(ST_HOST); send_hello(HS_HOSTING); }
                else if (s_msel == 2) { sfx(2); s_nhost = 0; s_bsel = 0; go(ST_BROWSE); }
                else if (s_msel == 3) { s_optsel = 0; sfx(2); go(ST_OPT); }
                else if (s_msel == 4) { sfx(2); go(ST_SCORES); }
                else                  { sfx(2); go(ST_HELP); }
            }
            return;
        case ST_OPT:
            if (k == NK_UP)        { s_optsel = (s_optsel + NOPT - 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_optsel = (s_optsel + 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_RIGHT || k == NK_ENTER) opt_change(s_optsel, +1);
            return;
        case ST_BROWSE:
            if (k == NK_UP)        { if (s_bsel > 0) s_bsel--; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { if (s_bsel < s_nhost - 1) s_bsel++; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER && s_nhost > 0 && !s_join_pending) {
                memcpy(s_peer, s_hosts[s_bsel].mac, 6); s_haspeer = true;
                snprintf(s_joinname, sizeof s_joinname, "%.21s", s_hosts[s_bsel].name);   // remember the room for named feedback
                pg_join_t j = { PG_M0, PG_M1, PG_VER, PG_JOIN, { 0 } };
                snprintf(j.name, sizeof j.name, "%s", pnet_name());
                pnet_send(s_peer, &j, sizeof j);
                s_join_pending = true; s_join_t0 = now_ms(); sfx(2);
                nucleo_app_request_draw();
            }
            return;
        case ST_PLAY:
            // paddle is polled continuously in poll() via nucleo_kbd_char_down (smooth hold): arrows + Z/N
            (void)ch;
            return;
        case ST_OVER:
            if (k == NK_ENTER) {
                if (s_mode == MODE_AI) { sfx(2); new_match(MODE_AI); go(ST_PLAY); }
                else if (s_mode == MODE_HOST && !s_peerleft) { sfx(2); new_match(MODE_HOST); go(ST_PLAY); }
                else leave_to_menu();
            }
            return;
        default: return;
    }
}
static bool on_back(int key) {
    if (key == NK_LEFT) {
        if (s_screen == ST_OPT) opt_change(s_optsel, -1);
        return true;                               // never let LEFT close the app
    }
    sfx(3);
    switch (s_screen) {
        case ST_MENU: return false;                // close app
        case ST_PLAY: case ST_OVER: leave_to_menu(); return true;
        default: s_haspeer = false; s_join_pending = false; s_msel = 0; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void on_draw(void) {
    int ch = nucleo_app_content_height();
    switch (s_screen) {
        case ST_MENU:   draw_menu(ch); break;
        case ST_HOST:   draw_host(ch); break;
        case ST_BROWSE: draw_browse(ch); break;
        case ST_PLAY:   draw_play(); break;
        case ST_OVER:   draw_over(ch); break;
        case ST_SCORES: draw_scores(ch); break;
        case ST_HELP:   draw_help(ch); break;
        case ST_OPT:    draw_options(ch); break;
        default: break;
    }
}
static bool poll(void) {
    s_now = now_ms();
    int dt = (int)(s_now - s_last); if (dt < 0) dt = 0; if (dt > 60) dt = 60;
    s_last = s_now;

    pnet_pkt_t p;
    while (pnet_recv(&p)) net_handle(&p);

    bool live = false;
    if (s_shake > 0.4f) s_shake *= 0.86f; else s_shake = 0;
    if (s_hitl > 0) s_hitl -= dt;
    if (s_hitr > 0) s_hitr -= dt;
    if (s_flash > 0) s_flash -= dt;
    sparks_step(dt);
    waves_step(dt);

    if (s_screen == ST_HOST) {
        if (s_now - s_last_hello > 400) { send_hello(HS_HOSTING); s_last_hello = s_now; }
        live = true;
    } else if (s_screen == ST_BROWSE) {
        hosts_prune();
        if (s_join_pending && s_now - s_join_t0 > 3000) { s_join_pending = false; s_haspeer = false; }
        live = true;
    } else if (s_screen == ST_PLAY) {
        move_local_paddle((float)dt);
        if (s_mode == MODE_GUEST) {
            if (s_now - s_last_tx > 25) send_input();
            s_netlost = (s_now - s_last_rx > 1200);
            if (s_peerleft || s_now - s_last_rx > 3500) { s_phase = PH_OVER; go(ST_OVER); return true; }
            if (s_phase == PH_OVER) { go(ST_OVER); return true; }   // host won -> follow to the over screen
        } else {
            heights_step(dt);
            if (s_mode == MODE_AI) ai_step((float)dt);
            pu_spawn_step(dt);
            phys_step((float)dt);
            if (s_mode == MODE_HOST) {
                if (s_now - s_last_tx > 25) send_state();
                s_netlost = (s_now - s_last_rx > 1200);
                if (s_peerleft || s_now - s_last_rx > 3500) { s_phase = PH_OVER; go(ST_OVER); return true; }
            }
            if (s_phase == PH_OVER) { record_match(); go(ST_OVER); return true; }
        }
        // ball trail history
        if (s_phase == PH_PLAY) { s_tx[s_ti] = bx; s_ty[s_ti] = by; s_ti = (s_ti + 1) % NTRAIL; }
        live = true;
    } else if (s_screen == ST_OVER) {
        if (s_mode == MODE_HOST && s_now - s_last_tx > 60) send_state();
        if (s_mode == MODE_GUEST && s_phase != PH_OVER) { go(ST_PLAY); return true; }
        live = true;
    }

    if (!live) return false;
    if (s_now - s_frame < 16) return false;        // ~60 Hz cap
    s_frame = s_now; s_anim++;
    return true;
}
static void on_enter(void) {
    ensure_dirs();
    cfg_read();
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(80);
    presynth();
    s_screen = ST_MENU; s_msel = 0; s_anim = 0;
    s_haspeer = s_join_pending = s_netlost = s_peerleft = false;
    s_nhost = s_bsel = 0; s_txseq = s_rxseq = 0;
    s_now = s_last = s_frame = now_ms();
    s_last_hello = 0;
    if (!pnet_start()) nucleo_app_set_hint(tx("ESP-NOW non avviato  Esc", "ESP-NOW not started  Esc"));
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_fullscreen(false);
    set_hint();
    nucleo_app_request_draw();
}
static void on_exit(void) { send_bye(); pnet_stop(); nucleo_audio_stop(); cfg_write(); }

extern "C" void nucleo_register_pong(void) {
    static const nucleo_app_def_t app = {
        "pong", "Pong", "Games", "Pong arcade: 1v1 in rete tra due Cardputer, o vs CPU",
        'P', C_BLUE, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP                                 // ~70KB freed, Wi-Fi STA (ESP-NOW) stays up
    };
    nucleo_app_register(&app);
}
