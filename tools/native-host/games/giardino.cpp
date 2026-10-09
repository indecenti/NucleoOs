// Giardino (app_sandgarden.cpp) on the simulated Cardputer: the merge rules, scoring, spawn, undo, game over,
// save + resume, one move per arrow press, long random games against invariants, and every screen in it/de.
#define NH_GAME_NAME "giardino"
#include "../../../firmware/components/nucleo_app/app_sandgarden.cpp"
#include "../core.h"

void nh_register(void) { nucleo_register_sandgarden(); }

static void set_board(const char *rows)          // 16 hex tiers, row-major ("0 = empty")
{
    for (int i = 0; i < NB * NB; i++) { char c = rows[i]; G->b[i] = (uint8_t)(c >= 'a' ? c - 'a' + 10 : c - '0'); }
    G->score = 0; G->over = false; G->can_undo = false; G->anim = -1; G->nmv = 0; s_dialog = 0;
    memset(G->pop, 0, sizeof G->pop);
}
static void row(int y, char *out) { for (int x = 0; x < NB; x++) { int t = G->b[y * NB + x]; out[x] = (char)(t >= 10 ? 'a' + t - 10 : '0' + t); } out[NB] = 0; }
static int tiles(void) { int n = 0; for (int i = 0; i < NB * NB; i++) n += G->b[i] != 0; return n; }
static bool idle(void) { return G && G->anim < 0; }
static bool in_play(void) { return s_scr == SC_PLAY; }

static void go_play_new(void)
{
    s_menu.sel = 0; s_menu.pos = 0;
    int n = menu_build();
    for (int i = 0; i < n; i++) if (menu_ids[i] == MI_NEW) s_menu.sel = (int8_t)i;
    nh_tap('\n');
    nh_run_until(idle, 1000);
}

static void sc_rules(void)
{
    printf("[rules]\n");
    nh_open_app(7);
    go_play_new();
    nh_check(in_play(), "ENTER on 'New game' should start a game (screen %d)", s_scr);
    nh_check(tiles() == 2, "a new game starts with 2 plants, has %d", tiles());
    struct Case { const char *board; int dx, dy; const char *row0; uint32_t score; };
    static const Case C[] = {
        { "1111" "0000" "0000" "0000", -1, 0, "22", 8 },          // two pairs -> two merges, never a chain
        { "1120" "0000" "0000" "0000", -1, 0, "22", 4 },          // 1+1 = 2; the existing 2 does not merge again
        { "2110" "0000" "0000" "0000", -1, 0, "22", 4 },
        { "0011" "0000" "0000" "0000", 1, 0, "0002", 4 },         // sliding right
        { "1010" "0000" "0000" "0000", -1, 0, "2", 4 },           // a gap between equal plants
        { "3322" "0000" "0000" "0000", -1, 0, "43", 24 },
    };
    for (const Case &c : C) {
        set_board(c.board);
        bool moved = slide(c.dx, c.dy);
        char r[NB + 1]; row(0, r);
        bool ok = moved && !strncmp(r, c.row0, strlen(c.row0)) && G->score == c.score;
        nh_check(ok, "slide %s (%+d,%+d): row0 '%s' score %u, want '%s...' score %u", c.board, c.dx, c.dy, r, (unsigned)G->score, c.row0, (unsigned)c.score);
        nh_check(tiles() >= 1 && G->can_undo, "after a move a seed falls and undo is armed");
    }
    // vertical
    set_board("1000" "1000" "2000" "2000");
    slide(0, -1);
    nh_check(G->b[0] == 2 && G->b[4] == 3, "slide up merges columns (got %d,%d)", G->b[0], G->b[4]);
    // no move = no seed, no undo
    set_board("1200" "0000" "0000" "0000");
    nh_check(!slide(-1, 0) && tiles() == 2 && !G->can_undo, "a move that changes nothing drops no seed");
    // undo restores board and score
    set_board("1100" "0000" "0000" "0000");
    slide(-1, 0); uint32_t sc = G->score;
    nh_tap('z');
    nh_check(G->b[0] == 1 && G->b[1] == 1 && tiles() == 2 && G->score == 0 && sc == 4, "Z undoes the last move (board + score)");
    nh_tap('z');
    nh_check(G->b[0] == 1 && G->b[1] == 1, "only one undo");
    // game over
    set_board("1212" "2121" "1212" "2120");
    slide(1, 0);                                  // fills the last hole; nothing can move after
    if (tiles() == 16 && !can_move()) nh_check(G->over && s_dialog == 2, "a full, stuck garden ends the game (dialog %d)", s_dialog);
    nh_dump("over");
    nh_tap('z');
    nh_check(!G->over && s_dialog == 0, "Z on the game-over card takes the last move back");
    // golden tree
    set_board("aa00" "0000" "0000" "0000");
    G->won_seen = false; slide(-1, 0);
    nh_check(G->b[0] == WIN_T && s_dialog == 3, "two cherry trees grow the golden tree (+dialog)");
    nh_loop_ms(400); nh_dump("golden");
    nh_tap('\n');
    nh_check(s_dialog == 0 && in_play(), "ENTER continues after the golden tree");
    nh_close_app();
}

static void sc_input(void)
{
    printf("[input]\n");
    nh_open_app(11);
    go_play_new();
    set_board("1000" "0000" "0000" "0000");
    uint16_t m0 = G->moves;
    nh_hold('/', 1200);                            // held: the driver repeats it ~10 times
    nh_run_until(idle, 1000);
    nh_check(G->moves - m0 == 1, "holding RIGHT 1.2 s made %d moves (want 1: one move per press)", G->moves - m0);
    m0 = G->moves;
    for (int i = 0; i < 6; i++) { nh_tap(i & 1 ? ';' : '.', 40, 30); }     // fast taps during the animations
    nh_run_until(idle, 2000);
    nh_check(G->moves - m0 >= 5, "6 quick taps should give 6 moves (buffered), got %d", G->moves - m0);
    m0 = G->moves;
    nh_tap('a'); nh_tap('d'); nh_tap('w'); nh_tap('s'); nh_run_until(idle, 1000);
    nh_check(G->moves - m0 >= 3, "WASD move too (%d)", G->moves - m0);
    nh_close_app();
}

static void sc_save(void)
{
    printf("[save]\n");
    nh_open_app(21);
    go_play_new();
    for (int i = 0; i < 12; i++) { nh_tap(";/.,"[i % 4]); nh_run_until(idle, 1000); }
    uint8_t snap[NB * NB]; memcpy(snap, G->b, sizeof snap); uint32_t sc = G->score;
    nh_tap('`');                                   // pause (saves)
    nh_check(s_dialog == 1, "Esc in play pauses");
    nh_tap('`');                                   // to the menu
    nh_check(s_scr == SC_MENU, "Esc on the pause card goes to the menu");
    nh_close_app();
    nh_open_app(22);
    nh_check(s_saved, "the menu offers Continue after reopening");
    int n = menu_build();
    nh_check(n == 5 && menu_ids[0] == MI_CONT, "Continue is the first menu row");
    s_menu.sel = 0; nh_tap('\n'); nh_run_until(idle, 1000);
    nh_check(in_play() && !memcmp(G->b, snap, sizeof snap) && G->score == sc, "Continue resumes the same garden and score");
    nh_close_app();
    // a damaged save never crashes and never resumes garbage
    FILE *f = fopen("sd/data/giardino/save.bin", "wb"); if (f) { fwrite("GDR1xxxxxxxx", 1, 12, f); fclose(f); }
    nh_open_app(23);
    nh_check(!s_saved, "a damaged save offers no Continue");
    nh_close_app();
}

static void sc_long(uint32_t seed)
{
    printf("[long]\n");
    nh_open_app(seed);
    go_play_new();
    int games = 0, best_t = 0;
    int64_t end = nh_us + (int64_t)900 * 1000000;   // 15 minutes of play
    while (nh_us < end) {
        if (s_dialog == 2) { games++; nh_tap('\n'); continue; }
        if (s_dialog) { nh_tap('\n'); continue; }
        static const char k[] = ";/.,;/.,z";
        nh_tap(k[nh_jitter(9)], 30 + nh_jitter(60), 20 + nh_jitter(80));
        for (int i = 0; i < NB * NB; i++) if (G->b[i] > MAXT) { nh_fail("tier %d out of range", G->b[i]); break; }
        if (max_tier() > best_t) best_t = max_tier();
    }
    nh_check(best_t >= 6, "random play should reach at least a sunflower (best tier %d)", best_t);
    nh_note("15 min random play: %d games over, best tier %d, best score %lu", games, best_t, (unsigned long)s_best);
    nh_close_app();
}

static void sc_frames(void)
{
    printf("[frames]\n");
    const char *langs[2] = { "it", "de" };
    for (int l = 0; l < 2; l++) {
        g_nh.lang = langs[l];
        char nm[32];
        nh_open_app(31 + l);
        nh_loop_ms(300); snprintf(nm, sizeof nm, "menu_%s", langs[l]); nh_dump(nm); nh_check_hint("menu");
        go_play_new();
        set_board("1234" "5678" "9abc" "0000");
        nh_loop_ms(100); snprintf(nm, sizeof nm, "family_%s", langs[l]); nh_dump(nm);
        set_board("1100" "0220" "0003" "3000");
        nh_tap(','); nh_loop_ms(50); snprintf(nm, sizeof nm, "slide_%s", langs[l]); nh_dump(nm);
        nh_loop_ms(140); snprintf(nm, sizeof nm, "pop_%s", langs[l]); nh_dump(nm);
        nh_run_until(idle, 800); snprintf(nm, sizeof nm, "play_%s", langs[l]); nh_dump(nm);
        nh_tap('`'); snprintf(nm, sizeof nm, "pause_%s", langs[l]); nh_dump(nm);
        nh_tap('`');
        for (int i = 0; i < 5; i++) {               // every menu screen
            int n = menu_build(); s_menu.sel = (int8_t)(i % n); s_menu.pos = s_menu.sel;
            if (menu_ids[s_menu.sel] == MI_CONT || menu_ids[s_menu.sel] == MI_NEW) continue;
            nh_tap('\n');
            snprintf(nm, sizeof nm, "screen%d_%s", menu_ids[s_menu.sel], langs[l]); nh_dump(nm); nh_check_hint(nm);
            if (s_scr == SC_HELP) for (int p = 1; p < 3; p++) { nh_tap('/'); snprintf(nm, sizeof nm, "help%d_%s", p, langs[l]); nh_dump(nm); }
            nh_tap('`');
        }
        nh_close_app();
    }
    g_nh.lang = "it";
}

void nh_scenarios(const char *which, uint32_t seed)
{
    bool all = !strcmp(which, "all");
    if (all || !strcmp(which, "rules")) sc_rules();
    if (all || !strcmp(which, "input")) sc_input();
    if (all || !strcmp(which, "save"))  sc_save();
    if (all || !strcmp(which, "long"))  sc_long(seed);
    if (all || !strcmp(which, "frames")) sc_frames();
}
