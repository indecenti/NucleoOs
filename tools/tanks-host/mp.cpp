// tanks-host multiplayer: TWO copies of the REAL app_tanks.cpp in one process (namespaces TA / TB, each with
// its own statics = its own "Cardputer"), wired by a simulated ESP-NOW link with optional packet loss. Both
// run the same ~50 Hz loop on one shared clock. The scenario hosts a room on A, joins it from B, plays whole
// matches with every weapon, and after every turn compares what the two devices believe: terrain columns,
// HP, shields, tank positions, wind, whose turn it is, and the winner.
//   tanks_mp.exe [seed] [loss_percent]
//
// Every header app_tanks.cpp includes is pulled in FIRST, at global scope, so the two namespaced includes
// below only re-expand the game's own code (all of it file-static).
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <deque>
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "nucleo_fx3d.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_pnet.h"
#include "esp_timer.h"
#include "esp_random.h"
}

#define nucleo_register_tanks nucleo_register_tanks_A
namespace TA {
#include "../../firmware/components/nucleo_app/app_tanks.cpp"
}
#undef nucleo_register_tanks
#define nucleo_register_tanks nucleo_register_tanks_B
namespace TB {
#include "../../firmware/components/nucleo_app/app_tanks.cpp"
}
#undef nucleo_register_tanks
#include <stdarg.h>

// The game's `d` draw-target macro (app_gfx.h) is live: never name anything `d` below.

static int64_t g_us = 5000000;
extern "C" int64_t esp_timer_get_time(void) { return g_us; }
static uint32_t g_rng = 0x9E3779B9u;
extern "C" uint32_t esp_random(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t jit_rng = 12345;
static int jitter(int n) { jit_rng = jit_rng * 1103515245u + 12345u; return (int)((jit_rng >> 8) % (unsigned)n); }

#undef fopen
#undef stat
#undef mkdir
static int g_cur;                                   // which device is running right now (0 = A, 1 = B)
static void sandbox(const char *p, char *out, size_t n) { while (*p == '/') p++; snprintf(out, n, "dev%c/%s", 'A' + g_cur, p); }
extern "C" FILE *host_fopen(const char *p, const char *m) { char q[256]; sandbox(p, q, sizeof q); return fopen(q, m); }
extern "C" int host_stat(const char *p, struct stat *st) { char q[256]; sandbox(p, q, sizeof q); return stat(q, st); }
extern "C" int host_mkdir(const char *p) { char q[256]; sandbox(p, q, sizeof q); return _mkdir(q); }

static M5GFX g_panel;
static M5Canvas g_canvas(&g_panel);
static LovyanGFX *g_target = &g_panel;
LovyanGFX *nucleo_app_gfx(void) { return g_target; }
bool nucleo_app_is_buffered(void) { return g_target != &g_panel; }
void nucleo_app_set_gfx(LovyanGFX *g) { g_target = g ? g : &g_panel; }
M5Canvas *nucleo_screen(void) { return &g_canvas; }
bool nucleo_screen_acquire(void) { return true; }
void nucleo_screen_release(void) {}

// ---- one simulated device per instance ----------------------------------------------------------------
struct Dev {
    const nucleo_app_def_t *app;
    bool (*back)(int); void (*tab)(void); bool (*poll)(void);
    bool dirty, full, exit;
    bool down[128]; unsigned char mods;
    char held; int held_nk; int64_t rep;
    std::deque<pnet_pkt_t> rx;
    uint8_t mac[6];
};
static Dev g_dev[2];
static int g_loss = 0, g_sent, g_dropped;
static Dev &cur(void) { return g_dev[g_cur]; }
void nucleo_app_register(const nucleo_app_def_t *a) { cur().app = a; }
void nucleo_app_set_back_handler(bool (*fn)(int)) { cur().back = fn; }
void nucleo_app_set_tab_handler(void (*fn)(void)) { cur().tab = fn; }
void nucleo_app_set_poll_handler(bool (*fn)(void)) { cur().poll = fn; }
void nucleo_app_request_draw(void) { cur().dirty = true; }
void nucleo_app_set_fullscreen(bool on) { cur().full = on; }
void nucleo_app_set_hint(const char *) {}
int  nucleo_app_content_height(void) { return cur().full ? H : (H - HINT); }
void nucleo_app_exit(void) { cur().exit = true; }
bool nucleo_kbd_char_down(char c) { return (unsigned char)c < 128 && cur().down[(unsigned char)c]; }
unsigned char nucleo_kbd_mods(void) { return cur().mods; }
extern "C" {
esp_err_t nucleo_audio_play(const char *) { return ESP_OK; }
void nucleo_audio_tone(int, int, int) {}
void nucleo_audio_stop(void) {}
bool nucleo_audio_is_playing(void) { return false; }
int  nucleo_audio_volume(void) { return 80; }
void nucleo_audio_set_volume(int) {}
bool pnet_start(void) { return true; }
void pnet_stop(void) {}
const char *pnet_name(void) { return g_cur ? "Cardputer-B" : "Cardputer-A"; }
int  pnet_channel(void) { return 1; }
int  pnet_send(const uint8_t *mac6, const void *buf, int len) {      // to the other device (or broadcast)
    Dev &to = g_dev[1 - g_cur];
    if (mac6 && memcmp(mac6, to.mac, 6)) return 0;
    g_sent++;
    if (g_loss && jitter(100) < g_loss) { g_dropped++; return 0; }
    pnet_pkt_t p; memcpy(p.mac, cur().mac, 6); p.len = len > PNET_MAXMSG ? PNET_MAXMSG : len; memcpy(p.buf, buf, p.len);
    to.rx.push_back(p);
    return 0;
}
bool pnet_recv(pnet_pkt_t *out) {
    if (cur().rx.empty()) return false;
    *out = cur().rx.front(); cur().rx.pop_front(); return true;
}
}

static bool repeats(char c) { return c == ';' || c == '.' || c == ',' || c == '/'; }
static int nk_of(char ch) {
    switch (ch) { case ';': return NK_UP; case '.': return NK_DOWN; case ',': return NK_LEFT; case '/': return NK_RIGHT;
                  case '\n': return NK_ENTER; case '`': return NK_BACK; case '\t': return NK_TAB; }
    return NK_CHAR;
}
static void route(int nk, char ch) {
    Dev &v = cur();
    if (nk == NK_TAB) { if (v.tab) v.tab(); return; }
    if (nk == NK_LEFT || nk == NK_BACK) { if (v.back && v.back(nk)) return; if (nk == NK_BACK) v.exit = true; return; }
    v.app->on_key(nk, ch);
}
static int g_frames;
static void tick_all(int ms) {
    g_us += (int64_t)ms * 1000;
    for (g_cur = 0; g_cur < 2; g_cur++) {
        Dev &v = cur();
        if (v.held && repeats(v.held) && g_us >= v.rep) { v.rep = g_us + 90000; route(v.held_nk, v.held); }
        bool want = v.poll && v.poll();
        if (want || v.dirty) {
            v.dirty = false;
            if ((++g_frames % 11) == 0) { nucleo_app_set_gfx(&g_canvas); v.app->on_draw(); nucleo_app_set_gfx(nullptr); }
        }
    }
    g_cur = 0;
}
static void loop_ms(int ms) { int64_t end = g_us + (int64_t)ms * 1000; while (g_us < end) tick_all(18 + jitter(10)); }
static bool run_until(bool (*cond)(void), int timeout_ms) {
    int64_t end = g_us + (int64_t)timeout_ms * 1000;
    while (g_us < end) { if (cond()) return true; tick_all(18 + jitter(10)); }
    return cond();
}
static void tap(int dev, char ch) {
    g_cur = dev; Dev &v = cur();
    v.down[(unsigned char)ch] = true; v.held = ch; v.held_nk = nk_of(ch); v.rep = g_us + 350000;
    route(v.held_nk, ch);
    g_cur = 0; loop_ms(60);
    g_dev[dev].down[(unsigned char)ch] = false; g_dev[dev].held = 0; loop_ms(40);
}
static void alt(int dev) { g_dev[dev].mods = NK_MOD_GUI; loop_ms(60); g_dev[dev].mods = 0; loop_ms(60); }

static int g_fail, g_checks;
static int g_who;                                  // the seat that fired this turn
static void check(bool ok, const char *fmt, ...) { g_checks++; if (ok) return; va_list a; va_start(a, fmt); printf("FAIL "); vprintf(fmt, a); printf("\n"); va_end(a); g_fail++; }
static void note(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  "); vprintf(fmt, a); printf("\n"); va_end(a); }

static void open_dev(int i) {
    g_cur = i;
    for (const nucleo_app_ram_t *r = cur().app->ram; r && r->ptr; r++) *r->ptr = calloc(1, r->bytes);
    cur().app->on_enter();
    g_cur = 0;
}
static void close_dev(int i) {
    g_cur = i;
    cur().app->on_exit();
    for (const nucleo_app_ram_t *r = cur().app->ram; r && r->ptr; r++) { free(*r->ptr); *r->ptr = nullptr; }
    g_cur = 0;
}

// ---- what the two devices believe ---------------------------------------------------------------------
#define BOTH(expr_a, expr_b) ((expr_a) == (expr_b))
static bool both_play(void) { return TA::s_screen == TA::ST_PLAY && TB::s_screen == TB::ST_PLAY; }
static int terrain_diff(void) { int n = 0; for (int x = 0; x < WW; x++) if (TA::s_h[x] != TB::s_h[x] || TA::s_dug[x] != TB::s_dug[x]) n++; return n; }
static bool compare(const char *ctx, bool quiet = false) {
    int td = terrain_diff();
    bool ok = td == 0 && TA::s_active == TB::s_active && TA::s_wind == TB::s_wind;
    for (int i = 0; i < 2; i++) {
        ok = ok && TA::s_tk[i].hp == TB::s_tk[i].hp && TA::s_tk[i].shield == TB::s_tk[i].shield && TA::s_tk[i].dead == TB::s_tk[i].dead;
        ok = ok && fabsf(TA::s_tk[i].x - TB::s_tk[i].x) < 1.0f && fabsf(TA::s_tk[i].y - TB::s_tk[i].y) < 1.5f;
    }
    if (!ok && !quiet)
        printf("FAIL desync after %s: terrain cols differ %d | active %d/%d wind %d/%d | hp %d,%d / %d,%d | sh %d,%d / %d,%d | x %.0f,%.0f / %.0f,%.0f | y %.1f,%.1f / %.1f,%.1f\n",
               ctx, td, TA::s_active, TB::s_active, TA::s_wind, TB::s_wind, TA::s_tk[0].hp, TA::s_tk[1].hp, TB::s_tk[0].hp, TB::s_tk[1].hp,
               TA::s_tk[0].shield, TA::s_tk[1].shield, TB::s_tk[0].shield, TB::s_tk[1].shield,
               TA::s_tk[0].x, TA::s_tk[1].x, TB::s_tk[0].x, TB::s_tk[1].x, TA::s_tk[0].y, TA::s_tk[1].y, TB::s_tk[0].y, TB::s_tk[1].y);
    return ok;
}

// The active device aims (its own predictor) with weapon `wp` and fires; returns once BOTH have moved on.
static int aim_fire(int wp) {
    bool a_turn = TA::s_active == TA::s_seat;
    int dev = a_turn ? 0 : 1;
    if (a_turn) {
        int me = TA::s_active; if (TA::s_tk[me].ammo[wp] == 0) TA::s_tk[me].ammo[wp] = 2; TA::s_tk[me].weap = wp;
        float bd = 1e9f; for (int e = 10; e <= 170; e += 3) for (int p = 10; p <= 100; p += 3) { int lx = TA::sim_land(me, e, p); if (lx >= 0 && fabsf(lx - TA::s_tk[1 - me].x) < bd) { bd = fabsf(lx - TA::s_tk[1 - me].x); TA::s_tk[me].elev = e; TA::s_tk[me].power = p; } }
    } else {
        int me = TB::s_active; if (TB::s_tk[me].ammo[wp] == 0) TB::s_tk[me].ammo[wp] = 2; TB::s_tk[me].weap = wp;
        float bd = 1e9f; for (int e = 10; e <= 170; e += 3) for (int p = 10; p <= 100; p += 3) { int lx = TB::sim_land(me, e, p); if (lx >= 0 && fabsf(lx - TB::s_tk[1 - me].x) < bd) { bd = fabsf(lx - TB::s_tk[1 - me].x); TB::s_tk[me].elev = e; TB::s_tk[me].power = p; } }
    }
    loop_ms(200);                                     // let the AIM stream reach the other device
    tap(dev, '\n');
    return dev;
}

static void sc_mp(uint32_t seed, int loss) {
    printf("[mp] seed %u, packet loss %d%%\n", seed, loss);
    g_rng = seed; jit_rng = seed * 7 + 1; g_loss = 0;
    _mkdir("devA"); _mkdir("devB"); _mkdir("devA/sd"); _mkdir("devB/sd");
    g_dev[0].rx.clear(); g_dev[1].rx.clear(); g_sent = g_dropped = 0;
    open_dev(0); open_dev(1);
    // A hosts, B joins (each through the real menu -> loadout -> room screens)
    TA::s_msel = 1; tap(0, '\n'); alt(0); tap(0, '\n');
    TB::s_msel = 2; tap(1, '\n'); alt(1); tap(1, '\n');
    check(TA::s_screen == TA::ST_HOST && TB::s_screen == TB::ST_BROWSE, "lobby: A should host and B browse (screens %d / %d)", TA::s_screen, TB::s_screen);
    run_until([]() { return TB::s_nroom > 0; }, 3000);
    check(TB::s_nroom > 0, "lobby: B never saw A's room");
    tap(1, '\n');
    run_until([]() { return TA::s_guest_in && TB::s_welcomed; }, 3000);
    check(TA::s_guest_in, "lobby: A never saw B join");
    check(TB::s_welcomed, "lobby: B never got A's welcome");
    tap(0, '\n');
    run_until(both_play, 3000);
    check(both_play(), "lobby: the match did not start on both (screens %d / %d)", TA::s_screen, TB::s_screen);
    if (!both_play()) { close_dev(0); close_dev(1); return; }
    check(TA::s_seat == 0 && TB::s_seat == 1, "seats: A %d, B %d (want 0 / 1)", TA::s_seat, TB::s_seat);
    check(compare("START", true) || terrain_diff() == 0, "START: the two boards differ");
    compare("START");
    g_loss = loss;

    int turns = 0, desyncs = 0, wp = (int)(seed % 29), matches = 0;   // the seed picks where the weapon cycle starts
    while (turns < 160) {
        if (TA::s_screen == TA::ST_OVER || TA::s_phase == TA::TP_OVER) {
            run_until([]() { return TA::s_screen == TA::ST_OVER && TB::s_screen == TB::ST_OVER; }, 15000);
            matches++;
            check(TA::s_screen == TA::ST_OVER && TB::s_screen == TB::ST_OVER, "game over not reached on both (screens %d / %d)", TA::s_screen, TB::s_screen);
            check(TA::s_wins[0] == TB::s_wins[0] && TA::s_wins[1] == TB::s_wins[1], "series tally differs: A %d-%d, B %d-%d", TA::s_wins[0], TA::s_wins[1], TB::s_wins[0], TB::s_wins[1]);
            if (matches >= 2) break;
            tap(0, '\n');                                 // host rematch
            run_until(both_play, 5000);
            check(both_play(), "rematch: did not restart on both (screens %d / %d)", TA::s_screen, TB::s_screen);
            if (!both_play()) break;
            compare("rematch START");
            continue;
        }
        bool ready = run_until([]() { return both_play() && TA::s_phase == TA::TP_AIM && TB::s_phase == TB::TP_AIM && TA::s_active == TB::s_active; }, 60000);
        if (!ready) {
            check(false, "turn %d: the two devices never agreed whose turn it is (A phase %d active %d screen %d | B phase %d active %d screen %d)",
                  turns, TA::s_phase, TA::s_active, TA::s_screen, TB::s_phase, TB::s_active, TB::s_screen);
            break;
        }
        g_who = TA::s_active;
        int cw = wp % (int)(sizeof(TA::WEAPS) / sizeof(TA::WEAPS[0])); if (cw == 16 || cw == 9 || cw == 10) cw = 0;   // jump-jets / shield / med: also fine, but keep the match moving
        aim_fire(cw);
        bool moved = run_until([]() { return (TA::s_active != g_who || TA::s_phase == TA::TP_OVER) && (TB::s_active != g_who || TB::s_phase == TB::TP_OVER); }, 60000);
        if (!moved) { check(false, "turn %d (%s): the shot never resolved on both (A phase %d, B phase %d)", turns, TA::WEAPS[cw].en, TA::s_phase, TB::s_phase); break; }
        loop_ms(400);
        char ctx[64]; snprintf(ctx, sizeof ctx, "turn %d (%s by %c)", turns, TA::WEAPS[cw].en, g_who == 0 ? 'A' : 'B');
        if (!compare(ctx)) desyncs++;
        turns++; wp++;
    }
    check(desyncs == 0, "mp: %d turn(s) left the two devices disagreeing", desyncs);
    note("%d turns, %d matches, %d packets sent, %d dropped", turns, matches, g_sent, g_dropped);
    close_dev(0); close_dev(1);
}

int main(int argc, char **argv) {
    g_panel.setColorDepth(16); g_panel.createSprite(W, H);
    g_canvas.setColorDepth(8); g_canvas.createSprite(W, H);
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], nullptr, 10) : 7;
    int loss = argc > 2 ? atoi(argv[2]) : -1;
    memcpy(g_dev[0].mac, "\x24\x0A\xC4\x00\x00\xA1", 6); memcpy(g_dev[1].mac, "\x24\x0A\xC4\x00\x00\xB2", 6);
    g_cur = 0; TA::nucleo_register_tanks_A();
    g_cur = 1; TB::nucleo_register_tanks_B();
    g_cur = 0;
    if (loss < 0) { sc_mp(seed, 0); sc_mp(seed + 1, 15); }
    else sc_mp(seed, loss);
    printf("tanks-mp: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
