// Dadi scenarios for the native-games host gate (tools/native-host/run.mjs dice).
// The face the player sees is the value the game reports (every die type, after every throw, after a
// settings change), the dice are fair, every screen in three languages fits (hint, footer), the input
// model (ENTER quick roll, scroll-to-mix then throw, GO hold), and the settings sheet.
#define NH_GAME_NAME "dice"
#include "../../../firmware/components/nucleo_app/app_dice.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_dice(); }

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
    if (!nh_full) nh_check(below_footer() == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static bool idle(void) { return s_state == ST_IDLE; }

// The value the player READS on die i: the front-most face (smallest rotated z). d6: the pip face whose
// outward normal faces the camera; d4/d8/d20: the numbered triangle t (value t+1) facing the camera.
static int shown_value(int i) {
    const Die &dd = s_d[i];
    const fx3d::Model &m = *TYPES[s_type].m;
    int best = 0; float bz = 1e9f;
    if (TYPES[s_type].faces == 6) {
        for (int v = 1; v <= 6; v++) {
            fx3d::V3 n = { FACE[v - 1].n[0], FACE[v - 1].n[1], FACE[v - 1].n[2] };
            int x, y; float z; fx3d::project(n, 0, 0, 1, dd.yaw, dd.pitch, dd.bank, &x, &y, &z);
            if (z < bz) { bz = z; best = v; }
        }
    } else {
        for (int t = 0; t < m.nt; t++) {
            float n[3]; face_normal(m, t, n);
            fx3d::V3 nv = { n[0], n[1], n[2] };
            int x, y; float z; fx3d::project(nv, 0, 0, 1, dd.yaw, dd.pitch, dd.bank, &x, &y, &z);
            if (z < bz) { bz = z; best = t + 1; }
        }
    }
    return best;
}
static int mismatches(void) { int n = 0; for (int i = 0; i < s_n; i++) if (shown_value(i) != s_d[i].value) n++; return n; }

// After every throw of every die type, each die lands showing the value the title adds up.
static void scen_faces(uint32_t seed) {
    nh_open_app(seed);
    for (int t = 0; t < NTYPES; t++) {
        s_type = t; s_n = 6;
        int bad = 0, throws = 30;
        for (int k = 0; k < throws; k++) {
            nh_tap('\n');
            nh_run_until(idle, 4000);
            bad += mismatches();
        }
        nh_check(bad == 0, "faces: %s — %d of %d dice landed showing a different value than the total counts", TYPES[t].name, bad, throws * 6);
    }
    nh_close_app();
}

// The dice on screen must always match the total — at launch, and after changing count or type.
static void scen_consistent(uint32_t seed) {
    s_type = 0; s_n = 2;                                             // (type + count persist across opens)
    nh_open_app(seed); nh_loop_ms(200);
    nh_check(mismatches() == 0, "launch: %d dice show a face that is not their value (the total lies)", mismatches());
    nh_tap('\t'); nh_tap('.'); nh_tap('/'); nh_tap('/');            // type d6 -> d8
    nh_tap('`');
    nh_check(s_type == 2, "settings: RIGHT on the type row did not step the die type (type %d)", s_type);
    nh_check(mismatches() == 0, "type change: %d dice show a face that is not their value", mismatches());
    nh_tap('\t'); nh_tap('/'); nh_tap('\n');                          // one more die, ENTER closes
    nh_check(!s_settings, "settings: ENTER did not close the sheet");
    nh_check(mismatches() == 0, "count change: %d dice show a face that is not their value", mismatches());
    nh_close_app();
}

// Uniform results: a chi-square over many throws, per die type.
static void scen_fair(uint32_t seed) {
    nh_open_app(seed);
    for (int t = 0; t < NTYPES; t++) {
        s_type = t; s_n = MAXDICE;
        int f = TYPES[t].faces, cnt[21] = { 0 }, n = 0;
        for (int k = 0; k < 4000; k++) { throw_dice(0.5f); for (int i = 0; i < s_n; i++) { int v = s_d[i].value; nh_check(v >= 1 && v <= f, "fair: value %d out of 1..%d", v, f); cnt[v]++; n++; } }
        s_state = ST_IDLE;
        double e = (double)n / f, chi = 0; for (int v = 1; v <= f; v++) chi += (cnt[v] - e) * (cnt[v] - e) / e;
        double crit = f == 4 ? 16.3 : f == 6 ? 20.5 : f == 8 ? 24.3 : 45.3;   // p ~ 0.001
        nh_check(chi < crit, "fair: %s chi-square %.1f over %d values (critical %.1f)", TYPES[t].name, chi, n, crit);
    }
    nh_close_app();
}

// ENTER = quick roll; scroll mixes and throws on release; GO hold charges and throws on release.
static void scen_input(uint32_t seed) {
    nh_open_app(seed);
    nh_tap('\n');
    nh_check(s_state == ST_ROLL, "input: ENTER did not roll");
    nh_run_until(idle, 4000);
    nh_tap('.'); nh_tap('.'); nh_tap('.');
    nh_check(s_state == ST_CHARGE, "input: scrolling did not mix (state %d)", s_state);
    nh_run_until(idle, 6000);
    nh_check(s_state == ST_IDLE && s_charge == 0, "input: a scroll-mix never threw / settled");
    nh_ptt(true); nh_loop_ms(900); nh_check(s_state == ST_CHARGE && s_charge > 0.5f, "input: holding GO 0.9 s charged only %.2f", s_charge);
    nh_ptt(false); nh_check(s_state == ST_ROLL, "input: releasing GO did not throw");
    nh_run_until(idle, 6000);
    nh_close_app();
}

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        s_type = 0; s_n = 3; nh_tap('\n'); nh_run_until(idle, 4000); screen("main");
        s_type = 2; s_n = 2; nh_tap('\n'); nh_run_until(idle, 4000); screen("d8");
        nh_tap('\t'); screen("settings"); nh_tap('`');
        s_type = 3; s_n = 6; nh_tap('\n'); nh_run_until(idle, 4000); screen("d20");
        s_type = 1; s_n = 4; nh_tap('\n'); nh_loop_ms(400); screen("rolling");
        nh_run_until(idle, 4000);
        nh_key_down('.'); nh_loop_ms(500); screen("mixing"); nh_key_up('.'); nh_run_until(idle, 6000);
        s_type = 0; s_n = 1; nh_tap('\n'); nh_run_until(idle, 4000); screen("one");
        nh_close_app();
    }
    g_nh.lang = "it";
}

// The sound pack has the three cues the code plays (read from the SD source tree: games.mjs seeds no data
// folder for dice into the sandbox, whose cwd is build/native-host/box/dice).
static void scen_sfx(uint32_t) {
    static const char *const CUES[3] = { "shake", "throw", "land" };
    for (const char *c : CUES) {
        char p[128]; snprintf(p, sizeof p, "../../../../tools/sd-sim/data/dice/pack/%s.wav", c);
        struct stat st; nh_check(stat(p, &st) == 0 && st.st_size > 44, "sfx: no pack WAV for '%s' (tools/sfx-gen/games/dice.py)", c);
    }
}

// Every board: mash keys, Esc leaves.
static void scen_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = round == 1;
        nh_open_app(seed + round * 101);
        nh_fuzz(60000);
        for (int i = 0; i < 4 && !nh_exit; i++) nh_tap('`', 60, 200);
        nh_check(nh_exit, "fuzz %d: Esc never left the app", round);
        nh_close_app();
    }
    g_nh.adv = g_nh.imu = false;
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "faces")) scen_faces(seed);
    if (all || !strcmp(which, "consistent")) scen_consistent(seed);
    if (all || !strcmp(which, "fair")) scen_fair(seed);
    if (all || !strcmp(which, "input")) scen_input(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "fuzz")) scen_fuzz(seed);
}
