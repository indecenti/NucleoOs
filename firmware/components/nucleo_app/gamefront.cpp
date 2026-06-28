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
// EmulationStation-style detail view: slim title header, a large rectangular cover with a soft
// shadow + accent frame, procedural neighbour "peek" cards flanking it, then a row of mode badges
// and carousel dots, over a vertical accent gradient.
#define GF_MAX     24
#define MARQ_H     22                 // title header band across the top
#define HERO_W     182                // big rectangular cover, centred under the header
#define HERO_H     72
#define HERO_X     ((W - HERO_W) / 2) // = 29; the side gutters host the neighbour peek cards
#define HERO_Y     25
#define COVER_W    HERO_W             // covers captured at hero size -> drawn 1:1, no scaler needed
#define COVER_H    HERO_H
#define BADGE_Y    (HERO_Y + HERO_H + 3)  // mode-badge row, just under the cover → y=100
#define DOT_Y      (BADGE_Y + 15)         // carousel position dots → y=115
#define FOOT       13                 // bottom footer → starts at y=122

#define GF_DIR   NUCLEO_SD_MOUNT "/data/GameShots"
#define SHOT_DIR NUCLEO_SD_MOUNT "/data/Screenshots"

// ---- per-game metadata: custom marquee title, play modes, one-line tagline ---
enum { GM_1P = 0x01, GM_CPU = 0x02, GM_2P = 0x04, GM_LAN = 0x08, GM_COOP = 0x10 };

typedef struct { const char *id; const char *title; unsigned modes; const char *tag; } META_t;
static const META_t META[] = {
    { "reactor",  "REACTOR",            GM_1P,           "Bilancia potenza e raffreddamento. Evita il meltdown." },
    { "stelle",   "COSTELLAZIONI 3D",   GM_1P,           "Sparatutto spaziale arcade fra le stelle." },
    { "giardino", "GIARDINO DI SABBIA", GM_1P,           "Sandbox di elementi che cadono. Relax puro." },
    { "slots",    "SLOT MACHINE",       GM_1P,           "Tira la leva e insegui il jackpot." },
    { "poker",    "VIDEO POKER",        GM_1P,           "5 carte: tieni le buone, punta alla scala reale." },
    { "pinball",  "PINBALL",            GM_1P,           "Flipper verticale: respingenti, spinner, punti." },
    { "pong",     "PONG",               GM_CPU | GM_LAN, "1v1 in rete fra due Cardputer, o contro la CPU." },
    { "tanks",    "NUCLEO TANKS",       GM_CPU | GM_2P,  "Artiglieria a turni: terreno distruttibile, vento, 9 armi." },
    { "brawler",  "SCORRIBANDA",        GM_1P | GM_COOP, "Picchiaduro noir a scorrimento, anche in co-op." },
    { "dice",     "DADI 3D",            GM_1P,           "Tiro di dadi: scuoti il device o premi invio." },
    { nullptr, nullptr, 0, nullptr },
};

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

// Background gradient colour at row y (shared so faux-rounded corners blend into the same gradient).
static uint16_t bg_at(uint16_t acc, int y)
{
    uint16_t top = mix565(acc, INK, 0.5f);
    float t = (float)y / H; if (t > 0.85f) t = 1.0f; else t /= 0.85f;
    return mix565(top, BG, t);
}

// Paint faux-rounded corner cutouts on a just-drawn rectangular cover, blending into the bg.
template <typename T> static void round_corners(T *c, int x, int y, int w, int h, int r, uint16_t acc)
{
    for (int yy = 0; yy < r; yy++)
        for (int xx = 0; xx < r; xx++) {
            if ((r - xx) * (r - xx) + (r - yy) * (r - yy) <= r * r) continue;
            uint16_t ct = bg_at(acc, y), cb = bg_at(acc, y + h - 1);
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

// Carousel position dots between badges and footer.
template <typename T> static void draw_dots(T *c, int sel, int cnt, uint16_t acc)
{
    if (cnt <= 1) return;
    const int D = 4, GAP = 4, MAX_D = 11;
    int show = cnt < MAX_D ? cnt : MAX_D;
    int start = 0;
    if (cnt > MAX_D) {
        start = sel - MAX_D / 2;
        if (start < 0) start = 0;
        if (start + MAX_D > cnt) start = cnt - MAX_D;
    }
    int total = show * D + (show - 1) * GAP;
    int ox = (W - total) / 2, y = DOT_Y + 1;
    for (int i = 0; i < show; i++) {
        bool cur = (start + i == sel);
        int x = ox + i * (D + GAP);
        if (cur) {
            c->fillRoundRect(x - 2, y - 1, D + 4, D + 2, 2, mix565(acc, FG, 0.90f));
        } else {
            c->fillCircle(x + D / 2, y + D / 2, D / 2 - 1, mix565(acc, INK, 0.28f));
            c->drawCircle(x + D / 2, y + D / 2, D / 2, mix565(acc, LINE, 0.55f));
        }
    }
}

// ---- the cover (real screenshot) or a procedural poster ---------------------
template <typename T> static void draw_poster(T *c, const nucleo_app_def_t *g)
{
    uint16_t acc = g->color;
    // Richer dark gradient body.
    for (int yy = 0; yy < HERO_H; yy++) {
        float t = (float)yy / HERO_H;
        c->drawFastHLine(HERO_X, HERO_Y + yy, HERO_W, mix565(acc, INK, 0.44f + 0.48f * t));
    }
    int cx = HERO_X + HERO_W / 2, cy = HERO_Y + HERO_H / 2;
    // App icon shifted to upper half.
    launcher_draw_icon(c, cx, cy - 14, 30, g->id, g->icon,
                       mix565(acc, FG, 0.68f), mix565(acc, INK, 0.42f));
    // Camera pictogram: body + lens + viewfinder bump.
    uint16_t cm = mix565(acc, FG, 0.28f);
    int bx = cx - 10, by = cy + 6;
    c->fillRoundRect(bx + 1, by + 2, 20, 12, 2, mix565(INK, BG, 0.5f)); // shadow
    c->fillRoundRect(bx, by, 20, 12, 2, cm);
    c->fillCircle(cx, by + 7, 4, mix565(acc, INK, 0.72f));              // lens
    c->fillCircle(cx, by + 7, 2, mix565(cm, FG, 0.22f));                // lens highlight
    c->fillRect(cx - 3, by - 3, 6, 4, cm);                              // viewfinder bump
    c->fillRect(cx - 1, by - 3, 2, 2, mix565(acc, INK, 0.72f));         // bump hole
    // Hint.
    c->setFont(&fonts::Font0); c->setTextSize(1);
    c->setTextColor(mix565(acc, FG, 0.32f));
    const char *cap = "Fn+P per catturare";
    c->setCursor(cx - (int)c->textWidth(cap) / 2, by + 16); c->print(cap);
}

template <typename T> static void draw_hero(T *c, const nucleo_app_def_t *g)
{
    uint16_t acc = g->color;

    // Accent glow halo: concentric rounded rects fading outward into the background.
    for (int i = 4; i >= 1; i--) {
        uint16_t gl = mix565(acc, bg_at(acc, HERO_Y), 0.62f + 0.10f * (4 - i));
        c->drawRoundRect(HERO_X - 2 - i, HERO_Y - 2 - i, HERO_W + 4 + 2 * i, HERO_H + 4 + 2 * i, 6 + i, gl);
    }
    // Soft drop-shadow, offset down-right (EmulationStation depth).
    c->fillRoundRect(HERO_X + 3, HERO_Y + 5, HERO_W, HERO_H, 6, mix565(BG, INK, 0.88f));

    c->setClipRect(HERO_X, HERO_Y, HERO_W, HERO_H);
    char p[192]; bool drawn = false;
    snprintf(p, sizeof p, "%s/%s.png", GF_DIR, g->id);
    if (file_exists(p)) { c->drawPngFile(p, HERO_X + HERO_W / 2, HERO_Y + HERO_H / 2, HERO_W, HERO_H, 0, 0, 0.0f, 0.0f, datum_t::middle_center); drawn = true; }
    if (!drawn) { snprintf(p, sizeof p, "%s/%s.jpg", GF_DIR, g->id);
        if (file_exists(p)) { c->drawJpgFile(p, HERO_X + HERO_W / 2, HERO_Y + HERO_H / 2, HERO_W, HERO_H, 0, 0, 0.0f, 0.0f, datum_t::middle_center); drawn = true; } }
    if (!drawn) { snprintf(p, sizeof p, "%s/%s.bmp", GF_DIR, g->id);
        if (file_exists(p)) { c->drawBmpFile(p, HERO_X, HERO_Y); drawn = true; } }
    if (!drawn) draw_poster(c, g);
    // A glossy sheen across the top third: lighten the real artwork pixels, fading out.
    // Read each row back (RGB565), brighten toward FG, write it again — proven readRect path.
    static uint16_t row[HERO_W];
    int sh = HERO_H / 3;
    for (int yy = 0; yy < sh; yy++) {
        float a = 0.16f * (1.0f - (float)yy / sh);
        if (a <= 0.01f) continue;
        int y = HERO_Y + yy;
        c->readRect(HERO_X, y, HERO_W, 1, row);
        for (int xx = 0; xx < HERO_W; xx++) row[xx] = mix565(row[xx], FG, a);
        c->pushImage(HERO_X, y, HERO_W, 1, row);
    }
    c->clearClipRect();

    round_corners(c, HERO_X, HERO_Y, HERO_W, HERO_H, 5, acc);

    // Double frame: a dark inset + a bright accent edge, plus a crisp top highlight.
    c->drawRoundRect(HERO_X - 1, HERO_Y - 1, HERO_W + 2, HERO_H + 2, 5, mix565(BG, INK, 0.6f));
    c->drawRoundRect(HERO_X - 2, HERO_Y - 2, HERO_W + 4, HERO_H + 4, 6, mix565(acc, FG, 0.4f));
    c->drawFastHLine(HERO_X + 3, HERO_Y - 2, HERO_W - 6, mix565(acc, FG, 0.75f));
}

// ---- a faded procedural "peek" of a neighbour game, in a side gutter --------
template <typename T> static void draw_peek(T *c, const nucleo_app_def_t *g, bool left)
{
    const int pw = 24, ph = 58;
    int py = HERO_Y + (HERO_H - ph) / 2;
    int px = left ? 1 : W - 1 - pw;
    uint16_t acc = g->color;

    // Receding gradient body.
    for (int yy = 0; yy < ph; yy++) {
        float t = (float)yy / ph;
        float fade = left ? 0.38f + 0.20f * t : 0.38f + 0.20f * t;
        c->drawFastHLine(px, py + yy, pw, mix565(mix565(acc, INK, 0.52f), BG, fade));
    }

    // Icon (slightly larger than before).
    launcher_draw_icon(c, px + pw / 2, py + ph / 2 - 10, 16, g->id, g->icon,
                       mix565(acc, FG, 0.55f), mix565(acc, INK, 0.45f));

    // First word of title (max 5 chars) at the bottom of the card.
    c->setFont(&fonts::Font0); c->setTextSize(1);
    c->setTextColor(mix565(acc, FG, 0.50f));
    const char *title = gf_title(g);
    char tb[6] = {0}; int tl = 0;
    for (int i = 0; title[i] && tl < 5 && title[i] != ' '; i++) tb[tl++] = title[i];
    int tw = tl * 6;
    c->setCursor(px + (pw - tw) / 2, py + ph - 13); c->print(tb);

    // Frame + inner accent edge.
    c->drawRoundRect(px, py, pw, ph, 3, mix565(acc, INK, 0.35f));
    c->drawFastVLine(left ? px + pw - 1 : px, py + 2, ph - 4, mix565(acc, LINE, 0.55f));
    c->drawFastHLine(px + 2, py, pw - 4, mix565(acc, FG, 0.18f));  // top gloss

    // Direction chevron.
    int cy = py + ph - 21, cx = px + pw / 2;
    uint16_t ch = mix565(acc, FG, 0.65f);
    if (left)  { c->fillTriangle(cx + 4, cy - 5, cx + 4, cy + 5, cx - 4, cy, ch); }
    else       { c->fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 4, cy, ch); }
}

// ---- title header (system bar) ----------------------------------------------
template <typename T> static void draw_header(T *c, const nucleo_app_def_t *g, int sel, int cnt)
{
    uint16_t acc = g->color;
    for (int yy = 0; yy < MARQ_H; yy++)                          // glassy accent band
        c->drawFastHLine(0, yy, W, mix565(acc, INK, 0.62f - 0.14f * (float)yy / MARQ_H));
    c->drawFastHLine(0, 0, W, mix565(acc, FG, 0.30f));          // top glass highlight
    c->drawFastHLine(0, MARQ_H - 2, W, mix565(acc, FG, 0.5f));   // bright accent baseline
    c->drawFastHLine(0, MARQ_H - 1, W, mix565(acc, INK, 0.4f));

    // "GIOCHI" system kicker always visible top-left.
    c->setFont(&fonts::Font0); c->setTextSize(1);
    c->setTextColor(mix565(acc, FG, 0.55f)); c->setCursor(4, 3); c->print("GIOCHI");

    const char *t = gf_title(g);
    c->setFont(&fonts::Font4);
    if ((int)c->textWidth(t) > W - 54) c->setFont(&fonts::Font2);
    int tw = (int)c->textWidth(t), tx = (W - tw) / 2; if (tx < 42) tx = 42;
    int ty = (MARQ_H - 2 - c->fontHeight()) / 2 + 1;
    c->setTextColor(mix565(acc, INK, 0.3f)); c->setCursor(tx + 1, ty + 1); c->print(t);   // shadow
    c->setTextColor(FG);                     c->setCursor(tx,     ty);     c->print(t);   // face

    c->setFont(&fonts::Font0); c->setTextSize(1);                // position / filter chip, top-right
    if (s_filter[0]) {
        char fb[20]; snprintf(fb, sizeof fb, "/%.12s", s_filter);
        int fw = (int)strlen(fb) * 6;
        c->fillRoundRect(W - fw - 5, 2, fw + 4, 10, 2, 0x1926);
        c->setTextColor(C_GREEN, 0x1926); c->setCursor(W - fw - 2, 3); c->print(fb);
    } else if (cnt > 0) {
        char kn[12]; snprintf(kn, sizeof kn, "%d/%d", sel + 1, cnt);
        int kw = (int)strlen(kn) * 6;
        c->fillRoundRect(W - kw - 5, 2, kw + 4, 10, 2, mix565(acc, INK, 0.45f));
        c->setTextColor(FG); c->setCursor(W - kw - 2, 3); c->print(kn);
    }
}

template <typename T> static void draw_footer(T *c, const char *hint)
{
    c->fillRect(0, H - FOOT, W, FOOT, INK);
    c->drawFastHLine(0, H - FOOT, W, LINE);
    int hx = (W - (int)strlen(hint) * 6) / 2; if (hx < 2) hx = 2;
    c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(MUTED, INK);
    c->setCursor(hx, H - FOOT + 3); c->print(hint);
}

// ---- cinematic background: accent gradient + diagonal sheen + edge vignette --
template <typename T> static void draw_bg(T *c, uint16_t acc)
{
    for (int y = 0; y < H; y++) c->drawFastHLine(0, y, W, bg_at(acc, y));

    // Two faint diagonal light bands sweeping top-left -> bottom-right.
    for (int k = 0; k < 2; k++) {
        int off = k * 70;
        for (int y = 0; y < H; y++) {
            int x = (H - y) + off - 40;
            if (x >= 0 && x < W) {
                uint16_t base = bg_at(acc, y);
                c->drawPixel(x, y, mix565(base, FG, 0.10f));
                if (x + 1 < W) c->drawPixel(x + 1, y, mix565(base, FG, 0.06f));
            }
        }
    }

    // Edge vignette: darken the left/right margins to focus the eye on the cover.
    for (int x = 0; x < 24; x++) {
        float v = 0.22f * (1.0f - (float)x / 24);
        for (int y = MARQ_H; y < H - FOOT; y += 1) {
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
        c->drawFastHLine(0, MARQ_H - 2, W, mix565(C_RED, FG, 0.45f));
        c->setFont(&fonts::Font2); c->setTextColor(FG, BG);
        c->setCursor(8, (MARQ_H - 2 - c->fontHeight()) / 2 + 1); c->print("Giochi");
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

    draw_bg(c, g->color);

    if (cnt > 1) {                                               // carousel neighbour peeks
        draw_peek(c, nucleo_app_at(gf_at((s_sel - 1 + cnt) % cnt)), true);
        draw_peek(c, nucleo_app_at(gf_at((s_sel + 1) % cnt)), false);
    }
    draw_hero(c, g);
    draw_header(c, g, s_sel, cnt);
    draw_badges(c, modes);
    draw_dots(c, s_sel, cnt, g->color);

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
    if ((long)COVER_W * sh > (long)COVER_H * sw) { cw = sw; ch = (int)((long)sw * COVER_H / COVER_W); }
    else                                         { ch = sh; cw = (int)((long)sh * COVER_W / COVER_H); }
    return save_bmp(c, p, COVER_W, COVER_H, (sw - cw) / 2, (sh - ch) / 2, cw, ch);
}

bool gamefront_save_screenshot(const char *name)
{
    M5Canvas *c = nucleo_screen(); if (!c) return false;
    ensure_dir(SHOT_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, name && name[0] ? name : "shot");
    return save_bmp(c, p, c->width(), c->height(), 0, 0, c->width(), c->height());
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
