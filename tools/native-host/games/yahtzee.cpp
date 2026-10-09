// Yahtzee scenarios for the native-games host gate (tools/native-host/run.mjs yahtzee).
// Scoring of every box (upper bonus, Yahtzee bonus, the official Joker rules), the dice showing their
// values, complete CPU games (legal moves, totals that add up, a sane strength), the console flow (title
// menu, Esc pauses a game, game over, play again), no SFX synthesis at launch, every screen in three
// languages clear of the footer.
#define NH_GAME_NAME "yahtzee"
#include "../../../firmware/components/nucleo_app/app_yahtzee.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_yahtzee(); }

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
    if (!nh_full) nh_check(below_footer() == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static void set_dice(const char *v) { for (int i = 0; i < 5; i++) s_d[i].value = v[i] - '0'; }
static Player *fresh_player(void) { memset(&s_pl[0], 0, sizeof(Player)); s_cur = 0; return &s_pl[0]; }

// Every box, on dice where the answer is known.
static void scen_scoring(uint32_t seed) {
    nh_open_app(seed);
    Player *p = fresh_player();
    struct { const char *dice; int box, want; } T[] = {
        { "11123", 0, 3 }, { "22256", 1, 6 }, { "33333", 2, 15 }, { "41444", 3, 16 }, { "55123", 4, 10 }, { "66666", 5, 30 },
        { "33345", 6, 18 }, { "33245", 6, 0 }, { "44441", 7, 17 }, { "44412", 7, 0 },
        { "22333", 8, 25 }, { "22233", 8, 25 }, { "22334", 8, 0 },
        { "12346", 9, 30 }, { "34561", 9, 30 }, { "23456", 9, 30 }, { "12456", 9, 0 }, { "11234", 9, 30 },
        { "12345", 10, 40 }, { "65432", 10, 40 }, { "12346", 10, 0 },
        { "55555", 11, 50 }, { "55556", 11, 0 }, { "61234", 12, 16 },
    };
    for (auto &t : T) { set_dice(t.dice); int v = preview(p, t.box); nh_check(v == t.want, "scoring: dice %s in box %d gave %d, want %d", t.dice, t.box, v, t.want); }
    // a 5-of-a-kind is a Full House / straight only under the Joker rule (the Yahtzee box already filled)
    set_dice("44444");
    nh_check(preview(p, 8) == 0, "scoring: a Yahtzee scored as Full House with the Yahtzee box still open");
    p->used[11] = true; p->score[11] = 50; p->used[3] = true; p->score[3] = 12;
    nh_check(preview(p, 8) == 25 && preview(p, 9) == 30 && preview(p, 10) == 40,
             "joker: with the Yahtzee box filled a Yahtzee must score FH 25 / SS 30 / LS 40 (got %d / %d / %d)", preview(p, 8), preview(p, 9), preview(p, 10));
    nh_check(preview(p, 6) == 20 && preview(p, 12) == 20, "joker: 3-kind / chance must be the dice sum");
    // the upper bonus and the total
    p = fresh_player();
    for (int i = 0; i < 6; i++) { p->used[i] = true; p->score[i] = (i + 1) * 3; }        // exactly 63
    nh_check(total(p) == 63 + 35, "bonus: upper 63 must add 35 (total %d)", total(p));
    p->score[0] = 2;
    nh_check(total(p) == 62, "bonus: upper 62 must not add 35 (total %d)", total(p));
    nh_close_app();
}

// Lock a box the way the player does: the scorecard, the cursor on the box, ENTER.
static bool lock_box(int box) { s_phase = PH_SCORE; s_sel = box; s_toast_us = 0; nh_tap('\n'); return s_pl[0].used[box]; }

// Yahtzee bonus (+100 only when the box holds 50) and the forced Joker placement.
static void scen_joker(uint32_t seed) {
    s_humans = 1; s_cpus = 0;
    nh_open_app(seed); start_game();
    Player &p = s_pl[0];
    p.used[11] = true; p.score[11] = 50;
    set_dice("44444"); s_rolled = true; s_rolls = 0;
    nh_check(!lock_box(8), "joker: a Yahtzee went into Full House while the Fours box was open (must go to Fours)");
    nh_check(lock_box(3) && p.score[3] == 20 && p.ybonus == 100, "joker: Fours took %d with bonus %d (want 20 + 100)", p.score[3], p.ybonus);
    // next turn (1 player): Fours now used -> any lower box at full value, another +100
    set_dice("44444"); s_rolled = true; s_rolls = 0; s_toast_us = 0;
    nh_check(lock_box(10) && p.score[10] == 40 && p.ybonus == 200, "joker: Large straight took %d, bonus %d (want 40, 200)", p.score[10], p.ybonus);
    // a zeroed Yahtzee box: still the Joker, but no bonus
    Player &q = s_pl[0];
    memset(&q, 0, sizeof q); q.used[11] = true; q.score[11] = 0; q.used[1] = true;
    set_dice("22222"); s_rolled = true; s_rolls = 0; s_toast_us = 0;
    nh_check(lock_box(8) && q.score[8] == 25 && q.ybonus == 0, "joker: zeroed Yahtzee box -> FH %d bonus %d (want 25, 0)", q.score[8], q.ybonus);
    nh_close_app();
}

// Whole games played by the CPU alone, as fast as the clock allows: legal, complete, totals that add up.
static int cpu_game(void) {
    s_humans = 0; s_cpus = 3;
    start_game();
    for (int guard = 0; guard < 20000 && s_phase != PH_OVER; guard++) { nh_us += 2000000; sim(0.02f); }
    nh_check(s_phase == PH_OVER, "cpu: a 3-CPU game never ended");
    int sum = 0;
    for (int i = 0; i < s_np; i++) {
        Player &p = s_pl[i]; int up = 0, all = 0;
        for (int c = 0; c < N_CAT; c++) { nh_check(p.used[c], "cpu: player %d left box %d empty", i, c); all += p.score[c]; if (c < 6) up += p.score[c]; }
        nh_check(total(&p) == all + (up >= 63 ? 35 : 0) + p.ybonus, "cpu: player %d total %d does not add up", i, total(&p));
        sum += total(&p);
    }
    return sum / (s_np ? s_np : 1);
}
static void scen_cpu(uint32_t seed) {
    nh_open_app(seed);
    nh_draw_every = 1000000;
    long sum = 0; int n = 40;
    for (int g = 0; g < n; g++) sum += cpu_game();
    nh_draw_every = 5;
    nh_note("cpu: average score %ld over %d games", sum / n, n);
    nh_check(sum / n >= 175, "cpu: average score %ld — too weak (a simple strategy makes ~190)", sum / n);
    nh_close_app();
}

// The dice show their values (the tray and the scorecard must agree).
static void scen_faces(uint32_t seed) {
    s_humans = 1; s_cpus = 0;
    nh_open_app(seed); start_game();
    int bad = 0;
    for (int k = 0; k < 30; k++) {
        s_rolls = 3; s_rolled = false; s_cool_us = 0; s_toast_us = 0; s_phase = PH_ROLL;
        nh_tap('\n'); nh_loop_ms(1600);
        for (int i = 0; i < 5; i++) {
            const Die &dd = s_d[i]; int best = 0; float bz = 1e9f;
            for (int v = 1; v <= 6; v++) { fx3d::V3 n = { FACE[v - 1].n[0], FACE[v - 1].n[1], FACE[v - 1].n[2] }; int x, y; float z; fx3d::project(n, 0, 0, 1, dd.yaw, dd.pitch, dd.bank, &x, &y, &z); if (z < bz) { bz = z; best = v; } }
            if (best != dd.value) bad++;
        }
    }
    nh_check(bad == 0, "faces: %d of 150 dice settled showing a different face than their value", bad);
    nh_close_app();
}

// Console flow: the title menu, Esc pauses a game (a second Esc leaves to the title), game over -> again.
static void scen_flow(uint32_t seed) {
    s_humans = 1; s_cpus = 1;
    nh_open_app(seed);
    nh_check(!nh_full && nh_hint[0], "flow: the title screen has no footer hint (keys explained in-app, cut at the edge)");
    nh_tap('\n');
    nh_check(s_phase == PH_ROLL, "flow: ENTER on the title did not start a game (phase %d)", s_phase);
    nh_tap('`');
    nh_check(!nh_exit && s_phase == PH_ROLL, "flow: Esc during a game left it without asking");
    nh_tap('\n');
    nh_check(!nh_exit && s_phase == PH_ROLL, "flow: ENTER on the pause card did not resume");
    nh_tap('`'); nh_tap('`');
    nh_check(!nh_exit && s_phase == PH_SETUP, "flow: Esc, Esc did not go back to the title (phase %d exit %d)", s_phase, nh_exit);
    nh_tap('`');
    nh_check(nh_exit, "flow: Esc on the title did not close the app");
    nh_close_app();
}

// Sound: the game never synthesizes on its task (the 8 s Task-WDT), and the deployed pack has a WAV for
// every cue the code names.
static int sfx_cached(void) { int n = 0; struct stat st; for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); if (host_stat(p, &st) == 0) n++; } return n; }
static void scen_sfx(uint32_t seed) {
    for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); remove(p); }
    nh_open_app(seed);
    nh_loop_ms(3000);
    nh_check(sfx_cached() == 0, "sfx: the game synthesized %d cues on the app task", sfx_cached());
    int miss = 0; struct stat st;
    for (int id = 1; id <= NSFX; id++) { char p[96]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id)); if (host_stat(p, &st) || st.st_size <= 44) { miss++; nh_note("sfx: no pack WAV for '%s'", sfx_name(id)); } }
    nh_check(miss == 0, "sfx: %d cues have no WAV in the pack (tools/sfx-gen/games/yahtzee.py)", miss);
    nh_close_app();
}

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        s_humans = 2; s_cpus = 1;
        nh_open_app(seed);
        screen("title");
        nh_tap('.'); screen("title_players"); nh_tap('.'); nh_tap('\n'); screen("help"); nh_tap('`');
        s_menu.sel = 0; s_menu.pos = 0;
        nh_tap('\n'); screen("turn");
        nh_tap('\n'); nh_loop_ms(1800); screen("rolled");
        nh_tap('4'); nh_tap('6'); screen("held");
        nh_tap('\t'); nh_loop_ms(600); screen("score");
        s_sel = 10; nh_loop_ms(600); screen("score_low");
        nh_tap('`'); nh_tap('`'); screen("pause"); nh_tap('\n');
        set_dice("33333"); s_anim = false; s_cele_start = nh_us; s_cele_us = nh_us + 2600000; spawn_burst(false); nh_loop_ms(600); screen("yahtzee");
        s_cele_us = 0; s_npart = 0;
        set_dice("22333"); s_phase = PH_SCORE; s_sel = 8; nh_tap('\n'); nh_loop_ms(200); screen("toast");
        // a finished game: everyone's card full
        for (int i = 0; i < s_np; i++) for (int c = 0; c < N_CAT; c++) { s_pl[i].used[c] = true; s_pl[i].score[c] = (c + 2 * i) % 7 * 3; }
        s_turn = N_CAT; s_cur = s_np - 1; s_toast_us = 0; advance_turn(nh_us); nh_loop_ms(1200); screen("over");
        nh_tap('\n'); nh_check(s_phase == PH_ROLL, "over: ENTER did not start a rematch (phase %d)", s_phase);
        nh_close_app();
    }
    g_nh.lang = "it";
}

static void scen_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = round == 1;
        s_humans = 1; s_cpus = 1;
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
    if (all || !strcmp(which, "scoring")) scen_scoring(seed);
    if (all || !strcmp(which, "joker")) scen_joker(seed);
    if (all || !strcmp(which, "cpu")) scen_cpu(seed);
    if (all || !strcmp(which, "faces")) scen_faces(seed);
    if (all || !strcmp(which, "flow")) scen_flow(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "fuzz")) scen_fuzz(seed);
}
