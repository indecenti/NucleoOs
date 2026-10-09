// Tank Duel scenarios for the native-games host gate (tools/native-host/run.mjs tankd).
// Every screen in three languages and clear of the footer, EMP / BURST / BOOST power-ups doing what they say,
// the Sniper refund, the powerup key, the guest's powerup reaching the host, a pause that freezes only vs CPU,
// the co-op win jingle, no SFX synthesis at launch.
#define NH_GAME_NAME "tankd"
#include "../../../firmware/components/nucleo_app/app_tankduel.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_tankduel(); }

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
static void menu_pick(int i) { s_menu.sel = (int8_t)i; s_menu.pos = i; nh_tap('\n'); }
static void duel(int tank) { menu_pick(0); s_tsel = tank; nh_tap('\n'); }

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        screen("menu");
        menu_pick(0); screen("select"); nh_tap('`');
        menu_pick(2); nh_tap('\n'); screen("host"); nh_tap('`');
        menu_pick(3); nh_tap('\n'); screen("browse"); nh_tap('`');
        menu_pick(4); screen("how"); nh_tap('`');
        nh_check(s_state == GS_MENU, "Esc from a sub-screen did not return to the menu");
        duel(1);
        nh_check(s_state == GS_PLAY && nh_full, "a duel did not start fullscreen");
        Tank &me = s_tanks[0];
        me.credits = 37; me.weapon = WP_ROCKET; me.pu = PU_BURST; me.pu_ms = 0;
        nh_key_down('d'); nh_key_down('k'); nh_loop_ms(700); nh_key_up('k'); nh_key_up('d');
        { char b[32]; snprintf(b, sizeof b, "play_%s", g_nh.lang); nh_dump(b); }
        s_shops[0] = { (uint8_t)((int)me.x / TILE_PX - 1), (uint8_t)((int)me.y / TILE_PX - 1), 20000, true };
        nh_loop_ms(200);
        nh_check(s_state == GS_SHOP, "standing on a shop pad did not open the shop");
        { char b[32]; snprintf(b, sizeof b, "shop_%s", g_nh.lang); nh_dump(b); }
        nh_tap('d'); { char b[32]; snprintf(b, sizeof b, "sell_%s", g_nh.lang); nh_dump(b); }
        nh_tap('`');
        nh_tap('`'); screen("pause");
        nh_tap('\n');
        s_tanks[1].hp = 1; damage_tank(1, 5, 0, s_tanks[1].x, s_tanks[1].y); nh_loop_ms(300);
        nh_check(s_state == GS_OVER, "killing the CPU did not end the match");
        screen("over");
        nh_tap('`');
        menu_pick(1); s_tsel = 0; nh_tap('\n'); nh_loop_ms(2500);
        { char b[32]; snprintf(b, sizeof b, "survival_%s", g_nh.lang); nh_dump(b); }
        nh_close_app();
    }
    g_nh.lang = "it";
}

// EMP: the rival is slowed and silenced for a while, then fully restored; its own held BOOST stays unarmed.
static void scen_emp(uint32_t seed) {
    nh_open_app(seed); duel(0);
    Tank &me = s_tanks[0], &cpu = s_tanks[1];
    cpu.x = me.x + 60; cpu.y = me.y; float spd = cpu.spd;
    cpu.pu = PU_BOOST; cpu.pu_ms = 0;
    me.pu = PU_EMP; nh_tap('l');
    nh_check(cpu.stun_ms > 0, "emp: the rival was not stunned");
    nh_check(cpu.pu_ms == 0, "emp: the rival's held BOOST got armed (pu_ms %d)", cpu.pu_ms);
    nh_loop_ms(3200);
    nh_check(cpu.spd == spd, "emp: the rival's speed stayed at %.3f (was %.3f)", cpu.spd, spd);
    nh_check(cpu.stun_ms <= 0, "emp: the stun never ended");
    nh_close_app();
}

// BURST: holding it changes nothing; an active burst fires three times as fast. BOOST only while active.
static void scen_burst(uint32_t seed) {
    nh_open_app(seed); duel(0);
    Tank &me = s_tanks[0];
    me.weapon = WP_CANNON; me.pu = PU_BURST; me.pu_ms = 0; me.fire_cd = 0;
    fire_owner(me, 0);
    nh_check(me.fire_cd == fire_cd_for(WP_CANNON), "burst: holding BURST made the cooldown %d (base %d)", me.fire_cd, fire_cd_for(WP_CANNON));
    me.fire_cd = 0; pu_use(0); fire_owner(me, 0);
    nh_check(me.fire_cd == fire_cd_for(WP_CANNON) / 3, "burst: an active burst left the cooldown at %d", me.fire_cd);
    me.pu = PU_BOOST; me.pu_ms = 0; float x0 = me.x = 200; me.y = 200;
    drive_tank(me, 1, 0, 100); float held = me.x - x0;
    me.x = x0; pu_use(0); drive_tank(me, 1, 0, 100); float active = me.x - x0;
    nh_check(active > held * 1.5f, "boost: active %.1f px vs held %.1f px", active, held);
    nh_close_app();
}

// Selling a Sniper or Minigun refunds 60% of its price (both used to refund 0).
static void scen_sell(uint32_t seed) {
    nh_open_app(seed); duel(0);
    for (int wp = WP_MG; wp <= WP_MINIGUN; wp++) {
        Tank &me = s_tanks[0]; me.weapon = wp; me.credits = 0;
        shop_sell(me, SELL_WP);
        nh_check(me.credits > 0 && me.weapon == WP_CANNON, "sell: weapon %d refunded %d", wp, me.credits);
    }
    nh_close_app();
}

// L uses the powerup on every press (a second L used to be ignored without another key in between).
static void scen_pukey(uint32_t seed) {
    nh_open_app(seed); duel(0);
    Tank &me = s_tanks[0];
    me.pu = PU_NONE; nh_tap('l');
    me.pu = PU_BOOST; me.pu_ms = 0; nh_tap('l');
    nh_check(me.pu_ms > 0, "pukey: the second L did not fire the powerup");
    // a guest's L must reach the host as the use-powerup bit of its input packets
    s_mode = GM_GUEST; s_local = 1; s_pu_req = 0; nh_tap('l');
    nh_check(s_pu_req > 0, "pukey: a guest's L was never sent to the host");
    nh_close_app();
}

// Vs CPU a pause freezes the match; online the match keeps running under the pause card.
static void scen_pause(uint32_t seed) {
    nh_open_app(seed); duel(0);
    nh_tap('`');
    nh_check(s_state == GS_PAUSE, "pause: Esc did not pause");
    int t = s_match_ms; nh_loop_ms(1000);
    nh_check(s_match_ms == t, "pause: the clock ran while paused vs CPU");
    nh_tap('\n'); nh_check(s_state == GS_PLAY, "pause: ENTER did not resume");
    s_mode = GM_HOST; s_haspeer = true; s_last_rx = INT64_MAX / 4;
    nh_tap('`'); t = s_match_ms; nh_loop_ms(1000);
    nh_check(s_match_ms < t - 500, "pause: an online pause froze the match (host)");
    s_haspeer = false; s_mode = GM_CPU;
    nh_tap('`');
    nh_check(s_state == GS_MENU, "pause: Esc Esc did not leave to the menu");
    nh_tap('`');
    nh_check(nh_exit, "Esc at the menu did not close the app");
    nh_close_app();
}

// A survived co-op match counts as a win for the local player (it played the defeat jingle).
static void scen_coop(uint32_t seed) {
    nh_open_app(seed);
    menu_pick(1); s_tsel = 0; nh_tap('\n');
    s_match_ms = 50; nh_loop_ms(300);
    nh_check(s_state == GS_OVER && s_winner == WIN_HUMANS, "coop: surviving the clock did not win");
    nh_check(local_won(), "coop: a co-op win counts as a defeat");
    nh_close_app();
}

// Launch never synthesizes (the deployed pack covers every cue).
static void scen_sfx(uint32_t seed) {
    nh_open_app(seed);
    int n = 0; struct stat st;
    for (int id = 1; id <= 11; id++) { char p[96]; snprintf(p, sizeof p, TD_SFX_DIR "/sfx/%s.wav", sfx_td_name(id)); if (host_stat(p, &st) == 0) n++; }
    nh_check(n == 0, "sfx: launch synthesized %d cues next to the deployed pack", n);
    nh_close_app();
}

void nh_scenarios(const char *which, uint32_t seed) {
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "emp")) scen_emp(seed);
    if (all || !strcmp(which, "burst")) scen_burst(seed);
    if (all || !strcmp(which, "sell")) scen_sell(seed);
    if (all || !strcmp(which, "pukey")) scen_pukey(seed);
    if (all || !strcmp(which, "pause")) scen_pause(seed);
    if (all || !strcmp(which, "coop")) scen_coop(seed);
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
