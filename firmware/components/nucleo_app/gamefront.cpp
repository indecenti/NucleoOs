// GameFront — see gamefront.h. A HyperSpin-style game launcher drawn into the shared 8bpp
// back-buffer (240x135) and blitted in one push, like the launcher list and Control Center.
// Layout: an arcade MARQUEE (title banner) on top, a large RECTANGULAR cover below it, and a
// neighbour wheel (side arrows). No description in the carousel — press TAB for an info card with a
// one-line tagline and the supported play modes (1P, vs CPU, 2P local, LAN, co-op).
// Image decode happens only on a key press, never per-frame, so it stays cheap on this no-PSRAM board.
#include "gamefront.h"
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "launcher_render.h"
#include "nucleo_kbd.h"
#include "nucleo_ui.h"      // panel readback for Solo-mode / direct-draw screenshots
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
extern "C" {
#include "nucleo_board.h"
}

// The real display + the shared off-screen canvas (both owned by nucleo_ui.cpp).
extern M5GFX d;
M5Canvas *nucleo_screen(void);

// Registry read access (defined in nucleo_app.cpp; plain C++ linkage, like launcher_menu.cpp uses).
int                     nucleo_app_count(void);
const nucleo_app_def_t *nucleo_app_at(int i);

// ---- layout (landscape 240x135) --------------------------------------------
// Minimal EmulationStation-style detail view: a slim system bar (GIOCHI kicker + "N / M" position),
// a single large centred cover (wide landscape by default, tall portrait for pinball) with a soft
// neutral shadow + clean accent frame, quiet side chevrons with a faint neighbour name in the
// gutters, then the game TITLE on its own full-width readable line under the cover, and a compact
// row of play-mode badges. Everything breathes; the cover art is the focal point.
#define GF_MAX     24
#define MARQ_H     13                 // slim system bar across the top (kicker + position only)
#define HERO_W      128               // landscape (default) cover width -> a clean 16:9 frame
#define HERO_W_PORT 44                // portrait cover width (pinball): tall narrow "flipper table" shape
#define HERO_H      72                // shared by both shapes; title/badges now live BELOW the cover
#define HERO_Y      15
#define TITLE_Y    (HERO_Y + HERO_H + 2)   // big readable title band under the cover → y=89
#define TITLE_H     16
#define BADGE_Y    (TITLE_Y + TITLE_H + 1) // mode-badge row → y=106
#define FOOT       13                 // bottom footer → starts at y=122

#define GF_DIR   NUCLEO_SD_MOUNT "/data/GameShots"
#define SHOT_DIR NUCLEO_SD_MOUNT "/data/Screenshots"

// ---- per-game metadata: custom marquee title, play modes, one-line tagline, cover shape ---
enum { GM_1P = 0x01, GM_CPU = 0x02, GM_2P = 0x04, GM_LAN = 0x08, GM_COOP = 0x10 };
// Cover box shape: wide landscape (default, matches a real screenshot) or tall portrait — reserved
// for games whose real-world reference is naturally vertical (pinball's flipper table).
enum { GF_LANDSCAPE = 0, GF_PORTRAIT = 1 };

typedef struct { const char *id; const char *title; unsigned modes; const char *tag; unsigned char shape; } META_t;
static const META_t META[] = {
    { "reactor",  "REATTORE",       GM_1P,           "Bilancia potenza e raffreddamento. Evita il meltdown.", GF_LANDSCAPE },
    { "stelle",   "COSTELLAZIONI",  GM_1P,           "Sparatutto spaziale arcade fra le stelle.", GF_LANDSCAPE },
    { "giardino", "GIARDINO",       GM_1P,           "Sandbox di elementi che cadono. Relax puro.", GF_LANDSCAPE },
    { "slots",    "SLOT",           GM_1P,           "Tira la leva e insegui il jackpot.", GF_LANDSCAPE },
    { "poker",    "POKER",          GM_1P,           "5 carte: tieni le buone, punta alla scala reale.", GF_LANDSCAPE },
    { "pinball",  "FLIPPER",        GM_1P,           "Flipper verticale: respingenti, spinner, punti.", GF_PORTRAIT },
    { "pong",     "PONG",           GM_CPU | GM_LAN, "1v1 in rete fra due Cardputer, o contro la CPU.", GF_LANDSCAPE },
    { "tanks",    "TANKS",          GM_CPU | GM_2P,  "Artiglieria a turni: terreno distruttibile, vento, 9 armi.", GF_LANDSCAPE },
    { "brawler",  "SCORRIBANDA",    GM_1P | GM_COOP, "Picchiaduro noir a scorrimento, anche in co-op.", GF_LANDSCAPE },
    { "dice",     "DADI",           GM_1P,           "Tiro di dadi: scuoti il device o premi invio.", GF_LANDSCAPE },
    { "snake",    "SNAKE",          GM_CPU | GM_LAN, "Serpente 1v1 in rete (ESP-NOW), o contro l'IA.", GF_LANDSCAPE },
    { "tankd",    "TANK DUEL",      GM_CPU | GM_LAN, "Arena 1v1 in rete: shop conteso, 4 carri, upgrade.", GF_LANDSCAPE },
    { "yahtzee",  "YAHTZEE",        GM_2P | GM_CPU,  "Yahtzee a turni, 1-4 giocatori + CPU, dadi 3D.", GF_LANDSCAPE },
    { "orde",     "ORDE",           GM_1P,           "Mini vampire-survivors: piu' armi auto-sparano, sopravvivi alle orde.", GF_LANDSCAPE },
    { "cardler",  "CARDLER",        GM_1P,           "Mini RPG top-down: esplora il mondo, parla con gli NPC, raccogli oro.", GF_LANDSCAPE },
    { nullptr, nullptr, 0, nullptr, GF_LANDSCAPE },
};

// A game's cover box: landscape (wide, default) sized to fill a real screenshot, or portrait
// (narrow) for pinball's flipper table. Both shapes share the same Y/height band so the
// header/badge/footer chrome never shifts depending on which game is focused.
typedef struct { int x, y, w, h; } gf_box_t;
static int gf_hero_w(unsigned shape) { return shape == GF_PORTRAIT ? HERO_W_PORT : HERO_W; }
static gf_box_t gf_hero_box(unsigned shape)
{
    gf_box_t b; b.w = gf_hero_w(shape); b.h = HERO_H; b.x = (W - b.w) / 2; b.y = HERO_Y;
    return b;
}

// ---- state ------------------------------------------------------------------
static int  s_games[GF_MAX];     // indices into the app registry (category == "Games")
static int  s_ngames = 0;
static int  s_sel = 0;           // selection WITHIN the filtered set
static char s_filter[16] = "";   // type-to-filter, like the main launcher
static bool s_info = false;      // TAB info card overlay open?
static char s_tag[256];          // current tagline, loaded on selection change
static int  s_tag_for = -1;      // registry index the tagline belongs to

// ---- small helpers ----------------------------------------------------------
static uint16_t mix565(uint16_t a, uint16_t b, float t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (int)((br - ar) * t + 0.5f), g = ag + (int)((bg - ag) * t + 0.5f), bl = ab + (int)((bb - ab) * t + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static bool file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) { fclose(f); return true; } return false; }

// Background gradient colour at row y. MINIMAL: only a faint accent haze at the very top that settles
// quickly into the flat dark BG — the cover/poster is the focal colour, not the backdrop.
static uint16_t bg_at(uint16_t acc, int y)
{
    uint16_t top = mix565(acc, INK, 0.74f);            // deep, desaturated accent tint (was 0.5 = loud)
    float t = (float)y / H; if (t > 0.55f) t = 1.0f; else t /= 0.55f;   // reach flat BG by ~55% height
    return mix565(top, BG, t);
}

// Paint faux-rounded corner cutouts on a just-drawn rectangular cover, blending into the bg.
template <typename T> static void round_corners(T *c, int x, int y, int w, int h, int r, uint16_t acc)
{
    for (int yy = 0; yy < r; yy++)
        for (int xx = 0; xx < r; xx++) {
            if ((r - xx) * (r - xx) + (r - yy) * (r - yy) <= r * r) continue;
            uint16_t ct = bg_at(acc, y + yy), cb = bg_at(acc, y + h - 1 - yy);
            c->drawPixel(x + xx, y + yy, ct);                 // TL
            c->drawPixel(x + w - 1 - xx, y + yy, ct);         // TR
            c->drawPixel(x + xx, y + h - 1 - yy, cb);         // BL
            c->drawPixel(x + w - 1 - xx, y + h - 1 - yy, cb); // BR
        }
}

static const META_t *gf_meta(const char *id)
{
    if (id) for (int i = 0; META[i].id; i++) if (!strcmp(META[i].id, id)) return &META[i];
    return nullptr;
}

// Case-insensitive substring (filter match), both args lowercased into small buffers.
static bool name_matches(const char *name, const char *filt)
{
    if (!filt[0]) return true;
    char a[40], b[20];
    int i = 0; for (; name[i] && i < 39; i++) a[i] = (char)tolower((unsigned char)name[i]); a[i] = 0;
    int j = 0; for (; filt[j] && j < 19; j++) b[j] = (char)tolower((unsigned char)filt[j]); b[j] = 0;
    return strstr(a, b) != nullptr;
}

// Filtered-view queries: s_sel indexes the matching subset (mirrors the main launcher's filter model).
static int gf_count(void)
{
    int n = 0;
    for (int i = 0; i < s_ngames; i++) if (name_matches(nucleo_app_at(s_games[i])->name, s_filter)) n++;
    return n;
}
static int gf_at(int sel)   // registry index of the sel-th match, or -1
{
    int seen = 0;
    for (int i = 0; i < s_ngames; i++)
        if (name_matches(nucleo_app_at(s_games[i])->name, s_filter)) { if (seen == sel) return s_games[i]; seen++; }
    return -1;
}

// Custom marquee title for a game: META override, else the registry name.
static const char *gf_title(const nucleo_app_def_t *g)
{
    const META_t *m = gf_meta(g->id);
    return (m && m->title) ? m->title : g->name;
}

// Greedy word-wrap under the CURRENT font. Draws lines [scroll, scroll+maxLines) of `s` into a
// `w`-px column at (x,y), `lineH` apart; returns the total line count. maxLines == 0 -> count only.
template <typename T> static int draw_wrapped(T *c, const char *s, int x, int y, int w, int lineH, int maxLines, int scroll)
{
    int line = 0, drawn = 0;
    const char *p = s ? s : "";
    while (*p) {
        char buf[64]; int len = 0; buf[0] = 0;
        while (*p) {
            if (*p == '\n') { p++; break; }
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            int wl = (int)(p - ws);
            char tmp[64]; int tl = len;
            if (tl && tl < (int)sizeof tmp - 1) tmp[tl++] = ' ';
            for (int i = 0; i < wl && tl < (int)sizeof tmp - 1; i++) tmp[tl++] = ws[i];
            tmp[tl] = 0;
            if (len && (int)c->textWidth(tmp) > w) { p = ws; break; }
            memcpy(buf, tmp, tl + 1); len = tl;
            while (*p == ' ') p++;
        }
        if (line >= scroll && drawn < maxLines) { c->setCursor(x, y + drawn * lineH); c->print(buf); drawn++; }
        line++;
    }
    return line;
}

// Tagline for the info card: /data/GameShots/<id>.txt override -> META tag -> registry desc.
static void load_tag(const nucleo_app_def_t *g, int regidx)
{
    s_tag[0] = 0;
    char p[192]; snprintf(p, sizeof p, "%s/%s.txt", GF_DIR, g->id);
    FILE *f = fopen(p, "rb");
    if (f) { size_t n = fread(s_tag, 1, sizeof s_tag - 1, f); s_tag[n] = 0; fclose(f); }
    if (!s_tag[0]) { const META_t *m = gf_meta(g->id); if (m && m->tag) snprintf(s_tag, sizeof s_tag, "%s", m->tag); }
    if (!s_tag[0]) snprintf(s_tag, sizeof s_tag, "%s", g->desc ? g->desc : "");
    s_tag_for = regidx;
}

// ---- play-mode lookup (label + accent colour, in display order) -------------
static const unsigned MODE_ORDER[] = { GM_1P, GM_CPU, GM_2P, GM_LAN, GM_COOP };
static uint16_t mode_col(unsigned f)
{
    switch (f) { case GM_1P: return C_GREEN; case GM_CPU: return C_YELLOW; case GM_2P: return C_BLUE;
                 case GM_LAN: return C_PINK; case GM_COOP: return C_PURPLE; } return C_GREY;
}
static const char *mode_lbl(unsigned f)
{
    switch (f) { case GM_1P: return "1 Giocatore"; case GM_CPU: return "vs CPU"; case GM_2P: return "2P locale";
                 case GM_LAN: return "Online / LAN"; case GM_COOP: return "Co-op"; } return "?";
}

// A tiny pictogram for one play mode, centred on (cx,cy) with the figure standing on `feet`.
template <typename T> static void draw_person(T *c, int cx, int feet, uint16_t col)
{
    c->fillCircle(cx, feet - 7, 2, col);                          // head
    c->fillTriangle(cx - 3, feet, cx + 3, feet, cx, feet - 4, col); // body
}
template <typename T> static void draw_pict(T *c, unsigned kind, int cx, int cy, int feet, uint16_t col)
{
    switch (kind) {
        case GM_1P: draw_person(c, cx, feet, col); break;
        case GM_2P: draw_person(c, cx - 4, feet, col); draw_person(c, cx + 4, feet, col); break;
        case GM_COOP:
            draw_person(c, cx - 4, feet, col); draw_person(c, cx + 4, feet, col);
            c->drawFastHLine(cx - 1, feet - 11, 3, col); c->drawFastVLine(cx, feet - 12, 3, col); break;  // link "+"
        case GM_CPU:
            c->fillRoundRect(cx - 4, cy - 4, 8, 8, 1, col);
            c->fillRect(cx - 2, cy - 2, 4, 4, mix565(col, INK, 0.55f));
            for (int i = -2; i <= 2; i += 2) {
                c->drawFastVLine(cx + i, cy - 6, 2, col); c->drawFastVLine(cx + i, cy + 5, 2, col);
                c->drawFastHLine(cx - 6, cy + i, 2, col); c->drawFastHLine(cx + 5, cy + i, 2, col);
            } break;
        case GM_LAN:                                              // globe (online)
            c->drawCircle(cx, cy, 5, col); c->drawFastHLine(cx - 5, cy, 11, col);
            c->drawEllipse(cx, cy, 2, 5, col); break;
    }
}

// One icon-only mode badge (carousel row): a rounded chip with the pictogram.
template <typename T> static void draw_badge(T *c, int x, int y, unsigned kind)
{
    const int BW = 20, BH = 14;
    uint16_t col = mode_col(kind);
    c->fillRoundRect(x + 1, y + 2, BW, BH, 3, mix565(BG, INK, 0.9f));
    c->fillRoundRect(x, y, BW, BH, 3, mix565(col, INK, 0.58f));
    c->drawFastHLine(x + 3, y + 1, BW - 6, mix565(col, FG, 0.4f));
    c->drawRoundRect(x, y, BW, BH, 3, col);
    draw_pict(c, kind, x + BW / 2, y + BH / 2, y + BH - 2, mix565(col, FG, 0.9f));
}
// Wide labeled badge for the single-mode case: icon + full text label.
template <typename T> static void draw_badge_labeled(T *c, unsigned kind)
{
    uint16_t col = mode_col(kind);
    const char *lbl = mode_lbl(kind);
    c->setFont(&fonts::Font2); c->setTextSize(1);
    int tw = (int)c->textWidth(lbl);
    const int BH = 14, PAD = 6;
    int BW = 20 + tw + PAD;
    int bx = (W - BW) / 2;
    c->fillRoundRect(bx + 1, BADGE_Y + 2, BW, BH, 3, mix565(BG, INK, 0.9f));
    c->fillRoundRect(bx, BADGE_Y, BW, BH, 3, mix565(col, INK, 0.55f));
    c->drawFastHLine(bx + 3, BADGE_Y + 1, BW - 6, mix565(col, FG, 0.4f));
    c->drawRoundRect(bx, BADGE_Y, BW, BH, 3, col);
    draw_pict(c, kind, bx + 11, BADGE_Y + BH / 2, BADGE_Y + BH - 2, mix565(col, FG, 0.9f));
    c->setTextColor(mix565(col, FG, 0.92f)); c->setCursor(bx + 20, BADGE_Y + 1); c->print(lbl);
}
template <typename T> static void draw_badges(T *c, unsigned modes)
{
    const int BW = 20, GAP = 5;
    int n = 0; for (unsigned i = 0; i < sizeof MODE_ORDER / sizeof MODE_ORDER[0]; i++) if (modes & MODE_ORDER[i]) n++;
    if (n <= 0) return;
    if (n == 1) {
        for (unsigned i = 0; i < sizeof MODE_ORDER / sizeof MODE_ORDER[0]; i++)
            if (modes & MODE_ORDER[i]) { draw_badge_labeled(c, MODE_ORDER[i]); return; }
    }
    int total = n * BW + (n - 1) * GAP, x = (W - total) / 2;
    for (unsigned i = 0; i < sizeof MODE_ORDER / sizeof MODE_ORDER[0]; i++)
        if (modes & MODE_ORDER[i]) { draw_badge(c, x, BADGE_Y, MODE_ORDER[i]); x += BW + GAP; }
}

// ---- the cover (real screenshot) or a procedural poster ---------------------
// Branded "key-art" poster shown when a game has no captured/curated cover yet. Designed to look
// like an intentional cover (accent gradient + big glyph + title), NOT an empty "press to capture"
// placeholder — so the carousel stays polished even before any screenshot exists.
template <typename T> static void draw_poster(T *c, const nucleo_app_def_t *g, const gf_box_t &box)
{
    uint16_t acc = g->color;
    // Vertical accent gradient body, deepening toward the bottom (poster feel).
    for (int yy = 0; yy < box.h; yy++) {
        float t = (float)yy / box.h;
        c->drawFastHLine(box.x, box.y + yy, box.w, mix565(acc, INK, 0.32f + 0.50f * t));
    }
    // Soft top sheen for depth.
    int sh = box.h / 3;
    for (int yy = 0; yy < sh; yy++) {
        float a = 0.12f * (1.0f - (float)yy / sh);
        if (a > 0.01f) c->drawFastHLine(box.x, box.y + yy, box.w, mix565(mix565(acc, INK, 0.32f), FG, a));
    }
    int cx = box.x + box.w / 2;
    // Big game glyph, upper-center — radius clamped to the box width so a narrow portrait
    // cover (pinball) doesn't blow past its own frame.
    int iconR = (box.w - 16) / 2; if (iconR > 34) iconR = 34; if (iconR < 12) iconR = 12;
    launcher_draw_icon(c, cx, box.y + iconR + 4, iconR, g->id, g->icon,
                       mix565(acc, FG, 0.88f), mix565(acc, INK, 0.30f));
    // Game title along the bottom, with a drop shadow for legibility over the gradient.
    const char *t = gf_title(g);
    c->setFont(&fonts::Font2);
    int tw = (int)c->textWidth(t);
    if (tw > box.w - 10) { c->setFont(&fonts::Font0); c->setTextSize(1); tw = (int)c->textWidth(t); }
    int tx = cx - tw / 2, ty = box.y + box.h - c->fontHeight() - 5;
    c->setTextColor(mix565(acc, INK, 0.45f)); c->setCursor(tx + 1, ty + 1); c->print(t);   // shadow
    c->setTextColor(FG);                      c->setCursor(tx,     ty);     c->print(t);   // face
}

template <typename T> static void draw_hero(T *c, const nucleo_app_def_t *g, const gf_box_t &box)
{
    uint16_t acc = g->color;

    // Soft NEUTRAL drop-shadow under the cover (EmulationStation-style). No coloured glow halo — that
    // was the source of the stray accent/pink horizontal lines around the box.
    c->fillRoundRect(box.x + 2, box.y + 4, box.w, box.h, 6, mix565(BG, INK, 0.82f));

    c->setClipRect(box.x, box.y, box.w, box.h);
    char p[192]; bool drawn = false;
    snprintf(p, sizeof p, "%s/%s.png", GF_DIR, g->id);
    if (file_exists(p)) { c->drawPngFile(p, box.x + box.w / 2, box.y + box.h / 2, box.w, box.h, 0, 0, 0.0f, 0.0f, datum_t::middle_center); drawn = true; }
    if (!drawn) { snprintf(p, sizeof p, "%s/%s.jpg", GF_DIR, g->id);
        if (file_exists(p)) { c->drawJpgFile(p, box.x + box.w / 2, box.y + box.h / 2, box.w, box.h, 0, 0, 0.0f, 0.0f, datum_t::middle_center); drawn = true; } }
    if (!drawn) { snprintf(p, sizeof p, "%s/%s.bmp", GF_DIR, g->id);
        // Fit the BMP into the current hero box (centre, aspect-preserved). Fitting instead of a raw
        // 1:1 blit keeps covers captured at an older box size sitting cleanly inside the new frame.
        if (file_exists(p)) { c->drawBmpFile(p, box.x + box.w / 2, box.y + box.h / 2, box.w, box.h, 0, 0, 0.0f, 0.0f, datum_t::middle_center); drawn = true; } }
    if (!drawn) draw_poster(c, g, box);
    // A glossy sheen across the top third: lighten the real artwork pixels, fading out.
    // Read each row back (RGB565), brighten toward FG, write it again — proven readRect path.
    // Sized to the landscape (max) width; portrait boxes just use a prefix of it.
    static uint16_t row[HERO_W];
    int sh = box.h / 3;
    for (int yy = 0; yy < sh; yy++) {
        float a = 0.16f * (1.0f - (float)yy / sh);
        if (a <= 0.01f) continue;
        int y = box.y + yy;
        c->readRect(box.x, y, box.w, 1, row);
        for (int xx = 0; xx < box.w; xx++) row[xx] = mix565(row[xx], FG, a);
        c->pushImage(box.x, y, box.w, 1, row);
    }
    c->clearClipRect();

    round_corners(c, box.x, box.y, box.w, box.h, 5, acc);

    // Clean frame: a thin dark inset + a single MUTED accent edge. No bright/pink highlight line.
    c->drawRoundRect(box.x - 1, box.y - 1, box.w + 2, box.h + 2, 5, mix565(INK, BG, 0.35f));
    c->drawRoundRect(box.x - 2, box.y - 2, box.w + 4, box.h + 4, 6, mix565(acc, INK, 0.42f));
}

// ---- quiet side indicator: a chevron + a faint neighbour name in the gutter --
// No framed card, no gradient box — just a soft directional cue so the eye stays on the cover.
// Centred vertically on the cover, centred horizontally in the leftover gutter.
template <typename T> static void draw_side(T *c, const nucleo_app_def_t *g, bool left, int gutter)
{
    uint16_t acc = g->color;
    int cy = HERO_Y + HERO_H / 2;
    int cx = left ? gutter / 2 : W - (gutter + 1) / 2;

    // Chevron pointing outward (toward the neighbour).
    uint16_t ch = mix565(acc, FG, 0.55f);
    if (left) c->fillTriangle(cx + 3, cy - 7, cx + 3, cy + 7, cx - 5, cy, ch);
    else      c->fillTriangle(cx - 3, cy - 7, cx - 3, cy + 7, cx + 5, cy, ch);

    // Faint neighbour name (first word, up to 6 chars) tucked under the chevron.
    c->setFont(&fonts::Font0); c->setTextSize(1);
    const char *title = gf_title(g);
    char tb[7] = {0}; int tl = 0;
    for (int i = 0; title[i] && tl < 6 && title[i] != ' '; i++) tb[tl++] = title[i];
    int tw = tl * 6; if (tw > gutter - 2) return;     // skip label if the gutter is too tight
    c->setTextColor(mix565(acc, FG, 0.34f));
    c->setCursor(cx - tw / 2, cy + 14); c->print(tb);
}

// ---- slim system bar: kicker + position chip (the title now lives under the cover) ----------
template <typename T> static void draw_topbar(T *c, uint16_t acc, int sel, int cnt)
{
    for (int yy = 0; yy < MARQ_H; yy++)                          // sober band, matches the minimal bg
        c->drawFastHLine(0, yy, W, mix565(acc, INK, 0.72f - 0.10f * (float)yy / MARQ_H));
    c->drawFastHLine(0, 0, W, mix565(acc, FG, 0.20f));          // faint top glass highlight
    c->drawFastHLine(0, MARQ_H - 1, W, mix565(acc, FG, 0.38f));  // one clean accent baseline

    int ty = (MARQ_H - 8) / 2;                                  // vertical-centre the 8px Font0 glyphs
    c->setFont(&fonts::Font0); c->setTextSize(1);
    c->setTextColor(mix565(acc, FG, 0.58f)); c->setCursor(5, ty); c->print("GIOCHI");   // kicker

    if (s_filter[0]) {                                          // filter takes the right slot when active
        char fb[20]; snprintf(fb, sizeof fb, "/%.12s", s_filter);
        int fw = (int)strlen(fb) * 6;
        c->setTextColor(C_GREEN); c->setCursor(W - fw - 5, ty); c->print(fb);
    } else if (cnt > 0) {
        char kn[12]; snprintf(kn, sizeof kn, "%d / %d", sel + 1, cnt);
        int kw = (int)strlen(kn) * 6;
        c->setTextColor(mix565(acc, FG, 0.72f)); c->setCursor(W - kw - 5, ty); c->print(kn);
    }
}

// ---- game title, on its own full-width readable line under the cover ------------------------
template <typename T> static void draw_title(T *c, const nucleo_app_def_t *g, uint16_t acc)
{
    const char *t = gf_title(g);
    c->setFont(&fonts::Font2); c->setTextSize(1);               // 16px, clean and legible
    if ((int)c->textWidth(t) > W - 24) { c->setFont(&fonts::Font0); c->setTextSize(1); }  // safety only
    int tw = (int)c->textWidth(t), tx = (W - tw) / 2;
    int ty = TITLE_Y + (TITLE_H - c->fontHeight()) / 2;
    c->setTextColor(mix565(acc, INK, 0.35f)); c->setCursor(tx + 1, ty + 1); c->print(t);   // soft shadow
    c->setTextColor(FG);                      c->setCursor(tx,     ty);     c->print(t);   // face
}

template <typename T> static void draw_footer(T *c, const char *hint)
{
    c->fillRect(0, H - FOOT, W, FOOT, INK);
    c->drawFastHLine(0, H - FOOT, W, LINE);
    int hx = (W - (int)strlen(hint) * 6) / 2; if (hx < 2) hx = 2;
    c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(MUTED, INK);
    c->setCursor(hx, H - FOOT + 3); c->print(hint);
}

// ---- minimal background: a single clean accent-to-dark vertical gradient. No diagonal sheen bands, no
// heavy vignette — the cover art is the focus, the backdrop just recedes. A whisper-thin edge fade keeps
// the frame from feeling cut off without the old cinematic noise.
template <typename T> static void draw_bg(T *c, uint16_t acc)
{
    for (int y = 0; y < H; y++) c->drawFastHLine(0, y, W, bg_at(acc, y));

    for (int x = 0; x < 10; x++) {                     // very light edge fade (was a 24px 0.22 vignette)
        float v = 0.10f * (1.0f - (float)x / 10);
        for (int y = MARQ_H; y < H - FOOT; y++) {
            c->drawPixel(x, y, mix565(bg_at(acc, y), INK, v));
            c->drawPixel(W - 1 - x, y, mix565(bg_at(acc, y), INK, v));
        }
    }
}

// ---- compose the whole carousel screen --------------------------------------
template <typename T> static void draw_gf(T *c)
{
    int cnt = gf_count();
    if (s_sel >= cnt) s_sel = cnt > 0 ? cnt - 1 : 0;

    if (cnt <= 0) {
        draw_bg(c, C_RED);
        for (int yy = 0; yy < MARQ_H; yy++) c->drawFastHLine(0, yy, W, mix565(C_RED, INK, 0.6f));
        c->drawFastHLine(0, MARQ_H - 1, W, mix565(C_RED, FG, 0.45f));
        c->setFont(&fonts::Font0); c->setTextColor(mix565(C_RED, FG, 0.58f));
        c->setCursor(5, (MARQ_H - 8) / 2); c->print("GIOCHI");
        c->setFont(&fonts::Font2); c->setTextColor(DIM);
        const char *m = s_filter[0] ? "Nessun risultato" : "Nessun gioco";
        c->setCursor((W - (int)c->textWidth(m)) / 2, H / 2 - 8); c->print(m);
        draw_footer(c, s_filter[0] ? "DEL cancella filtro   ESC" : "ESC esci");
        return;
    }

    int cur = gf_at(s_sel);
    const nucleo_app_def_t *g = nucleo_app_at(cur);
    if (cur != s_tag_for) load_tag(g, cur);
    const META_t *m = gf_meta(g->id);
    unsigned modes = m ? m->modes : (unsigned)GM_1P;
    unsigned shape = m ? m->shape : (unsigned)GF_LANDSCAPE;
    gf_box_t box = gf_hero_box(shape);

    draw_bg(c, g->color);

    if (cnt > 1) {                                               // quiet carousel neighbour cues
        draw_side(c, nucleo_app_at(gf_at((s_sel - 1 + cnt) % cnt)), true, box.x);
        draw_side(c, nucleo_app_at(gf_at((s_sel + 1) % cnt)), false, box.x);
    }
    draw_hero(c, g, box);
    draw_topbar(c, g->color, s_sel, cnt);
    draw_title(c, g, g->color);
    draw_badges(c, modes);

    draw_footer(c, s_filter[0] ? "DEL canc   < > scegli   INVIO   ESC"
                               : "TAB info   < > gioco   INVIO   ESC");
}

// ---- TAB info card: tagline + supported play modes --------------------------
template <typename T> static void draw_info(T *c, const nucleo_app_def_t *g)
{
    const META_t *m = gf_meta(g->id);
    unsigned modes = m ? m->modes : (unsigned)GM_1P;
    uint16_t acc = g->color;

    // Accent-tinted dim wash behind, then a framed panel.
    for (int y = 0; y < H; y++) c->drawFastHLine(0, y, W, mix565(BG, mix565(acc, INK, 0.4f), 0.4f));
    c->fillRoundRect(8, 5, W - 16, H - FOOT - 7, 6, INK);
    c->drawRoundRect(8, 5, W - 16, H - FOOT - 7, 6, mix565(acc, LINE, 0.4f));
    c->drawFastHLine(10, 6, W - 20, mix565(acc, FG, 0.4f));      // top highlight

    // Title + accent rule.
    c->setFont(&fonts::Font4);
    const char *t = gf_title(g);
    if ((int)c->textWidth(t) > W - 30) c->setFont(&fonts::Font2);
    c->setTextColor(acc, INK); c->setCursor(15, 9); c->print(t);
    int uy = 9 + c->fontHeight() + 1;
    c->drawFastHLine(15, uy, W - 30, mix565(acc, LINE, 0.5f));

    // Tagline (wrapped, up to 2 lines).
    int ty = uy + 5;
    c->setFont(&fonts::Font2); c->setTextSize(1); c->setTextColor(0xCE79, INK);
    draw_wrapped(c, s_tag, 15, ty, W - 28, 15, 2, 0);

    // Labelled mode pills (pictogram + text), flow-wrapped.
    int px = 15, py = ty + 2 * 15 + 3;
    c->setFont(&fonts::Font2);
    for (unsigned i = 0; i < sizeof MODE_ORDER / sizeof MODE_ORDER[0]; i++) {
        unsigned f = MODE_ORDER[i]; if (!(modes & f)) continue;
        const char *lbl = mode_lbl(f); uint16_t col = mode_col(f);
        int pw = 18 + (int)c->textWidth(lbl) + 8;
        if (px + pw > W - 14) { px = 15; py += 19; }
        c->fillRoundRect(px, py, pw, 16, 4, mix565(col, INK, 0.58f));
        c->drawRoundRect(px, py, pw, 16, 4, col);
        draw_pict(c, f, px + 10, py + 8, py + 13, mix565(col, FG, 0.9f));
        c->setTextColor(FG); c->setCursor(px + 19, py + 1); c->print(lbl);
        px += pw + 5;
    }

    draw_footer(c, "TAB/ESC chiudi   < > gioco   INVIO gioca");
}

// ---- public API -------------------------------------------------------------
void gamefront_open(void)
{
    s_ngames = 0;
    int n = nucleo_app_count();
    for (int i = 0; i < n && s_ngames < GF_MAX; i++) {
        const nucleo_app_def_t *a = nucleo_app_at(i);
        if (a->category && !strcmp(a->category, "Games")) s_games[s_ngames++] = i;
    }
    s_filter[0] = 0;                                // open fresh, no filter
    s_info = false;
    int cnt = gf_count();
    if (s_sel >= cnt) s_sel = cnt > 0 ? cnt - 1 : 0;
    if (s_sel < 0) s_sel = 0;
    s_tag_for = -1;                                // force reload on next render
}

int gamefront_key(int key, char ch, char *launch_id, int cap)
{
    int cnt = gf_count();
    if (s_info) {                                  // info card owns input while open
        switch (key) {
            case NK_TAB:
            case NK_BACK:  s_info = false; return GF_REDRAW;
            case NK_LEFT:  if (cnt > 0) s_sel = (s_sel - 1 + cnt) % cnt; return GF_REDRAW;
            case NK_RIGHT: if (cnt > 0) s_sel = (s_sel + 1) % cnt;       return GF_REDRAW;
            case NK_ENTER: {
                int cur = gf_at(s_sel); if (cur < 0) return GF_NONE;
                snprintf(launch_id, cap, "%s", nucleo_app_at(cur)->id); return GF_LAUNCH;
            }
            default: return GF_NONE;
        }
    }
    switch (key) {
        case NK_LEFT:  if (cnt > 0) s_sel = (s_sel - 1 + cnt) % cnt; return GF_REDRAW;
        case NK_RIGHT: if (cnt > 0) s_sel = (s_sel + 1) % cnt;       return GF_REDRAW;
        case NK_TAB:   if (cnt > 0) s_info = true; return GF_REDRAW;
        case NK_ENTER: {
            int cur = gf_at(s_sel);
            if (cur < 0) return GF_NONE;
            snprintf(launch_id, cap, "%s", nucleo_app_at(cur)->id);
            return GF_LAUNCH;
        }
        case NK_BACK:                                  // clear the filter first, then close (like the launcher)
            if (s_filter[0]) { s_filter[0] = 0; s_sel = 0; return GF_REDRAW; }
            return GF_CLOSE;
        case NK_DEL: {
            int l = (int)strlen(s_filter);
            if (l) { s_filter[l - 1] = 0; s_sel = 0; }
            return GF_REDRAW;
        }
        case NK_CHAR:
            if (ch > ' ') {
                int l = (int)strlen(s_filter);
                if (l < (int)sizeof s_filter - 1) { s_filter[l] = (char)tolower((unsigned char)ch); s_filter[l + 1] = 0; s_sel = 0; }
                return GF_REDRAW;
            }
            return GF_NONE;
        default: return GF_NONE;
    }
}

bool gamefront_step(void) { return false; }   // instant snap (no per-frame decode)

void gamefront_render(void)
{
    // Prefer the off-screen canvas (flicker-free, one blit). When the heap can't spare the 32 KB
    // canvas (fragmentation), fall back to drawing STRAIGHT to the display — same as the launcher's
    // list band — so the front-end always renders instead of a blank "Giochi" screen.
    M5Canvas *c = nucleo_screen();
    if (c) {
        draw_gf(c);
        if (s_info) { int cur = gf_at(s_sel); if (cur >= 0) draw_info(c, nucleo_app_at(cur)); }
        c->pushSprite(0, 0);
    } else {
        d.startWrite();
        draw_gf(&d);
        if (s_info) { int cur = gf_at(s_sel); if (cur >= 0) draw_info(&d, nucleo_app_at(cur)); }
        d.endWrite();
    }
}

// ---- on-device capture (24-bit BMP, streamed row-by-row; no big alloc) -------
// Recursively create every component of a directory path (FATFS mkdir only makes the last level;
// if /sd/data didn't exist, a single mkdir("/sd/data/GameShots") fails and the cover never gets
// written — which is exactly why captured covers weren't showing up).
static void ensure_dir(const char *path)
{
    char t[192]; size_t n = strlen(path); if (n == 0 || n >= sizeof t) return;
    memcpy(t, path, n + 1);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
    mkdir(t, 0777);
}

static void put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }

// Save a 24-bit BMP of dstW x dstH, sampled from the source crop rect (sx0,sy0,cw,ch). The crop lets
// the cover preserve the game's aspect (centre-crop) instead of squashing the whole frame.
static bool save_bmp(LovyanGFX *src, const char *path, int dstW, int dstH,
                     int sx0, int sy0, int cw, int ch)
{
    int sw = src->width(), sh = src->height();
    if (sw <= 0 || sh <= 0 || dstW <= 0 || dstH <= 0 || dstW > 320) return false;
    if (cw <= 0 || ch <= 0) { sx0 = 0; sy0 = 0; cw = sw; ch = sh; }   // no crop -> full frame
    FILE *f = fopen(path, "wb"); if (!f) return false;

    int rowSize = (dstW * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * dstH;
    uint8_t h[54]; memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M'; put32(h + 2, 54 + imgSize); put32(h + 10, 54);
    put32(h + 14, 40); put32(h + 18, dstW); put32(h + 22, dstH);
    put16(h + 26, 1); put16(h + 28, 24); put32(h + 38, 2835); put32(h + 42, 2835);
    fwrite(h, 1, 54, f);

    // Use rgb888_t so M5GFX handles hardware-display endianness and RGB/BGR conversion
    // internally. Reading as uint16_t from a hardware panel returns big-endian bytes
    // that get misinterpreted as little-endian RGB565, corrupting all colours.
    static lgfx::rgb888_t prow[320];
    uint8_t orow[320 * 3 + 4];
    for (int oy = 0; oy < dstH; oy++) {
        int sy = sy0 + oy * ch / dstH; if (sy >= sh) sy = sh - 1; if (sy < 0) sy = 0;
        src->readRect(0, sy, sw, 1, prow);
        memset(orow, 0, rowSize);
        for (int ox = 0; ox < dstW; ox++) {
            int sx = sx0 + ox * cw / dstW; if (sx >= sw) sx = sw - 1; if (sx < 0) sx = 0;
            orow[ox * 3 + 0] = prow[sx].b;   // BMP stores BGR
            orow[ox * 3 + 1] = prow[sx].g;
            orow[ox * 3 + 2] = prow[sx].r;
        }
        fseek(f, 54 + (long)(dstH - 1 - oy) * rowSize, SEEK_SET);   // BMP rows are bottom-up
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    return true;
}

bool gamefront_save_cover(const char *id)
{
    // Read from the shared CANVAS, not the panel. The ST7789 panel's readback is unreliable
    // (returns garbage / wrong byte order — greens came out purple), so the only trustworthy
    // source is the off-screen canvas in RAM, which holds the exact frame just blitted to the
    // LCD. It is 8bpp RGB332, so colours are quantised to what is actually shown on screen —
    // i.e. the capture matches what the eye sees. rgb888_t lets M5GFX expand 332->888 cleanly.
    M5Canvas *c = nucleo_screen(); if (!c || !id || !id[0]) return false;
    ensure_dir(GF_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", GF_DIR, id);
    int sw = c->width(), sh = c->height(), cw, ch;
    const META_t *m = gf_meta(id);
    int coverW = gf_hero_w(m ? m->shape : (unsigned)GF_LANDSCAPE), coverH = HERO_H;
    if ((long)coverW * sh > (long)coverH * sw) { cw = sw; ch = (int)((long)sw * coverH / coverW); }
    else                                       { ch = sh; cw = (int)((long)sh * coverW / coverH); }
    return save_bmp(c, p, coverW, coverH, (sw - cw) / 2, (sh - ch) / 2, cw, ch);
}

bool gamefront_save_screenshot(const char *name)
{
    M5Canvas *c = nucleo_screen(); if (!c) return false;
    ensure_dir(SHOT_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, name && name[0] ? name : "shot");
    return save_bmp(c, p, c->width(), c->height(), 0, 0, c->width(), c->height());
}

// Read the PHYSICAL PANEL (nucleo_ui_read_row) into a full-frame 24-bit BMP at `path`. Works with no
// network and no off-screen canvas (Solo boot, direct-draw apps) — a local SD write. Overwrites.
static bool panel_bmp_to(const char *path)
{
    int w = 0, h = 0; nucleo_ui_panel_size(&w, &h);
    if (w <= 0 || h <= 0 || w > 320) return false;
    FILE *f = fopen(path, "wb"); if (!f) return false;
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * h;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, w); put32(hd + 22, h);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);
    static uint16_t prow[320]; static uint8_t orow[320 * 3 + 4];
    for (int y = h - 1; y >= 0; y--) {                 // BMP rows bottom-up
        if (!nucleo_ui_read_row(y, w, prow)) memset(prow, 0, (size_t)w * 2);
        memset(orow, 0, rowSize);
        for (int x = 0; x < w; x++) {
            uint16_t raw = prow[x];
            uint16_t pp = (uint16_t)((raw >> 8) | (raw << 8));   // un-swap the panel's readback byte order
            orow[x * 3 + 0] = (uint8_t)(( pp        & 0x1F) * 255 / 31);   // BMP is BGR
            orow[x * 3 + 1] = (uint8_t)(((pp >> 5)  & 0x3F) * 255 / 63);
            orow[x * 3 + 2] = (uint8_t)(((pp >> 11) & 0x1F) * 255 / 31);
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    return true;
}

// Full-frame panel screenshot -> /data/Screenshots/<name>.bmp (direct-draw / Solo-boot apps).
bool gamefront_save_panel_screenshot(const char *name)
{
    ensure_dir(SHOT_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, name && name[0] ? name : "shot");
    return panel_bmp_to(p);
}

// Full-frame panel shot used AS the carousel cover -> /data/GameShots/<id>.bmp (overwrites; no history).
// The whole panel is FIT (letterboxed, aspect preserved) into the exact hero‑box size, so it drops in
// 1:1 and always sits perfectly inside the carousel frame — never cropped, never overflowing.
bool gamefront_save_panel_cover(const char *id)
{
    if (!id || !id[0]) return false;
    int pw = 0, ph = 0; nucleo_ui_panel_size(&pw, &ph);
    if (pw <= 0 || ph <= 0 || pw > 320) return false;
    ensure_dir(GF_DIR);
    char path[192]; snprintf(path, sizeof path, "%s/%s.bmp", GF_DIR, id);
    FILE *f = fopen(path, "wb"); if (!f) return false;

    const META_t *m = gf_meta(id);
    const int W = gf_hero_w(m ? m->shape : (unsigned)GF_LANDSCAPE), H = HERO_H;   // output = exact carousel box for this game's shape
    float sx = (float)W / pw, sy = (float)H / ph, s = sx < sy ? sx : sy;   // fit (letterbox)
    int dw = (int)(pw * s + 0.5f), dh = (int)(ph * s + 0.5f);
    if (dw > W) dw = W;
    if (dh > H) dh = H;
    int offX = (W - dw) / 2, offY = (H - dh) / 2;     // centred; black bars fill the rest

    int rowSize = (W * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * H;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, W); put32(hd + 22, H);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);

    static uint16_t prow[320]; static uint8_t orow[HERO_W * 3 + 4];
    for (int vy = H - 1; vy >= 0; vy--) {             // BMP rows bottom-up; vy = row-from-top
        memset(orow, 0, rowSize);                     // letterbox bars stay black
        if (vy >= offY && vy < offY + dh) {
            int srcY = (int)((vy - offY) / s); if (srcY < 0) srcY = 0; if (srcY >= ph) srcY = ph - 1;
            if (nucleo_ui_read_row(srcY, pw, prow)) {
                for (int ix = 0; ix < dw; ix++) {
                    int srcX = (int)(ix / s); if (srcX < 0) srcX = 0; if (srcX >= pw) srcX = pw - 1;
                    uint16_t raw = prow[srcX], pp = (uint16_t)((raw >> 8) | (raw << 8));
                    int dx = offX + ix;
                    orow[dx * 3 + 0] = (uint8_t)(( pp        & 0x1F) * 255 / 31);   // BGR
                    orow[dx * 3 + 1] = (uint8_t)(((pp >> 5)  & 0x3F) * 255 / 63);
                    orow[dx * 3 + 2] = (uint8_t)(((pp >> 11) & 0x1F) * 255 / 31);
                }
            }
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    return true;
}

// Carousel cover from the OFF-SCREEN CANVAS (RAM read — cheap, no slow panel readback). Scales the
// live frame to the exact hero box for this game's shape (full frame, aspect preserved → fills the
// box with no bars). Used to refresh a game's cover from live gameplay while it runs. Overwrites;
// no history kept.
bool gamefront_save_canvas_cover(const char *id)
{
    if (!id || !id[0]) return false;
    M5Canvas *c = nucleo_screen();
    if (!c) return false;
    int pw = c->width(), ph = c->height();
    if (pw <= 0 || ph <= 0 || pw > 320) return false;
    ensure_dir(GF_DIR);
    char path[192]; snprintf(path, sizeof path, "%s/%s.bmp", GF_DIR, id);
    FILE *f = fopen(path, "wb"); if (!f) return false;

    const META_t *m = gf_meta(id);
    const int W = gf_hero_w(m ? m->shape : (unsigned)GF_LANDSCAPE), H = HERO_H;
    float sx = (float)W / pw, sy = (float)H / ph, s = sx < sy ? sx : sy;
    int dw = (int)(pw * s + 0.5f), dh = (int)(ph * s + 0.5f);
    if (dw > W) dw = W;
    if (dh > H) dh = H;
    int offX = (W - dw) / 2, offY = (H - dh) / 2;

    int rowSize = (W * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * H;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, W); put32(hd + 22, H);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);

    static lgfx::rgb888_t prow[320]; static uint8_t orow[HERO_W * 3 + 4];
    for (int vy = H - 1; vy >= 0; vy--) {
        memset(orow, 0, rowSize);
        if (vy >= offY && vy < offY + dh) {
            int srcY = (int)((vy - offY) / s); if (srcY < 0) srcY = 0; if (srcY >= ph) srcY = ph - 1;
            c->readRect(0, srcY, pw, 1, prow);          // RAM read, M5GFX expands 8bpp -> rgb888
            for (int ix = 0; ix < dw; ix++) {
                int srcX = (int)(ix / s); if (srcX < 0) srcX = 0; if (srcX >= pw) srcX = pw - 1;
                int dx = offX + ix;
                orow[dx * 3 + 0] = prow[srcX].b;        // BMP is BGR
                orow[dx * 3 + 1] = prow[srcX].g;
                orow[dx * 3 + 2] = prow[srcX].r;
            }
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    return true;
}

// Retained API (no longer auto-called on exit): grab the current frame as <id>'s cover unless a
// curated .png/.jpg is present. Cover capture is now manual only (Fn+P / in-game 'C').
void gamefront_seed_cover(const char *id)
{
    if (!id || !id[0]) return;
    char p[192];
    snprintf(p, sizeof p, "%s/%s.png", GF_DIR, id); if (file_exists(p)) return;
    snprintf(p, sizeof p, "%s/%s.jpg", GF_DIR, id); if (file_exists(p)) return;
    gamefront_save_cover(id);
}
