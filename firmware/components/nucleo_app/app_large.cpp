// Two-screen kit (app_large.h): the shared look of on_draw_large on the 320x240 touch panel and of the Cardputer
// companion. Draws straight to `d`; every helper repaints its own box.
#include "app_large.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_touch.h"
#include "nucleo_i18n.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "app_gfx.h"
#include "launcher_theme.h"

namespace lg {

static unsigned short mix(unsigned short a, unsigned short b, int t)   // t = 0 (a) .. 256 (b)
{
    int r = (((a >> 11) & 31) * (256 - t) + ((b >> 11) & 31) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (unsigned short)((r << 11) | (g << 5) | bl);
}

unsigned short card(void) { return mix(BG, FG, 22); }

void font(int f)
{
    switch (f) {
    case 1:  d.setFont(&fonts::FreeSansBold9pt7b); break;
    case 2:  d.setFont(&fonts::FreeSansBold12pt7b); break;
    case 3:  d.setFont(&fonts::FreeSansBold18pt7b); break;
    case 4:  d.setFont(&fonts::Font0); break;
    default: d.setFont(&fonts::Font2); break;
    }
    d.setTextSize(1);
}

void fit(char *out, size_t n, const char *s, int maxw)
{
    snprintf(out, n, "%s", s ? s : "");
    if (d.textWidth(out) <= maxw) return;
    char t[128];
    for (size_t l = strlen(out); l > 1; l--) {
        snprintf(t, sizeof t, "%.*s..", (int)(l - 1), out);
        if (d.textWidth(t) <= maxw) { snprintf(out, n, "%s", t); return; }
    }
    out[0] = 0;
}

int wrap(const char *s, int x, int y, int maxw, int lh, int maxl)
{
    char line[128];
    int n = 0;
    if (!s) return 0;
    while (*s && n < maxl) {
        while (*s == ' ') s++;
        if (*s == '\n') { s++; n++; continue; }
        int fit_at = 0;
        for (int i = 1; s[i - 1] && s[i - 1] != '\n' && i < (int)sizeof line - 1; i++) {   // longest prefix ending at a break
            if (s[i] != ' ' && s[i] != 0 && s[i] != '\n') continue;
            snprintf(line, sizeof line, "%.*s", i, s);
            if (d.textWidth(line) > maxw) break;
            fit_at = i;
            if (!s[i] || s[i] == '\n') break;
        }
        int take = fit_at;
        if (!take) { take = (int)strcspn(s, " \n"); }           // one word longer than the line: cut it below
        if (n == maxl - 1 && s[take] && s[take] != '\n') take = (int)strcspn(s, "\n");   // last line: the rest, cut
        char raw[128]; snprintf(raw, sizeof raw, "%.*s", take, s);
        fit(line, sizeof line, raw, maxw);
        d.drawString(line, x, y + n * lh);
        s += take; n++;
        if (*s == '\n') s++;
    }
    return n;
}

static void chevron(int x, int y, unsigned short c)
{
    d.drawLine(x + 6, y, x, y + 6, c); d.drawLine(x + 7, y, x + 1, y + 6, c);
    d.drawLine(x, y + 6, x + 6, y + 12, c); d.drawLine(x + 1, y + 6, x + 7, y + 12, c);
}
static void menu_glyph(int x, int y, unsigned short c) { for (int k = 0; k < 3; k++) d.fillRoundRect(x, y + k * 5, 16, 2, 1, c); }

int header(const char *title, const char *right, unsigned short acc, int back_key, int menu_key)
{
    const int W = d.width();
    d.fillRect(0, 0, W, HEAD, BG);
    int tx = PAD;
    if (back_key) {
        chevron(10, HEAD / 2 - 7, acc);
        nucleo_touch_add(0, 0, 96, HEAD, back_key, 0);
        tx = 26;
    }
    int rx = W - PAD;
    if (menu_key) {
        menu_glyph(W - 26, HEAD / 2 - 6, MUTED);
        nucleo_touch_add(W - 48, 0, 48, HEAD, menu_key, 0);
        rx = W - 36;
    }
    int rw = 0;
    if (right && right[0]) {
        font(0); d.setTextDatum(middle_right); d.setTextColor(MUTED, BG);
        char r[40]; fit(r, sizeof r, right, 120);
        rw = d.textWidth(r) + 8;
        d.drawString(r, rx, HEAD / 2);
    }
    font(1); d.setTextDatum(middle_left); d.setTextColor(acc, BG);
    char t[64]; fit(t, sizeof t, title, rx - rw - tx);
    d.drawString(t, tx, HEAD / 2 + 1);
    const int tw = d.textWidth(t);
    d.drawFastHLine(PAD, HEAD - 2, W - 2 * PAD, LINE);
    d.fillRect(tx, HEAD - 2, tw < 40 ? tw : 40, 2, acc);
    d.setTextDatum(top_left);
    return HEAD;
}

int tabs(int y, const char *const *names, int n, int active, unsigned short acc, const char *keys)
{
    const int W = d.width(), h = 28;
    d.fillRect(0, y, W, h + 4, BG);
    if (n <= 0) return y + h + 4;
    const int gap = 4, tw = (W - 2 * 6 - (n - 1) * gap) / n;
    font(0);
    for (int i = 0; i < n; i++) {
        const int x = 6 + i * (tw + gap);
        const bool on = i == active;
        const unsigned short f = on ? acc : card();
        d.fillRoundRect(x, y + 2, tw, h - 2, 8, f);
        char t[32]; fit(t, sizeof t, names[i], tw - 8);
        d.setTextDatum(middle_center); d.setTextColor(on ? INK : MUTED, f);
        d.drawString(t, x + tw / 2, y + 2 + (h - 2) / 2);
        if (keys) nucleo_touch_add(x, y, tw, h + 2, NK_CHAR, keys[i]);
        else      nucleo_touch_add(x, y, tw, h + 2, NK_TAB, 0);
    }
    d.setTextDatum(top_left);
    return y + h + 4;
}

int list(int x, int y, int w, int h, int rowh, int n, int sel, int *scroll, row_fn fn, void *ud, unsigned short acc)
{
    const int rows = h / rowh > 0 ? h / rowh : 1;
    int sc = scroll ? *scroll : 0;
    if (sel < sc) sc = sel;
    if (sel >= sc + rows) sc = sel - rows + 1;
    if (sc > n - rows) sc = n - rows;
    if (sc < 0) sc = 0;
    if (scroll) *scroll = sc;
    const bool knob = n > rows;
    const int rw = knob ? w - 6 : w;
    int yy = y;
    for (int i = sc; i < n && i < sc + rows; i++, yy += rowh) {
        const bool focus = i == sel;
        d.fillRect(x, yy, rw, rowh, BG);
        if (focus) d.fillRoundRect(x + 4, yy + 1, rw - 8, rowh - 2, 7, acc);
        fn(i, x + 4, yy, rw - 8, rowh, focus, focus ? acc : BG, ud);
        if (i - sc < 16) nucleo_touch_add(x, yy, rw, rowh, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + (i - sc)));
    }
    if (yy < y + h) d.fillRect(x, yy, rw, y + h - yy, BG);
    if (knob) {
        const int track = h - 8, kh = track * rows / n < 14 ? 14 : track * rows / n;
        const int ky = y + 4 + (track - kh) * sc / (n - rows);
        d.fillRect(x + w - 6, y, 6, h, BG);
        d.fillRect(x + w - 4, y + 4, 2, track, LINE);
        d.fillRect(x + w - 4, ky, 2, kh, acc);
    }
    nucleo_touch_add_scroll(x, y, w, h, rowh, NK_UP, 0, NK_DOWN, 0);
    d.setTextDatum(top_left);
    return rows;
}

void row_text(int x, int y, int w, int h, bool focus, unsigned short bg, const char *label, const char *right, unsigned short col)
{
    font(0);
    int rw = 0;
    if (right && right[0]) {
        char r[40]; fit(r, sizeof r, right, w / 2);
        d.setTextDatum(middle_right); d.setTextColor(focus ? INK : MUTED, bg);
        d.drawString(r, x + w - 8, y + h / 2);
        rw = d.textWidth(r) + 12;
    }
    char t[96]; fit(t, sizeof t, label, w - 16 - rw);
    d.setTextDatum(middle_left); d.setTextColor(focus ? INK : col, bg);
    d.drawString(t, x + 8, y + h / 2);
    d.setTextDatum(top_left);
}

struct TextList { app_ui_text_fn label, right; app_ui_color_fn color; void *ud; };
static void text_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *p)
{
    const TextList *t = (const TextList *)p;
    row_text(x, y, w, h, focus, bg, t->label(i, t->ud), t->right ? t->right(i, t->ud) : nullptr, t->color ? t->color(i, t->ud) : FG);
}
int text_list(int x, int y, int w, int h, int n, int sel, int *scroll, app_ui_text_fn label, app_ui_text_fn right,
              app_ui_color_fn color, void *ud, unsigned short acc, int rowh)
{
    TextList t = { label, right, color, ud };
    return list(x, y, w, h, rowh, n, sel, scroll, text_row, &t, acc);
}

int row_tap(int key, char ch, int scroll, int n, int *sel)
{
    if (!nucleo_touch_is_row(key, ch)) return 0;
    const int i = scroll + (ch - NUCLEO_TOUCH_ROW);
    if (i < 0 || i >= n) return 1;
    if (*sel == i) return 2;
    *sel = i;
    return 1;
}

void button(int x, int y, int w, int h, const char *label, unsigned short fill, unsigned short ink, int key, char ch)
{
    d.fillRoundRect(x, y, w, h, h > 30 ? 10 : 8, fill);
    font(h >= 34 ? 1 : 0);
    char t[48]; fit(t, sizeof t, label, w - 10);
    d.setTextDatum(middle_center); d.setTextColor(ink, fill);
    d.drawString(t, x + w / 2, y + h / 2 + 1);
    d.setTextDatum(top_left);
    nucleo_touch_add(x, y, w, h, key, ch);
}

void panel(int x, int y, int w, int h, unsigned short fill) { d.fillRoundRect(x, y, w, h, 8, fill ? fill : card()); }

void kv(int x, int y, int w, const char *k, const char *v, unsigned short vcol, unsigned short bg)
{
    if (bg == 0xFFFF) bg = BG;
    font(0);
    d.setTextDatum(top_right); d.setTextColor(vcol, bg);
    char vv[64]; fit(vv, sizeof vv, v, w * 2 / 3);
    d.drawString(vv, x + w, y);
    const int vw = d.textWidth(vv);
    char kk[48]; fit(kk, sizeof kk, k, w - vw - 10);
    d.setTextDatum(top_left); d.setTextColor(MUTED, bg);
    d.drawString(kk, x, y);
}

void bar(int x, int y, int w, int h, int pct, unsigned short col)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d.fillRoundRect(x, y, w, h, h / 2, LINE);
    const int f = w * pct / 100;
    if (f > 0) d.fillRoundRect(x, y, f < h ? h : f, h, h / 2, col);
}

void message(int x, int y, int w, int h, const char *head, const char *sub, unsigned short col)
{
    d.fillRect(x, y, w, h, BG);
    font(1); d.setTextColor(col, BG); d.setTextDatum(top_center);
    const int cy = y + h / 2 - (sub && sub[0] ? 22 : 10);
    int nl = wrap(head, x + w / 2, cy, w - 24, 20, 2);
    // wrap() draws with the current datum: top_center keeps every line centred on x + w/2
    if (sub && sub[0]) {
        font(0); d.setTextColor(MUTED, BG);
        wrap(sub, x + w / 2, cy + nl * 20 + 6, w - 24, 17, 3);
    }
    d.setTextDatum(top_left);
}

int comp_head(const char *app, unsigned short acc)
{
    const int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, 14, BG);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(acc, BG);
    char t[64]; snprintf(t, sizeof t, "%s  -  %s", app, TR("sul secondo schermo", "on the second screen"));
    d.drawString(t, 8, top + 4);
    return top + 16;
}

void comp_keys(const char *keys)
{
    const int y = nucleo_app_content_top() + nucleo_app_content_height() - 11;
    d.fillRect(0, y - 1, 240, 12, BG);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(DIM, BG);
    char t[48]; snprintf(t, sizeof t, "%s", keys ? keys : "");
    d.drawString(t, 8, y);
}

void game(void (*draw_game)(void), void (*hud)(int y, int h), const char *name, unsigned short acc)
{
    LovyanGFX *panel = &d;                                    // the external panel (on_draw_large draws direct)
    const int W = panel->width(), H = panel->height();
    M5Canvas *cv = nucleo_screen();
    int fh = 0;
    if (cv) {
        nucleo_app_set_gfx(cv);
        cv->fillSprite(BG);
        draw_game();
        const int gh = nucleo_app_content_height();          // what the game drew: 121 rows, or 135 fullscreen
        nucleo_app_set_gfx(nullptr);
        const float s = (float)W / cv->width();
        fh = (int)(gh * s + 0.5f);
        if (fh > H) fh = H;
        panel->setClipRect(0, 0, W, fh);
        cv->pushRotateZoom(panel, W / 2.0f, cv->height() * s / 2.0f, 0.0f, s, s);
        panel->clearClipRect();
    }
    if (fh >= H) return;
    if (hud) { hud(fh, H - fh); return; }
    panel->fillRect(0, fh, W, H - fh, BG);
    font(1); d.setTextDatum(middle_center); d.setTextColor(acc, BG);
    d.drawString(name ? name : "", W / 2, fh + (H - fh) / 2);
    d.setTextDatum(top_left);
}

}  // namespace lg
