// tanks-host: compiles the REAL firmware/components/nucleo_app/app_tanks.cpp on the PC (MinGW + the real
// LovyanGFX core, as tools/ui-host does) and drives it like the device run loop does: a ~50 Hz loop with
// jitter, keys routed exactly as nucleo_app.cpp routes them (LEFT/BACK -> back handler, TAB -> tab handler,
// the rest -> on_key), the keyboard driver's auto-repeat (350 ms, then every 90 ms) for the arrow keys,
// physically-held keys for nucleo_kbd_char_down(), and the 8bpp shared canvas for every frame.
//
// The game's statics are visible here (the .cpp is #included), so each scenario asserts on the real state.
//   tanks_host.exe <scenario> [seed]      scenarios: aim | weapons | series | edges | frames | all
// Prints "FAIL <what>" per violation and exits 1 if any.
#include "../../firmware/components/nucleo_app/app_tanks.cpp"

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <string>

// The game's `d` draw-target macro (app_gfx.h) is live from here on: never name anything `d` below.

// ---- virtual clock + deterministic RNG --------------------------------------------------------------
static int64_t g_us = 5000000;
extern "C" int64_t esp_timer_get_time(void) { return g_us; }
static uint32_t g_rng = 0x9E3779B9u;
extern "C" uint32_t esp_random(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t jit_rng = 12345;
static int jitter(int n) { jit_rng = jit_rng * 1103515245u + 12345u; return (int)((jit_rng >> 8) % (unsigned)n); }

// ---- sandboxed file system (the absolute /sd paths land in the cwd) -----------------------------------
#undef fopen
#undef stat
#undef mkdir
static void sandbox(const char *p, char *out, size_t n) { while (*p == '/') p++; snprintf(out, n, "%s", p); }
extern "C" FILE *host_fopen(const char *p, const char *m) { char q[256]; sandbox(p, q, sizeof q); FILE *f = fopen(q, m); if (getenv("TK_FSDEBUG")) fprintf(stderr, "fopen %s %s -> %p\n", q, m, (void *)f); return f; }
extern "C" int host_stat(const char *p, struct stat *st) { char q[256]; sandbox(p, q, sizeof q); return stat(q, st); }
extern "C" int host_mkdir(const char *p) { char q[256]; sandbox(p, q, sizeof q); return _mkdir(q); }

// ---- display: a 16bpp "panel" + the 8bpp shared canvas every buffered app frame composites into --------
static M5GFX g_panel;
static M5Canvas g_canvas(&g_panel);
static LovyanGFX *g_target = &g_panel;
LovyanGFX *nucleo_app_gfx(void) { return g_target; }
bool nucleo_app_is_buffered(void) { return g_target != &g_panel; }
void nucleo_app_set_gfx(LovyanGFX *g) { g_target = g ? g : &g_panel; }
M5Canvas *nucleo_screen(void) { return &g_canvas; }
bool nucleo_screen_acquire(void) { return true; }
void nucleo_screen_release(void) {}

// ---- app framework (the slice of nucleo_app.h the game uses) ------------------------------------------
static const nucleo_app_def_t *g_app;
static bool (*g_back)(int);
static void (*g_tab)(void);
static bool (*g_poll)(void);
static bool g_dirty, g_full, g_exit;
static char g_hint[200];
void nucleo_app_register(const nucleo_app_def_t *a) { g_app = a; }
void nucleo_app_set_back_handler(bool (*fn)(int)) { g_back = fn; }
void nucleo_app_set_tab_handler(void (*fn)(void)) { g_tab = fn; }
void nucleo_app_set_poll_handler(bool (*fn)(void)) { g_poll = fn; }
void nucleo_app_request_draw(void) { g_dirty = true; }
void nucleo_app_set_fullscreen(bool on) { g_full = on; }
void nucleo_app_set_hint(const char *h) { snprintf(g_hint, sizeof g_hint, "%s", h ? h : ""); }
int  nucleo_app_content_height(void) { return g_full ? H : (H - HINT); }
void nucleo_app_exit(void) { g_exit = true; }

// ---- audio / radio stubs (counted, never played) ------------------------------------------------------
static int g_tones, g_plays;
extern "C" {
esp_err_t nucleo_audio_play(const char *) { g_plays++; return ESP_OK; }
void nucleo_audio_tone(int, int, int) { g_tones++; }
void nucleo_audio_stop(void) {}
bool nucleo_audio_is_playing(void) { return false; }
int  nucleo_audio_volume(void) { return 80; }
void nucleo_audio_set_volume(int) {}
bool pnet_start(void) { return true; }
void pnet_stop(void) {}
const char *pnet_name(void) { return "host"; }
int  pnet_channel(void) { return 1; }
int  pnet_send(const uint8_t *, const void *, int) { return 0; }
bool pnet_recv(pnet_pkt_t *) { return false; }
}

// ---- keyboard: the driver's auto-repeat + physically-held state ---------------------------------------
static bool g_down[128];
static unsigned char g_mods;
bool nucleo_kbd_char_down(char c) { return (unsigned char)c < 128 && g_down[(unsigned char)c]; }
unsigned char nucleo_kbd_mods(void) { return g_mods; }
static char g_held = 0; static int g_held_nk = 0; static int64_t g_rep_us = 0;
static bool repeats(char c) { return c == ';' || c == '.' || c == ',' || c == '/' || c == '\b'; }
static void route(int nk, char ch) {                    // nucleo_app.cpp's routing for a foreground app
    if (nk == NK_TAB) { if (g_tab) g_tab(); return; }
    if (nk == NK_LEFT || nk == NK_BACK) { if (g_back && g_back(nk)) return; if (nk == NK_BACK) g_exit = true; return; }
    if (g_app->on_key) g_app->on_key(nk, ch);
}
static int nk_of(char ch) {
    switch (ch) { case ';': return NK_UP; case '.': return NK_DOWN; case ',': return NK_LEFT; case '/': return NK_RIGHT;
                  case '\n': return NK_ENTER; case '`': return NK_BACK; case '\t': return NK_TAB; case '\b': return NK_DEL; }
    return NK_CHAR;
}
static void key_down(char ch) {
    g_down[(unsigned char)ch] = true; g_held = ch; g_held_nk = nk_of(ch); g_rep_us = g_us + 350000;
    route(g_held_nk, ch);
}
static void key_up(char ch) { g_down[(unsigned char)ch] = false; if (g_held == ch) g_held = 0; }

// ---- the run loop -------------------------------------------------------------------------------------
static int g_frames, g_draws;
static bool g_render_all = false;
static void draw_frame(void) {
    nucleo_app_set_gfx(&g_canvas);
    g_app->on_draw();
    nucleo_app_set_gfx(nullptr);
    if (!g_full) {                                       // the device's footer covers these rows (launcher_render_hint_bar:
        int y = H - HINT, len = (int)strlen(g_hint);     // centred, cut at 39 chars), so the frames show what the user sees
        if (len > 39) len = 39;
        g_canvas.fillRect(0, y, W, HINT, 0x0000); g_canvas.drawFastHLine(0, y, W, 0x4208);
        char b[40]; memcpy(b, g_hint, len); b[len] = 0;
        int x = (W - len * 6) / 2; if (x < 4) x = 4;
        g_canvas.setTextSize(1); g_canvas.setTextColor(0xFFFF, 0x0000); g_canvas.setCursor(x, y + 3); g_canvas.print(b);
    }
    g_draws++;
}
static void tick(int ms) {
    g_us += (int64_t)ms * 1000;
    if (g_held && repeats(g_held) && g_us >= g_rep_us) { g_rep_us = g_us + 90000; route(g_held_nk, g_held); }
    bool want = g_poll && g_poll();
    if (want || g_dirty) {
        g_dirty = false; g_frames++;
        if (g_render_all || (g_frames % 7) == 0) draw_frame();   // every 7th frame is drawn: draw code runs on live state
    }
}
static void loop_ms(int ms) { int64_t end = g_us + (int64_t)ms * 1000; while (g_us < end) tick(18 + jitter(10)); }
static bool run_until(bool (*cond)(void), int timeout_ms) {
    int64_t end = g_us + (int64_t)timeout_ms * 1000;
    while (g_us < end) { if (cond()) return true; tick(18 + jitter(10)); }
    return cond();
}
static void tap(char ch, int hold_ms = 60) { key_down(ch); loop_ms(hold_ms); key_up(ch); loop_ms(40); }

// ---- reporting ----------------------------------------------------------------------------------------
static int g_fail, g_checks;
static void fail(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("FAIL "); vprintf(fmt, a); printf("\n"); va_end(a); g_fail++; }
static void check(bool ok, const char *fmt, ...) { g_checks++; if (ok) return; va_list a; va_start(a, fmt); printf("FAIL "); vprintf(fmt, a); printf("\n"); va_end(a); g_fail++; }
static void note(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  "); vprintf(fmt, a); printf("\n"); va_end(a); }

static void dump(const char *name) {
    draw_frame();
    char p[256]; snprintf(p, sizeof p, "shots/%s.rgb", name);
    FILE *f = fopen(p, "wb"); if (!f) { fail("cannot write %s", p); return; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t c = (uint16_t)g_canvas.readPixel(x, y);
        unsigned char px[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

// ---- app lifecycle ------------------------------------------------------------------------------------
static void open_app(uint32_t seed) {
    g_rng = seed ? seed : 1; jit_rng = seed * 7 + 1;
    for (const nucleo_app_ram_t *r = g_app->ram; r && r->ptr; r++) { *r->ptr = calloc(1, r->bytes); }
    g_back = nullptr; g_tab = nullptr; g_poll = nullptr; g_exit = false; g_full = false;
    memset(g_down, 0, sizeof g_down); g_held = 0; g_mods = 0;
    g_app->on_enter();
}
static void close_app(void) {
    g_app->on_exit();
    for (const nucleo_app_ram_t *r = g_app->ram; r && r->ptr; r++) { free(*r->ptr); *r->ptr = nullptr; }
}
static bool in_play(void) { return s_screen == ST_PLAY; }
static bool my_aim(void) { return s_screen == ST_PLAY && s_phase == TP_AIM && s_active == 0; }
static bool cpu_aim_or_over(void) { return s_screen == ST_OVER || (s_screen == ST_PLAY && s_active == 1 && (s_phase == TP_TURN || s_phase == TP_AIM)) || (s_screen == ST_PLAY && s_phase == TP_OVER); }

// Menu -> "vs CPU" -> loadout (ALT = random 10) -> ENTER: the real key path into a match.
static bool start_cpu_match(void) {
    s_msel = 0; tap('\n');
    if (s_screen != ST_LOADOUT) { fail("menu ENTER did not open the loadout picker (screen %d)", s_screen); return false; }
    g_mods = NK_MOD_GUI; loop_ms(60); g_mods = 0; loop_ms(60);
    check(ld_count() == LOADOUT_N, "ALT should pick a full random loadout, got %d", ld_count());
    tap('\n');
    return run_until(in_play, 2000);
}
static void world_sane(const char *ctx) {
    for (int x = 0; x < WW; x++) if (s_h[x] < GTOP || s_h[x] > H - 1) { fail("%s: terrain column %d out of range (%d)", ctx, x, s_h[x]); break; }
    for (int i = 0; i < 2; i++) {
        if (!(s_tk[i].x >= 0 && s_tk[i].x < WW) || !isfinite(s_tk[i].y) || s_tk[i].y > H || s_tk[i].y < GTOP - 40)
            fail("%s: tank %d at (%.1f, %.1f) is off the world", ctx, i, s_tk[i].x, s_tk[i].y);
        if (s_tk[i].hp < 0 || s_tk[i].hp > 100) fail("%s: tank %d hp %d", ctx, i, s_tk[i].hp);
        if (s_tk[i].elev < ELEV_MIN || s_tk[i].elev > ELEV_MAX || s_tk[i].power < 5 || s_tk[i].power > 100)
            fail("%s: tank %d aim out of range (elev %d power %d)", ctx, i, s_tk[i].elev, s_tk[i].power);
    }
}

// ============================ scenarios =================================================================
// 1) aiming: a tap moves EXACTLY one step whatever the loop timing; a held key ramps; releasing stops it.
static void sc_aim(uint32_t seed) {
    printf("[aim] seed %u\n", seed);
    open_app(seed);
    if (!start_cpu_match()) { close_app(); return; }
    int axes[4][2] = { { ';', 0 }, { '.', 0 }, { '/', 1 }, { ',', 1 } };
    int sign[4] = { +1, -1, +1, -1 };
    for (int a = 0; a < 4; a++) {
        int bad = 0, tot = 0;
        for (int n = 0; n < 24; n++) {
            if (!run_until(my_aim, 90000)) { fail("aim: my turn never came back"); close_app(); return; }
            int me = s_active;
            s_tk[me].elev = 90; s_tk[me].power = 50;
            int before = axes[a][1] ? s_tk[me].power : s_tk[me].elev;
            tap((char)axes[a][0], 30 + jitter(260));            // a quick tap: shorter than the 350 ms repeat delay
            loop_ms(300);
            int after = axes[a][1] ? s_tk[me].power : s_tk[me].elev;
            tot++; if (after - before != sign[a]) { bad++; if (bad <= 3) note("tap '%c' moved %+d (want %+d)", axes[a][0], after - before, sign[a]); }
        }
        check(bad == 0, "aim: %d of %d single taps of '%c' did not move exactly one step", bad, tot, axes[a][0]);
    }
    // hold: ramps, then stops the moment the key is released
    for (int a = 0; a < 4; a++) {
        if (!run_until(my_aim, 90000)) { fail("aim: my turn never came back"); break; }
        int me = s_active; s_tk[me].elev = 90; s_tk[me].power = 50;
        key_down((char)axes[a][0]); loop_ms(1200);
        int *v = axes[a][1] ? &s_tk[me].power : &s_tk[me].elev;
        int at_release = *v; key_up((char)axes[a][0]); loop_ms(700);
        int moved = at_release - (axes[a][1] ? 50 : 90);
        check(moved * sign[a] >= 10, "aim: holding '%c' 1.2 s moved only %+d", axes[a][0], moved);
        check(*v == at_release, "aim: '%c' kept moving %+d after release", axes[a][0], *v - at_release);
    }
    // bounds
    if (run_until(my_aim, 90000)) {
        int me = s_active;
        key_down(';'); loop_ms(6000); key_up(';'); loop_ms(200); check(s_tk[me].elev == ELEV_MAX, "aim: elevation tops out at %d (want %d)", s_tk[me].elev, ELEV_MAX);
        key_down('.'); loop_ms(6000); key_up('.'); loop_ms(200); check(s_tk[me].elev == ELEV_MIN, "aim: elevation bottoms out at %d (want %d)", s_tk[me].elev, ELEV_MIN);
        key_down('/'); loop_ms(4000); key_up('/'); loop_ms(200); check(s_tk[me].power == 100, "aim: power tops out at %d", s_tk[me].power);
        key_down(','); loop_ms(4000); key_up(','); loop_ms(200); check(s_tk[me].power == 5, "aim: power bottoms out at %d", s_tk[me].power);
    }
    world_sane("aim");
    close_app();
}

// Best (elev, power) for `who` to land a plain ballistic shell on the foe.
static void best_aim(int who, int *be, int *bp) {
    float bd = 1e9f; *be = 45; *bp = 60;
    for (int e = 10; e <= 170; e += 2) for (int p = 10; p <= 100; p += 2) {
        int lx = sim_land(who, e, p); if (lx < 0) continue;
        float dd = fabsf(lx - s_tk[1 - who].x); if (dd < bd) { bd = dd; *be = e; *bp = p; }
    }
}

// 2) every weapon, aimed and at extreme aims: the turn always resolves and the world stays sane.
static void sc_weapons(uint32_t seed) {
    printf("[weapons] seed %u\n", seed);
    struct Aim { int e, p; } aims[] = { { -1, -1 }, { 5, 100 }, { 89, 100 }, { 175, 60 }, { 45, 5 }, { 60, 35 } };
    int stuck = 0;
    for (int wp = 0; wp < NWEAP; wp++) {
        open_app(seed + wp * 131);
        if (!start_cpu_match()) { close_app(); continue; }
        for (unsigned k = 0; k < sizeof aims / sizeof aims[0]; k++) {
            if (!run_until(my_aim, 120000)) { if (s_screen == ST_OVER || s_phase == TP_OVER) break; fail("weapons: wp %d: my turn never came (screen %d phase %d active %d)", wp, s_screen, s_phase, s_active); stuck++; break; }
            int me = s_active;
            s_tk[me].ammo[wp] = s_tk[me].ammo[wp] == 0 ? 3 : s_tk[me].ammo[wp]; s_tk[me].weap = wp;
            if (aims[k].e < 0) best_aim(me, &s_tk[me].elev, &s_tk[me].power); else { s_tk[me].elev = aims[k].e; s_tk[me].power = aims[k].p; }
            int hp0 = s_tk[0].hp, hp1 = s_tk[1].hp, e = s_tk[me].elev, p = s_tk[me].power;
            tap('\n');
            check(s_phase != TP_AIM || s_active != me, "weapons: wp %d (%s): ENTER did not fire", wp, WEAPS[wp].en);
            if (!run_until(cpu_aim_or_over, 30000)) {
                fail("weapons: wp %d (%s) e%d p%d: turn never resolved (phase %d, proj %d, ufo %d, dwell %d)", wp, WEAPS[wp].en, e, p, s_phase, any_proj(), s_ufo.on, s_dwell_t);
                stuck++; break;
            }
            world_sane(WEAPS[wp].en);
            if (k == 0 && aims[k].e < 0 && WEAPS[wp].dmg > 0 && WEAPS[wp].beh != WB_TELE && s_tk[1].hp == hp1 && hp1 > 0)
                note("aimed %s (e%d p%d) did no damage to the CPU (my hp %d->%d)", WEAPS[wp].en, e, p, hp0, s_tk[0].hp);
            if (s_screen == ST_OVER || s_phase == TP_OVER) break;
        }
        close_app();
    }
    check(stuck == 0, "weapons: %d weapon(s) left a turn stuck", stuck);
}

// 3) whole best-of series vs the CPU (perfect human aim): a series always ends, and the CPU lands hits.
static void sc_series(uint32_t seed) {
    printf("[series] seed %u\n", seed);
    for (int diff = 0; diff < 3; diff++) {
        int cpu_shots = 0, cpu_hits = 0, matches = 0, turns = 0;
        open_app(seed + diff);
        s_diff = diff;
        if (!start_cpu_match()) { close_app(); continue; }
        int64_t t0 = g_us;
        while (g_us - t0 < (int64_t)3600 * 1000000) {
            if (s_screen == ST_OVER) {
                matches++;
                if (s_wins[0] >= SERIES_TGT || s_wins[1] >= SERIES_TGT) break;
                tap('\n'); run_until(in_play, 3000); continue;
            }
            if (my_aim()) {
                int me = s_active; s_tk[me].weap = 0; best_aim(me, &s_tk[me].elev, &s_tk[me].power);
                s_tk[me].power = clampi(s_tk[me].power + (jitter(9) - 4), 5, 100);   // a human-ish small error
                tap('\n'); turns++; continue;
            }
            if (s_screen == ST_PLAY && s_active == 1 && s_phase == TP_AIM) {
                int hp = s_tk[0].hp; cpu_shots++;
                run_until([]() { return !(s_screen == ST_PLAY && s_active == 1 && s_phase != TP_TURN) || s_screen != ST_PLAY; }, 60000);
                if (s_tk[0].hp < hp) cpu_hits++;
                turns++; continue;
            }
            tick(18 + jitter(10));
            if (turns > 400) break;
        }
        check(s_wins[0] >= SERIES_TGT || s_wins[1] >= SERIES_TGT, "series diff %d: not decided after %d turns / %d matches (wins %d-%d)", diff, turns, matches, s_wins[0], s_wins[1]);
        note("diff %d: %d matches, %d turns, score %d-%d, CPU hit %d/%d shots", diff, matches, turns, s_wins[0], s_wins[1], cpu_hits, cpu_shots);
        world_sane("series");
        close_app();
    }
}

// 4) edges: menus, options, help, scores, leave-match confirm, game-over keys, reopen keeps settings.
static void sc_edges(uint32_t seed) {
    printf("[edges] seed %u\n", seed);
    open_app(seed);
    check(strlen(g_hint) <= 40, "hint '%s' is %d chars: wider than the 240 px footer (40 chars)", g_hint, (int)strlen(g_hint));
    for (int i = 0; i < 6; i++) { s_msel = i; tap('\n'); if (s_screen == ST_LOADOUT) check(strlen(g_hint) <= 40, "loadout hint is %d chars (footer fits 40): '%s'", (int)strlen(g_hint), g_hint);
        if (s_screen == ST_OPT) { for (int k = 0; k < 8; k++) { tap('.'); tap('\n'); tap('\n'); } }
        tap('`'); check(s_screen == ST_MENU, "Esc from menu item %d did not return to the menu (screen %d)", i, s_screen); }
    if (start_cpu_match()) {
        tap('`'); check(s_confirm, "first Esc in a match should ask to confirm leaving");
        tap('`'); check(!s_confirm && s_screen == ST_PLAY, "second Esc should cancel the confirm and stay");
        tap('\t'); check(s_screen == ST_OPT, "TAB in a match should open the settings overlay");
        tap('\t'); check(s_screen == ST_PLAY, "TAB again should return to the match");
        tap('`'); tap('\n'); check(s_screen == ST_MENU, "confirm + ENTER should leave the match");
    }
    g_lang = 1; s_diff = 2; cfg_write();                     // settings survive a close + reopen
    close_app();
    g_lang = 0; s_diff = 1;
    open_app(seed + 1);
    check(g_lang == 1 && s_diff == 2, "settings did not survive a reopen (lang %d diff %d)", g_lang, s_diff);
    g_lang = 0; cfg_write();
    close_app();
}

// 5) frames for a visual review (tools/tanks-host/run.mjs turns them into PNGs).
static void sc_frames(uint32_t seed) {
    printf("[frames] seed %u\n", seed);
    open_app(seed);
    dump("00_menu");
    s_msel = 0; tap('\n'); dump("01_loadout_empty");
    g_mods = NK_MOD_GUI; loop_ms(60); g_mods = 0; loop_ms(60); dump("02_loadout_full");
    tap('\n'); run_until(in_play, 2000);
    run_until(my_aim, 90000); loop_ms(400); dump("03_aim_hud");
    loop_ms(3500); dump("04_aim_idle");
    int me = s_active; best_aim(me, &s_tk[me].elev, &s_tk[me].power); s_tk[me].weap = 0;
    tap('\n'); loop_ms(300); dump("05_shell_flight");
    run_until([]() { return s_phase == TP_SETTLE; }, 20000); dump("06_impact");
    run_until([]() { return s_active == 1 && s_phase == TP_AIM; }, 20000); loop_ms(500); dump("07_cpu_aim");
    tap('\t'); dump("08_options"); tap('\t');
    tap('`'); dump("09_leave_confirm"); tap('`');
    for (int a = 0; a < ATM_N; a++) { s_seed = (uint32_t)a; gen_terrain(); s_cam = 0; dump((std::string("10_atmo_") + (char)('0' + a)).c_str()); }
    close_app();
}

// 6) physics: the shared predictor (CPU aim, aim-help reticle, alien-ship lock) must agree with the real flight.
static void sc_physics(uint32_t seed) {
    printf("[physics] seed %u\n", seed);
    open_app(seed);
    if (!start_cpu_match()) { close_app(); return; }
    int n = 0, far = 0; float worst = 0, sum = 0;
    for (int k = 0; k < 40; k++) {
        if (!run_until(my_aim, 120000)) break;
        if (s_screen == ST_OVER) break;
        int me = s_active;
        s_tk[me].weap = 0; s_tk[me].elev = 15 + jitter(150); s_tk[me].power = 20 + jitter(81);
        s_tk[1 - me].hp = 100; s_tk[me].hp = 100;                // keep the match alive for the sampling
        int pred = sim_land(me, s_tk[me].elev, s_tk[me].power);
        s_lava_x = -1;
        tap('\n');
        run_until([]() { return s_phase == TP_SETTLE || s_phase == TP_OVER || s_active != 0; }, 20000);
        if (pred < 0 || s_lava_x < 0) continue;                  // flew off the field: nothing to compare
        float err = fabsf((float)(pred - s_lava_x)); n++; sum += err; if (err > worst) worst = err;
        if (err > TANK_W / 2 + 2) {                              // beyond the hitbox half-width
            far++; note("e%d p%d wind %d: predicted %d, real impact %d (me x%.0f, foe x%.0f)", s_tk[me].elev, s_tk[me].power, s_wind, pred, s_lava_x, s_tk[me].x, s_tk[1 - me].x);
        }
    }
    note("predictor vs real flight: %d shots, mean error %.1f px, worst %.1f px, %d off by more than the hitbox", n, n ? sum / n : 0, worst, far);
    check(far == 0, "physics: %d of %d predicted landings are >10 px from the real impact (worst %.0f px)", far, n, worst);
    close_app();
}

// 7) CPU accuracy per difficulty: many CPU shots (the human throws harmless shots, HP reset each turn).
static void sc_cpu(uint32_t seed) {
    printf("[cpu] seed %u\n", seed);
    int lo[3] = { 7, 22, 38 }, hi[3] = { 30, 46, 66 };            // hit-rate bands: easy / normal / hard
    for (int diff = 0; diff < 3; diff++) {
        int shots = 0, hits = 0;
        for (int m = 0; m < 8; m++) {
            open_app(seed + diff * 1000 + m * 17); s_diff = diff;
            if (!start_cpu_match()) { close_app(); continue; }
            for (int t = 0; t < 25; t++) {
                bool ok = run_until([]() { return s_screen != ST_PLAY || s_phase == TP_AIM; }, 60000);
                if (!ok || s_screen != ST_PLAY) break;
                s_tk[0].hp = s_tk[1].hp = 100; s_tk[0].shield = s_tk[1].shield = 0; memset(s_fire, 0, sizeof s_fire);
                if (s_active == 0) {                             // harmless: a light lob straight back over my shoulder
                    s_tk[0].weap = 0; s_tk[0].elev = 120; s_tk[0].power = 25; tap('\n');
                    run_until([]() { return s_active == 1 || s_screen != ST_PLAY; }, 20000);
                    continue;
                }
                shots++;
                run_until([]() { return s_active == 0 || s_screen != ST_PLAY || s_phase == TP_OVER; }, 60000);
                if (s_tk[0].hp < 100) hits++;
            }
            close_app();
        }
        int pct = shots ? hits * 100 / shots : 0;
        note("difficulty %d: CPU hit %d/%d = %d%%", diff, hits, shots, pct);
        check(pct >= lo[diff] && pct <= hi[diff], "cpu: difficulty %d hits %d%%, outside the %d..%d%% band", diff, pct, lo[diff], hi[diff]);
    }
}

int main(int argc, char **argv) {
    g_panel.setColorDepth(16); g_panel.createSprite(W, H);
    g_canvas.setColorDepth(8); g_canvas.createSprite(W, H);
    _mkdir("shots"); _mkdir("sd");                         // the device always has /sd mounted
    nucleo_register_tanks();
    const char *sc = argc > 1 ? argv[1] : "all";
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 7;
    bool all = !strcmp(sc, "all");
    if (all || !strcmp(sc, "aim"))     { sc_aim(seed); sc_aim(seed + 100); }
    if (all || !strcmp(sc, "weapons")) sc_weapons(seed);
    if (all || !strcmp(sc, "series"))  sc_series(seed);
    if (all || !strcmp(sc, "edges"))   sc_edges(seed);
    if (all || !strcmp(sc, "physics")) sc_physics(seed);
    if (all || !strcmp(sc, "cpu"))     sc_cpu(seed);
    if (all || !strcmp(sc, "frames"))  sc_frames(seed);
    printf("tanks-host: %d checks, %d failed (%d frames, %d drawn, %d tones)\n", g_checks + g_fail, g_fail, g_frames, g_draws, g_tones);
    return g_fail ? 1 : 0;
}
