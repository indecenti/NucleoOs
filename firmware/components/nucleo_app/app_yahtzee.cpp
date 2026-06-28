// Yahtzee — Games. Classic 5-dice game, turn-based (hot-seat 1-4 + optional CPU). Reuses the fx3d engine
// from the Dadi app for the tumbling 3D d6 (outline trick + pips on the real faces). Designed for the tiny
// 240x135 screen: every turn fills the screen — ROLL view makes the dice the hero (felt + 3D tumble), SCORE
// view is a readable two-column scorecard (all 13 boxes visible, ghost previews of what each box would score)
// with a big size-2 focus band for the selected box. Honest randomness from the ESP32 hardware TRNG.
//
// FLOW per turn: roll (up to 3), hold dice between rolls, then assign the result to one of 13 boxes (each
// used once). After all players fill all 13 boxes -> results screen. Keys: see the on-screen hints.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include "nucleo_fx3d.h"
#include "nucleo_audio.h"
#include "nucleo_exclusive.h"  // NX_NET_APP: dedicate RAM + free the shared I2S line so the SFX reliably play
#include "game_sfx.h"          // shared SFX engine (versioned SD cache + pack/synth fallback, never-mute)
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>          // mkdir for the SFX cache dirs
#include "esp_random.h"
#include "esp_timer.h"
#include "app_gfx.h"

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif
#define TWO_PI 6.2831853f
#define DIRR "/sd/data/yahtzee"

// ---- d6 cube (RAM 0, flash) on the fx3d engine ----
static const fx3d::V3 CUBEV[8] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
static const fx3d::Tri CUBET[12] = { {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},{1,5,6},{1,6,2},{2,6,7},{2,7,3},{3,7,4},{3,4,0} };
static const fx3d::Model CUBE = { CUBEV, 8, CUBET, 12 };

// settle poses (turn the value's face toward the camera) + pip layout — same mapping as the Dadi app
static const float POSE_Y[6] = { (float)M_PI, (float)(M_PI/2), 0, 0, (float)(-M_PI/2), 0 };
static const float POSE_P[6] = { 0, 0, (float)(-M_PI/2), (float)(M_PI/2), 0, 0 };
struct FaceFrame { float n[3], u[3], v[3]; };
static const FaceFrame FACE[6] = {
    { { 0, 0, 1}, {1,0,0}, {0,1,0} }, { { 1, 0, 0}, {0,0,-1},{0,1,0} }, { { 0, 1, 0}, {1,0,0}, {0,0,-1} },
    { { 0,-1, 0}, {1,0,0}, {0,0,1} }, { {-1, 0, 0}, {0,0,1}, {0,1,0} }, { { 0, 0,-1}, {1,0,0}, {0,-1,0} },
};
static const int   PIPN[7] = { 0,1,2,3,4,5,6 };
static const float PIPS[7][6][2] = {
    {{0,0}}, {{0,0}}, {{-0.5f,0.5f},{0.5f,-0.5f}}, {{-0.55f,0.55f},{0,0},{0.55f,-0.55f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0,0},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.62f},{-0.5f,0},{-0.5f,0.62f},{0.5f,-0.62f},{0.5f,0},{0.5f,0.62f}},
};

// ---- scorecard categories ----
enum { N_CAT = 13 };
#define SC_VIS 4               // scorecard rows visible at once (tall, airy rows)
static const char *CAT_SHORT[N_CAT] = { "Uno","Due","Tre","Quattro","Cinque","Sei",
                                        "Tris","Poker","Full","Scala4","Scala5","Yahtzee","Chance" };
static const char *CAT_FULL[N_CAT]  = { "Uno (1)","Due (2)","Tre (3)","Quattro (4)","Cinque (5)","Sei (6)",
                                        "Tris","Poker","Full House","Scala piccola","Scala grande","YAHTZEE!","Chance" };

#define MAXP 4
struct Player { int score[N_CAT]; bool used[N_CAT]; int ybonus; bool cpu; };
static Player s_pl[MAXP];
static int    s_np, s_cur, s_turn;        // player count, current player, turn 0..12

struct Die { float yaw, pitch, bank, y0, p0, ye, pe, wob; int value; bool held, rolling; };
static Die s_d[5];

enum { PH_SETUP, PH_ROLL, PH_SCORE, PH_OVER };
static int  s_phase;
static int  s_rolls;            // rolls left this turn
static bool s_rolled;           // rolled at least once this turn
static int  s_diecur;           // ROLL view: selected die
static int   s_sel;             // SCORE view: selected category 0..12 (single scrollable list)
static float s_scroll;          // SCORE view: eased top-row offset for smooth scrolling
static int  s_setsel, s_humans = 1, s_cpus = 1;  // SETUP

static bool    s_anim;
static bool    s_bgdirty = true;   // ROLL: repaint the static felt/UI only when it changed (no per-frame full clear = no flicker)
static int64_t s_roll_us, s_dur_us, s_last_us, s_cool_us;   // s_cool_us: pause before the next roll is accepted
static bool    s_armed;                                     // shake-to-roll arming (see sim): never auto-fires
static int64_t s_toast_us; static char s_toast[24], s_toast2[24]; static uint16_t s_toast_col;
static int64_t s_cele_us;        // celebration / confetti overlay until (us)
static int64_t s_cele_start;     // celebration start (drives the 0..1 progress)
static int64_t s_cpu_us; static int s_cpu_step;
static bool    s_suggest[5];     // auto-hold suggestion mask (cleared on any keypress)
static int64_t s_suggest_us;     // suggestion visible until this time
static int64_t s_contrib_flash_us; // mini-dice strip flash duration on category change

// Heap-free particle burst: YAHTZEE fireworks (radial) + game-over confetti (rain). No per-frame alloc.
struct Part { float x, y, vx, vy; uint16_t col; uint8_t life, lmax; };
static Part s_part[30];
static int  s_npart;

static uint16_t COL_IVORY, COL_PIP, COL_PIP1, COL_FELT, COL_FELTG;
static const uint16_t PCOL[MAXP] = { C_GREEN, C_YELLOW, C_BLUE, C_PINK };

static int  randn(int n) { return (int)(esp_random() % (uint32_t)n); }
static int  clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Fill the particle pool. rain=false: a radial burst from the screen centre (YAHTZEE fireworks).
// rain=true: confetti falling from the top (game-over). Colours computed at runtime (fx3d::rgb isn't constexpr).
static void spawn_burst(bool rain)
{
    static const uint8_t COLS[6][3] = { {255,210,40},{255,72,72},{72,222,120},{96,156,255},{255,255,255},{232,96,232} };
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
        const uint8_t *c = COLS[(r >> 20) % 6];
        q.col = fx3d::rgb(c[0], c[1], c[2]);
        q.lmax = q.life = (uint8_t)(60 + (r >> 17) % 60);
    }
}

// ---- SD-cached SFX: synthesize each cue ONCE to a mono WAV on the SD (notify_synth), then play it
// async via nucleo_audio. Same engine as Poker/Pinball — richer than raw beeps and ~zero runtime RAM. ----
static int g_audio = 1;
static const char *sfx_name(int id)
{
    switch (id) {
        case 1: return "nav";    case 2: return "holdon"; case 3: return "holdf"; case 4:  return "roll";
        case 5: return "settle"; case 6: return "deny";   case 7: return "score"; case 8:  return "scoreB";
        case 9: return "bonus";  case 10:return "yahtzee";case 11:return "over";  case 12: return "turn";
        case 13:return "arm";    case 14:return "holdall"; case 15:return "suggest";
        default: return "x";
    }
}
#define NSFX 15
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case 1:  notify__voice(&v[0], 660, 0, 0.035f); v[0].amp = 0.45f; return 1;                                  // nav blip
        case 2:  notify__voice(&v[0], 784, 0, 0.04f); notify__voice(&v[1], 1175, 0.03f, 0.06f); v[0].amp = 0.7f; return 2; // hold ON (up tick)
        case 3:  notify__voice(&v[0], 660, 0, 0.04f); notify__voice(&v[1], 440, 0.03f, 0.06f); v[0].amp = 0.7f; return 2;  // hold OFF (down tick)
        case 4: {                                                                                                   // dice rattle (staggered clicks)
            static const float f[8] = { 1900,1500,2100,1300,1750,1200,1650,1400 };
            float t = 0; for (int i = 0; i < 8; i++) { notify__voice(&v[i], f[i], t, 0.022f); v[i].amp = 0.5f; t += 0.034f; }
            return 8;
        }
        case 5:  notify__voice(&v[0], 140, 0, 0.10f); v[0].amp = 1.0f; notify__voice(&v[1], 1800, 0, 0.02f); v[1].amp = 0.4f; return 2; // settle thunk + click
        case 6:  notify__voice(&v[0], 175, 0, 0.09f); v[0].amp = 0.6f; notify__voice(&v[1], 150, 0.06f, 0.10f); v[1].amp = 0.6f; return 2; // deny / zero
        case 7:  notify__voice(&v[0], 784, 0, 0.045f); notify__voice(&v[1], 1175, 0.04f, 0.07f); return 2;          // ka-ching (normal score)
        case 8:  notify__voice(&v[0], 659.25f, 0, 0.06f); notify__voice(&v[1], 987.77f, 0.05f, 0.06f);              // big score (3-note)
                 notify__voice(&v[2], 1318.5f, 0.10f, 0.10f); return 3;
        case 9:  notify__voice(&v[0], 784, 0, 0.06f); notify__voice(&v[1], 1046.5f, 0.06f, 0.06f);                  // bonus (4-note bright)
                 notify__voice(&v[2], 1318.5f, 0.12f, 0.06f); notify__voice(&v[3], 1760, 0.18f, 0.14f); return 4;
        case 10: {                                                                                                  // YAHTZEE! fanfare
            static const float M[6] = { 880,1108.7f,1318.5f,1760,1318.5f,1760 };
            float t = 0; for (int i = 0; i < 6; i++) { notify__voice(&v[i], M[i], t, 0.12f); v[i].amp = 0.9f; t += 0.09f; }
            return 6;
        }
        case 11: notify__voice(&v[0], 523.25f, 0, 0.16f); notify__voice(&v[1], 415.30f, 0.14f, 0.16f);              // game over toll (descending)
                 notify__voice(&v[2], 329.63f, 0.28f, 0.16f); notify__voice(&v[3], 261.63f, 0.42f, 0.34f); v[3].amp = 0.9f; return 4;
        case 12: notify__voice(&v[0], 587.33f, 0, 0.06f); notify__voice(&v[1], 880, 0.05f, 0.10f); return 2;        // turn-start chime
        case 13: notify__voice(&v[0], 1280, 0, 0.018f); v[0].amp = 0.3f; return 1;                                  // shake-arm tick
        case 14: {                                                                                                  // hold-all: 5-note ascending cascade
            static const float F[5] = { 523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f };
            float t = 0; for (int i = 0; i < 5; i++) { notify__voice(&v[i], F[i], t, 0.042f); v[i].amp = 0.48f; t += 0.022f; }
            return 5;
        }
        case 15: notify__voice(&v[0], 1480, 0, 0.07f); notify__voice(&v[1], 2093, 0.06f, 0.10f);                   // suggest: soft double ping
                 v[0].amp = 0.20f; v[1].amp = 0.18f; return 2;
    }
    return 0;
}
static bool sfx_important(int id) { return id == 5 || (id >= 7 && id <= 12) || id == 14; } // settle + scores/fanfares/over/turn/holdall
// All cache/synth/play/fallback logic lives in the shared game_sfx engine (bump `ver` to rebuild).
static const game_sfx_t SFX = { DIRR, sfx_name, build_voices, NSFX, 2, 12000, sfx_important, &g_audio };
static inline void sfx(int id) { game_sfx_play(&SFX, id); }
// thin, readable wrappers over the cued SFX ids
static void sfx_throw(void)   { sfx(4); }
static void sfx_settle(void)  { sfx(5); }
static void sfx_hold(bool on) { sfx(on ? 2 : 3); }
static void sfx_move(void)    { sfx(1); }
static void sfx_arm(void)     { sfx(13); }
static void sfx_deny(void)    { sfx(6); }
static void sfx_score(int val, bool bonus)
{
    if (val <= 0)        sfx_deny();
    else if (bonus)      sfx(9);
    else if (val >= 30)  sfx(8);
    else                 sfx(7);
}
static void sfx_yahtzee(void) { sfx(10); }
static void sfx_gameover(void){ sfx(11); }
static void sfx_holdall(void)  { sfx(14); }
static void sfx_suggest(void)  { sfx(15); }

// ---------------------------------------------------------------- scoring
static int preview(const Player *p, int idx)
{
    (void)p;
    int c[7] = {0}, sum = 0;
    for (int i = 0; i < 5; i++) { c[s_d[i].value]++; sum += s_d[i].value; }
    bool pr[7]; for (int v = 1; v <= 6; v++) pr[v] = c[v] > 0;
    int mx = 0; for (int v = 1; v <= 6; v++) if (c[v] > mx) mx = c[v];
    if (idx <= 5) return (idx + 1) * c[idx + 1];
    switch (idx) {
        case 6:  return mx >= 3 ? sum : 0;                                   // Tris
        case 7:  return mx >= 4 ? sum : 0;                                   // Poker
        case 8: {                                                            // Full house (3+2)
            bool h3 = false, h2 = false;
            for (int v = 1; v <= 6; v++) { if (c[v] == 3) h3 = true; if (c[v] == 2) h2 = true; }
            return (h3 && h2) ? 25 : 0;
        }
        case 9:  return ((pr[1]&&pr[2]&&pr[3]&&pr[4])||(pr[2]&&pr[3]&&pr[4]&&pr[5])||(pr[3]&&pr[4]&&pr[5]&&pr[6])) ? 30 : 0;
        case 10: return ((pr[1]&&pr[2]&&pr[3]&&pr[4]&&pr[5])||(pr[2]&&pr[3]&&pr[4]&&pr[5]&&pr[6])) ? 40 : 0;
        case 11: return mx == 5 ? 50 : 0;                                    // Yahtzee
        default: return sum;                                                 // Chance
    }
}
static int upper_sub(const Player *p) { int s = 0; for (int i = 0; i < 6; i++) s += p->score[i]; return s; }
static int total(const Player *p)
{
    int s = p->ybonus; for (int i = 0; i < N_CAT; i++) s += p->score[i];
    if (upper_sub(p) >= 63) s += 35;
    return s;
}
// idx of the highest-scoring still-open box (>0), or -1 if every open box would score 0
static int best_open_idx(const Player *p)
{
    int b = -1, bv = 0;
    for (int i = 0; i < N_CAT; i++) if (!p->used[i]) { int v = preview(p, i); if (v > bv) { bv = v; b = i; } }
    return b;
}

// Which dice contribute to scoring category idx with the current dice configuration.
// Returns false for every die when the category would score 0 (nothing to highlight).
static void compute_contribution(int idx, bool out[5])
{
    int c[7] = {}; for (int i = 0; i < 5; i++) c[s_d[i].value]++;
    if (idx <= 5) {                                                      // upper section: matching face
        int v = idx + 1; for (int i = 0; i < 5; i++) out[i] = (s_d[i].value == v);
    } else if (idx == 6 || idx == 7 || idx == 11) {                    // tris/poker/yahtzee: modal value
        int mv = 1, mc = 0; for (int v2 = 1; v2 <= 6; v2++) if (c[v2] > mc) { mc = c[v2]; mv = v2; }
        for (int i = 0; i < 5; i++) out[i] = (s_d[i].value == mv);
    } else if (idx == 8) {                                              // full house: all 5 iff 3+2
        bool h3 = false, h2 = false;
        for (int v2 = 1; v2 <= 6; v2++) { if (c[v2] == 3) h3 = true; if (c[v2] == 2) h2 = true; }
        if (h3 && h2) for (int i = 0; i < 5; i++) out[i] = true;
    } else if (idx == 9 || idx == 10) {                                 // straights: dice in the run
        bool pr[7] = {}; for (int v2 = 1; v2 <= 6; v2++) pr[v2] = c[v2] > 0;
        bool in[7] = {};
        if (idx == 9) {
            if      (pr[1]&&pr[2]&&pr[3]&&pr[4]) { in[1]=in[2]=in[3]=in[4]=true; }
            else if (pr[2]&&pr[3]&&pr[4]&&pr[5]) { in[2]=in[3]=in[4]=in[5]=true; }
            else if (pr[3]&&pr[4]&&pr[5]&&pr[6]) { in[3]=in[4]=in[5]=in[6]=true; }
        } else {
            if      (pr[1]&&pr[2]&&pr[3]&&pr[4]&&pr[5]) { for(int v2=1;v2<=5;v2++) in[v2]=true; }
            else if (pr[2]&&pr[3]&&pr[4]&&pr[5]&&pr[6]) { for(int v2=2;v2<=6;v2++) in[v2]=true; }
        }
        for (int i = 0; i < 5; i++) out[i] = in[s_d[i].value];
    } else {                                                            // chance: top 3 dice by value
        int ord[5] = {0,1,2,3,4};
        for (int a = 0; a < 5; a++) for (int b = a+1; b < 5; b++)
            if (s_d[ord[b]].value > s_d[ord[a]].value) { int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
        out[ord[0]] = out[ord[1]] = out[ord[2]] = true;
    }
}

// ---------------------------------------------------------------- dice setup / rolling
static void reset_dice_idle(void)
{
    for (int i = 0; i < 5; i++) {
        int v = 1 + randn(6);
        s_d[i].value = v; s_d[i].held = false; s_d[i].rolling = false;
        s_d[i].yaw = POSE_Y[v - 1]; s_d[i].pitch = POSE_P[v - 1]; s_d[i].bank = 0;
    }
}
static void do_roll(int64_t now)
{
    if (s_rolls <= 0 || s_anim) return;
    s_cele_us = 0; s_armed = false; s_suggest_us = 0;                 // consume arming + clear suggestion
    if (!s_rolled) for (int i = 0; i < 5; i++) s_d[i].held = false;   // first roll: nothing held
    s_rolled = true; s_rolls--;
    s_dur_us = 820000; s_roll_us = now; s_anim = true; s_bgdirty = true;
    sfx_throw();
    for (int i = 0; i < 5; i++) {
        Die &dd = s_d[i];
        if (dd.held) { dd.rolling = false; continue; }
        dd.rolling = true;
        dd.y0 = dd.yaw; dd.p0 = dd.pitch;
        dd.value = 1 + randn(6);
        int spins = 2 + randn(2);                                    // fewer turns -> the eye tracks it (no strobe)
        dd.ye = POSE_Y[dd.value - 1] + TWO_PI * spins;
        dd.pe = POSE_P[dd.value - 1] + TWO_PI * (spins - 1);
        dd.wob = 0.30f;                                              // gentler tumble wobble
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
    else                 sfx(12);                                    // soft chime: it's your turn
}
static void advance_turn(int64_t now)
{
    s_cur = (s_cur + 1) % s_np;
    if (s_cur == 0) s_turn++;
    if (s_turn >= N_CAT) { s_phase = PH_OVER; s_cele_start = now; s_cele_us = now + 3200000; spawn_burst(true); sfx_gameover(); return; }
    start_turn(now);
}
static void lock_category(int idx, int64_t now)
{
    Player &p = s_pl[s_cur];
    if (p.used[idx]) return;
    int val = preview(&p, idx);
    // extra-Yahtzee bonus: rolling a 5-of-a-kind when the Yahtzee box already holds 50
    bool bonus = (preview(&p, 11) == 50 && p.used[11] && p.score[11] == 50);
    if (bonus) p.ybonus += 100;
    p.score[idx] = val; p.used[idx] = true;
    snprintf(s_toast, sizeof(s_toast), "P%d  %s", s_cur + 1, CAT_SHORT[idx]);
    if (bonus)        snprintf(s_toast2, sizeof(s_toast2), "+%d  +100 YAHTZEE!", val);
    else if (val > 0) snprintf(s_toast2, sizeof(s_toast2), "+%d punti", val);
    else              snprintf(s_toast2, sizeof(s_toast2), "-- sacrificio");
    s_toast_col = bonus ? C_YELLOW : (val > 0 ? PCOL[s_cur] : MUTED);
    s_toast_us = now + 1300000;
    sfx_score(val, bonus);
    advance_turn(now);
}
// open the scorecard with the cursor pre-placed on the best move (big usability win)
static void goto_score(void)
{
    int idx = best_open_idx(&s_pl[s_cur]);
    if (idx < 0) for (int i = 0; i < N_CAT; i++) if (!s_pl[s_cur].used[i]) { idx = i; break; }
    if (idx < 0) idx = 0;
    s_sel = idx;
    s_scroll = (float)clampi(idx - 2, 0, N_CAT - SC_VIS);   // snap so the selection is visible the instant we open
    s_contrib_flash_us = esp_timer_get_time() + 350000;      // flash the mini dice strip on first open
    s_phase = PH_SCORE;
}

// ---------------------------------------------------------------- CPU
static int cpu_mode_value(void)
{
    int c[7] = {0}; for (int i = 0; i < 5; i++) c[s_d[i].value]++;
    int mv = 1, mc = 0;
    for (int v = 1; v <= 6; v++) if (c[v] > mc || (c[v] == mc && v > mv)) { mc = c[v]; mv = v; }
    return mv;
}
static void cpu_set_holds(void)
{
    int mv = cpu_mode_value();
    for (int i = 0; i < 5; i++) s_d[i].held = (s_d[i].value == mv);
}
static int cpu_best(void)
{
    const Player &p = s_pl[s_cur];
    int best = -1, bv = -1;
    for (int i = 0; i < N_CAT; i++) if (!p.used[i]) { int v = preview(&p, i); if (v > bv) { bv = v; best = i; } }
    if (bv <= 0) {                                                  // nothing scores: sacrifice a cheap box
        static const int sac[N_CAT] = { 0,1,11,8,10,9,7,6,2,3,4,5,12 };
        for (int k = 0; k < N_CAT; k++) if (!p.used[sac[k]]) { best = sac[k]; break; }
    }
    return best;
}
static void cpu_step(int64_t now)
{
    switch (s_cpu_step) {
        case 0: do_roll(now); s_cpu_step = 1; s_cpu_us = now + s_dur_us + 850000; break;
        case 1: cpu_set_holds(); do_roll(now); s_cpu_step = 2; s_cpu_us = now + s_dur_us + 850000; break;
        case 2: cpu_set_holds(); do_roll(now); s_cpu_step = 3; s_cpu_us = now + s_dur_us + 850000; break;
        default: lock_category(cpu_best(), now); break;
    }
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
    // SETUP: the two decorative dice idle-tumble so the menu feels alive
    if (s_phase == PH_SETUP) {
        s_d[0].yaw += dt * 1.1f; s_d[0].pitch += dt * 0.7f;
        s_d[1].yaw -= dt * 0.9f; s_d[1].pitch += dt * 0.6f;
    }
    // SCORE: ease the smooth-scroll toward the offset that keeps the selection on screen
    if (s_phase == PH_SCORE) {
        float tgt = (float)clampi(s_sel - 2, 0, N_CAT - SC_VIS);
        float k = dt * 12.0f; if (k > 1.0f) k = 1.0f;
        s_scroll += (tgt - s_scroll) * k;
    }

    if (s_anim) {
        float t = (float)(now - s_roll_us) / (float)s_dur_us; if (t > 1) t = 1;
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        for (int i = 0; i < 5; i++) {
            Die &dd = s_d[i];
            if (!dd.rolling) continue;
            dd.yaw = dd.y0 + (dd.ye - dd.y0) * e;
            dd.pitch = dd.p0 + (dd.pe - dd.p0) * e;
            dd.bank = dd.wob * (1.0f - t) * sinf(t * 9.0f);
        }
        if (t >= 1.0f) {
            s_anim = false; s_bgdirty = true; nucleo_app_request_draw();   // settle: repaint UI (new status) once
            for (int i = 0; i < 5; i++) if (s_d[i].rolling) { s_d[i].rolling = false; s_d[i].bank = 0; }
            int c[7] = {0}; for (int i = 0; i < 5; i++) c[s_d[i].value]++;
            bool yah = false; for (int v = 1; v <= 6; v++) if (c[v] == 5) yah = true;
            if (yah) { s_cele_start = now; s_cele_us = now + 2600000; spawn_burst(false); sfx_yahtzee(); }
            else {
                sfx_settle();
                // Auto-hold suggestion: show for 1.5 s which dice to keep for the best open category.
                // Only when rolls remain (no point suggesting if forced to score) and not CPU turn.
                if (!s_pl[s_cur].cpu && s_rolls > 0) {
                    int bidx = best_open_idx(&s_pl[s_cur]);
                    memset(s_suggest, 0, sizeof(s_suggest));
                    if (bidx >= 0) {
                        compute_contribution(bidx, s_suggest);
                        bool any = false; for (int i = 0; i < 5; i++) if (s_suggest[i]) { any = true; break; }
                        if (any) { s_suggest_us = now + 1500000; sfx_suggest(); }
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
        if (!s_armed) { if (m == NK_MOTION_STILL || m == NK_MOTION_HAND) { s_armed = true; sfx_arm(); s_bgdirty = true; nucleo_app_request_draw(); } }   // re-arm only when calm

        else if (nucleo_imu_energy() > 1.30f && now > s_cool_us && now > s_toast_us && now > s_cele_us) do_roll(now);
    }
    // CPU autopilot
    if (s_phase == PH_ROLL && s_pl[s_cur].cpu && now > s_toast_us && now >= s_cpu_us) cpu_step(now);
}

// ---------------------------------------------------------------- die rendering (3D + pips)
static bool face_visible(int v, float yaw, float pitch, float bank)
{
    fx3d::V3 nrm = { FACE[v - 1].n[0], FACE[v - 1].n[1], FACE[v - 1].n[2] };
    int dx, dy; float dz;
    fx3d::project(nrm, 0, 0, 1.0f, yaw, pitch, bank, &dx, &dy, &dz);
    return dz < -0.12f;
}
static void draw_pips(int v, float cx, float cy, float sc, float yaw, float pitch, float bank)
{
    const FaceFrame &f = FACE[v - 1];
    int pr = (int)(sc * 0.17f); if (pr < 2) pr = 2;                  // big, SOLID pips
    uint16_t base = (v == 1) ? COL_PIP1 : COL_PIP;
    for (int k = 0; k < PIPN[v]; k++) {
        float pu = PIPS[v][k][0] * 1.5f, pv = PIPS[v][k][1] * 1.5f;
        fx3d::V3 p = {
            f.n[0]*1.03f + f.u[0]*pu*0.42f + f.v[0]*pv*0.42f,
            f.n[1]*1.03f + f.u[1]*pu*0.42f + f.v[1]*pv*0.42f,
            f.n[2]*1.03f + f.u[2]*pu*0.42f + f.v[2]*pv*0.42f,
        };
        int px, py; float pz;
        fx3d::project(p, cx, cy, sc, yaw, pitch, bank, &px, &py, &pz);
        d.fillCircle(px, py, pr, base);                              // filled, no sheen (was reading as hollow)
    }
}
// cy is the GROUND centre; hop lifts the die (a clean toss arc) while its shadow stays on the felt and
// shrinks. show_pips=false during the fast part of a tumble so the pips don't strobe — they appear as it
// settles. Decorative/idle dice call it with the defaults (grounded, pips on).
static void draw_die(int i, int cx, int cy, int sc, bool show_pips = true, int hop = 0)
{
    Die &dd = s_d[i];
    int dcy = cy - hop;
    int shr = (int)(sc * 0.8f) - hop / 2; if (shr < 3) shr = 3;                          // shadow shrinks as it rises
    fx3d::dither_disc(cx, cy + (int)(sc * 1.06f), shr, fx3d::rgb(0, 0, 0));               // grounded contact shadow
    if (dd.held) fx3d::dither_disc(cx, dcy, (int)(sc * 1.5f), fx3d::rgb(24, 86, 48));     // held = soft green halo
    fx3d::draw_model_ex(CUBE, (float)cx, (float)dcy, (float)sc + 1.9f, dd.yaw, dd.pitch, dd.bank, fx3d::rgb(8,10,16), false);  // dark rim
    fx3d::draw_model_ex(CUBE, (float)cx, (float)dcy, (float)sc,        dd.yaw, dd.pitch, dd.bank, COL_IVORY, false);          // ivory body
    if (show_pips)
        for (int v = 1; v <= 6; v++)
            if (face_visible(v, dd.yaw, dd.pitch, dd.bank)) draw_pips(v, (float)cx, (float)dcy, (float)sc, dd.yaw, dd.pitch, dd.bank);
}

// ---------------------------------------------------------------- shared bits
#define TOPH 18
// Compact, dense top bar (reclaims the pixels the old title wasted): player chip + round + big total.
static void topbar(const char *mid)
{
    uint16_t pc = PCOL[s_cur];
    d.fillRect(0, 0, W, TOPH, INK);
    d.fillRoundRect(2, 1, 38, TOPH - 2, 3, pc);
    char tag[6]; snprintf(tag, sizeof(tag), s_pl[s_cur].cpu ? "C%d" : "P%d", s_cur + 1);
    d.setTextSize(2); d.setTextColor(INK, pc); d.setCursor(8, 2); d.print(tag);
    d.setTextSize(1); d.setTextColor(FG, INK); d.setCursor(46, 2); d.print(mid);
    char tb[12]; snprintf(tb, sizeof(tb), "%d", total(&s_pl[s_cur]));
    d.setTextSize(2); d.setTextColor(pc, INK); d.setCursor(W - (int)strlen(tb) * 12 - 4, 2); d.print(tb);
    d.drawFastHLine(0, TOPH - 1, W, fx3d::scl(pc, 170, 255));
}
static void draw_toast(void)
{
    int bw = 214, bx = (W - bw) / 2, by = 45, bh = 44;
    d.fillRoundRect(bx, by, bw, bh, 8, INK);
    d.drawRoundRect(bx, by, bw, bh, 8, s_toast_col);
    d.drawRoundRect(bx + 2, by + 2, bw - 4, bh - 4, 6, fx3d::scl(s_toast_col, 110, 255));
    d.setTextSize(2); d.setTextColor(s_toast_col, INK);
    int tw = (int)strlen(s_toast) * 12; d.setCursor(bx + (bw - tw) / 2, by + 7); d.print(s_toast);
    d.setTextSize(1); d.setTextColor(FG, INK);
    int tw2 = (int)strlen(s_toast2) * 6; d.setCursor(bx + (bw - tw2) / 2, by + 28); d.print(s_toast2);
}

// ---------------------------------------------------------------- ROLL view
static void draw_roll(void)
{
    int top = TOPH, bottom = H;
    int64_t tnow = esp_timer_get_time();
    bool human = !s_pl[s_cur].cpu;
    bool can_roll = human && !s_anim && s_rolls > 0;
    bool imu = nucleo_imu_present();
    uint16_t felt = fx3d::rgb(10, 26, 17);
    int cellH = 64, y0 = bottom - cellH - 1;

    if (tnow < s_cele_us || tnow < s_toast_us) s_bgdirty = true;          // overlays repaint cleanly over the felt

    // ---- static felt + UI: painted ONLY when it changed (no per-frame full clear = no flicker) ----
    if (s_bgdirty) {
        d.fillRect(0, top, W, bottom - top, felt);                       // clean flat felt (no jittery Mode-7 scroll)
        uint16_t frame = (can_roll && imu && s_armed) ? C_GREEN : fx3d::scl(COL_FELTG, 110, 255);
        d.drawRoundRect(2, top + 1, W - 4, bottom - top - 3, 6, frame);
        d.drawRoundRect(3, top + 2, W - 6, bottom - top - 5, 5, fx3d::scl(frame, 120, 255));

        char ln[18];
        if (!human)           snprintf(ln, sizeof(ln), "Gioca la CPU");
        else if (s_anim)      snprintf(ln, sizeof(ln), "...");
        else if (!s_rolled)   snprintf(ln, sizeof(ln), "Tocca a te");
        else if (s_rolls > 0) snprintf(ln, sizeof(ln), "Lanci: %d", s_rolls);
        else                  snprintf(ln, sizeof(ln), "Scegli casella");
        d.setTextSize(2); d.setTextColor(s_anim ? C_YELLOW : FG, felt);
        int lw = (int)strlen(ln) * 12; d.setCursor((W - lw) / 2, top + 6); d.print(ln);

        if (can_roll && imu) {
            int px = 40, pw = 160, py = top + 28, ph = 16;
            if (s_armed) {
                d.fillRoundRect(px, py, pw, ph, 6, C_GREEN);
                d.fillTriangle(px + 7, py + 8, px + 15, py + 3, px + 15, py + 13, INK);
                d.fillTriangle(px + pw - 7, py + 8, px + pw - 15, py + 3, px + pw - 15, py + 13, INK);
                d.setTextSize(1); d.setTextColor(INK, C_GREEN);
                const char *s = "SCUOTI PER LANCIARE"; int sw = (int)strlen(s) * 6;
                d.setCursor(px + (pw - sw) / 2, py + 5); d.print(s);
            } else {
                d.drawRoundRect(px, py, pw, ph, 6, MUTED);
                d.setTextSize(1); d.setTextColor(MUTED, felt);
                const char *s = "tieni fermo per caricare"; int sw = (int)strlen(s) * 6;
                d.setCursor(px + (pw - sw) / 2, py + 5); d.print(s);
            }
        } else if (human && !s_anim) {
            // Roll-count pill bar: 3 wide pills in the player's colour, one per remaining roll
            int pw = 56, ph = 12, pgap = 6, ptot = 3*pw + 2*pgap;
            int bpx = (W - ptot) / 2, bpy = top + 28;
            for (int r = 0; r < 3; r++) {
                int bx = bpx + r*(pw+pgap);
                if (r < s_rolls) {
                    d.fillRoundRect(bx, bpy, pw, ph, 5, PCOL[s_cur]);
                    d.fillRoundRect(bx+3, bpy+2, pw-6, ph/2-2, 3, fx3d::scl(0xFFFF, 55, 255)); // gloss
                    d.drawRoundRect(bx, bpy, pw, ph, 5, fx3d::scl(0xFFFF, 90, 255));
                } else {
                    d.fillRoundRect(bx, bpy, pw, ph, 5, fx3d::rgb(14,14,18));
                    d.drawRoundRect(bx, bpy, pw, ph, 5, fx3d::scl(MUTED, 55, 255));
                }
            }
        }

        const char *h = "";
        if (!human)           h = "Attendi il tuo turno";
        else if (s_anim)      h = "";
        else if (!s_rolled)   h = "INVIO o GO: lancia";
        else if (s_rolls > 0) h = "3-7 tieni  0=tutti  INVIO tira";
        else                  h = "INVIO o TAB: scegli la casella";
        if (h[0]) {
            d.setTextSize(1); d.setTextColor(C_YELLOW, felt);
            int hw = (int)strlen(h) * 6; d.setCursor((W - hw) / 2, y0 - 11); d.print(h);
        }
        s_bgdirty = false;
    }

    // ---- dice tray: repaint PER-CELL each frame over the persistent felt (the only animated region) ----
    int cy = y0 + 31, sc = 18;
    float t = 1.0f;                                                       // roll progress 0..1 (drives the toss arc)
    if (s_anim) { t = (float)(tnow - s_roll_us) / (float)s_dur_us; if (t > 1) t = 1; if (t < 0) t = 0; }
    for (int i = 0; i < 5; i++) {
        int cx = 24 + i * 48;
        bool cur = (i == s_diecur && s_rolled && human && !s_anim);
        bool rolling = s_anim && s_d[i].rolling;
        int  hop  = rolling ? (int)(sinf(t * (float)M_PI) * sc * 0.60f) : 0;   // clean vertical toss
        bool pips = !rolling || t > 0.55f;                               // hide pips during the fast spin -> no strobe
        int clrTop = rolling ? y0 - 14 : y0, clrH = rolling ? cellH + 14 : cellH;
        d.fillRect(cx - 24, clrTop, 48, clrH, felt);                     // clear this die's cell (taller while it hops)
        if (s_d[i].held)      { d.fillRoundRect(cx - 23, y0, 46, cellH, 6, fx3d::rgb(14, 46, 28)); d.drawRoundRect(cx - 23, y0, 46, cellH, 6, C_GREEN); }
        else if (cur)         { d.drawRoundRect(cx - 23, y0, 46, cellH, 6, C_YELLOW); d.drawRoundRect(cx - 22, y0 + 1, 44, cellH - 2, 6, fx3d::scl(C_YELLOW, 120, 255)); }
        // Auto-hold suggestion: pulsing cyan outline on suggested dice (clears on any key)
        if (tnow < s_suggest_us && !s_anim && s_suggest[i] && !s_d[i].held) {
            float pulse = 0.5f + 0.5f * sinf((float)tnow * 1e-6f * 9.0f);
            uint16_t sc2 = fx3d::scl(fx3d::rgb(72, 214, 255), (int)(pulse * 195) + 45, 255);
            d.drawRoundRect(cx - 23, y0, 46, cellH, 6, sc2);
            d.drawRoundRect(cx - 22, y0 + 1, 44, cellH - 2, 6, fx3d::scl(sc2, 100, 255));
        }
        draw_die(i, cx, cy, sc, pips, hop);
    }
}

// ---------------------------------------------------------------- SCORE view (scrollable scorecard)
// Category family palette — the upper numbers, the of-a-kinds, the straights, etc. each get their own
// hue so the card reads at a glance (smartwatch colour-coding).
static uint16_t cat_color(int idx)
{
    if (idx <= 5)               return fx3d::rgb(96, 176, 255);   // Uno..Sei  — blue (upper section)
    if (idx == 6 || idx == 7)   return fx3d::rgb(255, 168, 56);   // Tris/Poker — orange
    if (idx == 8)               return fx3d::rgb(255, 110, 196);  // Full House — pink
    if (idx == 9 || idx == 10)  return fx3d::rgb(104, 224, 132);  // Scale      — green
    if (idx == 11)              return fx3d::rgb(255, 214, 64);    // YAHTZEE    — gold
    return                              fx3d::rgb(176, 156, 232);  // Chance     — violet
}
// One readable single column: tall, airy size-2 rows, smooth-scrolled, each colour-tagged by family.
// Bonus strip on top + action band at the bottom are painted last so they clip the list overflow.
static void draw_score(void)
{
    Player &p = s_pl[s_cur];
    int top = TOPH, bm = best_open_idx(&p);
    d.fillRect(0, top, W, H - top, BG);

    const int VIS = SC_VIS, rowH = 16;                       // compact rows → room for mini dice strip below
    int listY = top + 15, listH = VIS * rowH;                // 33 .. 97
    int start = (int)s_scroll; float frac = s_scroll - (float)start;
    for (int r = 0; r <= VIS; r++) {
        int idx = start + r; if (idx < 0 || idx >= N_CAT) continue;
        int ry = listY + (int)((float)r * rowH - frac * rowH);
        if (ry + rowH < listY || ry > listY + listH) continue;
        bool used = p.used[idx], cursor = (idx == s_sel);
        uint16_t gc = cat_color(idx);
        if (cursor) {
            d.fillRoundRect(2, ry + 1, W - 8, rowH - 3, 4, fx3d::scl(gc, 60, 255));   // tinted by the family
            d.drawRoundRect(2, ry + 1, W - 8, rowH - 3, 4, gc);
        }
        d.fillRect(6, ry + 4, 4, rowH - 9, used ? fx3d::scl(gc, 80, 255) : gc);       // colour tag on the left
        uint16_t namec = cursor ? 0xFFFF : (used ? MUTED : gc);
        d.setTextSize(2); d.setTextColor(namec); d.setCursor(16, ry + 3); d.print(CAT_FULL[idx]);
        int val = used ? p.score[idx] : preview(&p, idx);
        char vb[6]; if (used && val == 0) { vb[0] = '-'; vb[1] = '-'; vb[2] = '\0'; } else snprintf(vb, sizeof(vb), "%d", val);
        uint16_t valc = used ? (val == 0 ? fx3d::rgb(140, 60, 60) : MUTED) : (idx == bm ? C_GREEN : 0xFFFF);
        d.setTextColor(valc); int vw = (int)strlen(vb) * 12; d.setCursor(W - 14 - vw, ry + 3); d.print(vb);
    }
    // scrollbar (position indicator)
    int denom = (N_CAT - VIS > 0) ? N_CAT - VIS : 1, thumbH = listH * VIS / N_CAT;
    d.fillRect(W - 3, listY, 2, listH, fx3d::scl(LINE, 120, 255));
    d.fillRect(W - 3, listY + (listH - thumbH) * start / denom, 2, thumbH, cat_color(s_sel));

    // ---- mini dice strip: 5 small dice with contribution highlighting ----
    {
        bool contrib[5] = {};
        if (s_rolled && !p.used[s_sel] && preview(&p, s_sel) > 0)
            compute_contribution(s_sel, contrib);
        uint16_t selc = cat_color(s_sel);
        int dw = 14, dgap = 3, dtot = 5*dw + 4*dgap;         // 82 px total
        int dx0 = (W - dtot) / 2;
        int dy = listY + listH + 2;                           // just below the list
        int64_t nt = esp_timer_get_time();
        bool fl = (nt < s_contrib_flash_us);                  // bright pop on category change
        for (int i = 0; i < 5; i++) {
            int dxi = dx0 + i*(dw+dgap);
            bool c = contrib[i];
            uint16_t fc = c ? (fl ? 0xFFFF : selc) : fx3d::rgb(22,22,28);
            uint16_t bc = c ? (fl ? selc : fx3d::scl(selc, 180, 255)) : fx3d::scl(MUTED, 50, 255);
            uint16_t tc = c ? (fl ? INK  : 0xFFFF) : MUTED;
            d.fillRoundRect(dxi, dy, dw, dw, 3, fc);
            d.drawRoundRect(dxi, dy, dw, dw, 3, bc);
            char nb[2]; nb[0] = '0' + s_d[i].value; nb[1] = '\0';
            d.setTextSize(1); d.setTextColor(tc);
            d.setCursor(dxi + (dw-6)/2, dy + (dw-8)/2); d.print(nb);
        }
    }

    // bonus strip (clips list overflow at the top)
    int sub = upper_sub(&p);
    d.fillRect(0, top, W, 14, INK);
    d.setTextSize(1);
    char bb[20]; snprintf(bb, sizeof(bb), "Superiori %d/63", sub);
    d.setTextColor(sub >= 63 ? C_GREEN : FG, INK); d.setCursor(4, top + 4); d.print(bb);
    int barx = 134, barw = W - barx - 26;
    d.fillRect(barx, top + 5, barw, 4, fx3d::scl(LINE, 200, 255));
    d.fillRect(barx, top + 5, barw * (sub > 63 ? 63 : sub) / 63, 4, sub >= 63 ? C_GREEN : C_YELLOW);
    d.setTextColor(sub >= 63 ? C_GREEN : MUTED, INK); d.setCursor(W - 22, top + 4); d.print(sub >= 63 ? "+35" : "->35");
    d.drawFastHLine(0, top + 14, W, fx3d::scl(PCOL[s_cur], 150, 255));

    // action band (clips list overflow at the bottom): coloured by the selected family
    int idx = s_sel; bool isbest = (!p.used[idx] && idx == bm);
    uint16_t accent = isbest ? C_GREEN : cat_color(idx);
    int bandY = H - 17;
    d.fillRect(0, bandY, W, 17, INK);
    d.drawFastHLine(0, bandY, W, accent);
    char fb[28];
    if (p.used[idx]) snprintf(fb, sizeof(fb), p.score[idx] == 0 ? "USATA  %s = 0" : "OK  %s = %d", CAT_SHORT[idx], p.score[idx]);
    else             snprintf(fb, sizeof(fb), "INVIO: %s +%d", CAT_SHORT[idx], preview(&p, idx));
    d.setTextSize(1); d.setTextColor(p.used[idx] ? MUTED : accent, INK); d.setCursor(4, bandY + 5); d.print(fb);
    d.setTextColor(MUTED, INK); d.setCursor(W - 8 * 6 - 4, bandY + 5); d.print("TAB dadi");
}

// ---------------------------------------------------------------- SETUP / OVER
static void draw_setup(void)
{
    int top = nucleo_app_content_top();
    d.fillRect(0, top, W, H - top, BG);
    draw_die(0, 22, top + 24, 15); draw_die(1, W - 22, top + 24, 15);    // decorative dice flanking the title

    // title — big and bold
    d.setTextSize(3); const char *t = "YAHTZEE"; int tw = (int)strlen(t) * 18, tx = (W - tw) / 2;
    d.setTextColor(fx3d::rgb(8, 8, 8)); d.setCursor(tx + 2, top + 10); d.print(t);             // drop shadow
    d.setTextColor(C_YELLOW, BG);       d.setCursor(tx,     top + 8);  d.print(t);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    const char *s = "5 dadi - 3 lanci - 13 caselle"; d.setCursor((W - (int)strlen(s) * 6) / 2, top + 40); d.print(s);

    // player-count steppers — taller cards, big value digit
    const char *labels[2] = { "Umani", "CPU" };
    int vals[2] = { s_humans, s_cpus };
    for (int i = 0; i < 2; i++) {
        bool sel = (i == s_setsel);
        int bw = 200, bx = (W - bw) / 2, bh = 28, ry = top + 54 + i * 32;
        uint16_t fill = sel ? fx3d::rgb(54, 54, 24) : INK;
        d.fillRoundRect(bx, ry, bw, bh, 6, fill);
        d.drawRoundRect(bx, ry, bw, bh, 6, sel ? C_YELLOW : LINE);
        d.setTextSize(2); d.setTextColor(sel ? C_YELLOW : FG, fill);
        d.setCursor(bx + 12, ry + 7); d.print(labels[i]);
        if (sel) { d.setCursor(bx + bw - 70, ry + 7); d.print("<"); d.setCursor(bx + bw - 16, ry + 7); d.print(">"); }
        char vb[4]; snprintf(vb, sizeof(vb), "%d", vals[i]);
        d.setTextSize(3); d.setTextColor(sel ? C_YELLOW : FG, fill);
        d.setCursor(bx + bw - 48, ry + 3); d.print(vb);
    }
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    const char *h = "su/giu scegli   < >  regola   INVIO inizia";
    d.setCursor((W - (int)strlen(h) * 6) / 2, H - 12); d.print(h);
}
static void draw_over(void)
{
    int top = nucleo_app_content_top();
    d.fillRect(0, top, W, H - top, BG);
    int ord[MAXP]; for (int i = 0; i < s_np; i++) ord[i] = i;
    for (int a = 0; a < s_np; a++) for (int b = a + 1; b < s_np; b++)
        if (total(&s_pl[ord[b]]) > total(&s_pl[ord[a]])) { int t = ord[a]; ord[a] = ord[b]; ord[b] = t; }
    d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
    char ttl[20]; snprintf(ttl, sizeof(ttl), "Vince P%d!", ord[0] + 1);
    d.setCursor((W - (int)strlen(ttl) * 12) / 2, top + 4); d.print(ttl);

    int rh = s_np <= 3 ? 26 : 22;
    for (int r = 0; r < s_np; r++) {
        int pi = ord[r], y = top + 28 + r * rh;
        uint16_t med = r == 0 ? fx3d::rgb(255, 205, 40) : r == 1 ? fx3d::rgb(200, 200, 215)
                     : r == 2 ? fx3d::rgb(200, 120, 50) : MUTED;
        if (r == 0) { d.fillRoundRect(4, y - 2, W - 8, rh - 2, 5, fx3d::rgb(50, 44, 14)); d.drawRoundRect(4, y - 2, W - 8, rh - 2, 5, C_YELLOW); }
        d.fillCircle(20, y + 8, 9, med); d.drawCircle(20, y + 8, 9, INK);            // medal
        d.setTextSize(2); d.setTextColor(INK, med);
        char rk[2]; snprintf(rk, sizeof(rk), "%d", r + 1); d.setCursor(15, y + 1); d.print(rk);
        char b[24]; snprintf(b, sizeof(b), "P%d%s  %d", pi + 1, s_pl[pi].cpu ? " CPU" : "", total(&s_pl[pi]));
        d.setTextSize(2); d.setTextColor(r == 0 ? C_YELLOW : PCOL[pi], r == 0 ? fx3d::rgb(50, 44, 14) : BG);
        d.setCursor(38, y + 1); d.print(b);
    }
    // game-over confetti rain
    for (int i = 0; i < s_npart; i++) {
        Part &q = s_part[i]; if (q.life == 0) continue;
        d.fillRect((int)q.x - 1, (int)q.y - 1, 3, 3, fx3d::scl(q.col, 255 * q.life / (q.lmax ? q.lmax : 1), 255));
    }
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, H - 11); d.print("INVIO: nuova partita   ESC: esci");
}

// Spectacular full-screen YAHTZEE overlay: rotating golden rays + expanding shockwaves + confetti +
// a popping, glowing, colour-cycling "YAHTZEE!". Composited over the whole frame (topbar included).
static void draw_celebration(int64_t now)
{
    float p = (float)(now - s_cele_start) * (1.0f / 2600000.0f); if (p > 1.0f) p = 1.0f;
    int cx = W / 2, cy = H / 2;
    float spin = (float)now * 1e-6f * 2.5f;

    for (int k = 0; k < 12; k++) {                                   // rotating golden light rays
        float a = spin + k * (TWO_PI / 12.0f);
        int x2 = cx + (int)(cosf(a) * 220), y2 = cy + (int)(sinf(a) * 220);
        d.drawLine(cx, cy, x2, y2, fx3d::scl((k & 1) ? 0xFFFF : C_YELLOW, (int)(110 * (1.0f - p)) + 30, 255));
    }
    for (int w = 0; w < 3; w++) {                                    // expanding shockwave rings
        float rp = p * 1.5f - w * 0.16f; if (rp <= 0.0f || rp >= 1.0f) continue;
        int rad = (int)(rp * 175);
        uint16_t cc = fx3d::scl((w & 1) ? 0xFFFF : C_YELLOW, (int)(255 * (1.0f - rp)), 255);
        d.drawCircle(cx, cy, rad, cc); d.drawCircle(cx, cy, rad - 1, fx3d::scl(cc, 150, 255));
    }
    for (int i = 0; i < s_npart; i++) {                              // confetti
        Part &q = s_part[i]; if (q.life == 0) continue;
        d.fillRect((int)q.x - 1, (int)q.y - 1, 3, 3, fx3d::scl(q.col, 255 * q.life / (q.lmax ? q.lmax : 1), 255));
    }
    int sz = p < 0.10f ? 1 : (p < 0.18f ? 2 : 3);                    // pop-in scale
    const char *b = "YAHTZEE!";
    int tw = (int)strlen(b) * sz * 6, tx = (W - tw) / 2;
    int ty = cy - sz * 4 + (int)(sinf((float)now * 1e-6f * 8.0f) * 2.0f);   // gentle bob
    d.setTextSize(sz);
    static const int ox[8] = { -2,2,-2,2,-1,1,0,0 }, oy[8] = { -2,-2,2,2,0,0,-2,2 };
    for (int o = 0; o < 8; o++) { d.setTextColor(fx3d::rgb(90, 60, 0)); d.setCursor(tx + ox[o], ty + oy[o]); d.print(b); }   // glow
    int ph = (now / 110) % 3;
    d.setTextColor(ph == 0 ? C_YELLOW : (ph == 1 ? 0xFFFF : fx3d::rgb(255, 120, 40)));
    d.setCursor(tx, ty); d.print(b);
    if (p > 0.22f) {
        d.setTextSize(1); const char *s = "CINQUE UGUALI!"; int sw = (int)strlen(s) * 6;
        d.setTextColor(0xFFFF); d.setCursor((W - sw) / 2, ty + sz * 8 + 5); d.print(s);
    }
}

static void draw(void)
{
    char mid[16]; snprintf(mid, sizeof(mid), "Giro %d/13", s_turn + 1);
    switch (s_phase) {
        case PH_SETUP: draw_setup(); break;
        case PH_OVER:  draw_over();  break;
        case PH_SCORE: topbar(mid); draw_score(); break;
        default:       topbar(mid); draw_roll(); break;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_toast_us && s_phase != PH_SETUP && !(s_phase == PH_ROLL && now < s_cele_us)) draw_toast();
    if (s_phase == PH_ROLL && now < s_cele_us) draw_celebration(now);
}

// ---------------------------------------------------------------- input
static void start_game(void)
{
    int n = s_humans + s_cpus; if (n < 1) n = 1; if (n > MAXP) n = MAXP;
    s_np = n;
    for (int i = 0; i < MAXP; i++) { memset(&s_pl[i], 0, sizeof(Player)); s_pl[i].cpu = (i >= s_humans); }
    s_cur = 0; s_turn = 0; s_toast_us = 0; s_npart = 0; s_cele_us = 0;   // clear any leftover game-over confetti
    start_turn(esp_timer_get_time());
}

static bool poll(void)
{
    int64_t now = esp_timer_get_time();
    float dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.02f;
    if (dt > 0.05f) dt = 0.05f;
    s_last_us = now;
    sim(dt);
    // Keep repainting while anything is moving: rolling dice, toast, celebration/confetti, the living
    // ROLL felt, the SETUP idle-tumble, and the SCORE smooth-scroll. Otherwise repaint on key only.
    bool scrolling = (s_phase == PH_SCORE) &&
                     (fabsf(s_scroll - (float)clampi(s_sel - 2, 0, N_CAT - SC_VIS)) > 0.02f);
    // Repaint ONLY while something actually moves (dice tumble, toast, confetti, score scroll, the CPU's
    // turn). Idle ROLL/SETUP no longer force a redraw every frame — that was the flicker. sim() still runs
    // every tick (line ~1363 calls poll unconditionally), so the IMU shake gesture keeps working.
    return s_anim || now < s_toast_us || now < s_cele_us || scrolling
        || (s_phase == PH_ROLL && s_pl[s_cur].cpu)
        || (s_phase == PH_ROLL && now < s_suggest_us)     // pulsing suggestion outline
        || (s_phase == PH_SCORE && now < s_contrib_flash_us); // mini dice flash
}
static void ptt(bool on)
{
    if (!on) return;
    int64_t now = esp_timer_get_time();
    if (s_phase == PH_ROLL && !s_pl[s_cur].cpu && !s_anim && now > s_toast_us && now > s_cool_us && s_rolls > 0) { do_roll(now); nucleo_app_request_draw(); }
}
static void on_key(int key, char ch)
{
    int64_t now = esp_timer_get_time();
    s_suggest_us = 0;                                                              // any key clears the auto-hold suggestion
    s_bgdirty = true;                                                              // any input repaints the ROLL static UI
    if (now < s_toast_us) { s_toast_us = 0; nucleo_app_request_draw(); return; }   // any key dismisses a toast
    if (s_pl[s_cur].cpu && s_phase == PH_ROLL) return;                              // hands off during CPU turn

    if (s_phase == PH_SETUP) {
        if (key == NK_UP || key == NK_DOWN) s_setsel ^= 1;
        else if (key == NK_RIGHT) { if (s_setsel == 0) { if (s_humans < 4) s_humans++; } else { if (s_cpus < 3) s_cpus++; } }
        else if (key == NK_ENTER) { start_game(); }
        if (s_humans + s_cpus > MAXP) { if (s_setsel == 0) s_humans = MAXP - s_cpus; else s_cpus = MAXP - s_humans; }
        nucleo_app_request_draw();
        return;
    }
    if (s_phase == PH_OVER) {
        if (key == NK_ENTER) { s_phase = PH_SETUP; nucleo_app_request_draw(); }
        return;
    }
    if (s_phase == PH_SCORE) {
        Player &p = s_pl[s_cur];
        if (key == NK_UP)        { if (s_sel > 0)         { s_sel--; sfx_move(); s_contrib_flash_us = now + 350000; } }
        else if (key == NK_DOWN) { if (s_sel < N_CAT - 1) { s_sel++; sfx_move(); s_contrib_flash_us = now + 350000; } }
        else if (key == NK_ENTER){ if (!p.used[s_sel]) lock_category(s_sel, now); else sfx_deny(); }
        nucleo_app_request_draw();
        return;
    }
    // PH_ROLL
    if (s_anim) return;
    if (ch >= '3' && ch <= '7') {                                    // poker-style direct hold: keys 3 4 5 6 7 = dice 1..5
        if (s_rolled) { int di = ch - '3'; s_diecur = di; bool &hd = s_d[di].held; hd = !hd; sfx_hold(hd); nucleo_app_request_draw(); }
        return;
    }
    if (ch == '0') {                                                 // hold-all / release-all
        if (s_rolled && !s_anim) {
            bool anyFree = false;
            for (int i = 0; i < 5; i++) if (!s_d[i].held) { anyFree = true; break; }
            for (int i = 0; i < 5; i++) s_d[i].held = anyFree;
            sfx_holdall();
            nucleo_app_request_draw();
        }
        return;
    }
    if (ch == ' ' || ch == 'z' || ch == 'Z') {                       // SPACE or Z: hold/release the selected die
        if (s_rolled) { bool &hd = s_d[s_diecur].held; hd = !hd; sfx_hold(hd); nucleo_app_request_draw(); }
        return;
    }
    if (key == NK_ENTER) {
        if (s_rolls > 0) { if (now > s_cool_us) do_roll(now); }       // roll — respects the inter-roll pause
        else if (s_rolled) goto_score();
        nucleo_app_request_draw();
        return;
    }
    if (key == NK_RIGHT) { if (s_diecur < 4) { s_diecur++; sfx_move(); } nucleo_app_request_draw(); }
    else if (key == NK_DOWN) { if (s_rolled) { goto_score(); nucleo_app_request_draw(); } }
}
// LEFT + BACK route here. LEFT = move/column; BACK = step back (score->roll) or exit.
static bool back(int key)
{
    int64_t now = esp_timer_get_time();
    if (now < s_toast_us) { s_toast_us = 0; nucleo_app_request_draw(); return true; }
    if (s_phase == PH_SETUP && key == NK_LEFT) {                                   // LEFT = decrement (RIGHT = increment)
        if (s_setsel == 0) { if (s_humans > 0 && s_humans + s_cpus > 1) s_humans--; }
        else               { if (s_cpus  > 0 && s_humans + s_cpus > 1) s_cpus--;  }
        nucleo_app_request_draw(); return true;
    }
    if (s_phase == PH_SCORE) {                                                     // LEFT or BACK -> back to rolling
        s_phase = PH_ROLL; s_bgdirty = true; sfx_move(); nucleo_app_request_draw(); return true;
    }
    if (s_phase == PH_ROLL && key == NK_LEFT) { if (s_diecur > 0) { s_diecur--; sfx_move(); } nucleo_app_request_draw(); return true; }
    return false;                                                                  // BACK exits the app
}
static void tab(void)
{
    if (s_phase == PH_ROLL && s_rolled) { goto_score(); nucleo_app_request_draw(); }
    else if (s_phase == PH_SCORE) { s_phase = PH_ROLL; s_bgdirty = true; nucleo_app_request_draw(); }
}

static void enter(void)
{
    COL_IVORY = fx3d::rgb(233, 227, 208);
    COL_PIP   = fx3d::rgb(30, 26, 38);
    COL_PIP1  = fx3d::rgb(196, 44, 44);
    COL_FELT  = fx3d::rgb(28, 96, 56);
    COL_FELTG = fx3d::rgb(46, 150, 88);
    // (The shared 32 KB canvas is re-acquired centrally by open_app_def() after on_enter for every buffered
    //  app — see ANTI-FLICKER.md technique 1 — so no per-app acquire is needed here anymore.)
    game_sfx_ensure(&SFX);                                           // build the SD WAV cache once (cheap on later launches)
    s_phase = PH_SETUP; s_setsel = 0; s_last_us = 0; s_toast_us = 0; s_cele_us = 0; s_anim = false;
    s_npart = 0; s_cele_start = 0; s_sel = 0; s_scroll = 0;
    s_suggest_us = 0; s_contrib_flash_us = 0; memset(s_suggest, 0, sizeof(s_suggest));
    reset_dice_idle();
    nucleo_app_set_fullscreen(true);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_ptt_handler(ptt);
    nucleo_app_request_draw();
}

static void on_exit(void) { nucleo_audio_stop(); }

extern "C" void nucleo_register_yahtzee(void)
{
    static const nucleo_app_def_t app = {
        "yahtzee", "Yahtzee", "Games", "Yahtzee a turni (1-4 + CPU), dadi 3D",
        'Y', C_YELLOW, enter, on_key, nullptr, draw, on_exit,
        NX_NET_APP   // dedicate RAM + free the shared I2S/mic line so the chiptune SFX reliably play
    };
    nucleo_app_register(&app);
}
