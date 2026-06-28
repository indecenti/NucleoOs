// brawler_menu.cpp — SCORRIBANDA: all non-play screens + the in-action HUD.
//
// Minimal-white belt-scroll brawler front-end: title, character-select (silhouette line-up with stat
// bars), options, pause, game-over, level-clear, help — plus hud_draw() that floats compact health/
// score/lives/combo over the 240x135 fight. Art law (frozen in brawler.h): warm-white "paper" field,
// GREY line-art chrome, INK silhouettes/type, RED only as the accent (blood/danger/combo). Everything
// reads dark-on-white. Bilingual via g.lang (0=IT default, 1=EN). All state lives in `g`
// (app_brawler.cpp); this module only reads/writes it and draws through `d`. HEAP-FREE, fixed arrays,
// ASCII only.
//
// Drawing idioms (text helpers, centered print, segmented bars) follow app_pong.cpp / app_tanks.cpp.
// Posed silhouettes on title/select reuse fighter_pose() + fxfig::figure() — no animation state owned
// here; we synth a throwaway Fighter, ask chars for the pose, and ink it BR_INK so it reads on paper.

#include "brawler.h"
#include "app_gfx.h"
#include "nucleo_kbd.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// ============================ minimal-white palette ==========================
// Strict palette only (brawler.h). Paper field, a grey line-art ramp for chrome/type, INK for the
// heavy silhouettes + strong type, RED as the lone warm accent. No other hue is introduced here.
#define MN_BG    BR_PAPER                // warm-white paper field
#define MN_PANEL br_rgb(236, 236, 231)   // a hair off-white — raised card on the paper
#define MN_SEL   br_rgb(224, 225, 222)   // selected-row fill (light grey, still dark text on top)
#define MN_INK   BR_INK                  // silhouette body + strongest type (near-black)
#define MN_GREY  br_rgb(96, 100, 106)    // primary type — dark grey on white
#define MN_DIM   BR_GREY_NEAR            // secondary type
#define MN_FAINT BR_GREY_FAR             // faint rules / inactive
#define MN_WHITE BR_INK                  // "highlight" type is now INK so it reads on paper
#define MN_RED   BR_BLOOD                // the only warm accent
#define MN_REDHI br_rgb(232, 56, 50)     // hotter red (combo pop) — same warm family
#define MN_RIM   BR_GREY_MID             // silhouette edge-light (grey rim on the ink body)

// ============================ text helpers (pong idiom) ======================
static inline const char *tx(const char *it, const char *en) { return g.lang ? en : it; }
static void txt(int x, int y, int sz, uint16_t col, const char *s) {
    d.setTextSize(sz); d.setTextColor(col); d.setCursor(x, y); d.print(s);
}
static void txt_c(int cx, int y, int sz, uint16_t col, const char *s) {   // centered
    txt(cx - (int)strlen(s) * 3 * sz, y, sz, col, s);
}
static void txt_r(int rx, int y, int sz, uint16_t col, const char *s) {   // right-aligned
    txt(rx - (int)strlen(s) * 6 * sz, y, sz, col, s);
}
// centered text that NEVER overflows a box `w` px wide: truncated to the chars that fit at size sz.
static void txt_cfit(int cx, int y, int w, int sz, uint16_t col, const char *s) {
    int maxch = w / (6 * sz);
    if (maxch < 1) maxch = 1;
    int n = (int)strlen(s);
    if (n <= maxch) { txt_c(cx, y, sz, col, s); return; }
    char buf[40];
    if (maxch > (int)sizeof(buf) - 1) maxch = (int)sizeof(buf) - 1;
    memcpy(buf, s, maxch);
    buf[maxch] = 0;
    txt_c(cx, y, sz, col, buf);
}

// ============================ shared chrome ==================================
// Minimal-white backdrop: a clean paper field with a few light grey line-art marks and gentle idle
// motion — multi-layer parallax (far/mid/near greys), lots of white space, NO floor grid, no fill
// boxes. The marks drift slowly with time so a static menu still breathes.
static void backdrop(void) {
    d.fillRect(0, 0, BR_SW, BR_SH, MN_BG);
    uint32_t t = br_now_ms();

    // (far) a faint horizon line low on the field + a couple of distant building outlines that
    // crawl very slowly to the left — the slowest parallax layer.
    int hz = BR_SH - 22;
    d.drawFastHLine(0, hz, BR_SW, BR_GREY_FAR);
    int fx = (int)((t / 90) % 80);
    for (int b = -1; b < 4; b++) {
        int bx = b * 80 - fx;
        int bh = 14 + ((b * 37) & 7);
        d.drawRect(bx + 10, hz - bh, 26, bh, BR_GREY_FAR);   // outline only — line-art, never filled
    }

    // (mid) a slim ground rule + a single drifting mid-grey post for parallax depth.
    d.drawFastHLine(0, hz + 8, BR_SW, BR_GREY_MID);
    int mx = (int)((t / 45) % (BR_SW + 40)) - 20;
    d.drawFastVLine(mx, hz - 18, 26, BR_GREY_MID);

    // (near) two long thin rules near the bottom for a clean stage edge — the nearest layer.
    d.drawFastHLine(0, BR_SH - 5, BR_SW, BR_GREY_NEAR);
    d.drawFastHLine(0, BR_SH - 3, BR_SW, BR_GREY_FAR);
}
// One stat bar: label-less gauge — a thin outlined casing (line-art) with a solid grey/red fill.
static void statbar(int x, int y, int w, float frac, uint16_t fill) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    d.drawRect(x, y, w, 5, BR_GREY_FAR);       // outlined casing — keeps the line-art look
    int fw = (int)((w - 2) * frac);
    if (fw > 0) d.fillRect(x + 1, y + 1, fw, 3, fill);
}

// A posed silhouette of hero `kind` in state `st`, drawn at screen (sx, feetY) at base scale `sc` facing
// dir. Uses the chars module for the pose; pure flavour, owns no animation state. The hero's OWN build is
// honoured here so the roster's size/shape differences read on the line-up: `sc` is multiplied by the
// hero's scale field (HEIGHT) and its girth feeds the renderer (BUILD/width) — Mole towers and bulks up,
// Vipera is small and lean, exactly as their defs intend. A 0/unset scale is guarded to 1.0.
static void pose_hero(int kind, BrState st, float anim, float sx, float feetY, float sc, int dir) {
    Fighter fr;
    memset(&fr, 0, sizeof fr);
    fr.on = true; fr.is_hero = true; fr.kind = kind; fr.player = 0;
    fr.dir = dir; fr.st = st; fr.anim = anim; fr.aspd = 1.0f;
    const HeroDef *h = brawler_hero(kind);
    fr.maxhp = h ? h->maxhp : 100; fr.hp = fr.maxhp;
    float hscale = (h && h->scale > 0.01f) ? h->scale : 1.0f;   // guard unset -> 1.0
    float hgirth = (h && h->girth > 0.01f) ? h->girth : 1.0f;
    fxfig::Pt j[fxfig::FX_NJ];
    fighter_pose(&fr, j);
    fxfig::figure(j, sx, feetY, sc * hscale, dir, MN_INK, MN_RIM, hgirth);
}

// ============================ SC_MENU (title) ================================
#define NM_ITEMS 5
static const char *menu_item(int i) {
    switch (i) {
        case 0: return tx("Gioca 1P", "Play 1P");
        case 1: return tx("Co-op 2P", "Co-op 2P");
        case 2: return tx("Opzioni", "Options");
        case 3: return tx("Aiuto", "Help");
        default: return tx("Esci", "Quit");
    }
}
static void draw_title(void) {
    backdrop();
    // two flanking brawler silhouettes squaring up over the title — ink on paper, idle sway.
    float t = (br_now_ms() % 4000) / 4000.0f;
    pose_hero(0, BS_IDLE, t,        46, BR_SH - 6, 30, +1);
    pose_hero(brawler_hero_count() > 1 ? 1 : 0, BS_IDLE, t + 0.5f, BR_SW - 46, BR_SH - 6, 30, -1);

    // big INK title with a faint grey offset for weight + a small red accent tick (the only colour).
    txt_c(BR_SW / 2 + 1, 13, 3, BR_GREY_FAR, "SCORRIBANDA");   // soft offset shadow on paper
    txt_c(BR_SW / 2,     11, 3, MN_INK,      "SCORRIBANDA");
    d.fillRect(BR_SW / 2 - 50, 33, 100, 1, BR_GREY_FAR);        // thin underline rule
    d.fillRect(BR_SW / 2 - 4,  32, 8, 3, MN_RED);              // centered red accent on the rule
    txt_c(BR_SW / 2, 38, 1, MN_DIM, tx("rissa a scorrimento", "belt-scroll brawl"));

    for (int i = 0; i < NM_ITEMS; i++) {
        int y = 48 + i * 17;
        bool f = (i == g.sel);
        if (f) {
            d.fillRoundRect(34, y - 3, BR_SW - 68, 17, 3, MN_SEL);   // light-grey highlight (taller for big type)
            d.fillRect(36, y - 1, 3, 13, MN_RED);                     // red tick
        }
        txt_c(BR_SW / 2, y, 2, MN_INK, menu_item(i));   // BIG menu type (size 2)
    }
    txt_c(BR_SW / 2, BR_SH - 9, 1, MN_DIM, tx("SU/GIU  INVIO  Esc", "UP/DN  ENTER  Esc"));
}

// ============================ SC_SEL (character select) ======================
// Co-op picks player 1 first then player 2; g.sel scrolls the 3 heroes, g.nplayers / g.hero_pick[]
// are committed on confirm. We stage the choice in module-local s_pick until both players are set.
static int s_pickslot = 0;     // 0 = choosing P1, 1 = choosing P2 (co-op only)
static int s_coop = 0;         // whether this select session is co-op
static int s_coop_host = 1;    // co-op role chosen on SC_COOP: 1 = Host (player 0), 0 = Join (player 1)

// ---- roster-relative stat normalisation -------------------------------------
// Trade-offs only read at a glance if each bar is filled RELATIVE to the strongest hero in that stat.
// We scan the (tiny, fixed) roster once and stretch each gauge over [min..max] so the leader is full and
// the laggard is visibly short — no magic divisors, and the picture stays honest if defs change.
struct StatRange { float hp_lo, hp_hi, sp_lo, sp_hi, pw_lo, pw_hi, rc_lo, rc_hi; };
static void stat_range(StatRange *r) {
    r->hp_lo = r->sp_lo = r->pw_lo = r->rc_lo = 1e9f;
    r->hp_hi = r->sp_hi = r->pw_hi = r->rc_hi = -1e9f;
    int n = brawler_hero_count();
    if (n > 3) n = 3;
    for (int i = 0; i < n; i++) {
        const HeroDef *h = brawler_hero(i);
        if (!h) continue;
        float hp = (float)h->maxhp;
        float sp = h->speed;
        float pw = (float)(h->pdmg + h->kdmg);
        float rc = h->reach;
        if (hp < r->hp_lo) r->hp_lo = hp;
        if (hp > r->hp_hi) r->hp_hi = hp;
        if (sp < r->sp_lo) r->sp_lo = sp;
        if (sp > r->sp_hi) r->sp_hi = sp;
        if (pw < r->pw_lo) r->pw_lo = pw;
        if (pw > r->pw_hi) r->pw_hi = pw;
        if (rc < r->rc_lo) r->rc_lo = rc;
        if (rc > r->rc_hi) r->rc_hi = rc;
    }
}
// Map a value into a comfortable 0.30..1.00 band over [lo..hi] so even the weakest stat shows a stub
// (an empty bar reads as "broken", a short bar reads as "this is the trade-off").
static float stat_frac(float v, float lo, float hi) {
    float span = hi - lo;
    if (span < 0.001f) return 1.0f;          // all equal -> all full
    float t = (v - lo) / span;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    return 0.30f + 0.70f * t;
}
// One labelled stat row: a 3-char ink tag on the left, a relative gauge on the right.
static void stat_row(int x, int y, int w, const char *tag, float frac, uint16_t fill) {
    txt(x, y - 1, 1, MN_DIM, tag);
    statbar(x + 22, y, w - 22, frac, fill);
}

// A red check tick (two strokes) — the unmistakable "this is your pick" mark, the lone warm accent.
static void tick(int cx, int cy) {
    d.drawLine(cx - 4, cy,     cx - 1, cy + 3, MN_RED);
    d.drawLine(cx - 4, cy + 1, cx - 1, cy + 4, MN_RED);   // doubled stroke = bolder on the tiny screen
    d.drawLine(cx - 1, cy + 3, cx + 5, cy - 4, MN_RED);
    d.drawLine(cx - 1, cy + 4, cx + 5, cy - 3, MN_RED);
}

// The selected hero performs a SIGNATURE move on a short loop so the pick feels alive: each hero cycles
// idle -> its flavour strike -> idle. OMBRA jabs, MOLE throws its heavy finisher, VIPERA snaps a long
// kick — matching their style blurbs. Returns the state + anim phase to pose with.
static void hero_signature(int kind, float loop, BrState *st, float *anim) {
    // loop is 0..1 over ~1.6s: hold idle, wind the strike through its 0->1 swing, settle back to idle.
    if (loop < 0.45f) {                       // idle breath before the move
        *st = BS_IDLE; *anim = loop;
        return;
    }
    float a = (loop - 0.45f) / 0.45f;         // 0..1 strike window (last slice is idle again)
    if (a > 1.0f) { *st = BS_IDLE; *anim = loop; return; }
    if (kind == 1) {                          // MOLE — devastating finisher (var at chain end)
        *st = BS_PUNCH; *anim = a;
        return;
    }
    if (kind == 2) {                          // VIPERA — long, snappy kick
        *st = BS_KICK; *anim = a;
        return;
    }
    *st = BS_PUNCH; *anim = a;                 // OMBRA — quick jab
}

static void hero_card(int idx, int x, int cw, int chosen_kind, bool active, const StatRange *rng) {
    const HeroDef *h = brawler_hero(idx);
    if (!h) return;
    int cy = 96;                              // feet line — lower so taller builds have headroom
    bool sel = (g.sel == idx) && active;
    bool locked = (chosen_kind == idx);      // already taken by P1 in co-op

    // card: off-white panel with a grey line-art border; selected gets a light fill + a bold red frame.
    d.fillRoundRect(x, 26, cw, 102, 4, sel ? MN_SEL : MN_PANEL);
    d.drawRoundRect(x, 26, cw, 102, 4, sel ? MN_RED : BR_GREY_FAR);
    if (sel) d.drawRoundRect(x + 1, 27, cw - 2, 100, 4, MN_RED);   // doubled frame = clearly chosen
    if (locked) {                            // greyed + a "P1" stamp: this fighter is the other player's
        d.drawRoundRect(x + 1, 27, cw - 2, 100, 4, BR_GREY_MID);
        txt_c(x + cw / 2, 30, 1, MN_DIM, "P1");
    }

    // a faint ground rule under the feet so the silhouette stands rather than floats on the card
    d.drawFastHLine(x + 6, cy + 1, cw - 12, BR_GREY_FAR);

    // posed silhouette AT THIS HERO'S OWN BUILD (pose_hero folds in scale=height + girth=width). The
    // selected hero performs its signature move on a loop; the others idle out of phase so the line-up
    // breathes. Base draw scale fits the tallest build inside the card height.
    float idleT = (br_now_ms() % 3000) / 3000.0f + idx * 0.33f;
    BrState pst = BS_IDLE; float panim = idleT;
    if (sel) {
        float loop = (br_now_ms() % 1600) / 1600.0f;
        hero_signature(idx, loop, &pst, &panim);
    }
    pose_hero(idx, pst, panim, x + cw / 2, cy, 23, +1);

    // name (ink) + style blurb (dim), BOTH clipped to the card width so they never spill outside it
    txt_cfit(x + cw / 2, 31, cw - 4, 1, MN_INK, h->name);
    txt_cfit(x + cw / 2, 41, cw - 4, 1, MN_DIM, g.lang ? h->style_en : h->style_it);

    // four stat rows, each filled RELATIVE to the roster so the trade-offs read instantly. Power + the
    // selected reach use the red accent; the rest stay grey ink so red still means "the edge".
    int bx = x + 5, bw = cw - 10;
    stat_row(bx, 100, bw, tx("VIT", "HP"),  stat_frac(h->maxhp,          rng->hp_lo, rng->hp_hi), MN_GREY);
    stat_row(bx, 107, bw, tx("VEL", "SPD"), stat_frac(h->speed,          rng->sp_lo, rng->sp_hi), MN_GREY);
    stat_row(bx, 114, bw, tx("FOR", "POW"), stat_frac(h->pdmg + h->kdmg, rng->pw_lo, rng->pw_hi), MN_RED);
    stat_row(bx, 121, bw, tx("GIT", "RCH"), stat_frac(h->reach,          rng->rc_lo, rng->rc_hi), MN_GREY);

    // a small red tick floating top-right of the selected card — the confirm-this affordance
    if (sel) tick(x + cw - 9, 31);
}
static void draw_select(void) {
    backdrop();
    const char *hdr;
    if (s_coop) hdr = s_coop_host ? tx("OSPITI - scegli il tuo", "HOST - pick yours")
                                  : tx("TI UNISCI - scegli il tuo", "JOIN - pick yours");
    else        hdr = tx("SCEGLI IL LOTTATORE", "CHOOSE YOUR FIGHTER");
    txt_c(BR_SW / 2, 6, 1, MN_INK, hdr);
    d.fillRect(BR_SW / 2 - 4, 15, 8, 2, MN_RED);                 // red accent tick under the header
    d.drawFastHLine(12, 20, BR_SW - 24, MN_FAINT);

    int n = brawler_hero_count();
    if (n > 3) n = 3;
    int gap = 5;
    int cw = (BR_SW - 10 - gap * (n - 1)) / (n > 0 ? n : 1);
    int chosen = (s_coop && s_pickslot == 1) ? g.hero_pick[0] : -1;
    StatRange rng;
    stat_range(&rng);
    for (int i = 0; i < n; i++) hero_card(i, 5 + i * (cw + gap), cw, chosen, true, &rng);

    txt(6, BR_SH - 7, 1, MN_DIM, tx("SX/DX scorri", "LEFT/RIGHT browse"));
    txt_r(BR_SW - 6, BR_SH - 7, 1, MN_RED, tx("INVIO scegli", "ENTER pick"));
}

// Commit the chosen hero(es) into `g`, configure the heroes and start the match.
static void start_match(void) {
    // Co-op brings up the ESP-NOW session as host; if the radio won't start we fall back to solo so the
    // menu item is never a dead end. (Full host/join lobby + sync verification is the co-op phase.)
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
        fr->maxhp = h ? h->maxhp : 100; fr->hp = fr->maxhp;
        fr->st = BS_IDLE; fr->anim = 0; fr->aspd = 1.0f;
    }
    g.lives = 3; g.score = 0; g.level = 0;
    g.combo = 0; g.combo_t = 0;
    brfx_reset();
    levels_begin(0);
    g.screen = SC_PLAY;
}

// Co-op: both peers paired -> set up BOTH heroes and enter the match. The HOST owns the whole simulation
// and streams snapshots; the GUEST opens its slots and renders what arrives. Hero kinds: the host knows
// its own (hero_pick[0]) and the guest's (hero_pick[1], learned via JOIN); the guest knows its own and
// gets the host's from the first snapshot (sb_hero.kind). Called by the shell when bnet_available().
void menu_coop_start(void) {
    g.nplayers = 2;
    br_reset_fighters();
    for (int i = 0; i < 2; i++) {
        Fighter *fr = &g.f[i];
        memset(fr, 0, sizeof *fr);
        const HeroDef *h = brawler_hero(g.hero_pick[i]);
        fr->on = true; fr->is_hero = true; fr->kind = g.hero_pick[i]; fr->player = (uint8_t)i;
        fr->x = 70.0f + i * 34.0f; fr->z = 0.55f + i * 0.10f; fr->dir = +1;
        fr->maxhp = h ? h->maxhp : 100; fr->hp = fr->maxhp;
        fr->st = BS_IDLE; fr->anim = 0; fr->aspd = 1.0f;
    }
    g.lives = 3; g.score = 0; g.level = 0; g.combo = 0; g.combo_t = 0;
    brfx_reset();
    if (g.is_host) levels_begin(0);   // only the host simulates the waves
    g.screen = SC_PLAY;
}

// ============================ SC_OPT ========================================
#define NO_ITEMS 3
static const char *opt_label(int i) {
    switch (i) {
        case 0: return "Audio";
        case 1: return tx("Lingua", "Language");
        default: return tx("Difficolta", "Difficulty");
    }
}
static void opt_value(int i, char *out, int cap, uint16_t *col) {
    *col = MN_GREY;
    switch (i) {
        case 0: snprintf(out, cap, "%s", g.audio ? "On" : "Off"); if (!g.audio) *col = MN_DIM; break;
        case 1: snprintf(out, cap, "%s", g.lang ? "English" : "Italiano"); break;
        default: snprintf(out, cap, "%s", g.diff == 0 ? tx("Facile", "Easy")
                                       : g.diff == 1 ? tx("Normale", "Normal")
                                                     : tx("Difficile", "Hard")); break;
    }
}
static void draw_options(void) {
    backdrop();
    txt_c(BR_SW / 2, 12, 2, MN_INK, tx("OPZIONI", "OPTIONS"));
    d.drawFastHLine(20, 34, BR_SW - 40, BR_GREY_FAR);
    for (int i = 0; i < NO_ITEMS; i++) {
        int y = 46 + i * 18;
        bool f = (i == g.sel);
        if (f) {
            d.fillRoundRect(20, y - 3, BR_SW - 40, 15, 3, MN_SEL);
            d.fillRect(22, y - 1, 2, 11, MN_RED);
        }
        txt(34, y, 1, MN_INK, opt_label(i));
        char vb[16]; uint16_t vc; opt_value(i, vb, sizeof vb, &vc);
        txt_r(BR_SW - 30, y, 1, vc, vb);
    }
    txt_c(BR_SW / 2, BR_SH - 10, 1, MN_DIM, tx("SX/DX cambia  Esc indietro", "LEFT/RIGHT change  Esc back"));
}

// ============================ SC_PAUSE ======================================
#define NP_ITEMS 2
static const char *pause_item(int i) {
    return i == 0 ? tx("Riprendi", "Resume") : tx("Esci al menu", "Quit to menu");
}
static void draw_pause(void) {
    // frost the frozen action: a paper scanline veil washes it toward white (translucent feel).
    for (int y = 0; y < BR_SH; y += 2) d.drawFastHLine(0, y, BR_SW, MN_BG);
    d.fillRoundRect(50, 32, BR_SW - 100, 70, 5, MN_PANEL);
    d.drawRoundRect(50, 32, BR_SW - 100, 70, 5, BR_GREY_MID);   // grey line-art card border
    txt_c(BR_SW / 2, 40, 2, MN_INK, tx("PAUSA", "PAUSED"));
    d.fillRect(BR_SW / 2 - 12, 58, 24, 2, MN_RED);             // small red accent under the title
    for (int i = 0; i < NP_ITEMS; i++) {
        int y = 66 + i * 16;
        bool f = (i == g.sel);
        if (f) {
            d.fillRoundRect(60, y - 2, BR_SW - 120, 13, 3, MN_SEL);
            d.fillRect(62, y, 2, 9, MN_RED);
        }
        txt_c(BR_SW / 2, y, 1, MN_INK, pause_item(i));
    }
}

// ============================ SC_OVER (game over) ===========================
#define NG_ITEMS 2
static const char *over_item(int i) {
    return i == 0 ? tx("Riprova", "Retry") : tx("Menu", "Menu");
}
static void draw_over(void) {
    backdrop();
    // a single blood smear behind the verdict — the only warm accent, dripping a couple of drops.
    d.fillEllipse(BR_SW / 2, 26, 64, 11, MN_RED);
    d.fillCircle(BR_SW / 2 - 30, 38, 2, MN_RED);
    d.fillCircle(BR_SW / 2 + 22, 40, 3, MN_RED);
    // INK verdict over the smear (reads on both paper and the red), faint paper offset for weight.
    txt_c(BR_SW / 2 + 1, 19, 3, BR_PAPER, "GAME OVER");
    txt_c(BR_SW / 2,     18, 3, MN_INK,   "GAME OVER");
    char sc[24]; snprintf(sc, sizeof sc, "%s %ld", tx("Punti", "Score"), g.score);
    txt_c(BR_SW / 2, 52, 1, MN_GREY, sc);
    const LevelDef *lv = brawler_level(g.level);
    if (lv) {
        char lb[32]; snprintf(lb, sizeof lb, "%s %d", tx("Livello", "Level"), g.level + 1);
        txt_c(BR_SW / 2, 64, 1, MN_DIM, lb);
    }
    for (int i = 0; i < NG_ITEMS; i++) {
        int y = 86 + i * 16;
        bool f = (i == g.sel);
        if (f) {
            d.fillRoundRect(70, y - 2, BR_SW - 140, 13, 3, MN_SEL);
            d.fillRect(72, y, 2, 9, MN_RED);
        }
        txt_c(BR_SW / 2, y, 1, MN_INK, over_item(i));
    }
}

// ============================ SC_CLEAR (level cleared / win) =================
#define NC_ITEMS 1
static void draw_clear(void) {
    backdrop();
    bool last = (g.level >= brawler_level_count() - 1);
    if (last) {
        txt_c(BR_SW / 2 + 1, 18, 3, BR_GREY_FAR, g.lang ? "VICTORY" : "VITTORIA");   // soft offset
        txt_c(BR_SW / 2,     17, 3, MN_INK,      g.lang ? "VICTORY" : "VITTORIA");
        d.fillRect(BR_SW / 2 - 16, 38, 32, 2, MN_RED);                                // red accent
        txt_c(BR_SW / 2, 48, 1, MN_GREY, tx("Hai ripulito la citta.", "You cleared the city."));
    } else {
        txt_c(BR_SW / 2, 20, 2, MN_INK, tx("ZONA RIPULITA", "AREA CLEARED"));
        const LevelDef *nx = brawler_level(g.level + 1);
        if (nx) {
            char nb[40]; snprintf(nb, sizeof nb, "%s: %s", tx("Prossima", "Next"),
                                  g.lang ? nx->name_en : nx->name_it);
            txt_c(BR_SW / 2, 48, 1, MN_DIM, nb);
        }
    }
    char sc[24]; snprintf(sc, sizeof sc, "%s %ld", tx("Punti", "Score"), g.score);
    txt_c(BR_SW / 2, 66, 1, MN_GREY, sc);

    int y = 96;
    d.fillRoundRect(70, y - 2, BR_SW - 140, 13, 3, MN_SEL);
    d.fillRect(72, y, 2, 9, MN_RED);
    txt_c(BR_SW / 2, y, 1, MN_INK, last ? tx("Continua", "Continue") : tx("Avanti", "Continue"));
}

// ============================ SC_HELP =======================================
static void draw_help(void) {
    backdrop();
    txt_c(BR_SW / 2, 8, 2, MN_INK, tx("COMANDI", "CONTROLS"));
    d.drawFastHLine(14, 28, BR_SW - 28, BR_GREY_FAR);
    txt(14, 34, 1, MN_INK,  tx("E su  S giu  A sx  D dx", "E up  S dn  A lt  D rt"));
    txt(14, 46, 1, MN_INK,  tx("J         pugno", "J         punch"));
    txt(14, 58, 1, MN_INK,  tx("K         calcio", "K         kick"));
    txt(14, 70, 1, MN_INK,  tx("L / Spazio  salto", "L / Space   jump"));
    txt(14, 82, 1, MN_RED,  tx("J ripetuto  combo", "repeat J    combo"));   // accent: the juice line
    txt(14, 94, 1, MN_DIM,  tx("Esc       pausa", "Esc       pause"));
    txt_c(BR_SW / 2, BR_SH - 10, 1, MN_DIM, tx("Esc indietro", "Esc back"));
}

// ============================ SC_COOP (host / join) =========================
static void draw_coop(void) {
    backdrop();
    txt_c(BR_SW / 2, 12, 2, MN_INK, "CO-OP 2P");
    d.fillRect(BR_SW / 2 - 16, 34, 32, 2, MN_RED);
    txt_c(BR_SW / 2, 42, 1, MN_DIM, tx("Uno OSPITA, l'altro si UNISCE (ESP-NOW)", "One HOSTS, the other JOINS (ESP-NOW)"));
    const char *it_[2] = { "Ospita la partita", "Unisciti" };
    const char *en_[2] = { "Host the game", "Join a game" };
    for (int i = 0; i < 2; i++) {
        int y = 62 + i * 20;
        bool f = (i == g.sel);
        if (f) {
            d.fillRoundRect(30, y - 3, BR_SW - 60, 17, 3, MN_SEL);
            d.fillRect(32, y - 1, 3, 13, MN_RED);
        }
        txt_c(BR_SW / 2, y, 2, MN_INK, g.lang ? en_[i] : it_[i]);
    }
    txt_c(BR_SW / 2, BR_SH - 9, 1, MN_DIM, tx("SU/GIU  INVIO  Esc", "UP/DN  ENTER  Esc"));
}

// ============================ SC_LOBBY (pairing) ============================
static void draw_lobby(void) {
    backdrop();
    txt_c(BR_SW / 2, 20, 2, MN_INK, g.is_host ? tx("OSPITO...", "HOSTING...") : tx("CERCO...", "SEARCHING..."));
    d.fillRect(BR_SW / 2 - 20, 42, 40, 2, MN_RED);
    txt_c(BR_SW / 2, 54, 1, MN_DIM, g.is_host ? tx("Attendo il 2o giocatore", "Waiting for player 2")
                                              : tx("Cerco una partita vicina", "Looking for a nearby game"));
    int dn = (int)((br_now_ms() / 400) % 4);          // animated pulse dots
    char dots[5]; int k = 0;
    while (k < dn && k < 3) { dots[k] = '.'; k++; }
    dots[k] = 0;
    txt_c(BR_SW / 2, 70, 3, MN_INK, dots);
    txt_c(BR_SW / 2, BR_SH - 9, 1, MN_DIM, tx("Esc annulla", "Esc cancel"));
}

// ============================ public: menu_draw =============================
void menu_draw(void) {
    switch (g.screen) {
        case SC_MENU:  draw_title();   break;
        case SC_SEL:   draw_select();  break;
        case SC_COOP:  draw_coop();    break;
        case SC_LOBBY: draw_lobby();   break;
        case SC_OPT:   draw_options(); break;
        case SC_PAUSE: draw_pause();   break;
        case SC_OVER:  draw_over();    break;
        case SC_CLEAR: draw_clear();   break;
        case SC_HELP:  draw_help();    break;
        default: break;
    }
}

// ============================ public: hud_draw =============================
// Compact top overlay over the 240x135 action: per-hero segmented health bar + name + lives pips,
// score (right), current level name (center), and a popping red COMBO xN when combo > 1.
static void hero_hud(int slot, int x, int w) {
    Fighter *fr = br_hero(slot);
    if (!fr) return;
    const HeroDef *h = brawler_hero(fr->kind);
    // name — dark on the white field
    txt(x, 1, 1, MN_INK, h ? h->name : "P");
    // segmented health bar (8 cells): INK body, RED when low; a thin grey outline reads on paper.
    int cells = 8;
    int bx = x, by = 10, bw = w, cellw = (bw - (cells - 1)) / cells;
    int filled = fr->maxhp > 0 ? (fr->hp * cells + fr->maxhp - 1) / fr->maxhp : 0;
    if (filled < 0) filled = 0;
    if (filled > cells) filled = cells;
    d.drawRect(bx - 1, by - 1, bw + 2, 6, BR_GREY_MID);   // casing outline for contrast on white
    for (int c = 0; c < cells; c++) {
        int cx = bx + c * (cellw + 1);
        uint16_t col = (c < filled) ? (c < 2 ? MN_RED : MN_INK) : BR_GREY_FAR;
        d.fillRect(cx, by, cellw, 4, col);
    }
    // lives pips (small red dots) to the right of the bar
    for (int p = 0; p < g.lives && p < 5; p++)
        d.fillCircle(bx + bw + 4 + p * 5, by + 2, 1, MN_RED);
}
void hud_draw(void) {
    // P1 bar top-left; in co-op a P2 bar mirrors top-right
    hero_hud(0, 4, 92);
    if (g.nplayers > 1) {
        // right-aligned: draw name/bar from the right inset
        Fighter *fr = br_hero(1);
        if (fr) {
            const HeroDef *h = brawler_hero(fr->kind);
            int w = 92, x = BR_SW - 4 - w;
            txt_r(BR_SW - 4, 1, 1, MN_INK, h ? h->name : "P2");
            int cells = 8, cellw = (w - (cells - 1)) / cells;
            int filled = fr->maxhp > 0 ? (fr->hp * cells + fr->maxhp - 1) / fr->maxhp : 0;
            if (filled < 0) filled = 0;
            if (filled > cells) filled = cells;
            d.drawRect(x - 1, 9, w + 2, 6, BR_GREY_MID);   // casing outline on white
            for (int c = 0; c < cells; c++) {
                int cx = x + c * (cellw + 1);
                uint16_t col = (c < filled) ? (c < 2 ? MN_RED : MN_INK) : BR_GREY_FAR;
                d.fillRect(cx, 10, cellw, 4, col);
            }
        }
    }

    // score: right side (single-player) / center strip (co-op) — INK on the white field
    char sb[20]; snprintf(sb, sizeof sb, "%ld", g.score);
    if (g.nplayers > 1) txt_c(BR_SW / 2, 1, 1, MN_INK, sb);
    else                txt_r(BR_SW - 4, 1, 1, MN_INK, sb);

    // current level name, centered just under the bars
    const LevelDef *lv = brawler_level(g.level);
    if (lv) txt_c(BR_SW / 2, (g.nplayers > 1) ? 10 : 1, 1, MN_DIM, g.lang ? lv->name_en : lv->name_it);

    // COMBO pop — only colour that flares mid-fight; size pulses while combo_t is fresh
    if (g.combo > 1) {
        char cb[18]; snprintf(cb, sizeof cb, "COMBO x%d", g.combo);
        int sz = (g.combo_t > 0.6f) ? 2 : 1;       // big right after a hit, then settle
        uint16_t col = ((br_now_ms() >> 6) & 1) ? MN_REDHI : MN_RED;
        txt_c(BR_SW / 2, 22, sz, col, cb);
    }
}

// ============================ public: menu_goto ============================
void menu_goto(BrScreen s) {
    // a "back" cue when retreating to a lighter screen, else a select cue
    bool back = (s == SC_MENU || s == SC_PAUSE);
    g.screen = s;
    g.sel = 0;
    if (s == SC_SEL) { s_pickslot = 0; }   // fresh select session (s_coop set by the caller via menu_key)
    bsfx(back ? BSFX_BACK : BSFX_SEL);
}

// ============================ public: menu_key ============================
// Navigate the current menu screen. Returns true if the key was consumed. The shell routes all
// keys here while g.screen != SC_PLAY; we never call framework fullscreen/exit (the shell owns that).
static void nav(int delta, int count) {
    g.sel = (g.sel + delta + count) % count;
    bsfx(BSFX_NAV);
}
bool menu_key(int key, char ch) {
    (void)ch;
    switch (g.screen) {
        // -------- title --------
        case SC_MENU:
            if (key == NK_UP)        { nav(-1, NM_ITEMS); return true; }
            if (key == NK_DOWN)      { nav(+1, NM_ITEMS); return true; }
            if (key == NK_ENTER) {
                switch (g.sel) {
                    case 0: s_coop = 0; menu_goto(SC_SEL); break;          // 1P
                    case 1: s_coop = 1; menu_goto(SC_COOP); break;         // co-op -> pick Host or Join first
                    case 2: menu_goto(SC_OPT);  break;
                    case 3: menu_goto(SC_HELP); break;
                    default: return false;                                  // Quit -> let the shell/back close the app
                }
                return true;
            }
            return false;

        // -------- character select --------
        case SC_SEL: {
            int n = brawler_hero_count(); if (n < 1) n = 1;
            if (key == NK_LEFT)  { g.sel = (g.sel + n - 1) % n; bsfx(BSFX_NAV); return true; }
            if (key == NK_RIGHT) { g.sel = (g.sel + 1) % n;     bsfx(BSFX_NAV); return true; }
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
            if (key == NK_UP)   { nav(-1, 2); return true; }
            if (key == NK_DOWN) { nav(+1, 2); return true; }
            if (key == NK_ENTER) {
                s_coop_host = (g.sel == 0) ? 1 : 0;   // item 0 = Host, item 1 = Join
                menu_goto(SC_SEL);                    // then pick YOUR fighter
                return true;
            }
            return false;

        // -------- co-op lobby (pairing over ESP-NOW) --------
        case SC_LOBBY:
            // The shell drives pairing (bnet_poll + bnet_available -> menu_coop_start); Esc cancels via
            // on_back. No per-key action here.
            return false;

        // -------- options --------
        case SC_OPT:
            if (key == NK_UP)    { nav(-1, NO_ITEMS); return true; }
            if (key == NK_DOWN)  { nav(+1, NO_ITEMS); return true; }
            if (key == NK_LEFT || key == NK_RIGHT || key == NK_ENTER) {
                int dir = (key == NK_LEFT) ? -1 : +1;
                switch (g.sel) {
                    case 0: g.audio = !g.audio; break;
                    case 1: g.lang ^= 1; break;
                    default: g.diff = (g.diff + (dir < 0 ? 2 : 1)) % 3; break;
                }
                bsfx(BSFX_SEL);
                return true;
            }
            return false;

        // -------- pause --------
        case SC_PAUSE:
            if (key == NK_UP)    { nav(-1, NP_ITEMS); return true; }
            if (key == NK_DOWN)  { nav(+1, NP_ITEMS); return true; }
            if (key == NK_ENTER) {
                if (g.sel == 0) { g.paused = false; g.screen = SC_PLAY; bsfx(BSFX_BACK); }
                else            { menu_goto(SC_MENU); }
                return true;
            }
            return false;

        // -------- game over --------
        case SC_OVER:
            if (key == NK_UP)    { nav(-1, NG_ITEMS); return true; }
            if (key == NK_DOWN)  { nav(+1, NG_ITEMS); return true; }
            if (key == NK_ENTER) {
                if (g.sel == 0) { s_coop = (g.nplayers > 1); start_match(); }   // retry, same roster
                else            { menu_goto(SC_MENU); }
                return true;
            }
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

        // -------- help --------
        case SC_HELP:
            return false;   // Esc handled by the shell's back handler -> SC_MENU

        default:
            return false;
    }
}
