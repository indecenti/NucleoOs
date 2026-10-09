// Pong scenarios for the native-games host gate (tools/native-host/run.mjs pong).
// Every screen in three languages, the menu clear of the footer, the ball never tunnelling through a paddle,
// sane bounce angles, power-ups, the CPU's skill order per difficulty, paddle input precision, pause, no SFX
// synthesis on launch.
#define NH_GAME_NAME "pong"
#include "../../../firmware/components/nucleo_app/app_pong.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_pong(); }

// Pixels the app drew in the footer band (rows H-HINT..H-1) on a non-fullscreen frame: must be 0.
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

static void go_menu_item(int i) { s_menu.sel = (int8_t)i; s_menu.pos = i; nh_tap('\n'); }
// A rally fixture: AI match, ball live in the middle.
static void start_play(int diff) {
    s_diff = diff; go_menu_item(0);
    nh_check(s_screen == ST_PLAY, "menu row 0 did not start a match");
    s_phase = PH_PLAY; s_phtimer = 0;
}

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        screen("menu");
        go_menu_item(3); screen("settings"); nh_tap('`');
        go_menu_item(4); screen("scores"); nh_tap('`');
        go_menu_item(5); screen("help"); nh_tap('`');
        go_menu_item(1); screen("host"); nh_tap('`');
        go_menu_item(2); screen("browse"); nh_tap('`');
        nh_check(s_screen == ST_MENU, "Esc from a sub-screen did not return to the menu");
        start_play(1);
        nh_loop_ms(2600); nh_dump(l == 0 ? "play_it" : l == 1 ? "play_en" : "play_de");
        s_level = 9; regen_level(9); nh_loop_ms(1500); nh_dump(l == 0 ? "play9_it" : "play9_x");
        nh_tap('`');
        if (s_screen == ST_PLAY) screen("pause");
        nh_tap('\n');
        sl = TARGET - 1; s_phase = PH_PLAY; bx = W - 2; vx = 0.2f; nh_loop_ms(200);
        screen("over");
        nh_close_app();
    }
    g_nh.lang = "it";
}

// A ball at top speed aimed straight at the paddle's centre must bounce, at every frame jitter.
static void scen_tunnel(uint32_t seed) {
    nh_open_app(seed); start_play(1);
    int miss = 0, n = 60;
    for (int i = 0; i < n; i++) {
        s_phase = PH_PLAY; s_shake = 0; s_lh = BASE_PH; s_lh_t = 0;
        pl = 60 + i % 40; bx = 70 + i * 0.37f; by = pl; vx = -MAX_SPD; vy = 0;
        int before = sr;
        for (int k = 0; k < 30 && vx < 0 && s_phase == PH_PLAY; k++) nh_loop_ms(1);
        if (sr != before || vx < 0) miss++;
    }
    nh_check(miss == 0, "tunnel: a top-speed ball went through a centred paddle %d/%d times", miss, n);
    nh_close_app();
}

// Repeated edge hits must not turn the ball into a near-vertical crawler.
static void scen_angle(uint32_t seed) {
    nh_open_app(seed); start_play(1);
    float worst = 1; vx = -0.15f; vy = 0.15f;
    for (int i = 0; i < 40; i++) {                 // successive hits on the paddle's lower edge
        s_phase = PH_PLAY; pl = 70; by = pl + s_lh / 2 + 1; bx = PADL_FACE + BR; vx = -fabsf(vx); vy = fabsf(vy);
        reflect_paddle(1);
        float sp = sqrtf(vx * vx + vy * vy), r = fabsf(vx) / sp;
        if (r < worst) worst = r;
    }
    nh_check(worst >= 0.5f, "angle: an edge hit left |vx| at %.2f of the speed (near-vertical ball)", worst);
    nh_close_app();
}

// A pickup the ball crosses before anybody touched it must stay on the field (it used to vanish, unused).
static void scen_powerup(uint32_t seed) {
    nh_open_app(seed); start_play(1);
    s_lasthit = 0; s_pu_on = true; s_pu_type = PU_GROW; s_pu_x = W / 2; s_pu_y = 70; s_pu_next = 999999;
    bx = W / 2 - 20; by = 70; vx = 0.12f; vy = 0;
    nh_loop_ms(400);
    nh_check(s_pu_on, "powerup: the ball ate a pickup with no hitter (lost for nobody)");
    s_lasthit = 1; bx = s_pu_x - 12; by = s_pu_y; vx = 0.12f; vy = 0; s_lh_t = 0; s_phase = PH_PLAY;
    nh_loop_ms(300);
    nh_check(!s_pu_on && s_lh_t > 0, "powerup: the left player's ball did not take GROW (on=%d lh_t=%d)", s_pu_on, s_lh_t);
    nh_close_app();
}

// The CPU: rallies get longer as the difficulty rises, and even Hard can be beaten.
static int rally_hits(int diff, uint32_t seed, int *cpu_missed) {
    nh_open_app(seed); start_play(diff);
    int hits = 0, misses = 0; int last_r = 0;
    nh_rng = seed * 31 + 7;
    for (int t = 0; t < 40000 && misses < 12; t++) {
        if (s_screen != ST_PLAY) { new_match(MODE_AI); go(ST_PLAY); }
        s_phase = s_phase == PH_COUNT ? PH_PLAY : s_phase;
        // a perfect human that aims somewhere along its paddle
        static float aim; if (vx > 0) aim = ((int)(esp_random() % 17) - 8) * (s_lh / 34.0f);
        pl = by - aim; float lo = COURT_TOP + s_lh / 2, hi = COURT_BOT - s_lh / 2; if (pl < lo) pl = lo; if (pl > hi) pl = hi;
        int r = s_hitr; if (r > last_r) hits++; last_r = r;
        int before = sl; nh_loop_ms(1); if (sl != before) misses++;
        if (sl >= TARGET - 1) sl = 0;
    }
    *cpu_missed = misses; nh_close_app();
    return hits;
}
static void scen_ai(uint32_t seed) {
    int miss[3], hits[3];
    for (int k = 0; k < 3; k++) hits[k] = rally_hits(k, seed + k, &miss[k]);
    nh_note("ai: CPU returns before missing, easy/normal/hard = %.1f / %.1f / %.1f", hits[0] / (float)(miss[0] ? miss[0] : 1), hits[1] / (float)(miss[1] ? miss[1] : 1), hits[2] / (float)(miss[2] ? miss[2] : 1));
    float e = hits[0] / (float)(miss[0] ? miss[0] : 1), n = hits[1] / (float)(miss[1] ? miss[1] : 1), h = hits[2] / (float)(miss[2] ? miss[2] : 1);
    nh_check(e < n && n < h, "ai: difficulty order broken (returns per miss %.1f / %.1f / %.1f)", e, n, h);
    nh_check(miss[2] > 0, "ai: Hard never missed — unbeatable");
    nh_check(e >= 1.0f, "ai: Easy misses almost every ball (%.1f returns per miss)", e);
}

// A tap moves the paddle a little and stops; a hold glides; release stops at once.
static void scen_input(uint32_t seed) {
    nh_open_app(seed); start_play(1);
    s_phase = PH_COUNT; s_phtimer = 999999;                      // freeze the ball, keep the paddle live
    pl = 80; nh_tap('.', 40, 120);
    float tap = pl - 80;
    nh_check(tap > 1 && tap < 10, "input: a 40 ms tap moved the paddle %.1f px (want a small nudge)", tap);
    pl = 40; nh_key_down('.'); nh_loop_ms(500); float held = pl - 40; nh_key_up('.'); float at = pl; nh_loop_ms(300);
    nh_check(held > 60, "input: a 500 ms hold moved only %.1f px", held);
    nh_check(fabsf(pl - at) < 0.01f, "input: the paddle kept moving %.1f px after release", pl - at);
    nh_close_app();
}


// Esc in a match pauses; ENTER resumes; Esc again leaves; Esc at the menu closes the app.
static void scen_pause(uint32_t seed) {
    nh_open_app(seed); start_play(1);
    nh_tap('`');
    nh_check(s_screen == ST_PLAY && s_paused, "pause: Esc in a match did not pause");
    float x = bx; nh_loop_ms(300);
    nh_check(bx == x, "pause: the ball moved while paused (vs CPU)");
    nh_tap('\n'); nh_check(!s_paused && s_screen == ST_PLAY, "pause: ENTER did not resume");
    nh_tap('`'); nh_tap('`');
    nh_check(s_screen == ST_MENU, "pause: Esc Esc did not leave to the menu");
    nh_tap('`');
    nh_check(nh_exit, "Esc at the menu did not close the app");
    nh_close_app();
}

static int sfx_cached(void) {
    int n = 0; struct stat st;
    for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", SFX_NAME[id]); if (host_stat(p, &st) == 0) n++; }
    return n;
}
// The pack is the sound: every cue resolves to it, and nothing is ever synthesized on the device (launch,
// menus or play). Without a pack a cue degrades to a tone, still without synthesis.
static void scen_sfx(uint32_t seed) {
    int packed = 0; struct stat st;
    for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", SFX_NAME[id]); if (host_stat(p, &st) == 0 && st.st_size > 44) packed++; }
    nh_check(packed == NSFX, "sfx: the pack covers %d/%d cues", packed, NSFX);
    nh_open_app(seed);
    int plays = nh_plays; sfx(10); nh_loop_ms(3000);
    nh_check(nh_plays > plays, "sfx: a cue with its WAV in the pack did not play it");
    nh_check(sfx_cached() == 0, "sfx: %d cues synthesized on the device", sfx_cached());
    nh_close_app();
    host_rename(DIRR "/pack", DIRR "/pack_off");
    nh_open_app(seed);
    int tones = nh_tones; sfx(10); nh_loop_ms(3000);
    nh_check(nh_tones > tones, "sfx: without a pack a cue was silent (no tone fallback)");
    nh_check(sfx_cached() == 0, "sfx: %d cues synthesized without a pack", sfx_cached());
    nh_close_app();
    host_rename(DIRR "/pack_off", DIRR "/pack");
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "tunnel")) scen_tunnel(seed);
    if (all || !strcmp(which, "angle")) scen_angle(seed);
    if (all || !strcmp(which, "powerup")) scen_powerup(seed);
    if (all || !strcmp(which, "ai")) scen_ai(seed);
    if (all || !strcmp(which, "input")) scen_input(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "pause")) scen_pause(seed);
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
