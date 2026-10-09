// Slot scenarios for the native-games host gate (tools/native-host/run.mjs slots).
// The reel stops are random (independent of the spin timing and the slam-stop), the paylines pay exactly
// the paytable (wilds, the cherry pair, every line), the credit accounting (a spin left half-way is
// settled, never swallowed; a win is credited once), the return to player, autospin, the console flow,
// a complete sound pack, every screen in three languages clear of the footer.
#define NH_GAME_NAME "slots"
#include "../../../firmware/components/nucleo_app/app_slots.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_slots(); }

static int below_footer(void) {
    nh_canvas.fillScreen(0xF81F); uint32_t sent = nh_canvas.readPixel(0, H - 1);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int n = 0;
    for (int y = H - HINT; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != sent) n++;
    return n;
}
static void screen(const char *name) {
    nh_loop_ms(400);
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static bool idle(void) { return s_pp == PP_IDLE; }
static void to_table(void) { s_screen = ST_MENU; nh_tap('\n'); nh_check(s_screen == ST_PLAY, "menu row 0 did not open the machine (screen %d)", s_screen); }

// The paytable as the rules state it, recomputed here from the visible stops: the reference for the game.
static int sym_at(int r, int row) { return s_strip[r][(r_top[r] + row) % SLEN]; }
static int ref_win(int lines, int bpl) {
    static const int P3[NSYM] = { 5, 8, 12, 20, 40, 80, 200, 500 };
    int tot = 0;
    for (int l = 0; l < lines; l++) {
        int s0 = sym_at(0, PAYLINE[l][0]), s1 = sym_at(1, PAYLINE[l][1]), s2 = sym_at(2, PAYLINE[l][2]);
        int base = s0 != SY_WILD ? s0 : s1 != SY_WILD ? s1 : s2;
        int run = 0; int sv[3] = { s0, s1, s2 };
        for (int r = 0; r < 3 && (sv[r] == base || sv[r] == SY_WILD); r++) run++;
        if (run == 3) tot += P3[base] * bpl;
        else if (run == 2 && base == SY_CHERRY) tot += CHERRY2 * bpl;
    }
    return tot;
}

// Every spin pays exactly what the stops show; the balance moves by -bet + win, once.
static void scen_pay(uint32_t seed) {
    nh_open_app(seed); to_table();
    nh_draw_every = 1000000;
    int bad = 0, wins = 0;
    for (int k = 0; k < 400; k++) {
        g_lines_idx = k % 3; g_bet_idx = k % 5; g_balance = 100000;
        int bet = total_bet(), before = g_balance;
        nh_tap(' ');
        nh_run_until(idle, 20000);
        int want = ref_win(active_lines(), BET_OPT[g_bet_idx]);
        if (g_balance - before != want - bet) { if (bad++ < 4) nh_note("pay: spin %d moved the balance by %d, the stops pay %d - bet %d", k, g_balance - before, want, bet); }
        if (want) wins++;
    }
    nh_draw_every = 5;
    nh_check(bad == 0, "pay: %d of 400 spins credited a different amount than the paylines show", bad);
    nh_check(wins > 40, "pay: only %d winning spins of 400", wins);
    nh_close_app();
}

// The stops are random: consecutive spins of a reel land anywhere, whatever the timing.
static void scen_random(uint32_t seed) {
    nh_open_app(seed); to_table();
    nh_draw_every = 1000000;
    int hist[SLEN] = { 0 }, n = 900;
    for (int k = 0; k < n; k++) {
        g_balance = 100000;
        int prev = r_top[0];
        nh_tap(' ');
        if (k & 1) { nh_loop_ms(150); nh_tap(' '); }                  // slam-stop half of them
        nh_run_until(idle, 20000);
        hist[(r_top[0] - prev + SLEN) % SLEN]++;
    }
    nh_draw_every = 5;
    double e = (double)n / SLEN, chi = 0; int mx = 0;
    for (int i = 0; i < SLEN; i++) { chi += (hist[i] - e) * (hist[i] - e) / e; if (hist[i] > mx) mx = hist[i]; }
    nh_check(chi < 70, "random: reel stops follow the spin timing (chi-square %.0f, one step taken %d/%d times)", chi, mx, n);
    nh_close_app();
}

// Return to player, exact: every one of the 30^3 equally likely stop combinations through the game's own
// line evaluation (5 lines, 1 per line) — the paytable's true return for these strips.
static void scen_rtp(uint32_t seed) {
    nh_open_app(seed);
    g_lines_idx = 2; g_bet_idx = 0;
    long out = 0, in = 0; bool j;
    for (int a = 0; a < SLEN; a++) for (int b = 0; b < SLEN; b++) for (int c = 0; c < SLEN; c++) {
        r_top[0] = a; r_top[1] = b; r_top[2] = c; out += line_wins(&j); in += total_bet();
    }
    nh_note("rtp: %.2f%% exact over all %d stop combinations (5 lines)", 100.0 * out / in, SLEN * SLEN * SLEN);
    nh_check(out * 1000 >= in * 880 && out * 1000 <= in * 960, "rtp: %.1f%% outside 88-96%%", 100.0 * out / in);
    nh_close_app();
}

// Leaving mid-spin (Esc, closing the app) settles the spin; autospin never outlives a game over.
static void scen_leave(uint32_t seed) {
    nh_open_app(seed); to_table();
    g_lines_idx = 2; g_bet_idx = 1; g_balance = 1000;
    nh_tap(' '); nh_loop_ms(100);
    nh_check(s_pp == PP_SPIN, "leave: not spinning");
    nh_tap('`'); nh_tap('`');
    nh_check(s_screen == ST_MENU, "leave: Esc, Esc did not reach the menu (screen %d)", s_screen);
    int want = 1000 - total_bet() + ref_win(active_lines(), BET_OPT[g_bet_idx]);
    nh_check(s_pp == PP_IDLE && g_balance == want, "leave: a spin left half-way -> balance %d, the stops pay %d (pp %d)", g_balance, want, s_pp);
    to_table();
    g_balance = 1000;
    nh_tap(' '); nh_loop_ms(100);
    want = 1000 - total_bet() + ref_win(active_lines(), BET_OPT[g_bet_idx]);   // the stops are drawn when the spin starts
    nh_close_app();
    nh_check(g_balance == want, "close: closing the app mid-spin -> balance %d, the stops pay %d", g_balance, want);

    nh_open_app(seed); to_table();
    g_balance = 1; g_lines_idx = 0; g_bet_idx = 0; s_auto = true; s_auto_ms = 100;
    for (int k = 0; k < 40 && s_screen == ST_PLAY; k++) { nh_run_until(idle, 20000); nh_loop_ms(700); if (g_balance > 0) g_balance = 1; }
    nh_check(s_screen == ST_OVER, "auto: autospin with 1 credit never reached the game over (screen %d)", s_screen);
    nh_tap('\n');
    nh_check(s_screen == ST_PLAY && !s_auto, "auto: the restarted game still has autospin armed (it never spins: AUTO badge forever)");
    nh_close_app();
}

// Sound: the pack covers every cue the code names; nothing is synthesized on the device.
static void scen_sfx(uint32_t seed) {
    nh_open_app(seed); nh_loop_ms(1000);
    int miss = 0; struct stat st;
    for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id)); if (host_stat(p, &st) || st.st_size <= 44) { miss++; nh_note("sfx: no pack WAV for '%s'", sfx_name(id)); } }
    nh_check(miss == 0, "sfx: %d cues missing from the pack", miss);
    nh_close_app();
}

// Console flow: Esc at the machine pauses (ENTER resumes); TAB opens the settings, Esc returns.
static void scen_flow(uint32_t seed) {
    nh_open_app(seed); to_table();
    nh_tap('`');
    nh_check(s_screen == ST_PLAY, "flow: Esc at the machine left it without asking (screen %d)", s_screen);
    nh_tap('\n'); nh_tap('\t');
    nh_check(s_screen == ST_SET, "flow: TAB at the machine did not open the settings (screen %d)", s_screen);
    nh_tap('`');
    nh_check(s_screen == ST_PLAY, "flow: Esc in the machine's settings did not return to it (screen %d)", s_screen);
    nh_close_app();
}

// Force a result on the reels: line `l` shows `sym` on every reel (via the strip), then settle.
static void force_line(int sym) {
    for (int r = 0; r < NREEL; r++) { int i = 0; while (s_strip[r][i] != sym) i++; r_top[r] = (i - 1 + SLEN) % SLEN; r_pos[r] = (float)r_top[r]; }
}
// Show the result now on the reels (as a finished spin) and run the win sequence.
static void win_check_and_show(void) { for (int r = 0; r < NREEL; r++) { r_phase[r] = 0; r_pos[r] = (float)r_top[r]; } s_pp = PP_SPIN; evaluate(); }
static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        g_balance = 1000; g_lines_idx = 2; g_bet_idx = 1; s_auto = false;
        screen("menu");
        s_menu.sel = 1; nh_tap('\n'); screen("help1"); nh_tap('/'); screen("help2"); nh_tap('/'); screen("help3"); nh_tap('`');
        s_menu.sel = 2; nh_tap('\n'); screen("settings"); nh_tap('`');
        s_menu.sel = 0; nh_tap('\n'); screen("play");
        nh_tap(' '); nh_loop_ms(250); screen("spin");
        nh_run_until(idle, 20000);
        // a big win on the middle line, then the jackpot
        s_pp = PP_IDLE; force_line(SY_BAR); g_balance = 1000; win_check_and_show(); nh_loop_ms(1500); screen("bigwin");
        nh_run_until(idle, 8000);
        force_line(SY_SEVEN); win_check_and_show(); nh_loop_ms(1800); screen("jackpot");
        nh_run_until(idle, 8000);
        nh_tap('`'); screen("pause"); nh_tap('\n');
        g_balance = 0; nh_tap(' '); nh_loop_ms(500); screen("over");
        nh_close_app();
    }
    g_nh.lang = "it";
}

static void scen_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = round == 1;
        nh_open_app(seed + round * 101);
        nh_fuzz(90000);
        for (int i = 0; i < 8 && !nh_exit; i++) nh_tap('`', 60, 300);
        nh_check(nh_exit, "fuzz %d: Esc never left the app", round);
        nh_close_app();
    }
    g_nh.adv = g_nh.imu = false;
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "pay")) scen_pay(seed);
    if (all || !strcmp(which, "random")) scen_random(seed);
    if (all || !strcmp(which, "rtp")) scen_rtp(seed);
    if (all || !strcmp(which, "leave")) scen_leave(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "flow")) scen_flow(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "fuzz")) scen_fuzz(seed);
}
