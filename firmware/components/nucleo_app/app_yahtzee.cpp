// Yahtzee — Games. Classic 5-dice game, turn-based (hot-seat 1-4 + optional CPU), with tumbling 3D d6
// dice (flat-shaded, 3/4 view, the same renderer as the Dadi app). Designed for the tiny 240x135 screen:
// the ROLL view makes the dice the hero (felt + tray + 3D tumble), the SCORE view is a readable scrolling
// scorecard (ghost previews of what each box would score, the best one in green) with the contributing
// dice highlighted. Honest randomness from the ESP32 hardware TRNG.
//
// FLOW: title (play / players / how to play) -> per turn roll up to 3 times, hold dice between rolls,
// then score one of the 13 boxes (each used once) -> after 13 rounds the results -> ENTER rematch.
// Esc in a game pauses it (a second Esc leaves to the title). Rules: upper bonus 35 at 63+, Yahtzee
// bonus +100 for every extra Yahtzee while the Yahtzee box holds 50, and the official forced Joker
// (a Yahtzee with its box filled must take its upper box if open, else any lower box at full value).
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include "nucleo_fx3d.h"
#include "nucleo_audio.h"
#include "nucleo_exclusive.h"  // NX_NET_APP: dedicate RAM + free the shared I2S line so the SFX reliably play
#include "game_text.h"
#include "game_ui.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "app_gfx.h"

#define TWO_PI 6.2831853f
#define DIRR "/sd/data/yahtzee"
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// ---- d6: faces (outward normal n, in-plane axes u/v), pip layouts, settle poses ----
struct FaceFrame { float n[3], u[3], v[3]; };
static const FaceFrame FACE[6] = {
    { { 0, 0, 1}, {1,0,0}, {0,1,0} }, { { 1, 0, 0}, {0,0,-1},{0,1,0} }, { { 0, 1, 0}, {1,0,0}, {0,0,-1} },
    { { 0,-1, 0}, {1,0,0}, {0,0,1} }, { {-1, 0, 0}, {0,0,1}, {0,1,0} }, { { 0, 0,-1}, {1,0,0}, {0,-1,0} },
};
static const float PIPS[7][6][2] = {
    {{0,0}}, {{0,0}}, {{-0.5f,0.5f},{0.5f,-0.5f}}, {{-0.55f,0.55f},{0,0},{0.55f,-0.55f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0,0},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.62f},{-0.5f,0},{-0.5f,0.62f},{0.5f,-0.62f},{0.5f,0},{0.5f,0.62f}},
};
// yaw / pitch / bank that turn value v's face to the camera in a 3/4 view (app_dice.cpp settle_pose(), tilt 1)
static const float POSE[6][3] = { { -2.7416f, 0.3600f, 0.0f }, { 1.9708f, 0.3600f, 0.0f }, { 0.8024f, -1.0392f, 0.8761f },
                                  { 2.3392f, 1.0392f, -2.2655f }, { -1.1708f, 0.3600f, 0.0f }, { 0.4000f, 0.3600f, 0.0f } };

// ---- scorecard categories ----
enum { N_CAT = 13 };
#define SC_VIS 4               // scorecard rows visible at once
static const char *cat_name(int i)
{
    switch (i) {
        case 0: return GT("Uno", "Ones");      case 1: return GT("Due", "Twos");       case 2: return GT("Tre", "Threes");
        case 3: return GT("Quattro", "Fours"); case 4: return GT("Cinque", "Fives");   case 5: return GT("Sei", "Sixes");
        case 6: return GT("Tris", "Three of a kind");  case 7: return GT("Poker", "Four of a kind");
        case 8: return GT("Full", "Full house");       case 9: return GT("Scala piccola", "Small straight");
        case 10: return GT("Scala grande", "Large straight");
        case 11: return "Yahtzee";
        default: return "Chance";
    }
}

#define MAXP 4
struct Player { int score[N_CAT]; bool used[N_CAT]; int ybonus; bool cpu; };
static Player *s_pl;                      // MAXP entries, APP_RAM (reset by start_game)
static int    s_np, s_cur, s_turn;        // player count, current player, turn 0..12

struct Die { float yaw, pitch, bank, y0, p0, b0, ye, pe, be, wob; int value; bool held, rolling; };
static Die *s_d;                          // 5 dice, APP_RAM (posed by reset_dice_idle)

enum { PH_SETUP, PH_HELP, PH_ROLL, PH_SCORE, PH_OVER };
static int  s_phase;
static bool s_paused;           // pause card over ROLL / SCORE
static int  s_rolls;            // rolls left this turn
static bool s_rolled;           // rolled at least once this turn
static int  s_diecur;           // ROLL view: selected die
static int   s_sel;             // SCORE view: selected category 0..12
static float s_scroll;          // SCORE view: eased top-row offset for smooth scrolling
static gui::Menu s_menu;        // title menu: play / players / how to play
static int  s_humans = 1, s_cpus = 1;
// player line-ups the title cycles with LEFT/RIGHT: humans << 4 | cpus
static const uint8_t LINEUP[] = { 0x11, 0x12, 0x13, 0x10, 0x20, 0x21, 0x22, 0x30, 0x31, 0x40 };

static bool    s_anim;
static bool    s_bgdirty = true;   // ROLL: repaint the static felt/UI only when it changed (no per-frame full clear)
static int64_t s_roll_us, s_dur_us, s_last_us, s_cool_us;   // s_cool_us: pause before the next roll is accepted
static bool    s_armed;                                     // shake-to-roll arming (see sim): never auto-fires
static int64_t s_toast_us; static char s_toast[32], s_toast2[32]; static uint16_t s_toast_col;
static int64_t s_cele_us;        // celebration / confetti overlay until (us)
static int64_t s_cele_start;     // celebration start (drives the 0..1 progress)
static int64_t s_cpu_us; static int s_cpu_step;
static bool    s_suggest[5];     // auto-hold suggestion mask (cleared on any keypress)
static int64_t s_suggest_us;     // suggestion visible until this time
static int64_t s_contrib_flash_us; // mini-dice strip flash duration on category change

// Particle burst: YAHTZEE fireworks (radial) + game-over confetti (rain). No per-frame alloc.
struct Part { float x, y, vx, vy; uint16_t col; uint8_t life, lmax; };
static Part *s_part;                      // 30 entries, APP_RAM
static int  s_npart;

static const uint16_t PCOL[MAXP] = { C_GREEN, C_YELLOW, C_BLUE, C_PINK };
static const uint16_t TRAY = RGB(0, 36, 0), K_BLK = 0;

static int  randn(int n) { return (int)(esp_random() % (uint32_t)n); }
static int  clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void ptag(int i, char *b) { snprintf(b, 8, s_pl[i].cpu ? "CPU%d" : "P%d", i + 1); }

// Fill the particle pool. rain=false: a radial burst from the screen centre (YAHTZEE fireworks).
// rain=true: confetti falling from the top (game over).
static void spawn_burst(bool rain)
{
    static const uint16_t COLS[6] = { RGB(255, 219, 0), RGB(255, 73, 85), RGB(73, 219, 85), RGB(109, 146, 255), 0xFFFF, RGB(219, 109, 255) };
    s_npart = 30;
    for (int i = 0; i < s_npart; i++) {
        uint32_t r = esp_random();
        Part &q = s_part[i];
        if (rain) {
            q.x = (float)(r % (uint32_t)W); q.y = -(float)((r >> 9) % 40);
            q.vx = (float)((r >> 4) % 40) - 20.0f; q.vy = 40.0f + (float)((r >> 12) % 70);
        } else {
            float a = (float)(r % 628) / 100.0f, sp = 60.0f + (float)((r >> 9) % 150);
            q.x = (float)(W / 2); q.y = (float)(H / 2 - 6);
            q.vx = cosf(a) * sp; q.vy = sinf(a) * sp - 55.0f;
        }
        q.col = COLS[(r >> 20) % 6];
        q.lmax = q.life = (uint8_t)(60 + (r >> 17) % 60);
    }
}

// ---- sound: the deployed pack (tools/sfx-gen/games/yahtzee.py) — never synthesized on the device; a cue
// whose WAV is missing plays a short tone instead ----
static const char *sfx_name(int id)
{
    static const char *const N[] = { "x", "nav", "holdon", "holdf", "roll", "settle", "deny", "score", "scoreB",
                                     "bonus", "yahtzee", "over", "turn", "arm", "holdall", "suggest" };
    return id > 0 && id < (int)(sizeof N / sizeof *N) ? N[id] : "x";
}
#define NSFX 15
static const uint16_t SFX_HZ[NSFX + 1] = { 0, 660, 1175, 440, 1500, 140, 175, 1175, 1318, 1760, 1760, 262, 880, 1280, 1318, 2093 };
static bool sfx_important(int id) { return id == 5 || (id >= 7 && id <= 12) || id == 14; }   // settle + scores/fanfares/over/turn/holdall
static void sfx(int id)
{
    if (id <= 0 || id > NSFX) return;
    bool imp = sfx_important(id);
    if (!imp && nucleo_audio_is_playing()) return;
    char p[48]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id));
    struct stat st;
    if (imp) nucleo_audio_stop();
    if (stat(p, &st) != 0 || st.st_size <= 44 || nucleo_audio_play(p) != ESP_OK) nucleo_audio_tone(SFX_HZ[id], 45, 55);
}
enum { SFX_NAV = 1, SFX_HOLDON, SFX_HOLDOFF, SFX_ROLL, SFX_SETTLE, SFX_DENY, SFX_SCORE, SFX_SCOREB, SFX_BONUS,
       SFX_YAHTZEE, SFX_OVER, SFX_TURN, SFX_ARM, SFX_HOLDALL, SFX_SUGGEST };

// ---------------------------------------------------------------- scoring
static void counts(int *c) { for (int v = 0; v < 7; v++) c[v] = 0; for (int i = 0; i < 5; i++) c[s_d[i].value]++; }
static bool is_yahtzee(void) { for (int i = 1; i < 5; i++) if (s_d[i].value != s_d[0].value) return false; return true; }
// Joker: a Yahtzee rolled when the Yahtzee box is already filled (50 or 0)
static bool joker(const Player *p) { return p->used[11] && is_yahtzee(); }
static int preview(const Player *p, int idx)
{
    int c[7], sum = 0; counts(c);
    for (int i = 0; i < 5; i++) sum += s_d[i].value;
    int mx = 0; for (int v = 1; v <= 6; v++) if (c[v] > mx) mx = c[v];
    if (idx <= 5) return (idx + 1) * c[idx + 1];
    bool jk = joker(p);
    int run = 0, best = 0; for (int v = 1; v <= 6; v++) { run = c[v] ? run + 1 : 0; if (run > best) best = run; }
    switch (idx) {
        case 6:  return mx >= 3 ? sum : 0;                                   // three of a kind
        case 7:  return mx >= 4 ? sum : 0;                                   // four of a kind
        case 8:  { bool h3 = false, h2 = false; for (int v = 1; v <= 6; v++) { h3 |= c[v] == 3; h2 |= c[v] == 2; }
                   return (h3 && h2) || jk ? 25 : 0; }                       // full house (3+2)
        case 9:  return best >= 4 || jk ? 30 : 0;                            // small straight
        case 10: return best >= 5 || jk ? 40 : 0;                            // large straight
        case 11: return mx == 5 ? 50 : 0;                                    // Yahtzee
        default: return sum;                                                 // chance
    }
}
// May box idx take this roll? (used boxes never; under the Joker the matching upper box first, then a lower box)
static bool allowed(const Player *p, int idx)
{
    if (p->used[idx]) return false;
    if (!joker(p)) return true;
    int up = s_d[0].value - 1;
    if (!p->used[up]) return idx == up;
    for (int i = 6; i < N_CAT; i++) if (!p->used[i]) return idx >= 6;
    return true;                                                             // only upper boxes left: zero one
}
static int upper_sub(const Player *p) { int s = 0; for (int i = 0; i < 6; i++) s += p->score[i]; return s; }
static int total(const Player *p)
{
    int s = p->ybonus; for (int i = 0; i < N_CAT; i++) s += p->score[i];
    if (upper_sub(p) >= 63) s += 35;
    return s;
}
// idx of the highest-scoring box this roll may take (>0), or -1 if every allowed box would score 0
static int best_open_idx(const Player *p)
{
    int b = -1, bv = 0;
    for (int i = 0; i < N_CAT; i++) if (allowed(p, i)) { int v = preview(p, i); if (v > bv) { bv = v; b = i; } }
    return b;
}

// Which dice make category idx's score (false for all when it would score 0: nothing to highlight).
static void compute_contribution(int idx, bool out[5])
{
    int c[7]; counts(c);
    for (int i = 0; i < 5; i++) out[i] = false;
    if (idx <= 5) { for (int i = 0; i < 5; i++) out[i] = s_d[i].value == idx + 1; return; }
    if (idx == 6 || idx == 7 || idx == 11) {                            // of-a-kind: the modal value
        int mv = 1; for (int v = 2; v <= 6; v++) if (c[v] > c[mv]) mv = v;
        for (int i = 0; i < 5; i++) out[i] = s_d[i].value == mv;
    } else if (idx == 9 || idx == 10) {                                 // straights: one die per value of the run
        int need = idx == 9 ? 4 : 5, run = 0, end = 0;
        for (int v = 1; v <= 6; v++) { run = c[v] ? run + 1 : 0; if (run >= need) end = v; }
        if (!end) return;
        for (int v = end - need + 1; v <= end; v++) for (int i = 0; i < 5; i++) if (s_d[i].value == v) { out[i] = true; break; }
    } else {                                                            // full house: every die; chance: the high ones
        for (int i = 0; i < 5; i++) out[i] = idx == 8 || s_d[i].value >= 4;
    }
}

// ---------------------------------------------------------------- dice setup / rolling
static void reset_dice_idle(void)
{
    for (int i = 0; i < 5; i++) {
        Die &dd = s_d[i];
        dd.value = 1 + randn(6); dd.held = false; dd.rolling = false;
        dd.yaw = POSE[dd.value - 1][0]; dd.pitch = POSE[dd.value - 1][1]; dd.bank = POSE[dd.value - 1][2];
    }
}
static void do_roll(int64_t now)
{
    if (s_rolls <= 0 || s_anim) return;
    s_cele_us = 0; s_armed = false; s_suggest_us = 0;                 // consume arming + clear suggestion
    if (!s_rolled) for (int i = 0; i < 5; i++) s_d[i].held = false;   // first roll: nothing held
    s_rolled = true; s_rolls--;
    s_dur_us = 820000; s_roll_us = now; s_anim = true; s_bgdirty = true;
    sfx(SFX_ROLL);
    for (int i = 0; i < 5; i++) {
        Die &dd = s_d[i];
        if (dd.held) { dd.rolling = false; continue; }
        dd.rolling = true;
        dd.y0 = dd.yaw; dd.p0 = dd.pitch; dd.b0 = dd.bank;
        dd.value = 1 + randn(6);
        int spins = 2 + randn(2);                                    // few turns -> the eye tracks it (no strobe)
        dd.ye = POSE[dd.value - 1][0] + TWO_PI * spins;
        dd.pe = POSE[dd.value - 1][1] + TWO_PI * (spins - 1);
        dd.be = POSE[dd.value - 1][2];
        dd.wob = 0.30f;
    }
}

// ---------------------------------------------------------------- turn flow
static void start_turn(int64_t now)
{
    s_rolls = 3; s_rolled = false; s_diecur = 0; s_phase = PH_ROLL;
    s_sel = 0; s_scroll = 0; s_cool_us = 0; s_armed = false;
    s_suggest_us = 0; s_contrib_flash_us = 0; memset(s_suggest, 0, sizeof(s_suggest));
    s_bgdirty = true;
    reset_dice_idle();
    if (s_pl[s_cur].cpu) { s_cpu_step = 0; s_cpu_us = now + 700000; }
    else                 sfx(SFX_TURN);                              // soft chime: it's your turn
}
static void advance_turn(int64_t now)
{
    s_cur = (s_cur + 1) % s_np;
    if (s_cur == 0) s_turn++;
    if (s_turn >= N_CAT) {
        s_phase = PH_OVER; s_paused = false; nucleo_app_set_fullscreen(false);
        nucleo_app_set_hint(GT("INVIO rivincita  Esc menu", "ENTER rematch  Esc menu"));
        s_cele_start = now; s_cele_us = now + 3200000; spawn_burst(true); sfx(SFX_OVER);
        return;
    }
    start_turn(now);
}
static void lock_category(int idx, int64_t now)
{
    Player &p = s_pl[s_cur];
    if (!allowed(&p, idx)) { sfx(SFX_DENY); return; }
    int val = preview(&p, idx);
    bool bonus = is_yahtzee() && p.used[11] && p.score[11] == 50;    // extra Yahtzee: +100
    if (bonus) p.ybonus += 100;
    p.score[idx] = val; p.used[idx] = true;
    char tag[8]; ptag(s_cur, tag);
    snprintf(s_toast, sizeof(s_toast), "%s", cat_name(idx));
    if (bonus)        snprintf(s_toast2, sizeof(s_toast2), "%s  +%d  +100 YAHTZEE!", tag, val);
    else if (val > 0) snprintf(s_toast2, sizeof(s_toast2), GT("%s  +%d punti", "%s  +%d points"), tag, val);
    else              snprintf(s_toast2, sizeof(s_toast2), GT("%s  casella sacrificata", "%s  box scratched"), tag);
    s_toast_col = bonus ? C_YELLOW : (val > 0 ? PCOL[s_cur] : MUTED);
    s_toast_us = now + 1300000;
    sfx(val <= 0 ? SFX_DENY : bonus ? SFX_BONUS : val >= 30 ? SFX_SCOREB : SFX_SCORE);
    advance_turn(now);
}
// open the scorecard with the cursor pre-placed on the best move (big usability win)
static void goto_score(void)
{
    const Player *p = &s_pl[s_cur];
    int idx = best_open_idx(p);
    for (int i = 0; idx < 0 && i < N_CAT; i++) if (allowed(p, i)) idx = i;
    s_sel = idx < 0 ? 0 : idx;
    s_scroll = (float)clampi(s_sel - 2, 0, N_CAT - SC_VIS);   // snap so the selection is visible the instant we open
    s_contrib_flash_us = esp_timer_get_time() + 350000;      // flash the mini dice strip on first open
    s_phase = PH_SCORE;
}

// ---------------------------------------------------------------- CPU
// Which dice to keep: a made Yahtzee / large straight / full house it can still score, else a straight
// chase (an open straight box and a 4-run, or a 3-run with nothing better), else the best of-a-kind value.
static int cpu_hold_mask(const Player &p)
{
    int c[7]; counts(c);
    int mx = 0, run = 0, len = 0, end = 0;
    for (int v = 1; v <= 6; v++) { if (c[v] > mx) mx = c[v]; run = c[v] ? run + 1 : 0; if (run > len) { len = run; end = v; } }
    if (mx == 5 || (!p.used[10] && len >= 5) || (!p.used[8] && preview(&p, 8) == 25 && mx == 3 && (p.used[2] || c[3] < 3))) return 31;
    bool straights = !p.used[9] || !p.used[10];
    if (straights && (len >= 4 || (len == 3 && mx < 3 && s_rolls > 0)) && !(len == 4 && p.used[10] && !p.used[9])) {
        int m = 0;
        for (int v = end - len + 1; v <= end; v++) for (int i = 0; i < 5; i++) if (s_d[i].value == v && !(m & (1 << i))) { m |= 1 << i; break; }
        return m;
    }
    if (len >= 4 && !p.used[9] && p.used[10]) return 31;            // small straight made, large one gone
    int bv = 0, bs = -1;
    for (int v = 1; v <= 6; v++) {
        int sc = c[v] * 10 + v + (!p.used[v - 1] ? 6 : 0) + (c[v] >= 2 && (!p.used[6] || !p.used[7] || !p.used[11]) ? 4 : 0);
        if (c[v] && sc > bs) { bs = sc; bv = v; }
    }
    int m = 0; for (int i = 0; i < 5; i++) if (s_d[i].value == bv) m |= 1 << i;
    return m;
}
// Which box to score: the preview, weighed — upper boxes by how far they keep the 63 pace, Chance kept for
// a bad roll, a scratch costs what that box is still worth.
static int cpu_best(void)
{
    const Player &p = s_pl[s_cur];
    int c[7]; counts(c);
    int best = -1, bh = -10000;
    for (int i = 0; i < N_CAT; i++) {
        if (!allowed(&p, i)) continue;
        int v = preview(&p, i), h;
        if (i < 6)       h = v + (c[i + 1] - 3) * (i + 1) * 2 + (c[i + 1] >= 3 ? 12 : 0);
        else if (i == 12) h = v - 22 + s_turn;
        else if (i == 6 || i == 7) h = v - (v && v < 18 ? 8 : 0);
        else             h = v ? v + 10 : 0;
        if (v == 0) {                                                // scratch: cheapest box first
            static const int8_t COST[N_CAT] = { 2, 4, 7, 9, 12, 14, 16, 12, 20, 22, 26, 10, 25 };
            h = -COST[i] - (i == 11 ? 0 : 0);
        }
        if (h > bh) { bh = h; best = i; }
    }
    return best;
}
static void cpu_step(int64_t now)
{
    if (s_cpu_step > 0) {
        int m = cpu_hold_mask(s_pl[s_cur]);
        if (m == 31 || s_rolls == 0) { lock_category(cpu_best(), now); return; }
        for (int i = 0; i < 5; i++) s_d[i].held = (m >> i) & 1;
    }
    do_roll(now); s_cpu_step++; s_cpu_us = now + s_dur_us + 850000;
}

// ---------------------------------------------------------------- simulation
static void sim(float dt)
{
    int64_t now = esp_timer_get_time();

    // particle burst (YAHTZEE fireworks / game-over confetti) — frame-stepped, heap-free
    if (s_npart) {
        if (now >= s_cele_us) s_npart = 0;
        else for (int i = 0; i < s_npart; i++) {
            Part &q = s_part[i];
            q.x += q.vx * dt; q.y += q.vy * dt; q.vy += 240.0f * dt;   // gravity
            if (q.life > 0) q.life--;
        }
    }
    if (s_paused) return;
    // SCORE: ease the smooth-scroll toward the offset that keeps the selection on screen
    if (s_phase == PH_SCORE) {
        float tgt = (float)clampi(s_sel - 2, 0, N_CAT - SC_VIS);
        float k = dt * 12.0f; if (k > 1.0f) k = 1.0f;
        s_scroll += (tgt - s_scroll) * k;
    }

    if (s_anim) {
        float t = (float)(now - s_roll_us) / (float)s_dur_us; if (t > 1) t = 1;
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t), wob = (1.0f - t) * sinf(t * 9.0f);
        for (int i = 0; i < 5; i++) {
            Die &dd = s_d[i];
            if (!dd.rolling) continue;
            dd.yaw = dd.y0 + (dd.ye - dd.y0) * e;
            dd.pitch = dd.p0 + (dd.pe - dd.p0) * e;
            dd.bank = dd.b0 + (dd.be - dd.b0) * e + dd.wob * wob;
        }
        if (t >= 1.0f) {
            s_anim = false; s_bgdirty = true; nucleo_app_request_draw();   // settle: repaint UI (new status) once
            for (int i = 0; i < 5; i++) if (s_d[i].rolling) { Die &dd = s_d[i]; dd.rolling = false; dd.yaw = fmodf(dd.ye, TWO_PI); dd.pitch = fmodf(dd.pe, TWO_PI); dd.bank = dd.be; }
            if (is_yahtzee()) { s_cele_start = now; s_cele_us = now + 2600000; spawn_burst(false); sfx(SFX_YAHTZEE); }
            else {
                sfx(SFX_SETTLE);
                // Auto-hold suggestion: show for 1.5 s which dice to keep for the best open category.
                if (!s_pl[s_cur].cpu && s_rolls > 0) {
                    int bidx = best_open_idx(&s_pl[s_cur]);
                    memset(s_suggest, 0, sizeof(s_suggest));
                    if (bidx >= 0) {
                        compute_contribution(bidx, s_suggest);
                        bool any = false; for (int i = 0; i < 5; i++) any |= s_suggest[i];
                        if (any) { s_suggest_us = now + 1500000; sfx(SFX_SUGGEST); }
                    }
                }
            }
            s_cool_us = now + 600000;          // PAUSE: no roll accepted for a beat after the dice settle
        }
        return;
    }
    // Shake-to-roll (IMU) — NEVER auto-fires. It must be ARMED first: after any roll the gesture is
    // disarmed and only re-arms once the device is held still/quiet again; then a single DELIBERATE,
    // strong shake rolls once. So holding it in hand or fidgeting can never start a roll on its own.
    if (s_phase == PH_ROLL && !s_pl[s_cur].cpu && s_rolls > 0 && nucleo_imu_present()) {
        nucleo_imu_sample();
        nk_motion_t m = nucleo_imu_motion();
        if (!s_armed) { if (m == NK_MOTION_STILL || m == NK_MOTION_HAND) { s_armed = true; sfx(SFX_ARM); s_bgdirty = true; nucleo_app_request_draw(); } }
        else if (nucleo_imu_energy() > 1.30f && now > s_cool_us && now > s_toast_us && now > s_cele_us) do_roll(now);
    }
    // CPU autopilot (waits for the toast and the Yahtzee show)
    if (s_phase == PH_ROLL && s_pl[s_cur].cpu && now > s_toast_us && now > s_cele_us && now >= s_cpu_us) cpu_step(now);
}

// ---------------------------------------------------------------- die rendering
struct Rot { float cy, sy, cp, sp, cb, sb; };
static float rot_pt(const Rot &r, float x, float y, float z, float *ox, float *oy)
{
    float z2; fx3d::rot3(x, y, z, r.cy, r.sy, r.cp, r.sp, r.cb, r.sb, ox, oy, &z2);
    return z2;
}
// RGB332-exact face shades (white plastic, cooling into shadow), ink / red pips, faint side pips
static const uint16_t SHADE[4] = { RGB(255, 255, 255), RGB(219, 219, 255), RGB(182, 182, 170), RGB(109, 109, 170) };
static const uint16_t PIP_FAINT = RGB(146, 146, 170);
// A die the crisp way: back faces culled, each visible face one flat shade (key light just above the
// camera), over a dark rim of the same faces 1.6 px bigger; pips on the result face in ink, the side
// faces' faint. cy is the GROUND centre: `hop` lifts the die while its shadow stays on the felt.
static void draw_die(int i, int cx, int cy, float sc, bool show_pips = true, int hop = 0)
{
    const Die &dd = s_d[i];
    Rot r = { cosf(dd.yaw), sinf(dd.yaw), cosf(dd.pitch), sinf(dd.pitch), cosf(dd.bank), sinf(dd.bank) };
    int dcy = cy - hop;
    int shr = (int)(sc * 1.05f) - hop / 2; if (shr < 3) shr = 3;
    fx3d::dither_disc(cx, cy + (int)(sc * 0.95f), shr, K_BLK);                         // contact shadow
    float qx[6][4], qy[6][4], fz = 9; int8_t sh[6]; int front = 0;
    for (int v = 0; v < 6; v++) {
        const FaceFrame &f = FACE[v]; float nx, ny;
        float nz = rot_pt(r, f.n[0], f.n[1], f.n[2], &nx, &ny);
        if (nz < fz) { fz = nz; front = v; }
        float k = -0.2f * nx - 0.35f * ny - 0.92f * nz;
        sh[v] = nz >= 0 ? 9 : k > 0.72f ? 0 : k > 0.5f ? 1 : k > 0.25f ? 2 : 3;      // shade index (9 = hidden)
        if (nz < 0) for (int c = 0; c < 4; c++) {                                     // the face quad's corners
            float su = (c == 1 || c == 2) ? 1.f : -1.f, sv = c >= 2 ? 1.f : -1.f;
            rot_pt(r, f.n[0] + f.u[0] * su + f.v[0] * sv, f.n[1] + f.u[1] * su + f.v[1] * sv, f.n[2] + f.u[2] * su + f.v[2] * sv, &qx[v][c], &qy[v][c]);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        float s = pass ? sc : sc + 1.6f;
        for (int v = 0; v < 6; v++) {
            if (sh[v] > 3) continue;
            uint16_t col = pass ? SHADE[sh[v]] : K_BLK;
            int x0 = cx + (int)(qx[v][0] * s), y0 = dcy + (int)(qy[v][0] * s), x2 = cx + (int)(qx[v][2] * s), y2 = dcy + (int)(qy[v][2] * s);
            d.fillTriangle(x0, y0, cx + (int)(qx[v][1] * s), dcy + (int)(qy[v][1] * s), x2, y2, col);
            d.fillTriangle(x0, y0, x2, y2, cx + (int)(qx[v][3] * s), dcy + (int)(qy[v][3] * s), col);
        }
    }
    if (!show_pips) return;
    float pr = sc * 0.16f; if (pr < 1.5f) pr = 1.5f;
    for (int v = 0; v < 6; v++) {
        if (sh[v] > 2) continue;                                                       // hidden or too oblique
        const FaceFrame &f = FACE[v];
        uint16_t col = v != front ? PIP_FAINT : (v == 0 ? RGB(219, 0, 0) : K_BLK);
        for (int k = 0; k <= v; k++) {
            float pu = PIPS[v + 1][k][0] * 0.63f, pv = PIPS[v + 1][k][1] * 0.63f, x, y;
            rot_pt(r, f.n[0] * 1.03f + f.u[0] * pu + f.v[0] * pv, f.n[1] * 1.03f + f.u[1] * pu + f.v[1] * pv, f.n[2] * 1.03f + f.u[2] * pu + f.v[2] * pv, &x, &y);
            d.fillSmoothCircle(cx + (int)(x * sc), dcy + (int)(y * sc), v != front ? pr * 0.75f : v == 0 ? pr * 1.4f : pr, col);
        }
    }
}

// ---------------------------------------------------------------- shared bits
#define TOPH 18
static int text(const char *s, int x, int y, int datum, int font, uint16_t col) { return gui::text(s, x, y, datum, font, col, K_BLK); }
// Compact top bar: player chip + round + big total.
static void topbar(void)
{
    uint16_t pc = PCOL[s_cur];
    d.fillRect(0, 0, W, TOPH, K_BLK);
    char b[24]; ptag(s_cur, b);
    int cw = gui::text_width(b, gui::F_BODY) + 10;
    d.fillSmoothRoundRect(2, 1, cw, TOPH - 2, 4, pc);
    gui::text(b, 2 + cw / 2, 0, 1, gui::F_BODY, K_BLK, pc);
    snprintf(b, sizeof b, GT("Giro %d/13", "Round %d/13"), s_turn + 1);
    text(b, cw + 9, 1, 0, gui::F_SMALL, FG);
    snprintf(b, sizeof b, "%d", total(&s_pl[s_cur]));
    text(b, W - 4, 0, 2, gui::F_BODY, pc);
    d.drawFastHLine(0, TOPH - 1, W, fx3d::scl(pc, 170, 255));
}
static void draw_toast(void)
{
    gui::panel(13, 42, 214, 48, K_BLK, s_toast_col);
    text(s_toast, W / 2, 47, 1, gui::F_BODY, s_toast_col);
    text(s_toast2, W / 2, 69, 1, gui::F_SMALL, 0xFFFF);
}

// ---------------------------------------------------------------- ROLL view
#define CELLH 64
static void draw_roll(void)
{
    int64_t tnow = esp_timer_get_time();
    bool human = !s_pl[s_cur].cpu;
    bool can_roll = human && !s_anim && s_rolls > 0;
    bool imu = nucleo_imu_present();
    int y0 = H - CELLH - 1;                                              // the dice tray

    if (tnow < s_cele_us || tnow < s_toast_us) s_bgdirty = true;          // overlays repaint cleanly over the felt

    // ---- static felt + UI: painted ONLY when it changed ----
    if (s_bgdirty) {
        topbar();
        gui::vgradient(0, TOPH, W, y0 - TOPH, RGB(0, 36, 0), RGB(0, 109, 36));
        d.fillRect(0, y0, W, H - y0, TRAY);
        d.drawFastHLine(0, y0, W, (can_roll && imu && s_armed) ? C_GREEN : RGB(73, 146, 85));
        char ln[32];
        if (!human)           snprintf(ln, sizeof ln, "%s", GT("Gioca la CPU", "CPU is playing"));
        else if (s_anim)      snprintf(ln, sizeof ln, "...");
        else if (!s_rolled)   { char t[8]; ptag(s_cur, t); snprintf(ln, sizeof ln, GT("Tocca a %s", "%s to roll"), t); }
        else if (s_rolls > 0) snprintf(ln, sizeof ln, GT("Lanci: %d", "Rolls left: %d"), s_rolls);
        else                  snprintf(ln, sizeof ln, "%s", GT("Scegli la casella", "Pick a box"));
        text(ln, W / 2, TOPH + 1, 1, gui::F_BODY, s_anim ? C_YELLOW : 0xFFFF);

        if (can_roll && imu) {
            int px = 40, pw = 160, py = TOPH + 23, ph = 14;
            if (s_armed) d.fillSmoothRoundRect(px, py, pw, ph, 6, C_GREEN); else d.drawRoundRect(px, py, pw, ph, 6, MUTED);
            text(s_armed ? GT("SCUOTI PER LANCIARE", "SHAKE TO ROLL") : GT("tieni fermo", "hold still"), W / 2, py - 1, 1, gui::F_SMALL, s_armed ? K_BLK : MUTED);
        } else if (human && !s_anim) {                                    // one pill per roll left, in the player's colour
            for (int r = 0; r < 3; r++) {
                int bx = 33 + r * 60, by = TOPH + 25;
                if (r < s_rolls) { d.fillSmoothRoundRect(bx, by, 54, 10, 5, PCOL[s_cur]); d.drawFastHLine(bx + 5, by + 2, 44, fx3d::mix(PCOL[s_cur], 0xFFFF, 120)); }
                else d.drawRoundRect(bx, by, 54, 10, 5, RGB(73, 109, 73));
            }
        }
        const char *h = !human ? "" : s_anim ? ""
                      : !s_rolled ? GT("INVIO o GO: lancia", "ENTER or GO: roll")
                      : s_rolls > 0 ? GT("3-7 tieni  0 tutti  INVIO tira", "3-7 hold  0 all  ENTER roll")
                      : GT("INVIO o TAB: segna", "ENTER or TAB: score");
        if (h[0]) text(h, W / 2, y0 - 17, 1, gui::F_SMALL, C_YELLOW);
        s_bgdirty = false;
    }

    // ---- dice tray: repaint PER CELL each frame (the only animated region) ----
    int cy = y0 + 27;
    float t = 1.0f;                                                       // roll progress 0..1 (drives the toss arc)
    if (s_anim) { t = (float)(tnow - s_roll_us) / (float)s_dur_us; if (t > 1) t = 1; if (t < 0) t = 0; }
    float pulse = 0.5f + 0.5f * sinf((float)tnow * 9e-6f);
    for (int i = 0; i < 5; i++) {
        int cx = 24 + i * 48;
        bool cur = (i == s_diecur && s_rolled && human && !s_anim);
        bool rolling = s_anim && s_d[i].rolling;
        int  hop = rolling ? (int)(sinf(t * 3.14159f) * 10.0f) : 0;       // a toss inside the cell
        d.fillRect(cx - 24, y0 + 1, 48, CELLH, TRAY);
        if (s_d[i].held) { d.fillSmoothRoundRect(cx - 22, y0 + 3, 44, CELLH - 4, 6, RGB(0, 73, 36)); d.drawRoundRect(cx - 22, y0 + 3, 44, CELLH - 4, 6, C_GREEN); }
        if (cur) { d.drawRoundRect(cx - 23, y0 + 2, 46, CELLH - 2, 6, C_YELLOW); d.drawRoundRect(cx - 22, y0 + 3, 44, CELLH - 4, 6, C_YELLOW); }
        if (tnow < s_suggest_us && !s_anim && s_suggest[i] && !s_d[i].held)    // auto-hold suggestion: pulsing cyan
            d.drawRoundRect(cx - 21, y0 + 4, 42, CELLH - 6, 5, fx3d::scl(RGB(73, 219, 255), (int)(pulse * 195) + 60, 255));
        draw_die(i, cx, cy, 14.5f, !rolling || t > 0.55f, hop);           // pips hidden during the fast spin
        if (s_d[i].held) { d.fillRect(cx - 21, H - 13, 42, 11, RGB(0, 73, 36)); text(GT("TIENI", "HOLD"), cx, H - 15, 1, gui::F_SMALL, C_GREEN); }
    }
}

// ---------------------------------------------------------------- SCORE view (scrollable scorecard)
// Category family palette — the upper numbers, the of-a-kinds, the straights, etc. each get their own hue.
static uint16_t cat_color(int idx)
{
    if (idx <= 5)              return RGB(109, 182, 255);   // upper section — blue
    if (idx == 6 || idx == 7)  return RGB(255, 146, 85);    // of-a-kinds — orange
    if (idx == 8)              return RGB(255, 109, 170);   // full house — pink
    if (idx == 9 || idx == 10) return RGB(109, 219, 170);   // straights — green
    if (idx == 11)             return RGB(255, 219, 85);    // YAHTZEE — gold
    return                            RGB(182, 146, 255);   // chance — violet
}
static void draw_score(void)
{
    Player &p = s_pl[s_cur];
    int bm = best_open_idx(&p);
    topbar();
    d.fillRect(0, TOPH, W, H - TOPH, BG);

    const int VIS = SC_VIS, rowH = 18;
    int listY = TOPH + 13, listH = VIS * rowH;                 // 31 .. 103
    int start = (int)s_scroll; float frac = s_scroll - (float)start;
    d.setClipRect(0, listY, W, listH);
    for (int r = 0; r <= VIS; r++) {
        int idx = start + r; if (idx >= N_CAT) break;
        int ry = listY + (int)((float)r * rowH - frac * rowH);
        bool used = p.used[idx], cursor = (idx == s_sel), ok = allowed(&p, idx);
        uint16_t gc = cat_color(idx);
        if (cursor) { d.fillSmoothRoundRect(2, ry + 1, W - 8, rowH - 2, 5, fx3d::scl(gc, 70, 255)); d.drawRoundRect(2, ry + 1, W - 8, rowH - 2, 5, gc); }
        d.fillRect(6, ry + 5, 4, rowH - 9, used ? fx3d::scl(gc, 80, 255) : gc);           // family tag
        text(cat_name(idx), 15, ry, 0, gui::F_BODY, cursor ? 0xFFFF : (used || !ok) ? MUTED : gc);
        int val = used ? p.score[idx] : preview(&p, idx);
        char vb[6]; snprintf(vb, sizeof vb, used && !val ? "--" : "%d", val);
        uint16_t valc = used ? (val ? MUTED : RGB(146, 73, 73)) : !ok ? RGB(73, 73, 73) : idx == bm ? C_GREEN : 0xFFFF;
        text(vb, W - 12, ry, 2, gui::F_BODY, valc);
    }
    d.clearClipRect();
    int thumbH = listH * VIS / N_CAT;                         // scrollbar
    d.fillRect(W - 3, listY, 2, listH, fx3d::scl(LINE, 120, 255));
    d.fillRect(W - 3, listY + (listH - thumbH) * start / (N_CAT - VIS), 2, thumbH, cat_color(s_sel));

    // mini dice strip: the dice that make the selected box glow in its colour
    bool contrib[5] = {};
    if (s_rolled && allowed(&p, s_sel) && preview(&p, s_sel) > 0) compute_contribution(s_sel, contrib);
    uint16_t selc = cat_color(s_sel);
    bool fl = esp_timer_get_time() < s_contrib_flash_us;
    int dy = listY + listH + 1;
    for (int i = 0; i < 5; i++) {
        int dx = (W - 82) / 2 + i * 17;
        bool c = contrib[i];
        d.fillSmoothRoundRect(dx, dy, 14, 14, 3, c ? (fl ? 0xFFFF : selc) : RGB(36, 36, 36));
        char nb[2] = { (char)('0' + s_d[i].value), 0 };
        d.setTextColor(c ? K_BLK : MUTED); d.setCursor(dx + 4, dy + 4); d.print(nb);
    }

    // upper-bonus strip (clips the list overflow at the top)
    int sub = upper_sub(&p);
    d.fillRect(0, TOPH, W, 13, K_BLK);
    char bb[28]; snprintf(bb, sizeof bb, GT("Superiori %d/63", "Upper %d/63"), sub);
    d.setTextColor(sub >= 63 ? C_GREEN : FG); d.setCursor(4, TOPH + 3); d.print(bb);
    int barx = 136, barw = W - barx - 28;
    d.fillRect(barx, TOPH + 4, barw, 4, fx3d::scl(LINE, 200, 255));
    d.fillRect(barx, TOPH + 4, barw * (sub > 63 ? 63 : sub) / 63, 4, sub >= 63 ? C_GREEN : C_YELLOW);
    d.setTextColor(sub >= 63 ? C_GREEN : MUTED); d.setCursor(W - 22, TOPH + 3); d.print("+35");

    // action band: what ENTER does here
    int idx = s_sel, bandY = H - 17;
    bool ok = allowed(&p, idx);
    uint16_t accent = ok && idx == bm ? C_GREEN : cat_color(idx);
    d.fillRect(0, bandY, W, 17, K_BLK);
    d.drawFastHLine(0, bandY, W, accent);
    char fb[40];
    if (p.used[idx])  snprintf(fb, sizeof fb, GT("%s: gia' segnata", "%s: already scored"), cat_name(idx));
    else if (!ok)     snprintf(fb, sizeof fb, GT("Jolly: segna in %s", "Joker: score in %s"), cat_name(joker(&p) && !p.used[s_d[0].value - 1] ? s_d[0].value - 1 : 6));
    else              snprintf(fb, sizeof fb, GT("INVIO: %s +%d", "ENTER: %s +%d"), cat_name(idx), preview(&p, idx));
    int fw = text(fb, 4, bandY + 1, 0, gui::F_SMALL, ok ? accent : MUTED);
    const char *tb = GT("TAB dadi", "TAB dice");
    if (fw + gui::text_width(tb, gui::F_SMALL) < W - 16) text(tb, W - 4, bandY + 1, 2, gui::F_SMALL, MUTED);   // only when it fits
}

// ---------------------------------------------------------------- title / help / results
static void draw_setup(void)
{
    int y = gui::title("YAHTZEE", GT("5 dadi, 3 lanci, 13 caselle", "5 dice, 3 rolls, 13 boxes"), C_YELLOW);
    draw_die(0, 26, 12, 8.5f); draw_die(1, W - 26, 12, 8.5f);              // a die on each side of the title
    char pl[32];
    if (s_cpus) snprintf(pl, sizeof pl, "%s: %d + %d CPU", GT("Giocatori", "Players"), s_humans, s_cpus);
    else        snprintf(pl, sizeof pl, "%s: %d", GT("Giocatori", "Players"), s_humans);
    const char *items[3] = { GT("Gioca", "Play"), pl, GT("Come si gioca", "How to play") };
    gui::menu(s_menu, items, 3, y, nucleo_app_content_height(), C_YELLOW);
}
static void draw_help(void)
{
    int y = gui::title(GT("Come si gioca", "How to play"), nullptr, C_YELLOW) + 2;
    const char *L[5] = {
        GT("Fino a 3 lanci per turno: tieni", "Up to 3 rolls a turn: hold the"),
        GT("i dadi (3-7, 0 tutti), poi segna", "dice (3-7, 0 all), then score one"),
        GT("una delle 13 caselle (TAB).", "of the 13 boxes (TAB)."),
        GT("Superiori 63 o piu': +35.", "Upper boxes 63 or more: +35."),
        GT("Altro Yahtzee: +100 e jolly.", "Extra Yahtzee: +100 and joker."),
    };
    for (int i = 0; i < 5; i++) text(L[i], 6, y + i * 16, 0, gui::F_SMALL, i < 3 ? 0xFFFF : C_YELLOW);
}
static void draw_over(void)
{
    int ord[MAXP]; for (int i = 0; i < s_np; i++) ord[i] = i;
    for (int a = 0; a < s_np; a++) for (int b = a + 1; b < s_np; b++)
        if (total(&s_pl[ord[b]]) > total(&s_pl[ord[a]])) { int t = ord[a]; ord[a] = ord[b]; ord[b] = t; }
    char ttl[24], tg[8];
    ptag(ord[0], tg);
    if (s_np > 1 && total(&s_pl[ord[0]]) == total(&s_pl[ord[1]])) snprintf(ttl, sizeof ttl, "%s", GT("Pareggio!", "It's a tie!"));
    else if (s_np > 1) snprintf(ttl, sizeof ttl, GT("Vince %s!", "%s wins!"), tg);
    else snprintf(ttl, sizeof ttl, "%s", GT("Partita finita", "Game over"));
    int y = gui::title(ttl, nullptr, C_YELLOW);
    for (int r = 0; r < s_np; r++) {
        int pi = ord[r], ry = y + r * 20;
        uint16_t med = r == 0 ? RGB(255, 219, 0) : r == 1 ? RGB(182, 182, 170) : r == 2 ? RGB(219, 109, 0) : MUTED;
        if (r == 0) gui::panel(4, ry, W - 10, 19, RGB(73, 73, 0), C_YELLOW);
        d.fillSmoothCircle(18, ry + 9, 7, med);
        char b[24]; snprintf(b, sizeof b, "%d", r + 1);
        text(b, 18, ry + 2, 1, gui::F_SMALL, K_BLK);
        ptag(pi, b); text(b, 34, ry, 0, gui::F_BODY, r == 0 ? C_YELLOW : PCOL[pi]);
        snprintf(b, sizeof b, "%d", total(&s_pl[pi])); text(b, W - 14, ry, 2, gui::F_BODY, r == 0 ? C_YELLOW : 0xFFFF);
    }
    for (int i = 0; i < s_npart; i++) {                                    // confetti rain
        Part &q = s_part[i]; if (q.life == 0) continue;
        d.fillRect((int)q.x - 1, (int)q.y - 1, 3, 3, fx3d::scl(q.col, 255 * q.life / (q.lmax ? q.lmax : 1), 255));
    }
}

// Full-screen YAHTZEE overlay: rotating golden rays + expanding shockwaves + confetti + a popping,
// glowing, colour-cycling "YAHTZEE!" in the big face.
static void draw_celebration(int64_t now)
{
    float p = (float)(now - s_cele_start) * (1.0f / 2600000.0f); if (p > 1.0f) p = 1.0f;
    int cx = W / 2, cy = H / 2;
    float spin = (float)now * 1e-6f * 2.5f;
    for (int k = 0; k < 12; k++) {                                   // rotating golden light rays
        float a = spin + k * (TWO_PI / 12.0f);
        d.drawLine(cx, cy, cx + (int)(cosf(a) * 220), cy + (int)(sinf(a) * 220), fx3d::scl((k & 1) ? 0xFFFF : C_YELLOW, (int)(110 * (1.0f - p)) + 30, 255));
    }
    for (int w = 0; w < 3; w++) {                                    // expanding shockwave rings
        float rp = p * 1.5f - w * 0.16f; if (rp <= 0.0f || rp >= 1.0f) continue;
        uint16_t cc = fx3d::scl((w & 1) ? 0xFFFF : C_YELLOW, (int)(255 * (1.0f - rp)), 255);
        d.drawCircle(cx, cy, (int)(rp * 175), cc); d.drawCircle(cx, cy, (int)(rp * 175) - 1, fx3d::scl(cc, 150, 255));
    }
    for (int i = 0; i < s_npart; i++) {                              // confetti
        Part &q = s_part[i]; if (q.life == 0) continue;
        d.fillRect((int)q.x - 1, (int)q.y - 1, 3, 3, fx3d::scl(q.col, 255 * q.life / (q.lmax ? q.lmax : 1), 255));
    }
    if (p < 0.08f) return;                                           // the word pops in after the flash
    int bh = p < 0.16f ? (int)((p - 0.08f) * 600) : 48;               // the band opens, then holds
    d.fillRect(0, cy - bh / 2, W, bh, K_BLK);
    d.drawFastHLine(0, cy - bh / 2, W, C_YELLOW); d.drawFastHLine(0, cy + bh / 2, W, C_YELLOW);
    if (bh < 48) return;
    int ph = (int)((now / 110000) % 3), ty = cy - 21;
    static const int8_t ox[4] = { -1, 1, 0, 0 }, oy[4] = { 0, 0, -1, 1 };
    for (int o = 0; o < 4; o++) gui::text("YAHTZEE!", cx + ox[o], ty + oy[o], 1, gui::F_BIG, RGB(146, 73, 0), RGB(146, 73, 0));   // outline
    gui::text("YAHTZEE!", cx, ty, 1, gui::F_BIG, ph == 0 ? C_YELLOW : ph == 1 ? 0xFFFF : RGB(255, 146, 0), K_BLK);
    text(GT("CINQUE UGUALI!", "FIVE OF A KIND!"), cx, ty + 26, 1, gui::F_SMALL, 0xFFFF);
}

static void draw(void)
{
    switch (s_phase) {
        case PH_SETUP: draw_setup(); return;
        case PH_HELP:  draw_help(); return;
        case PH_OVER:  draw_over(); return;
        case PH_SCORE: draw_score(); break;
        default:       draw_roll(); break;
    }
    int64_t now = esp_timer_get_time();
    if (s_phase == PH_ROLL && now < s_cele_us) draw_celebration(now);
    else if (now < s_toast_us) draw_toast();
    if (s_paused) {
        char l1[24]; snprintf(l1, sizeof l1, GT("Giro %d/13", "Round %d/13"), s_turn + 1);
        gui::dialog(GT("PAUSA", "PAUSED"), l1, nullptr, GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave"), C_YELLOW);
        s_bgdirty = true;                                            // the card covers the felt: repaint it on resume
    }
}

// ---------------------------------------------------------------- input
static void go_title(void)
{
    s_phase = PH_SETUP; s_paused = false; s_npart = 0; s_cele_us = 0; s_toast_us = 0;
    nucleo_app_set_fullscreen(false);
    nucleo_app_set_hint(GT("su/giu  sx/dx cambia  INVIO ok", "UP/DN  L/R change  ENTER ok"));
    nucleo_app_request_draw();
}
static void start_game(void)
{
    int n = s_humans + s_cpus; if (n < 1) n = 1; if (n > MAXP) n = MAXP;
    s_np = n;
    for (int i = 0; i < MAXP; i++) { memset(&s_pl[i], 0, sizeof(Player)); s_pl[i].cpu = (i >= s_humans); }
    s_cur = 0; s_turn = 0; s_toast_us = 0; s_npart = 0; s_cele_us = 0; s_paused = false;
    nucleo_app_set_fullscreen(true);
    nucleo_app_set_hint("");
    start_turn(esp_timer_get_time());
    nucleo_app_request_draw();
}
static void lineup_step(int dir)
{
    int n = (int)sizeof LINEUP, k = 0;
    for (int i = 0; i < n; i++) if (LINEUP[i] == (s_humans << 4 | s_cpus)) k = i;
    k = (k + n + dir) % n;
    s_humans = LINEUP[k] >> 4; s_cpus = LINEUP[k] & 15;
    sfx(SFX_NAV); nucleo_app_request_draw();
}

static bool poll(void)
{
    int64_t now = esp_timer_get_time();
    int dt_ms = s_last_us ? (int)((now - s_last_us) / 1000) : 20;
    if (dt_ms > 50) dt_ms = 50;
    s_last_us = now;
    if (s_phase == PH_SETUP) return gui::menu_tick(s_menu, dt_ms);   // a still title pushes nothing
    if (s_phase == PH_HELP) return false;
    sim(dt_ms * 0.001f);
    if (s_paused) return false;
    // Repaint ONLY while something moves: tumbling dice, a toast, the celebration / confetti, the scorecard
    // scroll, the CPU's turn, the pulsing suggestion, the mini-dice flash.
    bool scrolling = s_phase == PH_SCORE && fabsf(s_scroll - (float)clampi(s_sel - 2, 0, N_CAT - SC_VIS)) > 0.02f;
    return s_anim || now < s_toast_us || now < s_cele_us || scrolling
        || (s_phase == PH_ROLL && (s_pl[s_cur].cpu || now < s_suggest_us))
        || (s_phase == PH_SCORE && now < s_contrib_flash_us);
}
static void ptt(bool on)
{
    if (!on) return;
    int64_t now = esp_timer_get_time();
    if (s_phase == PH_ROLL && !s_paused && !s_pl[s_cur].cpu && !s_anim && now > s_toast_us && now > s_cool_us && s_rolls > 0) { do_roll(now); nucleo_app_request_draw(); }
}
static void on_key(int key, char ch)
{
    int64_t now = esp_timer_get_time();
    s_suggest_us = 0;                                                              // any key clears the auto-hold suggestion
    s_bgdirty = true;                                                              // any input repaints the ROLL static UI
    nucleo_app_request_draw();
    if (s_phase == PH_SETUP) {
        if (gui::menu_key(s_menu, key, 3)) sfx(SFX_NAV);
        else if (key == NK_RIGHT && s_menu.sel == 1) lineup_step(+1);
        else if (key == NK_ENTER) {
            if (s_menu.sel == 0) start_game();
            else if (s_menu.sel == 1) lineup_step(+1);
            else { s_phase = PH_HELP; nucleo_app_set_hint(GT("Esc indietro", "Esc back")); }
        }
        return;
    }
    if (s_phase == PH_HELP) { if (key == NK_ENTER) go_title(); return; }
    if (s_phase == PH_OVER) { if (key == NK_ENTER) start_game(); return; }
    if (s_paused) { if (key == NK_ENTER) { s_paused = false; sfx(SFX_NAV); } return; }
    if (now < s_toast_us) { s_toast_us = 0; return; }                              // any key dismisses a toast
    if (s_pl[s_cur].cpu && s_phase == PH_ROLL) return;                              // hands off during CPU turn

    if (s_phase == PH_SCORE) {
        if (key == NK_UP)        { if (s_sel > 0)         { s_sel--; sfx(SFX_NAV); s_contrib_flash_us = now + 350000; } }
        else if (key == NK_DOWN) { if (s_sel < N_CAT - 1) { s_sel++; sfx(SFX_NAV); s_contrib_flash_us = now + 350000; } }
        else if (key == NK_ENTER) lock_category(s_sel, now);
        return;
    }
    // PH_ROLL
    if (s_anim) return;
    if (ch >= '3' && ch <= '7') {                                    // direct hold: keys 3 4 5 6 7 = dice 1..5
        if (s_rolled) { int di = ch - '3'; s_diecur = di; bool &hd = s_d[di].held; hd = !hd; sfx(hd ? SFX_HOLDON : SFX_HOLDOFF); }
        return;
    }
    if (ch == '0') {                                                 // hold-all / release-all
        if (s_rolled) {
            bool anyFree = false;
            for (int i = 0; i < 5; i++) anyFree |= !s_d[i].held;
            for (int i = 0; i < 5; i++) s_d[i].held = anyFree;
            sfx(SFX_HOLDALL);
        }
        return;
    }
    if (ch == ' ' || ch == 'z' || ch == 'Z') {                       // SPACE or Z: hold/release the selected die
        if (s_rolled) { bool &hd = s_d[s_diecur].held; hd = !hd; sfx(hd ? SFX_HOLDON : SFX_HOLDOFF); }
        return;
    }
    if (key == NK_ENTER) {
        if (s_rolls > 0) { if (now > s_cool_us) do_roll(now); }       // roll — respects the inter-roll pause
        else if (s_rolled) goto_score();
        return;
    }
    if (key == NK_RIGHT) { if (s_diecur < 4) { s_diecur++; sfx(SFX_NAV); } }
    else if (key == NK_DOWN && s_rolled) goto_score();
}
// LEFT + BACK route here. LEFT = previous die / fewer players; BACK = step back: scorecard -> dice,
// game -> pause card -> title, help -> title, title -> leave the app.
static bool back(int key)
{
    nucleo_app_request_draw(); s_bgdirty = true;
    if (s_phase == PH_SETUP) { if (key == NK_LEFT && s_menu.sel == 1) lineup_step(-1); return key == NK_LEFT; }
    if (s_phase == PH_HELP || s_phase == PH_OVER) { go_title(); return true; }
    if (key == NK_BACK && s_paused) { go_title(); return true; }
    if (s_paused) return true;
    if (esp_timer_get_time() < s_toast_us) { s_toast_us = 0; return true; }
    if (s_phase == PH_SCORE) { s_phase = PH_ROLL; sfx(SFX_NAV); return true; }
    if (key == NK_LEFT) { if (s_diecur > 0 && !s_anim) { s_diecur--; sfx(SFX_NAV); } return true; }
    s_paused = true; sfx(SFX_NAV);                                                // Esc in a game: the pause card
    return true;
}
static void tab(void)
{
    if (s_paused) return;
    if (s_phase == PH_ROLL && s_rolled && !s_anim && !s_pl[s_cur].cpu) goto_score();
    else if (s_phase == PH_SCORE) s_phase = PH_ROLL;
    s_bgdirty = true; nucleo_app_request_draw();
}

static void enter(void)
{
    game_text_open("yahtzee");
    s_last_us = 0; s_anim = false; s_cele_start = 0; s_suggest_us = 0; s_contrib_flash_us = 0;
    memset(s_suggest, 0, sizeof(s_suggest));
    s_menu = {};
    reset_dice_idle();
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_ptt_handler(ptt);
    go_title();
}

static void on_exit(void) { nucleo_audio_stop(); game_text_close(); }

// Working RAM: allocated (zeroed) by the framework before enter(), freed after on_exit(). Foreground-only;
// every open starts at the title, so no game state needs to survive a close.
static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_part, sizeof(Part) * 30 },
    { (void **)&s_pl,   sizeof(Player) * MAXP },
    { (void **)&s_d,    sizeof(Die) * 5 },
    { nullptr, 0 }
};

extern "C" void nucleo_register_yahtzee(void)
{
    static const nucleo_app_def_t app = {
        "yahtzee", "Yahtzee", "Games", "Yahtzee a turni (1-4 + CPU), dadi 3D",
        'Y', C_YELLOW, enter, on_key, nullptr, draw, on_exit,
        NX_NET_APP,  // dedicate RAM + free the shared I2S/mic line so the SFX reliably play
        APP_RAM
    };
    nucleo_app_register(&app);
}
