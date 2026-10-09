// Scorribanda scenarios for the native-games host gate (tools/native-host/run.mjs brawler).
// Every screen in it/de with nothing under the footer and every hint fitting it; title/menu flow and Esc;
// the moves (tap = one strike, combo chain, kick, jump + land, held-key walking); enemies always walking
// IN from off-screen; the cleared street's GO + exit; a result screen that a mashed J cannot skip; the
// respawn shield; a long fuzz on both boards.
#define NH_GAME_NAME "brawler"
#include "../../../firmware/components/nucleo_app/app_brawler.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_brawler(); }

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
static Fighter *hero(void) { return &g.f[0]; }

// Kill every enemy each tick (so the waves roll on) and remember any that appeared ON screen.
static int s_popins, s_spawns;
static bool s_seen[BR_MAXF];
static bool kill_all_until_open(void) {
    for (int i = 2; i < BR_MAXF; i++) {
        if (g.f[i].on && !s_seen[i]) {
            s_spawns++;
            float sx = g.f[i].x - g.camx;
            if (sx > -4 && sx < BR_SW + 4) { s_popins++; nh_note("pop-in: enemy %d at screen x %.0f (camx %.0f)", g.f[i].kind, sx, g.camx); }
        }
        s_seen[i] = g.f[i].on;
        g.f[i].on = false; s_seen[i] = false;
    }
    return g.gatex >= g.level_len;
}
static bool on_clear(void) { return g.screen == SC_CLEAR; }
// The moves scenario runs on an empty street: every tick clears the enemies so nobody interrupts the hero.
static bool clear_street(void) { for (int i = 2; i < BR_MAXF; i++) g.f[i].on = false; return false; }
static void quiet(int ms) { nh_run_until(clear_street, ms); }
static void qtap(char ch, int hold = 40, int after = 60) { nh_key_down(ch); quiet(hold); nh_key_up(ch); quiet(after); }
static void qhold(char ch, int ms) { nh_key_down(ch); quiet(ms); nh_key_up(ch); }
static bool on_over(void) { return g.screen == SC_OVER; }

static void start_solo(int hero_ix) {
    nh_tap('1');                                      // Play
    for (int i = 0; i < hero_ix; i++) nh_tap('/');
    nh_tap('\n');
    nh_loop_ms(200);
}

// Every cue is a WAV of the SD pack (13 of them); nothing is synthesized on the device.
static const char *const CUES[13] = { "nav", "sel", "back", "whiff", "hit", "ko", "hurt", "jump", "clear", "over", "heavy", "block", "go" };
static int pack_files(void) {
    int n = 0;
    for (int i = 0; i < 13; i++) { char p[96]; snprintf(p, sizeof p, "sd/data/brawler/pack/%s.wav", CUES[i]); struct stat st; if (stat(p, &st) == 0 && st.st_size > 44) n++; }
    return n;
}
static void scenario_screens(const char *lang) {
    g_nh.lang = lang;
    nh_check(pack_files() == 13, "the SD pack has every cue (%d of 13)", pack_files());
    nh_open_app(11);
    struct stat st; nh_check(stat("sd/data/brawler/sfx", &st) != 0, "opening the app synthesizes nothing (no sfx cache folder)");
    int pl = nh_plays; nh_tap('.'); nh_check(nh_plays == pl + 1, "a menu move plays the pack's nav cue");
    nh_tap(';');
    screen("menu");
    nh_tap('3'); screen("options");                   // 1-3 quick pick
    nh_check(g.screen == SC_OPT, "digit 3 opens the options");
    nh_tap('.'); nh_tap('.'); nh_tap('\n'); screen("help");
    nh_check(g.screen == SC_HELP, "options row 3 opens the controls");
    nh_tap('`'); nh_check(g.screen == SC_OPT, "Esc in the controls -> options");
    nh_tap('`'); nh_check(g.screen == SC_MENU, "Esc in the options -> title");
    nh_tap('.'); nh_tap('\n'); screen("coop");
    nh_tap('\n'); screen("coop_select");
    nh_tap('\n'); screen("lobby");
    nh_check(g.screen == SC_LOBBY, "co-op host -> lobby");
    nh_tap('`'); nh_check(g.screen == SC_MENU && !g.net, "Esc in the lobby tears the session down -> menu");
    nh_tap('1'); nh_tap('/'); screen("select");
    nh_tap('\n'); nh_loop_ms(1500);
    nh_check(g.screen == SC_PLAY && nh_full, "ENTER on a fighter starts the fight, fullscreen");
    screen("play", false);
    nh_tap('`'); screen("pause", false);
    nh_check(g.screen == SC_PAUSE, "Esc pauses");
    nh_tap('\n'); nh_check(g.screen == SC_PLAY, "ENTER in pause resumes");
    nh_tap('\t'); nh_check(g.screen == SC_PAUSE, "TAB pauses");
    nh_tap('\t'); nh_check(g.screen == SC_PLAY, "TAB resumes");
    // game over
    g.lives = 0; hero()->hp = 0; hero()->st = BS_DOWN; hero()->cool = 0.05f;
    nh_check(nh_run_until(on_over, 3000), "out of lives -> game over");
    screen("over", false);
    nh_tap('\n'); nh_check(g.screen == SC_OVER, "the game-over card ignores keys for a moment");
    nh_loop_ms(500); nh_tap('\n');
    nh_check(g.screen == SC_PLAY && g.lives == 3 && g.score == 0, "ENTER on game over = the same fight again");
    nh_tap('`'); nh_tap('`');
    nh_check(g.screen == SC_MENU, "Esc, Esc leaves the fight for the title");
    nh_tap('`'); nh_check(nh_exit, "Esc on the title leaves the app");
    nh_close_app();
}

static void scenario_moves(void) {
    g_nh.lang = "it";
    nh_open_app(21);
    start_solo(0);
    nh_check(g.screen == SC_PLAY, "solo start");
    Fighter *h = hero();
    quiet(300);
    // tap J = exactly one jab (var 0), a held J does not repeat
    nh_key_down('j'); quiet(40);
    nh_check(h->st == BS_PUNCH && h->var == 0, "J opens a jab (st %d var %d)", h->st, h->var);
    quiet(700); nh_key_up('j'); quiet(80);
    nh_check(h->st == BS_IDLE, "a held J throws one punch only (st %d)", h->st);
    // combo chain: three taps in the chain window -> jab, cross, finisher
    int vars[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++) { nh_key_down('j'); quiet(20); vars[i] = h->var; nh_key_up('j'); quiet(110); }
    nh_check(vars[0] == 0 && vars[1] == 1 && vars[2] == 2, "J J J chains jab-cross-finisher (%d %d %d)", vars[0], vars[1], vars[2]);
    quiet(500);
    // kick + jump + jump-kick
    qtap('k', 40, 20); nh_check(h->st == BS_KICK, "K kicks");
    quiet(500);
    nh_key_down('l'); quiet(60); nh_key_up('l');
    nh_check(h->yoff > 5.0f, "L jumps (yoff %.1f)", h->yoff);
    qtap('j', 30, 10); nh_check(h->st == BS_JKICK, "J in the air = jump-kick (st %d)", h->st);
    quiet(1000); nh_check(h->yoff == 0.0f && h->st == BS_IDLE, "lands (yoff %.1f st %d)", h->yoff, h->st);
    // walking: D held 1 s moves ~speed px to the right, the release stops within a few px
    float x0 = h->x; qhold('d', 1000); float x1 = h->x; quiet(300);
    nh_check(x1 - x0 > 55 && x1 - x0 < 85, "D held 1 s walks %.0f px (speed 78)", x1 - x0);
    nh_check(h->x - x1 < 6, "releasing D stops (coasted %.1f px)", h->x - x1);
    float z0 = h->z; qhold('e', 300); nh_check(h->z < z0 - 0.15f, "E walks up the street (z %.2f -> %.2f)", z0, h->z);

    // respawn: a KO'd hero with lives left comes back at full hp, shielded for a moment
    int lives = g.lives;
    h->hp = 0; h->st = BS_DOWN; h->anim = 1; h->cool = 0.05f;
    nh_loop_ms(200);
    nh_check(g.lives == lives - 1 && h->hp == h->maxhp, "respawn spends a life (lives %d hp %d)", g.lives, h->hp);
    nh_check(br_invuln(h), "the respawned hero is shielded");
    // an enemy swinging in range lands nothing while shielded, then hits once the shield is gone
    for (int i = 2; i < BR_MAXF; i++) g.f[i].on = false;
    Fighter *e = brawler_spawn_enemy(0, h->x + 18, h->z);
    e->dir = -1; e->st = BS_PUNCH; e->anim = 0.35f; e->hit_done = false;
    int hp0 = h->hp; combat_resolve(0.016f);
    nh_check(h->hp == hp0, "the shield blocks a hit (hp %d -> %d)", hp0, h->hp);
    quiet(2200);
    nh_check(!br_invuln(h), "the shield wears off");
    e->on = true; e->x = h->x + 18; e->z = h->z; e->dir = -1; e->st = BS_PUNCH; e->anim = 0.35f; e->hit_done = false; e->hp = e->maxhp;
    h->st = BS_IDLE; hp0 = h->hp; combat_resolve(0.016f);
    nh_check(h->hp < hp0, "after the shield a hit lands (hp %d -> %d)", hp0, h->hp);
    nh_close_app();
}

static void scenario_street(void) {
    g_nh.lang = "it";
    nh_open_app(31);
    start_solo(2);
    nh_check(g.banner == 0 && g.banner_t > 1.0f, "a street opens on its stage card");
    nh_dump("intro_it");
    for (int i = 2; i < BR_MAXF; i++) g.f[i].on = false;   // the ones already walking in are legit
    memset(s_seen, 0, sizeof s_seen); s_popins = s_spawns = 0;
    nh_check(nh_run_until(kill_all_until_open, 60000), "the waves of street 1 roll through");
    nh_check(s_spawns >= 5 && s_popins == 0, "%d of %d enemies popped in on screen", s_popins, s_spawns);
    nh_loop_ms(400); screen("go", false);
    // walk to the exit while mashing J: the clear screen must survive the mash
    nh_key_down('d');
    bool ok = false;
    for (int t = 0; t < 200 && !ok; t++) { nh_tap('j', 20, 300); ok = (g.screen == SC_CLEAR); }
    nh_key_up('d');
    nh_check(ok, "walking right past the open gate clears the street (x %.0f / %.0f)", hero()->x, g.level_len);
    nh_tap('j', 30, 30); nh_tap(' ', 30, 30);
    nh_check(g.screen == SC_CLEAR, "a J/Space mashed into the clear screen does not skip it");
    nh_loop_ms(800); screen("clear", false);
    nh_tap('j');
    nh_check(g.screen == SC_PLAY && g.level == 1, "J (= ENTER) after the pause continues to street 2");
    // later streets: enemies still walk in from off-screen
    memset(s_seen, 0, sizeof s_seen); s_popins = s_spawns = 0;
    nh_run_until(kill_all_until_open, 60000);
    nh_check(s_popins == 0, "street 2: %d of %d enemies popped in", s_popins, s_spawns);
    nh_close_app();
}

static void scenario_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = (round == 1);
        g_nh.lang = round ? "fr" : "es";
        nh_open_app(seed + round * 101);
        start_solo(round);
        // mash the fight keys like a player: held WASD + J/K/L
        static const char keys[] = "jjjkkllwasdeee ";
        for (int t = 0; t < 1200 && !nh_exit; t++) {
            char ch = keys[nh_jitter((int)sizeof keys - 1)];
            nh_key_down(ch); nh_loop_ms(20 + nh_jitter(200)); nh_key_up(ch);
            if (g.screen == SC_OVER) { nh_loop_ms(800); nh_tap('\n'); }
            if (g.screen == SC_CLEAR) { nh_loop_ms(800); nh_tap('\n'); }
            nh_check_hint("fuzz");
        }
        nh_fuzz(20000);
        nh_dump(round ? "fuzz_adv" : "fuzz");
        for (int i = 0; i < 8 && !nh_exit; i++) nh_tap('`', 60, 300);   // pause -> title -> out (lock-timed cards wait)
        nh_check(nh_exit, "round %d: Esc never left the app (screen %d)", round, g.screen);
        nh_close_app();
    }
}

// A fighting bot: walks onto the nearest foe's lane, faces it, throws J-J-J strings and the odd kick, backs off
// when low. Every decision is a real key press (held movement keys, tapped attacks) on the ~50 Hz loop.
static Fighter *nearest_enemy(void) {
    Fighter *h = hero(), *best = nullptr; float bd = 1e9f;
    for (int i = 2; i < BR_MAXF; i++) { Fighter *e = &g.f[i]; if (!e->on || e->hp <= 0) continue;
        float dd = fabsf(e->x - h->x) + fabsf(e->z - h->z) * 200; if (dd < bd) { bd = dd; best = e; } }
    return best;
}
static char s_held[2];
static void bot_hold(int slot, char k) { if (s_held[slot] == k) return; if (s_held[slot]) nh_key_up(s_held[slot]); s_held[slot] = k; if (k) nh_key_down(k); }
static int s_dumped, s_hits_taken;
static void fight_bot(int ms, const char *tag) {
    int64_t end = nh_us + (int64_t)ms * 1000; int t = 0, lasthp = hero()->hp;
    while (nh_us < end && g.screen == SC_PLAY) {
        Fighter *h = hero(), *e = nearest_enemy();
        if (h->hp < lasthp) s_hits_taken++;
        lasthp = h->hp;
        char mx = 0, mz = 0;
        if (e) {
            float dx = e->x - h->x, dz = e->z - h->z, want = 22.0f;
            if (fabsf(dz) > 0.06f) mz = dz > 0 ? 's' : 'e';
            if (fabsf(dx) > want + 6) mx = dx > 0 ? 'd' : 'a';
            else if (fabsf(dx) < 10) mx = dx > 0 ? 'a' : 'd';
            else if ((dx > 0) != (h->dir > 0)) mx = dx > 0 ? 'd' : 'a';           // turn to face it
            if (!mx && fabsf(dz) <= 0.08f && (t % 3) == 0) { nh_key_down((t % 12) == 9 ? 'k' : 'j'); nh_loop_ms(20); nh_key_up((t % 12) == 9 ? 'k' : 'j'); }
        } else if (g.gatex > h->x + 40) mx = 'd';
        bot_hold(0, mx); bot_hold(1, mz);
        if (getenv("NH_VERBOSE") && t % 20 == 0 && t < 400) {
            nh_note("t %d hero x %.0f z %.2f st %d dir %d | cam %.0f gate %.0f", t, h->x, h->z, h->st, h->dir, g.camx, g.gatex);
            for (int i = 2; i < BR_MAXF; i++) if (g.f[i].on) nh_note("   e%d k%d x %.0f z %.2f st %d hp %d", i, g.f[i].kind, g.f[i].x, g.f[i].z, g.f[i].st, g.f[i].hp);
        }
        // show the fight: a few frames at telling moments
        if (s_dumped < 6 && e && ((s_dumped == 0 && h->st == BS_PUNCH) || (s_dumped == 1 && e->st == BS_HIT) ||
                                  (s_dumped == 2 && g.combo >= 3) || (s_dumped == 3 && e->st == BS_DOWN) ||
                                  (s_dumped == 4 && h->st == BS_HIT) || (s_dumped == 5 && e->st == BS_PUNCH))) {
            char b[40]; snprintf(b, sizeof b, "%s_%d", tag, s_dumped++); nh_dump(b);
        }
        nh_loop_ms(60); t++;
    }
    bot_hold(0, 0); bot_hold(1, 0);
}
static void scenario_fight(void) {
    g_nh.lang = "it";
    for (int hk = 0; hk < 3; hk++) {
        nh_open_app(41 + hk);
        start_solo(hk);
        s_dumped = hk == 0 ? 0 : 6; s_hits_taken = 0;
        int lives0 = g.lives; int64_t t0 = nh_us;
        fight_bot(150000, "fight");
        int secs = (int)((nh_us - t0) / 1000000);
        nh_note("hero %d: street %d wave %d after %d s, score %ld, hit %d times, lives %d -> %d, screen %d",
                hk, g.level + 1, g.wave + 1, secs, g.score, s_hits_taken, lives0, g.lives, g.screen);
        nh_check(g.level > 0 || g.screen == SC_CLEAR || g.gatex >= g.level_len, "hero %d: the bot clears street 1 (wave %d)", hk, g.wave + 1);
        nh_close_app();
    }
}

// Enemy AI + fairness: a foe keeps coming back after its swing (it used to retreat forever after the first
// one: a stalemate at the gate), every swing is telegraphed, two foes cannot stun-lock the hero, the boss
// walks in under its banner with its own bar.
static int s_swings; static BrState s_prev_st;
static bool count_swings(void) {
    Fighter *e = &g.f[2];
    if (e->on && e->st == BS_PUNCH && s_prev_st != BS_PUNCH) s_swings++;
    s_prev_st = e->on ? e->st : BS_IDLE;
    Fighter *h = hero(); h->hp = h->maxhp;                  // keep the hero standing
    return false;
}
static void scenario_ai(void) {
    g_nh.lang = "it";
    nh_open_app(51);
    start_solo(0);
    for (int i = 2; i < BR_MAXF; i++) g.f[i].on = false;
    g.wave = 99;                                              // no more waves: just this foe (levels_step idles)
    Fighter *e = brawler_spawn_enemy(0, hero()->x + 40, hero()->z);
    s_swings = 0; s_prev_st = BS_IDLE;
    nh_run_until(count_swings, 12000);
    nh_check(e->on && s_swings >= 4, "a thug keeps attacking a hero who stands still (%d swings in 12 s)", s_swings);
    // the tell: an enemy punch spends >= 120 ms winding up before its active window
    combat_begin_attack(e, BS_PUNCH);
    float tell = 0.28f / e->aspd;
    nh_check(tell >= 0.12f, "an enemy swing telegraphs %.0f ms before it can land (>= 120)", tell * 1000);
    // mercy frames: two foes swinging into the hero in the same tick land one hit, not two
    Fighter *e2 = brawler_spawn_enemy(0, hero()->x - 18, hero()->z);
    Fighter *h = hero(); h->hp = h->maxhp; h->cool = 0; h->st = BS_IDLE;
    e->x = h->x + 18; e->z = h->z; e->dir = -1; e->st = BS_PUNCH; e->anim = 0.35f; e->hit_done = false;
    e2->dir = 1; e2->st = BS_PUNCH; e2->anim = 0.35f; e2->hit_done = false;
    combat_resolve(0.016f);
    nh_check(h->hp == h->maxhp - 6, "two simultaneous blows land once (hp %d of %d)", h->hp, h->maxhp);
    // the boss
    for (int i = 2; i < BR_MAXF; i++) g.f[i].on = false;
    brawler_spawn_enemy(3, g.camx + BR_SW - 30, 0.6f);
    nh_check(g.banner == 1 && g.banner_t > 1.5f, "the boss walks in under the WARNING banner");
    quiet(1); g.f[2].on = true; nh_loop_ms(500);
    nh_dump("boss_it");
    nh_close_app();
}

void nh_scenarios(const char *which, uint32_t seed) {
    if (!strcmp(which, "fight")) { scenario_fight(); return; }
    if (!strcmp(which, "ai")) { scenario_ai(); return; }
    if (!strcmp(which, "all")) { scenario_ai(); scenario_fight(); }
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) { scenario_screens("it"); scenario_screens("de"); scenario_screens("en"); }
    if (all || !strcmp(which, "moves")) scenario_moves();
    if (all || !strcmp(which, "street")) scenario_street();
    if (all || !strcmp(which, "fuzz")) scenario_fuzz(seed);
}
