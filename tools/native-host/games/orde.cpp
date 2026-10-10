// Orde scenarios for the native-games host gate (tools/native-host/run.mjs orde).
// Regressions first (each failed on the old code): the XP bar filled before the sim levelled you up, a
// death on the same frame as a level-up opened the chooser (Vita+ revived you), Esc in play threw the run
// away, an unreadable record was overwritten by a worse one.
#define NH_GAME_NAME "orde"
#include "../../../firmware/components/nucleo_app/app_vs.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_vs(); }


static void start_play(void)
{
    s_gs = GS_START; nh_tap('\n');
    nh_check(s_gs == GS_PLAY, "hero select ENTER did not start a run (gs %d)", s_gs);
}
// Filled pixels of the XP bar (the blue line under the HP bar)
static int xp_bar_px(void)
{
    nh_canvas.fillScreen(0); nh_canvas.drawPixel(0, 0, (uint16_t)C_BLUE); uint32_t blue = nh_canvas.readPixel(0, 0);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int best = 0;
    for (int y = 2; y < 18; y++) { int n = 0; for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) == blue) n++; if (n > best) best = n; }
    return best;
}

static void scen_regress(uint32_t seed)
{
    nh_open_app(seed); start_play();
    VS *w = s_w;
    // XP bar: one gem short of the sim's requirement must not read as a full bar
    w->plevel = 3; w->pending_up = 0;
    int req = 8 + w->plevel * 3;                                 // vs_sim.c's level rule
    w->pxp = req - 1;
    int full = xp_bar_px();
    w->pxp = 0; int empty = xp_bar_px();
    nh_check(full - empty < W - 12, "xp: one gem short of a level, the bar is already full (%d px)", full - empty);
    // a lethal hit on the frame a level-up is due: game over, not the chooser
    w->php = 0; w->pending_up = 1;
    nh_loop_ms(200);
    nh_check(s_gs == GS_OVER, "death + level-up on one frame: gs %d, not game over", s_gs);
    nh_close_app();

    // Esc in play must not throw the run away: it pauses, a second Esc leaves
    nh_open_app(seed); start_play();
    nh_loop_ms(1500); uint32_t f0 = s_w->frame;
    nh_tap('`');
    nh_check(s_gs != GS_MENU, "Esc in play dropped the run straight to the menu");
    nh_close_app();

    // an unreadable record is never overwritten by a worse run
    FILE *f = host_fopen(ORDE_BEST, "wb"); fputs("{\"secs\":xx", f); fclose(f);
    nh_open_app(seed); start_play();
    s_w->frame = 30 * 5; s_w->kills = 1; s_w->php = 0; nh_loop_ms(200);
    nh_check(s_gs == GS_OVER, "run did not end");
    nh_close_app();
    char b[64] = {0}; f = host_fopen(ORDE_BEST, "rb"); fread(b, 1, sizeof b - 1, f); fclose(f);
    nh_check(!strcmp(b, "{\"secs\":xx"), "best: an unreadable record was overwritten with '%s'", b);
    host_remove(ORDE_BEST);
    (void)f0;
}

static int below_footer(void)
{
    nh_canvas.fillScreen(0xF81F); uint32_t sent = nh_canvas.readPixel(0, H - 1);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int n = 0;
    for (int y = H - HINT; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != sent) n++;
    return n;
}
static void screen(const char *name)
{
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s: %d px drawn under the footer", name, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}

// tile_blit.h's direct-to-canvas path must paint exactly what the GFX path paints (incl. clipping at the
// screen edges, flips and odd scales): draw both into canvases and compare every pixel.
static void scen_blit(uint32_t)
{
    TileAtlas a = {};
    nh_check(tile_atlas_load(&a, "/sd/data/Orde/atlas.bin", T_N), "blit: atlas did not load");
    if (!a.c[0]) return;
    M5Canvas c16(&nh_panel); c16.setColorDepth(16); c16.createSprite(W, H);
    LovyanGFX *tg[2] = { &nh_canvas, &c16 };
    for (int k = 0; k < 2; k++) {
        nucleo_app_set_gfx(tg[k]); d.fillScreen((uint16_t)0x0000);
        for (int i = 0; i < 12; i++) tile_blit_op(tile_px(&a, i), -7 + i * 21, i * 11 - 6);
        for (int i = 0; i < 8; i++) tile_blit_key(tile_px(&a, T_TREETOP + (i & 1)), 230 - i * 9, -5 + i * 18, i & 3);
        for (int D = 13; D <= 61; D += 6) tile_blit_sz(tile_px(&a, T_GHOST + (D & 3)), D & 1, (D * 37) % W, (D * 23) % H, D);
        nucleo_app_set_gfx(nullptr);
    }
    int diff = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != c16.readPixel(x, y)) diff++;
    nh_check(diff == 0, "blit: the direct canvas path differs from the GFX path on %d px", diff);
    c16.deleteSprite();
    tile_atlas_free(&a);
}

static void scen_screens(uint32_t seed)
{
    static const char *langs[] = { "it", "de" };
    for (int l = 0; l < 2; l++) {
        g_nh.lang = langs[l]; g_nh.imu = true;
        nh_open_app(seed);
        nh_loop_ms(300); screen("menu");
        nh_tap('.'); nh_tap('\n'); nh_loop_ms(300);
        nh_check(s_gs == GS_SETTINGS, "menu row 2 is not Settings"); screen("settings");
        nh_tap('`'); nh_check(s_gs == GS_MENU, "Esc from settings did not return to the menu");
        nh_tap(';'); nh_tap('\n'); nh_check(s_gs == GS_START, "Play did not open the hero select");
        nh_tap('/'); screen("start");
        nh_tap('\n'); nh_check(s_gs == GS_PLAY && nh_full, "hero select ENTER did not start fullscreen play");
        nh_loop_ms(4000); screen("play");
        s_w->pxp = vs_xp_req(s_w->plevel) - 1;                     // one gem short: next gem levels up
        s_w->gx[0] = s_w->ppx; s_w->gy[0] = s_w->ppy; s_w->galive[0] = 1; s_w->gval[0] = 1;
        nh_run_until([] { return s_gs == GS_LVLANIM; }, 1000);
        nh_loop_ms(150); screen("lvlanim");
        nh_run_until([] { return s_gs == GS_LEVELUP; }, 2000);
        screen("levelup");
        int lv = s_w->plevel; nh_tap('2');
        nh_check(s_gs == GS_PLAY && s_w->pending_up == 0, "level-up pick '2' did not resume play (gs %d)", s_gs);
        (void)lv;
        nh_tap('`'); nh_check(s_gs == GS_PAUSE, "Esc did not pause"); screen("pause");
        nh_tap('\n'); nh_check(s_gs == GS_PLAY, "ENTER did not resume");
        s_w->php = 0; nh_loop_ms(100);
        nh_check(s_gs == GS_OVER, "HP 0 is not game over"); screen("over");
        nh_tap('\n'); nh_check(s_gs == GS_PLAY && s_w->php > 0 && s_w->frame < 10, "ENTER on game over did not start a fresh run");
        nh_tap('`'); nh_tap('`'); nh_check(s_gs == GS_MENU, "Esc, Esc did not return to the title");
        nh_close_app();
    }
    g_nh.lang = "it"; g_nh.imu = false;
}

// Sound: the pack is played (not synthesized on the app task) and nothing is written to the synth cache.
static void scen_sfx(uint32_t seed)
{
    nh_open_app(seed); start_play();
    int plays = nh_plays;
    nh_loop_ms(6000);
    s_w->php = 0; nh_loop_ms(100);
    struct stat st;
    nh_check(nh_plays > plays, "sfx: a 6 s run played no pack sound");
    nh_check(host_stat("/sd/data/Orde/sfx/die.wav", &st) != 0, "sfx: the synth cache was built although the pack is there");
    nh_close_app();
}

// Balance: a kiting bot (steers away from the crowd, takes the first card) per hero, a passive one that
// stands still. Kiting must last, standing still must not; reported for tuning.
static VS *bw;
static void bot_keys(bool kite)
{
    memset(nh_down, 0, sizeof nh_down);
    if (!kite || !bw) return;
    long fx = 0, fy = 0;
    for (int i = 0; i < VS_MAX_EN; i++) {
        if (!bw->ealive[i]) continue;
        int dx = VS_TOINT(bw->ex[i] - bw->ppx), dy = VS_TOINT(bw->ey[i] - bw->ppy), d2 = dx * dx + dy * dy;
        if (d2 > 70 * 70 || d2 == 0) continue;
        fx -= dx * 10000L / d2; fy -= dy * 10000L / d2;
    }
    int best = 1 << 30, gx = 0, gy = 0;                          // and drift toward the nearest gem
    for (int g = 0; g < VS_MAX_GEM; g++) {
        if (!bw->galive[g]) continue;
        int dx = VS_TOINT(bw->gx[g] - bw->ppx), dy = VS_TOINT(bw->gy[g] - bw->ppy), d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; gx = dx; gy = dy; }
    }
    if (best < 120 * 120 && best > 0) { fx += gx * 60 / (1 + (int)sqrtf((float)best)); fy += gy * 60 / (1 + (int)sqrtf((float)best)); }
    if (fx > 40) nh_down['/'] = true; else if (fx < -40) nh_down[','] = true;
    if (fy > 40) nh_down['.'] = true; else if (fy < -40) nh_down[';'] = true;
}
static int bot_run(uint32_t seed, int hero, bool kite, int *kills)
{
    nh_open_app(seed); s_hero = hero; start_play(); bw = s_w;
    int64_t end = nh_us + 300LL * 1000000;
    while (nh_us < end && s_gs != GS_OVER) {
        bot_keys(kite);
        if (s_gs == GS_LEVELUP) nh_tap('\n', 30, 30);
        nh_loop_ms(100);
    }
    memset(nh_down, 0, sizeof nh_down);
    int secs = (int)(s_w->frame / 30); *kills = s_w->kills;
    nh_close_app(); bw = nullptr;
    return secs;
}
static void scen_balance(uint32_t seed)
{
    nh_draw_every = 60;
    int k, idle = bot_run(seed, HERO_MAGO, false, &k);
    nh_note("balance: standing still (Wizard) lasts %d s, %d kills", idle, k);
    nh_check(idle < 300, "balance: standing still survives 5 minutes");
    for (int h = 0; h < HERO_COUNT; h++) {
        int s = bot_run(seed + h, h, true, &k);
        nh_note("balance: kiting %-9s lasts %3d s, %4d kills", hero_name(h), s, k);
        nh_check(s >= 60, "balance: kiting with hero %d dies after %d s", h, s);
    }
    nh_draw_every = 1;
}

static void scen_fuzz(uint32_t seed)
{
    for (int round = 0; round < 3; round++) {
        g_nh.adv = g_nh.imu = (round == 2);
        nh_open_app(seed + round * 101);
        nh_loop_ms(300); nh_tap('\n'); nh_tap('\n');
        nh_fuzz(60000);
        if (round == 0) nh_dump("fuzz");
        for (int i = 0; i < 8 && !nh_exit; i++) nh_tap('`', 60, 300);
        nh_check(nh_exit, "round %d: Esc never left the app", round);
        nh_close_app();
    }
    g_nh.adv = g_nh.imu = false;
}

void nh_scenarios(const char *which, uint32_t seed)
{
    nh_draw_every = 1;
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "regress")) scen_regress(seed);
    if (all || !strcmp(which, "blit"))    scen_blit(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "sfx"))     scen_sfx(seed);
    if (all || !strcmp(which, "balance")) scen_balance(seed);
    if (all || !strcmp(which, "fuzz"))    scen_fuzz(seed);
}
