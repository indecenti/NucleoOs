// Reattore scenarios for the native-games host gate (tools/native-host/run.mjs reactor).
// Every screen in it/de with nothing under the footer and every hint fitting it; no SFX synthesis on launch;
// rod input precision (tap = one notch, hold repeats, 1-6 pick a rod), the reactor physics (full power melts
// the core, all rods in blacks the grid out, a steady hand survives), pause/leave flow, a top-10 run that
// always reaches the name entry, Esc; a long fuzz on both boards.
#define NH_GAME_NAME "reactor"
#include "../../../firmware/components/nucleo_app/app_reactor.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_reactor(); }

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
    if (!nh_full) { int n = below_footer(); nh_check(n == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, n); }
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static int sfx_files(void) {
    int n = 0; struct stat st;
    for (int id = 1; id <= 12; id++) { char p[96]; snprintf(p, sizeof p, "sd/data/reattore/sfx/%s.wav", sfx_name(id)); if (stat(p, &st) == 0) n++; }
    return n;
}
static int rods_out(void) { int r = 0; for (int i = 0; i < NROD; i++) r += ROD_MAX - s_rod[i]; return r; }
static bool over(void) { return s_screen == ST_OVER; }

// A steady operator: every ~250 ms trims the bank one notch toward just meeting the demand, easing off while
// the core is hot and the grid has margin — the way a careful player would. Returns once the game ends or `ms` pass.
static int steady_operator(int ms) {
    int t0 = s_time_ms;
    for (int t = 0; t < ms / 250 && s_screen == ST_PLAY; t++) {
        int target = (s_demand + 9) / 10;                       // just enough rods out to meet the demand...
        if (s_heat > 85 && s_stab > 50) target--;               // ...one fewer while hot, if the grid can take it
        int R = rods_out(), want = R < target ? +1 : R > target ? -1 : 0;
        if (want) {
            int best = -1;
            for (int i = 0; i < NROD; i++) if ((want > 0 && s_rod[i] > 0) || (want < 0 && s_rod[i] < ROD_MAX)) { best = i; break; }
            if (best >= 0) { while (s_cur != best) nh_tap('/', 20, 10); nh_tap(want > 0 ? ';' : '.', 20, 10); }
        }
        nh_loop_ms(220);
    }
    return s_time_ms - t0;
}

static void scenario_screens(const char *lang) {
    g_nh.lang = lang;
    nh_open_app(3);
    screen("menu");
    nh_tap('.'); nh_tap('\n'); screen("scores");
    nh_tap('`'); nh_check(s_screen == ST_MENU, "Esc on the scores -> menu");
    nh_tap('3'); screen("settings");
    nh_check(s_screen == ST_SET, "digit 3 opens the settings");
    nh_tap('.'); nh_tap('.'); nh_tap('\n'); screen("help0");
    nh_tap('/'); screen("help1");
    nh_tap('/'); screen("help2");
    nh_tap('`'); nh_check(s_screen == ST_SET, "Esc in the help -> settings");
    nh_tap('`'); nh_check(s_screen == ST_MENU, "Esc in the settings -> menu");
    nh_tap('1'); nh_check(s_screen == ST_PLAY, "1 starts a game");
    nh_loop_ms(4000); screen("play");
    nh_tap('`'); screen("pause");
    nh_check(s_screen == ST_PAUSE, "Esc during a game pauses (it does not drop the game)");
    int t = s_time_ms; nh_loop_ms(1000); nh_check(s_time_ms == t, "the reactor is frozen while paused");
    nh_tap('\n'); nh_check(s_screen == ST_PLAY, "ENTER resumes");
    for (int i = 0; i < NROD; i++) s_rod[i] = 0;              // everything out: the core runs away
    nh_check(nh_run_until(over, 8000), "full power melts the core");
    nh_check(s_over_reason == 0, "...as a meltdown (reason %d)", s_over_reason);
    nh_loop_ms(200); screen("meltdown");
    nh_loop_ms(1800); screen("over");
    nh_close_app();
}

static void scenario_rules(void) {
    g_nh.lang = "it";
    nh_open_app(7);
    nh_check(sfx_files() == 0, "opening the app synthesizes no SFX (%d files)", sfx_files());
    nh_tap('1'); nh_loop_ms(300);
    // rod precision: a tap moves exactly one notch, a hold repeats after the driver's delay
    s_cur = 0; s_rod[0] = 3;
    nh_tap(';', 60, 60); nh_check(s_rod[0] == 2, "UP tap pulls the rod one notch (%d)", s_rod[0]);
    nh_tap('.', 60, 60); nh_check(s_rod[0] == 3, "DOWN tap pushes it back one notch (%d)", s_rod[0]);
    nh_hold(';', 700); nh_check(s_rod[0] == 0, "holding UP pulls it all the way (%d)", s_rod[0]);
    nh_tap('4'); nh_check(s_cur == 3, "4 picks rod 4 (cur %d)", s_cur);
    nh_tap('/'); nh_check(s_cur == 4, "RIGHT moves to the next rod");
    nh_tap(','); nh_tap(','); nh_check(s_cur == 2, "LEFT moves back");
    nh_tap(' '); nh_check(rods_out() == 0, "SPACE scrams: every rod in");
    // all rods in: no power, the grid blacks out
    nh_check(nh_run_until(over, 20000), "no power -> the grid fails");
    nh_check(s_over_reason == 1, "...as a blackout (reason %d)", s_over_reason);
    nh_close_app();

    // a steady hand lasts: at normal difficulty a careful operator survives well past 30 s
    nh_open_app(8);
    g_diff = 1; nh_tap('1'); nh_loop_ms(200);
    int ms = steady_operator(120000);
    nh_note("steady operator (normal): %d s, score %d, end reason %d", ms / 1000, s_score, s_screen == ST_OVER ? s_over_reason : -1);
    nh_check(ms >= 60000, "a careful operator survives over a minute at normal (%d s)", ms / 1000);
    nh_check(s_score > 500, "and scores (%d)", s_score);
    // the run qualifies for the top 10: Esc on the result must still lead to the name entry
    nh_run_until(over, 120000);
    nh_loop_ms(800);
    nh_check(s_qualify, "a first run on an empty board qualifies");
    nh_tap('`'); nh_check(s_screen == ST_NAME, "Esc on a top-10 result still asks for the name (screen %d)", s_screen);
    screen("name");
    nh_tap('\n'); nh_check(s_screen == ST_SCORES && s_newrank == 0, "ENTER files it at #1");
    screen("scores_new");
    nh_tap('`'); nh_check(s_screen == ST_MENU, "Esc -> menu");
    nh_tap('`'); nh_check(nh_exit, "Esc on the menu leaves the app");
    nh_close_app();
}

// Difficulty order: the same careful operator lasts longest on easy, shortest on hard, and every game ends.
static void scenario_balance(void) {
    g_nh.lang = "en";
    int t[3];
    for (int df = 0; df < 3; df++) {
        nh_open_app(20 + df);
        g_diff = df; nh_tap('1'); nh_loop_ms(200);
        t[df] = steady_operator(600000);
        nh_check(s_screen == ST_OVER, "difficulty %d: the run ends (%d s)", df, t[df] / 1000);
        nh_note("difficulty %d: %d s, score %d, %s", df, t[df] / 1000, s_score, s_over_reason ? "blackout" : "meltdown");
        nh_close_app();
    }
    nh_check(t[0] > t[1] && t[1] > t[2], "easy outlasts normal outlasts hard (%d / %d / %d s)", t[0] / 1000, t[1] / 1000, t[2] / 1000);
    nh_check(t[2] >= 20000, "hard is still playable for 20 s (%d s)", t[2] / 1000);
}

static void scenario_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = (round == 1);
        g_nh.lang = round ? "fr" : "es";
        nh_open_app(seed + round * 101);
        nh_tap('1');
        for (int t = 0; t < 700 && !nh_exit; t++) {
            static const char keys[] = ";;..,,//123456 we\n";
            char ch = keys[nh_jitter((int)sizeof keys - 1)];
            nh_key_down(ch); nh_loop_ms(20 + nh_jitter(250)); nh_key_up(ch); nh_loop_ms(nh_jitter(150));
            nh_check_hint("fuzz");
            if (s_screen == ST_MENU) nh_tap('1');
        }
        nh_fuzz(20000);
        nh_dump(round ? "fuzz_adv" : "fuzz");
        for (int i = 0; i < 10 && !nh_exit; i++) nh_tap('`', 60, 300);
        nh_check(nh_exit, "round %d: Esc never left the app (screen %d)", round, s_screen);
        nh_close_app();
    }
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) { scenario_screens("it"); scenario_screens("de"); }
    if (all || !strcmp(which, "rules")) scenario_rules();
    if (all || !strcmp(which, "balance")) scenario_balance();
    if (all || !strcmp(which, "fuzz")) scenario_fuzz(seed);
}
