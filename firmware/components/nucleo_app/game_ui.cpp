// game_ui — see game_ui.h. One copy in flash for every game (that is the point: the games drop their own
// title/menu/panel code). Every face here is already linked by the launcher / Settings / Tanks.
#include "game_ui.h"
#include "app_gfx.h"
#include "launcher_theme.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include <string.h>

namespace gui {

static const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static inline int r8(uint16_t c) { return ((c >> 11) & 31) * 255 / 31; }
static inline int g8(uint16_t c) { return ((c >> 5) & 63) * 255 / 63; }
static inline int b8(uint16_t c) { return (c & 31) * 255 / 31; }
static inline uint16_t pack(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return pack((r * 7 + 127) / 255 * 255 / 7, (g * 7 + 127) / 255 * 255 / 7, (b * 3 + 127) / 255 * 255 / 3);
}

uint16_t mix(uint16_t a, uint16_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    return pack(r8(a) + (r8(b) - r8(a)) * t / 256, g8(a) + (g8(b) - g8(a)) * t / 256, b8(a) + (b8(b) - b8(a)) * t / 256);
}

// One channel to an RGB332 level with a Bayer threshold (levels = 7 for R/G, 3 for B).
static inline int dq(int v, int levels, int t) { int l = (v * levels * 16 + t * 255) / (255 * 16); return (l > levels ? levels : l) * 255 / levels; }

void vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    if (w <= 0 || h <= 0) return;
    int r0 = r8(top), g0 = g8(top), b0 = b8(top), r1 = r8(bottom), g1 = g8(bottom), b1 = b8(bottom);
    for (int j = 0; j < h; j++) {
        int r = r0 + (r1 - r0) * j / (h > 1 ? h - 1 : 1), g = g0 + (g1 - g0) * j / (h > 1 ? h - 1 : 1), b = b0 + (b1 - b0) * j / (h > 1 ? h - 1 : 1);
        uint16_t c[4];
        const uint8_t *row = BAYER4 + (((y + j) & 3) << 2);
        for (int k = 0; k < 4; k++) c[k] = pack(dq(r, 7, row[k]), dq(g, 7, row[k]), dq(b, 3, row[k]));
        d.drawFastHLine(x, y + j, w, c[0]);                       // most rows: one colour, one call
        if (c[1] == c[0] && c[2] == c[0] && c[3] == c[0]) continue;
        for (int i = x; i < x + w; i++) { uint16_t ci = c[i & 3]; if (ci != c[0]) d.drawPixel(i, y + j, ci); }
    }
}

void panel(int x, int y, int w, int h, uint16_t fill, uint16_t edge)
{
    int r = h < 16 ? 3 : 6;
    d.fillRoundRect(x + 2, y + 2, w, h, r, rgb(0, 0, 0));                  // drop shadow
    d.fillRoundRect(x, y, w, h, r, fill);
    d.drawFastHLine(x + r, y + 1, w - 2 * r, mix(fill, rgb(255, 255, 255), 70));   // light from above
    d.drawRoundRect(x, y, w, h, r, edge);
}

static const lgfx::IFont *face(int font)
{
    switch (font) {
        case F_BODY:  return &fonts::FreeSansBold9pt7b;
        case F_TITLE: return &fonts::FreeSansBold12pt7b;
        case F_BIG:   return &fonts::Font4;
        default:      return &fonts::Font2;
    }
}
static void restore(void) { d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(textdatum_t::top_left); }

int text_width(const char *s, int font)
{
    d.setFont(face(font)); d.setTextSize(1);
    int w = d.textWidth(s);
    restore();
    return w;
}

int font_h(int font)
{
    d.setFont(face(font)); d.setTextSize(1);
    int h = d.fontHeight();
    restore();
    return h;
}

int text(const char *s, int x, int y, int datum, int font, uint16_t col, uint16_t shadow)
{
    d.setFont(face(font)); d.setTextSize(1);
    d.setTextDatum(datum == 1 ? textdatum_t::top_center : datum == 2 ? textdatum_t::top_right : textdatum_t::top_left);
    d.setTextColor(shadow); d.drawString(s, x + 1, y + 1);
    d.setTextColor(col);    d.drawString(s, x, y);
    int w = d.textWidth(s);
    restore();
    return w;
}

int title(const char *name, const char *sub, uint16_t accent)
{
    int ch = nucleo_app_content_height();
    vgradient(0, 0, W, ch, mix(rgb(0, 0, 0), accent, 70), rgb(0, 0, 0));
    int tw = text(name, W / 2, 3, 1, F_TITLE, rgb(255, 255, 255), mix(rgb(0, 0, 0), accent, 120));
    int th = font_h(F_TITLE);
    int ux = W / 2 - tw / 2, uy = 3 + th - 1;
    d.fillRect(ux, uy, tw, 2, accent);                                     // accent underline, faded ends
    d.drawFastHLine(ux - 6, uy, 6, mix(rgb(0, 0, 0), accent, 120));
    d.drawFastHLine(ux + tw, uy, 6, mix(rgb(0, 0, 0), accent, 120));
    int y = uy + 4;
    if (sub && sub[0]) {                                                   // a subtitle too wide for Font2 drops to the 6 px face
        uint16_t sc = mix(rgb(255, 255, 255), accent, 90);
        if (text_width(sub, F_SMALL) <= W - 8) { text(sub, W / 2, y, 1, F_SMALL, sc, rgb(0, 0, 0)); y += font_h(F_SMALL); }
        else { d.setTextDatum(textdatum_t::top_center); d.setTextColor(sc); d.drawString(sub, W / 2, y + 3); d.setTextDatum(textdatum_t::top_left); y += 12; }
    }
    return y + 2;
}

static const int ROW_H = 22;

void menu(const Menu &m, const char *const *items, int n, int y0, int y1, uint16_t accent)
{
    if (n <= 0) return;
    int vis = (y1 - y0) / ROW_H; if (vis < 1) vis = 1; if (vis > n) vis = n;
    int top = (int)(m.pos + 0.5f) - vis / 2;                               // keep the cursor in the middle when scrolling
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;
    int pad = (y1 - y0 - vis * ROW_H) / 2; if (pad < 0) pad = 0;
    int base = y0 + pad;
    float py = base + (m.pos - top) * ROW_H;                               // the pill glides between rows
    if (py >= base - ROW_H / 2 && py <= base + (vis - 1) * ROW_H + ROW_H / 2) {
        int pyi = (int)(py + 0.5f);
        d.fillSmoothRoundRect(14, pyi + 1, W - 28, ROW_H - 2, 9, mix(rgb(0, 0, 0), accent, 150));
        d.drawRoundRect(14, pyi + 1, W - 28, ROW_H - 2, 9, accent);
        d.fillRect(20, pyi + 6, 3, ROW_H - 12, rgb(255, 255, 255));        // a bright tick on the left of the pill
    }
    for (int k = 0; k < vis; k++) {
        int i = top + k, y = base + k * ROW_H;
        bool cur = (i == m.sel);
        uint16_t col = cur ? rgb(255, 255, 255) : mix(rgb(160, 170, 200), rgb(0, 0, 0), (k == 0 && top > 0) || (k == vis - 1 && top + vis < n) ? 110 : 0);
        int f = text_width(items[i], F_BODY) <= W - 44 ? F_BODY : F_SMALL;
        text(items[i], W / 2, y + (ROW_H - font_h(f)) / 2 + (f == F_BODY ? 1 : 0), 1, f, col, rgb(0, 0, 0));
    }
    if (top > 0)            d.fillTriangle(W - 12, base + 6, W - 8, base + 2, W - 4, base + 6, accent);                 // more above
    if (top + vis < n)      d.fillTriangle(W - 12, base + vis * ROW_H - 6, W - 8, base + vis * ROW_H - 2, W - 4, base + vis * ROW_H - 6, accent);  // more below
}

bool menu_key(Menu &m, int key, int n)
{
    if (n <= 0) return false;
    if (key == NK_UP)   { m.sel = (int8_t)((m.sel + n - 1) % n); return true; }
    if (key == NK_DOWN) { m.sel = (int8_t)((m.sel + 1) % n); return true; }
    return false;
}

bool menu_tick(Menu &m, int dt_ms)
{
    float diff = m.sel - m.pos;
    if (diff == 0) return false;
    if (diff > 2.5f || diff < -2.5f) { m.pos = m.sel; return true; }       // a wrap-around jumps, it does not fly past every row
    float k = dt_ms / (dt_ms + 55.0f);                                     // ease-out, the same feel at any frame rate
    m.pos += diff * k;
    if ((m.sel - m.pos) * (m.sel - m.pos) < 0.0004f) m.pos = m.sel;
    return true;
}

void dialog(const char *head, const char *l1, const char *l2, const char *keys, uint16_t accent)
{
    int ch = nucleo_app_content_height();
    int lines = (l1 && l1[0]) + (l2 && l2[0]);
    int h = 30 + lines * 17 + (keys && keys[0] ? 18 : 0), w = 208;
    int x = (W - w) / 2, y = (ch - h) / 2;
    panel(x, y, w, h, rgb(0, 36, 85), accent);                             // navy: exact in RGB332 (18,22,40 snapped to olive)
    int ty = y + 6;
    text(head, W / 2, ty, 1, F_BODY, accent, rgb(0, 0, 0)); ty += 22;
    if (l1 && l1[0]) { text(l1, W / 2, ty, 1, F_SMALL, rgb(255, 255, 255), rgb(0, 0, 0)); ty += 17; }
    if (l2 && l2[0]) { text(l2, W / 2, ty, 1, F_SMALL, rgb(255, 255, 255), rgb(0, 0, 0)); ty += 17; }
    if (keys && keys[0]) text(keys, W / 2, ty + 1, 1, F_SMALL, mix(rgb(255, 255, 255), accent, 110), rgb(0, 0, 0));
}

}  // namespace gui
