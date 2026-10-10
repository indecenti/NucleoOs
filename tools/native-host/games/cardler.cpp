// Cardler scenarios for the native-games host gate (tools/native-host/run.mjs cardler).
// The run-length world decodes to the original map, the atlas never needs one big heap block, every screen
// in two languages (clear of the footer, hints that fit), the chest hunt to victory, fainting and waking at
// the village, villagers that never trap you, a save that survives a bad read, and random play on both boards.
#define NH_GAME_NAME "cardler"
#include "../../../firmware/components/nucleo_app/app_cardler.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_cardler(); }

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
    nh_loop_ms(400);
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s: %d px drawn under the footer", name, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
static uint32_t fnv_layer(const char *rle, const uint16_t *rows)
{
    uint32_t h = 2166136261u; char row[MAP_W];
    for (int ty = 0; ty < MAP_H; ty++) { rle_row(rle, rows, ty, row); for (int x = 0; x < MAP_W; x++) { h ^= (uint8_t)row[x]; h *= 16777619u; } }
    return h;
}
static void wipe_save(void) { host_remove(SAVE_PATH); host_remove(SAVE_PATH ".tmp"); }
// stand the hero just below chest #k's tile, facing up, so ENTER opens it
static bool face_chest(int k)
{
    char row[MAP_W]; int seen = 0;
    for (int ty = 0; ty < MAP_H; ty++) {
        rle_row(OVR, OVR_ROW, ty, row);
        for (int tx = 0; tx < MAP_W; tx++) if (is_chest(row[tx]) && seen++ == k) {
            s_px = tx * 16 + 8; s_py = ty * 16 + 8 + 14; s_fdx = 0; s_fdy = -1; return true;
        }
    }
    return false;
}

static void scen_world(uint32_t seed)
{
    // FNV-1a of the original 144x96 char grids (before the run-length conversion)
    nh_check(fnv_layer(BASE, BASE_ROW) == 0xdb0eeebeu, "world: BASE decodes to %08x, not the original map", fnv_layer(BASE, BASE_ROW));
    nh_check(fnv_layer(OVR, OVR_ROW) == 0x5b445fe4u, "world: OVR decodes to %08x, not the original map", fnv_layer(OVR, OVR_ROW));
    for (int ty = 0; ty < MAP_H; ty += 7) {                       // rle_at agrees with rle_row everywhere
        char row[MAP_W]; rle_row(OVR, OVR_ROW, ty, row);
        for (int tx = 0; tx < MAP_W; tx++) if (rle_at(OVR, OVR_ROW, tx, ty) != row[tx]) { nh_fail("world: rle_at(%d,%d) disagrees", tx, ty); return; }
    }
    nh_open_app(seed);
    nh_check(s_atlas_ok, "atlas did not load");
    nh_check(s_nchest == 20, "world: %d chests counted (the map holds 20)", s_nchest);
    nh_check(nh_heap.largest <= 4096, "RAM: the atlas took a %ld B block (chunks are 4 KB)", nh_heap.largest);
    nh_close_app();
}

static void scen_screens(uint32_t seed)
{
    static const char *langs[] = { "it", "de" };
    for (int l = 0; l < 2; l++) {
        g_nh.lang = langs[l]; g_nh.imu = true; wipe_save();
        nh_open_app(seed);
        nh_check(s_gs == GS_MENU && !nh_full, "opens on the title with the hint bar");
        screen("menu");
        nh_tap('.'); nh_tap('\n');
        nh_check(s_gs == GS_SETTINGS, "menu row 2 did not open the settings (IMU present)");
        screen("settings");
        nh_tap('`');
        s_menu.sel = 0; nh_tap('\n');
        nh_check(s_gs == GS_PLAY && nh_full, "New game did not start fullscreen play");
        nh_loop_ms(600); screen("play");
        // talk to the first villager: stand next to it, facing it
        for (int i = 0; i < NENT; i++) if (R->ent[i].kind == 0) { s_px = R->ent[i].x - 14; s_py = R->ent[i].y; s_fdx = 1; s_fdy = 0; R->ent[i].pause = 999; break; }
        s_hp = 4; nh_tap('\n');
        nh_check(s_dlg >= 0, "ENTER facing a villager did not open a dialogue");
        nh_check(s_hp == HPMAX, "talking to a villager did not restore HP (%d)", s_hp);
        screen("dialog");
        nh_tap('\n'); nh_check(s_dlg < 0, "ENTER did not close the dialogue");
        nh_tap('`'); nh_check(s_gs == GS_PAUSE, "Esc in play did not pause");
        screen("pause");
        nh_tap('\n'); nh_check(s_gs == GS_PLAY, "ENTER did not resume");
        s_hp = 1; s_hurt = 0;
        for (int i = 0; i < NENT; i++) if (R->ent[i].kind == 1) { s_px = R->ent[i].x; s_py = R->ent[i].y; break; }
        nh_loop_ms(100);
        nh_check(s_gs == GS_OVER, "HP 0 did not end in the fainted card (hp %d)", s_hp);
        screen("over");
        int gold = s_gold = 41; nh_tap('\n');
        nh_check(s_gs == GS_PLAY && s_hp == HPMAX && s_gold == gold / 2 && s_px == s_start_x && s_py == s_start_y,
                 "waking up: gs %d hp %d gold %d (want %d) at %d,%d", s_gs, s_hp, s_gold, gold / 2, s_px, s_py);
        for (int k = 0; k < s_nchest; k++) { nh_check(face_chest(k), "chest %d not found", k); nh_tap('\n'); }
        nh_check(R->nopen == s_nchest && s_won && s_gs == GS_WIN, "opening all chests: %d/%d won %d gs %d", R->nopen, s_nchest, s_won, s_gs);
        screen("win");
        nh_tap('\n'); nh_loop_ms(300);
        for (int i = 0; i < NPOP; i++) R->pop[i].t = 0;
        face_chest(3); s_py += 30; nh_loop_ms(100); screen("hud");
        nh_tap('`'); nh_tap('`');
        nh_check(s_gs == GS_MENU, "Esc from pause did not return to the title");
        nh_check(s_has_save, "leaving play wrote no save");
        screen("menu_save");
        nh_close_app();
    }
    g_nh.lang = "it"; g_nh.imu = false;
}

// A villager pacing into the hero must stop, and the hero must always be able to walk away (it used to
// overlap the player and block every step until it walked off again).
static void scen_npc(uint32_t seed)
{
    wipe_save(); nh_open_app(seed); s_menu.sel = 0; nh_tap('\n');
    int stuck = 0, tested = 0;
    for (int i = 0; i < NENT; i++) {
        Ent *e = &R->ent[i]; if (e->kind) continue;
        int hx = e->x + e->vx * 12, hy = e->y + e->vy * 12;          // right in its path
        if (blocked(hx, hy)) continue;
        s_px = hx; s_py = hy; e->pause = 0; tested++;
        nh_loop_ms(1500);
        nh_check(!near_player(e->x, e->y), "npc %d walked into the hero", i);
        bool moved = false;
        static const char dirs[4] = { ';', '.', ',', '/' };
        for (int k = 0; k < 4 && !moved; k++) { int x0 = s_px, y0 = s_py; nh_hold(dirs[k], 250); moved = s_px != x0 || s_py != y0; }
        if (!moved) stuck++;
    }
    nh_check(tested >= 5, "npc: only %d villagers tested", tested);
    nh_check(stuck == 0, "npc: the hero could not move at all next to %d villagers", stuck);
    nh_close_app();
}

// Every chest must be reachable on foot from the village (else the hunt could never be won): flood the
// walkable tiles from the spawn, each chest needs a reached neighbour.
static void scen_reach(uint32_t seed)
{
    wipe_save(); nh_open_app(seed);
    static uint8_t seen[MAP_H][MAP_W]; memset(seen, 0, sizeof seen);
    static int16_t q[MAP_W * MAP_H][2]; int qh = 0, qt = 0;
    q[qt][0] = s_start_x >> 4; q[qt][1] = s_start_y >> 4; qt++; seen[s_start_y >> 4][s_start_x >> 4] = 1;
    while (qh < qt) {
        int x = q[qh][0], y = q[qh][1]; qh++;
        static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) { int nx = x + dx[k], ny = y + dy[k];
            if (in_map(nx, ny) && !seen[ny][nx] && !solid_tile(nx, ny)) { seen[ny][nx] = 1; q[qt][0] = nx; q[qt][1] = ny; qt++; } }
    }
    int unreachable = 0; char row[MAP_W];
    for (int ty = 0; ty < MAP_H; ty++) { rle_row(OVR, OVR_ROW, ty, row);
        for (int tx = 0; tx < MAP_W; tx++) if (is_chest(row[tx])) {
            bool ok = (ty + 1 < MAP_H && seen[ty + 1][tx]) || (ty > 0 && seen[ty - 1][tx]) || (tx + 1 < MAP_W && seen[ty][tx + 1]) || (tx > 0 && seen[ty][tx - 1]);
            if (!ok) { unreachable++; nh_note("chest at %d,%d is walled in", tx, ty); } } }
    nh_check(unreachable == 0, "reach: %d chests cannot be reached from the village", unreachable);
    nh_close_app();
}

// The save round-trips; a corrupt save is reported, never overwritten by entering the game, and Continue
// on it stays on the title instead of starting over.
static void scen_save(uint32_t seed)
{
    wipe_save(); nh_open_app(seed); s_menu.sel = 0; nh_tap('\n');
    face_chest(0); nh_tap('\n'); s_hp = 7; int gold = s_gold, px = s_px, py = s_py;
    nh_tap('`'); nh_tap('`');
    nh_close_app();
    nh_open_app(seed);
    nh_check(s_has_save, "save: not found on the next launch");
    s_menu.sel = 0; nh_tap('\n');
    nh_check(s_gs == GS_PLAY && s_gold == gold && s_hp == 7 && R->nopen == 1 && s_px == px && s_py == py,
             "save: continue restored gold %d/%d hp %d chests %d pos %d,%d", s_gold, gold, s_hp, R->nopen, s_px, s_py);
    nh_close_app();
    // flip one byte: the checksum must reject it, and nothing may write over it
    FILE *f = host_fopen(SAVE_PATH, "r+b"); fseek(f, 10, SEEK_SET); int c = fgetc(f); fseek(f, 10, SEEK_SET); fputc(c ^ 0x5A, f); fclose(f);
    struct stat st0; host_stat(SAVE_PATH, &st0);
    nh_open_app(seed);
    nh_check(!s_has_save, "save: a corrupt save was accepted");
    nh_check(s_menu.sel == 0, "menu");
    nh_tap('`');                                                   // leave from the title
    nh_close_app();
    f = host_fopen(SAVE_PATH, "rb"); fseek(f, 10, SEEK_SET); int c2 = fgetc(f); fclose(f);
    nh_check(c2 == (c ^ 0x5A), "save: the corrupt save was overwritten without the player choosing New game");
}

static void scen_fuzz(uint32_t seed)
{
    for (int round = 0; round < 3; round++) {
        g_nh.adv = g_nh.imu = (round == 2);
        wipe_save(); nh_open_app(seed + round * 101);
        nh_loop_ms(500);
        s_menu.sel = 0; nh_tap('\n');
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
    if (all || !strcmp(which, "world"))   scen_world(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "npc"))     scen_npc(seed);
    if (all || !strcmp(which, "reach"))   scen_reach(seed);
    if (all || !strcmp(which, "save"))    scen_save(seed);
    if (all || !strcmp(which, "fuzz"))    scen_fuzz(seed);
}
