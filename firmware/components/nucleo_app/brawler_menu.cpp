// brawler_menu.cpp — SCORRIBANDA: all non-play screens + the in-action HUD.
//
// The front-end is the shared console kit (game_ui.h): the title screen, the menus and the pause / game
// over / street-cleared cards look like every other game on the Cardputer, with blood red as the accent and
// two black noir silhouettes squaring up behind the title menu. The character select is the game's own:
// three cards with the posed hero in its colour at its real build, and the pick's relative stats. hud_draw() floats compact health/score/lives/
// combo over the 240x135 fight. Text follows the OS language (GT). All state lives in `g` (app_brawler.cpp)
// plus this module's menu cursor; HEAP-FREE, ASCII only.
//
// Layout law: the non-play screens keep the OS footer (hints, set by the shell), so everything they draw
// fits the 121 px above it (CH).

#include "brawler.h"
#include "app_gfx.h"
#include "game_text.h"
#include "game_ui.h"
#include "nucleo_kbd.h"
#include "nucleo_app.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CH       (BR_SH - 14)            // content height above the OS footer (non-play screens)
#define ACC      gui::rgb(230, 40, 40)   // the game's accent: blood red, RGB332-exact
#define MN_RED   BR_BLOOD
#define MN_REDHI br_rgb(232, 56, 50)     // hotter red (combo pop) — same warm family
#define WHITE    gui::rgb(255, 255, 255)
#define BLACK    gui::rgb(0, 0, 0)
#define MUTED    gui::rgb(150, 160, 190)

static gui::Menu s_menu;                 // the list cursor of the title / options / co-op menus
static float s_trail[2];                 // HUD: health shown by each hero's damage trail (0..1)

// A posed silhouette of hero `kind` in state `st`, drawn at screen (sx, feetY) at base scale `sc` facing
// dir, in `body` with a `rim` outline. The hero's OWN build is honoured (scale = height, girth = width) so
// the roster's differences read on the line-up. Pure flavour: no animation state owned here.
static void pose_hero(int kind, BrState st, float anim, float sx, float feetY, float sc, int dir, int shade, uint16_t rim) {
    Fighter fr;
    memset(&fr, 0, sizeof fr);
    fr.on = true; fr.is_hero = true; fr.kind = kind;
    fr.dir = dir; fr.st = st; fr.anim = anim; fr.aspd = 1.0f;
    const HeroDef *h = brawler_hero(kind);
    fr.maxhp = fr.hp = h->maxhp;
    d.fillEllipse((int)sx, (int)feetY, (int)(0.32f * sc * h->scale), 2, BLACK);   // contact shadow
    br_figure(&fr, sx, feetY, sc * h->scale, shade, BLACK, rim);
}

// The picked hero performs a SIGNATURE move on a loop so the pick feels alive: idle -> its strike -> idle.
// OMBRA jabs, MOLE throws his heavy punch, VIPERA snaps a long kick — matching their style blurbs.
static void hero_signature(int kind, float loop, BrState *st, float *anim) {
    float a = (loop - 0.45f) / 0.45f;          // 0..1 strike window after an idle breath
    if (a < 0.0f || a > 1.0f) { *st = BS_IDLE; *anim = loop; return; }
    *st = (kind == 2) ? BS_KICK : BS_PUNCH; *anim = a;
}

// The kit's backdrop for a screen with no title header (select / help body).
static void backdrop(void) { gui::vgradient(0, 0, BR_SW, CH, gui::mix(BLACK, ACC, 60), BLACK); }

// ============================ SC_MENU (title) ================================
#define NM_ITEMS 3
static void draw_title(void) {
    int y = gui::title("SCORRIBANDA", GT("Picchiaduro a scorrimento", "Belt-scroll beat'em up"), ACC);
    // two big brawlers squaring up behind the menu like a noir fight poster: black silhouettes with a blood-red
    // rim at the screen edges (the menu text stays the sharpest thing); the left one shadow-boxes
    uint32_t now = br_now_ms();
    BrState st; float an;
    hero_signature(0, (now % 2400) / 2400.0f, &st, &an);
    pose_hero(0, st, an, 26, CH - 4, 28, +1, 150, ACC);
    pose_hero(1, BS_IDLE, (now % 4000) / 4000.0f, BR_SW - 26, CH - 4, 28, -1, 150, ACC);
    const char *it[NM_ITEMS] = { GT("Gioca", "Play"), GT("Co-op 2 giocatori", "Co-op 2 players"), GT("Opzioni", "Options") };
    gui::menu(s_menu, it, NM_ITEMS, y, CH, ACC);
}

// ============================ SC_SEL (character select) ======================
// Co-op: each Cardputer picks only ITS fighter (host = player 0, join = player 1).
static int s_coop = 0;         // whether this select session is co-op
static int s_coop_host = 1;    // co-op role chosen on SC_COOP: 1 = Host (player 0), 0 = Join (player 1)

// Trade-offs only read at a glance if each bar is filled RELATIVE to the roster: the leader is full, the
// laggard a short (never empty) stub. Mapped into 0.30..1.00 over [lo..hi].
static float stat_frac(int stat, int idx) {
    float v[3];
    for (int i = 0; i < 3; i++) {
        const HeroDef *h = brawler_hero(i);
        v[i] = stat == 0 ? (float)h->maxhp : stat == 1 ? h->speed : stat == 2 ? (float)(h->pdmg + h->kdmg) : h->reach;
    }
    float lo = fminf(v[0], fminf(v[1], v[2])), hi = fmaxf(v[0], fmaxf(v[1], v[2]));
    return hi - lo < 0.001f ? 1.0f : 0.30f + 0.70f * (v[idx] - lo) / (hi - lo);
}

static void hero_card(int idx, int x, int cw) {
    const HeroDef *h = brawler_hero(idx);
    bool sel = (g.sel == idx);
    int top = 21, hgt = 74;
    gui::panel(x, top, cw, hgt, sel ? gui::rgb(109, 0, 0) : gui::rgb(36, 36, 85), sel ? ACC : gui::rgb(73, 73, 85));
    gui::text(h->name, x + cw / 2, top + 2, 1, gui::text_width(h->name, gui::F_BODY) <= cw - 4 ? gui::F_BODY : gui::F_SMALL,
              sel ? WHITE : MUTED, BLACK);
    // the posed silhouette AT THIS HERO'S OWN BUILD; the pick performs its signature move on a loop, the
    // others idle out of phase so the line-up breathes.
    uint32_t now = br_now_ms();
    BrState pst = BS_IDLE; float panim = (now % 3000) / 3000.0f + idx * 0.33f;
    if (sel) hero_signature(idx, (now % 1600) / 1600.0f, &pst, &panim);
    pose_hero(idx, pst, panim, x + cw / 2, top + 55, 17, +1, sel ? 0 : 130, sel ? ACC : 0);
    gui::text(brawler_hero_style(idx), x + cw / 2, top + 56, 1, gui::F_SMALL, sel ? WHITE : MUTED, BLACK);
}
// The picked hero's four stats under the cards, relative to the roster (power in red: the edge).
static void hero_stats(int idx) {
    const char *tag[4] = { GT("VIT", "HP"), GT("VEL", "SPD"), GT("FOR", "POW"), GT("POR", "RCH") };
    for (int s = 0; s < 4; s++) {
        int x = 6 + (s & 1) * 118, y = 99 + (s >> 1) * 11;
        d.setTextColor(MUTED); d.setCursor(x, y); d.print(tag[s]);
        int bx = x + 22, bw = 88;
        d.fillRect(bx, y, bw, 7, gui::rgb(36, 36, 85));
        d.fillRect(bx, y, (int)(bw * stat_frac(s, idx)), 7, s == 2 ? ACC : WHITE);
    }
}
static void draw_select(void) {
    backdrop();
    const char *hdr = s_coop ? (s_coop_host ? GT("OSPITI: scegli il tuo", "HOST: pick yours") : GT("TI UNISCI: scegli il tuo", "JOIN: pick yours"))
                             : GT("Scegli il lottatore", "Choose your fighter");
    gui::text(hdr, BR_SW / 2, 2, 1, gui::text_width(hdr, gui::F_BODY) <= BR_SW - 8 ? gui::F_BODY : gui::F_SMALL, WHITE, BLACK);
    int cw = (BR_SW - 8 - 2 * 4) / 3;
    for (int i = 0; i < 3; i++) hero_card(i, 4 + i * (cw + 4), cw);
    hero_stats(g.sel);
}

// Commit the chosen hero(es) into `g`, configure the heroes and start the match (solo, or a co-op retry).
static void start_match(void) {
    // Co-op brings up the ESP-NOW session as host; if the radio won't start we fall back to solo so the
    // menu item is never a dead end.
    g.net = false; g.is_host = false;
    if (s_coop && bnet_start()) { g.net = true; g.is_host = true; }
    g.nplayers = g.net ? 2 : 1;
    br_reset_fighters();
    for (int i = 0; i < g.nplayers; i++) {
        Fighter *fr = &g.f[i];
        memset(fr, 0, sizeof *fr);
        const HeroDef *h = brawler_hero(g.hero_pick[i]);
        fr->on = true; fr->is_hero = true; fr->kind = g.hero_pick[i]; fr->player = (uint8_t)i;
        fr->x = 80.0f + i * 30.0f; fr->z = 0.6f; fr->dir = +1;
        fr->maxhp = fr->hp = h->maxhp;
        fr->st = BS_IDLE; fr->aspd = 1.0f;
    }
    g.lives = 3; g.score = 0; g.level = 0;
    g.combo = 0; g.combo_t = 0;
    s_trail[0] = s_trail[1] = 1.0f;
    brfx_reset();
    levels_begin(0);
    g.screen = SC_PLAY;
}

// Co-op: both peers paired -> set up BOTH heroes and enter the match. The HOST owns the whole simulation
// and streams snapshots; the GUEST opens its slots and renders what arrives. Called by the shell when
// bnet_available().
void menu_coop_start(void) {
    g.nplayers = 2;
    br_reset_fighters();
    for (int i = 0; i < 2; i++) {
        Fighter *fr = &g.f[i];
        memset(fr, 0, sizeof *fr);
        const HeroDef *h = brawler_hero(g.hero_pick[i]);
        fr->on = true; fr->is_hero = true; fr->kind = g.hero_pick[i]; fr->player = (uint8_t)i;
        fr->x = 70.0f + i * 34.0f; fr->z = 0.55f + i * 0.10f; fr->dir = +1;
        fr->maxhp = fr->hp = h->maxhp;
        fr->st = BS_IDLE; fr->aspd = 1.0f;
    }
    g.lives = 3; g.score = 0; g.level = 0; g.combo = 0; g.combo_t = 0;
    s_trail[0] = s_trail[1] = 1.0f;
    brfx_reset();
    if (g.is_host) levels_begin(0);   // only the host simulates the waves
    g.screen = SC_PLAY;
}

// ============================ SC_OPT ========================================
#define NO_ITEMS 3
static void draw_options(void) {
    int y = gui::title(GT("Opzioni", "Options"), nullptr, ACC);
    char a[32], df[40];
    snprintf(a, sizeof a, "%s: %s", GT("Audio", "Audio"), g.audio ? GT("Si", "On") : GT("No", "Off"));
    snprintf(df, sizeof df, "%s: %s", GT("Difficolta", "Difficulty"),
             g.diff == 0 ? GT("Facile", "Easy") : g.diff == 1 ? GT("Normale", "Normal") : GT("Difficile", "Hard"));
    const char *it[NO_ITEMS] = { a, df, GT("Comandi", "Controls") };
    gui::menu(s_menu, it, NO_ITEMS, y, CH, ACC);
}

// ============================ SC_HELP =======================================
static void draw_help(void) {
    int y = gui::title(GT("Comandi", "Controls"), nullptr, ACC);
    const char *key[5] = { "E S A D", "J", "K", GT("L / Spazio", "L / Space"), "Esc / TAB" };
    const char *act[5] = { GT("muovi", "move"), GT("pugno, ripeti: combo", "punch, repeat: combo"),
                           GT("calcio, atterra", "kick, knocks down"), GT("salto, J/K in aria", "jump, J/K in the air"),
                           GT("pausa", "pause") };
    for (int i = 0; i < 5; i++) {
        int ry = y + i * 16;
        gui::text(key[i], 10, ry, 0, gui::F_SMALL, i == 1 ? ACC : WHITE, BLACK);
        gui::text(act[i], 84, ry, 0, gui::F_SMALL, MUTED, BLACK);
    }
}

// ============================ SC_COOP (host / join) =========================
static void draw_coop(void) {
    int y = gui::title(GT("Co-op 2 giocatori", "Co-op 2 players"), GT("Uno ospita, l'altro si unisce", "One hosts, the other joins"), ACC);
    const char *it[2] = { GT("Ospita la partita", "Host a game"), GT("Unisciti", "Join") };
    gui::menu(s_menu, it, 2, y, CH, ACC);
}

// ============================ public: menu_draw =============================
// The cards over the frozen fight (pause / game over / street cleared) and every full screen.
void menu_draw(void) {
    char l1[48], l2[48];
    switch (g.screen) {
        case SC_MENU:  draw_title();   break;
        case SC_SEL:   draw_select();  break;
        case SC_COOP:  draw_coop();    break;
        case SC_OPT:   draw_options(); break;
        case SC_HELP:  draw_help();    break;
        case SC_LOBBY: {
            gui::title(GT("Co-op 2 giocatori", "Co-op 2 players"), nullptr, ACC);
            int dn = (int)((br_now_ms() / 300) % 4);             // a pulsing row of dots under the card
            for (int k = 0; k < 3; k++) d.fillCircle(BR_SW / 2 - 16 + k * 16, CH - 10, k < dn ? 4 : 2, k < dn ? ACC : gui::rgb(70, 80, 110));
            gui::dialog(g.is_host ? GT("Ospito...", "Hosting...") : GT("Cerco...", "Searching..."),
                        g.is_host ? GT("Attendo il 2o giocatore", "Waiting for player 2") : GT("Cerco una partita vicina", "Looking for a nearby game"),
                        nullptr, nullptr, ACC);
            break;
        }
        case SC_PAUSE:
            snprintf(l1, sizeof l1, GT("Punti %ld", "Score %ld"), g.score);
            gui::dialog(GT("PAUSA", "PAUSED"), brawler_level_name(g.level), l1, GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave"), ACC);
            break;
        case SC_OVER:
            snprintf(l1, sizeof l1, GT("Punti %ld", "Score %ld"), g.score);
            snprintf(l2, sizeof l2, "%s %d: %s", GT("Livello", "Level"), g.level + 1, brawler_level_name(g.level));
            gui::dialog(GT("FINE PARTITA", "GAME OVER"), l1, l2, GT("INVIO riprova  Esc menu", "ENTER retry  Esc menu"), ACC);
            break;
        case SC_CLEAR: {
            bool last = (g.level >= brawler_level_count() - 1);
            if (last) snprintf(l1, sizeof l1, "%s", GT("Hai ripulito la citta!", "You cleared the city!"));
            else      snprintf(l1, sizeof l1, "%s: %s", GT("Prossima", "Next"), brawler_level_name(g.level + 1));
            snprintf(l2, sizeof l2, GT("Punti %ld", "Score %ld"), g.score);
            gui::dialog(last ? GT("VITTORIA!", "VICTORY!") : GT("ZONA RIPULITA", "AREA CLEARED"), l1, l2,
                        last ? GT("INVIO menu", "ENTER menu") : GT("INVIO avanti", "ENTER continue"), ACC);
            break;
        }
        default: break;
    }
}

// True while the current non-play screen shows motion (posing heroes, the gliding pill, the lobby dots): a
// still screen returns false from poll and the run loop pushes nothing.
bool menu_animating(int dt_ms) {
    bool glide = gui::menu_tick(s_menu, dt_ms);
    return glide || g.screen == SC_MENU || g.screen == SC_SEL || g.screen == SC_LOBBY;
}

// The OS footer hint for the current screen (the shell refreshes it whenever the text changes).
const char *menu_hint(void) {
    switch (g.screen) {
        case SC_MENU:  return GT("SU/GIU scegli  INVIO ok  Esc esci", "UP/DN pick  ENTER ok  Esc quit");
        case SC_SEL:   return GT("SX/DX scegli  INVIO via  Esc menu", "LEFT/RIGHT pick  ENTER go  Esc menu");
        case SC_COOP:  return GT("SU/GIU scegli  INVIO ok  Esc menu", "UP/DN pick  ENTER ok  Esc menu");
        case SC_LOBBY: return GT("Esc annulla", "Esc cancel");
        case SC_OPT:   return GT("SU/GIU  INVIO cambia  Esc menu", "UP/DN  ENTER change  Esc menu");
        case SC_PAUSE: return GT("INVIO riprendi  Esc esci", "ENTER resume  Esc leave");
        case SC_OVER:  return GT("INVIO riprova  Esc menu", "ENTER retry  Esc menu");
        case SC_CLEAR: return GT("INVIO avanti  Esc menu", "ENTER continue  Esc menu");
        case SC_HELP:  return GT("Esc indietro", "Esc back");
        default:       return "";
    }
}

// ============================ public: hud_draw =============================
// The fight's HUD over the night street: per hero a name + an arcade health bar (gold, red when low, with a
// white "damage trail" that drains after the hit), shared lives, the score, the street's wave pips, a
// popping COMBO, the GO arrow once the street is clear, the stage / boss banner and the boss's own bar.
static void bar(int x, int y, int w, int h, float frac, float trail, bool right, uint16_t col) {
    d.fillRect(x - 1, y - 1, w + 2, h + 2, BR_INK);
    d.fillRect(x, y, w, h, gui::rgb(36, 36, 85));
    int fw = (int)(w * frac), tw = (int)(w * trail);
    if (tw > fw) d.fillRect(right ? x + w - tw : x + fw, y, tw - fw, h, BR_WHITE);
    if (fw > 0) {
        uint16_t c = col ? col : frac < 0.3f ? MN_RED : BR_GOLD;
        d.fillRect(right ? x + w - fw : x, y, fw, h, c);
        d.drawFastHLine(right ? x + w - fw : x, y, fw, gui::mix(c, BR_WHITE, 140));   // a lit top edge
    }
}
static void hero_hud(int slot, int x, int w, bool right) {
    Fighter *fr = br_hero(slot);
    if (!fr) return;
    float f = fr->maxhp > 0 ? (float)(fr->hp < 0 ? 0 : fr->hp) / fr->maxhp : 0;
    if (s_trail[slot] < f) s_trail[slot] = f;
    s_trail[slot] += (f - s_trail[slot]) * 0.06f;
    gui::text(brawler_hero(fr->kind)->name, right ? x + w : x, 0, right ? 2 : 0, gui::F_SMALL, brawler_hero_color(fr->kind), BR_INK);
    bar(x, 16, w, 5, f, s_trail[slot], right, 0);
}
void hud_draw(void) {
    hero_hud(0, 4, 80, false);
    if (g.nplayers > 1) hero_hud(1, BR_SW - 84, 80, true);
    for (int p = 0; p < g.lives && p < 5; p++) {                       // shared lives: red pips under P1's bar
        d.fillCircle(7 + p * 8, 27, 3, BR_INK); d.fillCircle(7 + p * 8, 27, 2, MN_RED);
    }
    // score (solo: top right; co-op: centre) + this street's waves as pips (filled = beaten)
    char sb[20]; snprintf(sb, sizeof sb, "%ld", g.score);
    int sx = g.nplayers > 1 ? BR_SW / 2 : BR_SW - 4;
    gui::text(sb, sx, 0, g.nplayers > 1 ? 1 : 2, gui::F_BODY, BR_WHITE, BR_INK);
    const LevelDef *L = brawler_level(g.level);
    for (int w = 0; w < L->nwaves; w++) {
        int px = sx - (g.nplayers > 1 ? (L->nwaves * 7) / 2 - w * 7 - 3 : 4 + (L->nwaves - 1 - w) * 7);
        bool done = w < g.wave || (w == g.wave && g.gatex >= g.level_len);
        d.fillCircle(px, 24, 2, done ? BR_GOLD : gui::rgb(73, 73, 85));
    }

    // COMBO pop — big right after a hit, then it settles
    if (g.combo > 1) {
        char cb[18]; snprintf(cb, sizeof cb, "COMBO x%d", g.combo);
        gui::text(cb, BR_SW / 2, 26, 1, g.combo_t > 0.6f ? gui::F_BODY : gui::F_SMALL, ((br_now_ms() >> 6) & 1) ? MN_REDHI : BR_GOLD, BR_INK);
    }
    // GO: the last wave is down and the street is open -> point the player at the exit
    if (g.gatex >= g.level_len && brawler_live_enemies() == 0 && ((br_now_ms() / 300) & 1)) {
        int x = BR_SW - 12, y = 58;
        gui::text(GT("VAI", "GO"), x - 14, y - 9, 2, gui::F_TITLE, BR_GOLD, BR_INK);
        d.fillTriangle(x - 10, y - 10, x - 10, y + 10, x + 2, y, BR_GOLD);
    }
    // the boss's own bar along the bottom while it stands
    for (int i = 2; i < BR_MAXF; i++) {
        Fighter *e = &g.f[i];
        if (!e->on || e->kind != 3 || e->hp <= 0) continue;
        gui::text(GT("CAPOBANDA", "GANG BOSS"), 6, BR_SH - 22, 0, gui::F_SMALL, gui::rgb(182, 73, 255), BR_INK);
        bar(6, BR_SH - 6, BR_SW - 12, 4, (float)e->hp / e->maxhp, 0, false, gui::rgb(182, 73, 255));
        break;
    }
    // the banner: the stage card as a street begins, a warning as the boss walks in. It slides in from
    // the left (ease-out), holds, and slides away.
    if (g.banner_t > 0.0f) {
        float t = g.banner_t, tot = g.banner ? 2.0f : 2.2f, in = tot - t;
        float k = in < 0.25f ? in / 0.25f : t < 0.25f ? t / 0.25f : 1.0f;
        k = 1.0f - (1.0f - k) * (1.0f - k);
        int ox = (int)((1.0f - k) * -BR_SW), y = 34;
        d.fillRect(ox, y, BR_SW, 34, BR_INK);
        d.drawFastHLine(ox, y, BR_SW, g.banner ? MN_RED : BR_GOLD);
        d.drawFastHLine(ox, y + 33, BR_SW, g.banner ? MN_RED : BR_GOLD);
        char hb[32];
        if (g.banner) snprintf(hb, sizeof hb, "%s", GT("ATTENZIONE", "WARNING"));
        else          snprintf(hb, sizeof hb, GT("STRADA %d", "STREET %d"), g.level + 1);
        gui::text(hb, ox + BR_SW / 2, y + 2, 1, gui::F_SMALL, g.banner ? MN_RED : BR_GOLD, BR_INK);
        gui::text(g.banner ? GT("Arriva il capobanda!", "Here comes the boss!") : brawler_level_name(g.level),
                  ox + BR_SW / 2, y + 15, 1, gui::F_BODY, BR_WHITE, BR_INK);
    }
}

// ============================ public: menu_goto ============================
void menu_goto(BrScreen s) {
    // a "back" cue when retreating to a lighter screen, else a select cue
    bool back = (s == SC_MENU || s == SC_PAUSE);
    g.screen = s;
    g.sel = 0;
    s_menu.sel = 0; s_menu.pos = 0;
    // a result screen ignores keys briefly: a player still mashing J/K (mapped to ENTER) must not skip it
    g.lock_until = (s == SC_OVER || s == SC_CLEAR) ? br_now_ms() + 700 : 0;
    bsfx(back ? BSFX_BACK : BSFX_SEL);
    nucleo_app_request_draw();
}

// ============================ public: menu_key ============================
// Navigate the current menu screen. Returns true if the key was consumed. The shell routes all
// keys here while g.screen != SC_PLAY; we never call framework fullscreen (the shell owns that).
bool menu_key(int key, char ch) {
    if (br_now_ms() < g.lock_until) return true;
    int n = g.screen == SC_MENU ? NM_ITEMS : g.screen == SC_OPT ? NO_ITEMS : g.screen == SC_COOP ? 2 : 0;
    if (n && key == NK_CHAR && ch >= '1' && ch < '1' + n) { s_menu.sel = (int8_t)(ch - '1'); key = NK_ENTER; }   // 1-n quick pick
    if (n && gui::menu_key(s_menu, key, n)) { bsfx(BSFX_NAV); return true; }
    switch (g.screen) {
        // -------- title --------
        case SC_MENU:
            if (key != NK_ENTER) return false;
            if (s_menu.sel == 0)      { s_coop = 0; menu_goto(SC_SEL); }    // solo
            else if (s_menu.sel == 1) { s_coop = 1; menu_goto(SC_COOP); }   // co-op -> pick Host or Join first
            else                      menu_goto(SC_OPT);
            return true;

        // -------- character select --------
        case SC_SEL: {
            int nh = brawler_hero_count();
            if (key == NK_LEFT || key == NK_UP)    { g.sel = (g.sel + nh - 1) % nh; bsfx(BSFX_NAV); return true; }
            if (key == NK_RIGHT || key == NK_DOWN) { g.sel = (g.sel + 1) % nh;      bsfx(BSFX_NAV); return true; }
            if (key == NK_ENTER) {
                if (s_coop) {
                    // NETWORK co-op: each Cardputer picks only ITS fighter, brings up ESP-NOW in its
                    // role (host = player 0, join = player 1), then waits in the lobby to pair.
                    int myslot = s_coop_host ? 0 : 1;
                    g.hero_pick[myslot] = g.sel;
                    g.is_host = s_coop_host ? true : false;
                    if (bnet_start()) { g.net = true; menu_goto(SC_LOBBY); }
                    else { s_coop = 0; g.net = false; g.hero_pick[0] = g.sel; bsfx(BSFX_SEL); start_match(); }
                } else {
                    g.hero_pick[0] = g.sel;
                    bsfx(BSFX_SEL);
                    start_match();                // sets g.screen = SC_PLAY
                }
                return true;
            }
            return false;
        }

        // -------- co-op: Host or Join --------
        case SC_COOP:
            if (key != NK_ENTER) return false;
            s_coop_host = (s_menu.sel == 0) ? 1 : 0;  // item 0 = Host, item 1 = Join
            menu_goto(SC_SEL);                        // then pick YOUR fighter
            return true;

        // -------- options --------
        case SC_OPT:
            if (key != NK_LEFT && key != NK_RIGHT && key != NK_ENTER) return false;
            if (s_menu.sel == 0)      g.audio = !g.audio;
            else if (s_menu.sel == 1) g.diff = (g.diff + (key == NK_LEFT ? 2 : 1)) % 3;
            else if (key == NK_ENTER) { menu_goto(SC_HELP); return true; }
            bsfx(BSFX_SEL);
            return true;

        // -------- pause: ENTER resumes (Esc leaves, in the shell's back handler) --------
        case SC_PAUSE:
            if (key == NK_ENTER) { g.paused = false; g.screen = SC_PLAY; bsfx(BSFX_BACK); return true; }
            return false;

        // -------- game over: ENTER = the same fighters again --------
        case SC_OVER:
            if (key == NK_ENTER) { s_coop = (g.nplayers > 1); start_match(); return true; }
            return false;

        // -------- level cleared / victory --------
        case SC_CLEAR:
            if (key == NK_ENTER) {
                if (g.level >= brawler_level_count() - 1) { menu_goto(SC_MENU); }   // final victory -> menu
                else {
                    g.level++;
                    brfx_reset();
                    levels_begin(g.level);
                    // drop the heroes back at the start of the new street
                    for (int p = 0; p < g.nplayers && p < 2; p++) {
                        Fighter *fr = &g.f[p];
                        if (!fr->on || !fr->is_hero) continue;
                        fr->x = g.camx + 30.0f + p * 22.0f; fr->z = 0.6f;
                        fr->vx = fr->vz = fr->vy = fr->yoff = 0.0f; fr->st = BS_IDLE; fr->anim = 0.0f;
                    }
                    g.combo = 0; g.combo_t = 0;
                    g.screen = SC_PLAY;
                    bsfx(BSFX_SEL);
                }
                return true;
            }
            return false;

        default:
            return false;   // help / lobby: Esc is the shell's back handler
    }
}
