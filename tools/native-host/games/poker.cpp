// Poker scenarios for the native-games host gate (tools/native-host/run.mjs poker).
// Hand ranking (every category + the edge cases: wheel, ace-high straight, royal vs straight flush, low
// pairs), the 9/6 Jacks-or-Better payouts incl. the max-bet royal, credit accounting when a hand is left
// half-way (Esc or closing the app), the hold advice on textbook hands, the return to player over many
// hands, no sound synthesis on the device + a complete pack, the console flow, every screen in three
// languages clear of the footer.
#define NH_GAME_NAME "poker"
#include "../../../firmware/components/nucleo_app/app_poker.cpp"
#include "../core.h"
void nh_register(void) { nucleo_register_poker(); }

static int below_footer(void) {
    nh_canvas.fillScreen(0xF81F); uint32_t sent = nh_canvas.readPixel(0, H - 1);
    nucleo_app_set_gfx(&nh_canvas); nh_app->on_draw(); nucleo_app_set_gfx(nullptr);
    int n = 0;
    for (int y = H - HINT; y < H; y++) for (int x = 0; x < W; x++) if (nh_canvas.readPixel(x, y) != sent) n++;
    return n;
}
static void screen(const char *name) {
    nh_loop_ms(400);
    nh_check_hint(name);
    if (!nh_full) nh_check(below_footer() == 0, "%s/%s: %d px drawn under the footer", name, g_nh.lang, below_footer());
    char b[64]; snprintf(b, sizeof b, "%s_%s", name, g_nh.lang); nh_dump(b);
}
// "AS KH 10D 3C 2S" -> card ids (suit*13 + rank, rank 0 = '2' .. 12 = 'A'; suits S H D C)
static void parse(const char *s, int *c) {
    for (int i = 0; i < 5; i++) {
        while (*s == ' ') s++;
        int r = *s == 'A' ? 12 : *s == 'K' ? 11 : *s == 'Q' ? 10 : *s == 'J' ? 9 : *s == '1' ? 8 : *s - '2';
        s += *s == '1' ? 2 : 1;
        int su = *s == 'S' ? 0 : *s == 'H' ? 1 : *s == 'D' ? 2 : 3; s++;
        c[i] = su * 13 + r;
    }
}
static void scen_rank(uint32_t) {
    struct { const char *h; int cat; } T[] = {
        { "AS KS QS JS 10S", HAND_ROYAL }, { "9H KH QH JH 10H", HAND_SF }, { "AD 2D 3D 4D 5D", HAND_SF },
        { "7C 7D 7H 7S 2C", HAND_QUADS }, { "3C 3D 3H 9S 9C", HAND_FULL }, { "2H 9H JH 4H KH", HAND_FLUSH },
        { "AS 2D 3H 4C 5S", HAND_STRAIGHT }, { "10S JD QH KC AS", HAND_STRAIGHT }, { "6S 7D 8H 9C 10S", HAND_STRAIGHT },
        { "QS KD AH 2C 3S", HAND_NONE }, { "5C 5D 5H KS 2C", HAND_TRIPS }, { "4C 4D 9H 9S AC", HAND_TWOPAIR },
        { "JC JD 2H 5S 8C", HAND_JACKS }, { "AC AD 2H 5S 8C", HAND_JACKS }, { "10C 10D 2H 5S 8C", HAND_NONE },
        { "2C 4D 6H 8S QC", HAND_NONE },
    };
    for (auto &t : T) { int c[5]; parse(t.h, c); int got = eval_hand(c); nh_check(got == t.cat, "rank: %s -> %d, want %d", t.h, got, t.cat); }
}

// Play one hand by hand: deal, force the cards, hold `hold`, draw, settle.
static void deal_with(const char *cards) {
    nh_tap(' '); finish_flips(); nh_loop_ms(80);
    nh_check(s_pp == PP_HOLD, "deal: not holding after the deal (pp %d)", s_pp);
    int c[5]; parse(cards, c);
    for (int i = 0; i < 5; i++) { s_card[i] = c[i]; s_id_now[i] = s_id_to[i] = c[i]; }
}
static void to_table(void) { s_menu.sel = 0; nh_tap('\n'); nh_check(s_screen == ST_PLAY, "menu row 0 did not open the table (screen %d)", s_screen); }

static void scen_pay(uint32_t seed) {
    nh_open_app(seed); to_table();
    struct { const char *h; int bet, win; } T[] = {
        { "JC JD 2H 5S 8C", 1, 1 }, { "4C 4D 9H 9S AC", 2, 4 }, { "5C 5D 5H KS 2C", 3, 9 }, { "AS 2D 3H 4C 5S", 1, 4 },
        { "2H 9H JH 4H KH", 5, 30 }, { "3C 3D 3H 9S 9C", 1, 9 }, { "7C 7D 7H 7S 2C", 2, 50 }, { "9H KH QH JH 10H", 1, 50 },
        { "AS KS QS JS 10S", 1, 250 }, { "AS KS QS JS 10S", 5, 4000 }, { "2C 4D 6H 8S QC", 4, 0 },
    };
    for (auto &t : T) {
        g_balance = 1000; g_bet = t.bet; s_pp = PP_READY;
        deal_with(t.h);
        for (int i = 0; i < 5; i++) s_hold[i] = true;
        int before = g_balance;
        nh_tap(' '); finish_flips(); nh_loop_ms(120);
        for (int k = 0; k < 4 && s_pp != PP_RESULT; k++) { nh_tap(' '); finish_flips(); nh_loop_ms(120); }
        nh_check(s_pp == PP_RESULT && g_balance - before == t.win, "pay: %s at %d coins paid %d, want %d", t.h, t.bet, g_balance - before, t.win);
    }
    nh_close_app();
}

// Leaving a hand half-way must settle it (the cards as held), never swallow the bet.
static void scen_leave(uint32_t seed) {
    nh_open_app(seed); to_table();
    g_balance = 100; g_bet = 1; g_draws = 2;
    deal_with("AC AD 2H 5S 8C");
    for (int i = 0; i < 5; i++) s_hold[i] = true;
    nh_tap('`'); nh_tap('`');
    nh_check(s_screen == ST_MENU, "leave: Esc, Esc did not reach the menu (screen %d)", s_screen);
    nh_check(g_balance == 100, "leave: a pair of aces left mid-hand paid back %d of 100 (the bet was swallowed)", g_balance);
    to_table();
    nh_check(s_pp == PP_READY, "leave: the table did not start a fresh hand (pp %d)", s_pp);
    deal_with("KC KD 2H 5S 8C");
    for (int i = 0; i < 5; i++) s_hold[i] = true;
    nh_close_app();                                        // closing the app mid-hand: the same
    nh_check(g_balance == 100, "close: kings left mid-hand by closing the app -> %d, want 100", g_balance);
}

// The hold advice on textbook Jacks-or-Better hands.
static void scen_advice(uint32_t seed) {
    nh_open_app(seed);
    struct { const char *h; int mask; } T[] = {
        { "JH JS 3H 7H 9H", 0x03 },     // high pair beats 4 to a flush
        { "2H 5H 3H 7H 9S", 0x0F },     // 4 to a flush beats nothing
        { "4C 4D 3H 7H 9H", 0x03 },     // low pair
        { "AH KH QH JH 3C", 0x0F },     // 4 to a royal
        { "AH KH QH JH 10H", 0x1F },    // made royal
        { "9C 10D JH QS 2C", 0x0F },    // open-ended straight
        { "5S 5D 5H 9C 2D", 0x07 },     // trips
        { "KS QD 2H 6C 8D", 0x03 },     // two high cards
        { "2S 4D 7H 9C 3D", 0x00 },     // nothing: draw five
    };
    for (auto &t : T) {
        int c[5]; parse(t.h, c); for (int i = 0; i < 5; i++) s_card[i] = c[i];
        int m = suggest_holds();
        nh_check(m == t.mask, "advice: %s -> hold %02x, want %02x", t.h, m, t.mask);
    }
    nh_close_app();
}

// Return to player with the advice over many hands (single draw, 1 coin): a fair machine pays back 95-101%.
static void scen_rtp(uint32_t seed) {
    nh_open_app(seed); to_table();
    nh_draw_every = 1000000;
    g_draws = 2; g_bet = 1;
    long in = 0, out = 0; int n = 30000;
    for (int k = 0; k < n; k++) {
        g_balance = 1000000; s_pp = PP_READY;
        do_deal(); finish_flips(); s_pp = PP_HOLD;
        int m = suggest_holds(); for (int i = 0; i < 5; i++) s_hold[i] = (m >> i) & 1;
        do_draw(); finish_flips();
        int b = g_balance; evaluate(); out += g_balance - b; in += 1;
    }
    nh_draw_every = 5;
    nh_note("rtp: %.1f%% over %d hands with the hold advice", 100.0 * out / in, n);
    nh_check(out * 100 >= in * 93 && out * 100 <= in * 102, "rtp: %.1f%% is outside 93-102%%", 100.0 * out / in);
    nh_close_app();
}

// Sound: nothing synthesized on the device; the pack covers every cue the code names.
static void scen_sfx(uint32_t seed) {
    nh_open_app(seed); nh_loop_ms(2000);
    int made = 0, miss = 0; struct stat st;
    for (int id = 1; id <= NSFX; id++) {
        char p[96]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", sfx_name(id)); if (!host_stat(p, &st)) made++;
        snprintf(p, sizeof p, DIRR "/pack/%s.wav", sfx_name(id)); if (host_stat(p, &st) || st.st_size <= 44) { miss++; nh_note("sfx: no pack WAV for '%s'", sfx_name(id)); }
    }
    nh_check(made == 0, "sfx: %d cues synthesized on the app task at launch (8 s Task-WDT)", made);
    nh_check(miss == 0, "sfx: %d cues missing from the pack (tools/sfx-gen/games/poker.py)", miss);
    nh_close_app();
}

// Console flow: Esc on the table pauses (ENTER resumes); TAB opens the settings and Esc returns to the table.
static void scen_flow(uint32_t seed) {
    nh_open_app(seed); to_table();
    nh_tap('`');
    nh_check(s_screen == ST_PLAY, "flow: Esc on the table left it without asking (screen %d)", s_screen);
    nh_tap('\n');
    nh_tap('\t');
    nh_check(s_screen == ST_SET, "flow: TAB on the table did not open the settings (screen %d)", s_screen);
    nh_tap('`');
    nh_check(s_screen == ST_PLAY, "flow: Esc in the table's settings did not return to the table (screen %d)", s_screen);
    nh_close_app();
}

static void scen_screens(uint32_t seed) {
    static const char *langs[] = { "it", "en", "de" };
    for (int l = 0; l < 3; l++) {
        g_nh.lang = langs[l];
        nh_open_app(seed);
        g_balance = 200; g_bet = 1; g_draws = 2; g_hint = 1;
        screen("menu");
        s_menu.sel = 1; nh_tap('\n'); screen("help1"); nh_tap('/'); screen("help2"); nh_tap('/'); screen("help3"); nh_tap('`');
        s_menu.sel = 2; nh_tap('\n'); screen("settings"); nh_tap('.'); nh_tap('.'); nh_tap('.'); nh_tap('.'); nh_tap('.'); screen("settings_end"); nh_tap('`');
        s_menu.sel = 0; nh_tap('\n'); screen("ready");
        deal_with("JH JS 3H 7H 9H"); screen("hold");
        nh_tap('3'); nh_tap('4'); screen("held");
        nh_tap(' '); finish_flips(); nh_loop_ms(150); screen("result");
        g_bet = 5; s_pp = PP_READY; deal_with("AS KS QS JS 10S"); for (int i = 0; i < 5; i++) s_hold[i] = true;
        nh_tap(' '); finish_flips(); nh_loop_ms(300); screen("jackpot");
        nh_loop_ms(2500); screen("jackpot_after");
        nh_tap('`'); screen("pause"); nh_tap('\n');
        g_balance = 0; s_pp = PP_RESULT; nh_tap(' '); nh_loop_ms(900); screen("over");
        nh_close_app();
    }
    g_nh.lang = "it";
}

static void scen_fuzz(uint32_t seed) {
    for (int round = 0; round < 2; round++) {
        g_nh.adv = g_nh.imu = round == 1;
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
    if (all || !strcmp(which, "rank")) scen_rank(seed);
    if (all || !strcmp(which, "pay")) scen_pay(seed);
    if (all || !strcmp(which, "leave")) scen_leave(seed);
    if (all || !strcmp(which, "advice")) scen_advice(seed);
    if (all || !strcmp(which, "rtp")) scen_rtp(seed);
    if (all || !strcmp(which, "sfx")) scen_sfx(seed);
    if (all || !strcmp(which, "flow")) scen_flow(seed);
    if (all || !strcmp(which, "screens")) scen_screens(seed);
    if (all || !strcmp(which, "fuzz")) scen_fuzz(seed);
}
