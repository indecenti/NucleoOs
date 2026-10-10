// Costellazioni scenarios for the native-games host gate (tools/native-host/run.mjs stelle).
// Every screen in two languages (clear of the footer, hints that fit), and the save rules: a death wipes the
// run at once (a quit during the "game over" cinematic used to save a hull-0 ship), Continue on an unreadable
// save never starts over on top of it, a write that fails keeps the old save, the web endpoint refuses a
// partial save, "Delete save" asks twice, Esc in a dogfight asks twice, sound comes from the pack.
#define NH_GAME_NAME "stelle"
#include "../../../firmware/components/nucleo_app/app_constellations.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_constellations(); }

#define SAVE "/sd/data/costellazioni/save.bin"
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
static bool exists(const char *p) { struct stat st; return host_stat(p, &st) == 0; }
static long fsize(const char *p) { struct stat st; return host_stat(p, &st) == 0 ? (long)st.st_size : -1; }
static void wipe(void) { host_remove(SAVE); host_remove("/sd/data/costellazioni/save.bak"); host_remove("/sd/data/costellazioni/save.bin.tmp"); }
// title -> New game -> skip the intro -> docked at the first system
static void new_run(void)
{
    s_tmenu.sel = s_has_save ? 1 : 0; nh_tap('\n');
    nh_check(s_screen == ST_CINE && s_cine == CINE_INTRO, "New game did not start the intro");
    nh_tap(' ');
    nh_check(s_screen == ST_SYSTEM, "a key did not skip the intro (screen %d)", s_screen);
}
static void hub(int i) { s_hubsel = i; nh_tap('\n'); }
static void dock_with_missions(void)
{
    for (int k = 0; k < NSYS; k++) { int el[NMISS_PER_SYS]; if (eligible_missions(el) > 0) return; g.sys = (g.sys + 1) % NSYS; }
}

static void scen_screens(uint32_t seed)
{
    static const char *langs[] = { "it", "de" };
    for (int l = 0; l < 2; l++) {
        g_nh.lang = langs[l]; g_nh.adv = g_nh.imu = (l == 1); wipe();
        nh_open_app(seed);
        nh_loop_ms(300); screen("title");
        s_tmenu.sel = 1; nh_tap('\n'); nh_check(s_screen == ST_SETTINGS, "title row 2 is not Settings"); nh_loop_ms(300); screen("settings");
        nh_tap('`');
        s_tmenu.sel = 0; nh_tap('\n'); nh_loop_ms(2600); screen("intro");
        nh_tap(' '); nh_check(s_screen == ST_SYSTEM, "intro skip"); screen("system");
        hub(0); screen("market"); nh_tap('`');
        hub(1); screen("shipyard"); nh_tap('`');
        hub(3); screen("bridge"); nh_tap('`');
        hub(4); nh_tap('.'); nh_loop_ms(300); screen("map"); nh_tap('`');
        dock_with_missions();
        hub(2); nh_check(s_screen == ST_MISSIONS, "hub row 3 is not the Mission Bay"); screen("missions");
        nh_tap('\n'); nh_check(s_screen == ST_BRIEF, "ENTER on a contract did not open the brief"); screen("brief");
        nh_tap('\n'); nh_check(s_screen == ST_COMBAT && nh_full, "Accept did not launch fullscreen combat");
        nh_loop_ms(1500); screen("combat");
        nh_tap('`'); nh_check(s_screen == ST_COMBAT, "one Esc in combat already fled");
        nh_loop_ms(200); screen("flee");
        nh_tap('`'); nh_check(s_screen == ST_DEBRIEF, "a second Esc did not disengage (screen %d)", s_screen);
        screen("debrief");
        nh_tap('\n');
        start_event(EV_PIRATI); screen("event");
        nh_tap('\n'); nh_check(s_screen != ST_EVENT, "an event choice did not resolve");
        nh_close_app();
    }
    g_nh.lang = "it"; g_nh.adv = g_nh.imu = false;
}

// A death ends the run on the spot: the save is wiped and nothing (not even quitting during the cinematic)
// writes the dead ship back.
static void scen_death(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run();
    nh_check(exists(SAVE), "death: the docked run was never saved");
    dock_with_missions(); hub(2); nh_tap('\n'); nh_tap('\n');
    nh_check(s_screen == ST_COMBAT, "death: no combat");
    s_shield = 0; g.hull = 1; hurt_player(50);
    nh_check(s_screen == ST_CINE && s_cine == CINE_LOSE, "death: no game-over cinematic");
    nh_check(!exists(SAVE), "death: the save survived the death");
    nh_close_app();                                               // quit during the cinematic
    nh_check(!exists(SAVE), "death: quitting during the game-over cinematic saved the dead run");
    nh_open_app(seed);
    nh_check(!s_has_save, "death: Continue is offered for a dead run");
    nh_close_app();
}

// Saves: an unreadable save is reported and survives Continue; a save swap that dies half-way is recovered
// from save.bak; the web endpoint refuses a partial body and accepts a whole one.
static void scen_saves(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run(); g.credits = 4321; hub(5);
    nh_check(s_screen == ST_TITLE && exists(SAVE), "Save & Title did not save");
    nh_close_app();
    long full = fsize(SAVE);
    // truncate it: unreadable
    FILE *f = host_fopen(SAVE, "r+b"); char head[16]; fread(head, 1, sizeof head, f); fclose(f);
    f = host_fopen(SAVE, "wb"); fwrite(head, 1, sizeof head, f); fclose(f);
    nh_open_app(seed);
    nh_check(!s_has_save && s_save_bad, "saves: a truncated save was not flagged");
    nh_loop_ms(200); screen("title_badsave");
    nh_tap('`');
    nh_close_app();
    nh_check(fsize(SAVE) == 16, "saves: the unreadable save was rewritten (%ld B)", fsize(SAVE));
    // a swap that died after moving the old save aside: save.bak is used
    wipe(); nh_open_app(seed); new_run(); g.credits = 777; hub(5); nh_close_app();
    host_rename(SAVE, "/sd/data/costellazioni/save.bak");
    nh_open_app(seed);
    nh_check(s_has_save, "saves: save.bak was not recovered");
    s_tmenu.sel = 0; nh_tap('\n');
    nh_check(s_screen == ST_SYSTEM && g.credits == 777, "saves: continue from save.bak gave %d cr", g.credits);
    nh_close_app();
    nh_check(fsize(SAVE) == full, "saves: playing on from save.bak did not write save.bin back");
    // the web endpoint
    nucleo_app_register_costellazioni_api(nullptr);
    httpd_uri_t *post = nullptr, *get = nullptr;
    for (int i = 0; i < nh_nuris; i++) { if (nh_uris[i].method == HTTP_POST) post = &nh_uris[i]; else if (strstr(nh_uris[i].uri, "/save")) get = &nh_uris[i]; }
    nh_check(post && get, "endpoint: save handlers not registered");
    if (!post || !get) return;
    static httpd_req_t rq;
    memset(&rq, 0, sizeof rq); rq.uri = "/api/game/costellazioni/save"; rq.method = HTTP_GET;
    get->handler(&rq);
    nh_check(strstr(rq.out, "\"credits\":777") != nullptr, "endpoint: GET did not return the save (%.60s)", rq.out);
    static char whole[1024]; snprintf(whole, sizeof whole, "%s", rq.out);
    const char *partial = "{\"credits\":5}";
    memset(&rq, 0, sizeof rq); rq.method = HTTP_POST; rq.body = partial; rq.content_len = strlen(partial);
    post->handler(&rq);
    nh_check(strstr(rq.status, "400") != nullptr, "endpoint: a partial save was accepted (%s)", rq.out);
    memset(&rq, 0, sizeof rq); rq.method = HTTP_GET; get->handler(&rq);
    nh_check(strstr(rq.out, "\"credits\":777") != nullptr, "endpoint: the partial POST changed the save");
    char *c = strstr(whole, "\"credits\":777"); if (c) memcpy(c, "\"credits\":778", 13);
    memset(&rq, 0, sizeof rq); rq.method = HTTP_POST; rq.body = whole; rq.content_len = strlen(whole);
    post->handler(&rq);
    nh_check(strstr(rq.out, "\"ok\":true") != nullptr, "endpoint: a whole save was refused (%s %s)", rq.status, rq.out);
}

// "Delete save" asks twice; moving away disarms it.
static void scen_delete(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run(); hub(5);
    s_tmenu.sel = 2; nh_tap('\n'); nh_check(s_screen == ST_SETTINGS, "no settings");
    s_smenu.sel = set_count() - 1; nh_tap('\n');
    nh_check(exists(SAVE) && s_del_armed, "delete: one ENTER already deleted the save");
    screen("delete_confirm");
    nh_tap(';'); nh_check(!s_del_armed, "delete: moving the cursor did not disarm");
    s_smenu.sel = set_count() - 1; nh_tap('\n'); nh_tap('\n');
    nh_check(!exists(SAVE) && !s_has_save, "delete: two ENTERs did not delete the save");
    nh_close_app();
}

// Sound: the pack is played, never synthesized (no cache written).
static void scen_sfx(uint32_t seed)
{
    wipe(); nh_open_app(seed);
    int plays = nh_plays;
    new_run(); hub(0); nh_tap('\n'); nh_tap('`');
    nh_check(nh_plays > plays, "sfx: no pack sound played");
    nh_check(!exists("/sd/data/costellazioni/sfx/ok.wav") && !exists("/sd/data/costellazioni/sfx/ok.v2.wav"), "sfx: a cue was synthesized on the app task");
    nh_close_app();
}

// Economy: buying then selling at the same port never makes money; a ship with no credits, no cargo and no
// fuel for any jump is towed (never soft-locked); the market can't be drained below zero.
static void scen_economy(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run();
    int gain = 0;
    for (uint32_t ep = 1; ep < 40; ep++) { g.epoch = ep;
        for (int sy = 0; sy < NSYS; sy++) for (int gd = 0; gd < NGOODS; gd++) if (unit_sell(sy, gd) > unit_buy(sy, gd)) gain++; }
    nh_check(gain == 0, "economy: %d port/good/epoch combos sell above the buy price (money loop)", gain);
    hub(0);
    int c0 = g.credits;
    for (int k = 0; k < 30; k++) { s_mktsel = 2; nh_tap('\n'); nh_tap('s'); }
    nh_check(g.credits <= c0, "economy: buy+sell x30 grew credits %d -> %d", c0, g.credits);
    nh_tap('`');
    // stranded: no credits, an empty hold, no fuel -> the fuel row tows a full tank
    g.credits = 0; for (int i = 0; i < NGOODS; i++) g.cargo[i] = 0; g.fuel = 0;
    hub(0); s_mktsel = NGOODS; nh_tap('\n');
    nh_check(g.fuel == g.fuel_max, "economy: a stranded ship was not rescued (fuel %d)", g.fuel);
    screen("rescue");
    nh_tap('`');
    // with cargo to sell it is not towed: selling pays for fuel
    g.fuel = 0; g.cargo[G_GRANO] = 3; s_mktsel = NGOODS; hub(0); nh_tap('\n');
    nh_check(g.fuel == 0, "economy: towed although the hold could be sold");
    nh_close_app();
}

// Combat balance: a competent bot pilot (steers onto the nearest foe, holds fire, fires a missile on a lock)
// flies the first contract of a sector. Reports win rate, sortie length and hull left per sector tier.
static int g_min_shield = 999, g_max_bolts = 0;
static int bot_mission(uint32_t seed, int sector, int slot, int *secs, int *hull, int *waves)
{
    wipe(); nh_open_app(seed); new_run();
    g.sector = (uint32_t)sector; regen_sector(); g.sys = entry_slot(g.sector); dock_with_missions();
    int up = sector / 2 > 4 ? 4 : sector / 2;                     // a modest upgrade path: one level every two sectors
    g.weapon = up; g.shield_max = 40 + 30 * up;
    g.hull_max = 100 + 25 * up; g.hull = g.hull_max;
    s_pick = slot; combat_begin_mission(slot); *waves = s_cc.waves;
    int64_t t0 = nh_us, next_look = 0; int best = -1;
    while (s_screen == ST_COMBAT && nh_us - t0 < 900LL * 1000000) {
        float bd = 1e9f, bx = 0, by = 0;
        if (nh_us >= next_look) { best = -1; next_look = nh_us + 250000; }   // a human re-picks a target ~4x/s
        for (int i = 0; i < NFOE; i++) {
            if (best >= 0 && i != best) continue;
            if (best < 0 && s_ward_on && i % 3 != 1) { bool hunter = false; for (int k = 1; k < NFOE; k += 3) hunter |= s_foe[k].on; if (hunter) continue; }   // marked ward hunters first
            float sx, sy, sc; if (!s_foe[i].on || !project(s_foe[i].ex, s_foe[i].ey, s_foe[i].ez, &sx, &sy, &sc)) continue;
            float dd = (sx - s_aimx) * (sx - s_aimx) + (sy - s_aimy) * (sy - s_aimy);
            if (dd < bd) { bd = dd; best = i; bx = sx; by = sy; }
        }
        if (best >= 0 && !s_foe[best].on) best = -1;
        if (best >= 0) {
            if (bx < s_aimx - 8) aim_steer(0, -1); else if (bx > s_aimx + 8) aim_steer(0, +1);
            if (by < s_aimy - 8) aim_steer(1, -1); else if (by > s_aimy + 8) aim_steer(1, +1);
        }
        player_fire();
        if (s_lock >= 0 && s_msl_ammo > 0 && (nh_us / 1000000) % 3 == 0) missile_fire();
        nh_loop_ms(90);                                            // the keyboard repeat rate
        if (s_shield < g_min_shield) g_min_shield = s_shield;
        int nb = 0; for (int i = 0; i < NBOLT; i++) nb += s_bolt[i].on; if (nb > g_max_bolts) g_max_bolts = nb;
    }
    *secs = (int)((nh_us - t0) / 1000000); *hull = g.hull;
    int r = s_screen == ST_DEBRIEF ? s_result : (s_screen == ST_CINE ? 2 : 0);
    if (r != 1) nh_note("  lost: sector %d type %d (ward hp %d/%d) after %d s, result %d", sector, s_cc.type, s_ward_hp, s_ward_max, *secs, r);
    nh_close_app();
    return r;
}
static void scen_balance(uint32_t seed)
{
    nh_draw_every = 30;
    static const int SECT[3] = { 0, 2, 5 };
    for (int k = 0; k < 3; k++) {
        int wins = 0, dead = 0, tsum = 0, hsum = 0, wsum = 0, n = 4;
        for (int r = 0; r < n; r++) {
            int secs, hull, waves, res = bot_mission(seed + r * 17 + k, SECT[k], r % 3, &secs, &hull, &waves);
            wins += res == 1; dead += res == 2; tsum += secs; hsum += res == 1 ? hull : 0; wsum += waves;
        }
        nh_note("balance: sector %d  bot wins %d/%d, died %d, sortie %d s avg (%d waves avg), hull left %d avg, min shield %d, max bolts %d", SECT[k], wins, n, dead,
                tsum / n, wsum / n, wins ? hsum / wins : 0, g_min_shield, g_max_bolts);
        g_min_shield = 999; g_max_bolts = 0;
        if (k == 0) nh_check(wins >= n - 1, "balance: a competent pilot loses first-sector contracts (%d/%d won)", wins, n);
    }
    nh_draw_every = 1;
}

static void scen_fuzz(uint32_t seed)
{
    for (int round = 0; round < 3; round++) {
        g_nh.adv = g_nh.imu = (round == 2);
        wipe(); nh_open_app(seed + round * 101);
        nh_loop_ms(300); nh_tap('\n'); nh_tap(' ');
        nh_fuzz(60000);
        if (round == 0) nh_dump("fuzz");
        for (int i = 0; i < 10 && !nh_exit; i++) nh_tap('`', 60, 300);
        nh_check(nh_exit, "round %d: Esc never left the app", round);
        nh_close_app();
    }
    g_nh.adv = g_nh.imu = false;
}

void nh_scenarios(const char *which, uint32_t seed)
{
    nh_draw_every = 1;
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "death"))   scen_death(seed);
    if (all || !strcmp(which, "saves"))   scen_saves(seed);
    if (all || !strcmp(which, "delete"))  scen_delete(seed);
    if (all || !strcmp(which, "sfx"))     scen_sfx(seed);
    if (all || !strcmp(which, "economy")) scen_economy(seed);
    if (all || !strcmp(which, "balance")) scen_balance(seed);
    if (all || !strcmp(which, "fuzz"))    scen_fuzz(seed);
}
