// app_constellations.cpp — NucleoOS "Costellazioni": an Elite-micro space-trader game (Games).
//
// A long, story-driven trading game built to the OS's constraints and pushed for polish.
// On top of the trade/explore loop sits a Wing-Commander-style ACTION layer: dock at a system,
// take a contract from the Mission Bay (patrol / bounty / escort / defend), read the briefing,
// then fly a real-time dogfight in the system's space — steer the Lucciola, manage throttle and
// shields, gun down enemy fighters and aces — and debrief for credits + reputation. Hyperspace
// jumps can also drop you into a pirate ambush. The dogfight runs entirely on the existing ~30 Hz
// poll handler with all combat state in static arrays (no heap, no decoder), so the exclusive /
// no-hoarding rule still holds. Laser and Shield are shipyard upgrades, tying trade money to
// combat power; pilot rank grows with kills.
//
//   • RAM / exclusive-mode: the game-state pools (systems cache + combat arrays, ~3 KB) are HEAP,
//     allocated on enter and freed on exit (zero boot-time cost). Because that calloc must succeed in
//     the tight, fragmented runtime heap, the app declares exclusive_flags = NX_NET_APP: the framework
//     frees ~60 KB (httpd/mDNS/voice/L1) BEFORE on_enter, so the pools always fit (no launch OOM) and
//     the freed I2S line makes the chiptune SFX reliable — same contract as Reactor/Slots/Sandgarden.
//   • Flicker: we draw only with d.<...> in on_draw(); the run loop composites the whole frame into
//     the shared canvas and blits once (ANTI-FLICKER technique 1). For smooth motion we register a
//     ~30 Hz poll handler that animates ONLY the live screens (title / map / cinematics) and returns
//     false elsewhere, so text screens repaint only on input.
//   • Input: the framework routes LEFT/BACK to the back-handler and everything else to on_key — so
//     LEFT is handled in on_back (screen-local nav) and BACK pops a screen, closing the app only at
//     the title (same split app_theme/app_recorder use).
//   • Audio: short chiptune cues are synthesized once to SD WAVs (notify_synth) and cached. If the
//     player drops a real WAV at /sd/data/costellazioni/custom/<name>.wav it is used instead — so
//     downloaded sounds "just work" without bloating the build. WAV needs no canvas release.
//   • Persistence: a small binary save + a tiny settings file on SD, written atomically (tmp+rename).
//
// Texts are ASCII only (the M5GFX bitmap font has no accents): Italian uses the apostrophe form.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "nucleo_fx3d.h"     // reusable pseudo-3D toolkit: Mode-7 grid + flat-shaded polygon ships
#include "nucleo_exclusive.h" // NX_NET_APP: free ~60KB before on_enter so the heap pools always allocate
#include "nucleo_ui.h"        // nucleo_ui_is_adv(): gate the tilt setting to the ADV
#include "notify_synth.h"     // notify_voice_t + notify_synth_voices_wav (pure inline, stdio+math)
// BMI270 tilt seam. Forward-declared (not #include "nucleo_imu.h") so we don't pull nucleo_imu's
// include dir into the whole nucleo_app component and recompile every sibling source. Symbols
// resolve at final link since main already pulls nucleo_imu in — same trick as nucleo_anima_l1_unload().
extern "C" {
    bool nucleo_imu_present(void);
    bool nucleo_imu_tilt(float *tx, float *ty);
    void nucleo_imu_recenter(void);
}
#include "constellations_content.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <stdlib.h>

extern "C" {
#include "nucleo_audio.h"     // nucleo_audio_play / is_playing / stop
#include "esp_timer.h"        // esp_timer_get_time (ms clock)
}
#include "esp_http_server.h"  // cross-play save endpoint /api/game/costellazioni/save
#include "nucleo_auth.h"      // NUCLEO_AUTH_GUARD (include dir via CMakeLists; same as /api/display)
#include "cJSON.h"            // parse the JSON save POSTed by the web Game Center

// ============================ palette (deep-space scene) =====================
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
#define COL_SPACE  rgb(7, 9, 22)
#define COL_PANEL  rgb(16, 20, 40)
#define COL_WHITE  0xFFFF
#define COL_CYAN   rgb(96, 206, 232)
#define COL_AMBER  rgb(255, 190, 64)
#define COL_GREEN  rgb(118, 230, 140)
#define COL_RED    rgb(240, 92, 80)
#define COL_GREY   rgb(150, 160, 184)
#define COL_DIM    rgb(78, 88, 116)
#define COL_PURPLE rgb(168, 130, 230)

// ---- watch-UI layout constants (Wear-OS-style big-row lists) ----------------
#define MARGIN   8                  // frame inset both edges -> content x in [8..232]
#define CW       (W - 2 * MARGIN)   // card width = 224 (x 8..232)
#define SCRL_X   233                // scroll thumb left (x 233..235, >=1px from edge)
#define HDR_H    26                 // title-band height (y 0..25); lists start at y 28
#define ROW_H    30                 // focused-row band (fits a size-2 line + a size-1 detail)
#define ROW_SM   18                 // unfocused neighbour band
#define PILL_H   26                 // selection pill height inside ROW_H
#define ACC_W    4                  // left accent rail width
// additive palette (no existing macro changed)
#define COL_FOCUS  rgb(28, 50, 96)    // selection pill fill
#define COL_FOCUS2 rgb(70, 120, 200)  // pill border + accent rail + scroll thumb
#define COL_FADE   rgb(58, 66, 92)    // far neighbour text (curved-rim fade)
#define COL_TRACK  rgb(28, 34, 54)    // scroll track / empty pips
enum { TIER_FAR = 0, TIER_NEAR = 1, TIER_FOCUS = 2 };   // row painter focus tier

// ============================ runtime state ==================================
enum { ST_TITLE = 0, ST_SETTINGS, ST_CINE, ST_MAP, ST_SYSTEM, ST_MARKET, ST_SHIPYARD, ST_PLANCIA, ST_EVENT,
       ST_MISSIONS, ST_BRIEF, ST_COMBAT, ST_DEBRIEF };

#define SAVE_MAGIC 0x4C545343u   // 'CSTL'
#define SAVE_VER   3             // bumped: procedural universe (seed/sector); ver<3 saves reset
#define CFG_MAGIC  0x47464343u   // 'CCFG'
#define DIR        "/sd/data/costellazioni"

struct Save {
    uint32_t magic; uint16_t ver; uint16_t pad;
    int      credits, fuel, fuel_max, hull, hull_max, cargo_max, jump_range, sensors;
    int      weapon, shield_max;          // laser level 0..4; shield capacity (40 + 30*lvl)
    int      sys;
    int      cargo[NGOODS];
    int      rep[NFAC];
    uint32_t flags, beacon_lit, epoch, missions_done, kills;
    uint32_t seed, sector;       // procedural universe: master seed + current sector (infinite)
};
static Save g;
// PROCEDURAL world: the current sector's NSYS systems live in this regenerated cache.
// `#define SYSTEMS cur_sys` lets every existing `SYSTEMS[i].field` read site work unchanged.
static Sys *cur_sys;   // NSYS — heap, allocated on app enter / freed on exit (zero boot-time .bss)
#define SYSTEMS cur_sys

static int  g_lang  = LANG_IT;   // mirrored in the settings file so the menu remembers it
static int  g_audio = 1;
static int  g_tilt  = 0;         // tilt controller on/off (Cardputer ADV BMI270); persisted in cfg

static int  s_screen;
static bool s_ingame;            // a run is active (continue/new), drives autosave on exit
static bool s_has_save;
static int64_t s_now, s_last_frame;
static unsigned s_anim;
static float s_scroll[3];

static int  s_msel, s_setsel, s_hubsel, s_mktsel, s_yardsel, s_evsel, s_target;
static int  s_cine, s_ev;
static int64_t s_cine_t0;
static char s_status[40];        // transient one-liner on market/shipyard

// ---- action combat: first-person pseudo-3D rail shooter (Star-Wars arcade) --
// Everything here is static (.bss), no heap: the no-hoarding rule kept by owning nothing.
// The camera sits in the cockpit looking down +ez; enemies fly toward you, growing by 1/ez.
#define NBOLT 18                     // enemy laser tracers (player fire is hitscan, not a projectile)
#define NFOE  6
#define NWARP 56                     // forward-streaking 3D starfield
#define CX     120.0f                // vanishing-point x (= W/2)
#define FOCAL  140.0f                // focal length (px); larger = more zoom / narrower FOV
#define ZNEAR    6.0f                // depth at which a foe/bolt reaches the cockpit
#define ZFAR   260.0f                // spawn / far-clip depth
#define ZCONV  140.0f                // depth where a tracer's converging line meets the rim
#define ZWARD  200.0f                // fixed depth of the escort/defend ward
#define RAIL    46.0f                // base forward-flight speed (starfield streak)
// reticle glide tuning (impulse + drag; see aim_steer): KICK = instant tap response, PUSH = per-repeat
// add so a held arrow accelerates, VMAX = terminal speed cap, FRIC = drag toward a soft inertial stop.
#define AIM_KICK 210.0f
#define AIM_PUSH 150.0f
#define AIM_VMAX 240.0f
#define AIM_FRIC   6.5f
struct Bolt { float ex, ey, ez, tx, ty, vz; int16_t life; uint8_t on, foe, aimward; };
struct Foe  { float ex, ey, ez, wphase, bank, engagez; int16_t hp, hpmax, firecd, strafecd, hitms; uint8_t on, kind, passed, strafe; };
static Bolt *s_bolt;                          // NBOLT — heap, allocated on enter / freed on exit
static Foe  *s_foe;                           // NFOE  — idem
struct Warp { float ex, ey, ez; };
static Warp *s_warp;                          // NWARP — forward-streaking starfield
// ---- juice pools (all .bss, no heap) ----------------------------------------
#define NPART 40        // debris + spark particles (shared round-robin pool)
#define NSHK  3         // expanding shockwave rings
#define NRIP  2         // shield-absorb ripples on the canopy
enum { PK_DEBRIS = 0, PK_SPARK, PK_STREAK };
struct Part { float ex, ey, ez, vx, vy, vz; int16_t life, life0; uint8_t on, kind; uint16_t col; };
struct Shk  { float cx_, cy_; int16_t life, life0; uint8_t on; uint16_t col; };
struct Rip  { int x, y; int16_t life, life0; uint8_t on; };
static Part *s_part;                          // NPART — heap, allocated on enter / freed on exit
static Shk  s_shk[NSHK];
static Rip  s_rip[NRIP];
// model-shatter deaths: a tiny pool that keeps a killed foe's pose so its FLASH model can be exploded
// into cooling, tumbling shards for ~0.4 s (fx3d::shatter). Pose only -> ~128 B .bss, no heap.
#define NDEATH 4
struct Death { uint8_t on, model; int16_t x, y, r; float yaw, bank; uint16_t col; int16_t t, t0; };
static Death s_death[NDEATH];
static int  s_part_rr;                 // round-robin cursor
static int64_t s_flash_until;          // brief dim full-frame flash window
static float   s_flash_x, s_flash_y;   // core-flash centre
static int     s_flash_r0;             // core-flash radius (capped)
static int64_t s_muz_until;            // twin muzzle-flash window
static int64_t s_hullvig_until;        // red edge vignette on a hull hit
static int64_t s_nearmiss_ms;          // rate-limit for the pass-by whoosh
static int64_t s_alarm_ms;             // rate-limit for the hull-critical klaxon
static int64_t s_smoke_ms;             // rate-limit for the wounded-foe ember trail
// the dogfight config in flight (filled from a Mission or synthesised for an ambush)
struct CombatCfg {
    int type, foe_fac, waves, per_wave, foe_hp, foe_dmg, ace;
    float foe_speed;                 // approach tuning
    int reward_cr, kill_cr, rep_fac, rep_gain, enemy_rep_fac, enemy_rep_loss;
};
static CombatCfg s_cc;
static float s_aimx, s_aimy;         // reticle screen position (float for smooth glide)
static float s_aim_hv, s_aim_vv;     // reticle velocity (px/s): impulse+drag glide, see aim_steer()
static int64_t s_aim_h_until, s_aim_v_until;   // "input recent" window per axis (gates the aim-assist)
static int   s_throttle10;           // repurposed as BOOST 0..10
static int   s_lock;                 // index of foe locked by the reticle, -1 = none
static int64_t s_lock_since, s_fire_flash_until, s_pfire_ms;
static int64_t s_hitmark_until;      // brief crosshair hitmarker window on confirmed damage (juice)
static float s_shake, s_cy;          // screen-shake magnitude; vanishing-point y (set on reset)
static int   s_shield, s_shieldmax, s_kills, s_mkills;
static int   s_wave, s_wave_left;
static float s_spawn_timer;          // ms until next wave when the field is clear
static int64_t s_shield_hit_ms;      // last time we took a hit (gates shield regen)
static int   s_mission;              // MISSIONS index, or -1 for a random ambush
static int   s_pick;                 // mission highlighted on the board/brief
static int   s_misssel, s_briefsel;
static int   s_result, s_earn_cr;    // debrief: 0 run / 1 win / -1 fail; credits earned
static char  s_cmsg[40];
static int64_t s_cmsg_until, s_combat_t0;
static uint8_t s_ward_on;            // escort/defend protectee present
static int   s_ward_hp, s_ward_max;
static float s_ward_x, s_ward_vx;

// ---- arcade layer: player missiles, power-ups, combo, per-wave look ---------
// All .bss (no heap), like s_shk/s_rip — the no-hoarding rule kept by owning nothing.
#define NMSL 3                       // missiles airborne at once (also the ammo cap = "max 3")
struct Msl { float ex, ey, ez; int target; uint8_t on; };
static Msl s_msl[NMSL];
static int     s_msl_ammo;           // reserve missiles 0..NMSL (refills slowly)
static float   s_msl_reload;         // ms accumulator toward the next +1 missile
static int64_t s_rapid_until;        // rapid-fire power-up window
#define NPU 3                        // power-ups drifting toward the cockpit at once
enum { PU_SHIELD = 0, PU_REPAIR, PU_MISSILE, PU_RAPID, PU_KINDS };
struct Pickup { float ex, ey, ez; int16_t life; uint8_t on, kind; };
static Pickup s_pu[NPU];
static int     s_combo; static int64_t s_combo_until;     // arcade kill combo
static uint16_t s_wave_tint;         // per-wave enemy + backdrop colour (rotates each wave)
static char    s_toast[28]; static int64_t s_toast_until; // brief power-up pickup banner

// forward decls used before their definitions
static void combat_begin_ambush(void);
static int  eligible_missions(int *out);
static inline uint16_t shade(uint16_t c, int num, int den);   // defined lower; draw_warp uses it earlier

// ============================ tiny helpers ===================================
static inline const char *tx(const char *it, const char *en) { return g_lang ? en : it; }
static inline const char *lp(const char *const p[2]) { return p[g_lang ? 1 : 0]; }
static inline uint32_t bit(int b) { return 1u << b; }
static inline bool flag(int b) { return (g.flags & bit(b)) != 0; }
static inline int  clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
static void req(void) { nucleo_app_request_draw(); }

static int cargo_used(void) { int n = 0; for (int i = 0; i < NGOODS; i++) n += g.cargo[i]; return n; }
static float sys_dist(int a, int b)
{
    float dx = (float)(SYSTEMS[a].x - SYSTEMS[b].x), dy = (float)(SYSTEMS[a].y - SYSTEMS[b].y);
    return sqrtf(dx * dx + dy * dy);
}
static int jump_cost(float dist) { int c = (int)(dist / 10.0f + 0.5f); return c < 1 ? 1 : c; }
static int beacons_total(void) { int n = 0; for (int i = 0; i < NSYS; i++) if (SYSTEMS[i].beacon) n++; return n; }
static int beacons_lit(void)   { int n = 0; for (int i = 0; i < NSYS; i++) if (SYSTEMS[i].beacon && (g.beacon_lit & bit(i))) n++; return n; }

// tiny LCG so we don't pull in <random>; seeded from the clock at enter.
static uint32_t s_rng = 0x1234abcdu;
static unsigned esp_random_local(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 1; }
static int rnd(int n) { return n > 0 ? (int)(esp_random_local() % (unsigned)n) : 0; }

// ============================ economy ========================================
static int refuel_price(int sys)
{
    switch (SYSTEMS[sys].econ) {
        case EC_REFU: return 6;  case EC_AGRI: return 11; case EC_MINE: return 9;
        case EC_INDU: return 9;  default:      return 13;   // TECH
    }
}
static int unit_buy(int sys, int good)
{
    long p = (long)GOODS[good].base * ECONMOD[SYSTEMS[sys].econ][good] / 100;
    uint32_t h = (uint32_t)sys * 2654435761u ^ (uint32_t)good * 40499u ^ g.epoch * 2246822519u;
    h ^= h >> 13;
    int var = (int)(h % 45) - 22;                 // -22..+22 %
    p = p * (100 + var) / 100;
    int fac = SYSTEMS[sys].faction;
    if (fac >= 0 && g.rep[fac] > 0) { int disc = g.rep[fac] / 8; if (disc > 15) disc = 15; p = p * (100 - disc) / 100; }
    return p < 1 ? 1 : (int)p;
}
static int unit_sell(int sys, int good) { int p = unit_buy(sys, good) * 88 / 100; return p < 1 ? 1 : p; }

// ============================ persistence ====================================
static void ensure_dirs(void)
{
    mkdir("/sd/data", 0777);
    mkdir(DIR, 0777);
    mkdir(DIR "/sfx", 0777);
    mkdir(DIR "/custom", 0777);
}
static bool save_read(Save *out)
{
    FILE *f = fopen(DIR "/save.bin", "rb");
    if (!f) return false;
    size_t n = fread(out, sizeof *out, 1, f);
    fclose(f);
    return n == 1 && out->magic == SAVE_MAGIC && out->ver == SAVE_VER;
}
// Atomically write a given Save to SD (tmp + rename). Shared by the game and the web save endpoint.
static bool save_write_buf(const Save *src)
{
    ensure_dirs();
    FILE *f = fopen(DIR "/save.bin.tmp", "wb");
    if (!f) return false;
    size_t n = fwrite(src, sizeof *src, 1, f);
    fclose(f);
    if (n != 1) { remove(DIR "/save.bin.tmp"); return false; }
    remove(DIR "/save.bin");
    rename(DIR "/save.bin.tmp", DIR "/save.bin");
    return true;
}
static bool save_write(void)
{
    g.magic = SAVE_MAGIC; g.ver = SAVE_VER;
    if (!save_write_buf(&g)) return false;
    s_has_save = true;
    return true;
}
static void save_wipe(void) { remove(DIR "/save.bin"); s_has_save = false; }

static void cfg_write(void)
{
    ensure_dirs();
    FILE *f = fopen(DIR "/cfg.bin", "wb");
    if (!f) return;
    struct { uint32_t m; int l, a, t; } c = { CFG_MAGIC, g_lang, g_audio, g_tilt };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a, t; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    if (n == 1 && c.m == CFG_MAGIC) { g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; g_tilt = c.t ? 1 : 0; fclose(f); return; }
    // Older 3-field cfg (pre-tilt): re-read just {magic,lang,audio} so the prefs survive the upgrade.
    rewind(f);
    struct { uint32_t m; int l, a; } o;
    n = fread(&o, sizeof o, 1, f);
    fclose(f);
    if (n == 1 && o.m == CFG_MAGIC) { g_lang = o.l ? 1 : 0; g_audio = o.a ? 1 : 0; }
}

// ============================ cross-play save endpoint =======================
// GET/POST /api/game/costellazioni/save — the web Game Center continues the SAME campaign as the
// native game by reading/writing the canonical /sd/data/costellazioni/save.bin as JSON. save.bin
// stays the single source of truth; this only (de)serializes it (no in-RAM game state touched, so
// it's safe from the httpd task). Registered at boot by nucleo_httpd (same pattern as /api/display).
// The JSON shape mirrors the Save struct field-for-field; clamps match the native invariants.
static_assert(NGOODS == 8 && NFAC == 4, "save JSON hardcodes 8 cargo / 4 rep — update if these change");

static esp_err_t cstl_save_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    Save s;
    httpd_resp_set_type(req, "application/json");
    if (!save_read(&s)) { httpd_resp_set_status(req, "404 Not Found"); httpd_resp_sendstr(req, "{\"error\":\"nosave\"}"); return ESP_OK; }
    char out[720];
    snprintf(out, sizeof out,
        "{\"ver\":%u,\"credits\":%d,\"fuel\":%d,\"fuel_max\":%d,\"hull\":%d,\"hull_max\":%d,"
        "\"cargo_max\":%d,\"jump_range\":%d,\"sensors\":%d,\"weapon\":%d,\"shield_max\":%d,\"sys\":%d,"
        "\"cargo\":[%d,%d,%d,%d,%d,%d,%d,%d],\"rep\":[%d,%d,%d,%d],"
        "\"flags\":%u,\"beacon_lit\":%u,\"epoch\":%u,\"missions_done\":%u,\"kills\":%u,\"seed\":%u,\"sector\":%u}",
        s.ver, s.credits, s.fuel, s.fuel_max, s.hull, s.hull_max, s.cargo_max, s.jump_range, s.sensors,
        s.weapon, s.shield_max, s.sys,
        s.cargo[0], s.cargo[1], s.cargo[2], s.cargo[3], s.cargo[4], s.cargo[5], s.cargo[6], s.cargo[7],
        s.rep[0], s.rep[1], s.rep[2], s.rep[3],
        (unsigned)s.flags, (unsigned)s.beacon_lit, (unsigned)s.epoch, (unsigned)s.missions_done,
        (unsigned)s.kills, (unsigned)s.seed, (unsigned)s.sector);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}
static esp_err_t cstl_save_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_type(req, "application/json");
    int len = req->content_len;
    if (len <= 0 || len > 1024) { httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_sendstr(req, "{\"error\":\"badlen\"}"); return ESP_OK; }
    char buf[1100];
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_sendstr(req, "{\"error\":\"recv\"}"); return ESP_OK; }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) { httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_sendstr(req, "{\"error\":\"json\"}"); return ESP_OK; }
    Save s; memset(&s, 0, sizeof s);
    s.magic = SAVE_MAGIC; s.ver = SAVE_VER;
#define JI(name, field, lo, hi) do { cJSON *it = cJSON_GetObjectItem(j, name); \
        if (cJSON_IsNumber(it)) s.field = clampi((int)it->valuedouble, lo, hi); } while (0)
#define JU(name, field) do { cJSON *it = cJSON_GetObjectItem(j, name); \
        if (cJSON_IsNumber(it) && it->valuedouble >= 0) s.field = (uint32_t)it->valuedouble; } while (0)
    JI("credits", credits, 0, 9999999);
    JI("fuel", fuel, 0, 9999); JI("fuel_max", fuel_max, 1, 9999);
    JI("hull", hull, 0, 9999); JI("hull_max", hull_max, 1, 9999);
    JI("cargo_max", cargo_max, 1, 9999); JI("jump_range", jump_range, 1, 9999);
    JI("sensors", sensors, 0, 9); JI("weapon", weapon, 0, 4); JI("shield_max", shield_max, 0, 9999);
    JI("sys", sys, 0, NSYS - 1);
    cJSON *carr = cJSON_GetObjectItem(j, "cargo");
    if (cJSON_IsArray(carr)) { int n = cJSON_GetArraySize(carr); if (n > NGOODS) n = NGOODS;
        for (int i = 0; i < n; i++) { cJSON *e = cJSON_GetArrayItem(carr, i); if (cJSON_IsNumber(e)) s.cargo[i] = clampi((int)e->valuedouble, 0, 9999); } }
    cJSON *rarr = cJSON_GetObjectItem(j, "rep");
    if (cJSON_IsArray(rarr)) { int n = cJSON_GetArraySize(rarr); if (n > NFAC) n = NFAC;
        for (int i = 0; i < n; i++) { cJSON *e = cJSON_GetArrayItem(rarr, i); if (cJSON_IsNumber(e)) s.rep[i] = clampi((int)e->valuedouble, -100, 100); } }
    JU("flags", flags); JU("beacon_lit", beacon_lit); JU("epoch", epoch);
    JU("missions_done", missions_done); JU("kills", kills);
    JU("seed", seed); JU("sector", sector);
#undef JI
#undef JU
    cJSON_Delete(j);
    if (s.epoch < 1) s.epoch = 1;
    bool ok = save_write_buf(&s);
    if (ok) s_has_save = true;
    char out[96];
    snprintf(out, sizeof out, "{\"ok\":%s,\"bytes\":%u,\"epoch\":%u}", ok ? "true" : "false", (unsigned)sizeof(Save), (unsigned)s.epoch);
    if (!ok) httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}
// ============================ procedural generator ===========================
// DETERMINISTIC, byte-identical to apps/games/www/games/constellations-gen.js (validated by the
// gentest endpoint vs a JS harness, the same way the economy hash validated at 0 mismatches).
// Same (seed, sector) -> identical universe on Cardputer and web. uint32 math only.
static inline uint32_t pg_hash32(uint32_t x)
{
    x ^= x >> 16; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16; return x;
}
static inline uint32_t pg_hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 0x9E3779B1u;
    h ^= pg_hash32(a + 0x85EBCA6Bu); h *= 2654435761u;
    h ^= pg_hash32(b + 0xC2B2AE35u); h *= 2654435761u;
    h ^= pg_hash32(c + 0x27D4EB2Fu); return pg_hash32(h);
}
enum { PG_COORD = 1, PG_ECON = 2, PG_FAC = 3, PG_BEACON = 4, PG_NAME = 5, PG_MISSION = 6, PG_MCOUNT = 7, PG_FLAVOR = 8 };
static inline uint32_t pg_rng_sys(uint32_t seed, uint32_t sector, uint32_t idx, uint32_t dom, uint32_t salt)
{ return pg_hash3(seed ^ dom, sector, ((idx & 0xff) << 8) | (salt & 0xff)); }
static inline uint32_t pg_rng_mis(uint32_t seed, uint32_t sector, uint32_t sysIdx, uint32_t slot, uint32_t dom, uint32_t salt)
{ return pg_hash3(seed ^ dom, ((sector & 0xffffff) << 8) | (sysIdx & 0xff), ((slot & 0xff) << 8) | (salt & 0xff)); }

static const char *PG_PRE[16] = { "Ve","Ach","El","Ty","Cu","Ro","For","Qui","Ze","Ab","Xan","Or","Ka","Ny","Vor","Lu" };
static const char *PG_MID[8]  = { "per","ron","iso","cho","sta","rax","mir","" };
static const char *PG_SUF[8]  = { "","Primo","Nova","Reach","IX","Gate","Hub","Cluster" };
static const int   PG_FAC_LADDER[4] = { 40, 65, 88, 100 };
static const int   PG_FAC_RIVAL[4]  = { F_RELITTI, F_RELITTI, F_GILDA, F_RELITTI };

static void pg_name(uint32_t seed, uint32_t sector, uint32_t idx, char *out, int cap)
{
    uint32_t h = pg_rng_sys(seed, sector, idx, PG_NAME, 0);
    const char *p = PG_PRE[h % 16], *m = PG_MID[(h >> 4) % 8], *s = PG_SUF[(h >> 7) % 8];
    if (s[0]) snprintf(out, cap, "%s%s %s", p, m, s);
    else      snprintf(out, cap, "%s%s", p, m);
}
// A generated system's gameplay fields (the procedural replacement for a SYSTEMS[] row).
struct GenSys { int x, y, econ, faction, beacon; char name[16]; };
static void pg_system(uint32_t seed, uint32_t sector, int idx, GenSys *o)
{
    int col = idx % 4, row = idx / 4;
    o->x = 2 + col * 24 + (int)(pg_rng_sys(seed, sector, idx, PG_COORD, 0) % 18);
    o->y = 2 + row * 32 + (int)(pg_rng_sys(seed, sector, idx, PG_COORD, 1) % 26);
    o->econ = (int)(pg_rng_sys(seed, sector, idx, PG_ECON, 0) % NECON);
    int r = (int)(pg_rng_sys(seed, sector, idx, PG_FAC, 0) % 100), fac = NFAC - 1;
    for (int f = 0; f < NFAC; f++) if (r < PG_FAC_LADDER[f]) { fac = f; break; }
    o->faction = fac;
    int bps = 3 + (int)(sector % 3), rank = 0;
    uint32_t mine = pg_rng_sys(seed, sector, idx, PG_BEACON, 0);
    for (int j = 0; j < NSYS; j++) { uint32_t hj = pg_rng_sys(seed, sector, j, PG_BEACON, 0);
        if (hj < mine || (hj == mine && j < idx)) rank++; }
    o->beacon = (rank < bps) ? 1 : 0;
    pg_name(seed, sector, idx, o->name, sizeof o->name);
}
// A generated mission's combat/reward fields (the procedural replacement for a MISSIONS[] row).
struct GenMis { int type, offer_fac, foe_fac, waves, per_wave, foe_hp, foe_dmg, foe_speed_pml, ace,
                    reward_cr, kill_cr, rep_gain, enemy_rep_loss, tier; };
static int pg_mission_count(uint32_t seed, uint32_t sector, int sysIdx, int sysFaction)
{
    if (sysFaction == F_ECO) return 0;
    return 3 + (int)(pg_rng_mis(seed, sector, sysIdx, 0, PG_MCOUNT, 0) % 3);   // 3..5 — fuller mission boards
}
static void pg_mission(uint32_t seed, uint32_t sector, int sysIdx, int slot, int sysFaction, GenMis *o)
{
    int tier = 1 + (int)sector; if (tier > 12) tier = 12;
    uint32_t b = pg_rng_mis(seed, sector, sysIdx, slot, PG_MISSION, 0);
    o->type = (int)(b % 4);
    o->offer_fac = sysFaction;
    o->foe_fac = PG_FAC_RIVAL[sysFaction];
    o->waves = 6 + (int)((b >> 4) % 4) + (tier >= 4 ? 1 : 0) + (tier >= 8 ? 2 : 0);   // 6..13, longer sorties
    o->per_wave = 3 + (int)((b >> 8) % 2);                                             // 3..4 (+ ace stays <= NFOE 6)
    o->foe_hp = 36 + tier * 7 + (int)((b >> 12) % 10);                                 // tougher: foes take more hits
    o->foe_dmg = 8 + tier + (int)((b >> 16) % 3);                                      // hits harder
    o->foe_speed_pml = 820 + tier * 45;
    o->ace = ((int)((b >> 20) % 100) < (15 + tier * 3)) ? 1 : 0;
    o->kill_cr = 18 + tier * 4;
    o->reward_cr = (60 + tier * 40) * o->waves;
    o->rep_gain = 3 + tier;
    o->enemy_rep_loss = 2 + tier / 2;
    o->tier = tier;
}

// GET /api/game/costellazioni/gentest?seed=&sector= — emit the generated sector as JSON so a JS
// harness can confirm the C and JS generators are byte-identical (debug/parity; harmless).
static esp_err_t cstl_gentest_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char q[64] = {0}, sv[16] = {0}, kv[16] = {0};
    if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        httpd_query_key_value(q, "seed", sv, sizeof sv);
        httpd_query_key_value(q, "sector", kv, sizeof kv);
    }
    uint32_t seed = (uint32_t)strtoul(sv, NULL, 10), sector = (uint32_t)strtoul(kv, NULL, 10);
    httpd_resp_set_type(req, "application/json");
    char b[200];
    snprintf(b, sizeof b, "{\"seed\":%u,\"sector\":%u,\"systems\":[", (unsigned)seed, (unsigned)sector);
    httpd_resp_send_chunk(req, b, HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < NSYS; i++) {
        GenSys s; pg_system(seed, sector, i, &s);
        snprintf(b, sizeof b, "%s{\"x\":%d,\"y\":%d,\"econ\":%d,\"faction\":%d,\"beacon\":%d,\"name\":\"%s\"}",
                 i ? "," : "", s.x, s.y, s.econ, s.faction, s.beacon, s.name);
        httpd_resp_send_chunk(req, b, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_sendstr_chunk(req, "],\"missions\":[");
    GenSys s0; pg_system(seed, sector, 0, &s0);
    int nm = pg_mission_count(seed, sector, 0, s0.faction);
    for (int slot = 0; slot < nm; slot++) {
        GenMis m; pg_mission(seed, sector, 0, slot, s0.faction, &m);
        snprintf(b, sizeof b,
            "%s{\"type\":%d,\"offer_fac\":%d,\"foe_fac\":%d,\"waves\":%d,\"per_wave\":%d,\"foe_hp\":%d,"
            "\"foe_dmg\":%d,\"foe_speed_pml\":%d,\"ace\":%d,\"reward_cr\":%d,\"kill_cr\":%d,\"rep_gain\":%d,\"enemy_rep_loss\":%d}",
            slot ? "," : "", m.type, m.offer_fac, m.foe_fac, m.waves, m.per_wave, m.foe_hp, m.foe_dmg,
            m.foe_speed_pml, m.ace, m.reward_cr, m.kill_cr, m.rep_gain, m.enemy_rep_loss);
        httpd_resp_send_chunk(req, b, HTTPD_RESP_USE_STRLEN);
    }
    snprintf(b, sizeof b, "],\"h32\":%u,\"h3\":%u}", (unsigned)pg_hash32(0x12345678u), (unsigned)pg_hash3(seed, sector, 7));
    httpd_resp_send_chunk(req, b, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
// Registered by nucleo_httpd at boot (forward-declared there; symbol resolves at final link, like /api/display).
extern "C" esp_err_t nucleo_app_register_costellazioni_api(httpd_handle_t server)
{
    httpd_uri_t uri_get  = { .uri = "/api/game/costellazioni/save", .method = HTTP_GET,  .handler = cstl_save_get };
    httpd_uri_t uri_post = { .uri = "/api/game/costellazioni/save", .method = HTTP_POST, .handler = cstl_save_post };
    httpd_uri_t uri_gen  = { .uri = "/api/game/costellazioni/gentest", .method = HTTP_GET, .handler = cstl_gentest_get };
    esp_err_t e1 = httpd_register_uri_handler(server, &uri_get);
    esp_err_t e2 = httpd_register_uri_handler(server, &uri_post);
    esp_err_t e3 = httpd_register_uri_handler(server, &uri_gen);
    return (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

// ============================ per-sector regeneration ========================
// Fill cur_sys[] with the current sector's procedural systems (called on new game, load, advance).
static void regen_sector(void)
{
    for (int i = 0; i < NSYS; i++) {
        GenSys gs; pg_system(g.seed, g.sector, i, &gs);
        cur_sys[i].x = gs.x; cur_sys[i].y = gs.y; cur_sys[i].econ = gs.econ;
        cur_sys[i].faction = gs.faction; cur_sys[i].beacon = gs.beacon;
        memcpy(cur_sys[i].name, gs.name, sizeof cur_sys[i].name);
    }
}
// A deterministic, non-beacon entry slot for a sector (so you start free to explore, not on a Beacon).
static int entry_slot(uint32_t sector)
{
    for (int i = 0; i < NSYS; i++) { GenSys gs; pg_system(g.seed, sector, i, &gs); if (!gs.beacon) return i; }
    return 0;
}

// ---- procedural missions templated into the existing Mission struct -------------------------
static const char *MT_WIN[4][2] = {
    { "Rotte ripulite. Crediti accreditati.",          "Lanes cleared. Credits paid." },
    { "L'asso e' abbattuto. Le stelle brindano a te.", "The ace is down. The stars toast you." },
    { "Convoglio al sicuro. Buon lavoro, pilota.",     "Convoy safe. Good work, pilot." },
    { "La piattaforma regge. Sei la sua sentinella.",  "The platform holds. You are its sentinel." },
};

// ---- procedural mission FLAVOR (ported from constellations-gen.js genMissionFlavor) -------------
// A No Man's Sky-style evocative layer: named raider captains, gangs, archetypes, rarity tiers and
// combat modifiers. It is drawn from a DEDICATED hash domain (PG_FLAVOR) that NEVER perturbs the
// numeric draws above, so the cross-play universe stays byte-identical; the firmware was simply
// missing the layer the web twin already had. Italian matches the JS verbatim; English is the twin.
static const char *FV_PRE[16] = { "Vex","Krull","Mor","Zar","Drix","Nyx","Hask","Orla","Veng","Skar","Rann","Tox","Grim","Vael","Korr","Zael" };
static const char *FV_SUF[16] = { "nor","ax","is","oth","ek","ul","ar","ix","one","ag","eth","os","un","ire","um","or" };
static const char *FV_EPI[10][2] = {
    { "il Rosso","the Red" }, { "Occhio-Morto","Deadeye" }, { "la Lama","the Blade" }, { "il Corvo","the Crow" },
    { "Senza-Volto","the Faceless" }, { "il Flagello","the Scourge" }, { "Mano-Fredda","Coldhand" },
    { "l'Avvoltoio","the Vulture" }, { "il Cremisi","the Crimson" }, { "lo Spettro","the Wraith" } };
static const char *FV_GANG[8][2] = {
    { "Corsari Cremisi","Crimson Corsairs" }, { "Lupi del Vuoto","Void Wolves" }, { "Sciacalli della Cenere","Ash Jackals" },
    { "Predoni di Ferro","Iron Raiders" }, { "Flotta Fantasma","Ghost Fleet" }, { "Branco di Dramir","Dramir Pack" },
    { "Mietitori Neri","Black Reapers" }, { "Vipere del Vuoto","Void Vipers" } };
static const char *FV_MODS[7][2] = {
    { "Nebulosa densa","Dense nebula" }, { "Campo di asteroidi","Asteroid field" }, { "Squadriglia d'elite","Elite squadron" },
    { "Veterani","Veterans" }, { "Branco","Pack" }, { "Taglia maggiorata","Bounty raised" }, { "Tempesta ionica","Ion storm" } };
static const char *FV_RARNAME[4][2] = { { "Comune","Common" }, { "Raro","Rare" }, { "Epico","Epic" }, { "Leggendario","Legendary" } };
static uint16_t fv_rarcol(int r)   // rarity colour, matching the web (grey / cyan / purple / gold)
{
    switch (r) { case 1: return rgb(94,230,255); case 2: return rgb(180,107,224); case 3: return rgb(224,177,59); default: return rgb(159,176,191); }
}
enum { FA_PATROL = 0, FA_HUNT, FA_DUEL, FA_ESCORT, FA_SWEEP, FA_DEFEND };
static const char *FA_NAME[6][2] = {
    { "Pattuglia","Patrol" }, { "Caccia","Hunt" }, { "Duello","Duel" },
    { "Scorta","Escort" }, { "Bonifica","Sweep" }, { "Difesa","Defense" } };

struct Flavor { int rarity, arch, gang, mod[2], nmod; bool has_enemy; };
static Flavor s_fv;                          // filled by cur_mission(); read by the board/brief painters
static char s_mn_it[40], s_mn_en[40];        // generated mission title (IT/EN)
static char s_brief_it[96], s_brief_en[96];  // flavored brief (IT/EN)
static char s_en_it[28], s_en_en[28];        // named target (epithet differs by language)
static Mission s_genm;
#define NMISS_PER_SYS 4

// Generate the mission in `slot` at the current system into s_genm, plus its flavor into s_fv. The
// display/combat code reads s_genm exactly like a flash MISSIONS[] row; the board/brief painters read
// s_fv for the rarity colour, named target, gang and modifiers.
static const Mission *cur_mission(int slot)
{
    int sf = SYSTEMS[g.sys].faction;
    GenMis gm; pg_mission(g.seed, g.sector, g.sys, slot, sf, &gm);
    int t = gm.type;
    uint32_t h  = pg_rng_mis(g.seed, g.sector, g.sys, slot, PG_FLAVOR, 1);
    uint32_t h2 = pg_rng_mis(g.seed, g.sector, g.sys, slot, PG_FLAVOR, 2);
    bool ace = gm.ace != 0;
    // named raider captain (+ epithet for aces)
    const char *pre = FV_PRE[h % 16], *suf = FV_SUF[(h >> 5) % 16];
    if (ace) {
        int e = (int)((h >> 10) % 10);
        snprintf(s_en_it, sizeof s_en_it, "%s%s %s", pre, suf, FV_EPI[e][0]);
        snprintf(s_en_en, sizeof s_en_en, "%s%s %s", pre, suf, FV_EPI[e][1]);
    } else {
        snprintf(s_en_it, sizeof s_en_it, "%s%s", pre, suf);
        snprintf(s_en_en, sizeof s_en_en, "%s%s", pre, suf);
    }
    s_fv.gang = (int)((h >> 16) % 8);
    // archetype + title + brief by mission type (mirror the JS branch-for-branch)
    const char *ti, *te, *bi, *be;
    bool sub_name = false, sub_gang = false;
    if (t == MT_BOUNTY) {
        bool duel = ace && (h2 & 1u);
        s_fv.arch = duel ? FA_DUEL : FA_HUNT;
        ti = duel ? "Duello: %s" : "Caccia: %s"; te = duel ? "Duel: %s" : "Hunt: %s";
        bi = duel ? "Solo tu e %s. Niente gregari, niente fughe." : "Taglia su %s: arriva con la sua scorta.";
        be = duel ? "Just you and %s. No wingmen, no escape." : "Bounty on %s: it arrives with an escort.";
        sub_name = true;
    } else if (t == MT_ESCORT) {
        s_fv.arch = FA_ESCORT;
        ti = "Scorta convoglio"; te = "Convoy escort";
        bi = "Tieni vivo il convoglio: i %s lo vogliono fermo.";
        be = "Keep the convoy alive: the %s want it stopped.";
        sub_gang = true;
    } else if (t == MT_DEFEND) {
        bool sweep = (h2 & 2u);
        s_fv.arch = sweep ? FA_SWEEP : FA_DEFEND;
        ti = sweep ? "Bonifica sciame" : "Difesa faro"; te = sweep ? "Swarm sweep" : "Beacon defense";
        bi = sweep ? "Sciame di droni-saccheggio: tanti, fragili, ovunque." : "Proteggi il faro dai %s finche' non cedono.";
        be = sweep ? "A swarm of scavenger drones: many, fragile, everywhere." : "Protect the beacon from the %s until they break.";
        sub_gang = !sweep;
    } else {
        s_fv.arch = FA_PATROL;
        ti = "Pattuglia"; te = "Patrol";
        bi = "I %s battono la zona. Ricacciali indietro.";
        be = "The %s comb the area. Drive them back.";
        sub_gang = true;
    }
    s_fv.has_enemy = sub_name;
    // title (with the named target where the archetype calls for it)
    if (sub_name) { snprintf(s_mn_it, sizeof s_mn_it, ti, s_en_it); snprintf(s_mn_en, sizeof s_mn_en, te, s_en_en); }
    else          { snprintf(s_mn_it, sizeof s_mn_it, "%s", ti);    snprintf(s_mn_en, sizeof s_mn_en, "%s", te); }
    // brief (substitute the named target or the gang)
    if (sub_name)      { snprintf(s_brief_it, sizeof s_brief_it, bi, s_en_it); snprintf(s_brief_en, sizeof s_brief_en, be, s_en_en); }
    else if (sub_gang) { snprintf(s_brief_it, sizeof s_brief_it, bi, FV_GANG[s_fv.gang][0]); snprintf(s_brief_en, sizeof s_brief_en, be, FV_GANG[s_fv.gang][1]); }
    else               { snprintf(s_brief_it, sizeof s_brief_it, "%s", bi); snprintf(s_brief_en, sizeof s_brief_en, "%s", be); }
    // rarity + modifiers (mirror the JS score thresholds and modifier rolls exactly)
    int score = gm.tier + (ace ? 2 : 0) + (gm.waves >= 5 ? 1 : 0) + (int)((h2 >> 3) % 3);
    s_fv.rarity = score >= 9 ? 3 : score >= 7 ? 2 : score >= 5 ? 1 : 0;
    s_fv.nmod = 0;
    if ((h2 >> 6) % 3 == 0) s_fv.mod[s_fv.nmod++] = (int)((h2 >> 8) % 7);
    if (gm.tier >= 4 && (h2 >> 12) % 3 == 0) { int x = (int)((h2 >> 14) % 7); if (s_fv.nmod == 0 || s_fv.mod[0] != x) s_fv.mod[s_fv.nmod++] = x; }

    s_genm.name[0] = s_mn_it; s_genm.name[1] = s_mn_en;
    s_genm.brief[0] = s_brief_it; s_genm.brief[1] = s_brief_en;
    s_genm.win[0] = MT_WIN[t][0];   s_genm.win[1] = MT_WIN[t][1];
    s_genm.type = gm.type; s_genm.offer_fac = gm.offer_fac; s_genm.foe_fac = gm.foe_fac;
    s_genm.waves = gm.waves; s_genm.per_wave = gm.per_wave; s_genm.foe_hp = gm.foe_hp;
    s_genm.foe_dmg = gm.foe_dmg; s_genm.foe_speed_pml = gm.foe_speed_pml; s_genm.ace = gm.ace;
    s_genm.reward_cr = gm.reward_cr; s_genm.kill_cr = gm.kill_cr; s_genm.rep_gain = gm.rep_gain;
    s_genm.enemy_rep_loss = gm.enemy_rep_loss; s_genm.req_flag = -1; s_genm.forbid_flag = -1;
    s_genm.once = 0; s_genm.set_flag = -1;
    return &s_genm;
}

// ============================ audio (chiptune + file override) ===============
static const char *sfx_name(int id)
{
    switch (id) {
        case SFX_MOVE: return "move";  case SFX_OK: return "ok";      case SFX_BACK: return "back";
        case SFX_BUY:  return "buy";   case SFX_DENY: return "deny";  case SFX_JUMP: return "jump";
        case SFX_EVENT:return "event"; case SFX_BEACON: return "beacon"; case SFX_TITLE: return "title";
        case SFX_WIN:  return "win";   case SFX_LOSE: return "lose";
        case SFX_LASER:return "laser"; case SFX_HIT:  return "hit";   case SFX_BOOM: return "boom";
        case SFX_LAUNCH:return "launch"; case SFX_LOCK: return "lock";
        case SFX_HULL: return "hull";  case SFX_SHIELD_DOWN: return "shielddown";
        case SFX_ALARM:return "alarm"; case SFX_PASS: return "pass";  default: return "x";
    }
}
static int build_voices(int id, notify_voice_t *v)
{
    switch (id) {
        case SFX_MOVE:  notify__voice(&v[0], 880, 0, 0.045f); v[0].amp = 0.7f; return 1;
        case SFX_OK:    notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 987.77f, 0.05f, 0.09f); return 2;
        case SFX_BACK:  notify__voice(&v[0], 659.25f, 0, 0.07f); notify__voice(&v[1], 440, 0.05f, 0.09f); return 2;
        case SFX_BUY:   notify__voice(&v[0], 1046.5f, 0, 0.06f); notify__voice(&v[1], 1568, 0.04f, 0.10f); return 2;
        case SFX_DENY:  notify__voice(&v[0], 196, 0, 0.16f); notify__voice(&v[1], 185, 0, 0.16f); v[0].amp = 0.8f; v[1].amp = 0.8f; return 2;
        case SFX_JUMP:  notify__voice(&v[0], 392, 0, 0.10f); notify__voice(&v[1], 523.25f, 0.08f, 0.10f);
                        notify__voice(&v[2], 659.25f, 0.16f, 0.10f); notify__voice(&v[3], 1046.5f, 0.24f, 0.22f); return 4;
        case SFX_EVENT: notify__voice(&v[0], 659.25f, 0, 0.10f); notify__voice(&v[1], 880, 0.09f, 0.14f); return 2;
        case SFX_BEACON:notify__voice(&v[0], 523.25f, 0, 0.70f); notify__voice(&v[1], 659.25f, 0.05f, 0.70f);
                        notify__voice(&v[2], 783.99f, 0.10f, 0.75f); notify__voice(&v[3], 1046.5f, 0.18f, 0.80f); return 4;
        case SFX_TITLE: notify__voice(&v[0], 523.25f, 0, 0.18f); notify__voice(&v[1], 659.25f, 0.16f, 0.18f);
                        notify__voice(&v[2], 783.99f, 0.32f, 0.20f); notify__voice(&v[3], 1046.5f, 0.48f, 0.50f); return 4;
        case SFX_WIN:   notify__voice(&v[0], 523.25f, 0, 0.16f); notify__voice(&v[1], 659.25f, 0.14f, 0.16f);
                        notify__voice(&v[2], 783.99f, 0.28f, 0.16f); notify__voice(&v[3], 1046.5f, 0.42f, 0.18f);
                        notify__voice(&v[4], 1318.5f, 0.56f, 0.60f); return 5;
        case SFX_LOSE:  notify__voice(&v[0], 392, 0, 0.30f); notify__voice(&v[1], 329.63f, 0.22f, 0.30f);
                        notify__voice(&v[2], 261.63f, 0.46f, 0.70f); return 3;
        case SFX_LASER: // twin "pew" with a downward chirp + attack tick + a low snap for weight
            notify__voice(&v[0], 2100, 0.000f, 0.018f); v[0].amp = 0.50f;
            notify__voice(&v[1], 1400, 0.014f, 0.018f); v[1].amp = 0.50f;
            notify__voice(&v[2],  900, 0.026f, 0.030f); v[2].amp = 0.45f;
            notify__voice(&v[3], 2040, 0.000f, 0.018f); v[3].amp = 0.40f;
            notify__voice(&v[4], 1360, 0.014f, 0.018f); v[4].amp = 0.40f;
            notify__voice(&v[5], 3200, 0.000f, 0.008f); v[5].amp = 0.30f;
            notify__voice(&v[6],  300, 0.000f, 0.022f); v[6].amp = 0.42f;   // low snap = body
            return 7;
        case SFX_HIT:   // bright metallic shield ping (inharmonic partials) + a low crunch for impact
            notify__voice(&v[0], 1760, 0.000f, 0.045f); v[0].amp = 0.55f;
            notify__voice(&v[1], 2637, 0.000f, 0.040f); v[1].amp = 0.40f;
            notify__voice(&v[2], 3520, 0.004f, 0.030f); v[2].amp = 0.30f;
            notify__voice(&v[3], 4699, 0.004f, 0.022f); v[3].amp = 0.22f;
            notify__voice(&v[4], 1245, 0.000f, 0.020f); v[4].amp = 0.30f;
            notify__voice(&v[5],  210, 0.000f, 0.030f); v[5].amp = 0.46f;   // low crunch = impact body
            return 6;
        case SFX_BOOM:  // big layered kill: low detuned rumble + dissonant crunch + decaying sparkle
            notify__voice(&v[0], 73.4f, 0.000f, 0.34f); v[0].amp = 1.00f;
            notify__voice(&v[1], 62.0f, 0.000f, 0.34f); v[1].amp = 0.95f;
            notify__voice(&v[2], 98.0f, 0.020f, 0.26f); v[2].amp = 0.70f;
            notify__voice(&v[3], 311.1f, 0.000f, 0.10f); v[3].amp = 0.55f;
            notify__voice(&v[4], 329.6f, 0.000f, 0.10f); v[4].amp = 0.55f;
            notify__voice(&v[5], 1900, 0.000f, 0.055f); v[5].amp = 0.45f;
            notify__voice(&v[6], 2700, 0.010f, 0.045f); v[6].amp = 0.35f;
            notify__voice(&v[7], 3700, 0.020f, 0.035f); v[7].amp = 0.25f;
            return 8;
        case SFX_LAUNCH:notify__voice(&v[0], 261.63f, 0, 0.10f); notify__voice(&v[1], 392, 0.08f, 0.10f);
                        notify__voice(&v[2], 523.25f, 0.16f, 0.10f); notify__voice(&v[3], 784, 0.24f, 0.18f); return 4;
        case SFX_LOCK:  // rising acquire
            notify__voice(&v[0], 1174.7f, 0.000f, 0.055f); v[0].amp = 0.55f;
            notify__voice(&v[1], 1567.98f, 0.050f, 0.070f); v[1].amp = 0.60f;
            notify__voice(&v[2], 3135.96f, 0.095f, 0.040f); v[2].amp = 0.30f;
            return 3;
        case SFX_HULL:  // dull low thud + clang (hull hit)
            notify__voice(&v[0], 110, 0.000f, 0.090f); v[0].amp = 0.95f;
            notify__voice(&v[1], 82.4f, 0.000f, 0.110f); v[1].amp = 0.90f;
            notify__voice(&v[2], 165, 0.000f, 0.045f); v[2].amp = 0.45f;
            notify__voice(&v[3], 55, 0.020f, 0.120f); v[3].amp = 0.60f;
            return 4;
        case SFX_SHIELD_DOWN: // descending power-loss into an ominous landing
            notify__voice(&v[0], 1245, 0.000f, 0.070f); v[0].amp = 0.60f;
            notify__voice(&v[1], 830, 0.060f, 0.080f); v[1].amp = 0.60f;
            notify__voice(&v[2], 554, 0.130f, 0.090f); v[2].amp = 0.60f;
            notify__voice(&v[3], 311, 0.210f, 0.160f); v[3].amp = 0.70f;
            return 4;
        case SFX_ALARM: // two-beat dissonant klaxon
            notify__voice(&v[0], 880, 0.000f, 0.090f); v[0].amp = 0.85f;
            notify__voice(&v[1], 622, 0.110f, 0.090f); v[1].amp = 0.85f;
            notify__voice(&v[2], 880, 0.230f, 0.090f); v[2].amp = 0.85f;
            notify__voice(&v[3], 622, 0.340f, 0.110f); v[3].amp = 0.85f;
            return 4;
        case SFX_PASS:  // fast down-glide whoosh
            notify__voice(&v[0], 1300, 0.000f, 0.040f); v[0].amp = 0.45f;
            notify__voice(&v[1], 700, 0.030f, 0.050f); v[1].amp = 0.40f;
            notify__voice(&v[2], 380, 0.065f, 0.060f); v[2].amp = 0.35f;
            return 3;
    }
    return 0;
}
static const char *sfx_path(int id)
{
    static char p[96];
    snprintf(p, sizeof p, DIR "/custom/%s.wav", sfx_name(id));   // player-supplied override?
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return p; }
    snprintf(p, sizeof p, DIR "/sfx/%s.v2.wav", sfx_name(id));    // cached synth (v2: bumped -> regen on first play)
    f = fopen(p, "rb");
    if (f) { fclose(f); return p; }
    notify_voice_t v[8];                                          // synth once, cache on SD
    int nv = build_voices(id, v);
    if (nv > 0 && notify_synth_voices_wav(v, nv, p, 12000) == 0) return p;
    return nullptr;
}
static void sfx(int id)
{
    if (!g_audio || id == SFX_NONE) return;
    // Big cues interrupt; small blips (incl. rapid laser/hit/lock) are dropped while busy, which
    // naturally rate-limits combat SFX so the SD WAV channel never thrashes.
    bool important = (id == SFX_JUMP || id == SFX_BEACON || id == SFX_TITLE || id == SFX_WIN ||
                      id == SFX_LOSE || id == SFX_LAUNCH || id == SFX_BOOM ||
                      id == SFX_ALARM || id == SFX_SHIELD_DOWN);
    if (!important && nucleo_audio_is_playing()) return;
    const char *p = sfx_path(id);
    if (!p) return;
    if (important) nucleo_audio_stop();
    nucleo_audio_play(p);
}

// ============================ starfield ======================================
#define NSTAR 84
static struct { uint8_t x, y, layer, tw; } star[NSTAR];
static void stars_init(void)
{
    uint32_t r = 0x9e3779b9u;
    for (int i = 0; i < NSTAR; i++) {
        r = r * 1664525u + 1013904223u;
        star[i].x = (uint8_t)((r >> 8) % 240);
        star[i].y = (uint8_t)((r >> 16) % 121);
        star[i].layer = (uint8_t)((r >> 5) % 3);
        star[i].tw = (uint8_t)((r >> 2) % 64);
    }
}
static void stars_draw(int ch)
{
    for (int i = 0; i < NSTAR; i++) {
        int L = star[i].layer;
        int x = (int)(star[i].x - s_scroll[L]);
        x %= 240; if (x < 0) x += 240;
        int y = star[i].y % (ch > 0 ? ch : 121);
        bool twk = ((s_anim + star[i].tw) & 31) < 3;
        uint16_t c;
        if (L == 0) c = twk ? COL_GREY : COL_DIM;
        else if (L == 1) c = COL_GREY;
        else c = twk ? COL_CYAN : COL_WHITE;
        if (L == 2) d.fillRect(x, y, 2, 2, c);
        else d.drawPixel(x, y, c);
    }
}

// ============================ text helpers ===================================
static void text_at(int x, int y, int size, uint16_t col, const char *s)
{
    d.setTextSize(size); d.setTextColor(col, COL_SPACE); d.setCursor(x, y); d.print(s);
}
static void center(int y, int size, uint16_t col, const char *s)
{
    int w = (int)strlen(s) * 6 * size;
    text_at((W - w) / 2, y, size, col, s);
}
static void mini_bar(int x, int y, int w, int h, int pct, uint16_t col)
{
    pct = clampi(pct, 0, 100);
    d.fillRoundRect(x, y, w, h, 1, rgb(30, 34, 52));
    if (pct > 0) d.fillRoundRect(x, y, w * pct / 100, h, 1, col);
}
// like draw_wrapped but stops after maxlines (keeps prose from spilling into UI below)
static int draw_wrapped_n(int x, int y, int maxw, int lineh, uint16_t col, const char *s, int maxlines)
{
    int cpl = maxw / 6; if (cpl < 1) cpl = 1; if (cpl > 60) cpl = 60;
    d.setTextSize(1); d.setTextColor(col, COL_SPACE);
    char line[64];
    int ln = 0;
    while (*s && ln < maxlines) {
        int n = 0, lastsp = -1;
        while (s[n] && n < cpl) { if (s[n] == ' ') lastsp = n; n++; }
        int take = n;
        if (s[n] && lastsp > 0) take = lastsp;
        if (take > 63) take = 63;
        memcpy(line, s, take); line[take] = 0;
        d.setCursor(x, y); d.print(line);
        s += take; while (*s == ' ') s++;
        y += lineh; ln++;
    }
    return y;
}

// ============================ watch-UI helpers ===============================
// Shrink font so `s` fits `maxw` px; never below size 1.
static int fit_size(const char *s, int maxw, int want)
{
    int len = (int)strlen(s); if (len < 1) len = 1;
    while (want > 1 && len * 6 * want > maxw) want--;
    return want;
}
// vertically center one line in band [y0, y0+h)
static void text_vc(int x, int y0, int h, int size, uint16_t col, const char *s)
{
    text_at(x, y0 + (h - 8 * size) / 2, size, col, s);
}
// right-aligned text whose RIGHT edge sits at xr, vertically centered in [y0,y0+h)
static void text_vr(int xr, int y0, int h, int size, uint16_t col, const char *s)
{
    int w = (int)strlen(s) * 6 * size;
    text_at(xr - w, y0 + (h - 8 * size) / 2, size, col, s);
}
// compact title band (y 0..HDR_H): big cyan title left, optional grey caption right
static void title_band(const char *title, const char *right)
{
    d.fillRect(0, 0, W, HDR_H, COL_PANEL);
    d.drawFastHLine(0, HDR_H, W, rgb(46, 60, 96));
    text_vc(MARGIN, 0, HDR_H, fit_size(title, 150, 2), COL_CYAN, title);
    if (right && right[0]) text_vr(W - MARGIN, 0, HDR_H, fit_size(right, 96, 1), COL_GREY, right);
}

// ---- reusable fisheye scrolling list ----------------------------------------
// The caller owns the (already-wrapped) selection index and supplies a free-function
// row painter that reads file-scope state. The engine owns the window math, the
// selection pill + accent rail, and the right-edge scroll thumb. No heap.
typedef void (*row_fn)(int idx, int bx, int by, int bw, int bh, int tier);
static void list_fisheye(int sel, int count, int top, int bot, row_fn render)
{
    if (count <= 0) return;
    sel = clampi(sel, 0, count - 1);
    int avail = bot - top;
    int around = (avail - ROW_H) / ROW_SM; if (around < 0) around = 0;
    int above = around / 2, below = around - above;
    int first = sel - above, last = sel + below;
    if (first < 0)        { last += -first; first = 0; }
    if (last > count - 1) { first -= (last - (count - 1)); last = count - 1; }
    if (first < 0) first = 0;

    int y = top;
    for (int i = first; i <= last; i++) {
        int h = (i == sel) ? ROW_H : ROW_SM;
        if (y + h > bot) break;
        if (i == sel) {
            int py = y + (ROW_H - PILL_H) / 2;
            d.fillRoundRect(MARGIN, py, CW, PILL_H, 6, COL_FOCUS);
            d.drawRoundRect(MARGIN, py, CW, PILL_H, 6, COL_FOCUS2);
            d.fillRect(MARGIN, py + 4, ACC_W, PILL_H - 8, COL_FOCUS2);
        }
        int tier = (i == sel) ? TIER_FOCUS : ((i == sel - 1 || i == sel + 1) ? TIER_NEAR : TIER_FAR);
        render(i, MARGIN, y, CW, h, tier);
        y += h;
    }
    // proportional scroll thumb on the right rim
    int win = last - first + 1;
    if (count > win) {
        d.fillRect(SCRL_X + 1, top, 1, avail, COL_TRACK);
        int th = avail * win / count; if (th < 6) th = 6;
        int denom = count - win; if (denom < 1) denom = 1;
        int ty = top + (avail - th) * first / denom;
        d.fillRoundRect(SCRL_X, ty, 3, th, 1, COL_FOCUS2);
    }
}
// row helpers: shared text origins inside a list band
static inline int row_x0(int bx) { return bx + ACC_W + 4; }       // 16: label origin
static inline int row_xr(int bx, int bw) { return bx + bw - 6; }  // 226: right edge
static inline uint16_t tier_col(int tier)
{
    return tier == TIER_FOCUS ? COL_WHITE : (tier == TIER_NEAR ? COL_GREY : COL_FADE);
}

// ============================ HUD ============================================
static void draw_hud(void)
{
    d.fillRect(0, 0, W, 13, COL_SPACE);
    d.drawFastHLine(0, 13, W, rgb(40, 50, 80));
    char b[24];
    snprintf(b, sizeof b, "%d cr", g.credits);
    text_at(4, 3, 1, COL_AMBER, b);
    // fuel
    int fx = 92;
    text_at(fx, 3, 1, COL_GREY, "F");
    int fpct = g.fuel_max ? g.fuel * 100 / g.fuel_max : 0;
    mini_bar(fx + 8, 4, 22, 6, fpct, fpct < 25 ? COL_RED : COL_CYAN);
    snprintf(b, sizeof b, "%d", g.fuel); text_at(fx + 32, 3, 1, COL_GREY, b);
    // hull
    int hx = 142;
    text_at(hx, 3, 1, COL_GREY, "H");
    int hpct = g.hull_max ? g.hull * 100 / g.hull_max : 0;
    mini_bar(hx + 8, 4, 22, 6, hpct, hpct < 30 ? COL_RED : COL_GREEN);
    // cargo
    snprintf(b, sizeof b, "C %d/%d", cargo_used(), g.cargo_max);
    text_at(192, 3, 1, COL_GREY, b);
}

// ============================ procedural planet ==============================
static uint16_t faction_col(int f)
{
    switch (f) {
        case F_GILDA:   return rgb(70, 120, 200);
        case F_CUSTODI: return rgb(40, 170, 150);
        case F_RELITTI: return rgb(176, 96, 52);
        default:        return COL_PURPLE;        // Eco
    }
}
// Per-wave colour: enemies + backdrop shift hue every wave so each one reads distinct (arcade).
static const uint16_t WAVE_PAL[6] = {
    rgb(80, 140, 220), rgb(40, 180, 150), rgb(210, 110, 60),
    rgb(170, 120, 235), rgb(225, 90, 130), rgb(120, 195, 90),
};
static inline uint16_t wave_col(void) { return s_wave_tint ? s_wave_tint : COL_CYAN; }
static uint16_t pu_col(int k)
{
    switch (k) { case PU_SHIELD: return COL_CYAN; case PU_REPAIR: return COL_GREEN;
                 case PU_MISSILE: return COL_AMBER; default: return COL_PURPLE; }   // PU_RAPID
}
// Per-kind identity tint blended into the wave colour so each foe class reads at a glance:
// scout = pale ice, heavy = hot orange, ace = gold, fighter = neutral (wave colour only).
static uint16_t kind_hue(int kind)
{
    switch (kind) {
        case FOE_SCOUT: return rgb(206, 228, 245);
        case FOE_HEAVY: return rgb(236, 150, 70);
        case FOE_ACE:   return COL_AMBER;
        default:        return 0;          // FOE_FIGHTER: no tint
    }
}
static void draw_planet(int cx, int cy, int r, int sys)
{
    uint16_t base = faction_col(SYSTEMS[sys].faction);
    int br = (base >> 11) & 31, bg = (base >> 5) & 63, bb = base & 31;
    d.fillCircle(cx, cy, r, base);
    for (int dy = -r + 2; dy < r - 1; dy += 3) {                  // banding
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        int shade = ((dy + r) / 3) & 1 ? -6 : 6;
        uint16_t cc = rgb((br + shade) * 8, (bg + shade) * 4, (bb + shade) * 8);
        d.drawFastHLine(cx - hw, cy + dy, 2 * hw, cc);
    }
    d.fillCircle(cx - r / 3, cy - r / 3, r / 5, rgb(255, 255, 255)); // specular highlight
    d.drawCircle(cx, cy, r, rgb(br * 8 + 40, bg * 4 + 40, bb * 8 + 40));
    if (SYSTEMS[sys].beacon) {                                    // orbital beacon ring
        bool lit = (g.beacon_lit & bit(sys)) != 0;
        uint16_t rc = lit ? COL_CYAN : rgb(120, 50, 50);
        int pr = r + 6 + (lit ? (int)((s_anim / 4) % 3) : 0);
        d.drawCircle(cx, cy, pr, rc);
        d.drawCircle(cx, cy, pr + 1, rc);
        d.fillCircle(cx, cy - pr, 2, lit ? COL_WHITE : rc);
    }
}

// little vector ship (the Lucciola), pointing right
static void draw_ship(int cx, int cy, int s, uint16_t col)
{
    d.fillTriangle(cx + 2 * s, cy, cx - 2 * s, cy - s, cx - 2 * s, cy + s, col);
    d.fillTriangle(cx - 2 * s, cy - s, cx - 2 * s, cy + s, cx - 3 * s, cy, rgb(40, 50, 80));
    d.fillCircle(cx, cy, s / 2 + 1, COL_CYAN);
}

// ============================ navigation between screens =====================
static void set_hint_for(int screen);
static void go(int screen) { s_screen = screen; set_hint_for(screen); req(); }

static void start_cine(int id)
{
    s_cine = id; s_cine_t0 = s_now; s_screen = ST_CINE;
    if (id == CINE_INTRO) sfx(SFX_TITLE);
    else if (id == CINE_JUMP) sfx(SFX_JUMP);
    else if (id == CINE_BEACON) sfx(SFX_BEACON);
    else if (id == CINE_WIN) sfx(SFX_WIN);
    else if (id == CINE_LOSE) sfx(SFX_LOSE);
    else if (id == CINE_SECTOR) sfx(SFX_WIN);
    set_hint_for(ST_CINE); req();
}

static void new_game(void)
{
    memset(&g, 0, sizeof g);
    g.credits = 600; g.fuel = g.fuel_max = 8; g.hull = g.hull_max = 100;
    g.cargo_max = 20; g.jump_range = 64; g.sensors = 0; g.epoch = 1;   // 64 keeps procedural sectors connected (no beacon soft-lock)
    g.weapon = 0; g.shield_max = 40; g.missions_done = 0; g.kills = 0;
    g.seed = esp_random_local(); g.sector = 0; g.beacon_lit = 0;   // pick the universe seed once
    regen_sector(); g.sys = entry_slot(0);
    s_ingame = true; s_target = (g.sys + 1) % NSYS; s_hubsel = 0;
    start_cine(CINE_INTRO);
}
static void continue_game(void)
{
    if (!save_read(&g)) { new_game(); return; }
    regen_sector();                                               // rebuild the saved sector
    if (g.sys < 0 || g.sys >= NSYS) g.sys = entry_slot(g.sector);
    s_ingame = true; s_target = (g.sys + 1) % NSYS; s_hubsel = 0;
    go(ST_SYSTEM);
}

// ============================ events =========================================
static bool ev_eligible(int i, const Sys *s)
{
    const Event *e = &EVENTS[i];
    if (e->weight <= 0 || e->story) return false;
    if (e->at_sys >= 0 && e->at_sys != g.sys) return false;
    if (e->need_faction >= 0 && e->need_faction != s->faction) return false;
    if (e->req_flag >= 0 && !flag(e->req_flag)) return false;
    if (e->forbid_flag >= 0 && flag(e->forbid_flag)) return false;
    return true;
}
static int pick_random_event(const Sys *s)
{
    int total = 0;
    for (int i = 0; i < NEVENTS; i++) if (ev_eligible(i, s)) total += EVENTS[i].weight;
    if (total <= 0) return -1;
    int r = rnd(total), acc = 0;
    for (int i = 0; i < NEVENTS; i++) if (ev_eligible(i, s)) { acc += EVENTS[i].weight; if (r < acc) return i; }
    return -1;
}
static void start_event(int id) { s_ev = id; s_evsel = 0; s_screen = ST_EVENT; sfx(SFX_EVENT); set_hint_for(ST_EVENT); req(); }

static bool choice_affordable(const Choice *c)
{
    const Effect *e = &c->eff;
    if (e->dcred < 0 && g.credits < -e->dcred) return false;
    if (e->good >= 0 && e->qty < 0 && e->qty != -99 && g.cargo[e->good] < -e->qty) return false;
    return true;
}
static void arrive(void)
{
    const Sys *s = &SYSTEMS[g.sys];
    if (s->beacon && !(g.beacon_lit & bit(g.sys))) { start_event(EV_FARO); save_write(); return; }
    if (s->faction == F_ECO && !flag(FL_MET_ECHO))  { start_event(EV_ECO); save_write(); return; }
    if (s->faction == F_GILDA && g.cargo[G_CONTRA] > 0 && rnd(100) < 70) { start_event(EV_DOGANA); save_write(); return; }
    // hostile interception: a real dogfight on arrival (sensors lower the odds; raider space is worse)
    {
        int amb = 14 - g.sensors * 3; if (amb < 5) amb = 5;
        if (s->faction == F_RELITTI || s->faction == F_ECO) amb += 8;
        if (rnd(100) < amb) { save_write(); combat_begin_ambush(); return; }
    }
    int chance = 52 - g.sensors * 6; if (chance < 20) chance = 20;
    if (rnd(100) < chance) { int e = pick_random_event(s); if (e >= 0) { start_event(e); save_write(); return; } }
    go(ST_SYSTEM); save_write();
}
// Lighting every Beacon in the sector opens the next, harder sector — endless, no hard "win".
static void advance_sector(void)
{
    g.sector++; g.beacon_lit = 0; regen_sector();
    g.sys = entry_slot(g.sector); s_target = (g.sys + 1) % NSYS;
    save_write(); start_cine(CINE_SECTOR);
}
static void after_beacon(void)
{
    if (beacons_lit() >= beacons_total()) advance_sector();
    else go(ST_SYSTEM);
}
static void apply_choice(void)
{
    const Event *ev = &EVENTS[s_ev];
    const Choice *c = &ev->ch[s_evsel];
    if (!choice_affordable(c)) { sfx(SFX_DENY); return; }
    const Effect *e = &c->eff;
    g.credits = clampi(g.credits + e->dcred, 0, 9999999);
    g.fuel    = clampi(g.fuel + e->dfuel, 0, g.fuel_max);
    g.hull    = clampi(g.hull + e->dhull, 0, g.hull_max);
    if (e->rep_fac >= 0) g.rep[e->rep_fac] = clampi(g.rep[e->rep_fac] + e->drep, -100, 100);
    if (e->flag >= 0) g.flags |= bit(e->flag);
    if (e->good >= 0) {
        if (e->qty == -99) g.cargo[e->good] = 0;
        else if (e->qty < 0) g.cargo[e->good] = clampi(g.cargo[e->good] + e->qty, 0, 9999);
        else { int room = g.cargo_max - cargo_used(); int q = e->qty; if (q > room) q = room; if (q > 0) g.cargo[e->good] += q; }
    }
    sfx(e->sfx);
    if (g.hull <= 0) { start_cine(CINE_LOSE); return; }
    if (e->act == ACT_RELIGHT) { g.beacon_lit |= bit(g.sys); save_write(); start_cine(CINE_BEACON); return; }
    if (e->next >= 0) { start_event(e->next); return; }
    go(ST_SYSTEM); save_write();
}

// ============================ jump ===========================================
static void do_jump(void)
{
    int t = s_target;
    if (t < 0 || t == g.sys) { sfx(SFX_DENY); return; }
    float dd = sys_dist(g.sys, t);
    if (dd > g.jump_range) { snprintf(s_status, sizeof s_status, "%s", tx("Fuori portata", "Out of range")); sfx(SFX_DENY); req(); return; }
    int cost = jump_cost(dd);
    if (g.fuel < cost) { snprintf(s_status, sizeof s_status, "%s", tx("Celle insufficienti", "Not enough cells")); sfx(SFX_DENY); req(); return; }
    g.fuel -= cost; g.sys = t; g.epoch++;
    s_status[0] = 0;
    start_cine(CINE_JUMP);
}

// ============================ cinematics =====================================
static int cine_dur(int id)
{
    switch (id) { case CINE_INTRO: return 7200; case CINE_JUMP: return 1300;
                  case CINE_BEACON: return 1700; case CINE_WIN: return 6500; case CINE_LOSE: return 4200;
                  case CINE_SECTOR: return 3600; default: return 1200; }
}
static void cine_end(void)
{
    switch (s_cine) {
        case CINE_INTRO:  g.flags |= bit(FL_INTRO); go(ST_SYSTEM); save_write(); break;
        case CINE_JUMP:   arrive(); break;
        case CINE_BEACON: after_beacon(); break;
        case CINE_WIN:    go(ST_TITLE); s_ingame = false; break;
        case CINE_LOSE:   save_wipe(); s_ingame = false; go(ST_TITLE); break;
        case CINE_SECTOR: go(ST_SYSTEM); save_write(); break;
        default:          go(ST_SYSTEM); break;
    }
}

static void draw_warp(int ch, float k)   // k in 0..1 intensity
{
    int cx = W / 2, cy = ch / 2;
    int bloom = (int)(2 + k * 8);                                  // wormhole throat: a soft central bloom
    for (int r = bloom; r >= 1; r--) d.fillCircle(cx, cy, r, shade(COL_CYAN, bloom + 2 - r, bloom + 2));
    for (int i = 0; i < NSTAR; i++) {
        float ang = (float)(star[i].tw) * 0.0982f + i;
        float reach = 6 + k * 130;
        float d0 = 6 + (float)((star[i].x + s_anim) % 40);
        float ca = cosf(ang), sa = sinf(ang);
        int x0 = cx + (int)(ca * d0), y0 = cy + (int)(sa * d0);
        int x1 = cx + (int)(ca * (d0 + reach)), y1 = cy + (int)(sa * (d0 + reach));
        uint16_t c = (i & 3) ? (k > 0.7f ? COL_WHITE : COL_CYAN) : COL_WHITE;
        d.drawLine(x0, y0, x1, y1, c);
        if (k > 0.6f && (i & 1)) d.drawLine(x0, y0 + 1, x1, y1 + 1, shade(c, 2, 5));   // thicken the fast streaks
    }
}
static void draw_cine(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    int64_t el = s_now - s_cine_t0;
    float p = (float)el / cine_dur(s_cine);
    if (p > 1) p = 1;

    if (s_cine == CINE_INTRO) {
        stars_draw(ch);
        draw_ship(40 + (int)(p * 30), ch / 2 - 24, 4, COL_AMBER);
        if (p < 0.30f) {
            uint16_t c = rgb(60 + (int)(p * 600), 140 + (int)(p * 380), 255);
            center(ch / 2 - 8, 2, c, "COSTELLAZIONI");        // size 2 = 156px (size 3 overflowed)
        } else {
            center(20, 2, COL_CYAN, "COSTELLAZIONI");
            int shown = (int)((p - 0.30f) / 0.70f * NINTRO) + 1;
            if (shown > NINTRO) shown = NINTRO;
            int y = 50;
            for (int i = 0; i < shown; i++) { center(y, 1, i == shown - 1 ? COL_WHITE : COL_GREY, lp(INTRO_LINES[i])); y += 12; }
        }
        center(ch - 12, 1, COL_DIM, tx("- premi un tasto -", "- press any key -"));
    } else if (s_cine == CINE_JUMP) {
        draw_warp(ch, p < 0.8f ? p / 0.8f : 1.0f);
        if (p > 0.86f) d.fillRect(0, 0, W, ch, rgb(220, 240, 255));   // arrival flash
        else center(ch - 14, 1, COL_DIM, tx("salto iperspaziale", "hyperspace jump"));
    } else if (s_cine == CINE_BEACON) {
        stars_draw(ch);
        int cx = W / 2, cy = ch / 2 + 6;
        int rr = (int)(p * 70);
        for (int k = 0; k < 3; k++) d.drawCircle(cx, cy, rr - k * 6, k == 0 ? COL_WHITE : COL_CYAN);  // expanding burst
        d.fillRect(cx - 3, cy - 26, 6, 30, rgb(120, 130, 160));       // tower
        d.fillCircle(cx, cy - 28, 4 + (int)(p * 4), p > 0.2f ? COL_CYAN : COL_DIM);
        center(18, 2, COL_CYAN, tx("FARO ACCESO", "BEACON LIT"));
        char b[28]; snprintf(b, sizeof b, "%d / %d", beacons_lit(), beacons_total());
        center(ch - 16, 1, COL_AMBER, b);
    } else if (s_cine == CINE_WIN) {
        stars_draw(ch);
        center(18, 3, COL_CYAN, tx("VITTORIA", "VICTORY"));
        int shown = (int)(p * (NWIN + 1)); int y = 56;
        for (int i = 0; i < NWIN && i < shown; i++) { center(y, 1, COL_WHITE, lp(WIN_LINES[i])); y += 14; }
    } else if (s_cine == CINE_SECTOR) {
        draw_warp(ch, p < 0.6f ? p / 0.6f : 1.0f);            // hyperspace surge into the new sector
        center(16, 2, COL_CYAN, tx("SETTORE RIPULITO", "SECTOR CLEARED"));
        char b[28]; snprintf(b, sizeof b, "%s %u", tx("SETTORE", "SECTOR"), (unsigned)g.sector);
        center(ch / 2 - 8, 3, COL_AMBER, b);
        center(ch - 16, 1, COL_DIM, tx("Piu' profondo, piu' pericoloso", "Deeper, deadlier"));
    } else { // LOSE
        stars_draw(ch);
        center(ch / 2 - 18, 3, COL_RED, tx("FINE", "GAME OVER"));
        int shown = (int)(p * (NLOSE + 1)); int y = ch / 2 + 14;
        for (int i = 0; i < NLOSE && i < shown; i++) { center(y, 1, COL_GREY, lp(LOSE_LINES[i])); y += 13; }
    }
    if (el >= cine_dur(s_cine)) cine_end();
}

// ============================ action combat (first-person 3D) =================
// You ride the cockpit looking down +ez. Steer the reticle with the arrows; ENTER fires twin
// lasers that converge on the crosshair (hitscan on the locked fighter). Enemies fly out of the
// vanishing point and grow as they close; a 3D starfield streaks past. Esc disengages.
static inline int pf_top(void) { return 14; }   // playfield top (under the combat HUD band)
static float s_regen_acc;                        // sub-unit shield-regen accumulator

// perspective: world (ex,ey,ez) -> screen (sx,sy); sc multiplies a world radius into px.
static inline bool project(float ex, float ey, float ez, float *sx, float *sy, float *sc)
{
    if (ez < ZNEAR) return false;
    float inv = FOCAL / ez;
    *sx = CX + ex * inv;
    *sy = s_cy + ey * inv;
    *sc = inv;
    return true;
}
static void respawn_warp(int i)
{
    s_warp[i].ex = (float)(rnd(401) - 200);
    s_warp[i].ey = (float)(rnd(401) - 200);
    s_warp[i].ez = ZNEAR + (float)rnd((int)(ZFAR - ZNEAR));
}
static void spawn_tracer(Foe *f, bool huntward)
{
    for (int b = 0; b < NBOLT; b++) if (!s_bolt[b].on) {
        Bolt *bo = &s_bolt[b];
        bo->on = 1; bo->foe = 1; bo->aimward = huntward ? 1 : 0;
        bo->ex = f->ex; bo->ey = f->ey; bo->ez = f->ez;
        bo->tx = (huntward ? s_ward_x : 0.0f) + (float)(rnd(31) - 15);   // slight spread
        bo->ty = (float)(rnd(31) - 15);
        bo->vz = -(f->ez / 0.9f);                                        // reaches the cockpit in ~0.9s
        bo->life = 1000;
        return;
    }
}
// scale an RGB565 colour by num/den (cheap shading for art + particles)
static inline uint16_t shade(uint16_t c, int num, int den)
{
    int r = ((c >> 11) & 31) * num / den, g = ((c >> 5) & 63) * num / den, b = (c & 31) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
// blend a->b by t (0..255); cheap RGB565 lerp for hit-flash / damage tint
static inline uint16_t cmix(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (br - ar) * t / 255) << 11) | ((ag + (bg - ag) * t / 255) << 5) | (ab + (bb - ab) * t / 255));
}
static Part *part_alloc(void)
{
    for (int k = 0; k < NPART; k++) { int i = (s_part_rr + k) % NPART;
        if (!s_part[i].on) { s_part_rr = (i + 1) % NPART; return &s_part[i]; } }
    Part *p = &s_part[s_part_rr]; s_part_rr = (s_part_rr + 1) % NPART; return p;   // steal oldest
}
// explosion at a foe's WORLD point; sc = its projection scale (bigger = closer = fiercer)
static void spawn_boom(float ex, float ey, float ez, float sc, uint16_t col, bool big)
{
    int n = (int)(10 + 12 * sc); if (n > (big ? 22 : 16)) n = big ? 22 : 16;
    float spd = 50.0f + 120.0f * sc;
    for (int i = 0; i < n; i++) {
        Part *p = part_alloc();
        float a = (float)rnd(628) * 0.01f, s = spd * (0.4f + 0.6f * (rnd(100) * 0.01f));
        p->on = 1; p->kind = PK_DEBRIS; p->ex = ex; p->ey = ey; p->ez = ez;
        p->vx = cosf(a) * s; p->vy = sinf(a) * s; p->vz = (float)(rnd(90) - 30);
        p->life = p->life0 = (int16_t)(320 + rnd(280));
        p->col = (i & 1) ? col : ((i & 2) ? COL_AMBER : COL_WHITE);
    }
    for (int s = 0; s < NSHK; s++) if (!s_shk[s].on) {
        float sx, sy, scc; if (!project(ex, ey, ez, &sx, &sy, &scc)) break;
        s_shk[s].on = 1; s_shk[s].cx_ = sx; s_shk[s].cy_ = sy;
        s_shk[s].life = s_shk[s].life0 = (int16_t)(big ? 420 : 320); s_shk[s].col = col;
        break;
    }
    int ns = big ? 9 : 5;                                  // white-hot core spark burst (extra punch)
    for (int i = 0; i < ns; i++) {
        Part *p = part_alloc(); float a = (float)rnd(628) * 0.01f, s = 60.0f + rnd(90);
        p->on = 1; p->kind = PK_SPARK; p->ex = ex; p->ey = ey; p->ez = ez;
        p->vx = cosf(a) * s; p->vy = sinf(a) * s; p->vz = (float)(rnd(90) - 30);
        p->life = p->life0 = (int16_t)(90 + rnd(120)); p->col = (i & 1) ? COL_WHITE : COL_AMBER;
    }
    float fsx, fsy, fsc;
    if (project(ex, ey, ez, &fsx, &fsy, &fsc)) {
        s_flash_until = s_now + (big ? 110 : 70); s_flash_x = fsx; s_flash_y = fsy;
        int r0 = (int)(7 + 10 * sc); if (r0 > 16) r0 = 16;
        s_flash_r0 = r0;
    }
}
static void spawn_sparks(float ex, float ey, float ez, uint16_t col)
{
    int n = 4 + rnd(3);
    for (int i = 0; i < n; i++) {
        Part *p = part_alloc(); float a = (float)rnd(628) * 0.01f, s = 40.0f + rnd(60);
        p->on = 1; p->kind = PK_SPARK; p->ex = ex; p->ey = ey; p->ez = ez;
        p->vx = cosf(a) * s; p->vy = sinf(a) * s; p->vz = (float)(rnd(60) - 20);
        p->life = p->life0 = (int16_t)(120 + rnd(120)); p->col = col;
    }
}
static void spawn_streak(float sx, float sy)        // ram pass-through: a bright slash past the canopy
{
    Part *p = part_alloc();
    p->on = 1; p->kind = PK_STREAK; p->ez = 0;
    p->ex = sx; p->ey = sy;                          // ex/ey reused as screen coords for streaks
    int side = (sx < CX) ? -1 : 1;
    p->vx = side * 420.0f; p->vy = 260.0f;
    p->life = p->life0 = 160; p->col = COL_WHITE;
}
// per-class behaviour tuning (scout = fast/erratic/weak, heavy = slow/tanky/aggressive)
static inline float kind_speed(int k) { switch (k) { case FOE_SCOUT: return 1.55f; case FOE_HEAVY: return 0.72f; case FOE_ACE: return 1.30f; default: return 1.0f; } }
static inline float kind_wrate(int k) { switch (k) { case FOE_SCOUT: return 2.9f;  case FOE_HEAVY: return 0.9f;  case FOE_ACE: return 2.5f;  default: return 1.6f; } }
static inline float kind_wamp(int k)  { switch (k) { case FOE_SCOUT: return 1.7f;  case FOE_HEAVY: return 0.5f;  default: return 1.0f; } }
static inline int   kind_firems(int k){ switch (k) { case FOE_SCOUT: return 1700;  case FOE_HEAVY: return 650;   case FOE_ACE: return 700;   default: return 1100; } }
static inline float kind_rscale(int k){ switch (k) { case FOE_SCOUT: return 0.75f; case FOE_HEAVY: return 1.4f;  default: return 1.0f; } }

// detailed wireframe TIE-style fighter; size r grows as it nears, banks/shears with b
static void draw_tie(int x, int y, int r, int b, int kind, uint16_t col)
{
    if (r <= 3) {                                       // far: glint + engine spark
        d.fillRect(x - 1, y - 1, 2, 2, col);
        d.drawPixel(x, y + 1, kind == FOE_ACE ? COL_AMBER : COL_CYAN); return;
    }
    // The per-wave colour `col` drives the hull; the class only adds a distinguishing accent.
    uint16_t panel = shade(col, 2, 5), edge = col;
    uint16_t accent = (kind == FOE_ACE) ? COL_AMBER : (kind == FOE_HEAVY) ? rgb(230, 150, 60)
                    : (kind == FOE_SCOUT) ? COL_WHITE : col;
    int bk = (b * r) / 2;                               // horizontal bank shift
    int sh = clampi(b / 2, -3, 3);                      // vertical wing shear (parallax)
    int pw = 3 + r / 6;
    int lx = x - r - pw, rx = x + r;
    d.fillTriangle(lx, y - r + sh, lx + pw, y - r + sh, lx + pw / 2, y - r - 2 + sh, panel);
    d.fillRect    (lx, y - r + sh, pw, 2 * r, panel);
    d.fillTriangle(lx, y + r + sh, lx + pw, y + r + sh, lx + pw / 2, y + r + 2 + sh, panel);
    d.drawRect    (lx, y - r + sh, pw, 2 * r, edge);
    d.drawLine    (lx + pw / 2, y - r - 2 + sh, lx + pw / 2, y + r + 2 + sh, edge);
    d.fillTriangle(rx, y - r - sh, rx + pw, y - r - sh, rx + pw / 2, y - r - 2 - sh, panel);
    d.fillRect    (rx, y - r - sh, pw, 2 * r, panel);
    d.fillTriangle(rx, y + r - sh, rx + pw, y + r - sh, rx + pw / 2, y + r + 2 - sh, panel);
    d.drawRect    (rx, y - r - sh, pw, 2 * r, edge);
    d.drawLine    (rx + pw / 2, y - r - 2 - sh, rx + pw / 2, y + r + 2 - sh, edge);
    if (r >= 10) { d.drawLine(lx, y + sh, lx + pw, y + sh, shade(edge, 3, 5));
                   d.drawLine(rx, y - sh, rx + pw, y - sh, shade(edge, 3, 5)); }
    d.drawLine(x - r, y, x + r, y, edge);              // spar
    if (r >= 7) d.drawLine(x - r, y - 1, x + r, y - 1, shade(edge, 2, 5));
    int br = (2 * r) / 3, fr = br > 22 ? 22 : br, cxp = x + bk / 3;     // pod fill radius capped (perf)
    d.fillCircle(cxp, y, fr, rgb(24, 28, 44));
    d.drawCircle(cxp, y, br, edge);
    if (r >= 6) { d.fillCircle(cxp, y, fr / 2, shade(edge, 3, 5));
                  d.drawPixel(cxp - br / 3, y - br / 3, COL_WHITE); }   // window glint
    if (r >= 8) { d.drawLine(cxp - r / 2, y, cxp + r / 2, y, edge);
                  d.drawLine(cxp, y - r / 2, cxp, y + r / 2, edge); }
    int gx = cxp - bk / 2, gy = y + br / 2 + 1;                          // engine glow
    uint16_t glow = accent;
    d.fillRect(gx - 1, gy, 2, 2, glow);
    if (r >= 6) { d.drawPixel(gx, gy + 2, shade(glow, 3, 5)); d.drawPixel(gx - bk / 4, gy + 3, COL_DIM); }
    if (kind == FOE_ACE) {
        d.drawCircle(x, y, br + 2, accent);
        d.drawLine(lx, y - r + sh, lx - 2, y - r - 2 + sh, accent);
        d.drawLine(rx + pw, y - r - sh, rx + pw + 2, y - r - 2 - sh, accent);
        d.fillRect(gx + 2, gy, 2, 2, COL_RED);
    } else if (kind == FOE_HEAVY && r >= 5) {              // armoured: double hull ring
        d.drawCircle(cxp, y, br + 2, edge);
    } else if (kind == FOE_SCOUT && r >= 4) {              // sleek: forward nose spike
        d.drawLine(x, y, x, y - r - 3, edge);
    }
}
// ---- 3D ship MODEL LIBRARY (the NEAR LOD tier; below ~7px we fall back to the wireframe TIE) --------
// Four distinct silhouettes so the wing reads at a glance: a sleek SCOUT dart, the balanced FIGHTER, a
// wide blocky HEAVY cruiser, and an ornate ACE with swept wings + dorsal fin. Each is `const` -> it
// lives in FLASH, costing ZERO runtime RAM (the whole point on a no-PSRAM board). The mesh banks/yaws
// with the foe's bank so every fighter tumbles as a solid shaded object instead of a flat decal.
static const fx3d::V3 MDL_FIGHTER_V[8] = {
    { 0.00f,  0.00f, -1.35f}, { 0.00f, -0.42f,  0.30f}, { 0.00f,  0.42f,  0.30f}, {-0.50f,  0.00f,  0.30f},
    { 0.50f,  0.00f,  0.30f}, { 0.00f,  0.05f,  1.05f}, {-1.30f,  0.06f,  0.55f}, { 1.30f,  0.06f,  0.55f},
};
static const fx3d::Tri MDL_FIGHTER_T[12] = {
    {0,1,4},{0,4,2},{0,2,3},{0,3,1}, {5,4,1},{5,2,4},{5,3,2},{5,1,3}, {3,6,2},{3,1,6}, {4,2,7},{4,7,1},
};
static const fx3d::Model MDL_FIGHTER = { MDL_FIGHTER_V, 8, MDL_FIGHTER_T, 12 };

static const fx3d::V3 MDL_SCOUT_V[8] = {       // long sleek dart, thin fins
    { 0.00f,  0.00f, -1.70f}, { 0.00f, -0.28f,  0.40f}, { 0.00f,  0.28f,  0.40f}, {-0.32f,  0.00f,  0.40f},
    { 0.32f,  0.00f,  0.40f}, { 0.00f,  0.00f,  0.95f}, {-0.95f,  0.02f,  0.70f}, { 0.95f,  0.02f,  0.70f},
};
static const fx3d::Tri MDL_SCOUT_T[10] = {
    {0,1,4},{0,4,2},{0,2,3},{0,3,1}, {5,4,1},{5,2,4},{5,3,2},{5,1,3}, {3,6,5},{4,5,7},
};
static const fx3d::Model MDL_SCOUT = { MDL_SCOUT_V, 8, MDL_SCOUT_T, 10 };

static const fx3d::V3 MDL_HEAVY_V[8] = {       // wide blocky cruiser, big wings
    { 0.00f, -0.05f, -1.15f}, {-0.75f, -0.50f,  0.15f}, { 0.75f, -0.50f,  0.15f}, {-0.75f,  0.50f,  0.15f},
    { 0.75f,  0.50f,  0.15f}, { 0.00f,  0.00f,  1.00f}, {-1.55f,  0.12f,  0.50f}, { 1.55f,  0.12f,  0.50f},
};
static const fx3d::Tri MDL_HEAVY_T[10] = {
    {0,1,2},{0,2,4},{0,4,3},{0,3,1}, {5,2,1},{5,4,2},{5,3,4},{5,1,3}, {1,6,3},{2,4,7},
};
static const fx3d::Model MDL_HEAVY = { MDL_HEAVY_V, 8, MDL_HEAVY_T, 10 };

static const fx3d::V3 MDL_ACE_V[9] = {         // swept wings + dorsal fin
    { 0.00f,  0.00f, -1.50f}, { 0.00f, -0.45f,  0.30f}, { 0.00f,  0.42f,  0.30f}, {-0.50f,  0.00f,  0.30f},
    { 0.50f,  0.00f,  0.30f}, { 0.00f,  0.05f,  1.15f}, {-1.30f,  0.12f,  0.95f}, { 1.30f,  0.12f,  0.95f},
    { 0.00f, -0.95f,  0.75f},
};
static const fx3d::Tri MDL_ACE_T[12] = {
    {0,1,4},{0,4,2},{0,2,3},{0,3,1}, {5,4,1},{5,2,4},{5,3,2},{5,1,3}, {3,6,2},{4,2,7}, {1,8,5},{1,0,8},
};
static const fx3d::Model MDL_ACE = { MDL_ACE_V, 9, MDL_ACE_T, 12 };

static const fx3d::Model *ship_model(int kind)
{
    switch (kind) {
        case FOE_SCOUT: return &MDL_SCOUT;
        case FOE_HEAVY: return &MDL_HEAVY;
        case FOE_ACE:   return &MDL_ACE;
        default:        return &MDL_FIGHTER;     // FOE_FIGHTER
    }
}
static void draw_ship3d(int x, int y, int r, float bank, int kind, uint16_t col)
{
    if (r < 7) { draw_tie(x, y, r, (int)(bank * 2.0f), kind, col); return; }   // far LOD: keep the wireframe
    float sc  = (float)r / 1.25f;
    float bk  = fmaxf(-0.9f, fminf(0.9f, bank * 0.18f));
    float yaw = bank * 0.32f;                                // a banking turn reads as a touch of yaw
    int fl = r / 2 + (int)(s_anim & 1);                      // thruster flame behind the hull (flickers)
    uint16_t flame = kind == FOE_SCOUT ? COL_CYAN : kind == FOE_HEAVY ? rgb(236, 150, 70) : COL_AMBER;
    d.fillTriangle(x - r / 4, y + r / 2, x + r / 4, y + r / 2, x, y + r / 2 + fl, flame);
    d.fillTriangle(x - r / 6, y + r / 2, x + r / 6, y + r / 2, x, y + r / 2 + fl * 2 / 3, COL_WHITE);
    fx3d::draw_model(*ship_model(kind), (float)x, (float)y, sc, yaw, bk, col);
    if (kind == FOE_ACE)        d.drawCircle(x, y, r + 2, COL_AMBER);          // ace halo
    else if (kind == FOE_HEAVY) d.drawCircle(x, y, (r * 5) / 6, shade(col, 3, 5));
}
static void draw_target_box(int x, int y, int r)                        // green corner brackets (pulsing)
{
    uint16_t gc = COL_GREEN; int L = clampi(r / 2, 4, 10);
    r += (int)((s_anim >> 2) & 1);                                       // breathe the bracket 1px (lock juice)
    int x0 = x - r, x1 = x + r, y0 = y - r, y1 = y + r;
    d.drawFastHLine(x0, y0, L, gc);     d.drawFastVLine(x0, y0, L, gc);
    d.drawFastHLine(x1 - L, y0, L, gc); d.drawFastVLine(x1, y0, L, gc);
    d.drawFastHLine(x0, y1, L, gc);     d.drawFastVLine(x0, y1 - L, L, gc);
    d.drawFastHLine(x1 - L, y1, L, gc); d.drawFastVLine(x1, y1 - L, L, gc);
}
static void draw_reticle(int x, int y, bool locked)
{
    uint16_t c = locked ? COL_RED : COL_CYAN;
    d.drawCircle(x, y, 5, c);
    d.drawFastHLine(x - 9, y, 5, c); d.drawFastHLine(x + 5, y, 5, c);
    d.drawFastVLine(x, y - 9, 5, c); d.drawFastVLine(x, y + 5, 5, c);
    d.drawPixel(x, y, COL_WHITE);
}
static void draw_cockpit(int ch)                                        // static canopy struts + gun ports
{
    uint16_t f = rgb(34, 40, 60), e = rgb(70, 84, 120);
    d.fillTriangle(0, ch - 1, 46, ch - 1, 0, ch - 22, f);
    d.fillTriangle(W - 1, ch - 1, W - 47, ch - 1, W - 1, ch - 22, f);
    d.drawLine(0, 14, 70, 36, e); d.drawLine(W - 1, 14, W - 71, 36, e);
    d.drawFastHLine(0, ch - 1, W, e);
    d.fillRect(1, ch - 4, 4, 4, COL_RED); d.fillRect(W - 5, ch - 4, 4, 4, COL_RED);
}
// Per-wave deep-space backdrop — an OPEN VOID, by design. The player asked for no sun on the backdrop
// and no horizontal scanline banding, so this draws ZERO full-width horizontal fills/lines: the flat
// space the caller already laid stays as the base, and ALL depth comes from RADIAL cues — a few soft
// nebula PUFFS (filled discs), faint parallax stars, and a dim bloom + tunnel rings where the warp
// streaks converge. Per (seed,sector,system) so each sortie reads distinct; recoloured per wave.
// Cheap (small circles + dots, no full-frame fills, no per-row sine), no heap.
static void draw_backdrop(float top, int ch, int jx, int jy)
{
    (void)jx; (void)jy;
    uint16_t fc = wave_col();
    int t0 = (int)top, h = ch - t0; if (h < 1) h = 1;
    int vx = (int)CX, vy = (int)s_cy;
    // gentle view parallax: the far field drifts opposite the reticle so panning your aim feels like
    // turning your head in the cockpit (a few px at most). A cheap depth cue, no extra draw passes.
    int parx = (int)((s_aimx - CX) * 0.05f), pary = (int)((s_aimy - s_cy) * 0.05f);

    // (1) nebula puffs: soft filled discs (dim halo + a slightly brighter core), placed per system.
    //     Discs, never lines -> no horizontal striping; a slow breath via s_anim keeps them alive.
    uint32_t hh = pg_hash3(g.seed ^ 0x51A7u, g.sector, (uint32_t)g.sys);
    int breath = (int)(2.0f * sinf(s_anim * 0.04f));
    for (int i = 0; i < 3; i++) {
        uint32_t hp = pg_hash3(hh, (uint32_t)i, 0x9E37u);
        int nx = 18 + (int)(hp % (W - 36)) - parx;
        int ny = t0 + 4 + (int)((hp >> 9) % (h > 8 ? h - 8 : 1)) - pary;
        int nr = 14 + (int)((hp >> 18) % 16) + breath;          // ~14..30
        d.fillCircle(nx, ny, nr, shade(fc, 1, 10));             // dim outer haze
        d.fillCircle(nx, ny, nr * 3 / 5, shade(fc, 1, 7));      // softer core
    }

    // (2) faint parallax far stars (fixed field, drifted by the reticle) under the moving warp streaks.
    for (int i = 0; i < NSTAR; i += 3) {
        if (((s_anim + star[i].tw) & 63) < 2) continue;                  // occasional twinkle-out
        int xx = ((int)star[i].x - parx) % W; if (xx < 0) xx += W;
        int yy = t0 + ((((int)star[i].y - pary) % h) + h) % h;
        d.drawPixel(xx, yy, star[i].layer == 2 ? COL_GREY : COL_DIM);
    }

    // (3) vanishing-point depth: a subtle radial bloom where the warp streaks converge (kept dim so it
    //     reads as the throat of the tunnel, NOT a sun), plus two faint rings for the tunnel mouth.
    for (int r = 16; r >= 2; r -= 2) d.fillCircle(vx, vy, r, shade(fc, 9 - r / 3, 36));
    d.drawCircle(vx, vy, 34, shade(fc, 2, 11));
    d.drawCircle(vx, vy, 58, shade(fc, 1, 11));

    // (4) battle-deck: a dim Mode-7 energy grid fanning down from the vanishing point -> a real ground
    //     reference that sells the 3D WITHOUT any full-width horizontal line (nh=0, so no scanline
    //     striping). It shares the reticle parallax and flares briefly with screen-shake (hit intensity).
    fx3d::Grid gc;
    gc.horizon = vy; gc.bottom = ch - 1; gc.vanx = CX - (float)parx; gc.scroll = 0.0f;
    gc.xspread = 168.0f; gc.nv = 6; gc.nh = 0;
    gc.col = shade(fc, 3, 6); gc.glow = 0;
    gc.intensity = 56 + (int)(s_shake * 6.0f); if (gc.intensity > 150) gc.intensity = 150;
    fx3d::grid(gc);
}
static int foes_alive(void) { int n = 0; for (int i = 0; i < NFOE; i++) if (s_foe[i].on) n++; return n; }

static void spawn_foe(int kind, int hp)
{
    for (int i = 0; i < NFOE; i++) if (!s_foe[i].on) {
        Foe *f = &s_foe[i];
        f->on = 1; f->kind = (uint8_t)kind; f->passed = 0; f->strafe = 0;
        f->ez = 150.0f + (float)rnd(60);                     // emerge closer -> reaches engage range fast
        f->ex = (float)(rnd(161) - 80); f->ey = (float)(rnd(101) - 50);
        f->wphase = (float)rnd(628) * 0.01f; f->bank = 0;
        f->engagez = 40.0f + (kind == FOE_HEAVY ? 22.0f : kind == FOE_SCOUT ? -2.0f : 8.0f) + (float)rnd(10);   // hold-and-fight distance
        f->hp = f->hpmax = (int16_t)hp; f->firecd = (int16_t)(700 + rnd(900)); f->strafecd = (int16_t)(2800 + rnd(3500));
        f->hitms = 0;
        return;
    }
}
static void combat_spawn_wave(void)
{
    bool last = (s_wave_left == 1);
    for (int i = 0; i < s_cc.per_wave; i++) {                 // mixed wing: scouts, fighters, heavies
        int roll = rnd(100), kind, hp;
        if (roll < 22)      { kind = FOE_SCOUT; hp = s_cc.foe_hp * 6 / 10; if (hp < 12) hp = 12; }
        else if (roll < 42) { kind = FOE_HEAVY; hp = s_cc.foe_hp * 9 / 5 + 20; }
        else                { kind = FOE_FIGHTER; hp = s_cc.foe_hp; }
        spawn_foe(kind, hp);
    }
    if (last && s_cc.ace) spawn_foe(FOE_ACE, s_cc.foe_hp * 22 / 10 + 30);
    s_wave++; s_wave_left--;
    s_wave_tint = WAVE_PAL[(s_wave - 1) % 6];       // each wave recolours enemies + backdrop
    s_spawn_timer = 1300.0f;                       // gap before the next wave once this one is clear
    snprintf(s_cmsg, sizeof s_cmsg, "%s %d/%d", tx("ONDATA", "WAVE"), s_wave, s_cc.waves);
    s_cmsg_until = s_now + 1400; sfx(SFX_LOCK);
    if (last && s_cc.ace) sfx(SFX_ALARM);          // dramatic incoming-ace klaxon
}

static void combat_end(int result)   // 1 = win, -1 = fail/retreat, 2 = destroyed
{
    nucleo_app_set_fullscreen(false);    // leaving the action screen -> the hint footer comes back
    s_result = result; s_earn_cr = 0;
    g.kills += s_mkills;
    if (result == 2) { g.hull = 0; save_write(); start_cine(CINE_LOSE); return; }
    if (result == 1) {
        s_earn_cr = s_cc.reward_cr + s_cc.kill_cr * s_mkills;
        g.credits = clampi(g.credits + s_earn_cr, 0, 9999999);
        if (s_cc.rep_fac >= 0)       g.rep[s_cc.rep_fac]       = clampi(g.rep[s_cc.rep_fac] + s_cc.rep_gain, -100, 100);
        if (s_cc.enemy_rep_fac >= 0) g.rep[s_cc.enemy_rep_fac] = clampi(g.rep[s_cc.enemy_rep_fac] - s_cc.enemy_rep_loss, -100, 100);
        if (s_mission >= 0) {
            const Mission *m = cur_mission(s_mission);
            if (m->once) g.missions_done |= bit(s_mission);
            if (m->set_flag >= 0) g.flags |= bit(m->set_flag);
        }
        sfx(SFX_WIN);
    } else {                                        // retreat / objective lost: salvage only on ambushes
        if (s_mission < 0) { s_earn_cr = s_cc.kill_cr * s_mkills; g.credits = clampi(g.credits + s_earn_cr, 0, 9999999); }
        sfx(SFX_LOSE);
    }
    nucleo_audio_stop();
    save_write();
    go(ST_DEBRIEF);
}

// ---- arcade helpers: combo, power-up drops/pickup, missiles --------------------------------
static void toast(const char *s) { snprintf(s_toast, sizeof s_toast, "%s", s); s_toast_until = s_now + 1300; }
static void register_kill(void)   // arcade combo: chained kills keep the meter alive
{
    if (s_now < s_combo_until) { if (s_combo < 99) s_combo++; } else s_combo = 1;
    s_combo_until = s_now + 2500;
}
static void maybe_drop_pickup(float ex, float ey, float ez)
{
    int chance = 22 + (s_combo > 1 ? s_combo * 4 : 0); if (chance > 55) chance = 55;   // combos drop more
    if (rnd(100) >= chance) return;
    for (int i = 0; i < NPU; i++) if (!s_pu[i].on) {
        Pickup *p = &s_pu[i];
        p->on = 1; p->kind = (uint8_t)rnd(PU_KINDS); p->ex = ex; p->ey = ey; p->ez = ez; p->life = 6500;
        return;
    }
}
static void apply_pickup(int kind)
{
    switch (kind) {
        case PU_SHIELD:  s_shield = clampi(s_shield + 30, 0, s_shieldmax);  toast(tx("SCUDO +", "SHIELD +"));     break;
        case PU_REPAIR:  g.hull   = clampi(g.hull + 18, 0, g.hull_max);     toast(tx("SCAFO +", "HULL +"));       break;
        case PU_MISSILE: if (s_msl_ammo < NMSL) s_msl_ammo++;               toast(tx("MISSILE +", "MISSILE +"));  break;
        default:         s_rapid_until = s_now + 6000;                      toast(tx("FUOCO RAPIDO", "RAPID FIRE")); break;  // PU_RAPID
    }
    sfx(SFX_BUY);
}
// kill bookkeeping shared by laser + missile: count, combo, shake, boom SFX, maybe a drop
static void kill_foe(Foe *f, float sx, float sy, float sc, bool vis)
{
    if (vis) {
        spawn_boom(f->ex, f->ey, f->ez, sc, wave_col(), f->kind == FOE_ACE);
        int rr = clampi((int)(9.0f * sc * kind_rscale(f->kind)), 4, 60);       // shatter the flash model
        for (int i = 0; i < NDEATH; i++) if (!s_death[i].on) {
            s_death[i].on = 1; s_death[i].model = (uint8_t)f->kind;
            s_death[i].x = (int16_t)sx; s_death[i].y = (int16_t)sy; s_death[i].r = (int16_t)rr;
            uint16_t dc = wave_col(); { uint16_t kh = kind_hue(f->kind); if (kh) dc = cmix(dc, kh, 120); }
            s_death[i].bank = f->bank; s_death[i].yaw = f->bank * 0.32f; s_death[i].col = dc;
            s_death[i].t = s_death[i].t0 = (int16_t)(f->kind == FOE_ACE ? 520 : 360);
            break;
        }
    }
    float kx = f->ex, ky = f->ey, kz = f->ez;
    f->on = 0; s_kills++; s_mkills++;
    s_shake = (f->kind == FOE_ACE) ? 7.0f : 4.0f;
    sfx(SFX_BOOM); register_kill(); maybe_drop_pickup(kx, ky, kz);
    (void)sx; (void)sy;
}
static void missile_fire(void)
{
    if (s_msl_ammo <= 0) { sfx(SFX_DENY); return; }
    int slot = -1; for (int i = 0; i < NMSL; i++) if (!s_msl[i].on) { slot = i; break; }
    if (slot < 0) { sfx(SFX_DENY); return; }
    s_msl_ammo--;
    Msl *m = &s_msl[slot];
    m->on = 1; m->target = (s_lock >= 0 && s_foe[s_lock].on) ? s_lock : -1;
    m->ex = (float)(rnd(5) - 2); m->ey = 2.0f; m->ez = ZNEAR + 6.0f;   // launches just below the nose, visible at once
    s_muz_until = s_now + 80; sfx(SFX_LAUNCH);
}

static void player_fire(void)
{
    int cd = 170 - g.weapon * 26; if (cd < 70) cd = 70;      // faster base fire than before...
    if (s_now < s_rapid_until) cd = cd / 2 + 8;              // ...rapid-fire power-up roughly doubles the rate
    if (s_now - s_pfire_ms < cd) return;
    s_pfire_ms = s_now;
    s_fire_flash_until = s_now + 80;                          // twin-laser FX window
    s_muz_until = s_now + 60;                                 // gun-port muzzle flash
    sfx(SFX_LASER);
    if (s_lock >= 0 && s_foe[s_lock].on) {                    // targeting computer: a lock is a guaranteed hit
        Foe *f = &s_foe[s_lock];
        f->hp -= 18 + g.weapon * 8; f->hitms = 90; s_hitmark_until = s_now + 110;
        float sx = 0, sy = 0, sc = 0; bool vis = project(f->ex, f->ey, f->ez, &sx, &sy, &sc);
        if (f->hp <= 0) kill_foe(f, sx, sy, sc, vis);
        else { if (vis) spawn_sparks(f->ex, f->ey, f->ez, COL_AMBER); sfx(SFX_HIT); }
    }
}
static void hurt_player(int dmg)
{
    s_shield_hit_ms = s_now;
    bool had_shield = (s_shield > 0);
    if (s_shield > 0) { int a = dmg < s_shield ? dmg : s_shield; s_shield -= a; dmg -= a; }
    bool hull_hit = (dmg > 0);
    if (hull_hit) { g.hull -= dmg; if (g.hull < 0) g.hull = 0; }

    if (had_shield && s_shield == 0) {                 // shields just dropped
        sfx(SFX_SHIELD_DOWN); s_shake = 6.0f;
    } else if (hull_hit) {                             // hull thud + red vignette
        sfx(SFX_HULL); s_hullvig_until = s_now + 200; s_shake = 9.0f;
    } else {                                           // fully absorbed by shields -> canopy ripple
        for (int i = 0; i < NRIP; i++) if (!s_rip[i].on) {
            s_rip[i].on = 1; s_rip[i].life = s_rip[i].life0 = 260;
            s_rip[i].x = (int)s_aimx; s_rip[i].y = (int)s_aimy; break;
        }
        s_shake = 4.0f; sfx(SFX_HIT);
    }
    int crit = g.hull_max / 4;                         // hull-critical klaxon (rate-limited; lets the thud ring)
    if (g.hull > 0 && hull_hit && g.hull < crit && s_now - s_alarm_ms > 1500) { s_alarm_ms = s_now; sfx(SFX_ALARM); }
    if (g.hull <= 0) combat_end(2);
}

static void combat_reset_common(void)
{
    nucleo_app_set_fullscreen(true);                 // combat reclaims the footer rows (bigger playfield)
    for (int i = 0; i < NBOLT; i++) s_bolt[i].on = 0;
    for (int i = 0; i < NFOE; i++)  s_foe[i].on = 0;
    for (int i = 0; i < NPART; i++) s_part[i].on = 0;
    for (int i = 0; i < NSHK; i++)  s_shk[i].on = 0;
    for (int i = 0; i < NRIP; i++)  s_rip[i].on = 0;
    for (int i = 0; i < NDEATH; i++) s_death[i].on = 0;
    for (int i = 0; i < NMSL; i++)  s_msl[i].on = 0;
    for (int i = 0; i < NPU; i++)   s_pu[i].on = 0;
    s_msl_ammo = NMSL; s_msl_reload = 0; s_rapid_until = 0;
    s_combo = 0; s_combo_until = 0; s_toast[0] = 0; s_toast_until = 0; s_wave_tint = WAVE_PAL[0];
    s_part_rr = 0; s_flash_until = s_muz_until = s_hullvig_until = s_nearmiss_ms = s_alarm_ms = 0;
    s_hitmark_until = s_smoke_ms = 0;
    s_cy = (pf_top() + nucleo_app_content_height()) * 0.5f;
    s_aimx = CX; s_aimy = s_cy; s_aim_hv = s_aim_vv = 0; s_aim_h_until = s_aim_v_until = 0;
    if (g_tilt && nucleo_imu_present()) nucleo_imu_recenter();   // "hold it as you like" -> capture neutral
    s_throttle10 = 5; s_pfire_ms = 0; s_lock = -1; s_lock_since = 0; s_fire_flash_until = 0; s_shake = 0;
    for (int i = 0; i < NWARP; i++) respawn_warp(i);
    s_shieldmax = g.shield_max; s_shield = s_shieldmax; s_shield_hit_ms = 0; s_regen_acc = 0;
    s_kills = s_mkills = 0; s_wave = 0; s_result = 0; s_earn_cr = 0;
    s_cmsg[0] = 0; s_cmsg_until = 0; s_combat_t0 = s_now; s_ward_on = 0;
}
static void combat_launch(void)
{
    s_wave_left = s_cc.waves; s_spawn_timer = 0;
    if (s_cc.type == MT_ESCORT)      { s_ward_on = 1; s_ward_max = s_ward_hp = 120; s_ward_x = -40; s_ward_vx = 6; }
    else if (s_cc.type == MT_DEFEND) { s_ward_on = 1; s_ward_max = s_ward_hp = 170; s_ward_x = 0;   s_ward_vx = 0; }
    combat_spawn_wave();
    go(ST_COMBAT); sfx(SFX_LAUNCH);
}
static void combat_begin_mission(int mid)
{
    const Mission *m = cur_mission(mid);
    s_mission = mid;
    s_cc.type = m->type; s_cc.foe_fac = m->foe_fac; s_cc.waves = m->waves; s_cc.per_wave = m->per_wave;
    s_cc.foe_hp = m->foe_hp; s_cc.foe_dmg = m->foe_dmg; s_cc.foe_speed = m->foe_speed_pml * 0.1f; s_cc.ace = m->ace;
    s_cc.reward_cr = m->reward_cr; s_cc.kill_cr = m->kill_cr;
    s_cc.rep_fac = m->offer_fac; s_cc.rep_gain = m->rep_gain;
    s_cc.enemy_rep_fac = m->foe_fac; s_cc.enemy_rep_loss = m->enemy_rep_loss;
    combat_reset_common(); combat_launch();
}
static void combat_begin_ambush(void)
{
    s_mission = -1;
    int tier = 1 + (int)g.sector; if (tier > 12) tier = 12;            // sector-tier (matches web ambushCfg)
    s_cc.type = MT_PATROL; s_cc.foe_fac = F_RELITTI;
    s_cc.waves = 2 + (tier >= 6 ? 1 : 0); s_cc.per_wave = 2 + (tier & 1);
    s_cc.foe_hp = 30 + tier * 6; s_cc.foe_dmg = 7 + tier; s_cc.foe_speed = 78.0f + tier * 4;
    s_cc.ace = 0;
    s_cc.reward_cr = 0; s_cc.kill_cr = 18 + tier * 4;
    s_cc.rep_fac = -1; s_cc.rep_gain = 0; s_cc.enemy_rep_fac = F_RELITTI; s_cc.enemy_rep_loss = 2;
    combat_reset_common(); combat_launch();
}

// Procedural mission slots offered at the current system (count scaled by sector; 0 at Echo systems).
static int eligible_missions(int *out)
{
    int n = pg_mission_count(g.seed, g.sector, g.sys, SYSTEMS[g.sys].faction);
    if (n > NMISS_PER_SYS) n = NMISS_PER_SYS;
    for (int i = 0; i < n; i++) out[i] = i;          // slot index IS the eligibility key now
    return n;
}

// Reticle steering. Each arrow event is a velocity impulse; the keyboard auto-repeats a held arrow
// (350ms, then ~11/s), so the impulses stack toward a terminal speed -> the reticle accelerates while
// held and keeps gliding through the repeat gap instead of stop-go. A fresh press (after a lull) kicks
// harder for an instant, responsive tap. combat_step (a) applies the drag that eases it back to rest.
static void aim_steer(int axis, int dir)   // axis: 0 = horizontal, 1 = vertical
{
    float vmax  = AIM_VMAX + s_throttle10 * 9.0f;         // boost widens the speed cap
    float  *v   = axis ? &s_aim_vv : &s_aim_hv;
    int64_t *un = axis ? &s_aim_v_until : &s_aim_h_until;
    bool fresh  = (s_now >= *un);                          // window lapsed -> treat as a new press
    *v += (float)dir * (fresh ? AIM_KICK : AIM_PUSH);
    if (*v >  vmax) *v =  vmax;
    if (*v < -vmax) *v = -vmax;
    *un = s_now + 220;                                     // input-recent window; aim-assist waits it out
}

// Shape a raw tilt axis (-1..1) into a steering command: a small deadzone kills rest jitter, then an
// expo curve gives fine control near centre and full authority at the edges. ~0.45g of tilt = full.
static float tilt_shape(float v)
{
    const float DZ = 0.08f, GAIN = 2.2f;
    float s = (v < 0) ? -1.0f : 1.0f, a = v * s;          // sign + magnitude
    if (a < DZ) return 0.0f;
    a = (a - DZ) / (1.0f - DZ);                            // remap past the deadzone
    a *= GAIN; if (a > 1.0f) a = 1.0f;                     // a modest tilt reaches full authority
    a = a * (0.45f + 0.55f * a * a);                       // expo: gentle near centre
    return s * a;
}

static void combat_step(float dt)
{
    int   ch  = nucleo_app_content_height();
    float top = (float)pf_top();
    int   ms  = (int)(dt * 1000.0f);

    for (int i = 0; i < NDEATH; i++) if (s_death[i].on) { s_death[i].t -= ms; if (s_death[i].t <= 0) s_death[i].on = 0; }

    // (a0) tilt controller (ADV BMI270): when the device is actively tilted, ease the reticle velocity
    //      toward the tilt-commanded velocity. A flat device produces 0 (deadzone) -> arrows still work
    //      unchanged, so the two input schemes coexist. Neutral was captured at combat start.
    if (g_tilt && nucleo_imu_present()) {
        float ttx, tty;
        if (nucleo_imu_tilt(&ttx, &tty)) {
            float vmax = AIM_VMAX + s_throttle10 * 9.0f;
            float dx = tilt_shape(ttx), dy = tilt_shape(tty);
            if (dx != 0.0f) s_aim_hv += (dx * vmax - s_aim_hv) * fminf(10.0f * dt, 1.0f);
            if (dy != 0.0f) s_aim_vv += (dy * vmax - s_aim_vv) * fminf(10.0f * dt, 1.0f);
        }
    }

    // (a) reticle glide: arrow impulses (aim_steer) decay under drag here -> smooth acceleration while
    //     a key is held and a soft inertial stop on release, with no stop-go across the keyboard's
    //     repeat gap. Drag is framerate-independent (expf), velocity settles to a true dead stop.
    float fr = expf(-dt * AIM_FRIC);
    s_aim_hv *= fr; s_aim_vv *= fr;
    if (s_aim_hv > -3.0f && s_aim_hv < 3.0f) s_aim_hv = 0;
    if (s_aim_vv > -3.0f && s_aim_vv < 3.0f) s_aim_vv = 0;
    s_aimx += s_aim_hv * dt;
    s_aimy += s_aim_vv * dt;
    if      (s_aimx < 10)     { s_aimx = 10;     if (s_aim_hv < 0) s_aim_hv = 0; }   // stop dead at a wall
    else if (s_aimx > W - 10) { s_aimx = W - 10; if (s_aim_hv > 0) s_aim_hv = 0; }
    if      (s_aimy < top + 10) { s_aimy = top + 10; if (s_aim_vv < 0) s_aim_vv = 0; }
    else if (s_aimy > ch - 10)  { s_aimy = ch - 10;  if (s_aim_vv > 0) s_aim_vv = 0; }

    // (b) screen-shake decay
    if (s_shake > 0) { s_shake -= dt * 24.0f; if (s_shake < 0) s_shake = 0; }

    // (c) forward rail: the starfield streaks toward the camera
    float rail = RAIL + s_throttle10 * 3.0f;
    for (int i = 0; i < NWARP; i++) { s_warp[i].ez -= rail * dt; if (s_warp[i].ez < ZNEAR) respawn_warp(i); }

    // (d) shield regen — quick recharge between hits (the player can't dodge; shield is the skill buffer)
    if (s_shield < s_shieldmax && (s_now - s_shield_hit_ms) > 1100) {
        s_regen_acc += dt * 26.0f;
        while (s_regen_acc >= 1.0f && s_shield < s_shieldmax) { s_shield++; s_regen_acc -= 1.0f; }
    }
    // (d2) missile reserve slowly refills back up to the cap (max 3)
    if (s_msl_ammo < NMSL) { s_msl_reload += dt * 1000.0f; if (s_msl_reload >= 6000.0f) { s_msl_reload -= 6000.0f; s_msl_ammo++; } }

    // (e) ward drifts laterally; its death fails the mission
    if (s_ward_on) {
        s_ward_x += s_ward_vx * dt;
        if (s_ward_x > 120) s_ward_x = 120; else if (s_ward_x < -120) s_ward_x = -120;
        if (s_ward_hp <= 0) { s_ward_on = 0; combat_end(-1); return; }
    }

    // (f) enemy AI: a real dogfight — foes close to an engage distance and HOLD there, weaving + firing;
    // they die ONLY to player fire. Periodically one makes a strafing run (dives in, deals a hit, retreats).
    // Ward-hunters dive on the escort instead of the player. No free fly-through clear.
    float closeF = 30.0f + s_cc.foe_speed * 0.3f;
    for (int i = 0; i < NFOE; i++) {
        Foe *f = &s_foe[i];
        if (!f->on) continue;
        if (f->hitms > 0) f->hitms -= ms;                       // hit-flash decay
        bool huntward = (s_ward_on && (i & 1));                 // half the wing hunts the ward
        float spd = closeF * kind_speed(f->kind);
        f->wphase += dt * kind_wrate(f->kind);
        if (f->strafe) {                                        // diving in for a close pass
            f->ez -= spd * 2.4f * dt;
            if (!f->passed && f->ez < 30.0f) { f->passed = 1; if ((s_now - s_nearmiss_ms) > 350) { s_nearmiss_ms = s_now; sfx(SFX_PASS); } }
            if (f->ez <= ZNEAR) {                               // contact: hit, then retreat (NOT removed)
                float sx, sy, sc; bool vis = project(f->ex, f->ey, ZNEAR + 0.5f, &sx, &sy, &sc);
                if (huntward) { s_ward_hp -= s_cc.foe_dmg + 3; sfx(SFX_PASS); }
                else { hurt_player(s_cc.foe_dmg + 3); if (vis) spawn_streak(sx, sy); }
                f->ez = 90.0f + (float)rnd(20); f->strafe = 0; f->passed = 0; f->strafecd = (int16_t)(3200 + rnd(3500));
                if (s_result) return;
            }
        } else {
            if (f->ez > f->engagez) { f->ez -= spd * dt; if (f->ez < f->engagez) f->ez = f->engagez; }
            else f->ez = f->engagez + sinf(f->wphase * 0.6f) * 7.0f;     // bob around engage range
            if (f->strafecd > 0) f->strafecd -= ms;
            if (f->strafecd <= 0 && f->ez < f->engagez + 16.0f) f->strafe = 1;
        }
        float amp = (18.0f + (130.0f - (f->ez < 130.0f ? f->ez : 130.0f)) * 0.16f) * kind_wamp(f->kind);
        float tgtx = huntward ? s_ward_x : 0.0f;
        float wx = sinf(f->wphase) * amp, wy = sinf(f->wphase * 0.7f + 1.0f) * amp * 0.5f;
        f->ex += (tgtx + wx - f->ex) * 1.4f * dt;
        f->ey += (wy - f->ey) * 1.4f * dt;
        f->bank += (sinf(f->wphase) * 0.8f - f->bank) * 4.0f * dt;
        if (f->firecd > 0) f->firecd -= ms;
        if (f->firecd <= 0 && f->ez < 130.0f && !f->strafe) {
            spawn_tracer(f, huntward);
            f->firecd = (int16_t)(kind_firems(f->kind) + 600 + rnd(600));
        }
        if (f->hp * 3 < f->hpmax && (s_now - s_smoke_ms) > 110) {       // wounded: trails embers (damaged read)
            s_smoke_ms = s_now;
            Part *sp = part_alloc(); float a = (float)rnd(628) * 0.01f;
            sp->on = 1; sp->kind = PK_SPARK; sp->ex = f->ex; sp->ey = f->ey; sp->ez = f->ez;
            sp->vx = cosf(a) * 30.0f; sp->vy = sinf(a) * 30.0f - 10.0f; sp->vz = -(20.0f + (float)rnd(25));
            sp->life = sp->life0 = (int16_t)(150 + rnd(140)); sp->col = rnd(2) ? COL_AMBER : rgb(210, 120, 60);
        }
    }

    // (f1) player missiles: race downrange, home onto the locked foe, detonate on contact (big hit)
    for (int i = 0; i < NMSL; i++) {
        Msl *m = &s_msl[i]; if (!m->on) continue;
        m->ez += 165.0f * dt;
        if (m->target >= 0 && s_foe[m->target].on) {
            Foe *f = &s_foe[m->target];
            m->ex += (f->ex - m->ex) * 4.5f * dt;
            m->ey += (f->ey - m->ey) * 4.5f * dt;
            if (m->ez >= f->ez - 4.0f) {
                float sx = 0, sy = 0, sc = 0; bool vis = project(f->ex, f->ey, f->ez, &sx, &sy, &sc);
                f->hp -= 70 + g.weapon * 12; f->hitms = 130; s_hitmark_until = s_now + 140;
                if (f->hp <= 0) kill_foe(f, sx, sy, sc, vis);
                else { if (vis) spawn_boom(f->ex, f->ey, f->ez, sc * 0.7f, COL_AMBER, false); sfx(SFX_BOOM); }
                m->on = 0;
            }
        } else {                                                // lock lost: fly straight out and fizz far away
            m->target = -1;
            if (m->ez > ZFAR) { spawn_boom(m->ex, m->ey, ZFAR - 1.0f, 0.3f, COL_AMBER, false); m->on = 0; }
        }
    }
    // (f1b) power-ups drift toward the cockpit; auto-collected on arrival
    for (int i = 0; i < NPU; i++) {
        Pickup *p = &s_pu[i]; if (!p->on) continue;
        p->ez -= 34.0f * dt; p->ey += 7.0f * dt; p->life -= ms;
        if (p->ez <= ZNEAR + 2.0f) { apply_pickup(p->kind); p->on = 0; continue; }
        if (p->life <= 0) p->on = 0;
    }

    // (f2) particle / shockwave / ripple advance
    for (int i = 0; i < NPART; i++) { Part *p = &s_part[i]; if (!p->on) continue;
        if (p->kind == PK_STREAK) { p->ex += p->vx * dt; p->ey += p->vy * dt; }
        else { p->ex += p->vx * dt; p->ey += p->vy * dt; p->ez += p->vz * dt; p->vy += 40.0f * dt; }
        p->life -= ms;
        if (p->life <= 0 || (p->kind != PK_STREAK && p->ez < ZNEAR)) p->on = 0;
    }
    for (int i = 0; i < NSHK; i++) { if (!s_shk[i].on) continue;
        s_shk[i].life -= ms; if (s_shk[i].life <= 0) s_shk[i].on = 0; }
    for (int i = 0; i < NRIP; i++) { if (!s_rip[i].on) continue;
        s_rip[i].life -= ms; if (s_rip[i].life <= 0) s_rip[i].on = 0; }

    // (g) lock-on: the on-screen foe nearest the reticle
    s_lock = -1;
    {
        float best = 1e9f;
        for (int i = 0; i < NFOE; i++) {
            Foe *f = &s_foe[i]; if (!f->on) continue;
            float sx, sy, sc; if (!project(f->ex, f->ey, f->ez, &sx, &sy, &sc)) continue;
            float dx = sx - s_aimx, dy = sy - s_aimy, dd = dx * dx + dy * dy;
            float r = 9.0f + 19.0f * sc;                        // lock window: forgiving enough to track weavers
            if (dd < r * r && dd < best) { best = dd; s_lock = i; }
        }
    }
    if (s_lock >= 0) { if (!s_lock_since) { s_lock_since = s_now; sfx(SFX_LOCK); } }
    else s_lock_since = 0;

    // (g2) gentle aim assist: when you are NOT steering an axis, the reticle eases onto the locked foe
    if (s_lock >= 0) {
        float sx, sy, sc;
        if (project(s_foe[s_lock].ex, s_foe[s_lock].ey, s_foe[s_lock].ez, &sx, &sy, &sc) &&
            sx > 8 && sx < W - 8 && sy > top + 8 && sy < ch - 8) {
            if (s_now >= s_aim_h_until) s_aimx += (sx - s_aimx) * 2.0f * dt;   // gentle pull only (was 5.0): helps tracking, not aiming
            if (s_now >= s_aim_v_until) s_aimy += (sy - s_aimy) * 2.0f * dt;
        }
    }

    // (h) enemy tracers converge toward you / the ward; damage on arrival
    for (int b = 0; b < NBOLT; b++) {
        Bolt *bo = &s_bolt[b]; if (!bo->on) continue;
        bo->ez += bo->vz * dt;
        float t = bo->ez / ZCONV;
        if (t > 1) t = 1; else if (t < 0) t = 0;
        bo->ex = bo->tx * t; bo->ey = bo->ty * t;
        bo->life -= ms;
        if (bo->life <= 0 || bo->ez < ZNEAR) {
            if (bo->ez < ZNEAR && bo->life > 0) {
                if (bo->aimward && s_ward_on) s_ward_hp -= s_cc.foe_dmg;
                else if (!bo->aimward) { hurt_player(s_cc.foe_dmg); if (s_result) return; }
            }
            bo->on = 0;
        }
    }

    // (i) ward death applied this frame (ram in (f) / tracer in (h)) fails the mission BEFORE any win
    if (s_ward_on && s_ward_hp <= 0) { s_ward_on = 0; combat_end(-1); return; }

    // (j) wave flow / win
    if (foes_alive() == 0) {
        if (s_wave_left > 0) { s_spawn_timer -= dt * 1000.0f; if (s_spawn_timer <= 0) combat_spawn_wave(); }
        else combat_end(1);
    }
}

// Off-screen enemy indicator: a small arrow pinned to the playfield frame, pointing toward a foe that
// has slipped outside the view — a dogfight readability staple so the player always knows where to turn.
static void draw_foe_arrow(int top, int ch, float sx, float sy, uint16_t col)
{
    float dx = sx - CX, dy = sy - s_cy;
    float L = sqrtf(dx * dx + dy * dy); if (L < 1.0f) L = 1.0f;
    float nx = dx / L, ny = dy / L;
    int px = clampi((int)sx, 7, W - 7), py = clampi((int)sy, top + 6, ch - 8);
    int tx_ = px + (int)(nx * 5.0f), ty_ = py + (int)(ny * 5.0f);     // tip points outward, toward the foe
    int bx = px - (int)(nx * 4.0f), by = py - (int)(ny * 4.0f);       // base centre
    int wx = (int)(-ny * 3.0f), wy = (int)(nx * 3.0f);               // base half-width (perpendicular)
    d.fillTriangle(tx_, ty_, bx + wx, by + wy, bx - wx, by - wy, col);
}

static void draw_combat(void)
{
    int ch = nucleo_app_content_height();
    float top = (float)pf_top();
    int jx = s_shake > 0 ? (rnd(3) - 1) : 0, jy = s_shake > 0 ? (rnd(3) - 1) : 0;   // hit shake
    d.fillRect(0, 0, W, ch, COL_SPACE);
    // ALWAYS paint the scene. The old code hid the whole backdrop behind a flat-gray fill during each
    // boom's s_flash_until window; with several foes dying in a wave those windows overlapped and the
    // background strobed dark<->bright -> the "flicker in battle". The boom now reads from the LOCAL
    // core-flash + shockwave + debris below, which are spatially anchored and never strobe.
    draw_backdrop(top, ch, jx, jy);

    // 3D warp starfield: radial streaks from the vanishing point
    for (int i = 0; i < NWARP; i++) {
        float sx, sy, sc; if (!project(s_warp[i].ex, s_warp[i].ey, s_warp[i].ez, &sx, &sy, &sc)) continue;
        if (sy < top || sy >= ch || sx < 0 || sx >= W) continue;   // cull off-screen (avoid huge drawLines)
        if (s_warp[i].ez > 180) { d.drawPixel((int)sx + jx, (int)sy + jy, COL_DIM); }
        else {
            float tx2, ty2, tc; project(s_warp[i].ex, s_warp[i].ey, s_warp[i].ez + 14.0f, &tx2, &ty2, &tc);
            d.drawLine((int)tx2 + jx, (int)ty2 + jy, (int)sx + jx, (int)sy + jy, s_warp[i].ez < 80 ? COL_WHITE : COL_GREY);
        }
    }

    // ward at fixed depth
    if (s_ward_on) {
        float sx, sy, sc;
        if (project(s_ward_x, 0.0f, ZWARD, &sx, &sy, &sc)) {
            int x = (int)sx + jx, y = (int)sy + jy, r = (int)(10 * sc); if (r < 3) r = 3;
            uint16_t wc = (s_ward_hp * 3 < s_ward_max) ? COL_RED : COL_CYAN;
            if (s_cc.type == MT_DEFEND) { d.drawRect(x - r, y - r, 2 * r, 2 * r, wc); d.fillCircle(x, y, r / 2, rgb(60, 70, 100)); }
            else { d.fillTriangle(x - r, y, x + r, y - r / 2, x + r, y + r / 2, rgb(70, 80, 110)); d.drawLine(x + r, y - r / 2, x + r, y + r / 2, wc); }
        }
    }

    // enemy tracers (red, grow toward you)
    for (int b = 0; b < NBOLT; b++) {
        Bolt *bo = &s_bolt[b]; if (!bo->on) continue;
        float sx, sy, sc; if (!project(bo->ex, bo->ey, bo->ez, &sx, &sy, &sc)) continue;
        float tx2, ty2, tc; project(bo->ex, bo->ey, bo->ez + 18.0f, &tx2, &ty2, &tc);
        d.drawLine((int)tx2 + jx, (int)ty2 + jy, (int)sx + jx, (int)sy + jy, COL_RED);
        if (bo->ez < 60) d.fillRect((int)sx + jx - 1, (int)sy + jy - 1, 3, 3, COL_RED);
    }

    // enemies, painted back-to-front
    int order[NFOE], nord = 0;
    for (int i = 0; i < NFOE; i++) if (s_foe[i].on) order[nord++] = i;
    for (int a = 1; a < nord; a++) {
        int k = order[a], j = a - 1;
        while (j >= 0 && s_foe[order[j]].ez < s_foe[k].ez) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int o = 0; o < nord; o++) {
        Foe *f = &s_foe[order[o]]; float sx, sy, sc;
        if (!project(f->ex, f->ey, f->ez, &sx, &sy, &sc)) continue;
        int x = (int)sx + jx, y = (int)sy + jy, r = clampi((int)(9.0f * sc * kind_rscale(f->kind)), 2, 60);
        uint16_t fcol = wave_col();
        { uint16_t kh = kind_hue(f->kind); if (kh) fcol = cmix(fcol, kh, 120); }   // per-kind identity
        if (f->ez > 95.0f) fcol = cmix(fcol, COL_DIM, clampi((int)((f->ez - 95.0f) * 1.1f), 0, 120));  // atmospheric haze: far foes sink into the void
        if (f->hp * 3 < f->hpmax) fcol = cmix(fcol, COL_RED, 110);   // wounded -> reddens
        if (f->hitms > 0)         fcol = cmix(fcol, COL_WHITE, 180);  // hit-flash
        draw_ship3d(x, y, r, f->bank, f->kind, fcol);
        if (f->hitms > 55 && r >= 5) fx3d::dither_disc(x, y, clampi(r + 2, 6, 20), COL_CYAN);  // dithered shield bubble (hit, bounded)
        if (f->firecd > 0 && f->firecd < 240 && f->ez < 130.0f && !f->strafe) {    // charging to fire: telegraph
            d.fillCircle(x, y - r / 3, 2 + (f->firecd < 120 ? 1 : 0), ((s_anim >> 1) & 1) ? COL_RED : COL_AMBER);
        }
        if (f->hp < f->hpmax && r >= 5) {
            int w = r * 2, fw = w * f->hp / f->hpmax;
            d.drawFastHLine(x - r, y - r - 3, w, rgb(60, 30, 30));
            d.drawFastHLine(x - r, y - r - 3, fw, COL_RED);
        }
    }

    // model-shatter deaths: blow each killed foe's flash model apart into cooling, spinning shards.
    for (int i = 0; i < NDEATH; i++) {
        if (!s_death[i].on) continue;
        float pr  = 1.0f - (float)s_death[i].t / (float)s_death[i].t0;
        float dsc = (float)s_death[i].r / 1.25f;
        fx3d::shatter(*ship_model(s_death[i].model), (float)(s_death[i].x + jx), (float)(s_death[i].y + jy),
                      dsc, s_death[i].yaw + pr * 2.2f, s_death[i].bank, s_death[i].col, pr);
    }

    // player missiles streaking downrange (homing) with a short trail
    for (int i = 0; i < NMSL; i++) {
        Msl *m = &s_msl[i]; if (!m->on) continue;
        float sx, sy, sc; if (!project(m->ex, m->ey, m->ez, &sx, &sy, &sc)) continue;
        int x = (int)sx + jx, y = (int)sy + jy;
        float tx2, ty2, tc; if (project(m->ex, m->ey, m->ez - 18.0f, &tx2, &ty2, &tc))
            d.drawLine((int)tx2 + jx, (int)ty2 + jy, x, y, COL_AMBER);
        int r = clampi((int)(3.0f * sc), 1, 4);
        d.fillCircle(x, y, r, COL_WHITE); d.drawCircle(x, y, r + 1, COL_AMBER);
    }
    // power-ups: pulsing diamond, colour-coded by kind, drifting in
    for (int i = 0; i < NPU; i++) {
        Pickup *p = &s_pu[i]; if (!p->on) continue;
        float sx, sy, sc; if (!project(p->ex, p->ey, p->ez, &sx, &sy, &sc)) continue;
        int x = (int)sx + jx, y = (int)sy + jy, r = clampi((int)(6.0f * sc), 3, 11);
        uint16_t c = pu_col(p->kind);
        d.fillTriangle(x, y - r, x - r, y, x + r, y, c);
        d.fillTriangle(x, y + r, x - r, y, x + r, y, shade(c, 3, 5));
        d.drawCircle(x, y, r + 2 + ((s_anim >> 1) & 1), c);
        d.drawPixel(x, y, COL_WHITE);
    }

    // explosions: 3D debris + sparks, shockwave rings, core flash
    for (int i = 0; i < NPART; i++) {
        Part *p = &s_part[i]; if (!p->on) continue;
        float fade = (float)p->life / p->life0;
        if (p->kind == PK_STREAK) {
            int x = (int)p->ex + jx, y = (int)p->ey + jy;
            d.drawLine(x, y, x - (int)(p->vx * 0.03f), y - (int)(p->vy * 0.03f), p->col);
            continue;
        }
        float sx, sy, sc; if (!project(p->ex, p->ey, p->ez, &sx, &sy, &sc)) continue;
        int x = (int)sx + jx, y = (int)sy + jy;
        uint16_t c = (p->kind == PK_SPARK) ? p->col
                   : (fade > 0.6f ? p->col : (fade > 0.3f ? COL_AMBER : COL_DIM));
        if (p->kind == PK_DEBRIS && sc > 0.6f && fade > 0.5f) d.fillRect(x - 1, y - 1, 2, 2, c);
        else d.drawPixel(x, y, c);
    }
    for (int i = 0; i < NSHK; i++) {
        Shk *s = &s_shk[i]; if (!s->on) continue;
        float t = 1.0f - (float)s->life / s->life0; int R = (int)(4 + 44 * t);
        int x = (int)s->cx_ + jx, y = (int)s->cy_ + jy;
        uint16_t c = t < 0.4f ? COL_WHITE : (t < 0.7f ? s->col : COL_DIM);
        d.drawCircle(x, y, R, c);
        if (t < 0.5f) d.drawCircle(x, y, R - 1, c);
    }
    if (s_now < s_flash_until) {                          // localized boom bloom (replaces the old full-frame flash)
        int x = (int)s_flash_x + jx, y = (int)s_flash_y + jy;
        d.fillCircle(x, y, s_flash_r0 + 4, shade(COL_AMBER, 2, 6));   // soft outer glow, anchored at the kill
        d.fillCircle(x, y, s_flash_r0, COL_AMBER);
        d.fillCircle(x, y, s_flash_r0 / 2, COL_WHITE);
    }

    // targeting-computer box on the locked foe
    if (s_lock >= 0 && s_foe[s_lock].on) {
        float sx, sy, sc;
        if (project(s_foe[s_lock].ex, s_foe[s_lock].ey, s_foe[s_lock].ez, &sx, &sy, &sc)) {
            int x = (int)sx + jx, y = (int)sy + jy, r = clampi((int)(9.0f * sc) + 4, 6, 64);
            draw_target_box(x, y, r);
        }
    }

    // twin converging lasers from the cockpit gun ports to the reticle (glow -> core + impact spark)
    if (s_now < s_fire_flash_until) {
        int ax = (int)s_aimx + jx, ay = (int)s_aimy + jy;
        uint16_t gl = shade(COL_RED, 2, 5);
        d.drawLine(1, ch - 1, ax, ay, gl);          d.drawLine(W - 2, ch - 1, ax, ay, gl);
        d.drawLine(3, ch - 1, ax, ay, COL_RED);     d.drawLine(W - 4, ch - 1, ax, ay, COL_RED);
        d.drawLine(4, ch - 1, ax, ay, COL_AMBER);   d.drawLine(W - 5, ch - 1, ax, ay, COL_AMBER);
        d.drawLine(5, ch - 1, ax, ay, COL_WHITE);   d.drawLine(W - 6, ch - 1, ax, ay, COL_WHITE);
        d.fillCircle(ax, ay, 3, COL_WHITE); d.drawCircle(ax, ay, 5, COL_AMBER);   // impact flash
    }
    if (s_now < s_muz_until) {                        // gun-port muzzle flash
        d.fillCircle(3, ch - 3, 5, COL_AMBER);   d.fillCircle(3, ch - 3, 3, COL_WHITE);
        d.fillCircle(W - 4, ch - 3, 5, COL_AMBER); d.fillCircle(W - 4, ch - 3, 3, COL_WHITE);
    }

    draw_reticle((int)s_aimx + jx, (int)s_aimy + jy, s_lock >= 0);
    if (s_now < s_hitmark_until) {                        // hitmarker: four ticks confirm a damaging hit
        int ax = (int)s_aimx + jx, ay = (int)s_aimy + jy;
        d.drawLine(ax - 8, ay - 8, ax - 4, ay - 4, COL_WHITE); d.drawLine(ax + 8, ay - 8, ax + 4, ay - 4, COL_WHITE);
        d.drawLine(ax - 8, ay + 8, ax - 4, ay + 4, COL_WHITE); d.drawLine(ax + 8, ay + 8, ax + 4, ay + 4, COL_WHITE);
    }
    // shield-absorb ripples on the canopy
    for (int i = 0; i < NRIP; i++) {
        if (!s_rip[i].on) continue;
        float t = 1.0f - (float)s_rip[i].life / s_rip[i].life0; int R = (int)(4 + 16 * t);
        d.drawCircle(s_rip[i].x + jx, s_rip[i].y + jy, R, t < 0.5f ? COL_CYAN : COL_DIM);
    }
    draw_cockpit(ch);
    if (s_now < s_hullvig_until) {                    // red hull-hit vignette (steady, no jitter)
        d.drawRect(0, (int)top, W, ch - (int)top, COL_RED);
        d.drawRect(1, (int)top + 1, W - 2, ch - (int)top - 2, rgb(140, 40, 36));
    }

    // off-screen enemy arrows: point toward any foe outside the frame (red if it's closing fast). Steady
    // (not jittered) and outside the foe loop so they read as instruments, not part of the chaos.
    for (int i = 0; i < NFOE; i++) {
        if (!s_foe[i].on) continue;
        float fx, fy, fsc; if (!project(s_foe[i].ex, s_foe[i].ey, s_foe[i].ez, &fx, &fy, &fsc)) continue;
        if (fx >= 6 && fx < W - 6 && fy >= top + 4 && fy < ch - 6) continue;     // on-screen -> no arrow
        draw_foe_arrow((int)top, ch, fx, fy, s_foe[i].ez < 80.0f ? COL_RED : COL_AMBER);
    }
    // hull-critical: a slow red border breath (distinct from the momentary hit vignette above) — a
    // peripheral "you're dying" cue that doesn't block the view.
    if (g.hull_max && g.hull * 100 / g.hull_max < 25 && ((s_anim >> 2) & 3) < 2)
        d.drawRect(0, (int)top, W, ch - (int)top, rgb(150, 30, 28));

    // HUD band: shield / hull / boost / missiles / wave|ward / kills (+ combo). Bars are outlined and the
    // shield/hull labels+bars BLINK when critical, so a glance tells you you're hurt. Steady, not jittered.
    d.fillRect(0, 0, W, 13, COL_SPACE);
    d.drawFastHLine(0, 13, W, rgb(40, 50, 80));
    char b[28];
    bool blink = ((s_anim >> 2) & 1);
    int spct = s_shieldmax ? s_shield * 100 / s_shieldmax : 0;
    int hpct = g.hull_max ? g.hull * 100 / g.hull_max : 0;
    uint16_t sc_col = (spct <= 0) ? (blink ? COL_RED : COL_DIM) : COL_CYAN;
    uint16_t hc_col = (hpct < 30) ? (blink ? COL_RED : COL_AMBER) : COL_GREEN;
    text_at(3, 3, 1, sc_col, "S");
    mini_bar(11, 4, 26, 6, spct, sc_col);
    d.drawRoundRect(11, 4, 26, 6, 1, rgb(40, 50, 80));
    text_at(41, 3, 1, (hpct < 30 && blink) ? COL_RED : COL_GREY, "H");
    mini_bar(49, 4, 26, 6, hpct, hc_col);
    d.drawRoundRect(49, 4, 26, 6, 1, rgb(40, 50, 80));
    snprintf(b, sizeof b, "B%d", s_throttle10); text_at(79, 3, 1, COL_AMBER, b);
    for (int i = 0; i < NMSL; i++) {                       // missile pips: filled = ready to fire
        int mx = 99 + i * 7;
        if (i < s_msl_ammo) d.fillTriangle(mx, 3, mx, 11, mx + 5, 7, COL_AMBER);
        else                d.drawTriangle(mx, 3, mx, 11, mx + 5, 7, COL_DIM);
    }
    if (s_now < s_rapid_until) d.drawFastHLine(99, 12, 19, COL_PURPLE);   // rapid-fire active
    if (s_ward_on) {
        int wpct = s_ward_max ? s_ward_hp * 100 / s_ward_max : 0;
        text_at(126, 3, 1, (wpct < 35 && blink) ? COL_RED : COL_GREEN, s_cc.type == MT_DEFEND ? tx("FAR", "BCN") : "CNV");
        mini_bar(150, 4, 24, 6, wpct, wpct < 35 ? COL_RED : COL_GREEN);
        d.drawRoundRect(150, 4, 24, 6, 1, rgb(40, 50, 80));
    } else {
        snprintf(b, sizeof b, "%s%d/%d", tx("O", "W"), s_wave, s_cc.waves);
        text_at(128, 3, 1, COL_GREY, b);
    }
    snprintf(b, sizeof b, "K%d", s_kills); text_at(226 - (int)strlen(b) * 6, 3, 1, COL_AMBER, b);
    if (s_combo > 1 && s_now < s_combo_until) {            // arcade combo meter, under the kill count
        snprintf(b, sizeof b, "x%d", s_combo);
        text_at(226 - (int)strlen(b) * 6, 16, 1, blink ? COL_WHITE : COL_PURPLE, b);
    }

    if (s_cmsg[0]  && s_now < s_cmsg_until)  center(ch / 2 - 4, 2, COL_CYAN, s_cmsg);
    if (s_toast[0] && s_now < s_toast_until) center(ch / 2 + 16, 1, COL_GREEN, s_toast);
    if (s_now - s_combat_t0 < 3500)          // opening control legend (the hint footer is hidden in combat)
        center(ch - 31, 1, COL_DIM, tx("Frecce mira  A spara  S missile  Esc fuga",
                                       "Arrows aim  A fire  S missile  Esc flee"));
}

// ============================ screens: mission bay ===========================
static int s_elig[NMISSIONS];   // cached eligible-mission indices (set in draw_missions)
// `n` rarity pips (small diamonds) right-aligned ending at xr; n=0 (Common) draws nothing.
static void draw_stars(int xr, int y, int n, uint16_t col)
{
    for (int i = 0; i < n; i++) {
        int cx = xr - 3 - i * 8;
        d.fillTriangle(cx, y, cx - 3, y + 3, cx + 3, y + 3, col);
        d.fillTriangle(cx, y + 6, cx - 3, y + 3, cx + 3, y + 3, col);
    }
}
static void miss_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    const Mission *m = cur_mission(s_elig[idx]);          // also fills s_fv (rarity / archetype / target)
    uint16_t rc = fv_rarcol(s_fv.rarity);
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    char rew[12]; snprintf(rew, sizeof rew, "%d cr", m->reward_cr);
    if (tier == TIER_FOCUS) {
        text_at(x0, by + 2, fit_size(lp(m->name), xr - x0 - 28, 2), rc, lp(m->name));   // title in rarity colour
        draw_stars(xr, by + 4, s_fv.rarity, rc);                                        // rarity pips, top-right
        text_at(x0, by + 19, 1, COL_AMBER, rew);                                        // reward
        text_at(x0 + 46, by + 19, 1, COL_DIM, lp(FA_NAME[s_fv.arch]));                  // archetype tag
        int diff = m->waves * m->per_wave + (m->ace ? 2 : 0);                           // difficulty pips, right
        for (int s = 0; s < diff && s < 8; s++) d.fillRect(xr - 3 - (7 - s) * 5, by + 21, 3, 3, COL_RED);
    } else {
        int rw = (int)strlen(rew) * 6;
        uint16_t nc = (tier == TIER_NEAR) ? rc : tier_col(tier);
        text_vc(x0, by, bh, fit_size(lp(m->name), xr - rw - 8 - x0, 1), nc, lp(m->name));
        text_vr(xr, by, bh, 1, COL_AMBER, rew);
    }
}
static void draw_missions(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    title_band(tx("MISSIONI", "MISSIONS"), SYSTEMS[g.sys].name);
    int n = eligible_missions(s_elig);
    if (n == 0) { center(56, 2, COL_DIM, tx("Nessun contratto", "No contracts")); return; }
    if (s_misssel >= n) s_misssel = n - 1;
    list_fisheye(s_misssel, n, 28, 120, miss_row_fn);
}
static void draw_brief(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    const Mission *m = cur_mission(s_pick);                // also fills s_fv
    uint16_t rc = fv_rarcol(s_fv.rarity);
    d.fillRoundRect(6, 2, 228, 116, 6, COL_PANEL);
    d.drawRoundRect(6, 2, 228, 116, 6, rc);               // panel border tinted by rarity
    text_at(14, 5, fit_size(lp(m->name), 150, 2), rc, lp(m->name));
    text_vr(228, 6, 8, 1, rc, lp(FV_RARNAME[s_fv.rarity]));   // rarity name, top-right
    draw_wrapped_n(14, 24, 212, 11, COL_WHITE, lp(m->brief), 3);   // flavored brief (y24..57)
    char b[72];
    // intel line: named target (bounty) or gang, then any combat modifiers — clamped to the panel.
    int li = s_fv.has_enemy ? snprintf(b, sizeof b, "%s: %s", tx("Bersaglio", "Target"), g_lang ? s_en_en : s_en_it)
                            : snprintf(b, sizeof b, "%s: %s", tx("Banda", "Gang"), lp(FV_GANG[s_fv.gang]));
    for (int i = 0; i < s_fv.nmod && li > 0 && li < (int)sizeof b - 1; i++)
        li += snprintf(b + li, sizeof b - li, " . %s", lp(FV_MODS[s_fv.mod[i]]));
    if ((int)strlen(b) > 35) b[35] = 0;                   // keep it inside the panel width
    text_at(14, 59, 1, COL_CYAN, b);
    snprintf(b, sizeof b, "%s: %d x%d%s  vs %s", tx("Ostili", "Hostiles"),
             m->waves, m->per_wave, m->ace ? " +ASSO" : "", lp(FAC_NAME[m->foe_fac]));
    text_at(14, 70, 1, COL_GREY, b);
    if (m->offer_fac >= 0)
        snprintf(b, sizeof b, "%s %d cr (+%d/%s)  %s +%d", tx("Paga", "Pay"), m->reward_cr,
                 m->kill_cr, tx("abb", "kill"), lp(FAC_NAME[m->offer_fac]), m->rep_gain);
    else
        snprintf(b, sizeof b, "%s %d cr (+%d/%s)", tx("Paga", "Pay"), m->reward_cr, m->kill_cr, tx("abb", "kill"));
    text_at(14, 81, 1, COL_AMBER, b);
    const char *opt[2] = { tx("Accetta e lancia", "Accept & launch"), tx("Annulla", "Decline") };
    int oy = 95;
    for (int i = 0; i < 2; i++) {
        bool sel = (i == s_briefsel);
        if (sel) { d.fillRoundRect(12, oy, 216, 11, 3, COL_FOCUS); d.fillRect(12, oy + 2, ACC_W, 7, COL_FOCUS2); }
        text_vc(20, oy, 11, 1, sel ? COL_WHITE : COL_GREY, opt[i]);
        oy += 12;
    }
}
static void draw_debrief(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    bool win = (s_result == 1);
    center(14, 3, win ? COL_CYAN : COL_RED, win ? tx("VITTORIA", "VICTORY") : tx("RITIRATA", "RETREAT"));
    if (win && s_mission >= 0) draw_wrapped_n(MARGIN, 44, CW, 10, COL_WHITE, lp(cur_mission(s_mission)->win), 2);
    char b[40];
    snprintf(b, sizeof b, "%s: %d", tx("Abbattuti", "Kills"), s_mkills);
    center(72, 2, COL_GREY, b);
    snprintf(b, sizeof b, "+%d cr", s_earn_cr);
    center(92, 2, COL_AMBER, b);
    center(112, 1, COL_DIM, tx("- INVIO continua -", "- ENTER continue -"));
}

// ============================ screens: title / settings ======================
static int title_items(int *act)   // returns count; fills action ids
{
    int n = 0;
    if (s_has_save) act[n++] = 0;   // Continue
    act[n++] = 1;                   // New Game
    act[n++] = 2;                   // Settings
    return n;
}
static void title_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    int act[4]; title_items(act);
    const char *lbl[3] = { tx("Continua", "Continue"), tx("Nuova Partita", "New Game"), tx("Impostazioni", "Settings") };
    const char *t = lbl[act[idx]];
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    int sz = fit_size(t, xr - x0, tier == TIER_FOCUS ? 2 : 1);
    text_vc(x0, by, bh, sz, tier_col(tier), t);
}
// Shared menu backdrop: deep-space stars + a dim Mode-7 neon floor that recedes to the horizon, panned
// a few px by the IMU so physically tilting the Cardputer parallaxes the grid (ADV only; a harmless
// no-op otherwise). The floor scrolls slowly toward you -> a living 3D menu, not a flat star field.
static void menu_backdrop(int ch)
{
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    static float s_menu_panx = 0.0f;                       // IMU read throttled to ~8 Hz (I2C; pure eye-candy)
    if (nucleo_imu_present() && (s_anim & 3) == 0) { float ttx, tty; if (nucleo_imu_tilt(&ttx, &tty)) s_menu_panx = ttx; }
    fx3d::Grid gc;
    gc.horizon = (int)(ch * 0.46f); gc.bottom = ch - 1;
    gc.vanx = (float)W * 0.5f + s_menu_panx * 80.0f;
    gc.scroll = fmodf((float)s_anim * 0.02f, 1.0f);
    gc.xspread = 252.0f; gc.nv = 7; gc.nh = 9;
    gc.col = shade(COL_CYAN, 4, 6); gc.glow = shade(COL_CYAN, 2, 6);
    gc.intensity = 110;
    fx3d::grid(gc);
}
static void draw_title(void)
{
    int ch = nucleo_app_content_height();
    menu_backdrop(ch);                                         // Mode-7 neon floor + stars (IMU parallax)
    center(7, 2, shade(COL_CYAN, 2, 6), "COSTELLAZIONI");       // soft drop shadow
    center(6, 2, COL_CYAN, "COSTELLAZIONI");                    // 13*12=156 -> x42..198
    center(24, 1, COL_DIM, tx("mercante tra le stelle", "trader among the stars"));
    d.drawFastHLine(50, 35, W - 100, shade(COL_CYAN, 2, 5));    // cyan rule with a small flagship emblem
    d.fillCircle(W / 2 + 9, 35, 2, shade(COL_CYAN, 3, 5));      // engine glow
    draw_ship(W / 2, 35, 1, COL_AMBER);
    int act[4]; int n = title_items(act);
    if (s_msel >= n) s_msel = n - 1;
    list_fisheye(s_msel, n, 44, 120, title_row_fn);
}
// Settings rows. The TILT row exists only on the Cardputer ADV (it owns a BMI270); on the original
// board it is not shown at all, so the row count and the idx->kind map both depend on is_adv().
enum { SET_LANG = 0, SET_AUDIO, SET_TILT, SET_DELETE };
static int set_count(void) { return nucleo_ui_is_adv() ? 4 : 3; }
static int set_kind(int idx)
{
    if (nucleo_ui_is_adv()) return idx;                  // 0 lang, 1 audio, 2 tilt, 3 delete
    return (idx >= 2) ? SET_DELETE : idx;                // base: 0 lang, 1 audio, 2 delete
}
static void set_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    int kind = set_kind(idx);
    const char *nm = kind == SET_LANG  ? tx("Lingua", "Language")
                   : kind == SET_AUDIO ? tx("Audio", "Audio")
                   : kind == SET_TILT  ? tx("Inclinazione", "Tilt control")
                   :                      tx("Cancella salvataggio", "Delete save");
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    int vx = 150;                                               // value zone left edge
    int sz = fit_size(nm, vx - 8 - x0, tier == TIER_FOCUS ? 2 : 1);
    text_vc(x0, by, bh, sz, tier_col(tier), nm);
    int cy = by + bh / 2;
    if (kind == SET_LANG || kind == SET_AUDIO || kind == SET_TILT) {  // segmented toggle chips
        // Tilt with no working IMU yet (config blob not vendored) reads "n/d" instead of a toggle.
        if (kind == SET_TILT && !nucleo_imu_present()) { text_vr(xr, by, bh, 1, COL_AMBER, tx("n/d", "n/a")); return; }
        const char *a = kind == SET_LANG ? "IT" : "ON", *b = kind == SET_LANG ? "EN" : "OFF";
        bool left = kind == SET_LANG ? (g_lang == 0) : kind == SET_AUDIO ? (g_audio != 0) : (g_tilt != 0);
        d.fillRoundRect(vx,      cy - 8, 36, 16, 3, left ? COL_GREEN : COL_TRACK);
        d.fillRoundRect(vx + 40, cy - 8, 36, 16, 3, !left ? COL_GREEN : COL_TRACK);
        d.setTextSize(1);                                       // transparent bg over the chips
        d.setTextColor(left ? COL_SPACE : COL_GREY);
        d.setCursor(vx + 18 - (int)strlen(a) * 3, cy - 3); d.print(a);
        d.setTextColor(!left ? COL_SPACE : COL_GREY);
        d.setCursor(vx + 58 - (int)strlen(b) * 3, cy - 3); d.print(b);
    } else {                                                    // delete-save chip
        const char *v = s_has_save ? tx("Cancella", "Reset") : "-";
        text_vr(xr, by, bh, 1, s_has_save ? COL_RED : COL_DIM, v);
    }
}
static void draw_settings(void)
{
    int ch = nucleo_app_content_height();
    menu_backdrop(ch);
    title_band(tx("Impostazioni", "Settings"), NULL);
    list_fisheye(s_setsel, set_count(), 28, 120, set_row_fn);
}

// ============================ screens: map ===================================
static int map_x(int lx) { return 12 + lx * 216 / 100; }
static int map_y(int ly) { return 18 + ly * 99 / 100; }
static void draw_map(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    draw_hud();

    int cx = map_x(SYSTEMS[g.sys].x), cy = map_y(SYSTEMS[g.sys].y);
    // routes within range
    for (int i = 0; i < NSYS; i++) {
        if (i == g.sys) continue;
        float dd = sys_dist(g.sys, i);
        if (dd > g.jump_range) continue;
        int x = map_x(SYSTEMS[i].x), y = map_y(SYSTEMS[i].y);
        bool ok = g.fuel >= jump_cost(dd);
        uint16_t lc = ok ? rgb(40, 70, 110) : rgb(60, 30, 30);
        for (int s = 0; s < 10; s++) {                 // dashed
            int xa = cx + (x - cx) * s / 10, ya = cy + (y - cy) * s / 10;
            int xb = cx + (x - cx) * (s * 2 + 1) / 20, yb = cy + (y - cy) * (s * 2 + 1) / 20;
            d.drawLine(xa, ya, xb, yb, lc);
        }
    }
    // systems
    for (int i = 0; i < NSYS; i++) {
        int x = map_x(SYSTEMS[i].x), y = map_y(SYSTEMS[i].y);
        uint16_t c = faction_col(SYSTEMS[i].faction);
        if (SYSTEMS[i].beacon) {
            bool lit = (g.beacon_lit & bit(i)) != 0;
            d.drawCircle(x, y, 5, lit ? COL_CYAN : rgb(120, 50, 50));
        }
        d.fillCircle(x, y, 3, c);
    }
    // current system pulse + ship
    int pr = 6 + (int)((s_anim / 3) % 4);
    d.drawCircle(cx, cy, pr, COL_GREEN);
    draw_ship(cx, cy - 1, 2, COL_WHITE);
    // selected target reticle + info card
    if (s_target >= 0 && s_target != g.sys) {
        int x = map_x(SYSTEMS[s_target].x), y = map_y(SYSTEMS[s_target].y);
        int o = 7 + (int)((s_anim / 2) % 3);
        uint16_t rc = COL_AMBER;
        d.drawLine(x - o, y - o, x - o + 3, y - o, rc); d.drawLine(x - o, y - o, x - o, y - o + 3, rc);
        d.drawLine(x + o, y - o, x + o - 3, y - o, rc); d.drawLine(x + o, y - o, x + o, y - o + 3, rc);
        d.drawLine(x - o, y + o, x - o + 3, y + o, rc); d.drawLine(x - o, y + o, x - o, y + o - 3, rc);
        d.drawLine(x + o, y + o, x + o - 3, y + o, rc); d.drawLine(x + o, y + o, x + o, y + o - 3, rc);

        float dd = sys_dist(g.sys, s_target);
        int cost = jump_cost(dd);
        bool reach = (dd <= g.jump_range) && (g.fuel >= cost);
        d.fillRoundRect(MARGIN, 89, CW, 28, 4, COL_PANEL);
        d.drawRoundRect(MARGIN, 89, CW, 28, 4, COL_FOCUS2);
        const char *nm = SYSTEMS[s_target].name;
        char b[40];
        snprintf(b, sizeof b, "%s %d  %s %d", tx("cel", "cel"), cost, tx("dist", "dist"), (int)dd);
        int vw = (int)strlen(b) * 6;
        text_at(14, 92, fit_size(nm, CW - 12 - vw - 8, 2), COL_WHITE, nm);
        text_vr(226, 92, 16, 1, reach ? COL_GREEN : COL_RED, b);
        snprintf(b, sizeof b, "%s . %s", lp(ECON_NAME[SYSTEMS[s_target].econ]), lp(FAC_NAME[SYSTEMS[s_target].faction]));
        text_at(14, 108, 1, COL_GREY, b);
    }
    if (s_status[0]) center(80, 1, COL_RED, s_status);
}

// ============================ screens: system hub ============================
static const char *hub_label(int i)
{
    switch (i) {
        case 0: return tx("Mercato", "Market");
        case 1: return tx("Cantiere", "Shipyard");
        case 2: return tx("Sala missioni", "Mission Bay");
        case 3: return tx("Plancia", "Bridge");
        case 4: return tx("Mappa stellare", "Star map");
        default: return tx("Salva & Titolo", "Save & Title");
    }
}
static int s_jobs;   // cached eligible-mission count for the hub badge (set in draw_system)
static void hub_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    char badge[16]; badge[0] = 0; uint16_t bc = COL_AMBER;
    if (idx == 2 && s_jobs > 0) { snprintf(badge, sizeof badge, "%d %s", s_jobs, tx("lavori", "jobs")); bc = COL_AMBER; }
    else if (idx == 3)          { snprintf(badge, sizeof badge, "%d/%d", beacons_lit(), beacons_total()); bc = COL_CYAN; }
    int bw_px = (int)strlen(badge) * 6 * (tier == TIER_FOCUS ? 2 : 1);
    int avail = badge[0] ? (xr - bw_px - 8 - x0) : (xr - x0);
    const char *nm = hub_label(idx);
    text_vc(x0, by, bh, fit_size(nm, avail, tier == TIER_FOCUS ? 2 : 1), tier_col(tier), nm);
    if (badge[0]) text_vr(xr, by, bh, tier == TIER_FOCUS ? 2 : 1, bc, badge);
}
static void draw_system(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    const Sys *s = &SYSTEMS[g.sys];
    // top data card: planet + name + econ/faction + credits
    d.fillRect(0, 0, W, HDR_H, COL_PANEL);
    d.drawFastHLine(0, HDR_H, W, rgb(46, 60, 96));
    draw_planet(13, 13, 10, g.sys);
    char b[40];
    snprintf(b, sizeof b, "%d cr", g.credits);
    int crw = (int)strlen(b) * 6;                       // credits reserved on the econ line
    text_at(30, 3, fit_size(s->name, W - MARGIN - 30, 2), COL_CYAN, s->name);
    text_vr(W - MARGIN, 16, 8, 1, COL_AMBER, b);
    snprintf(b, sizeof b, "%s . %s", lp(ECON_NAME[s->econ]), lp(FAC_NAME[s->faction]));
    int el2 = fit_size(b, (W - MARGIN - crw - 6) - 30, 1);
    text_at(30, 18, el2, COL_GREY, b);
    int el[NMISSIONS]; s_jobs = eligible_missions(el);
    list_fisheye(s_hubsel, 6, 28, 120, hub_row_fn);
}

// ============================ screens: market ================================
static void buy_good(int gd)
{
    int price = unit_buy(g.sys, gd);
    if (cargo_used() >= g.cargo_max) { snprintf(s_status, sizeof s_status, "%s", tx("Stiva piena", "Hold full")); sfx(SFX_DENY); return; }
    if (g.credits < price) { snprintf(s_status, sizeof s_status, "%s", tx("Crediti insuff.", "Not enough cr")); sfx(SFX_DENY); return; }
    g.credits -= price; g.cargo[gd]++; sfx(SFX_BUY);
    snprintf(s_status, sizeof s_status, "%s %s -%d", tx("Comprato", "Bought"), lp(GOODS[gd].name), price);
}
static void sell_good(int gd)
{
    if (g.cargo[gd] <= 0) { sfx(SFX_DENY); return; }
    int price = unit_sell(g.sys, gd);
    g.credits += price; g.cargo[gd]--; sfx(SFX_OK);
    snprintf(s_status, sizeof s_status, "%s %s +%d", tx("Venduto", "Sold"), lp(GOODS[gd].name), price);
}
static void buy_fuel(void)
{
    if (g.fuel >= g.fuel_max) { snprintf(s_status, sizeof s_status, "%s", tx("Serbatoio pieno", "Tank full")); sfx(SFX_DENY); return; }
    int price = refuel_price(g.sys);
    if (g.credits < price) { sfx(SFX_DENY); return; }
    g.credits -= price; g.fuel++; sfx(SFX_BUY);
    snprintf(s_status, sizeof s_status, "%s +1 (-%d)", tx("Cella", "Cell"), price);
}
// market row: focused = big name + a detail line (price/owned); neighbours = one compact line.
static void mkt_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    bool fuel = (idx == NGOODS);
    const char *name = fuel ? tx("Carburante", "Fuel") : lp(GOODS[idx].name);
    int price = fuel ? refuel_price(g.sys) : unit_buy(g.sys, idx);
    uint16_t pc = fuel ? COL_CYAN
                       : (price < GOODS[idx].base ? COL_GREEN : (price > GOODS[idx].base ? COL_RED : COL_GREY));
    char b[20];
    if (tier == TIER_FOCUS) {
        text_at(x0, by + 2, fit_size(name, xr - x0, 2), COL_WHITE, name);
        if (fuel) snprintf(b, sizeof b, "%s %d", tx("Cella", "Cell"), price);
        else      snprintf(b, sizeof b, "%s %d", tx("Compra", "Buy"), price);
        text_at(x0, by + 19, 1, pc, b);
        if (fuel) snprintf(b, sizeof b, "%d/%d", g.fuel, g.fuel_max);
        else      snprintf(b, sizeof b, "x%d", g.cargo[idx]);
        text_vr(xr, by + 19, 8, 1, fuel ? COL_AMBER : (g.cargo[idx] ? COL_AMBER : COL_DIM), b);
    } else {
        uint16_t lc = tier_col(tier);
        text_vc(x0, by, bh, fit_size(name, 150 - x0, 1), lc, name);
        snprintf(b, sizeof b, "%d", price);
        text_vr(176, by, bh, 1, pc, b);
        if (fuel) snprintf(b, sizeof b, "%d/%d", g.fuel, g.fuel_max);
        else      snprintf(b, sizeof b, "x%d", g.cargo[idx]);
        text_vr(xr, by, bh, 1, fuel ? COL_AMBER : (g.cargo[idx] ? COL_AMBER : COL_DIM), b);
    }
}
static void draw_market(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    char cap[24]; snprintf(cap, sizeof cap, "%dcr %d/%d", g.credits, cargo_used(), g.cargo_max);
    title_band(tx("MERCATO", "MARKET"), cap);
    int bot = s_status[0] ? 110 : 120;
    list_fisheye(s_mktsel, NGOODS + 1, 28, bot, mkt_row_fn);
    if (s_status[0]) center(112, 1, COL_GREEN, s_status);
}

// ============================ screens: shipyard ==============================
static int up_level(int item)
{
    switch (item) { case 0: return (g.cargo_max - 20) / 10; case 1: return (g.fuel_max - 8) / 4;
                    case 2: return (g.jump_range - 64) / 12; case 3: return (g.hull_max - 100) / 25;
                    case 4: return g.sensors; case 5: return g.weapon; case 6: return (g.shield_max - 40) / 30;
                    default: return 0; }
}
static int up_cost(int item)
{
    int lv = up_level(item);
    switch (item) {
        case 0: return 250 * (lv + 1); case 1: return 180 * (lv + 1); case 2: return 300 * (lv + 1);
        case 3: return 220 * (lv + 1); case 4: return 200 * (lv + 1); case 5: return 240 * (lv + 1);
        case 6: return 260 * (lv + 1);
        default: return (g.hull_max - g.hull) * 3;     // repair
    }
}
static void buy_upgrade(int item)
{
    if (item == 7) {     // repair
        if (g.hull >= g.hull_max) { sfx(SFX_DENY); return; }
        int cost = up_cost(7); if (g.credits < cost) { sfx(SFX_DENY); return; }
        g.credits -= cost; g.hull = g.hull_max; sfx(SFX_OK); save_write(); return;
    }
    if (up_level(item) >= 4) { sfx(SFX_DENY); return; }      // maxed
    int cost = up_cost(item);
    if (g.credits < cost) { sfx(SFX_DENY); return; }
    g.credits -= cost;
    switch (item) { case 0: g.cargo_max += 10; break; case 1: g.fuel_max += 4; break;
                    case 2: g.jump_range += 12; break; case 3: g.hull_max += 25; g.hull = g.hull_max; break;
                    case 4: g.sensors += 1; break; case 5: g.weapon += 1; break; case 6: g.shield_max += 30; break; }
    sfx(SFX_BUY); save_write();
}
static const char *yard_name(int i)
{
    switch (i) {
        case 0: return tx("Stiva +10", "Hold +10");      case 1: return tx("Serbatoio +4", "Tank +4");
        case 2: return tx("Iperdrive +12", "Hyperdrive +12"); case 3: return tx("Scafo +25", "Hull +25");
        case 4: return tx("Sensori +1", "Sensors +1");   case 5: return tx("Laser +1", "Laser +1");
        case 6: return tx("Scudo +30", "Shield +30");    default: return tx("Riparazione", "Repair");
    }
}
static void yard_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    char b[16]; bool dim = false;
    if (idx == 7) {
        if (g.hull >= g.hull_max) { snprintf(b, sizeof b, "%s", tx("integro", "intact")); dim = true; }
        else snprintf(b, sizeof b, "%d cr", up_cost(7));
    } else if (up_level(idx) >= 4) { snprintf(b, sizeof b, "MAX"); dim = true; }
    else snprintf(b, sizeof b, "%d cr", up_cost(idx));
    const char *nm = yard_name(idx);
    uint16_t vc = dim ? COL_DIM : COL_AMBER;
    if (tier == TIER_FOCUS) {
        text_at(x0, by + 2, fit_size(nm, xr - x0, 2), COL_WHITE, nm);
        text_at(x0, by + 19, 1, vc, b);
        if (idx < 7) {                                       // level pips (0..4) on the focus row
            int lvl = up_level(idx);
            for (int s = 0; s < 4; s++) d.fillRect(xr - 4 - (3 - s) * 7, by + 20, 5, 5, s < lvl ? COL_GREEN : COL_TRACK);
        }
    } else {
        int vw = (int)strlen(b) * 6;
        text_vc(x0, by, bh, fit_size(nm, xr - vw - 8 - x0, 1), tier_col(tier), nm);
        text_vr(xr, by, bh, 1, vc, b);
    }
}
static void draw_shipyard(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    char cap[16]; snprintf(cap, sizeof cap, "%d cr", g.credits);
    title_band(tx("CANTIERE", "SHIPYARD"), cap);
    list_fisheye(s_yardsel, 8, 28, 120, yard_row_fn);
}

// ============================ screens: bridge (status) =======================
static const char *rank_name(void)
{
    int k = (int)g.kills;
    if (k >= 60) return tx("Asso leggendario", "Legendary Ace");
    if (k >= 30) return tx("Asso", "Ace");
    if (k >= 15) return tx("Veterano", "Veteran");
    if (k >= 5)  return tx("Pilota", "Pilot");
    return tx("Recluta", "Rookie");
}
static void draw_plancia(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    draw_hud();
    text_at(MARGIN, 16, 2, COL_CYAN, tx("Plancia", "Bridge"));
    char b[44];
    snprintf(b, sizeof b, "%s K%d", rank_name(), (int)g.kills);          // pilot rank + kill tally
    text_vr(W - MARGIN, 16, 16, 1, COL_GREY, b);
    text_at(MARGIN, 33, 2, COL_WHITE, "Lucciola");
    snprintf(b, sizeof b, "%s %d/%d   %s %d", tx("scafo", "hull"), g.hull, g.hull_max,
             tx("scudo", "shield"), g.shield_max);
    text_at(MARGIN, 53, 1, COL_GREY, b);
    snprintf(b, sizeof b, "cr %d   cel %d   rng %d", g.credits, g.fuel, g.jump_range);
    text_at(MARGIN, 63, 1, COL_GREY, b);
    snprintf(b, sizeof b, "%s %d/4   %s %d   %s %d/%d", tx("laser", "laser"), g.weapon,
             tx("sens", "sens"), g.sensors, tx("fari", "bcn"), beacons_lit(), beacons_total());
    text_at(MARGIN, 73, 1, COL_CYAN, b);
    text_at(MARGIN, 83, 1, COL_AMBER, tx("Reputazione", "Reputation"));
    int y = 91;
    for (int i = 0; i < NFAC; i++) {                          // 4 rows: 91,98,105,112 -> last bar 112..118
        text_at(14, y, 1, COL_GREY, lp(FAC_NAME[i]));
        int pct = (g.rep[i] + 100) / 2;     // -100..100 -> 0..100
        mini_bar(70, y, 86, 6, pct, g.rep[i] >= 0 ? COL_GREEN : COL_RED);
        snprintf(b, sizeof b, "%d", g.rep[i]);
        text_vr(226, y, 8, 1, g.rep[i] >= 0 ? COL_GREEN : COL_RED, b);
        y += 7;
    }
}

// ============================ screens: event =================================
static void draw_event(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    const Event *e = &EVENTS[s_ev];
    d.fillRoundRect(6, 2, 228, 116, 6, COL_PANEL);
    d.drawRoundRect(6, 2, 228, 116, 6, COL_FOCUS2);
    text_at(14, 7, fit_size(lp(e->title), 212, 2), COL_CYAN, lp(e->title));
    draw_wrapped_n(14, 28, 212, 11, COL_WHITE, lp(e->body), 3);        // clamp prose to 3 lines
    int oy = 70;                                                       // choices anchored low, always fit
    for (int i = 0; i < e->nch; i++) {
        bool sel = (i == s_evsel);
        bool ok = choice_affordable(&e->ch[i]);
        if (sel) { d.fillRoundRect(12, oy, 216, 13, 3, COL_FOCUS); d.fillRect(12, oy + 3, ACC_W, 7, COL_FOCUS2); }
        uint16_t c = !ok ? COL_DIM : (sel ? COL_WHITE : COL_GREY);
        int sz = fit_size(lp(e->ch[i].label), 200, 1);
        text_vc(20, oy, 13, sz, c, lp(e->ch[i].label));
        oy += 15;
    }
}

// ============================ input ==========================================
static void set_hint_for(int screen)
{
    switch (screen) {
        case ST_TITLE:    nucleo_app_set_hint(tx("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit")); break;
        case ST_SETTINGS: nucleo_app_set_hint(tx("SU/GIU  INVIO cambia  Esc indietro", "UP/DN  ENTER change  Esc back")); break;
        case ST_CINE:     nucleo_app_set_hint(tx("premi un tasto per saltare", "press any key to skip")); break;
        case ST_MAP:      nucleo_app_set_hint(tx("frecce mira  INVIO salta  Esc", "arrows aim  ENTER jump  Esc")); break;
        case ST_SYSTEM:   nucleo_app_set_hint(tx("SU/GIU  INVIO apri  Esc titolo", "UP/DN  ENTER open  Esc title")); break;
        case ST_MARKET:   nucleo_app_set_hint(tx("SU/GIU  DX/B compra  SX/S vendi", "UP/DN  R/B buy  L/S sell")); break;
        case ST_SHIPYARD: nucleo_app_set_hint(tx("SU/GIU  INVIO compra  Esc", "UP/DN  ENTER buy  Esc")); break;
        case ST_PLANCIA:  nucleo_app_set_hint(tx("Esc indietro", "Esc back")); break;
        case ST_EVENT:    nucleo_app_set_hint(tx("SU/GIU scegli  INVIO conferma", "UP/DN pick  ENTER confirm")); break;
        case ST_MISSIONS: nucleo_app_set_hint(tx("SU/GIU scegli  INVIO briefing  Esc", "UP/DN pick  ENTER brief  Esc")); break;
        case ST_BRIEF:    nucleo_app_set_hint(tx("SU/GIU  INVIO conferma  Esc", "UP/DN  ENTER confirm  Esc")); break;
        case ST_COMBAT:   nucleo_app_set_hint(g_tilt && nucleo_imu_present()
                              ? tx("Inclina/Frecce mira  A spara  S missile  Esc fuggi", "Tilt/Arrows aim  A fire  S missile  Esc flee")
                              : tx("Frecce mira  A spara  S missile  Esc fuggi", "Arrows aim  A fire  S missile  Esc flee")); break;
        case ST_DEBRIEF:  nucleo_app_set_hint(tx("INVIO continua", "ENTER continue")); break;
        default: break;
    }
}
static void title_enter(void)
{
    int act[4]; int n = title_items(act);
    int a = act[clampi(s_msel, 0, n - 1)];
    sfx(SFX_OK);
    if (a == 0) continue_game();
    else if (a == 1) new_game();
    else { s_setsel = 0; go(ST_SETTINGS); }
}
static void settings_activate(void)
{
    switch (set_kind(s_setsel)) {
        case SET_LANG:  g_lang ^= 1; cfg_write(); sfx(SFX_OK); break;
        case SET_AUDIO: g_audio ^= 1; cfg_write(); sfx(SFX_OK); break;
        case SET_TILT:  if (!nucleo_imu_present()) { sfx(SFX_DENY); break; }   // no IMU -> can't enable
                        g_tilt ^= 1; cfg_write(); sfx(SFX_OK); break;
        case SET_DELETE: if (s_has_save) { save_wipe(); sfx(SFX_BACK); } else sfx(SFX_DENY); break;
        default: break;
    }
    req();
}
static void map_cycle(int dir)
{
    int t = s_target;
    for (int k = 0; k < NSYS; k++) { t = (t + dir + NSYS) % NSYS; if (t != g.sys) break; }
    s_target = t; s_status[0] = 0; sfx(SFX_MOVE); req();
}
static void hub_open(void)
{
    sfx(SFX_OK); s_status[0] = 0;
    switch (s_hubsel) {
        case 0: s_mktsel = 0; go(ST_MARKET); break;
        case 1: s_yardsel = 0; go(ST_SHIPYARD); break;
        case 2: s_misssel = 0; go(ST_MISSIONS); break;
        case 3: go(ST_PLANCIA); break;
        case 4: go(ST_MAP); break;
        default: save_write(); s_ingame = true; go(ST_TITLE); break;   // keep run; just back to title
    }
}

static void on_key(int k, char ch)
{
    if (!cur_sys) return;          // OOM error state: ignore input (Esc closes via on_back at ST_TITLE)
    switch (s_screen) {
        case ST_CINE: cine_end(); return;
        case ST_TITLE: {
            int act[4]; int n = title_items(act);
            if (k == NK_UP)   { s_msel = (s_msel - 1 + n) % n; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_msel = (s_msel + 1) % n; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) title_enter();
            return;
        }
        case ST_SETTINGS:
            if (k == NK_UP)   { s_setsel = (s_setsel + set_count() - 1) % set_count(); sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % set_count(); sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) settings_activate();
            return;
        case ST_MAP:
            if (k == NK_UP || k == NK_DOWN || k == NK_RIGHT) map_cycle(k == NK_UP ? -1 : 1);
            else if (k == NK_ENTER) do_jump();
            return;
        case ST_SYSTEM:
            if (k == NK_UP)   { s_hubsel = (s_hubsel + 5) % 6; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_hubsel = (s_hubsel + 1) % 6; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) hub_open();
            return;
        case ST_MARKET:
            if (k == NK_UP)   { s_mktsel = (s_mktsel + NGOODS) % (NGOODS + 1); sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_mktsel = (s_mktsel + 1) % (NGOODS + 1); sfx(SFX_MOVE); req(); }
            else if (k == NK_RIGHT || ch == 'b' || ch == 'B' || k == NK_ENTER) { if (s_mktsel == NGOODS) buy_fuel(); else buy_good(s_mktsel); req(); }
            else if (ch == 's' || ch == 'S') { if (s_mktsel < NGOODS) sell_good(s_mktsel); req(); }
            return;
        case ST_SHIPYARD:
            if (k == NK_UP)   { s_yardsel = (s_yardsel + 7) % 8; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_yardsel = (s_yardsel + 1) % 8; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) { buy_upgrade(s_yardsel); req(); }
            return;
        case ST_EVENT: {
            const Event *e = &EVENTS[s_ev];
            if (k == NK_UP)   { s_evsel = (s_evsel - 1 + e->nch) % e->nch; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_evsel = (s_evsel + 1) % e->nch; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) apply_choice();
            return;
        }
        case ST_MISSIONS: {
            int el[NMISSIONS]; int n = eligible_missions(el);
            if (n == 0) return;
            if (k == NK_UP)   { s_misssel = (s_misssel - 1 + n) % n; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_misssel = (s_misssel + 1) % n; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) { s_pick = el[clampi(s_misssel, 0, n - 1)]; s_briefsel = 0; sfx(SFX_OK); go(ST_BRIEF); }
            return;
        }
        case ST_BRIEF:
            if (k == NK_UP || k == NK_DOWN) { s_briefsel ^= 1; sfx(SFX_MOVE); req(); }
            else if (k == NK_ENTER || k == NK_RIGHT) {
                if (s_briefsel == 0) combat_begin_mission(s_pick);
                else { sfx(SFX_BACK); go(ST_MISSIONS); }
            }
            return;
        case ST_COMBAT:
            if (s_result) return;
            if (k == NK_RIGHT)      aim_steer(0, +1);
            else if (k == NK_UP)    aim_steer(1, -1);
            else if (k == NK_DOWN)  aim_steer(1, +1);
            else if (ch == 's' || ch == 'S') missile_fire();                              // secondary: missile
            else if (k == NK_ENTER || ch == ' ' || ch == 'a' || ch == 'A' || ch == 'k' || ch == 'K' ||
                     ch == 'l' || ch == 'L' || ch == 'j' || ch == 'J' || ch == 'f' || ch == 'F' ||
                     ch == 'm' || ch == 'M') player_fire();
            else if (ch == ']' || ch == '=') { if (s_throttle10 < 10) s_throttle10++; }   // boost+
            else if (ch == '[' || ch == '-') { if (s_throttle10 > 0)  s_throttle10--; }   // boost-
            return;
        case ST_DEBRIEF:
            if (k == NK_ENTER || k == NK_RIGHT) { sfx(SFX_OK); go(ST_SYSTEM); }
            return;
        default: (void)k; (void)ch; return;
    }
}
// LEFT and BACK are delivered here only.
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        switch (s_screen) {
            case ST_TITLE:    break;                                // Left has no meaning on the title
            case ST_SETTINGS: settings_activate(); break;
            case ST_MAP:      map_cycle(-1); break;
            case ST_SYSTEM:   s_hubsel = (s_hubsel + 5) % 6; sfx(SFX_MOVE); req(); break;
            case ST_MARKET:   if (s_mktsel < NGOODS) { sell_good(s_mktsel); req(); } break;
            case ST_EVENT:    { const Event *e = &EVENTS[s_ev]; s_evsel = (s_evsel - 1 + e->nch) % e->nch; sfx(SFX_MOVE); req(); } break;
            case ST_BRIEF:    s_briefsel ^= 1; sfx(SFX_MOVE); req(); break;
            case ST_COMBAT:   if (!s_result) aim_steer(0, -1); break;   // aim left
            default: break;
        }
        return true;
    }
    // NK_BACK: pop one level; close app only at the title.
    switch (s_screen) {
        case ST_TITLE:    return false;                       // let the framework close the app
        case ST_CINE:     cine_end(); return true;
        case ST_EVENT:    return true;                        // must choose
        case ST_SETTINGS: sfx(SFX_BACK); go(ST_TITLE); return true;
        case ST_MAP: case ST_MARKET: case ST_SHIPYARD: case ST_PLANCIA: case ST_MISSIONS:
                          sfx(SFX_BACK); go(ST_SYSTEM); return true;
        case ST_BRIEF:    sfx(SFX_BACK); go(ST_MISSIONS); return true;
        case ST_COMBAT:   if (!s_result) combat_end(-1); return true;   // disengage = mission failed
        case ST_DEBRIEF:  go(ST_SYSTEM); return true;
        case ST_SYSTEM:   save_write(); sfx(SFX_BACK); go(ST_TITLE); return true;
        default:          return false;
    }
}

// ============================ draw / tick / poll =============================
static void on_draw(void)
{
    if (!cur_sys) {                // OOM at enter: honest message instead of dereferencing NULL pools
        d.fillScreen(COL_SPACE);
        d.setTextSize(1); d.setTextColor(COL_RED, COL_SPACE);
        d.setCursor(12, H / 2 - 4); d.print(g_lang ? "Out of memory - Esc" : "Memoria insufficiente - Esc");
        return;
    }
    switch (s_screen) {
        case ST_TITLE:    draw_title(); break;
        case ST_SETTINGS: draw_settings(); break;
        case ST_CINE:     draw_cine(); break;
        case ST_MAP:      draw_map(); break;
        case ST_SYSTEM:   draw_system(); break;
        case ST_MARKET:   draw_market(); break;
        case ST_SHIPYARD: draw_shipyard(); break;
        case ST_PLANCIA:  draw_plancia(); break;
        case ST_EVENT:    draw_event(); break;
        case ST_MISSIONS: draw_missions(); break;
        case ST_BRIEF:    draw_brief(); break;
        case ST_COMBAT:   draw_combat(); break;
        case ST_DEBRIEF:  draw_debrief(); break;
        default: break;
    }
}
// ~30 Hz animation only on the live screens; static screens repaint on input.
static bool poll(void)
{
    if (!cur_sys) return false;
    s_now = esp_timer_get_time() / 1000;
    // The title is a menu: it repaints on input only (request_draw / go()), so it sits perfectly
    // still instead of re-blitting the starfield at ~30 Hz (the idle flicker). Map / cinematic /
    // combat are motion screens and keep animating.
    // TITLE/SETTINGS now animate too: the Mode-7 menu floor scrolls and the IMU parallax tracks live.
    // The double buffer composites off-screen and blits once, so this is flicker-free (same as combat).
    bool animated = (s_screen == ST_MAP || s_screen == ST_CINE || s_screen == ST_COMBAT ||
                     s_screen == ST_TITLE || s_screen == ST_SETTINGS);
    if (!animated) return false;
    int64_t elapsed = s_now - s_last_frame;
    if (elapsed < 33) return false;
    s_last_frame = s_now;
    s_anim++;
    s_scroll[0] += 0.18f; if (s_scroll[0] >= 240) s_scroll[0] -= 240;
    s_scroll[1] += 0.45f; if (s_scroll[1] >= 240) s_scroll[1] -= 240;
    s_scroll[2] += 0.95f; if (s_scroll[2] >= 240) s_scroll[2] -= 240;
    if (s_screen == ST_COMBAT && !s_result) {
        float dt = elapsed / 1000.0f; if (dt > 0.05f) dt = 0.05f;
        combat_step(dt);
    }
    return true;
}

// EXCLUSIVE-MODE RAM: the game state (systems cache + combat pools) lives on the HEAP only while the
// app is open — allocated here, freed in on_exit — so it costs ZERO RAM at boot (the no-hoarding rule).
static bool cstl_alloc(void)
{
    cur_sys = (Sys  *)calloc(NSYS,  sizeof(Sys));
    s_warp  = (Warp *)calloc(NWARP, sizeof(Warp));
    s_part  = (Part *)calloc(NPART, sizeof(Part));
    s_bolt  = (Bolt *)calloc(NBOLT, sizeof(Bolt));
    s_foe   = (Foe  *)calloc(NFOE,  sizeof(Foe));
    return cur_sys && s_warp && s_part && s_bolt && s_foe;
}
static void cstl_free(void)
{
    free(cur_sys); cur_sys = nullptr;
    free(s_warp);  s_warp  = nullptr;
    free(s_part);  s_part  = nullptr;
    free(s_bolt);  s_bolt  = nullptr;
    free(s_foe);   s_foe   = nullptr;
}

static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
    if (!cstl_alloc()) {            // OOM: harmless error state (on_draw/poll/on_key guard on cur_sys)
        cstl_free();
        s_screen = ST_TITLE;
        nucleo_app_set_back_handler(on_back);
        nucleo_app_set_hint("Memoria insufficiente - Esc");
        req();
        return;
    }
    s_rng ^= (uint32_t)esp_timer_get_time();
    s_has_save = save_read(&g);     // peek (g is overwritten by new/continue anyway)
    s_ingame = false;
    s_screen = ST_TITLE; s_msel = 0; s_anim = 0; s_status[0] = 0;
    s_scroll[0] = s_scroll[1] = s_scroll[2] = 0;
    s_now = s_last_frame = esp_timer_get_time() / 1000;
    stars_init();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint_for(ST_TITLE);
    sfx(SFX_TITLE);
    req();
}
static void on_exit(void)
{
    if (s_ingame) save_write();
    nucleo_audio_stop();
    cstl_free();                   // release the game-state heap -> back to ZERO RAM until next open
}

extern "C" void nucleo_register_constellations(void)
{
    static const nucleo_app_def_t app = {
        "stelle", "Costellazioni", "Games", "Mercante stellare: riaccendi i Fari",
        'C', C_BLUE, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP   // free ~60KB (httpd/mDNS/voice/L1) before on_enter so the combat pools always fit -> no launch OOM
    };
    nucleo_app_register(&app);
}
