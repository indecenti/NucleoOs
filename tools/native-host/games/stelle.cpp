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
// Every screen is checked in all five languages (footer clear, hint fits, no text cut to fit its room —
// s_overflow counts the cuts); the it/de frames are saved for review (de has the longest strings).
static void screen(const char *name)
{
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s: %d px drawn under the footer", name, below_footer());
    s_overflow = 0;
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang);
    if (!strcmp(g_nh.lang, "it") || !strcmp(g_nh.lang, "de")) nh_dump(b);
    else nh_draw_frame();
    nh_check(s_overflow == 0, "%s: %d text run(s) did not fit their room", b, s_overflow);
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
    static const char *langs[] = { "it", "de", "en", "es", "fr" };
    for (int l = 0; l < 5; l++) {
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
    s_shield = 0; g.hull = 1; hurt_player(50, CX + 30, s_cy);
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
    if (r != 1) nh_note("  lost: sector %d type %d%s (ward hp %d/%d) after %d s, result %d, wave %d/%d", sector, s_cc.type, s_cc.ace ? " +ace" : "",
                        s_ward_hp, s_ward_max, *secs, r, s_wave, s_cc.waves);
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

// The dogfight gallery: every faction's wing and its named ace (frozen at readable spots, the ace mid-telegraph),
// an explosion mid-bloom, the Keeper's shield and the Echo's phase. Checks the ambush faction rules and that an
// explosion's particles, fireball and ring all burn out.
static void stage(int lock)
{
    static const float X[NFOE] = { -50, 4, 46, -34, 34, 10 }, Y[NFOE] = { -10, 14, 2, 30, 26, -16 }, Z[NFOE] = { 60, 48, 64, 80, 98, 72 };
    for (int i = 0; i < NFOE; i++) if (s_foe[i].on) {
        Foe *f = &s_foe[i]; f->ex = X[i]; f->ey = Y[i]; f->ez = Z[i]; f->engagez = Z[i]; f->vx = (i & 1 ? 40.0f : -30.0f); f->vy = 8; f->vz = 0;
        f->bank = (i & 1 ? 0.6f : -0.5f); f->strafe = 0; f->strafecd = 3000; f->hitms = 0;
    }
    float sx, sy, sc;
    if (lock >= 0 && project(s_foe[lock].ex, s_foe[lock].ey, s_foe[lock].ez, &sx, &sy, &sc)) { s_aimx = sx + 6; s_aimy = sy + 1; s_lock = lock; }
    s_shield = s_shieldmax; g.hull = g.hull_max; s_fx->cmsg_until = 0; s_fx->dmg_until = 0;
}
static void scen_gallery(uint32_t seed)
{
    static const char *nm[4] = { "gilda", "custodi", "relitti", "eco" };
    static const int ace[4] = { ACE_DAX, ACE_VIGIL, ACE_GUTTER, ACE_WARDEN };
    for (int fct = 0; fct < 4; fct++) {
        g_nh.lang = fct == 3 ? "de" : "it";
        wipe(); nh_open_app(seed + fct); new_run();
        SYSTEMS[g.sys].faction = fct == F_RELITTI ? F_GILDA : fct;       // the sky we fight under
        if (fct == F_GILDA || fct == F_CUSTODI) g.rep[fct] = -40;         // crossed them: their patrols hunt you
        combat_begin_ambush();
        nh_check(s_cc.foe_fac == fct, "gallery: an ambush at a %s station came from faction %d", nm[fct], s_cc.foe_fac);
        nh_loop_ms(3700);                                                 // past the opening legend
        for (int i = 0; i < NFOE; i++) s_foe[i].on = 0;
        s_fx->ace_id = (uint8_t)ace[fct];
        spawn_foe(FOE_FIGHTER, 50); spawn_foe(FOE_HEAVY, 90); spawn_foe(FOE_SCOUT, 30); spawn_foe(FOE_FIGHTER, 50); spawn_foe(FOE_HEAVY, 90); spawn_foe(FOE_ACE, 150);
        s_foe[5].ph = ACE_CHARGE; s_foe[5].phms = 380; s_foe[0].firecd = 200;
        stage(1);
        if (fct == F_RELITTI) { s_fx->comm_until = nh_us / 1000 + 2000; s_fx->comm_id = ACE_GUTTER; }   // her radio line
        char b[40]; snprintf(b, sizeof b, "combat_%s", nm[fct]); nh_dump(b);
        if (fct == F_RELITTI) {                                           // a won contract: pay, standing, the ace's fall
            s_mission = 0; s_cc.rep_fac = F_GILDA; s_cc.rep_gain = 4; s_cc.enemy_rep_fac = F_RELITTI; s_cc.enemy_rep_loss = 2;
            s_fx->ace_down = 1; s_mkills = 9; combat_end(1);
            nh_check(s_screen == ST_DEBRIEF, "gallery: a win did not debrief");
            nh_check(!(g.flags & bit(FL_ACE_DEAD)), "gallery: FL_ACE_DEAD set without a kill");
            nh_dump("debrief_win");
            nh_close_app(); continue;
        }
        if (fct == F_CUSTODI || fct == F_ECO) {                           // the ace's guard: immune, untargetable
            s_foe[5].ph = ACE_EVADE; s_foe[5].phms = 600; s_lock = 5;
            int hp = s_foe[5].hp; s_pfire_ms = 0; player_fire();
            nh_check(s_foe[5].hp == hp, "gallery: the %s ace took a hit through its guard", nm[fct]);
            snprintf(b, sizeof b, "combat_%s_guard", nm[fct]); nh_dump(b);
        }
        if (fct == F_GILDA) {                                             // a kill: fireball, ring, debris, shards
            float sx, sy, sc; project(s_foe[1].ex, s_foe[1].ey, s_foe[1].ez, &sx, &sy, &sc);
            kill_foe(&s_foe[1], sx, sy, sc, true);
            int maxlife = 0; for (int i = 0; i < NPART; i++) if (s_part[i].on && s_part[i].life0 > maxlife) maxlife = s_part[i].life0;
            nh_check(maxlife <= 700, "gallery: a particle lives %d ms", maxlife);
            s_foe[0].on = s_foe[2].on = s_foe[3].on = s_foe[4].on = s_foe[5].on = 0; s_wave_left = 1; s_spawn_timer = 1e9f;   // no win, no next wave
            int64_t t0 = nh_us; while (nh_us - t0 < 110000) nh_tick(18);
            nh_dump("combat_boom");
            t0 = nh_us; while (nh_us - t0 < 1500000 && s_screen == ST_COMBAT) { s_spawn_timer = 1e9f; nh_tick(18); }
            int on = 0; for (int i = 0; i < NPART; i++) on += s_part[i].on; for (int i = 0; i < NSHK; i++) on += s_shk[i].on;
            nh_check(on == 0, "gallery: %d explosion particles/rings still alive 1.5 s after the kill", on);
        }
        nh_close_app();
    }
    g_nh.lang = "it";
}

// The map's beacon threads: a spanning tree over each sector's beacons (every beacon joined, beacons only), and gold
// (COL_THREAD, a colour nothing else on the map uses) only where BOTH ends are lit.
static int count_col(uint16_t c)
{
    nh_canvas.drawPixel(0, 0, c); uint32_t want = nh_canvas.readPixel(0, 0);
    nh_draw_frame();
    int n = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) == want) n++;
    return n;
}
static void scen_threads(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run();
    for (int sec = 0; sec < 9; sec++) {
        g.sector = (uint32_t)sec; regen_sector();
        uint8_t e[NSYS][2]; int ne = map_threads(e), nb = beacons_total(), root[NSYS];
        nh_check(ne == (nb ? nb - 1 : 0), "threads: sector %d has %d beacons and %d threads", sec, nb, ne);
        for (int i = 0; i < NSYS; i++) root[i] = i;
        for (int k = 0; k < ne; k++) {
            nh_check(SYSTEMS[e[k][0]].beacon && SYSTEMS[e[k][1]].beacon, "threads: sector %d joins a system without a beacon", sec);
            int a = e[k][0], b = e[k][1]; while (root[a] != a) a = root[a]; while (root[b] != b) b = root[b]; root[a] = b;
        }
        int comp = 0; for (int i = 0; i < NSYS; i++) if (SYSTEMS[i].beacon && root[i] == i) comp++;
        nh_check(nb == 0 || comp == 1, "threads: sector %d's beacons fall into %d separate nets", sec, comp);
    }
    g.sector = 0; regen_sector(); g.sys = entry_slot(0); s_target = (g.sys + 1) % NSYS;
    hub(4); nh_check(s_screen == ST_MAP, "threads: no map");
    uint8_t e[NSYS][2]; int ne = map_threads(e);
    g.beacon_lit = 0;
    nh_check(count_col(COL_THREAD) == 0, "threads: a gold thread with no beacon lit");
    if (ne > 0) {
        g.beacon_lit = bit(e[0][0]);
        nh_check(count_col(COL_THREAD) == 0, "threads: a gold thread with only one of its beacons lit");
        g.beacon_lit = bit(e[0][0]) | bit(e[0][1]);
        nh_check(count_col(COL_THREAD) > 8, "threads: two lit neighbours drew no gold thread");
        nh_dump("map_threads");
    }
    nh_close_app();
}

// The dogfight's new rules: a tracer leaves the ship that fired it and the hit points back at it; a lead hit on a
// charging foe spoils its shot (a hull hit does not); an ace runs its whole duel pattern, and the Keeper's guard
// turns every shot aside while it lasts.
static void scen_duel(uint32_t seed)
{
    wipe(); nh_open_app(seed); new_run();
    SYSTEMS[g.sys].faction = F_CUSTODI; g.rep[F_CUSTODI] = -40;
    combat_begin_ambush();
    nh_loop_ms(200);
    for (int i = 0; i < NFOE; i++) s_foe[i].on = 0;
    for (int i = 0; i < NBOLT; i++) s_bolt[i].on = 0;
    s_wave_left = 1; s_spawn_timer = 1e9f;
    g.hull = g.hull_max = 9000; s_shieldmax = s_shield = 0;
    // a tracer from a foe on the right: it starts on the foe and the hit is shown coming from the right
    spawn_foe(FOE_FIGHTER, 999); Foe *f = &s_foe[0];
    f->ex = 70; f->ey = 0; f->ez = 60; f->firecd = 30000; f->strafecd = 30000;
    float fx, fy, fs; project(f->ex, f->ey, f->ez, &fx, &fy, &fs);
    spawn_tracer(f, false, 0.0f);
    int b = -1; for (int i = 0; i < NBOLT; i++) if (s_bolt[i].on) b = i;
    nh_check(b >= 0, "duel: no tracer");
    if (b >= 0) {
        int64_t t0 = nh_us; while (nh_us - t0 < 150000) nh_tick(18);
        float bx, by, bs; project(s_bolt[b].ex, s_bolt[b].ey, s_bolt[b].ez, &bx, &by, &bs);
        nh_check(fabsf(bx - fx) < 40 && bx > CX + 20, "duel: a tracer 150 ms out is at x %d, its shooter at %d", (int)bx, (int)fx);
        t0 = nh_us; while (nh_us - t0 < 1000000 && s_bolt[b].on) nh_tick(18);
        nh_check(s_now < s_fx->dmg_until && cosf(s_fx->dmg_ang) > 0.5f, "duel: the hit from the right was not shown on the right (%.2f rad)", s_fx->dmg_ang);
    }
    // the telegraph: a lead hit spoils the shot; a hull hit does not
    s_shieldmax = s_shield = 9000;
    f->vx = 160; f->vy = 0; f->vz = 0; f->firecd = 200;
    float lx, ly; project(f->ex, f->ey, f->ez, &fx, &fy, &fs); lead_pip(f, &lx, &ly);
    nh_check(charging(f), "duel: a foe 200 ms from firing is not telegraphing");
    s_aimx = fx; s_aimy = fy; s_lock = 0; s_pfire_ms = 0; player_fire();
    nh_check(f->firecd == 200, "duel: a hull hit spoiled the shot (firecd %d)", f->firecd);
    s_aimx = lx; s_aimy = ly; s_lock = 0; s_pfire_ms = 0; int hp = f->hp; player_fire();
    nh_check(f->firecd > TELE_MS, "duel: a lead hit on a charging foe did not spoil its shot");
    nh_check(hp - f->hp == (12 + g.weapon * 5) * 3 / 2, "duel: a lead hit did %d, not 1.5x", hp - f->hp);
    f->on = 0;
    // the Keeper ace: weave -> charge -> burst -> evade (guarded) -> weave
    s_fx->ace_id = ACE_VIGIL;
    spawn_foe(FOE_ACE, 30000); Foe *a = &s_foe[0];
    bool seen[4] = { false, false, false, false }; int guarded = 0, leaked = 0, shots = 0;
    int64_t t0 = nh_us;
    while (nh_us - t0 < 16000000 && s_screen == ST_COMBAT && a->on) {
        int nb0 = 0; for (int i = 0; i < NBOLT; i++) nb0 += s_bolt[i].on;
        nh_tick(18);
        int nb1 = 0; for (int i = 0; i < NBOLT; i++) nb1 += s_bolt[i].on;
        if (a->ph == ACE_BURST && nb1 > nb0) shots++;
        seen[a->ph] = true;
        if (a->ph == ACE_EVADE) {
            s_lock = 0; s_pfire_ms = 0; int h = a->hp; player_fire();
            if (a->hp == h) guarded++; else leaked++;
        }
    }
    nh_check(seen[ACE_WEAVE] && seen[ACE_CHARGE] && seen[ACE_BURST] && seen[ACE_EVADE], "duel: the ace skipped a phase (%d%d%d%d)", seen[0], seen[1], seen[2], seen[3]);
    nh_check(shots >= 3, "duel: the ace's burst fired %d tracers", shots);
    nh_check(guarded > 0 && leaked == 0, "duel: the Keeper's guard let %d of %d shots through", leaked, guarded + leaked);
    nh_close_app();
}

// Texts that change with the language: every encounter, every contract brief of a few sectors, the intro and the
// cast. All five languages, nothing cut to fit; an ace bounty names its ace from the cast.
static void scen_texts(uint32_t seed)
{
    static const char *langs[] = { "it", "en", "es", "fr", "de" };
    int aces = 0;
    for (int l = 0; l < 5; l++) {
        g_nh.lang = langs[l]; wipe(); nh_open_app(seed); new_run();
        for (int ev = 0; ev < NEVENTS; ev++) {
            start_event(ev); s_overflow = 0; nh_draw_frame();
            nh_check(s_overflow == 0, "texts: event %d does not fit in %s", ev, g_nh.lang);
            for (int c = 0; c < EVENTS[ev].nch; c++) nh_check((int)strlen(lp(EVENTS[ev].ch[c].label)) * 6 <= 200, "texts: event %d choice %d is too long in %s", ev, c, g_nh.lang);
        }
        for (int i = 0; i < NINTRO; i++) nh_check((int)strlen(lp(INTRO_LINES[i])) * 6 <= W - 8, "texts: intro line %d is %d chars in %s", i, (int)strlen(lp(INTRO_LINES[i])), g_nh.lang);
        for (int i = 0; i < NACE + 1; i++) nh_check((int)strlen(lp(ACE_QUIP[i])) * 6 <= W - 14, "texts: radio line %d is too long in %s", i, g_nh.lang);
        for (int sec = 0; sec < 4; sec++) {
            g.sector = (uint32_t)sec; regen_sector();
            for (int sy = 0; sy < NSYS; sy++) {
                g.sys = sy; int el[NMISS_PER_SYS], n = eligible_missions(el);
                for (int k = 0; k < n; k++) {
                    s_pick = el[k]; s_screen = ST_BRIEF; s_overflow = 0; nh_draw_frame();
                    nh_check(s_overflow == 0, "texts: the brief of sector %d system %d slot %d does not fit in %s (line %d: %s / %s)", sec, sy, k, g_nh.lang,
                             s_overflow_line, cur_mission(el[k])->name, cur_mission(el[k])->brief);
                    const Mission *m = cur_mission(el[k]);
                    if (m->type == MT_BOUNTY && m->ace) {
                        bool cast = false; for (int a = 0; a < NACE; a++) cast |= !strcmp(s_mt->target, ACE_NAME[a]);
                        nh_check(cast, "texts: an ace bounty hunts '%s', not one of the cast", s_mt->target); aces++;
                    }
                }
            }
        }
        nh_close_app();
    }
    nh_check(aces > 0, "texts: no ace bounty in four sectors");
    g_nh.lang = "it";
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
    if (all || !strcmp(which, "gallery")) scen_gallery(seed);
    if (all || !strcmp(which, "threads")) scen_threads(seed);
    if (all || !strcmp(which, "duel"))    scen_duel(seed);
    if (all || !strcmp(which, "texts"))   scen_texts(seed);
    if (all || !strcmp(which, "balance")) scen_balance(seed);
    if (all || !strcmp(which, "fuzz"))    scen_fuzz(seed);
}
