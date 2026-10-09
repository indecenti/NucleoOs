// Snake Duel scenarios for the native-games host gate (tools/native-host/run.mjs snake).
// Every screen in three languages and clear of the footer, food never inside a rock, start lanes free of
// rocks, a body that stays connected when it grows, the 3-2-1 start, pause, one turn per tap, the CPU's
// survival (dead-end avoidance), persisted stats, no SFX synthesis on launch.
#define NH_GAME_NAME "snake"
#include "../../../firmware/components/nucleo_app/app_snake.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_snake(); }

static int below_footer(void) {
    nh_canvas.fillScreen(0xF81F); uint32_t sent = nh_canvas.readPixel(0, H - 1);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int n = 0;
    for (int y = H - HINT; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != sent) n++;
    return n;
}
static void screen(const char *name) {
    nh_loop_ms(300);
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s: %d px drawn under the footer", name, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static void menu_item(int i) { s_menu.sel = (int8_t)i; s_menu.pos = i; nh_tap('\n'); }
static bool started(void) { return esp_timer_get_time() >= s_go_us; }

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        screen("menu");
        menu_item(1); screen("host"); nh_tap('`');
        menu_item(2); screen("browse"); nh_tap('`');
        menu_item(3); screen("scores"); nh_tap('`');
        menu_item(4); screen("help"); nh_tap('\t'); screen("help2"); nh_tap('`');
        nh_check(s_st == ST_MENU, "Esc from a sub-screen did not return to the menu");
        menu_item(0);
        nh_check(s_st == ST_PLAY && nh_full, "menu row 0 did not start a fullscreen match");
        nh_loop_ms(500); { char b[32]; snprintf(b, sizeof b, "ready_%s", g_nh.lang); nh_dump(b); }
        nh_run_until(started, 3000);
        s_s1.pu = PU_SHIELD; s_s1.pu_t = 20; s_s2.pu = PU_SPEED; s_s2.pu_t = 6; snake_grow(s_s1, 8);
        s_pu_type = PU_GHOST; s_pu_x = s_s1.bx[0] + 6; s_pu_y = s_s1.by[0] - 2; s_pu_life = 20;
        s_fx = s_s1.bx[0] + 9; s_fy = s_s1.by[0] + 2; s_fx2 = s_s1.bx[0] + 3; s_fy2 = s_s1.by[0] + 3;
        nh_tap('w'); nh_loop_ms(600); nh_tap('d'); nh_loop_ms(250);
        { char b[32]; snprintf(b, sizeof b, "play_%s", g_nh.lang); nh_dump(b); }
        nh_tap('`'); screen("pause");
        nh_tap('\n');
        s_s1.pu = PU_NONE; s_s2.alive = false; nh_loop_ms(400);
        nh_check(s_st == ST_OVER, "a dead CPU did not end the match");
        screen("over");
        nh_close_app();
    }
    g_nh.lang = "it";
}

// Food and power-ups never spawn inside a rock (it could never be eaten).
static void scen_food(uint32_t seed) {
    nh_open_app(seed);
    int bad = 0;
    for (int k = 0; k < 150; k++) {
        s_mode = MODE_AI; start_game(seed * 977 + k);
        for (int i = 0; i < 40; i++) { spawn_food(); spawn_food2(); spawn_pu();
            if (is_obstacle(s_fx, s_fy) || is_obstacle(s_fx2, s_fy2) || is_obstacle(s_pu_x, s_pu_y)) bad++; }
    }
    nh_check(bad == 0, "food: %d items spawned inside a rock", bad);
    nh_close_app();
}

// The first 10 cells ahead of each snake are always free (a rock there killed it before the first turn).
static void scen_lanes(uint32_t seed) {
    nh_open_app(seed);
    int bad = 0;
    for (int k = 0; k < 400; k++) {
        s_mode = MODE_AI; start_game(seed * 131 + k);
        for (int i = 0; i <= 10; i++) {
            if (is_obstacle(12 + i, WORLD_H / 2)) bad++;
            if (is_obstacle(WORLD_W - 13 - i, WORLD_H / 2)) bad++;
            for (int j = 0; j < 5; j++) { if (is_obstacle(12 - j, WORLD_H / 2) || is_obstacle(WORLD_W - 13 + j, WORLD_H / 2)) bad++; }
        }
    }
    nh_check(bad == 0, "lanes: %d rock cells on a snake's body or its first 10 cells", bad);
    nh_close_app();
}

// After a SHORT power-up and a meal the body is still one connected chain.
static bool connected(const Snake &s) {
    for (int i = 1; i < s.len; i++) if (abs(s.bx[i] - s.bx[i - 1]) + abs(s.by[i] - s.by[i - 1]) > 1) return false;
    return true;
}
static void scen_grow(uint32_t seed) {
    nh_open_app(seed); menu_item(0); nh_run_until(started, 3000);
    s_s2.alive = true; s_s2.bx[0] = WORLD_W - 3; s_s2.by[0] = 3;          // park the CPU out of the way
    // a long snake doubling back on itself, then SHORT, then food right ahead
    Snake &s = s_s1; s.len = 0;
    for (int i = 0; i < 20; i++) { s.bx[s.len] = (int8_t)(30 - i); s.by[s.len] = (int8_t)(WORLD_H / 2); s.len++; }
    s.dir = s.next_dir = DRT; s.inq_n = 0; s.pu = PU_NONE;
    apply_pu(s, s_s2, PU_SHORT);
    s_fx = s.bx[0] + 1; s_fy = s.by[0]; s_fx2 = 1; s_fy2 = 1;
    int64_t now = esp_timer_get_time(); s.move_next_us = now;
    game_step(now);
    nh_check(connected(s), "grow: the body broke apart after SHORT + food (stale segment exposed, len %d)", s.len);
    nh_close_app();
}

// 3-2-1: nobody moves before the start; the snake moves right after.
static void scen_ready(uint32_t seed) {
    nh_open_app(seed); menu_item(0);
    int8_t x0 = s_s1.bx[0];
    nh_loop_ms((int)READY_MS - 150);
    nh_check(s_s1.bx[0] == x0, "ready: the snake moved during the countdown");
    nh_loop_ms(600);
    nh_check(s_s1.bx[0] > x0, "ready: the snake did not start after the countdown");
    nh_close_app();
}

// Esc pauses (frozen vs CPU), ENTER resumes, Esc Esc leaves, Esc at the menu closes.
static void scen_pause(uint32_t seed) {
    nh_open_app(seed); menu_item(0); nh_run_until(started, 3000);
    nh_tap('`');
    nh_check(s_st == ST_PLAY && s_paused, "pause: Esc did not pause");
    int8_t x = s_s1.bx[0]; nh_loop_ms(1500);
    nh_check(s_s1.bx[0] == x, "pause: the snake moved while paused");
    nh_tap('\n'); nh_loop_ms(400);
    nh_check(!s_paused && s_s1.bx[0] != x, "pause: ENTER did not resume");
    nh_tap('`'); nh_tap('`');
    nh_check(s_st == ST_MENU, "pause: Esc Esc did not leave to the menu");
    nh_tap('`');
    nh_check(nh_exit, "Esc at the menu did not close the app");
    nh_close_app();
}

// A tap turns once; holding an arrow (auto-repeat) never queues extra turns.
static void scen_input(uint32_t seed) {
    nh_open_app(seed); menu_item(0);
    nh_tap(';', 40, 10);
    nh_check(s_s1.inq_n == 1 && s_s1.inq[0] == DUP, "input: a tap queued %d turns", s_s1.inq_n);
    s_s1.inq_n = 0;
    nh_key_down('.'); nh_loop_ms(1000); nh_key_up('.');      // hold DOWN through the auto-repeat
    nh_check(s_s1.inq_n <= 1, "input: a held key queued %d turns", s_s1.inq_n);
    nh_close_app();
}

// CPU vs CPU (the same AI drives the green snake): matches last, and few deaths are self-inflicted.
static int8_t p1_ai(void) {
    Snake t = s_snk[0]; s_snk[0] = s_snk[1]; s_snk[1] = t;
    int8_t dir = ai_choose();
    t = s_snk[0]; s_snk[0] = s_snk[1]; s_snk[1] = t;
    return dir;
}
static void scen_ai(uint32_t seed) {
    nh_open_app(seed);
    int games = 30, steps = 0, self = 0;
    for (int g = 0; g < games; g++) {
        s_mode = MODE_AI; start_game(seed * 7919 + g * 31);
        s_go_us = 0; s_s1.move_next_us = s_s2.move_next_us = 0;
        int t = 0;
        for (; t < 6000 && s_st == ST_PLAY; t++) {
            s_s1.next_dir = p1_ai(); s_s1.inq_n = 0;
            int8_t h1x = s_s1.bx[0], h1y = s_s1.by[0], h2x = s_s2.bx[0], h2y = s_s2.by[0];
            nh_loop_ms(1);
            if (s_st != ST_PLAY) {                     // who died, and into what
                for (int k = 0; k < 2; k++) {
                    const Snake &s = s_snk[k]; if (s.alive) continue;
                    int hx = k ? h2x : h1x, hy = k ? h2y : h1y, nx = hx + DX[s.dir], ny = hy + DY[s.dir];
                    const Snake &o = s_snk[k ^ 1];
                    bool by_rival = snake_has(o, (int8_t)nx, (int8_t)ny) || (abs(nx - o.bx[0]) + abs(ny - o.by[0]) <= 1);
                    if (!by_rival) self++;
                }
            }
        }
        steps += s_tick;
    }
    nh_note("ai: CPU vs CPU, %.0f moves per match, %d self-inflicted deaths in %d matches", steps / (float)games, self, games);
    nh_check(self <= games / 5, "ai: %d of %d matches ended with a snake crashing into a wall or itself", self, games);
    nh_close_app();
}

// Wins and the food record survive closing the app.
static void scen_stats(uint32_t seed) {
    host_remove(STATS_PATH);
    nh_open_app(seed); menu_item(0); nh_run_until(started, 3000);
    s_s1.score = 7; s_hisc = 7; s_s2.alive = false; nh_loop_ms(300);
    nh_check(s_st == ST_OVER && s_wins1 == 1, "stats: a win was not counted (%d)", s_wins1);
    nh_close_app();
    nh_open_app(seed);
    nh_check(s_wins1 == 1 && s_hisc == 7, "stats: wins %d / record %d were not kept across a relaunch", s_wins1, s_hisc);
    nh_close_app();
}


static int sfx_cached(void) {
    int n = 0; struct stat st;
    for (int id = 1; id <= SX_COUNT; id++) { char p[96]; snprintf(p, sizeof p, "/sd/data/snake" "/sfx/%s.wav", SFX_NAME[id]); if (host_stat(p, &st) == 0) n++; }
    return n;
}
// The pack is the sound: every cue resolves to it, and nothing is ever synthesized on the device (launch,
// menus or play). Without a pack a cue degrades to a tone, still without synthesis.
static void scen_sfx(uint32_t seed) {
    int packed = 0; struct stat st;
    for (int id = 1; id <= SX_COUNT; id++) { char p[96]; snprintf(p, sizeof p, "/sd/data/snake" "/pack/%s.wav", SFX_NAME[id]); if (host_stat(p, &st) == 0 && st.st_size > 44) packed++; }
    nh_check(packed == SX_COUNT, "sfx: the pack covers %d/%d cues", packed, SX_COUNT);
    nh_open_app(seed);
    int plays = nh_plays; SFX(SX_WIN); nh_loop_ms(3000);
    nh_check(nh_plays > plays, "sfx: a cue with its WAV in the pack did not play it");
    nh_check(sfx_cached() == 0, "sfx: %d cues synthesized on the device", sfx_cached());
    nh_close_app();
    host_rename("/sd/data/snake" "/pack", "/sd/data/snake" "/pack_off");
    nh_open_app(seed);
    int tones = nh_tones; SFX(SX_WIN); nh_loop_ms(3000);
    nh_check(nh_tones > tones, "sfx: without a pack a cue was silent (no tone fallback)");
    nh_check(sfx_cached() == 0, "sfx: %d cues synthesized without a pack", sfx_cached());
    nh_close_app();
    host_rename("/sd/data/snake" "/pack_off", "/sd/data/snake" "/pack");
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "food")) scen_food(seed);
    if (all || !strcmp(which, "lanes")) scen_lanes(seed);
    if (all || !strcmp(which, "grow")) scen_grow(seed);
    if (all || !strcmp(which, "ready")) scen_ready(seed);
    if (all || !strcmp(which, "pause")) scen_pause(seed);
    if (all || !strcmp(which, "input")) scen_input(seed);
    if (all || !strcmp(which, "ai")) scen_ai(seed);
    if (all || !strcmp(which, "stats")) scen_stats(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "fuzz")) {
        for (int round = 0; round < 2; round++) {
            g_nh.adv = g_nh.imu = round == 1;
            nh_open_app(seed + round * 101);
            nh_fuzz(60000);
            for (int i = 0; i < 8 && !nh_exit; i++) { nh_tap('`', 60, 300); if (!nh_exit && i == 3) nh_tap('\n'); }
            nh_check(nh_exit, "fuzz round %d: Esc never left the app", round);
            nh_close_app();
        }
    }
}
