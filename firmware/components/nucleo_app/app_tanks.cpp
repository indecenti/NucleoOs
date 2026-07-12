// app_tanks.cpp — NucleoOS "NUCLEO TANKS": turn-based artillery (Worms / Pocket Tanks lineage) with a
// scrollable destructible battlefield, cinematic camera, hybrid 2D+3D look, biomes, 29 weapons, and a
// 16-bit SNES audio/visual treatment. Category "Games". Plays vs CPU (best-of series) or 1v1 over ESP-NOW.
//
// This is the "polish v3" build: high-contrast complementary biome palettes (sky vs ground always read),
// a multi-layer PARALLAX backdrop (the old Mode-7 sky grid is gone), a comet projectile that pops on any
// background, longer cannon, layered terrain strata, per-weapon explosions with 3D shatter debris + a
// nuke mushroom + palette-cycling magma scars, a TAB settings overlay (audio/lang/difficulty/manual-aim/
// aim-help + key guide), E/S/A/D free camera, optional manual numeric aim entry, a fight-game-style HUD
// with side HP gauges, polyphonic SNES cues (a triumphant win fanfare, defeat cadence, menu motif...),
// turn-change wipe, and a celebratory victory screen.
//
// Constraints: exclusive_flags = NX_NET_APP; ALL state static (NO heap); buffered d.* (one blit/frame);
// fx3d models const (RAM 0); audio pre-synthesized to SD WAV. Bilingual IT/EN. ASCII only. Never name a
// local `d`. Engine routes NK_LEFT/NK_BACK to the back handler; everything else to on_key; TAB to the
// tab handler.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "nucleo_fx3d.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
#include "nucleo_pnet.h"
#include "esp_timer.h"
#include "esp_random.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================ color helpers ==================================
static inline uint16_t rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t mix(uint16_t a, uint16_t b, int t) {
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar + (br - ar) * t / 256) & 31) << 11) |
                      (((ag + (bg - ag) * t / 256) & 63) << 5) |
                       ((ab + (bb - ab) * t / 256) & 31));
}
#define COL_WHITE 0xFFFF
#define COL_P0    rgb(96, 212, 236)     // player 1 / left  (cyan)
#define COL_P1    rgb(255, 150, 80)     // player 2 / right (orange)
#define COL_DIM   rgb(96, 106, 140)
#define COL_MUT   rgb(170, 180, 205)
#define COL_GOLD  rgb(255, 200, 80)
#define COL_RED   rgb(244, 84, 72)
#define COL_GREEN rgb(120, 232, 142)
#define COL_CYAN  rgb(120, 220, 255)
#define COL_INK   rgb(8, 10, 18)
// projectile — outline+glow+core pops on snow-white, night-black, magenta sky, teal ground alike
#define PROJ_GLOW    rgb(255, 176, 40)
#define PROJ_CORE    rgb(255, 255, 255)
// HUD
#define HUD_BG    COL_INK
#define HUD_TEXT  rgb(244, 248, 255)
#define HUD_TEXT2 rgb(150, 166, 198)

// ============================ world geometry =================================
#define WW   720
#define GTOP 26
#define TANK_W 16
#define TANK_H 9
#define BARREL 18                       // cannon length (draw + spawn must match)
// Elevation range. >90 deg makes cos(angle) negative, so the shell flips to the OPPOSITE horizontal
// direction: the player can lob BACKWARD too, covering both sides (5 fwd-horiz .. 90 up .. 175 back-horiz).
#define ELEV_MIN 5
#define ELEV_MAX 175

// ============================ fx3d cube (RAM 0) ==============================
static const fx3d::V3 CUBEV[8] = {
    {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
};
static const fx3d::Tri CUBET[12] = {
    {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
    {1,5,6},{1,6,2},{2,6,7},{2,7,3},{3,7,4},{3,4,0}
};
static const fx3d::Model CUBE = { CUBEV, 8, CUBET, 12 };

// ============================ weapons ========================================
enum { WB_BLAST = 0, WB_DIG, WB_CLUSTER, WB_RAIN, WB_MIRV, WB_ROLLER, WB_TELE,
       WB_SHIELD, WB_BEAM, WB_DRILL, WB_WALL, WB_HOMING, WB_NAPALM, WB_UFO, WB_BOUNCE };
struct Weap { const char *it, *en; int beh, crater, dmg, count, ammo; uint16_t col; };
static const Weap WEAPS[] = {
    { "Standard",     "Standard",  WB_BLAST,   12, 26, 1, -1, rgb(255,230,120) }, // 0
    { "Bomba",        "Big Bomb",  WB_BLAST,   20, 44, 1,  4, rgb(255,150, 60) }, // 1  strong lob: ammo trimmed to its Mortar/Heavy tier (was an over-generous 6)
    { "Scavatore",    "Digger",    WB_DIG,     30, 12, 1,  5, rgb(190,150,100) }, // 2
    { "Grappolo",     "Cluster",   WB_CLUSTER,  8, 11, 8,  4, rgb(180,255,120) }, // 3  many small bomblets, wide ground carpet
    { "Pioggia",      "Rain",      WB_RAIN,     8, 14, 6,  3, rgb(120,200,255) }, // 4
    { "Sciame",       "MIRV",      WB_MIRV,    13, 26, 3,  3, rgb(220,140,255) }, // 5  few heavy warheads, steep airburst rain
    { "Rimbalzo",     "Roller",    WB_ROLLER,  14, 30, 1,  4, rgb(120,255,210) }, // 6
    { "Astronave",    "Alien Ship",WB_UFO,     10, 48, 1,  2, rgb(140,255,180) }, // 7  near-guaranteed overhead laser if aimed right -> rarer (ammo 3->2)
    { "Atomica",      "Nuke",      WB_BLAST,   40, 80, 1,  1, rgb(255, 90, 70) }, // 8
    { "Scudo Bolla",  "Bubble",    WB_SHIELD,   0,  0, 1,  2, rgb(120,200,255) }, // 9
    { "Riparazione",  "Med-Kit",   WB_SHIELD,   0,  0, 1,  1, rgb(120,255,150) }, // 10
    { "Raggio",       "Beam",      WB_BEAM,     6, 34, 1,  3, rgb(255, 90,160) }, // 11
    { "Trivella",     "Drill",     WB_DRILL,   26, 30, 1,  3, rgb(170,120, 70) }, // 12
    { "Muraglia",     "Builder",   WB_WALL,     0,  0, 1,  3, rgb(150,255,170) }, // 13
    { "Cercatore",    "Homing",    WB_HOMING,  11, 24, 1,  3, rgb(255,230, 90) }, // 14
    { "Tripletta",    "Tripler",   WB_BLAST,    8, 14, 3,  4, rgb(255,150,210) }, // 15
    { "Propulsori",   "Jump-Jets", WB_TELE,     0,  0, 1,  3, rgb(120,255,255) }, // 16
    { "Napalm",       "Napalm",    WB_NAPALM,  12, 14, 1,  3, rgb(255,120, 40) }, // 17
    { "Cecchino",     "Sniper",    WB_BLAST,    7, 40, 1,  2, rgb(230,240,255) }, // 18
    { "Granata",      "Grenade",   WB_BOUNCE,  13, 30, 1,  4, rgb(180,230,120) }, // 19  bounces along terrain, then blows
    { "Sventaglio",   "Spread",    WB_BLAST,    8, 12, 5,  3, rgb(255,186,110) }, // 20  five-shot muzzle fan
    { "Bordata",      "Salvo",     WB_BLAST,   12, 20, 2,  4, rgb(255,140,150) }, // 21  twin volley
    { "Mortaio",      "Mortar",    WB_BLAST,   16, 34, 1,  3, rgb(200,160,255) }, // 22  heavy single lob
    { "Massiccia",    "Heavy",     WB_BLAST,   26, 62, 1,  2, rgb(255,120, 60) }, // 23  big crater, big damage
    { "Talpa",        "Mole",      WB_DRILL,   30, 34, 1,  2, rgb(150,110, 60) }, // 24  bores deep, blasts under
    { "Palla",        "Bowling",   WB_ROLLER,  17, 34, 1,  2, rgb(120,255,190) }, // 25  HEAVY ball: bigger crater + more damage, fewer shots -> a distinct role from the light Roller (6)
    { "Incendio",     "Firestorm", WB_NAPALM,  13, 12, 1,  2, rgb(255,100, 30) }, // 26  wider + longer burning patch than Napalm (see resolve_impact)
    { "Grandine",     "Hail",      WB_CLUSTER,  6,  8,10,  3, rgb(150,220,255) }, // 27  dense icy carpet
    { "Sisma",        "Quake",     WB_DIG,     34, 16, 1,  2, rgb(190,150,100) }, // 28  huge terrain shove + quake
};
#define NWEAP ((int)(sizeof(WEAPS)/sizeof(WEAPS[0])))
enum { MED_WP = 10, JETS_WP = 16, SNIPE_WP = 18 };   // indices needing fire-time special-casing
// Per-weapon audio cues (index = weapon idx). Fire 20-28/40-46, impact 30-38/47. Utility hit = unused.
static const int fire_sfx[NWEAP] = { 20,21,22,23,24,25,26,27,28, 40,42,44,45,46,20,23,43,21,20,
                                     21,23,21,21,28,45,26,46,24,22 };   // 19..28 new weapons
static const int hit_sfx [NWEAP] = { 30,31,32,33,34,35,36,37,38, 30,30,30,38,30,30,33,30,47,30,
                                     6, 33,31,31,38,45,36,47,33,32 };   // 19..28 new weapons

// ============================ state ==========================================
enum { ST_MENU = 0, ST_PLAY, ST_OVER, ST_HELP, ST_SCORES, ST_OPT, ST_HOST, ST_BROWSE, ST_LOADOUT };
enum { TP_TURN = 0, TP_AIM, TP_FIRE, TP_SETTLE, TP_OVER };
enum { ATM_DAY = 0, ATM_SUNSET, ATM_EVENING, ATM_NIGHT, ATM_RAIN, ATM_N };   // time-of-day / weather skies
enum { MODE_AI = 0, MODE_HOST, MODE_GUEST };

// ---- multiplayer (active-authoritative over nucleo_pnet) ----
#define TK_M0 'T'
#define TK_M1 'K'
#define TK_VER 1
enum { TK_HELLO = 1, TK_JOIN, TK_WELCOME, TK_START, TK_AIM, TK_RESULT, TK_ACK, TK_BYE, TK_FIRE };
static int     s_mode;                  // MODE_AI / MODE_HOST / MODE_GUEST
static int     s_seat;                  // 0 = host/left, 1 = guest/right
static uint8_t s_peer[6];
static bool    s_haspeer, s_peerleft;
static int64_t s_last_rx, s_last_hello, s_last_aim, s_join_t0;
static bool    s_join_pending;
static uint32_t s_resseq;               // result sequence (dedup)
struct Room { uint8_t mac[6]; char name[22]; int64_t seen; };
#define NROOM 6
static Room    s_rooms[NROOM];
static int     s_nroom, s_rsel;
static bool    s_guest_in;              // host: a guest joined my room
static bool    s_welcomed;              // guest: the host acknowledged my JOIN (in the room, waiting for START)
static char    s_peer_name[22];
// reliable outbound (one in flight: turn-based)
static uint8_t s_rel[120]; static int s_rel_len; static int64_t s_rel_next; static bool s_rel_on;
static int     s_last_weap;             // weapon fired this turn (echoed in RESULT)

static int    s_screen, s_msel, s_diff = 1;
static int    g_lang = 0, g_audio = 1, g_manual = 0, g_aimhelp = 0, g_windvar = 0;   // trajectory OFF by default; windvar re-rolls wind each turn (vs CPU only)
static int64_t s_now, s_last, s_frame;
static unsigned s_anim;

// terrain / theme
static uint8_t s_h[WW];
static uint8_t s_dug[WW];   // 1 = column was cratered/raised -> show exposed dirt, no grass rim
static int     s_atmo;                 // current atmosphere (time of day / weather)
static uint16_t th_skyT, th_skyM, th_skyB, th_ground, th_ground2, th_rim, th_accent, th_glow, th_sun, th_cloud;
static int     s_starN, s_sunR, s_sunX, s_sunY; static bool s_moon, s_rain;
static bool    th_night;
static uint32_t s_seed;
static int     s_wind;

// tanks
struct Tank { float x, y; int hp; int elev; int power; int weap; int ammo[NWEAP]; bool dead; int shield; };
static Tank   s_tk[2];
static int    s_active, s_wins[2], s_draw_flag;
#define SERIES_TGT 3   // vs-CPU is a best-of series: first tank to win SERIES_TGT rounds takes the match
// --- juicy bonus attribution (vs CPU) ---
static int      s_shooter;       // tank that fired the current shot (whose hits the bonus belongs to)
static int      s_shot_hits;     // damaging hits this shot landed on the FOE (2+ frags = DOPPIETTA)
static bool     s_shot_long;     // long-shot bonus still unclaimed this shot
static unsigned s_bonus;         // player (P0) bonus points this round, folded into the leaderboard score

// camera
static float  s_cam, s_camtgt, s_camfree, s_camY;

// turn / phase
static int    s_phase, s_turn_t, s_aiwait;
static bool   s_ai_planned;
// multiplayer turn clock: each player gets TURN_SECS to choose+fire; at 0 the shot auto-launches ("VIA").
#define TURN_SECS 45
static int64_t s_aim_deadline;         // now_ms() by which the local active player must have fired (net only)
static int64_t s_yt_t;                  // "YOUR TURN" banner visible until this time (net: our turn just began)
static int     s_last_tick;            // last whole-second announced (drives the 5..1 ticks + VIA cue)

// settings overlay
static int    s_optsel, s_optguide, s_opt_from = ST_MENU;

// Pocket-Tanks-style loadout: the player picks 10 weapons from the full arsenal before a vs-CPU match.
// s_ldpick = which weapons P0 has chosen; s_ldsel = cursor in the picker list. Each tank's chosen 10 are
// flattened into s_slot[] at match start (fixed order) so the number keys / quick-bar map to real slots.
#define LOADOUT_N 10
static bool    s_ldpick[NWEAP];
static int     s_ldsel;
static int     s_ld_go;                  // where ENTER leaves the picker: 0 = vs CPU, 1 = host, 2 = join
static uint8_t s_slot[2][LOADOUT_N];
static int     s_nslot[2];
static int  ld_count(void) { int n = 0; for (int w = 0; w < NWEAP; w++) if (s_ldpick[w]) n++; return n; }

// manual aim entry
static int    s_entry_field;
static char   s_entry_ang[4], s_entry_pow[4];

// projectiles
struct Proj { bool on; float x, y, vx, vy; int wp, beh, owner; bool child, rolling, split; int fuse; int dig; int bnc; };
#define NPROJ 14
static Proj   s_pr[NPROJ];
// napalm burning patches + beam FX + impact-dwell camera
#define NFIRE 3
struct Fire { bool on; int x, w, ticks; };
static Fire   s_fire[NFIRE];
static int    s_beam_t, s_beam_x0, s_beam_y0, s_beam_x1, s_beam_y1;
static int    s_dwell_t; static float s_dwell_x, s_dwell_y;
// Alien saucer weapon (single, static): launched from the barrel along the shot arc; if the aim would
// hit the foe it peels off to hover directly above and blasts it with a death-ray from above.
enum { UF_FLYIN = 0, UF_MOVE, UF_HOVER, UF_LASER, UF_LEAVE, UF_MISS };
struct Ufo { bool on; int phase, t; float x, y, vx, vy, hx, hy; int owner, foe, wp; bool hit; };
static Ufo    s_ufo;
static int    s_ai_tgtE, s_ai_tgtP;
// terrain ops recorded during the active player's shot (packaged into the multiplayer RESULT)
#define NOPS 8
struct Op { uint8_t kind; int16_t x; uint8_t y, r; };   // kind 0 = crater (x,y,r); 1 = raise (x=cx, y=height, r=radius)
static Op   s_ops[NOPS]; static int s_nops; static bool s_rec;
static bool s_spectate;   // net passive: locally re-simulating the opponent's shot so the camera can follow it

// FX pools
#define NSPK 36
struct Spark { float x, y, vx, vy; int life, max; uint16_t col; };
static Spark  s_spk[NSPK];
#define NSHAT 4
struct Shat { bool on; float x, y, sc, yaw; int t, dur; uint16_t col; };
static Shat   s_sh[NSHAT];
#define NRING 6
struct Ring { bool on; float x, y, r, max; int life, lmax; uint16_t col; };
static Ring   s_rg[NRING];
#define NDMG 5
struct Dpop { bool on; float x, y; int life, val; uint16_t col; const char *msg; };  // msg!=null -> bonus banner "msg +val"
static Dpop   s_dp[NDMG];
static float  s_shake;
static int    s_flash, s_flashmax, s_flashpeak;   // peak = max screen coverage 0..255 (small blasts subtle, nuke = white-out)
static uint16_t s_flashcol;
static int    s_nuke_t;
#define NUKE_MS 720          // nuke mushroom rise time (explode_w sets s_nuke_t, draw_nuke normalises by it)
static float  s_nuke_x, s_nuke_y;
static int    s_lava_x = -1, s_lava_w;        // palette-cycling magma scar (latest crater)
static int64_t s_lava_t;

// clouds
#define NCLOUD 4
static float  s_cl_x[NCLOUD], s_cl_y[NCLOUD], s_cl_s[NCLOUD];

// input momentum — discrete on tap (precise ±1), auto-repeat with acceleration on hold.
static int     s_inE, s_inP;            // active direction (-1/0/+1) for elevation / power
static int64_t s_inUntil;               // hold deadline: cleared when no key event refreshes it
static int64_t s_inNext;                // next auto-repeat instant
static int64_t s_inT0;                  // when the current hold began (for ramp acceleration)
static int64_t s_wbar_t;                // weapon quick-card visible until this time (set on weapon change)
static int64_t s_hud_t;                 // last aim interaction: the HUD auto-hides once idle, ceding the screen to the scene
static bool    s_confirm;               // in-match: Esc raises a "leave match?" confirm instead of bailing instantly
#define HOLD_MS 230
#define HUD_SHOW_MS 2600                // full HUD chrome stays this long after the last input...
#define HUD_FADE_MS 480                 // ...then fades over this long, leaving just the big glance readout

// persistence
#define DIRR "/sd/data/tanks"
#define NTOP 6
static unsigned g_top[NTOP];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static float   frnd(float a, float b) { return a + (b - a) * ((esp_random() & 0xFFFF) / 65535.0f); }
static const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static int clampi(int v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; return v; }
static float clampf(float v, float lo, float hi) { if (v < lo) v = lo; if (v > hi) v = hi; return v; }
// Short one-word behaviour tag for a weapon, so the picker and the in-match card teach WHAT each gun
// does (not just its name) — you arm your 10 informed instead of blind. Bilingual; a handful of blast
// variants get a sharper label than a generic "blast". Pure lookup over the const arsenal: net-safe.
static const char *weap_kind(int wp) {
    switch (wp) {
        case 8:  return tx("ATOMICA", "NUKE");
        case 10: return tx("CURA", "HEAL");
        case 16: return tx("SALTO", "JUMP");
        case 18: return tx("PRECISO", "SNIPER");
    }
    switch (WEAPS[wp].beh) {
        case WB_DIG:     return tx("SCAVA", "DIGGER");
        case WB_CLUSTER: return tx("GRAPPOLO", "CLUSTER");
        case WB_RAIN:    return tx("PIOGGIA", "RAIN");
        case WB_MIRV:    return tx("SCIAME", "MIRV");
        case WB_ROLLER:  return tx("ROTOLA", "ROLLER");
        case WB_TELE:    return tx("SALTO", "JUMP");
        case WB_SHIELD:  return tx("SCUDO", "SHIELD");
        case WB_BEAM:    return tx("RAGGIO", "BEAM");
        case WB_DRILL:   return tx("TRIVELLA", "DRILL");
        case WB_WALL:    return tx("MURO", "WALL");
        case WB_HOMING:  return tx("GUIDATO", "HOMING");
        case WB_NAPALM:  return tx("FUOCO", "FIRE");
        case WB_UFO:     return tx("ALIENO", "ALIEN");
        case WB_BOUNCE:  return tx("RIMBALZO", "BOUNCE");
        default:         return (WEAPS[wp].count > 1) ? tx("MULTIPLO", "MULTI") : tx("ESPLOSIVO", "BLAST");
    }
}

// ============================ text helpers ===================================
static void txt(int x, int y, int sz, uint16_t col, const char *s) { d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s); }
static void txt_c(int cx, int y, int sz, uint16_t col, const char *s) { txt(cx - (int)strlen(s) * 3 * sz, y, sz, col, s); }
static void txt_r(int rx, int y, int sz, uint16_t col, const char *s) { txt(rx - (int)strlen(s) * 6 * sz, y, sz, col, s); }

// ============================ audio (polyphonic SNES cues) ===================
#define NSFX 49
static const char *sfx_name(int id) {
    switch (id) {
        case 1: return "nav";  case 2: return "sel";  case 3: return "back"; case 4: return "fire";
        case 5: return "fly";  case 6: return "boom"; case 7: return "small";case 8: return "hit";
        case 9: return "win";  case 10:return "lose"; case 11:return "tele"; case 12:return "turn";
        case 13:return "extra";case 14:return "theme";
        case 20:return "f_std"; case 21:return "f_bmb"; case 22:return "f_dig"; case 23:return "f_clu";
        case 24:return "f_rai"; case 25:return "f_swm"; case 26:return "f_rol"; case 27:return "f_tel"; case 28:return "f_nuk";
        case 30:return "h_std"; case 31:return "h_bmb"; case 32:return "h_dig"; case 33:return "h_clu";
        case 34:return "h_rai"; case 35:return "h_swm"; case 36:return "h_rol"; case 37:return "h_tel"; case 38:return "h_nuk";
        case 40:return "shld"; case 41:return "shldhit"; case 42:return "heal"; case 43:return "jump";
        case 44:return "beam"; case 45:return "drill";   case 46:return "build";case 47:return "burn";
        case 48:return "snipe"; case 49:return "lock";
        default: return "x";
    }
}
// MELODY = staggered t0; HARMONY = overlapping. amp is relative (peak auto-normalized). Real note Hz.
static int build_voices(int id, notify_voice_t *v) {
    switch (id) {
        case 1:  notify__voice(&v[0], 880.00f, 0.00f, 0.05f); v[0].amp = 0.55f; return 1;
        case 2:  notify__voice(&v[0], 659.25f, 0.00f, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.11f); v[1].amp = 0.9f; return 2;
        case 3:  notify__voice(&v[0], 783.99f, 0.00f, 0.07f); v[0].amp = 0.8f; notify__voice(&v[1], 523.25f, 0.06f, 0.10f); v[1].amp = 0.7f; return 2;
        case 4:  notify__voice(&v[0], 196.00f, 0.00f, 0.06f); v[0].amp = 1.0f; notify__voice(&v[1], 82.41f, 0.01f, 0.12f); v[1].amp = 0.95f;
                 notify__voice(&v[2], 440.00f, 0.00f, 0.03f); v[2].amp = 0.35f; return 3;
        case 5:  notify__voice(&v[0], 520, 0, 0.18f); v[0].amp = 0.22f; return 1;
        case 6:  notify__voice(&v[0], 55.00f, 0.00f, 0.40f); v[0].amp = 1.0f; notify__voice(&v[1], 82.41f, 0.00f, 0.30f); v[1].amp = 0.9f;
                 notify__voice(&v[2], 110.00f, 0.02f, 0.22f); v[2].amp = 0.7f; notify__voice(&v[3], 174.61f, 0.00f, 0.10f); v[3].amp = 0.55f;
                 notify__voice(&v[4], 261.63f, 0.03f, 0.16f); v[4].amp = 0.4f; return 5;
        case 7:  notify__voice(&v[0], 130.81f, 0.00f, 0.11f); v[0].amp = 0.9f; notify__voice(&v[1], 87.31f, 0.02f, 0.12f); v[1].amp = 0.8f;
                 notify__voice(&v[2], 207.65f, 0.00f, 0.06f); v[2].amp = 0.4f; return 3;
        case 8:  notify__voice(&v[0], 311.13f, 0.00f, 0.06f); v[0].amp = 0.9f; notify__voice(&v[1], 466.16f, 0.03f, 0.08f); v[1].amp = 0.7f; return 2;
        case 9: { int n = 0;                                                          // WIN — triumphant fanfare
                 notify__voice(&v[n], 261.63f, 0.00f, 1.55f); v[n].amp = 0.32f; n++;
                 notify__voice(&v[n], 392.00f, 0.00f, 1.55f); v[n].amp = 0.28f; n++;
                 notify__voice(&v[n], 329.63f, 0.40f, 1.15f); v[n].amp = 0.26f; n++;
                 notify__voice(&v[n], 523.25f, 0.00f, 0.22f); v[n].amp = 0.85f; n++;
                 notify__voice(&v[n], 659.25f, 0.14f, 0.22f); v[n].amp = 0.9f;  n++;
                 notify__voice(&v[n], 783.99f, 0.28f, 0.26f); v[n].amp = 0.95f; n++;
                 notify__voice(&v[n], 523.25f, 0.55f, 0.18f); v[n].amp = 0.8f;  n++;
                 notify__voice(&v[n], 659.25f, 0.70f, 0.18f); v[n].amp = 0.85f; n++;
                 notify__voice(&v[n], 783.99f, 0.85f, 0.20f); v[n].amp = 0.9f;  n++;
                 notify__voice(&v[n],1046.50f, 1.02f, 0.55f); v[n].amp = 1.0f;  n++;
                 notify__voice(&v[n], 523.25f, 1.02f, 0.52f); v[n].amp = 0.55f; n++;
                 notify__voice(&v[n], 659.25f, 1.02f, 0.52f); v[n].amp = 0.5f;  n++;
                 notify__voice(&v[n], 783.99f, 1.02f, 0.52f); v[n].amp = 0.5f;  n++;
                 notify__voice(&v[n],1567.98f, 1.10f, 0.30f); v[n].amp = 0.4f;  n++;
                 notify__voice(&v[n],2093.00f, 1.20f, 0.28f); v[n].amp = 0.35f; n++; return n; }
        case 10:{ int n = 0;                                                          // LOSE — sad cadence
                 notify__voice(&v[n], 110.00f, 0.00f, 1.15f); v[n].amp = 0.3f;  n++;
                 notify__voice(&v[n], 440.00f, 0.00f, 0.30f); v[n].amp = 0.85f; n++;
                 notify__voice(&v[n], 349.23f, 0.28f, 0.30f); v[n].amp = 0.8f;  n++;
                 notify__voice(&v[n], 293.66f, 0.56f, 0.34f); v[n].amp = 0.8f;  n++;
                 notify__voice(&v[n], 220.00f, 0.86f, 0.40f); v[n].amp = 0.9f;  n++;
                 notify__voice(&v[n], 261.63f, 0.86f, 0.38f); v[n].amp = 0.45f; n++; return n; }
        case 11: notify__voice(&v[0], 880, 0, 0.05f); notify__voice(&v[1], 1320, 0.04f, 0.06f); notify__voice(&v[2], 1760, 0.09f, 0.08f); return 3;
        case 12:{ int n = 0;                                                          // turn sting
                 notify__voice(&v[n], 587.33f, 0.00f, 0.10f); v[n].amp = 0.75f; n++;
                 notify__voice(&v[n], 783.99f, 0.08f, 0.10f); v[n].amp = 0.85f; n++;
                 notify__voice(&v[n], 987.77f, 0.16f, 0.26f); v[n].amp = 1.0f;  n++;
                 notify__voice(&v[n], 392.00f, 0.16f, 0.22f); v[n].amp = 0.4f;  n++; return n; }
        case 13:{ int n = 0;                                                          // extra/levelup
                 notify__voice(&v[n], 523.25f, 0.00f, 0.09f); v[n].amp = 0.7f;  n++;
                 notify__voice(&v[n], 659.25f, 0.07f, 0.09f); v[n].amp = 0.75f; n++;
                 notify__voice(&v[n], 783.99f, 0.14f, 0.09f); v[n].amp = 0.8f;  n++;
                 notify__voice(&v[n],1046.50f, 0.21f, 0.12f); v[n].amp = 0.9f;  n++;
                 notify__voice(&v[n],1318.51f, 0.30f, 0.26f); v[n].amp = 1.0f;  n++;
                 notify__voice(&v[n], 523.25f, 0.30f, 0.22f); v[n].amp = 0.35f; n++; return n; }
        case 14:{ int n = 0;                                                          // menu theme motif
                 notify__voice(&v[n], 261.63f, 0.00f, 1.00f); v[n].amp = 0.22f; n++;
                 notify__voice(&v[n], 392.00f, 0.00f, 1.00f); v[n].amp = 0.20f; n++;
                 notify__voice(&v[n], 349.23f, 1.00f, 1.00f); v[n].amp = 0.22f; n++;
                 notify__voice(&v[n], 440.00f, 1.00f, 1.00f); v[n].amp = 0.20f; n++;
                 notify__voice(&v[n], 783.99f, 0.00f, 0.22f); v[n].amp = 0.7f;  n++;
                 notify__voice(&v[n], 659.25f, 0.25f, 0.22f); v[n].amp = 0.65f; n++;
                 notify__voice(&v[n], 523.25f, 0.50f, 0.22f); v[n].amp = 0.7f;  n++;
                 notify__voice(&v[n], 587.33f, 0.75f, 0.22f); v[n].amp = 0.65f; n++;
                 notify__voice(&v[n], 659.25f, 1.00f, 0.24f); v[n].amp = 0.72f; n++;
                 notify__voice(&v[n], 880.00f, 1.25f, 0.24f); v[n].amp = 0.8f;  n++;
                 notify__voice(&v[n], 783.99f, 1.50f, 0.24f); v[n].amp = 0.72f; n++;
                 notify__voice(&v[n], 659.25f, 1.74f, 0.30f); v[n].amp = 0.7f;  n++; return n; }
        // ---- per-weapon FIRE 20-28 ----
        case 20: notify__voice(&v[0],392,0,0.05f); v[0].amp=0.9f; notify__voice(&v[1],196,0,0.08f); v[1].amp=0.8f; notify__voice(&v[2],784,0,0.03f); v[2].amp=0.35f; return 3;
        case 21: notify__voice(&v[0],65.41f,0,0.16f); v[0].amp=1.0f; notify__voice(&v[1],98,0,0.12f); v[1].amp=0.85f; notify__voice(&v[2],130.81f,0.02f,0.08f); v[2].amp=0.5f; return 3;
        case 22: notify__voice(&v[0],110,0,0.10f); v[0].amp=0.9f; notify__voice(&v[1],146.83f,0.04f,0.10f); v[1].amp=0.7f; notify__voice(&v[2],73.42f,0,0.14f); v[2].amp=0.85f; return 3;
        case 23: notify__voice(&v[0],523.25f,0,0.03f); v[0].amp=0.7f; notify__voice(&v[1],587.33f,0.03f,0.03f); v[1].amp=0.7f; notify__voice(&v[2],659.25f,0.06f,0.04f); v[2].amp=0.75f; notify__voice(&v[3],196,0,0.06f); v[3].amp=0.6f; return 4;
        case 24: notify__voice(&v[0],440,0,0.06f); v[0].amp=0.45f; notify__voice(&v[1],659.25f,0.05f,0.07f); v[1].amp=0.5f; notify__voice(&v[2],880,0.10f,0.10f); v[2].amp=0.4f; return 3;
        case 25: notify__voice(&v[0],330,0,0.12f); v[0].amp=0.7f; notify__voice(&v[1],336,0,0.12f); v[1].amp=0.6f; notify__voice(&v[2],494,0.06f,0.10f); v[2].amp=0.6f; notify__voice(&v[3],660,0.12f,0.08f); v[3].amp=0.5f; return 4;
        case 26: notify__voice(&v[0],261.63f,0,0.05f); v[0].amp=0.85f; notify__voice(&v[1],392,0.05f,0.06f); v[1].amp=0.8f; notify__voice(&v[2],329.63f,0.10f,0.10f); v[2].amp=0.7f; return 3;
        case 27: notify__voice(&v[0],660,0,0.05f); v[0].amp=0.6f; notify__voice(&v[1],990,0.05f,0.05f); v[1].amp=0.65f; notify__voice(&v[2],1320,0.10f,0.07f); v[2].amp=0.7f; notify__voice(&v[3],1760,0.15f,0.08f); v[3].amp=0.6f; return 4;
        case 28: notify__voice(&v[0],49,0,0.24f); v[0].amp=1.0f; notify__voice(&v[1],61.74f,0,0.20f); v[1].amp=0.8f; notify__voice(&v[2],82.41f,0.04f,0.14f); v[2].amp=0.55f; notify__voice(&v[3],110,0.08f,0.10f); v[3].amp=0.35f; return 4;
        // ---- per-weapon IMPACT 30-38 ----
        case 30: notify__voice(&v[0],82.41f,0,0.20f); v[0].amp=1.0f; notify__voice(&v[1],130.81f,0,0.12f); v[1].amp=0.8f; notify__voice(&v[2],196,0.02f,0.08f); v[2].amp=0.45f; return 3;
        case 31: notify__voice(&v[0],55,0,0.34f); v[0].amp=1.0f; notify__voice(&v[1],73.42f,0,0.26f); v[1].amp=0.9f; notify__voice(&v[2],110,0.02f,0.16f); v[2].amp=0.6f; notify__voice(&v[3],164.81f,0,0.09f); v[3].amp=0.4f; return 4;
        case 32: notify__voice(&v[0],61.74f,0,0.26f); v[0].amp=1.0f; notify__voice(&v[1],92.50f,0,0.20f); v[1].amp=0.7f; notify__voice(&v[2],46.25f,0.02f,0.30f); v[2].amp=0.85f; return 3;
        case 33: notify__voice(&v[0],220,0,0.05f); v[0].amp=0.8f; notify__voice(&v[1],311.13f,0.04f,0.05f); v[1].amp=0.75f; notify__voice(&v[2],174.61f,0.08f,0.06f); v[2].amp=0.7f; notify__voice(&v[3],98,0,0.12f); v[3].amp=0.6f; return 4;
        case 34: notify__voice(&v[0],659.25f,0,0.07f); v[0].amp=0.5f; notify__voice(&v[1],880,0.05f,0.07f); v[1].amp=0.45f; notify__voice(&v[2],1046.50f,0.10f,0.09f); v[2].amp=0.4f; notify__voice(&v[3],523.25f,0,0.08f); v[3].amp=0.3f; return 4;
        case 35: notify__voice(&v[0],130.81f,0,0.08f); v[0].amp=0.85f; notify__voice(&v[1],110,0.06f,0.08f); v[1].amp=0.75f; notify__voice(&v[2],146.83f,0.12f,0.09f); v[2].amp=0.7f; notify__voice(&v[3],82.41f,0,0.16f); v[3].amp=0.6f; return 4;
        case 36: notify__voice(&v[0],196,0,0.05f); v[0].amp=0.9f; notify__voice(&v[1],196,0.09f,0.05f); v[1].amp=0.85f; notify__voice(&v[2],98,0.09f,0.14f); v[2].amp=0.7f; notify__voice(&v[3],146.83f,0,0.06f); v[3].amp=0.4f; return 4;
        case 37: notify__voice(&v[0],1318.51f,0,0.06f); v[0].amp=0.6f; notify__voice(&v[1],987.77f,0.05f,0.07f); v[1].amp=0.55f; notify__voice(&v[2],659.25f,0.10f,0.10f); v[2].amp=0.5f; return 3;
        case 38:{ int n=0; notify__voice(&v[n],41.20f,0,0.90f); v[n].amp=1.0f; n++; notify__voice(&v[n],55,0,0.55f); v[n].amp=1.0f; n++;
                 notify__voice(&v[n],82.41f,0,0.40f); v[n].amp=0.9f; n++; notify__voice(&v[n],110,0.02f,0.30f); v[n].amp=0.7f; n++;
                 notify__voice(&v[n],174.61f,0,0.14f); v[n].amp=0.55f; n++; notify__voice(&v[n],261.63f,0.03f,0.20f); v[n].amp=0.4f; n++;
                 notify__voice(&v[n],61.74f,0.30f,0.70f); v[n].amp=0.6f; n++; notify__voice(&v[n],46.25f,0.45f,0.85f); v[n].amp=0.7f; n++;
                 notify__voice(&v[n],92.50f,0.55f,0.45f); v[n].amp=0.4f; n++; return n; }
        // ---- mechanics 40-47 ----
        case 40: notify__voice(&v[0],440,0,0.22f); v[0].amp=0.55f; notify__voice(&v[1],554.37f,0.04f,0.22f); v[1].amp=0.55f; notify__voice(&v[2],659.25f,0.08f,0.26f); v[2].amp=0.6f; notify__voice(&v[3],880,0.12f,0.24f); v[3].amp=0.5f; return 4;
        case 41: notify__voice(&v[0],1244.51f,0,0.10f); v[0].amp=0.8f; notify__voice(&v[1],1318.51f,0,0.14f); v[1].amp=0.6f; notify__voice(&v[2],880,0.02f,0.16f); v[2].amp=0.5f; return 3;
        case 42: notify__voice(&v[0],523.25f,0,0.14f); v[0].amp=0.6f; notify__voice(&v[1],659.25f,0.08f,0.14f); v[1].amp=0.65f; notify__voice(&v[2],783.99f,0.16f,0.16f); v[2].amp=0.7f; notify__voice(&v[3],1046.50f,0.24f,0.22f); v[3].amp=0.6f; return 4;
        case 43: notify__voice(&v[0],392,0,0.04f); v[0].amp=0.6f; notify__voice(&v[1],587.33f,0.04f,0.04f); v[1].amp=0.65f; notify__voice(&v[2],880,0.08f,0.07f); v[2].amp=0.6f; return 3;
        case 44: notify__voice(&v[0],1318.51f,0,0.05f); notify__voice(&v[1],1760,0.02f,0.06f); notify__voice(&v[2],2093,0.05f,0.08f); return 3;
        case 45: notify__voice(&v[0],130.81f,0,0.18f); v[0].amp=0.9f; notify__voice(&v[1],123.47f,0.06f,0.16f); v[1].amp=0.8f; return 2;
        case 46: notify__voice(&v[0],392,0,0.08f); notify__voice(&v[1],523.25f,0.06f,0.08f); notify__voice(&v[2],659.25f,0.12f,0.16f); return 3;
        case 47: notify__voice(&v[0],82.41f,0,0.30f); v[0].amp=0.9f; notify__voice(&v[1],110,0.05f,0.22f); v[1].amp=0.6f; return 2;
        case 48: notify__voice(&v[0],1174.66f,0,0.03f); v[0].amp=0.85f; notify__voice(&v[1],1567.98f,0.02f,0.04f); v[1].amp=0.7f; notify__voice(&v[2],587.33f,0,0.08f); v[2].amp=0.5f; return 3;   // sniper crack — sharp high snap
        case 49: notify__voice(&v[0],659.25f,0,0.05f); v[0].amp=0.6f; notify__voice(&v[1],987.77f,0.04f,0.05f); v[1].amp=0.65f; notify__voice(&v[2],1567.98f,0.09f,0.07f); v[2].amp=0.6f; return 3;   // homing lock — rising zap
    }
    return 0;
}
static bool sfx_important(int id) {
    return id == 4 || id == 6 || id == 9 || id == 10 || id == 12 || id == 14
        || (id >= 30 && id <= 38) || id == 41 || id == 48 || id == 49;
}
static void sfx(int id) {
    if (!g_audio || id <= 0) return;
    if (!sfx_important(id) && nucleo_audio_is_playing()) return;
    char p[80]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id));   // 1) deployed arcade WAV pack (best, real chiptune)
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); f = fopen(p, "rb"); }   // 2) on-device synth cache
    if (!f) return;                 // 3) NOT available -> skip. NEVER synthesize inline here: a multi-voice
    fclose(f);                      // synth+SD-write on the game task blocks for seconds = frozen screen at turn change.
    if (sfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
// Impact cue per weapon. Signature weapons keep their dedicated sound; generic blasts ROTATE through three
// explosion variants so firing the same gun twice never sounds identical (the #1 "samey audio" tell).
static unsigned s_sfxrot;
static void play_hit_sfx(int wp) {
    if (wp == 8)  { sfx(38); return; }                       // nuke — deep signature boom
    if (wp == 1)  { sfx(31); return; }                       // big bomb — its own big boom
    if (wp == 18) { sfx(48); return; }                       // sniper — sharp crack
    if (wp == 14) { sfx(49); return; }                       // homing — lock-on zap
    if (WEAPS[wp].beh == WB_BLAST) {                          // standard / tripler -> varied explosions
        static const int v[3] = { 30, 6, 35 };
        sfx(v[s_sfxrot++ % 3]); return;
    }
    sfx(hit_sfx[wp]);                                         // digger/cluster/rain/beam/... keep their cue
}
static void ensure_dirs(void) { mkdir("/sd/data", 0777); mkdir(DIRR, 0777); mkdir(DIRR "/sfx", 0777); }
static void presynth(void) {
    if (!g_audio) return;
    notify_voice_t v[16];
    for (int id = 1; id <= NSFX; id++) {
        char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ============================ persistence ====================================
#define CFG_MAGIC 0x544E4B36u   // 'TNK6' (bumped: adds the variable-wind toggle)
struct TkCfg { uint32_t m; int lang, audio, diff, manual, aimhelp, windvar; unsigned top[NTOP]; uint8_t ld[NWEAP]; };
static void cfg_write(void) {
    ensure_dirs();
    FILE *f = fopen(DIRR "/cfg.bin", "wb");
    if (!f) return;
    TkCfg c = { CFG_MAGIC, g_lang, g_audio, s_diff, g_manual, g_aimhelp, g_windvar, { 0 }, { 0 } };
    for (int i = 0; i < NTOP; i++) c.top[i] = g_top[i];
    for (int w = 0; w < NWEAP; w++) c.ld[w] = s_ldpick[w] ? 1 : 0;
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void) {
    FILE *f = fopen(DIRR "/cfg.bin", "rb");
    if (!f) return;
    TkCfg c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n != 1 || c.m != CFG_MAGIC) return;                // stale/foreign save -> fall back to defaults
    g_lang = c.lang ? 1 : 0; g_audio = c.audio ? 1 : 0; s_diff = clampi(c.diff, 0, 2);
    g_manual = c.manual ? 1 : 0; g_aimhelp = c.aimhelp ? 1 : 0; g_windvar = c.windvar ? 1 : 0;
    for (int i = 0; i < NTOP; i++) g_top[i] = c.top[i];
    for (int w = 0; w < NWEAP; w++) s_ldpick[w] = c.ld[w] != 0;
}
// Guarantee a legal 10-weapon loadout (first run, or a corrupt/old save): a varied, fun default set.
static void ld_ensure_valid(void) {
    if (ld_count() == LOADOUT_N) return;
    for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false;
    static const int def[LOADOUT_N] = { 0, 1, 3, 5, 6, 11, 14, 17, 19, 23 };
    for (int i = 0; i < LOADOUT_N; i++) if (def[i] < NWEAP) s_ldpick[def[i]] = true;
}
static void leaderboard_add(unsigned sc) {
    if (!sc) return;
    int pos = NTOP;
    for (int i = 0; i < NTOP; i++) if (sc > g_top[i]) { pos = i; break; }
    if (pos >= NTOP) return;
    for (int i = NTOP - 1; i > pos; i--) g_top[i] = g_top[i - 1];
    g_top[pos] = sc;
}

// ============================ FX =============================================
static void shake(float a) { if (a > s_shake) s_shake = a; }
static void flash(uint16_t c, int ms, int peak = 110) { s_flash = ms; s_flashmax = ms; s_flashpeak = peak; s_flashcol = c; }
static void spark_burst(float x, float y, int n, uint16_t col) {
    for (int i = 0; i < NSPK && n > 0; i++) {
        if (s_spk[i].life > 0) continue;
        float a = frnd(0, 6.283f), sp = frnd(0.3f, 1.9f);
        s_spk[i].x = x; s_spk[i].y = y; s_spk[i].vx = cosf(a) * sp; s_spk[i].vy = sinf(a) * sp - 0.5f;
        s_spk[i].max = s_spk[i].life = 14 + (int)frnd(0, 20); s_spk[i].col = col; n--;
    }
}
// Directional spark fan: emit n sparks around angle `ang` (rad) within +/-spread, speed in [spmin,spmax].
// Lets each weapon throw debris/embers/tracers in a signature shape instead of one generic omni-burst.
static void spark_cone(float x, float y, int n, uint16_t col, float ang, float spread, float spmin, float spmax) {
    for (int i = 0; i < NSPK && n > 0; i++) {
        if (s_spk[i].life > 0) continue;
        float a = ang + frnd(-spread, spread), sp = frnd(spmin, spmax);
        s_spk[i].x = x; s_spk[i].y = y; s_spk[i].vx = cosf(a) * sp; s_spk[i].vy = sinf(a) * sp;
        s_spk[i].max = s_spk[i].life = 14 + (int)frnd(0, 20); s_spk[i].col = col; n--;
    }
}
static void shat_spawn(float x, float y, float sc, uint16_t col, int dur) {
    for (int i = 0; i < NSHAT; i++) {
        if (s_sh[i].on) continue;
        s_sh[i] = { true, x, y, sc, frnd(0, 6.283f), 0, dur, col }; return;
    }
}
static void ring_spawn(float x, float y, float maxr, uint16_t col) {
    for (int i = 0; i < NRING; i++) {
        if (s_rg[i].on) continue;
        s_rg[i] = { true, x, y, 2, maxr, 300, 300, col }; return;
    }
}
static void dmg_popup(float x, float y, int val, uint16_t col) {
    for (int i = 0; i < NDMG; i++) {
        if (s_dp[i].on) continue;
        s_dp[i] = { true, x, y, 900, val, col, nullptr }; return;
    }
}
// A juicy "+pts MESSAGE" banner that reuses the damage-popup pool (msg!=null switches the renderer).
static void bonus_pop(float x, float y, const char *msg, int pts, uint16_t col) {
    for (int i = 0; i < NDMG; i++) {
        if (s_dp[i].on) continue;
        s_dp[i] = { true, x, y, 900, pts, col, msg }; return;
    }
}
static void fx_step(int dt) {
    float k = dt / 16.0f;
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        s_spk[i].x += s_spk[i].vx * k; s_spk[i].y += s_spk[i].vy * k; s_spk[i].vy += 0.05f * k;
        s_spk[i].life -= dt;
    }
    for (int i = 0; i < NSHAT; i++) {
        if (!s_sh[i].on) continue;
        s_sh[i].t += dt; if (s_sh[i].t >= s_sh[i].dur) s_sh[i].on = false;
    }
    for (int i = 0; i < NRING; i++) {
        if (!s_rg[i].on) continue;
        s_rg[i].life -= dt; if (s_rg[i].life <= 0) { s_rg[i].on = false; continue; }
        s_rg[i].r = 2 + (s_rg[i].max - 2) * (1.0f - (float)s_rg[i].life / s_rg[i].lmax);
    }
    for (int i = 0; i < NDMG; i++) {
        if (!s_dp[i].on) continue;
        s_dp[i].y -= 0.02f * dt; s_dp[i].life -= dt; if (s_dp[i].life <= 0) s_dp[i].on = false;
    }
    if (s_nuke_t > 0) s_nuke_t -= dt;
    if (s_beam_t > 0) s_beam_t -= dt;
    if (s_shake > 0.4f) s_shake *= 0.86f; else s_shake = 0;
    if (s_flash > 0) s_flash -= dt;
}

// ============================ terrain gen (value-noise + biomes) =============
static float h01(int i, uint32_t s) {
    uint32_t h = (uint32_t)i * 374761393u + s * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (h & 0xFFFF) / 65535.0f;
}
static float vn(float x, float period, uint32_t s) {
    float xp = x / period; int x0 = (int)floorf(xp); float t = xp - x0;
    float a = h01(x0, s), b = h01(x0 + 1, s); t = t * t * (3 - 2 * t);
    return a + (b - a) * t;
}
// Time-of-day / weather skies. Each sets a 3-stop sky ramp (top/mid/horizon), a matching ground palette,
// and the celestial/particle features (sun size+pos, moon, star count, rain). Tanks stay readable on all.
static void set_atmo(void) {
    s_atmo = s_seed % ATM_N; th_night = false;
    s_starN = 0; s_sunR = 0; s_moon = false; s_rain = false; s_sunX = (int)(W * 0.76f); s_sunY = 16;
    switch (s_atmo) {
    case ATM_DAY:        // clear blue afternoon, small high yellow sun, green land
        th_skyT = rgb(36,108,206); th_skyM = rgb(98,166,232); th_skyB = rgb(178,214,244); th_cloud = rgb(252,252,255);
        th_ground = rgb(98,166,72); th_ground2 = rgb(32,78,40); th_rim = rgb(184,230,140);
        th_accent = rgb(150,210,120); th_glow = rgb(255,250,205); th_sun = rgb(255,236,120);
        s_sunR = 7; s_sunX = (int)(W * 0.80f); s_sunY = 15; break;
    case ATM_SUNSET:     // huge sun setting on the horizon, violet -> orange
        th_skyT = rgb(48,26,92); th_skyM = rgb(196,82,98); th_skyB = rgb(255,170,66); th_cloud = rgb(255,150,96);
        th_ground = rgb(96,54,52); th_ground2 = rgb(26,14,30); th_rim = rgb(255,178,104);
        th_accent = rgb(255,150,90); th_glow = rgb(255,198,112); th_sun = rgb(255,214,96);
        s_sunR = 18; s_sunX = (int)(W * 0.5f); s_sunY = GTOP - 1; break;     // big, centered, sinking behind the ridge
    case ATM_EVENING:    // dusk: indigo -> rose, first stars, small low sun
        th_skyT = rgb(22,18,66); th_skyM = rgb(80,52,118); th_skyB = rgb(198,104,92); th_cloud = rgb(150,108,140);
        th_ground = rgb(62,74,86); th_ground2 = rgb(18,22,36); th_rim = rgb(170,160,198);
        th_accent = rgb(182,150,192); th_glow = rgb(232,150,120); th_sun = rgb(255,150,86);
        s_sunR = 9; s_sunX = (int)(W * 0.66f); s_sunY = GTOP - 5; s_starN = 9; break;
    case ATM_NIGHT:      // deep night, twinkling stars, small crescent moon
        th_night = true;
        th_skyT = rgb(5,7,26); th_skyM = rgb(14,18,48); th_skyB = rgb(34,34,76); th_cloud = rgb(40,46,80);
        th_ground = rgb(66,82,120); th_ground2 = rgb(14,18,36); th_rim = rgb(152,184,242);
        th_accent = rgb(160,190,250); th_glow = rgb(150,190,255); th_sun = rgb(240,242,226);
        s_starN = 30; s_moon = true; s_sunX = (int)(W * 0.76f); s_sunY = 16; break;
    default:             // ATM_RAIN: overcast grey-blue afternoon with rain
        th_skyT = rgb(54,62,82); th_skyM = rgb(90,100,120); th_skyB = rgb(128,138,156); th_cloud = rgb(66,74,92);
        th_ground = rgb(64,90,76); th_ground2 = rgb(20,28,32); th_rim = rgb(158,178,172);
        th_accent = rgb(140,170,180); th_glow = rgb(192,206,212); th_sun = rgb(200,210,220);
        s_rain = true; break;
    }
}
static void gen_terrain(void) {
    set_atmo();
    memset(s_dug, 0, sizeof s_dug);     // fresh board = pristine grass everywhere
    float amp = (s_atmo == ATM_RAIN) ? 0.85f : (s_atmo == ATM_DAY) ? 1.1f : 1.0f;   // gentle per-atmosphere relief
    float base = 84 + h01(0, s_seed) * 18;
    for (int x = 0; x < WW; x++) {
        float hh = base + amp * (
              34.0f * (vn(x, 210, s_seed + 1) - 0.5f)
            + 17.0f * (vn(x,  86, s_seed + 2) - 0.5f)
            +  8.0f * (vn(x,  36, s_seed + 3) - 0.5f)
            +  4.0f * (vn(x,  16, s_seed + 4) - 0.5f));
        s_h[x] = (uint8_t)clampi((int)hh, GTOP + 20, H - 6);
    }
}
static int surf(int x) { x = clampi(x, 0, WW - 1); return s_h[x]; }
static void plateau(int cx, int w) {
    int hv = surf(cx);
    for (int x = cx - w; x <= cx + w; x++) if (x >= 0 && x < WW) s_h[x] = (uint8_t)hv;
}
static void carve(int cx, int cy, int r) {
    int x0 = clampi(cx - r, 0, WW - 1), x1 = clampi(cx + r, 0, WW - 1);
    for (int x = x0; x <= x1; x++) {
        int dx = x - cx, inside = r * r - dx * dx;
        if (inside <= 0) continue;
        int dy = (int)sqrtf((float)inside), top = cy - dy, bot = cy + dy;
        if (bot <= s_h[x]) continue;
        if (top <= s_h[x]) { int nh = clampi(bot, GTOP + 6, H - 1); if (nh > s_h[x]) s_h[x] = (uint8_t)nh; s_dug[x] = 1; }
    }
    s_lava_x = cx; s_lava_w = r; s_lava_t = now_ms();   // molten scar
    if (s_rec && s_nops < NOPS) s_ops[s_nops++] = { 0, (int16_t)cx, (uint8_t)clampi(cy, 0, 255), (uint8_t)clampi(r, 0, 255) };
}
static void raise_wall(int cx, int r, int height) {     // Builder: inverse of carve, raises a mound
    for (int dx = -r; dx <= r; dx++) {
        int wx = cx + dx; if (wx < 0 || wx >= WW) continue;
        int bump = (int)(height * (1.0f - fabsf((float)dx) / (r + 1)));
        s_h[wx] = (uint8_t)clampi(surf(wx) - bump, GTOP + 6, H - 1);
        if (bump > 0) s_dug[wx] = 1;     // raised dirt mound -> exposed soil, no grass cap
    }
    if (s_rec && s_nops < NOPS) s_ops[s_nops++] = { 1, (int16_t)cx, (uint8_t)clampi(height, 0, 255), (uint8_t)clampi(r, 0, 255) };
}

// ============================ tanks / match ==================================
static int firstweap(int who) { for (int w = 0; w < NWEAP; w++) if (s_tk[who].ammo[w] != 0) return w; return 0; }
static void start_turn(void);
// Build a match from EXPLICIT params so host and guest produce an identical board (net) — or random (AI).
static void build_match(int mode, uint32_t seed, int wind, int t0x, int t1x, int starter) {
    s_mode = mode;
    s_seed = seed; gen_terrain(); s_wind = wind;
    // Decide each tank's arsenal.
    // vs CPU: P0 uses the player's chosen 10; the CPU rolls a fresh offensive 10 each match (replay variety).
    // Net: each side arms its OWN tank (s_seat) from the LOCAL loadout and gives the opponent the full
    // arsenal, so re-simming the opponent's transmitted shots never runs out of ammo. Loadouts stay LOCAL
    // (never in the START packet) — every shot's TK_FIRE carries the weapon, so the boards can't desync.
    bool have[2][NWEAP];
    for (int w = 0; w < NWEAP; w++) { have[0][w] = have[1][w] = false; }
    if (mode == MODE_AI) {
        ld_ensure_valid();
        for (int w = 0; w < NWEAP; w++) have[0][w] = s_ldpick[w];
        static const int aipool[] = { 0,1,2,3,4,5,6,8,11,12,14,15,17,18,20,21,22,23,24,25,27,28 };  // aimable/offensive only
        int np = (int)(sizeof(aipool) / sizeof(aipool[0])), picked = 0, guard = 0;
        while (picked < LOADOUT_N && guard++ < 400) { int w = aipool[esp_random() % np]; if (w < NWEAP && !have[1][w]) { have[1][w] = true; picked++; } }
    } else {
        ld_ensure_valid();
        for (int w = 0; w < NWEAP; w++) { have[s_seat][w] = s_ldpick[w]; have[1 - s_seat][w] = true; }
    }
    for (int i = 0; i < 2; i++) {
        s_tk[i].hp = 100; s_tk[i].dead = false; s_tk[i].elev = 45; s_tk[i].power = 75; s_tk[i].shield = 0;
        int c = 0;
        for (int w = 0; w < NWEAP; w++) {
            s_tk[i].ammo[w] = have[i][w] ? WEAPS[w].ammo : 0;
            if (have[i][w] && c < LOADOUT_N) s_slot[i][c++] = (uint8_t)w;    // flatten into the fixed quick-bar
        }
        s_nslot[i] = c;
        s_tk[i].weap = firstweap(i);
    }
    s_tk[0].x = t0x; s_tk[1].x = t1x;
    plateau(t0x, 9); plateau(t1x, 9);
    for (int i = 0; i < 2; i++) s_tk[i].y = surf((int)s_tk[i].x) - TANK_H / 2;
    for (int i = 0; i < NCLOUD; i++) { s_cl_x[i] = frnd(0, W); s_cl_y[i] = frnd(2, GTOP - 6); s_cl_s[i] = frnd(0.7f, 1.6f); }
    memset(s_pr, 0, sizeof s_pr); memset(s_spk, 0, sizeof s_spk); memset(s_sh, 0, sizeof s_sh);
    memset(s_rg, 0, sizeof s_rg); memset(s_dp, 0, sizeof s_dp); memset(s_fire, 0, sizeof s_fire);
    s_shake = 0; s_flash = 0; s_nuke_t = 0; s_camfree = 0; s_camY = 0; s_lava_x = -1;
    s_beam_t = 0; s_dwell_t = 0; s_draw_flag = 0; s_resseq = 0; s_rel_on = false; s_peerleft = false;
    s_ufo.on = false;
    s_bonus = 0; s_shooter = 0; s_shot_hits = 0; s_shot_long = false;   // fresh bonus tally each round
    s_last_rx = s_last_aim = now_ms();
    s_active = starter;
    s_cam = s_camtgt = clampf(s_tk[s_active].x - W / 2, 0, WW - W);
    // NB: the net win tally is NOT reset here — it's zeroed once when the lobby opens (menu -> picker),
    // so a MP "rematch" (host re-START from game-over) carries the best-of series across boards.
    start_turn();
}
static void new_match(void) {   // local vs CPU: random params
    build_match(MODE_AI, esp_random(), (int)frnd(-70, 70), (int)frnd(70, 160), (int)frnd(WW - 160, WW - 70), esp_random() & 1);
}

// ============================ ballistics =====================================
// Wind acceleration on a shell's horizontal velocity per ms (s_wind is -70..70). Kept in ONE place so the
// live physics (step_proj_once), the AI predictor (sim_land) and the aim-help preview (draw_traj) can never
// diverge. Eased from 0.00013 to a slightly gentler push: wind still nudges the arc, but no longer dominates it.
static const float WIND_ACCEL = 0.00011f;
static float v0_of(int power) { return 0.08f + power / 100.0f * 0.56f; }  // range ~v^2/g: power 100 reaches across the 720px world
static void barrel_tip(int who, float *bx, float *by, float *dx, float *dy) {
    float e = s_tk[who].elev * (float)M_PI / 180.0f, dir = (who == 0) ? 1.0f : -1.0f;
    *dx = cosf(e) * dir; *dy = -sinf(e);
    *bx = s_tk[who].x + *dx * BARREL; *by = s_tk[who].y - 3 + *dy * BARREL;
}
static int spawn_proj(float x, float y, float vx, float vy, int wp, int owner, bool child) {
    for (int i = 0; i < NPROJ; i++) {
        if (s_pr[i].on) continue;
        s_pr[i] = { true, x, y, vx, vy, wp, WEAPS[wp].beh, owner, child, false, false, 0, 0, 0 };
        if (WEAPS[wp].beh == WB_BOUNCE) s_pr[i].bnc = 3;   // grenade: bounces before it detonates
        return i;
    }
    return -1;
}
static bool any_proj(void) { for (int i = 0; i < NPROJ; i++) if (s_pr[i].on) return true; return false; }

// Reward a hit on the FOE with a floating banner; points (player only) accrue into the round's leaderboard
// score. Called from explode_w once per damaging hit, vs CPU only. foe = victim tank, pre = its HP before
// the hit, hx = impact x (for the cross-field "long shot" test).
static void award_juice(int foe, int pre, bool killed, float hx) {
    bool me = (s_shooter == 0);                                   // P0 = the human player vs CPU
    if (killed && pre >= 100)      { bonus_pop(s_tk[foe].x, s_tk[foe].y - 34, tx("ONE-SHOT!", "ONE-SHOT!"), 120, COL_GOLD); if (me) s_bonus += 120; }
    else if (s_shot_hits == 2)     { bonus_pop(s_tk[foe].x, s_tk[foe].y - 34, tx("DOPPIETTA", "DOUBLE HIT"), 60, COL_GOLD); if (me) s_bonus += 60; }
    if (s_shot_long && fabsf(hx - s_tk[s_shooter].x) > 480.0f) {  // a shot that arced across most of the field
        s_shot_long = false; bonus_pop(s_tk[foe].x, s_tk[foe].y - 46, tx("TIRO LUNGO", "LONG SHOT"), 50, COL_GOLD); if (me) s_bonus += 50;
    }
}
static void explode_w(float x, float y, int wp) {
    const Weap *w = &WEAPS[wp];
    carve((int)x, (int)y, w->crater);
    for (int i = 0; i < 2; i++) {
        if (s_tk[i].dead) continue;
        float dx = s_tk[i].x - x, dy = s_tk[i].y - y, dist = sqrtf(dx * dx + dy * dy);
        if (dist > w->crater + 8) continue;
        float f = 1.0f - dist / (w->crater + 8); if (f < 0) f = 0;
        int dd = (int)(w->dmg * f);
        if (dd > 0 && s_tk[i].shield > 0) {                 // bubble dome eats damage first
            int eat = dd < s_tk[i].shield ? dd : s_tk[i].shield;
            s_tk[i].shield -= eat; dd -= eat;
            ring_spawn(s_tk[i].x, s_tk[i].y, 20, rgb(140, 210, 255));
            dmg_popup(s_tk[i].x, s_tk[i].y - 24, eat, rgb(140, 210, 255)); sfx(41);
        }
        if (dd > 0) {
            int pre = s_tk[i].hp;
            s_tk[i].hp -= dd; dmg_popup(s_tk[i].x, s_tk[i].y - 16, dd, i ? COL_P1 : COL_P0); sfx(8);
            if (s_mode == MODE_AI && i != s_shooter) { s_shot_hits++; award_juice(i, pre, s_tk[i].hp <= 0, x); }   // a hit on the FOE -> juice
        }
        if (s_tk[i].hp <= 0) { s_tk[i].hp = 0; s_tk[i].dead = true; shat_spawn(s_tk[i].x, s_tk[i].y, 4.5f, i ? COL_P1 : COL_P0, 900); }
    }
    int nsp = 9; float shsc = 1.8f; uint16_t shc = mix(th_ground, w->col, 110), fc = w->col; int fms = 70; float ring = w->crater * 1.5f;
    // per-weapon calibrated camera kick: heavier ordnance (bigger crater / more damage) shakes harder,
    // so a pistol pop and a heavy shell no longer land with the same jolt (nuke overrides to 15 below).
    float shk = clampf(2.2f + w->crater * 0.16f + w->dmg * 0.045f, 2.5f, 8.5f);
    int fp = w->crater >= 20 ? 150 : 110;                  // bigger craters flash a touch brighter
    if (w->beh == WB_DIG)    { nsp = 16; shsc = 2.6f; shc = th_ground2; fms = 36; }
    if (w->beh == WB_CLUSTER){ nsp = 7;  shsc = 1.5f; }
    if (wp == 8) {
        nsp = 34; shsc = 5.5f; shc = mix(th_ground, COL_WHITE, 60); fc = COL_WHITE; fms = 360; shk = 15; ring = 96; fp = 255;   // full white-out
        s_nuke_t = NUKE_MS; s_nuke_x = x; s_nuke_y = y;
        ring_spawn(x, y, 128, rgb(255, 190, 110));        // outer fireball shock ring
        spark_burst(x, y - 6, 16, rgb(255, 220, 150));    // bright upward ejecta column
    }
    spark_burst(x, y, nsp, w->col);
    shat_spawn(x, y, shsc, shc, wp == 8 ? 750 : 440);
    ring_spawn(x, y, ring, fc);
    flash(fc, fms, fp); shake(shk);
    // --- per-weapon signature flourish: every attack should read DIFFERENTLY at the moment of impact ---
    switch (w->beh) {
        case WB_RAIN:                                                              // cold blue splash, twin droplet rings
            ring_spawn(x - 10, y, 12, rgb(120, 200, 255)); ring_spawn(x + 10, y, 12, rgb(120, 200, 255));
            spark_cone(x, y, 8, rgb(150, 220, 255), -1.57f, 0.7f, 0.6f, 1.6f); break;
        case WB_MIRV:                                                              // purple starburst up + halo ring
            spark_cone(x, y, 12, rgb(220, 140, 255), -1.57f, 1.4f, 1.0f, 2.6f); ring_spawn(x, y, 30, rgb(200, 120, 255)); break;
        case WB_CLUSTER:                                                           // low green scatter (kept modest: many bomblets)
            spark_burst(x, surf((int)x) - 1, 4, rgb(180, 255, 120)); break;
        case WB_ROLLER:                                                            // teal dust hugging the ground
            spark_cone(x, surf((int)x) - 2, 9, rgb(120, 255, 210), 0.0f, 3.14f, 0.5f, 1.6f); break;
        case WB_DIG:                                                               // brown earth chunks thrown up
            spark_cone(x, y, 12, th_ground2, -1.57f, 0.9f, 0.6f, 1.9f); break;
        case WB_DRILL:                                                             // stacked boring-dust rings underground
            for (int k = 0; k < 3; k++) ring_spawn(x, y + k * 4, 10, rgb(170, 120, 70));
            break;
        case WB_NAPALM:                                                            // orange fire fountain
            spark_cone(x, y, 14, rgb(255, 140, 40), -1.57f, 1.0f, 0.8f, 2.4f); break;
        case WB_HOMING:                                                            // yellow ring-of-sparks burst
            for (int k = 0; k < 10; k++) { float a = k * 0.628f; spark_cone(x + cosf(a) * 4, y + sinf(a) * 4, 1, rgb(255, 230, 90), a, 0.1f, 1.0f, 1.7f); }
            break;
        case WB_BEAM:                                                              // hot pink concentric double ring
            ring_spawn(x, y, 18, rgb(255, 90, 160)); ring_spawn(x, y, 32, rgb(255, 150, 200)); break;
        default:
            if (wp == 18)      { spark_cone(x, y, 6, COL_WHITE, -1.57f, 0.35f, 1.6f, 3.0f); ring_spawn(x, y, 16, COL_WHITE); }   // Sniper: tight white crack
            else if (wp == 15) { for (int k = -1; k <= 1; k++) ring_spawn(x + k * 8, y, 12, rgb(255, 150, 210)); }               // Tripler: three pink rings
            else if (wp == 1)  { ring_spawn(x, y, w->crater * 2.0f, rgb(255, 150, 60)); spark_cone(x, y, 10, rgb(255, 185, 90), -1.57f, 1.3f, 1.2f, 2.8f); }  // Big Bomb: fireball ring + embers
            else if (wp == 23) { ring_spawn(x, y, w->crater * 2.2f, rgb(255, 130, 60)); ring_spawn(x, y, 20, rgb(255, 200, 120)); spark_cone(x, y, 14, rgb(255, 180, 90), -1.57f, 1.5f, 1.4f, 3.2f); shake(7.0f); }  // Heavy: double fireball + big kick
            else if (wp == 22) { ring_spawn(x, y, 26, rgb(200, 160, 255)); spark_cone(x, y, 10, rgb(210, 170, 255), -1.57f, 1.0f, 1.0f, 2.4f); }   // Mortar: violet crown
            break;
    }
    if (wp == 28) { shake(9.0f); for (int k = -1; k <= 1; k++) ring_spawn(x + k * 22, surf((int)x + k * 22) - 2, 16, rgb(190, 150, 100)); }   // Quake: ground heaves, extra shake
    s_dwell_x = x; s_dwell_y = y; s_dwell_t = (wp == 8) ? 900 : 420;   // hold the camera on the crater
    play_hit_sfx(wp);
}
static int sim_land(int who, int elev, int power);
static void resolve_impact(Proj *p) {
    const Weap *w = &WEAPS[p->wp];
    float x = p->x, y = p->y;
    switch (p->beh) {
        case WB_TELE:
            s_tk[p->owner].x = clampf(x, 24, WW - 24);
            s_tk[p->owner].y = surf((int)s_tk[p->owner].x) - TANK_H / 2;
            for (int s = 0; s < 14; s++) { float a = frnd(0, 6.283f); spark_burst(x + cosf(a) * 6, y + sinf(a) * 6, 1, COL_WHITE); }
            ring_spawn(x, y, 22, COL_WHITE); flash(COL_WHITE, 90); sfx(11); p->on = false; return;
        case WB_CLUSTER:
            // Believable cluster munition: the shell CRACKS OPEN on contact and throws a fan of bomblets
            // UP and OUTWARD. Each one arcs and lands on its own, so the carpet pops in a natural stagger
            // over a wide footprint — not one fat blast with a few token sparks. Children do the real work.
            if (p->child) {
                explode_w(x, y, p->wp);                            // one bomblet = its own small crater + hit
            } else {
                spark_cone(x, y, 12, w->col, -1.57f, 1.5f, 1.4f, 3.0f);   // burst of casing sparks up and out
                ring_spawn(x, y, 16, w->col);
                flash(w->col, 60, 90); shake(2.6f);
                s_dwell_x = x; s_dwell_y = y; s_dwell_t = 340;    // hold the camera on the scatter footprint
                int n = w->count; if (n < 1) n = 1;
                for (int k = 0; k < n; k++) {
                    // Fan the bomblets across an upward arc (left-up .. up .. right-up) with jittered speed,
                    // so they spread out and rain down at different times and distances.
                    float t01 = (n > 1) ? (float)k / (n - 1) : 0.5f;
                    float ang = -2.60f + 1.90f * t01 + frnd(-0.14f, 0.14f);   // ~ -150deg .. -40deg
                    float sp  = frnd(0.16f, 0.34f);
                    if (spawn_proj(x, y - 4, cosf(ang) * sp, sinf(ang) * sp, p->wp, p->owner, true) < 0) break;
                }
                play_hit_sfx(p->wp);
            }
            p->on = false; return;
        case WB_ROLLER:
            if (!p->rolling) { p->rolling = true; p->fuse = 1400; p->vy = 0; p->y = surf((int)x) - 2; return; }
            explode_w(x, y, p->wp); p->on = false; return;
        case WB_DRILL:
            if (p->dig == 0) { p->dig = 22; spark_burst(x, y, 6, w->col); sfx(45); }   // start boring; step_proj drives it down
            return;
        case WB_WALL:
            raise_wall((int)x, 26, 30);                                                 // build a defensive hill
            for (int s = 0; s < 12; s++) spark_burst(x + frnd(-20, 20), surf((int)x) + frnd(-6, 2), 1, w->col);
            ring_spawn(x, surf((int)x), 24, w->col); flash(w->col, 50); shake(2); sfx(46); p->on = false; return;
        case WB_NAPALM: {
            explode_w(x, y, p->wp);
            int fw = (p->wp == 26) ? 34 : 24, ft = (p->wp == 26) ? 5 : 4;   // Firestorm burns WIDER and LONGER than Napalm (delivers its promised area-denial)
            for (int s = 0; s < NFIRE; s++) if (!s_fire[s].on) { s_fire[s] = { true, (int)x, fw, ft }; break; }
            sfx(47); p->on = false; return;
        }
        default:
            explode_w(x, y, p->wp); p->on = false; return;
    }
}
static void step_proj_once(int dt) {
    float g = 0.00040f, wind = s_wind / 100.0f * WIND_ACCEL, k = dt;
    for (int i = 0; i < NPROJ; i++) {
        Proj *p = &s_pr[i];
        if (!p->on) continue;
        if (p->dig > 0) {                                   // Drill: burrow straight down, blast underground
            p->y += 0.9f; p->dig--;
            if (((int)p->y & 1) == 0) spark_burst(p->x, p->y, 1, WEAPS[p->wp].col);
            if (p->dig <= 0) { explode_w(p->x, p->y, p->wp); p->on = false; }
            continue;
        }
        if (p->rolling) {
            float dir = (p->owner == 0) ? 1.0f : -1.0f;
            p->x += dir * 0.06f * k; p->y = surf((int)p->x) - 2; p->fuse -= dt;
            for (int t = 0; t < 2; t++) if (!s_tk[t].dead && fabsf(s_tk[t].x - p->x) < 8) p->fuse = 0;
            if (p->fuse <= 0 || p->x < 4 || p->x > WW - 4) resolve_impact(p);
            continue;
        }
        p->vy += g * k; p->vx += wind * k;
        if (p->beh == WB_HOMING && !p->child) {            // Guided: gentle seek toward the foe
            int foe = 1 - p->owner;
            if (!s_tk[foe].dead) {
                float tx2 = s_tk[foe].x - p->x, ty2 = s_tk[foe].y - p->y, l = sqrtf(tx2 * tx2 + ty2 * ty2) + 1e-3f;
                p->vx += (tx2 / l) * 0.00018f * k; p->vy += (ty2 / l) * 0.00018f * k;
            }
        }
        p->x += p->vx * k; p->y += p->vy * k;
        if (p->beh == WB_MIRV && !p->child && !p->split && p->vy >= 0) {
            p->split = true;
            // AIRBURST: at apex the shell flashes and splits into a few heavy warheads that fan apart and DROP
            // steeply (shed forward speed) -> a concentrated vertical rain on the target. Opposite of the cluster's
            // low ground scatter.
            int n = WEAPS[p->wp].count;
            for (int s = 0; s < n; s++) {
                float off = (s - (n - 1) / 2.0f) * 0.14f;
                spawn_proj(p->x, p->y, p->vx * 0.45f + off, p->vy + 0.05f, p->wp, p->owner, true);
            }
            spark_burst(p->x, p->y, 12, WEAPS[p->wp].col); ring_spawn(p->x, p->y, 18, WEAPS[p->wp].col);   // bright air-split flash
            p->on = false; continue;
        }
        if (p->x < 0 || p->x > WW - 1) { p->on = false; continue; }
        if (p->y >= surf((int)p->x)) {
            if (p->beh == WB_BOUNCE && p->bnc > 0 && p->vy > 0) {  // grenade: hop off the ground, shedding energy
                p->bnc--; p->y = surf((int)p->x) - 2;
                p->vy = -fabsf(p->vy) * 0.55f; p->vx *= 0.72f;
                spark_cone(p->x, p->y, 4, WEAPS[p->wp].col, -1.57f, 1.0f, 0.4f, 1.2f); sfx(7);
                continue;
            }
            p->y = surf((int)p->x); resolve_impact(p); continue;
        }
        for (int t = 0; t < 2; t++) {
            if (s_tk[t].dead) continue;
            // hitbox matched to the drawn tank: full hull width + turret reach upward, a touch of ground below.
            // Owner is immune only while the shell is still rising out of its own muzzle (so a clean lob never
            // self-detonates, but a shot that arcs back onto you still counts).
            if (t == p->owner && p->vy < 0 && fabsf(s_tk[t].x - p->x) < BARREL) continue;
            if (fabsf(s_tk[t].x - p->x) <= TANK_W / 2 + 2 && p->y >= s_tk[t].y - 10 && p->y <= s_tk[t].y + 5) { resolve_impact(p); break; }
        }
    }
}
static void step_proj(int dt) {                  // sub-step: fast shells never tunnel through tanks/terrain (≤4 ms = ≤~6 px/step)
    while (dt > 0) { int s = dt > 4 ? 4 : dt; step_proj_once(s); dt -= s; }
}
static int sim_land(int who, int elev, int power) {
    float e = elev * (float)M_PI / 180.0f, dir = (who == 0) ? 1.0f : -1.0f, v = v0_of(power);
    float x = s_tk[who].x + cosf(e) * dir * BARREL, y = s_tk[who].y - 3 - sinf(e) * BARREL;
    float vx = cosf(e) * dir * v, vy = -sinf(e) * v, g = 0.00040f, wind = s_wind / 100.0f * WIND_ACCEL;
    for (int it = 0; it < 4000; it++) {
        vy += g * 16; vx += wind * 16; x += vx * 16; y += vy * 16;
        if (x < 0 || x > WW - 1) return -1;
        if (y >= surf((int)x)) return (int)x;
    }
    return -1;
}
static void fire_weapon(int who) {
    if (s_phase != TP_AIM || who != s_active) return;            // single-fire guard (no double shots)
    int wp = s_tk[who].weap;
    if (s_tk[who].ammo[wp] == 0) { wp = firstweap(who); s_tk[who].weap = wp; }   // never burn a turn on empty
    const Weap *w = &WEAPS[wp];
    s_last_weap = wp; s_nops = 0; s_rec = (s_mode != MODE_AI);   // record terrain ops for the net RESULT
    s_shooter = who; s_shot_hits = 0; s_shot_long = true;        // start juicy-bonus attribution for this shot
    if (s_tk[who].ammo[wp] > 0) s_tk[who].ammo[wp]--;
    float bx, by, dx, dy; barrel_tip(who, &bx, &by, &dx, &dy);
    float v = v0_of(s_tk[who].power);
    sfx(fire_sfx[wp]);
    // --- muzzle blast: a bright directional flash cone + a lingering smoke puff at the barrel tip, scaled
    //     to the shot's power so a heavy charge kicks visibly harder than a light tap. Reads as a real shot.
    {
        float ang = atan2f(dy, dx);
        int kick = 3 + s_tk[who].power / 12;                     // heavier charge -> bigger flash + shake
        spark_cone(bx, by, kick, PROJ_GLOW, ang, 0.30f, 1.2f, 2.4f + s_tk[who].power / 40.0f);
        spark_cone(bx, by, 3, mix(w->col, COL_WHITE, 90), ang, 0.55f, 0.4f, 1.0f);   // hot core spit
        spark_cone(bx - dx * 3, by - dy * 3, 3, rgb(150, 150, 160), ang + 3.14f, 0.7f, 0.2f, 0.7f);  // back-blast smoke
        ring_spawn(bx, by, 8.0f, mix(w->col, COL_WHITE, 60));
        flash(mix(w->col, COL_WHITE, 40), 40, 70); shake(1.6f + kick * 0.25f);
    }
    // --- fire-time special weapons (no arcing projectile) ---
    if (w->beh == WB_SHIELD) {                                   // Bubble shield OR Med-Kit
        if (wp == MED_WP) { int b = s_tk[who].hp; s_tk[who].hp = clampi(s_tk[who].hp + 35, 0, 100);
            dmg_popup(s_tk[who].x, s_tk[who].y - 18, s_tk[who].hp - b, COL_GREEN); ring_spawn(s_tk[who].x, s_tk[who].y, 22, COL_GREEN); flash(COL_GREEN, 60); }
        else { s_tk[who].shield = 45; ring_spawn(s_tk[who].x, s_tk[who].y, 26, rgb(140, 210, 255)); flash(rgb(140, 210, 255), 60); }
        s_phase = TP_FIRE; return;
    }
    if (wp == JETS_WP) {                                         // Jump-jets: short directional hop
        float nx = clampf(s_tk[who].x + dx * 44, 24, WW - 24);
        for (int s = 0; s < 14; s++) spark_burst(s_tk[who].x, s_tk[who].y + frnd(0, 6), 1, rgb(120, 255, 255));
        s_tk[who].x = nx; s_tk[who].y = surf((int)nx) - TANK_H / 2;
        ring_spawn(nx, s_tk[who].y, 18, rgb(120, 255, 255)); s_phase = TP_FIRE; return;
    }
    if (w->beh == WB_BEAM) {                                     // Laser: instant straight ray
        float rx = bx, ry = by; int hx = -1, hy = -1;
        for (int s = 0; s < 900; s++) {
            rx += dx * 1.5f; ry += dy * 1.5f;
            if (rx < 0 || rx > WW - 1 || ry > H) break;
            bool hit = false;
            for (int t = 0; t < 2; t++) if (!s_tk[t].dead && fabsf(s_tk[t].x - rx) < TANK_W / 2 + 2 && fabsf(s_tk[t].y - ry) < TANK_H + 2) { hx = (int)rx; hy = (int)ry; hit = true; break; }
            if (hit) break;
            if (ry >= surf((int)rx)) { hx = (int)rx; hy = surf((int)rx); break; }
        }
        if (hx >= 0) { s_beam_x0 = (int)bx; s_beam_y0 = (int)by; s_beam_x1 = hx; s_beam_y1 = hy; s_beam_t = 240; explode_w(hx, hy, wp); }
        s_phase = TP_FIRE; return;
    }
    if (w->beh == WB_UFO) {                                      // Alien saucer: fly out of the barrel, then blast from above
        int foe = 1 - who;
        int lx = sim_land(who, s_tk[who].elev, s_tk[who].power);  // would this angle+power land on the foe?
        bool hit = (lx >= 0 && !s_tk[foe].dead && fabsf(lx - s_tk[foe].x) <= TANK_W / 2 + 8);
        s_ufo = { true, UF_FLYIN, 0, bx, by, dx * v, dy * v,
                  s_tk[foe].x, (float)(surf((int)s_tk[foe].x) - TANK_H / 2 - 42), who, foe, wp, hit };
        s_phase = TP_FIRE; return;
    }
    if (w->beh == WB_BLAST && w->count > 1) {                    // Tripler: 3-shot muzzle fan
        for (int s = 0; s < w->count; s++) {
            float a = atan2f(dy, dx) + (s - (w->count - 1) / 2.0f) * 0.09f;
            spawn_proj(bx, by, cosf(a) * v, sinf(a) * v, wp, who, true);
        }
        s_phase = TP_FIRE; return;
    }
    if (wp == SNIPE_WP) v *= 2.2f;                               // Sniper: flat & fast
    if (w->beh == WB_RAIN) {
        int lx = sim_land(who, s_tk[who].elev, s_tk[who].power);
        if (lx < 0) lx = (int)clampf(bx + dx * 120, 20, WW - 20);
        for (int k = 0; k < w->count; k++) spawn_proj(lx + frnd(-40, 40), GTOP - 6, frnd(-0.02f, 0.02f), frnd(0.12f, 0.18f), wp, who, true);
    } else spawn_proj(bx, by, dx * v, dy * v, wp, who, false);
    s_phase = TP_FIRE;
}

// Drive the alien saucer through its attack sequence: launch out of the barrel, reposition over the foe
// (only if the shot would have hit), charge, then rain a death-ray straight down. A miss just crash-fizzles.
static bool ufo_active(void) { return s_ufo.on; }
static void ufo_step(int dt) {
    if (!s_ufo.on) return;
    Ufo *u = &s_ufo;
    u->t += dt;
    float g = 0.00040f, wind = s_wind / 100.0f * WIND_ACCEL;
    switch (u->phase) {
        case UF_FLYIN:                                           // ballistic launch, exactly where you aimed
            u->vy += g * dt; u->vx += wind * dt; u->x += u->vx * dt; u->y += u->vy * dt;
            if ((s_anim & 1) == 0) spark_cone(u->x, u->y + 3, 1, rgb(150, 255, 200), 1.57f, 0.5f, 0.4f, 1.1f);
            if (u->t >= 380) { u->phase = u->hit ? UF_MOVE : UF_MISS; u->t = 0; }
            break;
        case UF_MOVE: {                                          // glide over to hover above the target
            float k = clampf(dt / 240.0f, 0, 1);
            u->x += (u->hx - u->x) * k; u->y += (u->hy - u->y) * k;
            if ((s_anim & 3) == 0) spark_burst(u->x + frnd(-6, 6), u->y + 8, 1, rgb(150, 255, 200));
            if (fabsf(u->x - u->hx) < 2 && fabsf(u->y - u->hy) < 2) { u->x = u->hx; u->y = u->hy; u->phase = UF_HOVER; u->t = 0; }
            break; }
        case UF_HOVER:                                           // hover + charge the beam
            u->y = u->hy + sinf(s_anim * 0.3f) * 2.0f;
            if ((s_anim & 1) == 0) spark_cone(u->x, u->y + 6, 1, rgb(120, 255, 180), -1.57f, 0.4f, 0.3f, 0.9f);
            if (u->t >= 520) {                                   // FIRE — death-ray from directly overhead
                explode_w(s_tk[u->foe].x, s_tk[u->foe].y, u->wp);
                flash(rgb(160, 255, 200), 120, 150); shake(6.0f); sfx(44);
                u->phase = UF_LASER; u->t = 0;
            }
            break;
        case UF_LASER:
            u->y = u->hy + sinf(s_anim * 0.3f) * 2.0f;
            if (u->t >= 420) { u->phase = UF_LEAVE; u->t = 0; }
            break;
        case UF_LEAVE:                                           // zip up and off
            u->y -= 0.22f * dt; u->x += 0.02f * dt * (u->owner == 0 ? 1 : -1);
            if (u->t >= 380 || u->y < GTOP - 30) u->on = false;
            break;
        case UF_MISS:                                            // no lock: keep arcing until it crash-fizzles
            u->vy += g * dt; u->vx += wind * dt; u->x += u->vx * dt; u->y += u->vy * dt;
            if (u->x < 2 || u->x > WW - 2 || u->y >= surf((int)clampf(u->x, 0, WW - 1))) {
                float gx = clampf(u->x, 2, WW - 2), gy = surf((int)gx);
                spark_burst(gx, gy, 10, rgb(150, 255, 200)); ring_spawn(gx, gy, 20, rgb(150, 255, 200));
                flash(rgb(150, 255, 200), 60); shake(2.0f); sfx(37);
                s_dwell_x = gx; s_dwell_y = gy; s_dwell_t = 300; u->on = false;
            }
            break;
    }
}

// ============================ AI =============================================
static void ai_plan(void) {
    int me = s_active, foe = 1 - me, wp = 0, tries = 0;
    do { wp = (int)(esp_random() % NWEAP); tries++; } while (s_tk[me].ammo[wp] == 0 && tries < 30);
    // CPU sticks to offence: skip teleport, jump, shield/med (utility) — they don't aim at the foe
    if (s_tk[me].ammo[wp] == 0 || WEAPS[wp].beh == WB_TELE || WEAPS[wp].beh == WB_SHIELD || wp == JETS_WP) wp = firstweap(me);
    s_tk[me].weap = wp;
    int bestE = 45, bestP = 60; float bestD = 1e9f;
    for (int e = 20; e <= 80; e += 5)
        for (int pw = 45; pw <= 100; pw += 5) {
            int lx = sim_land(me, e, pw);
            if (lx < 0) continue;
            float dd = fabsf(lx - s_tk[foe].x);
            if (dd < bestD) { bestD = dd; bestE = e; bestP = pw; }
        }
    int err[3] = { 10, 5, 2 };
    bestE += (int)frnd(-err[s_diff], err[s_diff]);
    bestP += (int)frnd(-err[s_diff] * 2, err[s_diff] * 2);
    s_ai_tgtE = clampi(bestE, 5, 88); s_ai_tgtP = clampi(bestP, 20, 100);   // barrel eases onto target in poll
}

// ============================ turn flow ======================================
static void go(int s);
static void net_finish_turn(void);
static void check_over(void) {
    if (s_tk[0].dead || s_tk[1].dead) {
        s_phase = TP_OVER;
        s_draw_flag = (s_tk[0].dead && s_tk[1].dead);            // mutual destruction = draw
        if (s_draw_flag) sfx(10);
        else {
            int win = s_tk[0].dead ? 1 : 0; s_wins[win]++;
            int mywin = (s_mode == MODE_GUEST) ? (win == 1) : (win == 0);   // did THIS device win?
            sfx(mywin ? 9 : 10);
            if (s_mode == MODE_AI && win == 0) leaderboard_add((unsigned)s_tk[0].hp + (s_diff + 1) * 100u + s_bonus);
        }
        cfg_write();
    }
}
static bool drop_tanks(int dt) {                                // ease both tanks down onto the terrain; true while still falling
    bool moving = false;
    for (int i = 0; i < 2; i++) {
        float tgt = surf((int)s_tk[i].x) - TANK_H / 2;
        if (s_tk[i].y < tgt - 0.5f) { s_tk[i].y += 0.12f * dt; if (s_tk[i].y > tgt) s_tk[i].y = tgt; moving = true; }
        else s_tk[i].y = tgt;
    }
    return moving;
}
static void settle_fall_only(int dt) { drop_tanks(dt); }        // drop tanks during the impact dwell, without ending the turn
static void settle(int dt) {
    if (drop_tanks(dt)) return;                                 // still settling -> let them finish falling first
    if (s_mode != MODE_AI) { net_finish_turn(); return; }      // net: active controller packages + sends the result
    check_over(); if (s_phase != TP_OVER) { s_active = 1 - s_active; start_turn(); }
}
static void seed_entry(void) {                       // manual aim: prime the digit strings
    snprintf(s_entry_ang, sizeof s_entry_ang, "%d", s_tk[s_active].elev);
    snprintf(s_entry_pow, sizeof s_entry_pow, "%d", s_tk[s_active].power);
    s_entry_field = 0;
}
static void start_turn(void) {
    s_spectate = false;
    // Wind is fixed for the match by default (set once in build_match) — re-rolling every turn felt random/unfair.
    // Optional "variable wind" (Settings) re-rolls it each turn for a Pocket-Tanks feel. vs CPU ONLY: on the net
    // board the wind is carried authoritatively in the RESULT, so a local re-roll here would desync the boards.
    if (g_windvar && s_mode == MODE_AI) s_wind = (int)frnd(-70, 70);
    // napalm patches burn whoever stands in them, then age out
    for (int s = 0; s < NFIRE; s++) if (s_fire[s].on) {
        for (int t = 0; t < 2; t++) if (!s_tk[t].dead && fabsf(s_tk[t].x - s_fire[s].x) < s_fire[s].w) {
            int dd = 8;
            if (s_tk[t].shield > 0) { int e = dd < s_tk[t].shield ? dd : s_tk[t].shield; s_tk[t].shield -= e; dd -= e; }
            if (dd > 0) { s_tk[t].hp -= dd; if (s_tk[t].hp <= 0) { s_tk[t].hp = 0; s_tk[t].dead = true; } dmg_popup(s_tk[t].x, s_tk[t].y - 16, dd, COL_RED); }
        }
        if (--s_fire[s].ticks <= 0) s_fire[s].on = false;
    }
    if (s_tk[0].dead || s_tk[1].dead) { check_over(); return; } // a burn can be lethal
    if (s_tk[s_active].ammo[s_tk[s_active].weap] == 0) s_tk[s_active].weap = firstweap(s_active);
    s_camfree = 0; s_camtgt = clampf(s_tk[s_active].x - W / 2, 0, WW - W);
    s_phase = TP_TURN; s_turn_t = 1050; s_ai_planned = false; s_aiwait = 700; s_dwell_t = 0;
    s_inE = s_inP = 0; s_hud_t = now_ms();             // fresh turn: clear held momentum, show the HUD chrome
    bool mine = (s_mode == MODE_AI) ? (s_active == 0) : (s_active == s_seat);
    if (mine) s_wbar_t = now_ms() + 2400;              // flash the weapon picker so you start the turn knowing your loadout
    if (mine && s_mode != MODE_AI) { s_yt_t = now_ms() + 1300; sfx(8); }   // net: announce YOUR TURN (banner + chirp) so you never miss the handoff
    if (g_manual && s_active == s_seat) seed_entry();
}

// ============================ multiplayer (active-authoritative) ==============
struct __attribute__((packed)) TkStart { uint8_t m0, m1, ver, type; uint32_t seed; int16_t wind, t0x, t1x; uint8_t starter; };
struct __attribute__((packed)) TkOp { uint8_t kind; int16_t x; uint8_t y, r; };
struct __attribute__((packed)) TkFire { int16_t x; uint8_t w, ticks; };
struct __attribute__((packed)) TkResult {
    uint8_t m0, m1, ver, type; uint32_t seq;
    uint8_t weapon, owner; int16_t elev, power;
    uint8_t nops; TkOp op[NOPS];
    uint8_t hp0, hp1, sh0, sh1;
    int16_t t0x; uint8_t t0y; int16_t t1x; uint8_t t1y;
    uint8_t nfire; TkFire fire[NFIRE];
    int16_t wind; uint8_t next, winner;
};
static bool local_active(void) { return s_mode == MODE_AI ? (s_active == 0) : (s_active == s_seat); }
static void net_ack(uint32_t seq) { uint8_t b[8] = { TK_M0, TK_M1, TK_VER, TK_ACK }; memcpy(b + 4, &seq, 4); pnet_send(s_peer, b, 8); }
static void send_reliable(const void *buf, int len) { memcpy(s_rel, buf, len); s_rel_len = len; s_rel_on = true; s_rel_next = now_ms() + 160; pnet_send(s_peer, buf, len); }
static void net_pump_reliable(void) { if (s_rel_on && now_ms() >= s_rel_next) { pnet_send(s_peer, s_rel, s_rel_len); s_rel_next = now_ms() + 160; } }
static void net_send_hello(void) { uint8_t b[28] = { TK_M0, TK_M1, TK_VER, TK_HELLO }; snprintf((char *)b + 4, 22, "%s", pnet_name()); pnet_send(NULL, b, 28); }
static void net_send_bye(void) { if (s_haspeer) { uint8_t b[4] = { TK_M0, TK_M1, TK_VER, TK_BYE }; pnet_send(s_peer, b, 4); } }
static void net_send_aim(void) {
    uint8_t b[10] = { TK_M0, TK_M1, TK_VER, TK_AIM, (uint8_t)s_tk[s_seat].weap };
    int16_t e = (int16_t)s_tk[s_seat].elev, p = (int16_t)s_tk[s_seat].power;
    memcpy(b + 6, &e, 2); memcpy(b + 8, &p, 2); pnet_send(s_peer, b, 10);
}
static void net_send_fire(void) {   // tell the peer we just fired, with the exact shot params so it can re-sim the arc locally
    uint8_t b[12] = { TK_M0, TK_M1, TK_VER, TK_FIRE, (uint8_t)s_tk[s_seat].weap };
    int16_t e = (int16_t)s_tk[s_seat].elev, p = (int16_t)s_tk[s_seat].power, w = (int16_t)s_wind;
    memcpy(b + 6, &e, 2); memcpy(b + 8, &p, 2); memcpy(b + 10, &w, 2); pnet_send(s_peer, b, 12);
}
static void room_add(const uint8_t *mac, const char *name) {
    for (int i = 0; i < s_nroom; i++) if (!memcmp(s_rooms[i].mac, mac, 6)) { s_rooms[i].seen = now_ms(); snprintf(s_rooms[i].name, 22, "%s", name); return; }
    if (s_nroom >= NROOM) return;
    memcpy(s_rooms[s_nroom].mac, mac, 6); snprintf(s_rooms[s_nroom].name, 22, "%s", (name && name[0]) ? name : "?"); s_rooms[s_nroom].seen = now_ms(); s_nroom++;
}
static void rooms_prune(void) {
    for (int i = 0; i < s_nroom; ) { if (now_ms() - s_rooms[i].seen > 4000) { for (int k = i; k < s_nroom - 1; k++) s_rooms[k] = s_rooms[k + 1]; s_nroom--; } else i++; }
    if (s_rsel >= s_nroom) s_rsel = s_nroom > 0 ? s_nroom - 1 : 0;
}
static void go(int s);
static void host_start_match(void) {                            // host picks the board and tells the guest
    uint32_t seed = esp_random();
    int wind = (int)frnd(-70, 70), t0x = (int)frnd(70, 160), t1x = (int)frnd(WW - 160, WW - 70), starter = esp_random() & 1;
    s_seat = 0;
    build_match(MODE_HOST, seed, wind, t0x, t1x, starter);
    TkStart st = { TK_M0, TK_M1, TK_VER, TK_START, seed, (int16_t)wind, (int16_t)t0x, (int16_t)t1x, (uint8_t)starter };
    send_reliable(&st, sizeof st);
    go(ST_PLAY);
}
static void net_over(int winner) {                              // shared game-over bookkeeping (both devices)
    s_phase = TP_OVER; s_draw_flag = (winner == 3);
    if (winner == 1 || winner == 2) s_wins[winner - 1]++;
    int mywin = (s_mode == MODE_GUEST) ? (winner == 2) : (winner == 1);
    sfx(mywin ? 9 : 10); cfg_write();
}
static void net_finish_turn(void) {                             // ACTIVE controller: package + send + advance
    int winner = 0;
    if (s_tk[0].dead && s_tk[1].dead) winner = 3; else if (s_tk[1].dead) winner = 1; else if (s_tk[0].dead) winner = 2;
    int newwind = winner ? s_wind : (int)frnd(-70, 70), next = winner ? s_active : (1 - s_active);
    TkResult r; memset(&r, 0, sizeof r);
    r.m0 = TK_M0; r.m1 = TK_M1; r.ver = TK_VER; r.type = TK_RESULT; r.seq = ++s_resseq;
    r.weapon = (uint8_t)s_last_weap; r.owner = (uint8_t)s_active; r.elev = (int16_t)s_tk[s_active].elev; r.power = (int16_t)s_tk[s_active].power;
    r.nops = (uint8_t)s_nops; for (int i = 0; i < s_nops; i++) { r.op[i].kind = s_ops[i].kind; r.op[i].x = s_ops[i].x; r.op[i].y = s_ops[i].y; r.op[i].r = s_ops[i].r; }
    r.hp0 = (uint8_t)s_tk[0].hp; r.hp1 = (uint8_t)s_tk[1].hp; r.sh0 = (uint8_t)clampi(s_tk[0].shield, 0, 255); r.sh1 = (uint8_t)clampi(s_tk[1].shield, 0, 255);
    r.t0x = (int16_t)s_tk[0].x; r.t0y = (uint8_t)clampi((int)s_tk[0].y, 0, 255); r.t1x = (int16_t)s_tk[1].x; r.t1y = (uint8_t)clampi((int)s_tk[1].y, 0, 255);
    int nf = 0; for (int i = 0; i < NFIRE; i++) if (s_fire[i].on && nf < NFIRE) { r.fire[nf].x = (int16_t)s_fire[i].x; r.fire[nf].w = (uint8_t)s_fire[i].w; r.fire[nf].ticks = (uint8_t)s_fire[i].ticks; nf++; }
    r.nfire = (uint8_t)nf; r.wind = (int16_t)newwind; r.next = (uint8_t)next; r.winner = (uint8_t)winner;
    send_reliable(&r, sizeof r);
    s_rec = false;
    if (winner) net_over(winner);
    else { s_wind = newwind; s_active = next; start_turn(); }
}
static void apply_result(const TkResult *r) {                   // PASSIVE: apply the authoritative outcome
    net_ack(r->seq);
    if (r->seq <= s_resseq) return;                             // duplicate
    s_resseq = r->seq;
    bool spectated = s_spectate; s_spectate = false;            // did we already re-sim this shot locally (saw arc+blast)?
    for (int i = 0; i < NPROJ; i++) s_pr[i].on = false;         // end any in-flight spectator projectiles
    int wp = (r->weapon < NWEAP) ? r->weapon : 0;   // guard: weapon id comes off the wire (uint8_t, so >=0 implicitly)
    for (int i = 0; i < r->nops && i < NOPS; i++) {
        if (r->op[i].kind == 0) { carve(r->op[i].x, r->op[i].y, r->op[i].r); if (!spectated) { spark_burst(r->op[i].x, r->op[i].y, 8, COL_GOLD); ring_spawn(r->op[i].x, r->op[i].y, r->op[i].r * 1.5f, COL_GOLD); } }
        else raise_wall(r->op[i].x, r->op[i].r, r->op[i].y);
    }
    s_tk[0].hp = r->hp0; s_tk[1].hp = r->hp1; s_tk[0].shield = r->sh0; s_tk[1].shield = r->sh1;
    s_tk[0].x = r->t0x; s_tk[0].y = r->t0y; s_tk[1].x = r->t1x; s_tk[1].y = r->t1y;
    s_tk[0].dead = (r->hp0 == 0); s_tk[1].dead = (r->hp1 == 0);
    memset(s_fire, 0, sizeof s_fire);
    for (int i = 0; i < r->nfire && i < NFIRE; i++) s_fire[i] = { true, r->fire[i].x, r->fire[i].w, r->fire[i].ticks };
    if (!spectated) {                                           // fallback (FIRE packet lost): snap onto the impact and play the blast
        if (r->nops > 0) { s_dwell_x = r->op[0].x; s_dwell_y = r->op[0].y; s_dwell_t = (wp == 8) ? 700 : 360; shake(wp == 8 ? 9.0f : 4.0f); flash(WEAPS[wp].col, 90); s_cam = clampf((float)r->op[0].x - W / 2, 0, WW - W); }
        sfx(hit_sfx[wp]);
    }
    s_wind = r->wind; s_last_rx = now_ms();
    if (r->winner) net_over(r->winner);
    else { s_active = r->next; start_turn(); }
}
static void net_handle(const pnet_pkt_t *p) {
    if (p->len < 4 || p->buf[0] != TK_M0 || p->buf[1] != TK_M1 || p->buf[2] != TK_VER) return;
    int type = p->buf[3];
    if (type == TK_ACK) { if (s_rel_on && p->len >= 8) { uint32_t a; memcpy(&a, p->buf + 4, 4); if (a == s_resseq) s_rel_on = false; } return; }
    if (type == TK_BYE) { s_peerleft = true; return; }
    // Rematch: the host re-STARTs from game-over while the guest sits in ST_OVER. Accept the new board
    // right here (peer + seat already known) and drop straight back into play, ACKing like a first join.
    if (type == TK_START && s_screen == ST_OVER && s_seat == 1 && p->len >= (int)sizeof(TkStart)) {
        const TkStart *st = (const TkStart *)p->buf; s_haspeer = true;
        build_match(MODE_GUEST, st->seed, st->wind, st->t0x, st->t1x, st->starter);
        net_ack(0); go(ST_PLAY); return;
    }
    if (s_screen == ST_BROWSE) {
        if (type == TK_HELLO && p->len >= 28) room_add(p->mac, (const char *)p->buf + 4);
        else if (type == TK_WELCOME && s_join_pending) { memcpy(s_peer, p->mac, 6); s_haspeer = true; s_join_pending = false; s_welcomed = true; sfx(2); }
        else if (type == TK_START && p->len >= (int)sizeof(TkStart)) {
            const TkStart *st = (const TkStart *)p->buf; memcpy(s_peer, p->mac, 6); s_haspeer = true; s_seat = 1;
            build_match(MODE_GUEST, st->seed, st->wind, st->t0x, st->t1x, st->starter);
            net_ack(0); go(ST_PLAY);
        }
        return;
    }
    if (s_screen == ST_HOST) {
        if (type == TK_JOIN && p->len >= 26) { memcpy(s_peer, p->mac, 6); s_haspeer = true; s_guest_in = true; snprintf(s_peer_name, 22, "%s", (const char *)p->buf + 4);
            uint8_t b[4] = { TK_M0, TK_M1, TK_VER, TK_WELCOME }; pnet_send(s_peer, b, 4); }
        return;
    }
    if (s_screen == ST_PLAY || s_screen == ST_OVER) {
        if (!s_haspeer || memcmp(p->mac, s_peer, 6)) return;
        if (type == TK_AIM && !local_active() && p->len >= 10) {
            s_tk[s_active].weap = p->buf[4]; int16_t e, pw; memcpy(&e, p->buf + 6, 2); memcpy(&pw, p->buf + 8, 2);
            s_tk[s_active].elev = e; s_tk[s_active].power = pw; s_last_rx = now_ms();
        } else if (type == TK_FIRE && !local_active() && s_phase == TP_AIM && p->len >= 12) {
            int16_t e, pw, wd; memcpy(&e, p->buf + 6, 2); memcpy(&pw, p->buf + 8, 2); memcpy(&wd, p->buf + 10, 2);
            int wp = p->buf[4]; if (wp >= NWEAP) wp = 0;   // p->buf is uint8_t, so wp is already >= 0
            s_tk[s_active].weap = wp; s_tk[s_active].elev = e; s_tk[s_active].power = pw; s_wind = wd; s_last_rx = now_ms();
            if (s_tk[s_active].ammo[wp] == 0) s_tk[s_active].ammo[wp] = 9;   // keep fire_weapon from switching weapons
            s_spectate = true; fire_weapon(s_active);                        // re-simulate the shot locally so the camera follows the arc
        } else if (type == TK_RESULT && p->len >= (int)sizeof(TkResult)) {
            apply_result((const TkResult *)p->buf);
        }
    }
}

// ============================ contrast helpers (always-pop) ==================
static inline int luma565(uint16_t c) {
    int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    return (30 * r + 59 * g + 11 * b) / 100;
}
static inline uint16_t outline_for(uint16_t bg) { return luma565(bg) >= 128 ? COL_INK : COL_WHITE; }
static inline uint16_t bg_at(int wx, int wy) {     // reconstruct what the world drew at a world point
    if (wx < 0) wx = 0;
    if (wx >= WW) wx = WW - 1;
    if (wy >= s_h[wx]) { int gy = s_h[wx]; return mix(th_ground, th_ground2, (gy - GTOP) * 256 / (H - GTOP)); }
    return th_skyB;
}

// ============================ rendering: world ===============================
// 8bpp = RGB332 (R,G:8 levels, B:4): raw vertical ramps band hard. Ordered (Bayer) dithering blends them.
static const uint8_t BAYER4[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };
static inline int R5(uint16_t c){ return ((c >> 11) & 31) * 255 / 31; }
static inline int G5(uint16_t c){ return ((c >> 5) & 63) * 255 / 63; }
static inline int B5(uint16_t c){ return (c & 31) * 255 / 31; }
static inline uint16_t dith332(int r, int g, int b, int t) {     // t = Bayer threshold 0..15
    int rl = (r * 7 * 16 + t * 255) / (255 * 16); if (rl > 7) rl = 7; if (rl < 0) rl = 0;
    int gl = (g * 7 * 16 + t * 255) / (255 * 16); if (gl > 7) gl = 7; if (gl < 0) gl = 0;
    int bl = (b * 3 * 16 + t * 255) / (255 * 16); if (bl > 3) bl = 3; if (bl < 0) bl = 0;
    return rgb(rl * 255 / 7, gl * 255 / 7, bl * 255 / 3);
}
static inline uint16_t sky_ramp(int y) {                          // smooth (undithered) 3-stop sky colour for overlays
    if (y < 0) y = 0;
    if (y >= H) y = H - 1;
    float f = (float)y / (H - 1);
    if (f < 0.5f) return mix(th_skyT, th_skyM, (int)(f * 2 * 256));
    return mix(th_skyM, th_skyB, (int)((f - 0.5f) * 2 * 256));
}
static void world_sky(int oy) {
    // ---- clean banded gradient (top -> mid -> horizon) ----
    // One solid colour PER ROW (no per-pixel Bayer dither): the sky reads as calm, homogeneous
    // bands instead of a speckle of colours — the single biggest "less chaotic" win. Also cheaper
    // (one HLine per row vs W drawPixel). Celestial/parallax accents below still give it depth.
    for (int y = 0; y < H; y++) d.drawFastHLine(0, y, W, sky_ramp(y));
    // ---- stars (night + a few at dusk): twinkling single pixels, a few sparkle ----
    for (int i = 0; i < s_starN; i++) {
        int sxp = (int)(h01(i, s_seed) * W), syp = (int)(h01(i + 99, s_seed) * (GTOP + 32)) + oy;
        int ph = ((s_anim >> 2) + i * 5) & 7; if (ph >= 6) continue;
        uint16_t sc = mix(th_skyB, COL_WHITE, 150 + (int)(h01(i + 7, s_seed) * 105));
        d.drawPixel(sxp, syp, sc);
    }
    // ---- crescent moon ----
    if (s_moon) {
        int mx = s_sunX - (int)(s_cam * 0.03f), my = s_sunY + oy, mr = 8;
        d.drawCircle(mx, my, mr + 2, mix(th_skyB, th_sun, 40));
        d.fillCircle(mx, my, mr, th_sun);
        d.fillCircle(mx - 3, my - 2, 2, mix(th_sun, COL_INK, 34));
        d.fillCircle(mx + 2, my + 3, 1, mix(th_sun, COL_INK, 30));
        d.fillCircle(mx + 1, my - 3, 1, mix(th_sun, COL_INK, 26));
        d.fillCircle(mx + 5, my - 3, mr, sky_ramp(my));            // carve the crescent with the sky behind
    }
    // ---- sun ----
    if (s_sunR > 0) {
        int sx = s_sunX - (int)(s_cam * 0.03f), sy = s_sunY + oy;
        if (s_atmo == ATM_SUNSET) {                               // big setting sun: warm halo + atmospheric bands + bright core
            for (int rr = s_sunR + 12; rr > s_sunR; rr--) d.drawCircle(sx, sy, rr, mix(sky_ramp(sy), th_glow, (s_sunR + 12 - rr) * 16));
            d.fillCircle(sx, sy, s_sunR, th_sun);
            for (int yy = -s_sunR; yy <= s_sunR; yy += 3) { int hw = (int)sqrtf((float)(s_sunR * s_sunR - yy * yy)); d.drawFastHLine(sx - hw, sy + yy, hw * 2, mix(th_sun, th_skyB, 64)); }
            d.fillCircle(sx, sy - 1, s_sunR / 2, mix(th_sun, COL_WHITE, 60));
        } else {                                                  // small sun (day/evening) with a soft glow halo
            d.fillCircle(sx, sy, s_sunR + 2, mix(sky_ramp(sy), th_glow, 120));
            d.fillCircle(sx, sy, s_sunR, th_sun);
            d.fillCircle(sx - 1, sy - 1, s_sunR / 2, mix(th_sun, COL_WHITE, 80));
        }
    }
    // ---- clouds (tinted; bigger and darker when raining) ----
    for (int i = 0; i < NCLOUD; i++) {
        float cx = s_cl_x[i] - s_cam * 0.25f - (s_anim * (0.2f + s_wind * 0.004f)); cx = fmodf(cx, W + 60); if (cx < -60) cx += W + 60;
        int cy = (int)s_cl_y[i] + oy; uint16_t cc = th_cloud; float ss = s_cl_s[i] * (s_rain ? 1.5f : 1.0f);
        d.fillRoundRect((int)cx, cy, (int)(22 * ss), 6, 3, cc); d.fillCircle((int)cx + (int)(8 * ss), cy + 1, 4, cc);
    }
    // ---- multi-layer parallax ridges ----
    float p1 = (s_cam + s_camfree) * 0.10f;
    for (int sx = 0; sx < W; sx++) {
        float wx = (sx + p1) * 0.7f;
        int ry = GTOP - 6 + (int)(5.0f * vn(wx, 60, s_seed + 11) + 3.0f * vn(wx, 23, s_seed + 12));
        d.drawFastVLine(sx, ry + oy, GTOP + 26 - ry, mix(COL_INK, th_skyB, 160));
    }
    float p2 = (s_cam + s_camfree) * 0.22f;
    for (int sx = 0; sx < W; sx++) {
        float wx = (sx + p2);
        int ry = GTOP - 2 + (int)(8.0f * vn(wx, 95, s_seed + 21) + 4.0f * vn(wx, 30, s_seed + 22));
        d.drawFastVLine(sx, ry + oy, GTOP + 30 - ry, mix(th_ground2, th_skyB, 120));   // calm silhouette (no white sparkle cap)
    }
}
static void draw_rain(int oy) {                                   // diagonal streaks over the whole scene
    uint16_t rc = mix(th_skyB, COL_WHITE, 90);
    int lean = (s_wind < 0) ? 1 : -1;
    for (int i = 0; i < 40; i++) {
        int x = (int)(h01(i, 5) * W);
        int y = ((int)(h01(i + 30, 5) * H) + s_anim * 11 + i * 7) % H;
        d.drawLine(x, y + oy, x + lean * 2, y + 5 + oy, rc);
    }
}
static void world_terrain(int ox, int oy) {
    int camx = (int)(s_cam + s_camfree);
    for (int sx = 0; sx < W; sx++) {
        int wx = camx + sx; if (wx < 0 || wx >= WW) continue;
        int sh = s_h[wx];                                       // WORLD surface row (camera-independent colour key)
        int gy = sh + oy; if (gy < 0) gy = 0; if (gy >= H) continue;   // gy = screen surface row
        // Soil CROSS-SECTION (Pocket-Tanks-style): grass rim -> topsoil -> soil -> subsoil -> rock,
        // shaded by DEPTH below the surface instead of one flat colour. A faint elevation darkening keeps
        // lower/back ground from glaring. Drawn as a handful of vertical segments (cheap).
        int elev = (sh - GTOP) * 64 / (H - GTOP); if (elev < 0) elev = 0; if (elev > 64) elev = 64;
        // Pocket-Tanks look: biome grass on top, BROWN dirt + dark rock below (dirt blends a touch of the
        // biome's deep tint so night/rain stay cohesive).
        uint16_t DIRT = mix(rgb(120, 82, 50), th_ground2, 60), ROCK = mix(rgb(64, 44, 30), th_ground2, 60);
        uint16_t topsoil = fx3d::scl(mix(th_ground, th_rim, 70),  256 - elev,     256);
        uint16_t soil    = fx3d::scl(mix(th_ground, DIRT, 120),   256 - elev,     256);
        uint16_t subsoil = fx3d::scl(mix(DIRT, ROCK, 90),         256 - elev / 2, 256);
        uint16_t rock    = ROCK;
        // Cratered / raised columns expose DIRT — no grass cap, no tufts (Pocket-Tanks craters).
        bool dug = s_dug[wx];
        uint16_t top1 = dug ? soil : topsoil;
        uint16_t rimc = dug ? fx3d::scl(DIRT, 205, 256) : th_rim;
        int y = gy, n;
        n = (gy + 3 <= H) ? 3  : H - gy; if (n > 0) { d.drawFastVLine(sx + ox, y, n, top1);   y += n; }
        n = (y  + 8 <= H) ? 8  : H - y;  if (n > 0) { d.drawFastVLine(sx + ox, y, n, soil);    y += n; }
        n = (y + 14 <= H) ? 14 : H - y;  if (n > 0) { d.drawFastVLine(sx + ox, y, n, subsoil); y += n; }
        if (y < H) d.drawFastVLine(sx + ox, y, H - y, rock);
        if (((wx * 7) & 7) == 0) { int sy2 = gy + 4 + ((wx * 13) & 7); if (sy2 < H) d.drawPixel(sx + ox, sy2, fx3d::scl(soil, 222, 256)); }  // dirt grain
        if (((wx * 131 + 5) & 15) == 0) { int py2 = gy + 13 + ((wx * 37) & 13); if (py2 < H - 1) { uint16_t pc = ((wx * 7) & 1) ? fx3d::scl(rock, 150, 256) : mix(subsoil, th_rim, 40); d.drawPixel(sx + ox, py2, pc); d.drawPixel(sx + ox, py2 + 1, fx3d::scl(pc, 170, 256)); } }  // embedded pebbles
        d.drawFastVLine(sx + ox, gy, 1, rimc);                                                // surface line (grass or bare dirt)
        if (gy + 1 < H) d.drawPixel(sx + ox, gy + 1, outline_for(rimc));
        if (!dug && s_atmo == ATM_DAY && (wx % 11) == 0 && gy - 1 >= 0) d.drawPixel(sx + ox, gy - 1, th_accent);        // grass tufts (pristine only)
        else if (s_atmo == ATM_RAIN && ((wx + (s_anim >> 3)) & 15) == 0) d.drawPixel(sx + ox, gy, mix(th_rim, COL_WHITE, 150)); // wet glint
    }
    // napalm flames flicker on the surface
    for (int s = 0; s < NFIRE; s++) if (s_fire[s].on)
        for (int dx = -s_fire[s].w; dx <= s_fire[s].w; dx += 2) {
            int wx = s_fire[s].x + dx; if (wx < 0 || wx >= WW) continue;
            int px = wx - camx + ox; if (px < 0 || px >= W) continue;
            int base = surf(wx) + oy;
            int py = base - (int)(3 + 4 * ((esp_random() & 7) / 7.0f));
            d.drawPixel(px, base, COL_INK);                                  // dark foot: flame reads on ANY ground
            int ph = (int)((sinf(dx * 0.6f + s_anim * 0.4f) + 1) * 128);
            d.drawPixel(px, py, mix(rgb(255, 70, 0), rgb(255, 240, 130), ph)); // hotter peak -> more separation
        }
    // palette-cycling molten scar at the last crater (flows for ~4s)
    if (s_lava_x >= 0 && now_ms() - s_lava_t < 4000) {
        int fade = 256 - (int)((now_ms() - s_lava_t) * 256 / 4000);
        for (int dx = -s_lava_w; dx <= s_lava_w; dx++) {
            int wx = s_lava_x + dx; if (wx < 0 || wx >= WW) continue;
            int px = wx - camx + ox, py = surf(wx) + oy + 1; if (px < 0 || px >= W) continue;
            int ph = (int)(128 + 127 * sinf(dx * 0.5f + s_anim * 0.25f));
            uint16_t hot = rgb(255, 120, 30);
            d.drawPixel(px, py, mix(COL_INK, mix(hot, COL_WHITE, ph), fade));
            if (ph > 200 && py - 1 >= 0) d.drawPixel(px, py - 1, mix(COL_INK, COL_WHITE, fade));
        }
    }
    if (s_rain) draw_rain(oy);                                       // rain falls in front of the terrain
}
// --- body-tilt helpers: rotate a hull-local offset about the track pivot, and fill a rotated bar ---
static inline int rtx(float dx, float dy, float c, float s, int px) { return px + (int)lroundf(dx * c - dy * s); }
static inline int rty(float dx, float dy, float c, float s, int py) { return py + (int)lroundf(dx * s + dy * c); }
static void rbar(int px, int py, float c, float s, float x0, float y0, float x1, float y1, uint16_t col) {
    int ax = rtx(x0, y0, c, s, px), ay = rty(x0, y0, c, s, py), bx = rtx(x1, y0, c, s, px), by = rty(x1, y0, c, s, py);
    int cx = rtx(x1, y1, c, s, px), cy = rty(x1, y1, c, s, py), dx = rtx(x0, y1, c, s, px), dy = rty(x0, y1, c, s, py);
    d.fillTriangle(ax, ay, bx, by, cx, cy, col); d.fillTriangle(ax, ay, cx, cy, dx, dy, col);
}
static void draw_tank(int i, int camx, int ox, int oy) {
    int sx = (int)s_tk[i].x - camx + ox; if (sx < -20 || sx > W + 20) return;
    int sy = (int)s_tk[i].y + oy; uint16_t col = i ? COL_P1 : COL_P0;
    uint16_t obg = bg_at((int)s_tk[i].x, (int)s_tk[i].y), olc = outline_for(obg);   // background-adaptive outline
    d.fillEllipse(sx, sy + 5, TANK_W / 2 + 1, 2, COL_INK);          // ground shadow stays flat on the terrain
    // ---- hull tilts to the LOCAL ground slope so the tank sits ON the hill instead of floating flat over it ----
    int xl = clampi((int)s_tk[i].x - TANK_W / 2, 0, WW - 1), xr = clampi((int)s_tk[i].x + TANK_W / 2, 0, WW - 1);
    float tilt = clampf(atan2f((float)((int)s_h[xr] - (int)s_h[xl]), (float)(xr - xl)), -0.5f, 0.5f);
    float ct = cosf(tilt), st = sinf(tilt);
    int px = sx, py = sy + 3, HW = TANK_W / 2;                       // pivot on the track line
    // ---- running gear: dark track band + evenly spaced road wheels (reads as a tread, not a flat bar) ----
    rbar(px, py, ct, st, -(HW + 1), -2, (HW + 1), 4, olc);                                  // outer silhouette
    rbar(px, py, ct, st, -HW, -1, HW, 3, mix(COL_INK, col, 45));                            // track band
    for (int t = 0; t < 5; t++) { int wxo = -(HW - 2) + t * (HW - 2) / 2;
        d.fillCircle(rtx(wxo, 2, ct, st, px), rty(wxo, 2, ct, st, py), 1, mix(col, COL_WHITE, 70)); }   // road wheels
    // ---- hull: 3D shaded body + a bright glacis edge on top ----
    fx3d::draw_model(CUBE, rtx(0, -3, ct, st, px), rty(0, -3, ct, st, py), TANK_W * 0.30f, 0.5f, tilt, mix(col, COL_INK, 70));
    rbar(px, py, ct, st, -HW + 1, -6, HW - 1, -4, mix(col, COL_WHITE, 60));                 // glacis highlight
    // ---- turret: rounded dome with a top-left highlight + dark rim ----
    int turx = rtx(0, -7, ct, st, px), tury = rty(0, -7, ct, st, py);
    d.fillCircle(turx, tury, 6, olc);                                                       // dark halo so it reads on any bg
    d.fillCircle(turx, tury, 5, col);
    d.fillCircle(turx - 1, tury - 2, 2, mix(col, COL_WHITE, 120));                          // sheen
    d.drawCircle(turx, tury, 5, mix(col, COL_INK, 70));
    // ---- barrel: tapered, exits a mantlet, capped by a muzzle brake (absolute aim angle) ----
    float e = s_tk[i].elev * (float)M_PI / 180.0f, dir = (i == 0) ? 1.0f : -1.0f;
    float ca = cosf(e) * dir, sa = -sinf(e);
    int bx0 = turx + (int)(ca * 3), by0 = tury + (int)(sa * 3);                             // gun root at the mantlet
    int ex  = turx + (int)(ca * BARREL), ey = tury + (int)(sa * BARREL);
    d.drawLine(bx0, by0 - 1, ex, ey - 1, olc); d.drawLine(bx0, by0 + 1, ex, ey + 1, olc);   // dark edges (thickness)
    d.drawLine(bx0, by0, ex, ey, mix(col, COL_WHITE, 130));                                 // bright core
    d.fillCircle(ex, ey, 2, mix(col, COL_WHITE, 150)); d.drawCircle(ex, ey, 2, olc);        // muzzle brake
    // bubble shield dome — pulses, fades as it depletes
    if (s_tk[i].shield > 0) {
        int rad = 12 + (int)(2 * sinf(s_anim * 0.25f)), str = clampi(s_tk[i].shield * 256 / 45, 60, 256);
        d.drawCircle(sx, sy - 2, rad + 1, COL_INK);                 // dark halo so the dome reads on snow / pale sky
        d.drawCircle(sx, sy - 2, rad, mix(COL_INK, COL_WHITE, str));
        d.drawCircle(sx, sy - 2, rad - 1, mix(COL_INK, rgb(150, 215, 255), str / 2));
        for (int a = 0; a < 8; a++) { float an = a * 0.785f + s_anim * 0.1f; d.drawPixel(sx + (int)(cosf(an) * rad), sy - 2 + (int)(sinf(an) * rad), mix(COL_INK, rgb(180, 230, 255), str)); }
    }
    if (i == s_active && s_phase == TP_AIM) {
        int a = (int)(2 + 2 * sinf(s_anim * 0.3f));
        d.fillTriangle(sx - 5, sy - 23 - a, sx + 5, sy - 23 - a, sx, sy - 16 - a, olc);
        d.fillTriangle(sx - 4, sy - 22 - a, sx + 4, sy - 22 - a, sx, sy - 17 - a, col);
    }
}
static void draw_traj(int camx, int ox, int oy) {
    int who = s_active;
    float e = s_tk[who].elev * (float)M_PI / 180.0f, dir = (who == 0) ? 1.0f : -1.0f, v = v0_of(s_tk[who].power);
    float x = s_tk[who].x + cosf(e) * dir * BARREL, y = s_tk[who].y - 3 - sinf(e) * BARREL;
    float vx = cosf(e) * dir * v, vy = -sinf(e) * v, g = 0.00040f, wind = s_wind / 100.0f * WIND_ACCEL;
    float ix = x, iy = y; bool landed = false;
    for (int n = 0; n < 30; n++) {
        for (int s = 0; s < 6; s++) { vy += g * 16; vx += wind * 16; x += vx * 16; y += vy * 16; }
        ix = x; iy = y;
        if (x < 0 || x > WW - 1) break;                               // flew off the field: no ground marker
        if (y >= surf((int)x)) { iy = surf((int)x); landed = true; break; }
        int px = (int)x - camx + ox, py = (int)y + oy;
        if (px >= 0 && px < W && py >= 0 && py < H && (n & 1)) {
            d.drawPixel(px, py, outline_for(bg_at((int)x, (int)y)));   // dark on snow, light on night
            d.drawPixel(px, py - 1, mix(th_glow, COL_WHITE, 90));      // bright core
        }
    }
    // predicted-impact reticle: a small crosshair where the arc meets the ground, so the aim reads as a
    // clear TARGET instead of trailing off into pixels. Player-coloured, outlined to pop on any terrain.
    if (landed) {
        int mx = (int)ix - camx + ox, my = (int)iy + oy;
        if (mx >= -6 && mx < W + 6 && my >= 0 && my < H) {
            uint16_t mc = who ? COL_P1 : COL_P0;
            d.drawFastHLine(mx - 5, my, 11, COL_INK); d.drawFastVLine(mx, my - 5, 11, COL_INK);   // dark cross (readable on snow)
            d.drawFastHLine(mx - 4, my, 9, mc);       d.drawFastVLine(mx, my - 4, 9, mc);
            d.drawCircle(mx, my, 4, COL_INK); d.drawCircle(mx, my, 3, mc);
            d.drawPixel(mx, my, COL_WHITE);
        }
    }
}
static void draw_nuke(int camx, int ox, int oy) {
    if (s_nuke_t <= 0) return;
    float p = 1.0f - (float)s_nuke_t / NUKE_MS;                 // 0 -> 1 as the cloud rises
    int bx = (int)s_nuke_x - camx + ox, by = (int)s_nuke_y + oy, colh = (int)(p * 62);   // taller column
    for (int yy = 0; yy < colh; yy++) {                         // glowing stem, fat at the base, tapering up
        int t = yy * 256 / (colh + 1), hw = 2 + (yy * 4) / (colh + 1);
        d.drawFastHLine(bx - hw, by - yy, hw * 2, mix(COL_WHITE, rgb(255, 130, 30), t));
    }
    int capy = by - colh, cap = (int)(7 + p * 22);              // big billowing cap
    d.fillCircle(bx, capy, cap + 3, mix(rgb(255, 170, 70), rgb(55, 28, 18), (int)(p * 256)));     // dark smoke halo
    d.fillCircle(bx, capy, cap, mix(rgb(255, 190, 90), rgb(180, 60, 30), (int)(p * 256)));        // fireball cap
    d.fillCircle(bx - cap / 2, capy + 1, cap / 2, mix(rgb(255, 200, 110), rgb(150, 50, 25), (int)(p * 220)));  // billow lobes
    d.fillCircle(bx + cap / 2, capy + 1, cap / 2, mix(rgb(255, 200, 110), rgb(150, 50, 25), (int)(p * 220)));
    d.fillCircle(bx, capy - 1, cap / 2, mix(COL_WHITE, rgb(255, 210, 130), (int)(p * 200)));      // hot core
}
static void draw_projectiles(int camx, int ox, int oy) {
    for (int i = 0; i < NPROJ; i++) {
        if (!s_pr[i].on) continue;
        Proj *p = &s_pr[i];
        int px = (int)p->x - camx + ox, py = (int)p->y + oy;
        if (px < -8 || px > W + 8) continue;
        uint16_t wc = WEAPS[p->wp].col, obg = bg_at((int)p->x, (int)p->y), olc = outline_for(obg);
        int beh = p->beh;
        float ang = atan2f(p->vy, p->vx);
        int nx = (int)(cosf(ang) * 4), ny = (int)(sinf(ang) * 4);   // unit-ish heading, 4px

        // --- signature TRAIL: napalm smokes fire, rockets leave exhaust, others a tinted comet streak ---
        int tn = (beh == WB_HOMING || beh == WB_NAPALM || WEAPS[p->wp].crater >= 20) ? 6 : 5;
        for (int t = tn; t >= 1; t--) {
            float f = (float)t / tn;
            int tx2 = px - (int)(p->vx * t * 3.4f), ty2 = py - (int)(p->vy * t * 3.4f);
            uint16_t tc = (beh == WB_NAPALM) ? mix(rgb(255, 90, 20), COL_INK, (int)(f * 210))
                        : (beh == WB_HOMING) ? mix(rgb(255, 210, 110), COL_INK, (int)(f * 210))
                        :                      mix(COL_INK, wc, 55 + (int)((1 - f) * 150));
            d.fillCircle(tx2, ty2, t > 3 ? 1 : 2, tc);
        }

        if (beh == WB_ROLLER && p->rolling) {                        // spinning ground ball with a rotating pip
            d.fillCircle(px, py - 2, 3, olc); d.fillCircle(px, py - 2, 2, wc);
            float r = s_anim * 0.6f;
            d.drawPixel(px + (int)(cosf(r) * 1.4f), py - 2 + (int)(sinf(r) * 1.4f), mix(wc, COL_INK, 150));
            continue;
        }
        if (beh == WB_BOUNCE) {                                      // tumbling grenade: body + a spinning seam + pin
            float r = s_anim * 0.5f; int ex = (int)(cosf(r) * 3), ey = (int)(sinf(r) * 3);
            d.fillCircle(px, py, 3, olc); d.fillCircle(px, py, 2, mix(wc, COL_INK, 30));
            d.drawLine(px - ex, py - ey, px + ex, py + ey, mix(wc, COL_WHITE, 120));
            d.drawPixel(px + ey / 3, py - ex / 3, COL_WHITE);
            continue;
        }
        if (beh == WB_HOMING) {                                      // guided rocket: flame tail + nosed body
            int fl = 2 + ((s_anim) & 1);
            d.fillCircle(px - nx, py - ny, fl, rgb(255, 170, 50));
            d.fillCircle(px - nx, py - ny, 1, COL_WHITE);
            d.fillCircle(px, py, 3, olc); d.fillCircle(px, py, 2, wc);
            d.fillCircle(px + nx / 2, py + ny / 2, 1, COL_WHITE);
            continue;
        }
        if (beh == WB_NAPALM) {                                      // fireball with a flickering hot core
            d.fillCircle(px, py, 3, olc); d.fillCircle(px, py, 2, rgb(255, 110, 30));
            d.fillCircle(px, py, 1, ((s_anim) & 1) ? COL_WHITE : rgb(255, 220, 120));
            continue;
        }
        // default shell: adaptive outline ring + glow + hot core; heavy shells tumble a 3D cube
        d.fillCircle(px, py, 3, olc);
        d.fillCircle(px, py, 2, PROJ_GLOW);
        d.fillCircle(px, py, 1, PROJ_CORE);
        if (WEAPS[p->wp].crater >= 20) fx3d::draw_model(CUBE, px, py, 2.2f, s_anim * 0.4f, s_anim * 0.25f, wc);
    }
}
// The alien saucer: hull disc + glowing dome + blinking rim lights, a charging orb underneath, and the
// vertical death-ray during the LASER phase. All primitives, no assets, RAM 0.
static void ufo_draw(int camx, int ox, int oy) {
    if (!s_ufo.on) return;
    Ufo *u = &s_ufo;
    int px = (int)u->x - camx + ox, py = (int)u->y + oy;
    if (px < -30 || px > W + 30) return;
    uint16_t glow = rgb(120, 255, 180), hull = rgb(92, 106, 132), hull2 = rgb(158, 178, 208);

    // Death-ray: thick wobbling green column from the saucer belly straight down onto the foe.
    if (u->phase == UF_LASER) {
        int fx = (int)s_tk[u->foe].x - camx + ox, fy = (int)s_tk[u->foe].y + oy;
        int top = py + 4, hgt = fy - top; if (hgt < 0) hgt = 0;
        int bw = 5 + (int)(sinf(s_anim * 0.9f) * 1.5f);
        for (int b = bw; b >= 1; b--) {
            uint16_t c = (b > 2) ? mix(COL_INK, rgb(80, 255, 150), 90) : mix(glow, COL_WHITE, 120);
            d.fillRect(px - b, top, 2 * b, hgt, c);
        }
        d.fillCircle(fx, fy, 6 + ((s_anim >> 1) & 3), mix(COL_INK, rgb(160, 255, 200), 120));   // impact bloom
    }
    // Charging orb while hovering.
    if (u->phase == UF_HOVER) d.fillCircle(px, py + 4, 1 + u->t / 120, mix(COL_INK, glow, 120));

    d.fillEllipse(px, py, 13, 5, hull);                          // saucer disc
    d.fillEllipse(px, py - 1, 13, 4, hull2);                     // top highlight band
    d.drawEllipse(px, py, 13, 5, mix(hull, COL_INK, 120));
    d.fillCircle(px, py - 4, 5, mix(glow, hull2, 90));           // dome
    d.fillCircle(px, py - 5, 3, mix(glow, COL_WHITE, 110));      // dome shine
    for (int k = -2; k <= 2; k++) {                              // blinking rim running-lights
        uint16_t lc = ((s_anim + k) & 3) ? mix(COL_INK, glow, 90) : rgb(255, 220, 120);
        d.fillCircle(px + k * 5, py + 3, 1, lc);
    }
    if (u->phase >= UF_HOVER && u->phase <= UF_LASER)            // pulsing belly emitter
        d.fillCircle(px, py + 4, 2, mix(glow, COL_WHITE, (s_anim & 1) ? 120 : 60));
}
// HUD chrome visibility: solid right after you touch the keys, then fades to hand the panel back
// to the battlefield (smartwatch "glance, then clear"). Hidden during the action phases; while
// watching the opponent/CPU aim it stays dimly up (informative). 0..255.
static int hud_alpha(void) {
    if (s_phase != TP_AIM) return 0;
    if (!local_active())   return 205;
    int idle = (int)(now_ms() - s_hud_t);
    if (idle < HUD_SHOW_MS) return 255;
    if (idle < HUD_SHOW_MS + HUD_FADE_MS) return 255 - (idle - HUD_SHOW_MS) * 255 / HUD_FADE_MS;
    return 0;
}
// The two numbers you actually aim with — BIG and double-outlined so they read over any terrain.
// Stays up through the whole aim (even once the chrome has faded) so the scene gets the screen
// without ever leaving the player aiming blind. Smartwatch glanceability: huge digits, tiny labels.
static void draw_glance(void) {
    int me = s_active; char b[8];
    int gy = H - 33;
    // ANGLE tile (left): big gold digits on a solid dark plate -> always legible over any terrain
    snprintf(b, sizeof b, "%d", s_tk[me].elev);
    int aw = (int)strlen(b) * 18 + 12;
    d.fillRoundRect(8, gy - 10, aw, 34, 5, mix(COL_INK, COL_DIM, 26));
    d.drawRoundRect(8, gy - 10, aw, 34, 5, mix(COL_GOLD, COL_INK, 150));
    txt(14, gy - 8, 1, COL_GOLD, "ANG");
    txt(14, gy + 2, 3, COL_GOLD, b);
    // POWER tile (right): digits in the live green->red power colour, same plate treatment
    snprintf(b, sizeof b, "%d", s_tk[me].power);
    uint16_t pc = mix(COL_GREEN, COL_RED, s_tk[me].power * 256 / 100);
    int pw = (int)strlen(b) * 18 + 12;
    d.fillRoundRect(W - 8 - pw, gy - 10, pw, 34, 5, mix(COL_INK, COL_DIM, 26));
    d.drawRoundRect(W - 8 - pw, gy - 10, pw, 34, 5, mix(pc, COL_INK, 150));
    txt_r(W - 14, gy - 8, 1, pc, tx("POT", "PWR"));
    txt_r(W - 14, gy + 2, 3, pc, b);
}
// Wind: a bold arrow that points DOWNWIND, its length AND colour scaling with strength
// (green = light, gold = moderate, orange = strong, red = gale). Shown top-centre throughout
// aiming (it's aim-critical), outlined so it reads over sky or terrain. Replaces the old faint tick.
static void draw_wind(void) {
    int th = s_wind < 0 ? -s_wind : s_wind;
    uint16_t wc = th < 14 ? COL_GREEN : th < 32 ? COL_GOLD : th < 52 ? rgb(255, 140, 50) : COL_RED;
    int cx = W / 2, ay = 12;
    txt_c(cx + 1, 1, 1, COL_INK, tx("VENTO", "WIND")); txt_c(cx, 0, 1, mix(COL_INK, HUD_TEXT2, 230), tx("VENTO", "WIND"));
    if (th < 3) { txt_c(cx, 8, 1, COL_GREEN, tx("calmo", "calm")); return; }
    int dir = s_wind < 0 ? -1 : 1, len = 12 + th * 30 / 70; if (len > 42) len = 42;
    int x0 = cx - dir * len / 2, x1 = cx + dir * len / 2, xl = x0 < x1 ? x0 : x1;
    d.fillRect(xl, ay - 1, len, 4, COL_INK);                                  // shaft outline
    d.fillRect(xl, ay, len, 2, wc);
    d.fillTriangle(x1 - dir * 7, ay - 5, x1 - dir * 7, ay + 6, x1 + dir * 3, ay, COL_INK);   // head outline
    d.fillTriangle(x1 - dir * 6, ay - 3, x1 - dir * 6, ay + 4, x1 + dir * 2, ay, wc);
}
static void draw_hud(void) {
    int a = hud_alpha();
    int me = s_active; uint16_t ac = me ? COL_P1 : COL_P0;
    char buf[40];
    // side HP gauges — always on (6 px each edge), bordered with a 50% tick. As the HUD chrome fades
    // the chip + gauge slide UP into the reclaimed top strip and the bar grows downward into the freed
    // power-bar row, so the life readout always uses the space the HUD just handed back.
    uint16_t c0 = s_tk[0].hp > 40 ? COL_P0 : COL_RED, c1 = s_tk[1].hp > 40 ? COL_P1 : COL_RED;
    int chipy = 3 + a * 18 / 255;                            // a=255 -> 21 (under top bar); a=0 -> 3 (top edge)
    int gtop  = chipy + 18, gbot = (a > 6) ? H - 12 : H - 3, HHP = gbot - gtop;
    d.fillRect(0, gtop, 6, HHP, rgb(22, 26, 38));
    int h0 = s_tk[0].hp * HHP / 100; d.fillRect(0, gtop + (HHP - h0), 6, h0, c0);
    d.drawFastHLine(0, gtop + HHP / 2, 6, mix(c0, COL_INK, 140)); d.drawRect(0, gtop, 6, HHP, rgb(52, 58, 78));
    d.fillRect(W - 6, gtop, 6, HHP, rgb(22, 26, 38));
    int h1 = s_tk[1].hp * HHP / 100; d.fillRect(W - 6, gtop + (HHP - h1), 6, h1, c1);
    d.drawFastHLine(W - 6, gtop + HHP / 2, 6, mix(c1, COL_INK, 140)); d.drawRect(W - 6, gtop, 6, HHP, rgb(52, 58, 78));
    snprintf(buf, sizeof buf, "%d", s_tk[0].hp);
    { int cw = (int)strlen(buf) * 12 + 7; d.fillRoundRect(0, chipy, cw, 17, 3, COL_INK); d.drawRoundRect(0, chipy, cw, 17, 3, c0); txt(4, chipy + 2, 2, c0, buf); }
    snprintf(buf, sizeof buf, "%d", s_tk[1].hp);
    { int cw = (int)strlen(buf) * 12 + 7; d.fillRoundRect(W - cw, chipy, cw, 17, 3, COL_INK); d.drawRoundRect(W - cw, chipy, cw, 17, 3, c1); txt_r(W - 4, chipy + 2, 2, c1, buf); }
    // top bar — fades toward the sky behind it
    if (a > 6) {
        uint16_t sky = sky_ramp(9);
        d.fillRect(0, 0, W, 18, mix(sky, HUD_BG, a));
        d.fillRoundRect(2, 2, 30, 14, 3, mix(sky, ac, a)); snprintf(buf, sizeof buf, "P%d", me + 1); txt(7, 3, 2, mix(sky, COL_INK, a), buf);
        const Weap *w = &WEAPS[s_tk[me].weap];
        { char wn[16]; snprintf(wn, sizeof wn, "%s", tx(w->it, w->en)); if ((int)strlen(wn) > 10) wn[10] = 0; txt(36, 1, 1, mix(sky, w->col, a), wn); }
        if (s_tk[me].ammo[s_tk[me].weap] >= 0) { snprintf(buf, sizeof buf, "%s x%d", tx("mun", "ammo"), s_tk[me].ammo[s_tk[me].weap]); txt(36, 10, 1, mix(sky, HUD_TEXT2, a), buf); }
        else txt(36, 10, 1, mix(sky, COL_DIM, a), tx("illim.", "unltd"));
    }
    // wind: bold directional arrow, top-centre, always up during aiming (drawn after the bar so it stays on top)
    if (s_phase == TP_AIM) draw_wind();
    // bottom power bar — fades toward the ground; the NUMBER lives in the always-on glance
    if (a > 6) {
        uint16_t gnd = th_ground;
        d.fillRect(0, H - 9, W, 9, mix(gnd, HUD_BG, a));
        d.fillRect(3, H - 7, W - 6, 4, mix(gnd, rgb(40, 44, 60), a));
        d.fillRect(3, H - 7, (W - 6) * s_tk[me].power / 100, 4, mix(gnd, mix(COL_GREEN, COL_RED, s_tk[me].power * 256 / 100), a));
        for (int q = 1; q < 4; q++) d.drawFastVLine(3 + (W - 6) * q / 4, H - 7, 4, mix(gnd, COL_INK, a));   // 25/50/75% reference ticks -> power readable at a glance
    }
    // manual numeric entry owns the bottom card; otherwise the big glance shows angle/power
    if (g_manual && local_active() && s_phase == TP_AIM) {
        d.fillRoundRect(W / 2 - 78, H - 34, 156, 24, 5, mix(COL_INK, COL_DIM, 70)); d.drawRoundRect(W / 2 - 78, H - 34, 156, 24, 5, ac);
        txt(W / 2 - 72, H - 31, 1, HUD_TEXT2, "ANG"); txt(W / 2 - 50, H - 32, 3, s_entry_field == 0 ? COL_WHITE : COL_GOLD, s_entry_ang);
        txt(W / 2 + 8, H - 31, 1, HUD_TEXT2, tx("POT", "PWR")); txt(W / 2 + 40, H - 32, 3, s_entry_field == 1 ? COL_WHITE : COL_GOLD, s_entry_pow);
    } else if (s_phase == TP_AIM) {
        draw_glance();
    }
    // who's aiming
    if (s_mode == MODE_AI && s_active == 1 && s_phase == TP_AIM) txt_c(W / 2, 21, 1, ((s_anim >> 2) & 1) ? COL_WHITE : ac, tx("la CPU mira...", "CPU aiming..."));
    else if (s_mode != MODE_AI && !local_active() && s_phase == TP_AIM) txt_c(W / 2, 21, 1, ((s_anim >> 2) & 1) ? COL_WHITE : ac, tx("avversario mira...", "opponent aiming..."));
}
static void draw_wcard(void) {     // weapon picker card: pops on weapon change so the choice is BIG and legible, not blind
    if (!local_active() || s_phase != TP_AIM || now_ms() >= s_wbar_t) return;
    int me = s_active, wsel = s_tk[me].weap; const Weap *cw = &WEAPS[wsel];
    // main card — large weapon name on a solid plate
    int pw = 176, px = W / 2 - pw / 2, py = 28;
    d.fillRoundRect(px, py, pw, 30, 7, mix(COL_INK, COL_DIM, 46));
    d.drawRoundRect(px, py, pw, 30, 7, cw->col);
    d.fillRoundRect(px + 6, py + 6, 12, 18, 3, cw->col);                          // colour swatch
    char nm[16]; snprintf(nm, sizeof nm, "%s", tx(cw->it, cw->en)); if ((int)strlen(nm) > 11) nm[11] = 0;
    txt(px + 25, py + 6, 2, COL_WHITE, nm);                                       // BIG name (size 2)
    char ab[18]; if (s_tk[me].ammo[wsel] >= 0) snprintf(ab, sizeof ab, "%s x%d", tx("mun", "ammo"), s_tk[me].ammo[wsel]); else snprintf(ab, sizeof ab, "%s", tx("illimitate", "unlimited"));
    txt(px + 25, py + 22, 1, s_tk[me].ammo[wsel] == 0 ? COL_RED : HUD_TEXT2, ab);
    int slots = s_nslot[me]; txt_r(px + pw - 7, py + 22, 1, mix(cw->col, COL_WHITE, 60), weap_kind(wsel));   // behaviour tag (WHAT it does) — clearer than a raw arsenal index
    uint16_t hc = ((s_anim >> 2) & 1) ? COL_WHITE : COL_DIM;                      // Q/W cycle hints
    txt(px - 9, py + 12, 1, hc, "Q"); txt(px + pw + 3, py + 12, 1, hc, "W");
    // 1..0 quick-slot ribbon: the ten LOADOUT slots (fixed order), coloured by weapon, current highlighted,
    // depleted slots greyed -> teaches the number shortcuts and shows the arsenal at a glance.
    int rx = W / 2 - 100, ry = py + 36;
    for (int i = 0; i < slots; i++) {
        int wp = s_slot[me][i], cx = rx + i * 20 + 10; bool cur = (wp == wsel), empty = (s_tk[me].ammo[wp] == 0);
        uint16_t pc = empty ? rgb(50, 54, 68) : WEAPS[wp].col;
        d.fillRoundRect(cx - 9, ry, 18, 15, 3, cur ? pc : mix(COL_INK, pc, 95));
        if (cur) d.drawRoundRect(cx - 10, ry - 1, 20, 17, 4, COL_WHITE);
        char dg[2] = { (char)(i < 9 ? '1' + i : '0'), 0 };
        txt_c(cx, ry + 4, 1, cur ? COL_INK : (empty ? COL_DIM : COL_WHITE), dg);
    }
}
// Multiplayer turn clock (net only, our turn): a calm chip while there's time, then a BIG throbbing 5..1
// countdown that swells and pulses as the clock runs out, ending on a "VIA" beat as the shot auto-launches.
static void draw_turn_timer(void) {
    if (s_mode == MODE_AI || !local_active() || s_phase != TP_AIM) return;
    int rem = (int)(s_aim_deadline - now_ms());
    if (rem <= -400) return;                                     // shot already gone
    int secs = rem > 0 ? (rem + 999) / 1000 : 0;
    if (secs > 5) {                                             // calm chip, top-centre
        char b[8]; snprintf(b, sizeof b, "0:%02d", secs);
        uint16_t col = secs <= 10 ? COL_GOLD : COL_CYAN;
        int bw = 34, bx = W / 2 - bw / 2, by = 24;
        d.fillRoundRect(bx, by, bw, 14, 4, mix(COL_INK, col, 40));
        d.drawRoundRect(bx, by, bw, 14, 4, mix(col, COL_INK, 80));
        txt_c(W / 2, by + 3, 1, col, b);
        return;
    }
    // final 5 seconds + the VIA beat: size grows as seconds fall, with a per-second heartbeat kick
    char nb[6]; const char *lbl;
    if (secs >= 1) { snprintf(nb, sizeof nb, "%d", secs); lbl = nb; } else lbl = "VIA";
    int frac = rem > 0 ? (rem % 1000) : (400 + rem);            // ms remaining inside the current beat
    int beat = (frac > 780) ? 1 : 0;                           // swell at the top of each second
    int base = secs >= 1 ? (4 + (5 - secs)) : 7;               // 5s->4 ... 1s->8 ; VIA->7
    int sz = clampi(base + beat, 3, 9);
    uint16_t col = (secs == 0) ? COL_GOLD : (beat ? COL_WHITE : COL_RED);
    int cyv = 50, cx = W / 2, cyc = cyv + sz * 4;
    d.drawCircle(cx, cyc, beat ? 24 : 13, mix(COL_INK, col, 120));   // throbbing ring
    txt_c(cx + 2, cyv + 2, sz, COL_INK, lbl);                       // drop shadow
    txt_c(cx, cyv, sz, col, lbl);
}
// Screen-wide explosion flash. It was set by flash() but NEVER drawn, so blasts had no punch. Peak intensity
// = solid white-out (the nuke "schermo bianco"); as it decays it dissolves into a Bayer-dithered veil so the
// battlefield bleeds back in instead of cutting hard. Cheap: one fillScreen at the peak, a 4x4 ordered dither
// only during the short fade.
static void draw_flash(void) {
    // ONLY a nuke-class flash (peak>=200) veils the whole screen. Small blasts previously dithered ~40% of
    // EVERY pixel on EVERY shot -> a full-screen speckle that read as flicker. They now rely on their local
    // sparks/rings/shatter for punch; the screen itself is left alone.
    if (s_flash <= 0 || s_flashmax <= 0 || s_flashpeak < 200) return;
    int fa = s_flash * s_flashpeak / s_flashmax;               // 0..peak
    if (fa <= 0) return;
    if (fa >= 232) { d.fillScreen(s_flashcol); return; }       // blinding peak (nuke white-out)
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (BAYER4[((y & 3) << 2) | (x & 3)] * 16 + 8 < fa) d.drawPixel(x, y, s_flashcol);
}
static void draw_play(void) {
    int ox = 0, oy = (int)s_camY;
    if (s_shake > 0.4f) { ox += (int)frnd(-s_shake, s_shake); oy += (int)frnd(-s_shake, s_shake); }
    int camx = (int)(s_cam + s_camfree);
    world_sky(oy);
    if (oy < 0) d.fillRect(0, H + oy, W, -oy, th_ground2);   // cover the strip a downward/shake look exposes
    world_terrain(ox, oy);
    draw_tank(0, camx, ox, oy); draw_tank(1, camx, ox, oy);
    if (s_phase == TP_AIM && g_aimhelp) draw_traj(camx, ox, oy);   // telegraph human AND CPU aim
    for (int i = 0; i < NRING; i++) {
        if (!s_rg[i].on) continue;
        int a = s_rg[i].life * 256 / s_rg[i].lmax, X = (int)s_rg[i].x - camx + ox, Y = (int)s_rg[i].y + oy, R = (int)s_rg[i].r;
        d.drawCircle(X, Y, R, mix(COL_INK, s_rg[i].col, a));
        if (R > 3) d.drawCircle(X, Y, R - 1, mix(COL_INK, s_rg[i].col, a / 2));
    }
    draw_projectiles(camx, ox, oy);
    if (s_beam_t > 0) {                                  // laser beam streak
        int a = s_beam_t * 256 / 240;
        d.drawLine(s_beam_x0 - camx + ox, s_beam_y0 + oy, s_beam_x1 - camx + ox, s_beam_y1 + oy, mix(COL_INK, rgb(255, 120, 200), a));
        d.drawLine(s_beam_x0 - camx + ox, s_beam_y0 + oy - 1, s_beam_x1 - camx + ox, s_beam_y1 + oy - 1, mix(COL_INK, COL_WHITE, a / 2));
    }
    ufo_draw(camx, ox, oy);
    for (int i = 0; i < NSHAT; i++) {
        if (!s_sh[i].on) continue;
        int px = (int)s_sh[i].x - camx + ox, py = (int)s_sh[i].y + oy;
        fx3d::shatter(CUBE, (float)px, (float)py, s_sh[i].sc, s_sh[i].yaw, 0.0f, s_sh[i].col, (float)s_sh[i].t / s_sh[i].dur);
    }
    draw_nuke(camx, ox, oy);
    for (int i = 0; i < NSPK; i++) {
        if (s_spk[i].life <= 0) continue;
        int f = s_spk[i].life * 256 / (s_spk[i].max > 0 ? s_spk[i].max : 1);
        int sf = th_night && f < 90 ? 90 : f;
        d.fillCircle((int)s_spk[i].x - camx + ox, (int)s_spk[i].y + oy, f > 130 ? 2 : 1, mix(COL_INK, s_spk[i].col, sf));
    }
    draw_flash();                                               // explosion white-out / coloured blast veil, on top of the scene
    for (int i = 0; i < NDMG; i++) {
        if (!s_dp[i].on) continue;
        char b[24];
        if (s_dp[i].msg) snprintf(b, sizeof b, "%s +%d", s_dp[i].msg, s_dp[i].val);   // juicy bonus banner
        else             snprintf(b, sizeof b, "-%d", s_dp[i].val);                   // plain damage number
        int dpx = (int)s_dp[i].x - camx + ox, dpy = (int)s_dp[i].y + oy, da = s_dp[i].life * 256 / 900;
        txt_c(dpx + 1, dpy + 1, 1, COL_INK, b);                      // drop shadow, matches HUD HP text
        txt_c(dpx, dpy, 1, mix(COL_INK, s_dp[i].col, da), b);
    }
    draw_hud();
    draw_wcard();
    draw_turn_timer();
    // net YOUR-TURN call-out: a brief bold banner the instant the handoff lands on us, so a remote
    // player never misses that it's their move (paired with the chirp fired in start_turn).
    if (s_yt_t > now_ms() && s_mode != MODE_AI && local_active()) {
        uint16_t c = (s_seat == 0) ? COL_P0 : COL_P1;
        const char *t = tx("TOCCA A TE", "YOUR TURN");
        int tw = (int)strlen(t) * 12, bx = W / 2 - tw / 2 - 8, by = 40;
        d.fillRoundRect(bx, by, tw + 16, 20, 5, mix(COL_INK, c, 70));
        d.drawRoundRect(bx, by, tw + 16, 20, 5, ((s_anim >> 2) & 1) ? COL_WHITE : c);
        txt_c(W / 2 + 1, by + 4, 2, COL_INK, t);
        txt_c(W / 2,     by + 3, 2, c, t);
    }
    // turn-change diagonal wipe in the new player's colour
    if (s_phase == TP_TURN) {
        float t = clampf((1050 - s_turn_t) / 350.0f, 0, 1);
        int edge = (int)(t * (W + H)); uint16_t wcol = s_active ? COL_P1 : COL_P0;
        for (int y = 0; y < H; y += 4) if ((edge - y) < W) d.fillRect(clampi(edge - y, 0, W), y, W, 4, mix(COL_INK, wcol, 40));
        char tb[24]; snprintf(tb, sizeof tb, "%s P%d", tx("TURNO", "TURN"), s_active + 1);
        if (t > 0.5f) { txt_c(W / 2 + 1, 47, 2, COL_INK, tb); txt_c(W / 2, 46, 2, COL_WHITE, tb); }
    }
    // leave-match confirm modal (Esc in-match) — never bail out of a game by accident
    if (s_confirm) {
        for (int y = 0; y < H; y += 2) d.drawFastHLine(0, y, W, COL_INK);          // dim scrim (cheap, no alpha)
        int bw = 206, bh = 62, bx = W / 2 - bw / 2, by = H / 2 - bh / 2;
        d.fillRoundRect(bx - 2, by - 2, bw + 4, bh + 4, 9, COL_INK);
        d.fillRoundRect(bx, by, bw, bh, 8, rgb(28, 32, 50));
        d.drawRoundRect(bx, by, bw, bh, 8, COL_GOLD);
        txt_c(W / 2 + 1, by + 9, 2, COL_INK, tx("Uscire?", "Leave?")); txt_c(W / 2, by + 8, 2, COL_GOLD, tx("Uscire?", "Leave?"));
        txt_c(W / 2, by + 28, 1, HUD_TEXT2, tx("Abbandoni la partita", "This abandons the match"));
        txt_c(W / 2, by + 44, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_GOLD, tx("INVIO esci    Esc resta", "ENTER quit    Esc stay"));
    }
}

// ============================ menus ==========================================
static void felt(int ch) {
    // clean vertical gradient backdrop (deep navy top -> lifted bottom) — premium and calm, no banding noise
    int den = ch > 0 ? ch : 1;
    for (int y = 0; y < ch; y++) d.drawFastHLine(0, y, W, mix(rgb(9, 12, 24), rgb(20, 25, 46), y * 256 / den));
    d.drawFastHLine(0, ch - 1, W, rgb(30, 38, 64));   // faint floor line grounds the panel
}
static void mini_tank(int cx, int cy, uint16_t col) {
    d.fillRect(cx - 8, cy, 16, 5, mix(col, COL_INK, 60)); d.fillCircle(cx, cy - 2, 4, col); d.drawLine(cx, cy - 2, cx + 12, cy - 9, COL_WHITE);
    for (int t = -1; t <= 1; t++) d.fillCircle(cx + t * 5, cy + 4, 1, mix(col, COL_INK, 120));
}
#define NMENU 7
static const char *menu_label(int i) {
    switch (i) {
        case 0: return tx("GIOCA vs CPU", "PLAY vs CPU");
        case 1: return tx("Crea stanza", "Create room");
        case 2: return tx("Entra in stanza", "Join room");
        case 3: return tx("Impostazioni", "Settings");
        case 4: return tx("Record", "Scores");
        case 5: return tx("Guida", "Help");
        default: return tx("Esci", "Quit");
    }
}
static void draw_menu(int ch) {
    felt(ch);
    mini_tank(20, 18, COL_P0); mini_tank(W - 20, 18, COL_P1);
    txt_c(W / 2 + 2, 4, 4, mix(COL_GOLD, rgb(9, 12, 24), 120), "TANKS");
    txt_c(W / 2, 2, 4, ((s_anim >> 3) & 1) ? COL_WHITE : COL_GOLD, "TANKS");
    // windowed list: the selection + two neighbours each side, ALL at size 2 (bigger, legible), the
    // far rows dimmed so the focus is obvious. Wraps smoothly => a clean carousel that fits 7 items.
    const int cy = 46, rowh = 17;
    for (int dlt = -2; dlt <= 2; dlt++) {
        int i = (s_msel + dlt + NMENU) % NMENU, y = cy + (dlt + 2) * rowh;
        if (dlt == 0) {
            d.fillRoundRect(12, y - 1, W - 24, rowh + 1, 5, rgb(30, 40, 70));
            d.drawRoundRect(12, y - 1, W - 24, rowh + 1, 5, ((s_anim >> 3) & 1) ? rgb(120, 150, 220) : rgb(78, 98, 158));
            d.fillRect(15, y + 1, 3, rowh - 3, COL_GOLD);                              // accent bar
            txt_c(W / 2 + 4, y + 1, 2, COL_WHITE, menu_label(i));
        } else {
            int fade = (dlt == -2 || dlt == 2) ? 120 : 55;                            // outer rows fade toward the backdrop
            txt_c(W / 2, y + 1, 2, mix(COL_MUT, rgb(14, 18, 34), fade), menu_label(i));
        }
    }
}
// One weapon row in the picker carousel. big => the focused centre row (size-2 name, full chrome);
// otherwise a compact neighbour. Selected weapons are unmistakable at any size: green plate + tick.
static void ld_row(int i, int y, bool cur, bool big) {
    const Weap *w = &WEAPS[i]; bool on = s_ldpick[i];
    int rh = big ? 22 : 16, pad = big ? 3 : 2;
    // row plate: green when chosen, neutral otherwise; the cursor row gets a bright animated border
    uint16_t fillc = on ? mix(COL_GREEN, COL_INK, big ? 150 : 185) : (big ? rgb(28, 36, 60) : rgb(18, 24, 40));
    d.fillRoundRect(8, y, W - 16, rh, 5, fillc);
    if (cur) d.drawRoundRect(8, y, W - 16, rh, 5, ((s_anim >> 3) & 1) ? COL_WHITE : rgb(120, 150, 220));
    else if (on) d.drawRoundRect(8, y, W - 16, rh, 5, mix(COL_GREEN, COL_INK, 90));
    int cbx = 13, cby = y + rh / 2 - 5, cs = 10;                     // check box / tick
    d.fillRoundRect(cbx, cby, cs, cs, 2, on ? COL_GREEN : COL_INK);
    d.drawRoundRect(cbx, cby, cs, cs, 2, on ? COL_GREEN : rgb(60, 70, 96));
    if (on) { d.drawLine(cbx + 2, cby + 5, cbx + 4, cby + 7, COL_INK); d.drawLine(cbx + 4, cby + 7, cbx + 8, cby + 2, COL_INK); }
    d.fillRoundRect(28, y + pad, 5, rh - 2 * pad, 1, w->col);        // colour swatch
    char nm[16]; snprintf(nm, sizeof nm, "%s", tx(w->it, w->en));
    int tsz = big ? 2 : 1; if (big && (int)strlen(nm) > 11) nm[11] = 0; else if ((int)strlen(nm) > 13) nm[13] = 0;
    uint16_t nmc = on ? COL_WHITE : (cur ? COL_WHITE : COL_MUT);
    txt(38, y + rh / 2 - (big ? 8 : 4), tsz, nmc, nm);
    char mt[18];
    if (w->beh == WB_SHIELD) snprintf(mt, sizeof mt, "%s", tx("difesa", "defense"));
    else snprintf(mt, sizeof mt, "d%d x%d", w->dmg, w->ammo);
    if (big) {                                                       // focused row: tag WHAT it does, then damage x ammo
        txt_r(W - 14, y + 4,  1, mix(w->col, COL_WHITE, 40), weap_kind(i));
        txt_r(W - 14, y + 13, 1, mix(w->col, COL_WHITE, 70), mt);
    } else {
        txt_r(W - 14, y + rh / 2 - 3, 1, mix(w->col, COL_INK, 40), mt);
    }
}
// Pocket-Tanks loadout picker: an INFINITE wrapping carousel (big centre row), a live strip of the ten
// picks so you always see your set, and a bold counter. Tick exactly 10 to arm the START.
static void draw_loadout(int ch) {
    felt(ch);
    int n = ld_count();
    txt_c(W / 2 + 1, 4, 2, mix(COL_GOLD, COL_INK, 130), tx("ARSENALE", "ARSENAL"));
    txt_c(W / 2, 3, 2, COL_GOLD, tx("ARSENALE", "ARSENAL"));
    char cc[10]; snprintf(cc, sizeof cc, "%d/%d", n, LOADOUT_N);
    uint16_t ccol = (n == LOADOUT_N) ? COL_GREEN : COL_GOLD;
    int cw = (int)strlen(cc) * 12 + 8;
    d.fillRoundRect(W - cw - 6, 2, cw + 2, 19, 4, mix(COL_INK, ccol, 55)); d.drawRoundRect(W - cw - 6, 2, cw + 2, 19, 4, ((n == LOADOUT_N && (s_anim >> 3) & 1)) ? COL_WHITE : ccol);
    txt(W - cw - 2, 4, 2, ccol, cc);

    // infinite carousel: big centre row + one neighbour each side, wraps with modulo (endless scroll)
    ld_row((s_ldsel - 1 + NWEAP) % NWEAP, 25, false, false);
    ld_row(s_ldsel,                        45, true,  true);
    ld_row((s_ldsel + 1) % NWEAP,          75, false, false);
    // mode tag (top-left): the picker now precedes every mode, so show which match you're arming for
    const char *modew = (s_ld_go == 1) ? tx("OSPITA", "HOST") : (s_ld_go == 2) ? tx("UNISCITI", "JOIN") : tx("vs CPU", "vs CPU");
    uint16_t modec = (s_ld_go == 1) ? COL_P0 : (s_ld_go == 2) ? COL_P1 : COL_GOLD;
    txt(10, 7, 1, modec, modew);

    // live loadout strip: ten slots filled in pick order with the weapon colour; every empty slot shows
    // its number (1..10) so the goal reads at a glance, and the NEXT slot to fill pulses.
    int sy = 95, sw = 20, sx = (W - LOADOUT_N * sw) / 2, filled = 0;
    for (int w = 0; w < NWEAP && filled < LOADOUT_N; w++) if (s_ldpick[w]) {
        int x = sx + filled * sw; d.fillRoundRect(x + 1, sy, sw - 3, 12, 3, mix(WEAPS[w].col, COL_INK, 40));
        d.drawRoundRect(x + 1, sy, sw - 3, 12, 3, WEAPS[w].col); filled++;
    }
    for (int e = filled; e < LOADOUT_N; e++) {
        int x = sx + e * sw;
        bool nextslot = (e == filled) && (n < LOADOUT_N);          // where the next pick lands
        uint16_t oc = nextslot ? (((s_anim >> 2) & 1) ? COL_GOLD : rgb(96, 108, 150)) : rgb(40, 48, 72);
        d.drawRoundRect(x + 1, sy, sw - 3, 12, 3, oc);
        char sn[3]; snprintf(sn, sizeof sn, "%d", e + 1);
        txt_c(x + 1 + (sw - 3) / 2, sy + 3, 1, nextslot ? COL_GOLD : rgb(72, 82, 112), sn);
    }

    // adaptive guidance: teach the empty picker, then count the picks down, then arm START
    if (n == LOADOUT_N)
        txt_c(W / 2, ch - 9, 1, ((s_anim >> 2) & 1) ? COL_GREEN : COL_WHITE, tx("INVIO avvia   SPAZIO togli", "ENTER start   SPACE remove"));
    else if (n == 0)
        txt_c(W / 2, ch - 9, 1, COL_GOLD, tx("SPAZIO scegli   ALT casuali", "SPACE pick   ALT random"));
    else {
        char g[44]; snprintf(g, sizeof g, tx("ancora %d   ALT casuali", "%d more   ALT random"), LOADOUT_N - n);
        txt_c(W / 2, ch - 9, 1, COL_GOLD, g);
    }
}
static const char *opt_label(int i) {
    switch (i) {
        case 0: return "Audio";
        case 1: return tx("Lingua", "Language");
        case 2: return tx("Difficolta", "Difficulty");
        case 3: return tx("Mira manuale", "Manual aim");
        case 4: return tx("Aiuto mira", "Aim help");
        case 5: return tx("Vento variabile", "Variable wind");
        default: return tx("Comandi", "Key guide");
    }
}
static void opt_value(int i, char *out, int cap, uint16_t *col) {
    *col = COL_GOLD;
    switch (i) {
        case 0: snprintf(out, cap, "%s", g_audio ? "On" : "Off"); if (!g_audio) *col = COL_DIM; break;
        case 1: snprintf(out, cap, "%s", g_lang ? "English" : "Italiano"); break;
        case 2: snprintf(out, cap, "%s", s_diff == 0 ? tx("Facile","Easy") : s_diff == 1 ? tx("Normale","Normal") : tx("Difficile","Hard")); break;
        case 3: snprintf(out, cap, "%s", g_manual ? "On" : "Off"); if (!g_manual) *col = COL_DIM; break;
        case 4: snprintf(out, cap, "%s", g_aimhelp ? "On" : "Off"); if (!g_aimhelp) *col = COL_DIM; break;
        case 5: snprintf(out, cap, "%s", g_windvar ? "On" : "Off"); if (!g_windvar) *col = COL_DIM; break;
        default: snprintf(out, cap, "%s", tx("apri", "open")); break;
    }
}
#define NOPT 7
static void draw_options(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 6, 3, mix(COL_GOLD, COL_INK, 130), tx("IMPOSTAZIONI", "SETTINGS"));   // drop shadow
    txt_c(W / 2, 5, 3, COL_GOLD, tx("IMPOSTAZIONI", "SETTINGS"));
    d.drawFastHLine(14, 27, W - 28, rgb(54, 64, 104));
    if (s_optguide) {
        txt(14, 32, 1, HUD_TEXT, tx("SU/GIU  alzata", "UP/DN   elevation"));
        txt(14, 44, 1, HUD_TEXT, tx("SX/DX   potenza", "LEFT/RIGHT power"));
        txt(14, 56, 1, COL_CYAN, tx("E S A D  muovi camera", "E S A D  pan camera"));
        txt(14, 68, 1, HUD_TEXT, tx("1-9 0 / Q W  scegli arma", "1-9 0 / Q W  weapon"));
        txt(14, 80, 1, COL_GOLD, tx("INVIO/SPAZIO  spara", "ENTER/SPACE  fire"));
        txt(14, 92, 1, HUD_TEXT, tx("TAB  impostazioni", "TAB  settings"));
        txt(14, 104, 1, COL_DIM, tx("Esc menu  -  occhio al VENTO", "Esc menu  -  mind the WIND"));
        txt_c(W / 2, ch - 12, 1, COL_DIM, tx("INVIO/TAB indietro", "ENTER/TAB back"));
        return;
    }
    // clean fixed list: every option fits (no scroll). The selected row blooms into a labelled pill with an
    // accent bar; the rest stay compact and column-aligned. Sequential layout => nothing ever overlaps.
    // Adaptive row pitch: the whole list must clear the footer whatever NOPT is — at the old fixed pitch
    // 7 options overran the 121 px content area. Derive a per-row height from the room available; the
    // selected row is a few px taller (its size-2 label + pill). A legibility floor keeps rows readable.
    int footer = ch - 9;
    int y0 = 30, budget = footer - 3 - y0;
    int perNon = (budget - 7) / NOPT; if (perNon > 14) perNon = 14; if (perNon < 10) perNon = 10;
    int perSel = perNon + 7;
    int y = y0;
    for (int i = 0; i < NOPT; i++) {
        bool sel = (i == s_optsel);
        char vb[16]; uint16_t vc; opt_value(i, vb, sizeof vb, &vc);
        if (sel) {
            int ph = perSel - 1;
            d.fillRoundRect(10, y, W - 20, ph, 5, rgb(30, 40, 70));
            d.drawRoundRect(10, y, W - 20, ph, 5, ((s_anim >> 3) & 1) ? rgb(120, 150, 220) : rgb(78, 98, 158));
            d.fillRect(12, y + 2, 3, ph - 4, COL_GOLD);                    // accent bar
            txt(20, y + (ph - 14) / 2, 2, COL_WHITE, opt_label(i));        // size-2 label, vertically centred
            char vv[20];
            if (i == NOPT - 1) snprintf(vv, sizeof vv, "%s", vb);          // guide row: "open"
            else               snprintf(vv, sizeof vv, "< %s >", vb);      // chevrons hint SX/DX changes it
            txt_r(W - 16, y + (ph - 8) / 2, 1, vc == COL_DIM ? COL_MUT : vc, vv);
            y += perSel;
        } else {
            txt(20, y + (perNon - 8) / 2, 1, COL_MUT, opt_label(i));
            txt_r(W - 16, y + (perNon - 8) / 2, 1, mix(vc, rgb(8, 10, 20), 70), vb);
            y += perNon;
        }
    }
    txt_c(W / 2, ch - 9, 1, COL_DIM, tx("SU/GIU scegli  SX/DX cambia  TAB esci", "UP/DN pick  L/R change  TAB exit"));
}
static void draw_over(int ch) {
    felt(ch);
    for (int i = 0; i < NRING; i++) { if (!s_rg[i].on) continue; int a = s_rg[i].life * 256 / s_rg[i].lmax; d.drawCircle((int)s_rg[i].x, (int)s_rg[i].y, (int)s_rg[i].r, mix(rgb(8,10,20), s_rg[i].col, a)); }
    for (int i = 0; i < NSPK; i++) { if (s_spk[i].life <= 0) continue; int f = s_spk[i].life * 256 / (s_spk[i].max > 0 ? s_spk[i].max : 1); d.fillCircle((int)s_spk[i].x, (int)s_spk[i].y, f > 130 ? 2 : 1, mix(rgb(8,10,20), s_spk[i].col, f)); }
    int win = s_tk[0].dead ? 1 : 0; uint16_t wc = s_draw_flag ? COL_GOLD : (win ? COL_P1 : COL_P0);
    bool mywin = (s_mode == MODE_GUEST) ? (win == 1) : (win == 0);
    int swin = (s_wins[0] >= SERIES_TGT) ? 0 : (s_wins[1] >= SERIES_TGT) ? 1 : -1;
    bool series_over = (s_mode == MODE_AI && swin >= 0);
    // Banner box — tall enough for banner + subtitle + series tag, all non-overlapping inside
    d.fillRect(0, 6, W, 48, mix(wc, COL_INK, 150)); d.fillRect(0, 6, W, 3, wc); d.fillRect(0, 51, W, 3, wc);
    int bz = 3 + (s_anim < 14 ? (14 - (int)s_anim) / 5 : 0);   // slams in large, settles to 3
    const char *ban = s_draw_flag         ? tx("PAREGGIO!", "DRAW!")
                    : series_over         ? (swin == 0 ? tx("SERIE VINTA!", "SERIES WON!") : tx("SERIE PERSA!", "SERIES LOST!"))
                    : (s_mode == MODE_AI) ? (win  == 0 ? tx("ROUND VINTO!", "ROUND WON!")  : tx("ROUND PERSO!", "ROUND LOST!"))
                    :                       (mywin     ? tx("VITTORIA!",    "VICTORY!")    : tx("SCONFITTA!",   "DEFEAT!"));
    txt_c(W / 2 + 1, 9, bz, COL_INK, ban);
    txt_c(W / 2, 8, bz, ((s_anim >> 2) & 1) ? COL_WHITE : COL_GOLD, ban);
    // subtitle and series tag — both size 1, spaced inside the box, revealed as banner shrinks
    char t[28];
    if (s_draw_flag) snprintf(t, sizeof t, "%s", tx("Distruzione totale", "Mutual destruction"));
    else if (s_mode != MODE_AI) snprintf(t, sizeof t, "%s", mywin ? tx("Hai vinto", "You win") : tx("Hai perso", "You lose"));
    else if (g_lang) snprintf(t, sizeof t, "P%d WINS", win + 1);
    else snprintf(t, sizeof t, "VINCE P%d", win + 1);
    txt_c(W / 2, 36, 1, wc, t);
    if (s_mode == MODE_AI) { char cap[20]; snprintf(cap, sizeof cap, "%s %d", tx("PRIMO A", "FIRST TO"), SERIES_TGT); txt_c(W / 2, 44, 1, mix(COL_GOLD, COL_INK, 60), cap); }
    // score (size 3) — clear gap below box before the score row
    char sc[24]; snprintf(sc, sizeof sc, "%d  -  %d", s_wins[0], s_wins[1]); txt_c(W / 2 + 1, 61, 3, COL_INK, sc); txt_c(W / 2, 60, 3, COL_WHITE, sc);
    // HP chip (score size 3 = 24px → ends y=84; chip at y=88, clear gap)
    if (!s_draw_flag) { char hp[40]; snprintf(hp, sizeof hp, "%s %d", tx("Vita rimasta", "HP left"), s_tk[win].hp); int hw = (int)strlen(hp) * 6 + 12; d.fillRoundRect(W / 2 - hw / 2, 88, hw, 12, 4, mix(COL_GREEN, COL_INK, 200)); txt_c(W / 2, 90, 1, COL_GREEN, hp); }
    // round bonus you racked up (long shots / doubles / one-shots) — the points that feed the leaderboard
    if (s_mode == MODE_AI && !s_draw_flag && win == 0 && s_bonus > 0) { char bb[28]; snprintf(bb, sizeof bb, "%s +%u", tx("Bonus", "Bonus"), s_bonus); txt_c(W / 2, 103, 1, COL_GOLD, bb); }
    // hint at content bottom — no mini-tanks here (they pushed below hint boundary)
    txt_c(W / 2, ch - 12, 1, ((s_anim >> 2) & 1) ? COL_WHITE : COL_DIM,
          s_mode != MODE_AI ? tx("INVIO menu", "ENTER menu")
          : series_over     ? tx("INVIO nuova serie   Esc menu", "ENTER new series   Esc menu")
                            : tx("INVIO prossimo round   Esc menu", "ENTER next round   Esc menu"));
}
static void draw_scores(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, COL_INK, tx("RECORD", "SCORES"));
    txt_c(W / 2, 6, 3, COL_GOLD, tx("RECORD", "SCORES"));
    d.drawFastHLine(16, 32, W - 32, rgb(54, 64, 104));
    const int rh = 16, top = 36;
    for (int i = 0; i < NTOP; i++) {
        int y = top + i * rh;
        bool filled = g_top[i] != 0;
        uint16_t acc = i == 0 ? COL_GOLD : i < 3 ? mix(COL_GOLD, COL_INK, 70) : rgb(60, 70, 110);
        if (filled) {
            d.fillRoundRect(14, y, W - 28, rh - 3, 4, mix(acc, COL_INK, i == 0 ? 150 : 200));
            d.fillRect(14, y, 3, rh - 3, acc);
        } else {
            d.drawRoundRect(14, y, W - 28, rh - 3, 4, rgb(26, 32, 52));
        }
        char r[5]; snprintf(r, sizeof r, "%d", i + 1);
        d.fillCircle(28, y + (rh - 3) / 2, 7, filled ? acc : rgb(22, 28, 46));
        txt_c(28, y + 1, 1, filled ? COL_INK : COL_DIM, r);
        if (filled) { char s[14]; snprintf(s, sizeof s, "%u", g_top[i]); txt_r(W - 22, y + 1, 2, i < 3 ? COL_WHITE : HUD_TEXT, s); }
        else txt_r(W - 22, y + 1, 2, rgb(40, 48, 74), "---");
    }
    txt_c(W / 2, ch - 9, 1, COL_DIM, tx("Esc indietro", "Esc back"));
}
static void draw_help(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 3, mix(COL_GOLD, COL_INK, 130), tx("Guida", "Help"));   // drop shadow
    txt_c(W / 2, 6, 3, COL_GOLD, tx("Guida", "Help"));
    d.drawFastHLine(14, 34, W - 28, rgb(54, 64, 104));
    txt(12, 40, 1, HUD_TEXT, tx("SU/GIU alzata - SX/DX potenza", "UP/DN elevation - LEFT/RIGHT power"));
    txt(12, 54, 1, COL_CYAN, tx("E S A D  muovi la camera", "E S A D  pan the camera"));
    txt(12, 68, 1, HUD_TEXT, tx("1-9 0 / Q W arma - INVIO spara", "1-9 0 / Q W weapon - ENTER fire"));
    txt(12, 82, 1, COL_GOLD, tx("TAB impostazioni - occhio al VENTO", "TAB settings - mind the WIND"));
    txt(12, 96, 1, HUD_TEXT, tx("29 armi: scegli le tue 10", "29 weapons: pick your 10"));
    txt_c(W / 2, ch - 12, 1, COL_DIM, tx("Esc indietro", "Esc back"));
}
static void draw_host(int ch) {
    felt(ch);
    mini_tank(22, 18, COL_P0);
    txt_c(W / 2 + 1, 7, 3, mix(COL_INK, COL_P0, 90), tx("Crea stanza", "Create room"));   // big title + shadow
    txt_c(W / 2, 6, 3, COL_P0, tx("Crea stanza", "Create room"));
    d.drawFastHLine(14, 30, W - 28, rgb(54, 64, 104));
    // room identity card: name + channel, big and centred
    d.fillRoundRect(20, 36, W - 40, 30, 6, rgb(24, 30, 50));
    char nm[24]; snprintf(nm, sizeof nm, "%.11s", pnet_name()); txt_c(W / 2, 40, 2, COL_GOLD, nm);
    char cc[28]; snprintf(cc, sizeof cc, tx("canale %d", "channel %d"), pnet_channel()); txt_c(W / 2, 56, 1, COL_GREEN, cc);
    if (s_guest_in) {
        char g[24]; snprintf(g, sizeof g, "%.10s", s_peer_name);
        txt_c(W / 2, 74, 1, COL_MUT, tx("Sfida da", "Challenger"));
        txt_c(W / 2 + 1, 83, 2, COL_INK, g); txt_c(W / 2, 82, 2, COL_P1, g);
        d.fillRoundRect(W / 2 - 70, 102, 140, 22, 6, mix(COL_GOLD, COL_INK, 150));
        txt_c(W / 2, 106, 2, ((s_anim >> 2) & 1) ? COL_WHITE : COL_GOLD, tx("INVIO = AVVIA", "ENTER = START"));
    } else {
        char dots[5] = "    "; for (int i = 0; i < (int)((s_anim >> 2) % 4); i++) dots[i] = '.';
        char w[40]; snprintf(w, sizeof w, "%s%s", tx("Attendo sfidante", "Waiting for rival"), dots); txt_c(W / 2, 84, 2, COL_WHITE, w);
        txt_c(W / 2, 106, 1, COL_DIM, tx("sull'altro: Entra in stanza, stesso canale", "on the other: Join room, same channel"));
    }
}
static void draw_browse(int ch) {
    felt(ch);
    txt_c(W / 2 + 1, 7, 2, mix(COL_INK, COL_P1, 90), tx("Entra in stanza", "Join room"));
    txt_c(W / 2, 6, 2, COL_P1, tx("Entra in stanza", "Join room"));
    char cc[24]; snprintf(cc, sizeof cc, tx("canale %d", "channel %d"), pnet_channel()); txt_r(W - 8, 10, 1, COL_GREEN, cc);
    d.drawFastHLine(10, 26, W - 20, rgb(54, 64, 104));
    char dots[5] = "    "; for (int i = 0; i < (int)((s_anim >> 2) % 4); i++) dots[i] = '.';
    if (s_welcomed) {                                    // JOINED: clear feedback so the guest isn't left blind
        d.fillRoundRect(20, 40, W - 40, 30, 6, mix(COL_GREEN, COL_INK, 200)); d.drawRoundRect(20, 40, W - 40, 30, 6, COL_GREEN);
        txt_c(W / 2, 44, 1, COL_GREEN, tx("SEI NELLA STANZA", "YOU'RE IN THE ROOM"));
        char rn[24]; snprintf(rn, sizeof rn, "%.11s", s_peer_name); txt_c(W / 2 + 1, 57, 2, COL_INK, rn); txt_c(W / 2, 56, 2, COL_P0, rn);
        char w[44]; snprintf(w, sizeof w, "%s%s", tx("l'host sta per avviare", "host is about to start"), dots); txt_c(W / 2, 84, 1, COL_WHITE, w);
        txt_c(W / 2, 104, 1, COL_DIM, tx("resta qui, parte tra poco", "stay here, starting soon"));
        return;
    }
    if (s_join_pending) {                                // CONNECTING: name the room we're reaching
        txt_c(W / 2, 46, 1, COL_DIM, tx("Entro nella stanza", "Joining room"));
        char rn[16]; snprintf(rn, sizeof rn, "%.10s", s_peer_name); txt_c(W / 2 + 1, 61, 2, COL_INK, rn); txt_c(W / 2, 60, 2, COL_GOLD, rn);
        char w[24]; snprintf(w, sizeof w, "%s%s", tx("contatto", "reaching"), dots); txt_c(W / 2, 84, 1, COL_DIM, w);
        return;
    }
    if (s_nroom == 0) {
        txt_c(W / 2, 52, 1, COL_WHITE, tx("Nessuna stanza trovata", "No rooms found"));
        txt_c(W / 2, 68, 1, COL_DIM, tx("Sull'altro Cardputer: Crea stanza", "On the other Cardputer: Create room"));
        return;
    }
    txt(12, 30, 1, COL_DIM, tx("Scegli una stanza:", "Pick a room:"));
    int y = 42;
    for (int i = 0; i < s_nroom; i++) {
        bool f = (i == s_rsel);
        if (f) {
            d.fillRoundRect(10, y, W - 20, 22, 5, rgb(40, 30, 24)); d.drawRoundRect(10, y, W - 20, 22, 5, COL_P1);
            d.fillRect(12, y + 2, 3, 18, COL_P1);
            char nm[24]; snprintf(nm, sizeof nm, "%.11s", s_rooms[i].name); txt(20, y + 4, 2, COL_WHITE, nm);
            txt_r(W - 16, y + 8, 1, ((s_anim >> 2) & 1) ? COL_GREEN : COL_WHITE, tx("INVIO>", "ENTER>"));
            y += 26;
        } else {
            char nm[24]; snprintf(nm, sizeof nm, "%.18s", s_rooms[i].name); txt(20, y + 3, 1, COL_MUT, nm);
            y += 16;
        }
        if (y > ch - 16) break;
    }
    if (s_nroom > 1) { char nv[12]; snprintf(nv, sizeof nv, "%d/%d", s_rsel + 1, s_nroom); txt_c(W / 2, ch - 10, 1, COL_DIM, nv); }
}

// ============================ input ==========================================
static void set_hint(void) {
    switch (s_screen) {
        case ST_MENU:   nucleo_app_set_hint(tx("SU/GIU  INVIO  Esc esci", "UP/DN  ENTER  Esc quit")); break;
        case ST_OVER:   nucleo_app_set_hint(s_mode == MODE_AI ? tx("INVIO rivincita  Esc menu", "ENTER rematch  Esc menu")
                                          : s_seat == 0        ? tx("INVIO rivincita  Esc menu", "ENTER rematch  Esc menu")
                                                              : tx("Attendi la rivincita host  Esc menu", "Waiting for host rematch  Esc menu")); break;
        case ST_OPT:    nucleo_app_set_hint(tx("SU/GIU  INVIO  TAB chiudi", "UP/DN  ENTER  TAB close")); break;
        case ST_HOST:   nucleo_app_set_hint(tx("INVIO avvia (con sfidante)  Esc", "ENTER start (with challenger)  Esc")); break;
        case ST_BROWSE: nucleo_app_set_hint(tx("SU/GIU  INVIO entra  Esc", "UP/DN  ENTER join  Esc")); break;
        case ST_SCORES: case ST_HELP: nucleo_app_set_hint(tx("Esc indietro", "Esc back")); break;
        case ST_LOADOUT: nucleo_app_set_hint(tx("SU/GIU  SPAZIO scegli  ALT casuali  DEL azzera  INVIO avvia  Esc", "UP/DN  SPACE pick  ALT random  DEL clear  ENTER start  Esc")); break;
        default: break;
    }
}
static void go(int s) { s_screen = s; s_confirm = false; s_welcomed = false; nucleo_app_set_fullscreen(s == ST_PLAY); set_hint(); nucleo_app_request_draw(); }
static void cycle_weapon(int dir) {
    int w = s_tk[s_active].weap;
    for (int n = 0; n < NWEAP; n++) { w = (w + dir + NWEAP) % NWEAP; if (s_tk[s_active].ammo[w] != 0) break; }
    s_tk[s_active].weap = w; s_wbar_t = now_ms() + 1700; sfx(1);
}
static void tab_handler(void) {
    if (s_screen == ST_PLAY) { s_optsel = 0; s_optguide = 0; s_opt_from = ST_PLAY; go(ST_OPT); sfx(2); }
    else if (s_screen == ST_OPT) { go(s_opt_from); sfx(3); }
}
static void opt_change(int i, int dir) {
    switch (i) {
        case 0: g_audio ^= 1; if (g_audio) presynth(); break;   // cache all sfx now (menu ctx) so the game task never synthesizes
        case 1: g_lang ^= 1; set_hint(); break;
        case 2: s_diff = (s_diff + (dir < 0 ? 2 : 1)) % 3; break;
        case 3: g_manual ^= 1; if (g_manual && local_active()) seed_entry(); break;   // seed for whoever is actually aiming (host OR guest)
        case 4: g_aimhelp ^= 1; break;
        case 5: g_windvar ^= 1; break;
        default: s_optguide = 1; break;
    }
    cfg_write(); sfx(2); nucleo_app_request_draw();
}
static void entry_key(int k, char ch) {                 // manual numeric aim entry
    char *fld = s_entry_field == 0 ? s_entry_ang : s_entry_pow;
    int len = (int)strlen(fld);
    if (ch >= '0' && ch <= '9') { if (len < 3) { fld[len] = ch; fld[len + 1] = 0; sfx(1); nucleo_app_request_draw(); } return; }
    if (k == NK_DEL) { if (len > 0) { fld[len - 1] = 0; nucleo_app_request_draw(); } return; }
    if (ch == 'p' || ch == 'P' || ch == '.' || ch == ',') { s_entry_field ^= 1; sfx(1); nucleo_app_request_draw(); return; }
    if (k == NK_ENTER) {
        int me = s_active;
        if (s_entry_field == 0) { s_tk[me].elev = clampi(atoi(s_entry_ang), ELEV_MIN, ELEV_MAX); snprintf(s_entry_ang, sizeof s_entry_ang, "%d", s_tk[me].elev); s_entry_field = 1; sfx(2); nucleo_app_request_draw(); }
        else { s_tk[me].power = clampi(atoi(s_entry_pow), 5, 100); if (s_mode != MODE_AI) net_send_fire(); fire_weapon(me); }
        return;
    }
}
// One precise aim step (±1, clamped) on the active tank. Wakes the HUD.
static void aim_nudge(int axis, int dir, int step) {     // axis 0=elevation, 1=power
    int me = s_active;
    if (axis == 0) s_tk[me].elev  = clampi(s_tk[me].elev  + dir * step, ELEV_MIN, ELEV_MAX);
    else           s_tk[me].power = clampi(s_tk[me].power + dir * step, 5, 100);
    s_hud_t = now_ms();
}
// A key event on an aim axis: a fresh tap nudges exactly ±1 NOW (precise), and arms the
// accelerating auto-repeat for as long as the key stays held (handled in poll()).
static void aim_input(int axis, int dir) {
    int64_t t = now_ms();
    int *cur = axis ? &s_inP : &s_inE;
    bool fresh = (*cur != dir);                          // new press / reversal -> one immediate step
    s_inE = (axis == 0) ? dir : 0;                       // one axis at a time (UP/DOWN vs LEFT/RIGHT)
    s_inP = (axis == 1) ? dir : 0;
    s_inUntil = t + HOLD_MS;                             // refreshed by every auto-repeat key event
    s_hud_t = t;
    if (fresh) { aim_nudge(axis, dir, 1); s_inT0 = t; s_inNext = t + 230; }   // delay before the hold ramp kicks in
}
static void on_key(int k, char ch) {
    switch (s_screen) {
        case ST_MENU:
            if (k == NK_UP)        { s_msel = (s_msel + NMENU - 1) % NMENU; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % NMENU; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER) {
                // Every mode picks its 10 weapons FIRST (empty picker). ENTER then routes to the match /
                // the host room / the room browser — see the ST_LOADOUT ENTER handler.
                if (s_msel == 0)      { sfx(2); s_ld_go = 0; for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false; s_ldsel = 0; go(ST_LOADOUT); }   // vs CPU
                else if (s_msel == 1) { sfx(2); s_ld_go = 1; for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false; s_ldsel = 0; go(ST_LOADOUT); }   // host a room
                else if (s_msel == 2) { sfx(2); s_ld_go = 2; for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false; s_ldsel = 0; go(ST_LOADOUT); }   // join a room
                else if (s_msel == 3) { s_optsel = 0; s_optguide = 0; s_opt_from = ST_MENU; sfx(2); go(ST_OPT); }
                else if (s_msel == 4) { sfx(2); go(ST_SCORES); }
                else if (s_msel == 5) { sfx(2); go(ST_HELP); }
                else nucleo_app_exit();
            }
            return;
        case ST_LOADOUT:
            if (k == NK_UP || k == NK_LEFT)         { s_ldsel = (s_ldsel + NWEAP - 1) % NWEAP; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN || k == NK_RIGHT) { s_ldsel = (s_ldsel + 1) % NWEAP; sfx(1); nucleo_app_request_draw(); }
            else if (ch == ' ') {                                      // toggle this weapon (block going over 10)
                if (s_ldpick[s_ldsel]) { s_ldpick[s_ldsel] = false; sfx(3); }
                else if (ld_count() < LOADOUT_N) { s_ldpick[s_ldsel] = true; sfx(2); }
                else sfx(3);                                           // loadout already full
                nucleo_app_request_draw();
            }
            else if (k == NK_DEL) {                                    // wipe the loadout and start the pick over
                if (ld_count()) { for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false; sfx(3); nucleo_app_request_draw(); }
            }
            else if (k == NK_ENTER) {                                  // ready only with a full 10-weapon loadout
                if (ld_count() != LOADOUT_N) sfx(3);
                else {
                    cfg_write(); sfx(2);
                    if (s_ld_go == 1)      { s_wins[0] = s_wins[1] = 0; s_mode = MODE_HOST;  s_seat = 0; s_haspeer = false; s_guest_in = false; s_nroom = 0; s_last_hello = 0; go(ST_HOST); }   // -> open the room (series starts 0-0)
                    else if (s_ld_go == 2) { s_wins[0] = s_wins[1] = 0; s_mode = MODE_GUEST; s_seat = 1; s_haspeer = false; s_join_pending = false; s_nroom = s_rsel = 0; go(ST_BROWSE); }        // -> browse rooms
                    else                   { s_mode = MODE_AI; s_wins[0] = s_wins[1] = 0; new_match(); go(ST_PLAY); }                                                  // -> straight into the CPU match
                }
            }
            return;
        case ST_HOST:
            if (k == NK_ENTER && s_guest_in) host_start_match();
            return;
        case ST_BROWSE:
            if (k == NK_UP)        { if (s_rsel > 0) s_rsel--; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { if (s_rsel < s_nroom - 1) s_rsel++; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_ENTER && s_nroom > 0 && !s_join_pending && !s_welcomed) {
                memcpy(s_peer, s_rooms[s_rsel].mac, 6); s_haspeer = true;
                snprintf(s_peer_name, 22, "%.21s", s_rooms[s_rsel].name);   // remember the room so the guest gets named feedback
                uint8_t b[26] = { TK_M0, TK_M1, TK_VER, TK_JOIN }; snprintf((char *)b + 4, 22, "%s", pnet_name()); pnet_send(s_peer, b, 26);
                s_join_pending = true; s_join_t0 = now_ms(); sfx(2); nucleo_app_request_draw();
            }
            return;
        case ST_OPT:
            if (s_optguide) { if (k == NK_ENTER || k == NK_TAB) { s_optguide = 0; sfx(3); nucleo_app_request_draw(); } return; }
            if (k == NK_UP)        { s_optsel = (s_optsel + NOPT - 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_DOWN) { s_optsel = (s_optsel + 1) % NOPT; sfx(1); nucleo_app_request_draw(); }
            else if (k == NK_RIGHT || k == NK_ENTER) opt_change(s_optsel, +1);
            return;
        case ST_PLAY: {
            if (s_confirm) {                                      // leave-match confirm is up: ENTER quits, Esc (back) cancels
                if (k == NK_ENTER) { s_confirm = false; if (s_mode != MODE_AI) { net_send_bye(); s_haspeer = false; s_mode = MODE_AI; } s_msel = 0; go(ST_MENU); }
                return;
            }
            // camera A/S/D/E are polled continuously in poll() via nucleo_kbd_char_down (hold-to-pan)
            if (!local_active() || s_phase != TP_AIM) return;     // only the player whose turn it is
            int me = s_active;                                    // the tank we control this turn (0 host/AI, 1 guest)
            s_hud_t = now_ms();                                   // any input wakes the HUD chrome back up
            if (g_manual) { entry_key(k, ch); return; }
            if (k == NK_UP)        aim_input(0, +1);
            else if (k == NK_DOWN) aim_input(0, -1);
            else if (k == NK_RIGHT)aim_input(1, +1);
            else if (ch == 'w' || ch == 'W') cycle_weapon(+1);
            else if (ch == 'q' || ch == 'Q') cycle_weapon(-1);
            else if ((ch >= '1' && ch <= '9') || ch == '0') {          // 1..9,0 -> the ten quick-bar slots
                int slot = (ch == '0') ? 9 : (ch - '1');
                if (slot < s_nslot[me] && s_tk[me].ammo[s_slot[me][slot]] != 0) { s_tk[me].weap = s_slot[me][slot]; s_wbar_t = now_ms() + 1700; sfx(1); }
                else sfx(3);
            }
            else if (k == NK_ENTER || ch == ' ') { if (s_mode != MODE_AI) net_send_fire(); fire_weapon(me); }
            return;
        }
        case ST_OVER:
            if (k == NK_ENTER) {
                if (s_mode == MODE_AI) {
                    if (s_wins[0] >= SERIES_TGT || s_wins[1] >= SERIES_TGT) s_wins[0] = s_wins[1] = 0;  // series decided -> start a fresh one
                    sfx(2); new_match(); go(ST_PLAY);   // otherwise the tally carries over: next round of the same series
                }
                else if (s_seat == 0 && s_haspeer) {    // HOST: rematch on a FRESH board; the series tally carries over, the guest gets the new START
                    if (s_wins[0] >= SERIES_TGT || s_wins[1] >= SERIES_TGT) s_wins[0] = s_wins[1] = 0;
                    sfx(2); host_start_match();          // rolls a new seed, sends START, and go(ST_PLAY)
                }
                else sfx(3);                            // GUEST: can't initiate a rematch — waits for the host (Esc leaves)
            }
            return;
        default: return;
    }
}
static bool on_back(int key) {
    if (key == NK_LEFT) {
        if (s_screen == ST_PLAY && local_active() && s_phase == TP_AIM && !g_manual) aim_input(1, -1);
        else if (s_screen == ST_OPT && !s_optguide) opt_change(s_optsel, -1);
        else if (s_screen == ST_LOADOUT) { s_ldsel = (s_ldsel + NWEAP - 1) % NWEAP; sfx(1); nucleo_app_request_draw(); }
        return true;
    }
    sfx(3);
    switch (s_screen) {
        case ST_MENU: return false;
        case ST_LOADOUT: s_msel = 0; go(ST_MENU); return true;
        case ST_OPT: if (s_optguide) { s_optguide = 0; nucleo_app_request_draw(); return true; } go(s_opt_from); return true;
        case ST_HOST: case ST_BROWSE: net_send_bye(); s_haspeer = false; s_mode = MODE_AI; s_msel = 0; go(ST_MENU); return true;
        case ST_PLAY:
            if (s_confirm) { s_confirm = false; nucleo_app_request_draw(); return true; }   // Esc again = cancel, stay in the match
            s_confirm = true; nucleo_app_request_draw(); return true;                        // first Esc = ask before abandoning
        case ST_OVER: if (s_mode != MODE_AI) { net_send_bye(); s_haspeer = false; s_mode = MODE_AI; } s_msel = 0; go(ST_MENU); return true;
        default: s_msel = 0; go(ST_MENU); return true;
    }
}

// ============================ poll / draw / lifecycle ========================
static void on_draw(void) {
    int ch = nucleo_app_content_height();
    switch (s_screen) {
        case ST_MENU:   draw_menu(ch); break;
        case ST_PLAY:   draw_play(); break;
        case ST_OVER:   draw_over(ch); break;
        case ST_OPT:    draw_options(ch); break;
        case ST_HOST:   draw_host(ch); break;
        case ST_BROWSE: draw_browse(ch); break;
        case ST_SCORES: draw_scores(ch); break;
        case ST_HELP:   draw_help(ch); break;
        case ST_LOADOUT: draw_loadout(ch); break;
        default: break;
    }
}
static bool proj_focus(float *fx, float *fy) {     // centroid of live projectiles + small velocity lead
    float sx = 0, sy = 0, leadx = 0; int n = 0;
    for (int i = 0; i < NPROJ; i++) { if (!s_pr[i].on) continue; sx += s_pr[i].x; sy += s_pr[i].y; leadx += s_pr[i].vx; n++; }
    if (!n) return false;
    *fx = sx / n + (leadx / n) * 120.0f; *fy = sy / n; return true;
}
static bool poll(void) {
    s_now = now_ms();
    int dt = (int)(s_now - s_last); if (dt < 0) dt = 0; if (dt > 60) dt = 60;
    s_last = s_now;

    // ---- network pump (lobby + in-match) ----
    if (s_mode != MODE_AI) { pnet_pkt_t pk; while (pnet_recv(&pk)) net_handle(&pk); net_pump_reliable(); }
    if (s_screen == ST_HOST) {
        if (s_now - s_last_hello > 400) { net_send_hello(); s_last_hello = s_now; }
        if (s_now - s_frame < 50) return false;
        s_frame = s_now; s_anim++; return true;
    }
    if (s_screen == ST_BROWSE) {
        rooms_prune();
        if (s_join_pending && s_now - s_join_t0 > 3500) s_join_pending = false;
        if (s_now - s_frame < 50) return false;
        s_frame = s_now; s_anim++; return true;
    }
    if (s_screen == ST_OVER) {                         // celebratory fireworks
        fx_step(dt);
        if ((s_anim % 18) == 0) { float fx2 = frnd(40, W - 40), fy2 = frnd(20, H / 2); spark_burst(fx2, fy2, 12, ((s_anim >> 3) & 1) ? (s_tk[0].dead ? COL_P1 : COL_P0) : COL_GOLD); ring_spawn(fx2, fy2, 18, s_tk[0].dead ? COL_P1 : COL_P0); }
        if (s_now - s_frame < 33) return false;
        s_frame = s_now; s_anim++; return true;
    }
    if (s_screen == ST_LOADOUT) {
        // ALT = roll a fresh RANDOM 10-weapon loadout, different every press. Alt is a bare modifier
        // (it fires no key event), so edge-detect it from the live modifier bitmask here in poll().
        // NB: the Cardputer's PHYSICAL "alt" key reports as NK_MOD_GUI — the driver's NK_MOD_* labels
        // are swapped vs the key legends (physical ALT->GUI, verified: app_pinball uses it for tilt).
        static bool alt_prev = false;
        bool alt = (nucleo_kbd_mods() & NK_MOD_GUI) != 0;
        if (alt && !alt_prev) {
            for (int w = 0; w < NWEAP; w++) s_ldpick[w] = false;
            int picked = 0, guard = 0;
            while (picked < LOADOUT_N && guard++ < 400) { int w = (int)(esp_random() % NWEAP); if (!s_ldpick[w]) { s_ldpick[w] = true; picked++; } }
            sfx(2); nucleo_app_request_draw();
        }
        alt_prev = alt;
    }
    if (s_screen != ST_PLAY) { if (s_now - s_frame < 60) return false; s_frame = s_now; s_anim++; return true; }

    fx_step(dt);
    if (s_now > s_inUntil) { s_inE = s_inP = 0; }
    bool ai_turn = (s_mode == MODE_AI && s_active == 1);
    bool sim_here = (s_mode == MODE_AI) || local_active();      // this device runs the turn's simulation
    // net: lost opponent -> back to menu
    // peer-loss -> menu. The 8s silence timeout ONLY while we're the PASSIVE observer: during our OWN turn the
    // opponent is legitimately silent (longer now that shots fly far), so counting it would self-eject mid-turn.
    // CRUCIAL: keep s_last_rx fresh THROUGHOUT our own turn, so after we hand off the 8s timer starts from zero
    // (otherwise a long turn leaves it already near-expired and the now-passive device ejects to menu instantly).
    if (s_mode != MODE_AI && local_active()) s_last_rx = s_now;
    if (s_mode != MODE_AI && (s_peerleft || (!local_active() && s_now - s_last_rx > 8000))) { net_send_bye(); s_haspeer = false; s_mode = MODE_AI; s_msel = 0; go(ST_MENU); return true; }
    // free-look from PHYSICALLY-HELD keys (smooth hold-to-pan): A/D = left/right, E/S = up/down
    int panx = (nucleo_kbd_char_down('a') ? -1 : 0) + (nucleo_kbd_char_down('d') ? 1 : 0);
    int pany = (nucleo_kbd_char_down('e') ? -1 : 0) + (nucleo_kbd_char_down('s') ? 1 : 0);
    if (panx) { s_camfree += panx * 0.22f * dt; s_camfree = clampf(s_camfree, -s_cam, (WW - W) - s_cam); }
    else { s_camfree *= (s_phase == TP_AIM && sim_here) ? 1.0f : 0.88f; if (fabsf(s_camfree) < 0.5f) s_camfree = 0; }
    if (pany) s_camY = clampf(s_camY + pany * 0.16f * dt, -14, 10); else s_camY *= 0.90f;
    if (s_phase == TP_AIM && sim_here && !ai_turn && !g_manual && (s_inE || s_inP) && s_now <= s_inUntil && s_now >= s_inNext) {
        int held = (int)(s_now - s_inT0);
        int step = held > 1500 ? 3 : held > 650 ? 2 : 1;          // tap stays a precise ±1; hold ramps up to sweep 5..100 fast
        if (s_inE) aim_nudge(0, s_inE, step);
        if (s_inP) aim_nudge(1, s_inP, step);
        s_inNext = s_now + 55;
    }
    // net: stream our aim so the opponent watches the barrel/trajectory
    if (s_mode != MODE_AI && sim_here && s_now - s_last_aim > (s_phase == TP_AIM ? 80 : 200)) { net_send_aim(); s_last_aim = s_now; }   // keepalive in ALL phases (not just aiming) so the passive peer never starves the 8s watchdog during our shot/settle
    // net turn clock: 45s to choose+fire. Tick each of the last 5 seconds, then auto-launch ("VIA") at expiry.
    if (s_mode != MODE_AI && local_active() && s_phase == TP_AIM) {
        int rem = (int)(s_aim_deadline - s_now);
        int secs = rem > 0 ? (rem + 999) / 1000 : 0;
        if (secs != s_last_tick) { s_last_tick = secs; if (secs >= 3 && secs <= 5) sfx(1); else if (secs >= 1 && secs <= 2) sfx(8); else if (secs == 0) sfx(4); }   // escalating urgency: soft tick -> chirp -> buzzer
        if (rem <= -400) { flash(COL_GOLD, 140, 120); shake(2.0f); net_send_fire(); fire_weapon(s_active); }   // time up -> the shot goes
    }
    // camera target: FIRE frames shooter+shell; SETTLE dwell holds the crater; else the active tank
    if (s_phase == TP_FIRE) {
        float fx, fy;
        bool have = ufo_active() ? (fx = s_ufo.x, fy = s_ufo.y, true) : proj_focus(&fx, &fy);
        if (have) {
            float w = clampf(fabsf(fx - s_tk[s_active].x) / (WW * 0.5f), 0.35f, 0.85f);
            float foc = s_tk[s_active].x + (fx - s_tk[s_active].x) * w;
            s_camtgt = clampf(foc - W / 2, 0, WW - W);
            if (!pany) { float vy_t = clampf((GTOP + 30) - fy, -10, 14); s_camY += (vy_t - s_camY) * clampf(dt / 200.0f, 0, 1); }
        }
    } else if (s_phase == TP_SETTLE && s_dwell_t > 0) {
        s_camtgt = clampf(s_dwell_x - W / 2, 0, WW - W);
        if (!pany) { float vy_t = clampf((GTOP + 30) - s_dwell_y, -10, 14); s_camY += (vy_t - s_camY) * clampf(dt / 220.0f, 0, 1); }
    } else {
        s_camtgt = clampf(s_tk[s_active].x - W / 2, 0, WW - W);
    }
    float ease = (s_phase == TP_FIRE) ? (dt / 70.0f) : (s_phase == TP_TURN) ? (dt / 150.0f) : (dt / 110.0f);
    s_cam += (s_camtgt - s_cam) * clampf(ease, 0, 1);

    if (s_phase == TP_TURN) {
        s_turn_t -= dt;
        if (s_turn_t <= 0) { s_phase = TP_AIM; s_aim_deadline = s_now + TURN_SECS * 1000; s_last_tick = TURN_SECS + 1; }   // arm the net turn clock
    } else if (s_phase == TP_AIM && ai_turn) {
        if (!s_ai_planned && s_aiwait <= 0) { ai_plan(); s_ai_planned = true; s_aiwait = 700; }
        s_aiwait -= dt;
        if (s_ai_planned) {                                   // visibly swing the barrel onto the planned aim
            if (s_tk[1].elev  < s_ai_tgtE) s_tk[1].elev++;  else if (s_tk[1].elev  > s_ai_tgtE) s_tk[1].elev--;
            if (s_tk[1].power < s_ai_tgtP) s_tk[1].power += 2; else if (s_tk[1].power > s_ai_tgtP) s_tk[1].power -= 2;
        }
        if (s_ai_planned && s_aiwait <= 0) fire_weapon(1);
    } else if (s_phase == TP_FIRE && (sim_here || s_spectate)) {
        step_proj(dt); if (ufo_active()) ufo_step(dt);
        if (!any_proj() && !ufo_active()) s_phase = TP_SETTLE;   // spectator(passive): sim the arc for the camera, then hold for the authoritative RESULT
    } else if (s_phase == TP_SETTLE && sim_here) {
        if (s_dwell_t > 0) { s_dwell_t -= dt; settle_fall_only(dt); }
        else settle(dt);
    } else if (s_phase == TP_OVER) {
        if (!any_proj() && !ufo_active() && s_shake <= 0 && s_nuke_t <= 0) {
            for (int i = 0; i < 4; i++) spark_burst(frnd(40, W - 40), frnd(20, H / 2), 12, COL_GOLD);   // confetti pop
            flash(s_tk[0].dead ? COL_P1 : COL_P0, 130); go(ST_OVER); return true;
        }
    }

    if (s_now - s_frame < 16) return false;
    s_frame = s_now; s_anim++;
    return true;
}
static void on_enter(void) {
    ensure_dirs();
    cfg_read();
    ld_ensure_valid();                         // start from a legal 10-weapon loadout (saved or default)
    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(80);
    presynth();
    s_screen = ST_MENU; s_msel = 0; s_anim = 0; s_wins[0] = s_wins[1] = 0;
    s_now = s_last = s_frame = now_ms(); s_inE = s_inP = 0;
    s_mode = MODE_AI; s_seat = 0; s_haspeer = false; s_join_pending = false; s_guest_in = false; s_nroom = s_rsel = 0; s_rel_on = false;
    pnet_start();   // ESP-NOW up for the lobby/multiplayer (Wi-Fi STA stays under NX_NET_APP)
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);
    nucleo_app_set_fullscreen(false);
    set_hint();
    sfx(14);   // menu theme
    nucleo_app_request_draw();
}
static void on_exit(void) { net_send_bye(); pnet_stop(); nucleo_audio_stop(); cfg_write(); }

extern "C" void nucleo_register_tanks(void) {
    static const nucleo_app_def_t app = {
        "tanks", "Tanks", "Games", "Artiglieria a turni: terreno distruttibile, 29 armi, scudi, biomi, vs CPU",
        'T', C_GREEN, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP
    };
    nucleo_app_register(&app);
}
