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
//   • RAM / exclusive-mode: ALL game state is one small static struct + static arrays (~250 B);
//     the rich content (systems, goods, events, bilingual text) is `static const` in flash. No heap,
//     no tasks, no decoder. So, like every other light app in the exclusive audit, exclusive_flags=0
//     and we never call nucleo_exclusive_* — the no-hoarding rule honoured by owning nothing.
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
#include "notify_synth.h"     // notify_voice_t + notify_synth_voices_wav (pure inline, stdio+math)
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
static Sys cur_sys[NSYS];
#define SYSTEMS cur_sys

static int  g_lang  = LANG_IT;   // mirrored in the settings file so the menu remembers it
static int  g_audio = 1;

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
struct Bolt { float ex, ey, ez, tx, ty, vz; int16_t life; uint8_t on, foe, aimward; };
struct Foe  { float ex, ey, ez, wphase, bank, engagez; int16_t hp, hpmax, firecd, strafecd; uint8_t on, kind, passed, strafe; };
static Bolt s_bolt[NBOLT];
static Foe  s_foe[NFOE];
static struct { float ex, ey, ez; } s_warp[NWARP];
// ---- juice pools (all .bss, no heap) ----------------------------------------
#define NPART 40        // debris + spark particles (shared round-robin pool)
#define NSHK  3         // expanding shockwave rings
#define NRIP  2         // shield-absorb ripples on the canopy
enum { PK_DEBRIS = 0, PK_SPARK, PK_STREAK };
struct Part { float ex, ey, ez, vx, vy, vz; int16_t life, life0; uint8_t on, kind; uint16_t col; };
struct Shk  { float cx_, cy_; int16_t life, life0; uint8_t on; uint16_t col; };
struct Rip  { int x, y; int16_t life, life0; uint8_t on; };
static Part s_part[NPART];
static Shk  s_shk[NSHK];
static Rip  s_rip[NRIP];
static int  s_part_rr;                 // round-robin cursor
static int64_t s_flash_until;          // brief dim full-frame flash window
static float   s_flash_x, s_flash_y;   // core-flash centre
static int     s_flash_r0;             // core-flash radius (capped)
static int64_t s_muz_until;            // twin muzzle-flash window
static int64_t s_hullvig_until;        // red edge vignette on a hull hit
static int64_t s_nearmiss_ms;          // rate-limit for the pass-by whoosh
static int64_t s_alarm_ms;             // rate-limit for the hull-critical klaxon
// the dogfight config in flight (filled from a Mission or synthesised for an ambush)
struct CombatCfg {
    int type, foe_fac, waves, per_wave, foe_hp, foe_dmg, ace;
    float foe_speed;                 // approach tuning
    int reward_cr, kill_cr, rep_fac, rep_gain, enemy_rep_fac, enemy_rep_loss;
};
static CombatCfg s_cc;
static float s_aimx, s_aimy;         // reticle screen position (float for smooth glide)
static int8_t s_aim_hx, s_aim_vx;    // steer intent: horizontal / vertical (-1/0/+1)
static int64_t s_aim_h_until, s_aim_v_until;
static int   s_throttle10;           // repurposed as BOOST 0..10
static int   s_lock;                 // index of foe locked by the reticle, -1 = none
static int64_t s_lock_since, s_fire_flash_until, s_pfire_ms;
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

// forward decls used before their definitions
static void combat_begin_ambush(void);
static int  eligible_missions(int *out);

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
    struct { uint32_t m; int l, a; } c = { CFG_MAGIC, g_lang, g_audio };
    fwrite(&c, sizeof c, 1, f);
    fclose(f);
}
static void cfg_read(void)
{
    FILE *f = fopen(DIR "/cfg.bin", "rb");
    if (!f) return;
    struct { uint32_t m; int l, a; } c;
    size_t n = fread(&c, sizeof c, 1, f);
    fclose(f);
    if (n == 1 && c.m == CFG_MAGIC) { g_lang = c.l ? 1 : 0; g_audio = c.a ? 1 : 0; }
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
enum { PG_COORD = 1, PG_ECON = 2, PG_FAC = 3, PG_BEACON = 4, PG_NAME = 5, PG_MISSION = 6, PG_MCOUNT = 7 };
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
static const char *MT_NAME[4][2]  = { { "Pattuglia", "Patrol" }, { "Taglia", "Bounty" }, { "Scorta", "Escort" }, { "Difesa", "Defend" } };
static const char *MT_BRIEF[4][2] = {
    { "Ripulisci le rotte dai predoni: ondate di caccia in arrivo.", "Sweep raiders off the lanes: fighter waves inbound." },
    { "Un asso nemico guida lo sciame. Battilo: veloce e blindato.", "An enemy ace leads the swarm. Beat him: fast and armoured." },
    { "Proteggi il convoglio finche' non e' al sicuro.",            "Protect the convoy until it reaches safety." },
    { "Difendi la piattaforma del Faro a ogni costo.",              "Defend the Beacon platform at all costs." },
};
static const char *MT_WIN[4][2] = {
    { "Rotte ripulite. Crediti accreditati.",          "Lanes cleared. Credits paid." },
    { "L'asso e' abbattuto. Le stelle brindano a te.", "The ace is down. The stars toast you." },
    { "Convoglio al sicuro. Buon lavoro, pilota.",     "Convoy safe. Good work, pilot." },
    { "La piattaforma regge. Sei la sua sentinella.",  "The platform holds. You are its sentinel." },
};
static char s_mn_it[24], s_mn_en[24];      // generated mission-name buffers (IT/EN)
static Mission s_genm;
#define NMISS_PER_SYS 4
// Generate the mission in `slot` at the current system into the static Mission s_genm (repeatable,
// difficulty scaled by sector). The display/combat code reads it exactly like a flash MISSIONS[] row.
static const Mission *cur_mission(int slot)
{
    int sf = SYSTEMS[g.sys].faction;
    GenMis gm; pg_mission(g.seed, g.sector, g.sys, slot, sf, &gm);
    int t = gm.type;
    snprintf(s_mn_it, sizeof s_mn_it, "%s %s", MT_NAME[t][0], SYSTEMS[g.sys].name);
    snprintf(s_mn_en, sizeof s_mn_en, "%s %s", MT_NAME[t][1], SYSTEMS[g.sys].name);
    s_genm.name[0] = s_mn_it; s_genm.name[1] = s_mn_en;
    s_genm.brief[0] = MT_BRIEF[t][0]; s_genm.brief[1] = MT_BRIEF[t][1];
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
        case SFX_LASER: // twin "pew" with a downward chirp + attack tick
            notify__voice(&v[0], 2100, 0.000f, 0.018f); v[0].amp = 0.50f;
            notify__voice(&v[1], 1400, 0.014f, 0.018f); v[1].amp = 0.50f;
            notify__voice(&v[2],  900, 0.026f, 0.030f); v[2].amp = 0.45f;
            notify__voice(&v[3], 2040, 0.000f, 0.018f); v[3].amp = 0.40f;
            notify__voice(&v[4], 1360, 0.014f, 0.018f); v[4].amp = 0.40f;
            notify__voice(&v[5], 3200, 0.000f, 0.008f); v[5].amp = 0.30f;
            return 6;
        case SFX_HIT:   // bright metallic shield ping (inharmonic partials)
            notify__voice(&v[0], 1760, 0.000f, 0.045f); v[0].amp = 0.55f;
            notify__voice(&v[1], 2637, 0.000f, 0.040f); v[1].amp = 0.40f;
            notify__voice(&v[2], 3520, 0.004f, 0.030f); v[2].amp = 0.30f;
            notify__voice(&v[3], 4699, 0.004f, 0.022f); v[3].amp = 0.22f;
            notify__voice(&v[4], 1245, 0.000f, 0.020f); v[4].amp = 0.30f;
            return 5;
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
    snprintf(p, sizeof p, DIR "/sfx/%s.wav", sfx_name(id));       // cached synth?
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
    for (int i = 0; i < NSTAR; i++) {
        float ang = (float)(star[i].tw) * 0.0982f + i;
        float reach = 6 + k * 120;
        float d0 = 6 + (float)((star[i].x + s_anim) % 40);
        int x0 = cx + (int)(cosf(ang) * d0), y0 = cy + (int)(sinf(ang) * d0);
        int x1 = cx + (int)(cosf(ang) * (d0 + reach)), y1 = cy + (int)(sinf(ang) * (d0 + reach));
        uint16_t c = (i & 3) ? COL_CYAN : COL_WHITE;
        d.drawLine(x0, y0, x1, y1, c);
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
    float fsx, fsy, fsc;
    if (project(ex, ey, ez, &fsx, &fsy, &fsc)) {
        s_flash_until = s_now + (big ? 80 : 50); s_flash_x = fsx; s_flash_y = fsy;
        int r0 = (int)(6 + 9 * sc); if (r0 > 14) r0 = 14;
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
    uint16_t panel = shade(col, 2, 5), edge = col;
    if (kind == FOE_ACE)        { panel = shade(rgb(200, 60, 40), 2, 5); edge = COL_AMBER; }
    else if (kind == FOE_HEAVY) { panel = shade(rgb(180, 110, 40), 2, 5); edge = rgb(230, 150, 60); }
    else if (kind == FOE_SCOUT) { panel = shade(COL_CYAN, 2, 5); edge = COL_CYAN; }
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
    uint16_t glow = kind == FOE_ACE ? COL_AMBER : COL_CYAN;
    d.fillRect(gx - 1, gy, 2, 2, glow);
    if (r >= 6) { d.drawPixel(gx, gy + 2, shade(glow, 3, 5)); d.drawPixel(gx - bk / 4, gy + 3, COL_DIM); }
    if (kind == FOE_ACE) {
        d.drawCircle(x, y, br + 2, COL_AMBER);
        d.drawLine(lx, y - r + sh, lx - 2, y - r - 2 + sh, COL_AMBER);
        d.drawLine(rx + pw, y - r - sh, rx + pw + 2, y - r - 2 - sh, COL_AMBER);
        d.fillRect(gx + 2, gy, 2, 2, COL_RED);
    } else if (kind == FOE_HEAVY && r >= 5) {              // armoured: double hull ring
        d.drawCircle(cxp, y, br + 2, edge);
    } else if (kind == FOE_SCOUT && r >= 4) {              // sleek: forward nose spike
        d.drawLine(x, y, x, y - r - 3, edge);
    }
}
static void draw_target_box(int x, int y, int r)                        // green corner brackets
{
    uint16_t gc = COL_GREEN; int L = clampi(r / 2, 4, 10);
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
// faction-tinted deep-space backdrop: nebula bands + a banded planet low on the view
static void draw_backdrop(float top, int ch, int jx, int jy)
{
    (void)jx; (void)jy;
    uint16_t fc = faction_col(s_cc.foe_fac);
    uint16_t neb = shade(fc, 1, 6);
    int ny0 = (int)top, ny1 = (int)top + (ch - (int)top) / 3;
    for (int y = ny0; y < ny1; y++)                       // nebula: sparse animated bands (4 on / 4 off)
        if (((y + (int)(s_anim >> 3)) & 4)) d.drawFastHLine(0, y, W, neb);
    int px = 150 + (int)(18.0f * sinf(s_anim * 0.004f));  // distant planet, smooth sway (no wrap jump)
    int pr = 16, py = ch - 18;                            // sits fully above the clipped y>=121 band
    uint16_t pc = shade(fc, 4, 5);
    d.fillCircle(px, py, pr, pc);
    for (int dy = -pr + 3; dy < pr - 5; dy += 4)          // latitude bands
        d.drawFastHLine(px - pr + 2, py + dy, 2 * pr - 4, shade(pc, (dy < 0 ? 3 : 2), 5));
    d.drawCircle(px, py, pr, shade(pc, 3, 4));
    d.fillCircle(px + pr / 3, py - pr / 3, pr - 4, COL_SPACE);   // terminator crescent
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
    s_spawn_timer = 1300.0f;                       // gap before the next wave once this one is clear
    snprintf(s_cmsg, sizeof s_cmsg, "%s %d/%d", tx("ONDATA", "WAVE"), s_wave, s_cc.waves);
    s_cmsg_until = s_now + 1400; sfx(SFX_LOCK);
    if (last && s_cc.ace) sfx(SFX_ALARM);          // dramatic incoming-ace klaxon
}

static void combat_end(int result)   // 1 = win, -1 = fail/retreat, 2 = destroyed
{
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

static void player_fire(void)
{
    int cd = 240 - g.weapon * 28; if (cd < 110) cd = 110;
    if (s_now - s_pfire_ms < cd) return;
    s_pfire_ms = s_now;
    s_fire_flash_until = s_now + 90;                          // twin-laser FX window
    s_muz_until = s_now + 70;                                 // gun-port muzzle flash
    sfx(SFX_LASER);
    if (s_lock >= 0 && s_foe[s_lock].on) {                    // targeting computer: a lock is a guaranteed hit
        Foe *f = &s_foe[s_lock];
        f->hp -= 18 + g.weapon * 8;
        float sx, sy, sc; bool vis = project(f->ex, f->ey, f->ez, &sx, &sy, &sc);
        if (f->hp <= 0) {
            if (vis) spawn_boom(f->ex, f->ey, f->ez, sc, faction_col(s_cc.foe_fac), f->kind == FOE_ACE);
            f->on = 0; s_kills++; s_mkills++;
            s_shake = (f->kind == FOE_ACE) ? 7.0f : 4.0f;
            sfx(SFX_BOOM);
        } else {
            if (vis) spawn_sparks(f->ex, f->ey, f->ez, COL_AMBER);
            sfx(SFX_HIT);
        }
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
    for (int i = 0; i < NBOLT; i++) s_bolt[i].on = 0;
    for (int i = 0; i < NFOE; i++)  s_foe[i].on = 0;
    for (int i = 0; i < NPART; i++) s_part[i].on = 0;
    for (int i = 0; i < NSHK; i++)  s_shk[i].on = 0;
    for (int i = 0; i < NRIP; i++)  s_rip[i].on = 0;
    s_part_rr = 0; s_flash_until = s_muz_until = s_hullvig_until = s_nearmiss_ms = s_alarm_ms = 0;
    s_cy = (pf_top() + nucleo_app_content_height()) * 0.5f;
    s_aimx = CX; s_aimy = s_cy; s_aim_hx = s_aim_vx = 0; s_aim_h_until = s_aim_v_until = 0;
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

static void combat_step(float dt)
{
    int   ch  = nucleo_app_content_height();
    float top = (float)pf_top();
    int   ms  = (int)(dt * 1000.0f);

    // (a) reticle steering via intent windows (arrow auto-repeat keeps the window alive)
    float retspd = 150.0f + s_throttle10 * 6.0f;
    if (s_now < s_aim_h_until) s_aimx += s_aim_hx * retspd * dt;
    if (s_now < s_aim_v_until) s_aimy += s_aim_vx * retspd * dt;
    if (s_aimx < 10) s_aimx = 10; else if (s_aimx > W - 10) s_aimx = W - 10;
    if (s_aimy < top + 10) s_aimy = top + 10; else if (s_aimy > ch - 10) s_aimy = ch - 10;

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
            float r = 10.0f + 26.0f * sc;                       // closer/bigger = easier lock
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
            if (s_now >= s_aim_h_until) s_aimx += (sx - s_aimx) * 5.0f * dt;   // ~16%/frame when idle
            if (s_now >= s_aim_v_until) s_aimy += (sy - s_aimy) * 5.0f * dt;
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

static void draw_combat(void)
{
    int ch = nucleo_app_content_height();
    float top = (float)pf_top();
    int jx = s_shake > 0 ? (rnd(3) - 1) : 0, jy = s_shake > 0 ? (rnd(3) - 1) : 0;   // hit shake
    d.fillRect(0, 0, W, ch, COL_SPACE);
    bool flashing = (s_now < s_flash_until);
    if (!flashing) draw_backdrop(top, ch, jx, jy);        // distant planet + nebula (hidden under a flash)
    else d.fillRect(0, (int)top, W, ch - (int)top, rgb(120, 130, 150));   // brief DIM full-frame flash

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
        draw_tie(x, y, r, (int)(f->bank * 2.0f), f->kind, faction_col(s_cc.foe_fac));
        if (f->hp < f->hpmax && r >= 5) {
            int w = r * 2, fw = w * f->hp / f->hpmax;
            d.drawFastHLine(x - r, y - r - 3, w, rgb(60, 30, 30));
            d.drawFastHLine(x - r, y - r - 3, fw, COL_RED);
        }
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
    if (s_now < s_flash_until) {
        int x = (int)s_flash_x + jx, y = (int)s_flash_y + jy;
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

    // twin converging lasers from the cockpit gun ports to the reticle (hot triple-stroke)
    if (s_now < s_fire_flash_until) {
        int ax = (int)s_aimx + jx, ay = (int)s_aimy + jy;
        d.drawLine(2, ch - 1, ax, ay, COL_RED);     d.drawLine(W - 3, ch - 1, ax, ay, COL_RED);
        d.drawLine(3, ch - 1, ax, ay, COL_AMBER);   d.drawLine(W - 4, ch - 1, ax, ay, COL_AMBER);
        d.drawLine(4, ch - 1, ax, ay, COL_WHITE);   d.drawLine(W - 5, ch - 1, ax, ay, COL_WHITE);
    }
    if (s_now < s_muz_until) {                        // gun-port muzzle flash
        d.fillCircle(3, ch - 3, 5, COL_AMBER);   d.fillCircle(3, ch - 3, 3, COL_WHITE);
        d.fillCircle(W - 4, ch - 3, 5, COL_AMBER); d.fillCircle(W - 4, ch - 3, 3, COL_WHITE);
    }

    draw_reticle((int)s_aimx + jx, (int)s_aimy + jy, s_lock >= 0);
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

    // HUD band (shield / hull / boost / wave|ward / kills) — steady, not jittered
    d.fillRect(0, 0, W, 13, COL_SPACE);
    d.drawFastHLine(0, 13, W, rgb(40, 50, 80));
    char b[28];
    text_at(4, 3, 1, COL_CYAN, "S");
    int spct = s_shieldmax ? s_shield * 100 / s_shieldmax : 0;
    mini_bar(12, 4, 30, 6, spct, COL_CYAN);
    text_at(48, 3, 1, COL_GREY, "H");
    int hpct = g.hull_max ? g.hull * 100 / g.hull_max : 0;
    mini_bar(56, 4, 30, 6, hpct, hpct < 30 ? COL_RED : COL_GREEN);
    snprintf(b, sizeof b, "B%d", s_throttle10);
    text_at(92, 3, 1, COL_AMBER, b);
    if (s_ward_on) {
        int wpct = s_ward_max ? s_ward_hp * 100 / s_ward_max : 0;
        text_at(114, 3, 1, COL_GREEN, s_cc.type == MT_DEFEND ? tx("FAR", "BCN") : tx("CNV", "CNV"));
        mini_bar(138, 4, 28, 6, wpct, wpct < 35 ? COL_RED : COL_GREEN);
    } else {
        snprintf(b, sizeof b, "%s%d", tx("O", "W"), s_wave);
        text_at(120, 3, 1, COL_GREY, b);
    }
    snprintf(b, sizeof b, "K%d", s_kills);
    text_at(226 - (int)strlen(b) * 6, 3, 1, COL_AMBER, b);

    if (s_cmsg[0] && s_now < s_cmsg_until) center(ch / 2 - 4, 2, COL_CYAN, s_cmsg);
}

// ============================ screens: mission bay ===========================
static int s_elig[NMISSIONS];   // cached eligible-mission indices (set in draw_missions)
static void miss_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    static const char *TAG[4][2] = { { "Pattuglia", "Patrol" }, { "Taglia", "Bounty" },
                                     { "Scorta", "Escort" }, { "Difesa", "Defend" } };
    const Mission *m = cur_mission(s_elig[idx]);
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    char rew[12]; snprintf(rew, sizeof rew, "%d cr", m->reward_cr);
    if (tier == TIER_FOCUS) {
        text_at(x0, by + 2, fit_size(lp(m->name), xr - x0, 2), COL_WHITE, lp(m->name));
        text_at(x0, by + 19, 1, COL_AMBER, rew);                       // reward
        text_at(x0 + 46, by + 19, 1, COL_DIM, lp(TAG[m->type]));       // type tag
        int diff = m->waves * m->per_wave + (m->ace ? 2 : 0);          // difficulty pips, right side
        for (int s = 0; s < diff && s < 8; s++) d.fillRect(xr - 3 - (7 - s) * 5, by + 21, 3, 3, COL_RED);
    } else {
        int rw = (int)strlen(rew) * 6;
        text_vc(x0, by, bh, fit_size(lp(m->name), xr - rw - 8 - x0, 1), tier_col(tier), lp(m->name));
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
    const Mission *m = cur_mission(s_pick);
    d.fillRoundRect(6, 2, 228, 116, 6, COL_PANEL);
    d.drawRoundRect(6, 2, 228, 116, 6, COL_FOCUS2);
    text_at(14, 7, fit_size(lp(m->name), 212, 2), COL_CYAN, lp(m->name));
    draw_wrapped_n(14, 28, 212, 11, COL_WHITE, lp(m->brief), 4);       // clamp to 4 lines (y 28..68)
    char b[52];
    snprintf(b, sizeof b, "%s: %d x%d%s  vs %s", tx("Ostili", "Hostiles"),
             m->waves, m->per_wave, m->ace ? "+ASSO" : "", lp(FAC_NAME[m->foe_fac]));
    text_at(14, 74, 1, COL_GREY, b);
    if (m->offer_fac >= 0)
        snprintf(b, sizeof b, "%s %d cr (+%d/%s)  %s +%d", tx("Paga", "Pay"), m->reward_cr,
                 m->kill_cr, tx("abb", "kill"), lp(FAC_NAME[m->offer_fac]), m->rep_gain);
    else
        snprintf(b, sizeof b, "%s %d cr (+%d/%s)", tx("Paga", "Pay"), m->reward_cr, m->kill_cr, tx("abb", "kill"));
    text_at(14, 86, 1, COL_AMBER, b);
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
static void draw_title(void)
{
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    center(6, 2, COL_CYAN, "COSTELLAZIONI");                    // 13*12=156 -> x42..198
    draw_ship(W / 2 + (int)(8 * sinf(s_anim * 0.06f)), 28, 2, COL_AMBER);
    int act[4]; int n = title_items(act);
    if (s_msel >= n) s_msel = n - 1;
    list_fisheye(s_msel, n, 42, 120, title_row_fn);
}
static void set_row_fn(int idx, int bx, int by, int bw, int bh, int tier)
{
    const char *names[3] = { tx("Lingua", "Language"), tx("Audio", "Audio"),
                             tx("Cancella salvataggio", "Delete save") };
    int x0 = row_x0(bx), xr = row_xr(bx, bw);
    int vx = 150;                                               // value zone left edge
    const char *nm = names[idx];
    int sz = fit_size(nm, vx - 8 - x0, tier == TIER_FOCUS ? 2 : 1);
    text_vc(x0, by, bh, sz, tier_col(tier), nm);
    int cy = by + bh / 2;
    if (idx == 0 || idx == 1) {                                 // segmented toggle chips
        const char *a = idx == 0 ? "IT" : "ON", *b = idx == 0 ? "EN" : "OFF";
        bool left = idx == 0 ? (g_lang == 0) : (g_audio != 0);
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
    d.fillRect(0, 0, W, ch, COL_SPACE);
    stars_draw(ch);
    title_band(tx("Impostazioni", "Settings"), NULL);
    list_fisheye(s_setsel, 3, 28, 120, set_row_fn);
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
        case ST_COMBAT:   nucleo_app_set_hint(tx("Frecce mira  A/INVIO spara  Esc fuggi", "Arrows aim  A/ENTER fire  Esc flee")); break;
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
    switch (s_setsel) {
        case 0: g_lang ^= 1; cfg_write(); sfx(SFX_OK); break;
        case 1: g_audio ^= 1; cfg_write(); sfx(SFX_OK); break;
        case 2: if (s_has_save) { save_wipe(); sfx(SFX_BACK); } else sfx(SFX_DENY); break;
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
            if (k == NK_UP)   { s_setsel = (s_setsel + 2) % 3; sfx(SFX_MOVE); req(); }
            else if (k == NK_DOWN) { s_setsel = (s_setsel + 1) % 3; sfx(SFX_MOVE); req(); }
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
            if (k == NK_RIGHT)      { s_aim_hx = 1;  s_aim_h_until = s_now + 150; }
            else if (k == NK_UP)    { s_aim_vx = -1; s_aim_v_until = s_now + 150; }
            else if (k == NK_DOWN)  { s_aim_vx = 1;  s_aim_v_until = s_now + 150; }
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
            case ST_COMBAT:   if (!s_result) { s_aim_hx = -1; s_aim_h_until = s_now + 150; } break;   // aim left
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
    s_now = esp_timer_get_time() / 1000;
    bool animated = (s_screen == ST_TITLE || s_screen == ST_MAP || s_screen == ST_CINE || s_screen == ST_COMBAT);
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

static void on_enter(void)
{
    ensure_dirs();
    cfg_read();
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
}

extern "C" void nucleo_register_constellations(void)
{
    static const nucleo_app_def_t app = {
        "stelle", "Costellazioni", "Games", "Mercante stellare: riaccendi i Fari",
        'C', C_BLUE, on_enter, on_key, nullptr, on_draw, on_exit
    };
    nucleo_app_register(&app);
}
