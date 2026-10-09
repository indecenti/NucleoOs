// Flipper scenarios for the native-games host gate (tools/native-host/run.mjs pinball).
// Every screen in it/de with nothing under the footer and every hint fitting it; no SFX synthesis on launch;
// flipper physics (a ball hit by a swinging bat never ends up under it, a cradled ball stays on the bat),
// hold-to-flip on every flipper key (incl. the auto-repeating , and /), one clack per press, the ball saver,
// tilt warnings that fade, level-up, pause/leave flow, Esc; a long fuzz on both boards.
#define NH_GAME_NAME "pinball"
#include "../../../firmware/components/nucleo_app/app_pinball.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_pinball(); }

// Pixels the app drew in the footer band (rows H-HINT..H-1) on a non-fullscreen frame: must be 0.
static int below_footer(void) {
    nh_canvas.fillScreen(0xF81F); uint32_t sent = nh_canvas.readPixel(0, H - 1);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int n = 0;
    for (int y = H - HINT; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != sent) n++;
    return n;
}
static void screen(const char *name, bool footer_free = true) {
    nh_loop_ms(300);
    nh_check_hint(name);
    if (!nh_full && footer_free) { int n = below_footer(); nh_check(n == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, n); }
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static int sfx_files(void) {
    int n = 0; struct stat st;
    for (int id = 1; id <= 28; id++) { char p[96]; snprintf(p, sizeof p, "sd/data/pinball/sfx/%s.wav", sfx_name(id)); if (stat(p, &st) == 0) n++; }
    return n;
}

// Put a live ball on the table at (x, y) with velocity (vx_, vy_), flippers down, no saver, no tilt.
static void place(float x, float y, float vx_, float vy_) {
    s_bp = BP_PLAY; s_inchute = false; s_charging = false; s_save_ms = 0; s_tilt = 0; s_tilt_warn = 0;
    bx = x; by = y; vx = vx_; vy = vy_; s_stuck_ms = 0; s_launch_ms = 0; s_dmd_ms = 0;
    s_fl.sw = s_fr.sw = 0; s_fl.hits = s_fr.hits = 0;
}
// Which side of flipper f's bat line the ball is on (+1 = the playfield side, above the bat), and whether
// its projection falls inside the bat.
static int side_of(const Flip *f, bool *inside) {
    float tx2, ty2; flip_tip(f, &tx2, &ty2);
    float dx = tx2 - f->px, dy = ty2 - f->py, l2 = dx * dx + dy * dy;
    float t = ((bx - f->px) * dx + (by - f->py) * dy) / l2;
    *inside = t > 0.05f && t < 0.95f;
    float s = dx * (by - f->py) - dy * (bx - f->px), ref = dx * (-20.0f);   // reference point 20 px above the pivot
    return (s * ref > 0) ? 1 : -1;
}
static Flip *s_watch; static int s_tunnels, s_prev_side; static bool s_prev_in;
static bool watch_tick(void) {
    if (s_bp != BP_PLAY) return true;
    bool in; int sd = side_of(s_watch, &in);
    if (s_prev_side == 1 && sd == -1 && in && s_prev_in) s_tunnels++;
    s_prev_side = sd; s_prev_in = in;
    return by < 150;                                       // sent back up the table
}

static void scenario_screens(const char *lang) {
    g_nh.lang = lang;
    nh_open_app(5);
    screen("menu");
    nh_tap('.'); nh_tap('\n'); screen("scores");
    nh_tap('`'); nh_tap('3'); screen("settings");
    nh_check(s_screen == ST_SET, "digit 3 opens the settings");
    int au = g_audio; nh_tap('\n'); nh_check(g_audio != au, "ENTER on Audio toggles it"); nh_tap('\n');
    nh_tap('.'); nh_tap('.'); nh_tap('\n'); screen("help0");
    nh_tap('/'); screen("help1");
    nh_tap('/'); screen("help2");
    nh_tap('`'); nh_check(s_screen == ST_SET, "Esc in the help -> settings");
    nh_tap('`'); nh_check(s_screen == ST_MENU, "Esc in settings -> menu");
    nh_tap('1'); nh_check(s_screen == ST_PLAY && nh_full, "Play opens the fullscreen table");
    nh_loop_ms(1800); screen("ready");
    nh_tap(' '); nh_loop_ms(1500); screen("play");
    nh_tap('`'); screen("pause", false);
    nh_check(s_screen == ST_OPT, "Esc during a game asks first (pause), it does not drop the game");
    nh_tap('\n'); nh_check(s_screen == ST_PLAY, "ENTER in the pause resumes");
    nh_tap('\t'); nh_tap('\t'); nh_check(s_screen == ST_PLAY, "TAB pauses and resumes");
    s_lv_score = s_lv.goal; nh_loop_ms(400); screen("levelup");
    nh_check(s_level == 2, "reaching the goal levels up (level %d)", s_level);
    nh_loop_ms(2500);
    s_balls = 1; place(67, 230, 0, 2.5f); nh_loop_ms(3000);
    nh_check(s_screen == ST_OVER, "last ball drained -> game over (screen %d)", s_screen);
    screen("over", false);
    nh_tap('`'); nh_check(s_screen == ST_MENU, "Esc on game over -> menu");
    nh_tap('`'); nh_check(nh_exit, "Esc on the menu leaves the app");
    nh_close_app();
}

static void scenario_rules(void) {
    g_nh.lang = "it";
    nh_open_app(9);
    nh_check(sfx_files() == 0, "opening the app synthesizes no SFX (%d files)", sfx_files());
    nh_tap('\n'); nh_loop_ms(1700);
    // one clack per flipper press
    s_bp = BP_PLAY; s_inchute = false; bx = 67; by = 100; vx = vy = 0;
    bx = 67; by = 150; int p0 = nh_plays; nh_key_down('1'); nh_loop_ms(60); int dp = nh_plays - p0; nh_key_up('1'); nh_loop_ms(200);
    nh_check(dp == 1, "a '1' press plays one flipper clack (%d)", dp);
    // hold-to-flip on every key, including the auto-repeating , and / (350 ms repeat delay)
    const char keys[4] = { '1', ',', '0', '/' };
    for (int k = 0; k < 4; k++) {
        place(67, 60, 0, 0);
        Flip *f = (k < 2) ? &s_fl : &s_fr;
        nh_key_down(keys[k]);
        float lo = 1;
        for (int t = 0; t < 45; t++) { nh_loop_ms(20); bx = 67; by = 60; vx = vy = 0; if (t > 5 && f->sw < lo) lo = f->sw; }
        nh_key_up(keys[k]); nh_loop_ms(300);
        nh_check(lo > 0.99f, "holding '%c' keeps the flipper up for 0.9 s (dipped to %.2f)", keys[k], lo);
        nh_check(f->sw < 0.01f, "releasing '%c' drops the flipper (%.2f)", keys[k], f->sw);
    }
    // ball saver: a drain inside the saver window gives the ball back
    place(67, 60, 0, 0); s_save_ms = 2000; int balls = s_balls;
    bx = 67; by = 232; vy = 2.0f; nh_loop_ms(200);
    nh_check(s_balls == balls && s_bp == BP_READY, "a drain while the saver is lit is not a lost ball (balls %d -> %d, phase %d)", balls, s_balls, s_bp);
    // tilt: occasional nudges fade, a burst tilts
    place(67, 60, 0, 0);
    int tilted = 0;
    for (int i = 0; i < 6; i++) { s_bp = BP_PLAY; bx = 67; by = 60; vx = vy = 0; nh_tap('n'); tilted |= s_tilt > 0; nh_loop_ms(3000); }
    nh_check(!tilted, "a nudge every 3 s never tilts (warn %d)", s_tilt_warn);
    place(67, 60, 0, 0);
    for (int i = 0; i < 4; i++) { bx = 67; by = 60; vx = vy = 0; nh_tap('n', 30, 30); }
    nh_check(s_tilt > 0, "four quick nudges tilt");
    nh_close_app();
}

static void scenario_flippers(void) {
    g_nh.lang = "en";
    nh_open_app(13);
    nh_tap('\n'); nh_loop_ms(1700);
    int tunnels = 0, saved = 0, n = 0;
    for (int side = 0; side < 2; side++) {
        Flip *f = side ? &s_fr : &s_fl; char key = side ? '0' : '1';
        for (int i = 0; i < 40; i++) {
            float u = (i % 8) / 7.0f;                              // where along the bat it lands
            float x = side ? 96 - u * 20 : 40 + u * 20;
            float v = 1.2f + (i / 8) * 0.55f;                      // 1.2 .. 3.4 px/frame
            place(x, 196 + u * 6, 0, v);
            s_watch = f; s_tunnels = 0; bool in; s_prev_side = side_of(f, &in); s_prev_in = in;
            int delay = 20 + (i * 37) % 260;
            nh_run_until(watch_tick, delay);
            nh_key_down(key);
            nh_run_until(watch_tick, 1500);
            bool up = s_bp == BP_PLAY;                             // flipped back into play (not drained)
            nh_key_up(key);
            tunnels += s_tunnels; saved += up; n++;
            if (getenv("NH_VERBOSE")) nh_note("side %d i %2d u %.2f v %.2f delay %3d -> %s (%.0f,%.0f) phase %d tun %d", side, i, u, v, delay, up ? "UP" : "--", bx, by, s_bp, s_tunnels);
            s_balls = 3;
            if (s_bp != BP_PLAY) { s_bp = BP_PLAY; }
        }
    }
    nh_note("flippers: %d of %d balls sent back up, %d went through a bat", saved, n, tunnels);
    nh_check(tunnels == 0, "a swinging flipper never lets the ball through the bat (%d of %d)", tunnels, n);
    nh_check(saved * 10 >= n * 7, "most balls landing on a flipper are kept in play (%d of %d)", saved, n);
    // a cradled ball (flipper held up) stays above the bat
    place(52, 200, 0, 0.5f); nh_key_down('1');
    s_watch = &s_fl; s_tunnels = 0; s_prev_side = 1; s_prev_in = false;
    for (int t = 0; t < 100; t++) { nh_loop_ms(20); bool in; int sd = side_of(&s_fl, &in); if (sd < 0 && in) s_tunnels++; if (s_bp != BP_PLAY) break; }
    nh_key_up('1');
    nh_check(s_tunnels == 0, "a ball cradled on a raised flipper never sinks into it");
    nh_close_app();
}

static void scenario_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = (round == 1);
        g_nh.lang = round ? "fr" : "es";
        nh_open_app(seed + round * 101);
        nh_tap('\n');
        for (int t = 0; t < 900 && !nh_exit; t++) {
            static const char keys[] = "1100,,//  n";
            char ch = keys[nh_jitter((int)sizeof keys - 1)];
            nh_key_down(ch); nh_loop_ms(20 + nh_jitter(250)); nh_key_up(ch); nh_loop_ms(nh_jitter(150));
            if (s_screen == ST_OVER) nh_tap('\n');
            nh_check(bx >= LWALL && bx <= RWALL && by >= TWALL - 1, "ball left the table (%.1f, %.1f)", bx, by);
        }
        nh_fuzz(30000);
        nh_dump(round ? "fuzz_adv" : "fuzz");
        for (int i = 0; i < 10 && !nh_exit; i++) nh_tap('`', 60, 300);   // pause -> menu -> out
        nh_check(nh_exit, "round %d: Esc never left the app (screen %d)", round, s_screen);
        nh_close_app();
    }
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) { scenario_screens("it"); scenario_screens("de"); }
    if (all || !strcmp(which, "rules")) scenario_rules();
    if (all || !strcmp(which, "flip")) scenario_flippers();
    if (all || !strcmp(which, "fuzz")) scenario_fuzz(seed);
}
