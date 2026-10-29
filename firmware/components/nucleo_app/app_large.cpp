// Two-screen kit (app_large.h): the shared look of on_draw_large on the 320x240 touch panel and of the Cardputer
// companion. Draws straight to `d`; every helper repaints its own box.
#include "app_large.h"
#include "esp_timer.h"     // capture(): a request that never meets a whole frame times out
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_touch.h"
#include "nucleo_i18n.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_gfx.h"
#include "launcher_theme.h"
#include "ui_glyph.h"
#include "nucleo_theme.h"

namespace lg {

static unsigned short mix(unsigned short a, unsigned short b, int t)   // t = 0 (a) .. 256 (b)
{
    int r = (((a >> 11) & 31) * (256 - t) + ((b >> 11) & 31) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (unsigned short)((r << 11) | (g << 5) | bl);
}

static int q332(unsigned short c) { return ((c >> 13) << 5) | (((c >> 8) & 7) << 2) | ((c >> 3) & 3); }
// A themed tint that must survive the 8-bit canvas of lg::frame (RGB332: 3-3-2 bits): there a dark grey
// either collapses into BG or turns olive (blue keeps only 2 bits). Walk from c toward FG and take the first
// step whose RGB332 is not BG and stays close to the colour it stands for. 16-bit targets get c as is.
static unsigned short survive(unsigned short c)
{
    if (d.getColorDepth() > 8) return c;
    const int qb = q332(BG);
    unsigned short first = 0; bool have = false;
    for (int t = 0; t <= 200; t += 6) {
        const unsigned short m = mix(c, FG, t);
        const int q = q332(m);
        if (q == qb) continue;
        if (!have) { first = m; have = true; }
        const int r = (q >> 5) * 255 / 7, g = ((q >> 2) & 7) * 255 / 7, b = (q & 3) * 255 / 3;
        const int R = ((m >> 11) & 31) * 255 / 31, G = ((m >> 5) & 63) * 255 / 63, B = (m & 31) * 255 / 31;
        if (abs(r - R) + abs(g - G) + abs(b - B) <= 48) return m;
    }
    return have ? first : c;
}
unsigned short card(void) { return survive(mix(BG, FG, 22)); }

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

static void menu_glyph(int x, int y, unsigned short c) { for (int k = 0; k < 3; k++) d.fillRoundRect(x, y + k * 5, 16, 2, 1, c); }

int back(int x, int y, int h, int key)
{
    const int w = 40, cy = y + h / 2;
    d.fillRoundRect(x, y, w, h, h / 2, LINE);
    for (int k = 0; k < 3; k++) { d.drawLine(x + 24 + k, cy - 6, x + 18 + k, cy, FG); d.drawLine(x + 18 + k, cy, x + 24 + k, cy + 6, FG); }
    nucleo_touch_add(x > 4 ? x - 4 : 0, y > 4 ? y - 4 : 0, w + 24, h + 8, key, 0);
    return x + w;
}

int header(const char *title, const char *right, unsigned short acc, int back_key, int menu_key)
{
    const int W = d.width();
    d.fillRect(0, 0, W, HEAD, BG);
    int tx = PAD;
    if (back_key) {
        back(3, 3, HEAD - 6, back_key);
        nucleo_touch_add(0, 0, 96, HEAD + 2, back_key, 0);     // the title beside it backs too
        tx = 52;
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

int search(int x, int y, int w, int h, const char *text, const char *hint, const char *count, unsigned short ccol, unsigned short acc)
{
    const unsigned short cb = card(), ic = text && text[0] ? acc : MUTED;
    const int ph = h - 8, cy = y + h / 2, r = ph >= 24 ? 6 : 4, mx = x + 8 + ph / 2 + 2;
    d.fillRect(x, y, w, h, BG);
    d.fillRoundRect(x + 4, y + 4, w - 8, ph, ph / 2, cb);
    d.drawCircle(mx, cy - 1, r, ic); d.drawCircle(mx, cy - 1, r - 1, ic);
    d.drawLine(mx + r - 2, cy + r - 3, mx + r + 2, cy + r + 1, ic); d.drawLine(mx + r - 1, cy + r - 3, mx + r + 3, cy + r + 1, ic);
    const int tx = mx + r + 8;
    font(0);
    int cw = 0;
    if (count && count[0]) {
        d.setTextDatum(middle_right); d.setTextColor(ccol, cb);
        d.drawString(count, x + w - 14, cy + 1);
        cw = d.textWidth(count) + 8;
    }
    const int room = x + w - 14 - cw - tx;
    d.setTextDatum(middle_left);
    if (text && text[0]) {
        const char *t = text;
        while (*t && d.textWidth(t) > room - 4) t++;          // the tail of a long query: what was typed last
        d.setTextColor(FG, cb); d.drawString(t, tx, cy + 1);
        d.fillRect(tx + 1 + d.textWidth(t), cy - ph / 2 + 5, 2, ph - 10, acc);
    } else if (hint) {
        char t[48]; fit(t, sizeof t, hint, room);
        d.setTextColor(MUTED, cb); d.drawString(t, tx, cy + 1);
    }
    d.setTextDatum(top_left);
    return y + h;
}

void wheel(int x, int y, int w, int h, const Wheel &wl)
{
    d.fillRect(x, y, w, h, BG);
    if (wl.n <= 0 || wl.slots <= 0) return;
    const int mid = wl.slots / 2, ww = w - 6;
    int sum = 0;
    for (int k = 0; k < wl.slots; k++) sum += wl.heights[k];
    const int top = y + (h - sum) / 2;
    int pill = top;
    for (int k = 0; k < mid; k++) pill += wl.heights[k];
    const int ph = wl.heights[mid];
    d.fillRoundRect(x, pill + 1, ww, ph - 2, (ph - 2) / 2, wl.acc);      // the pill stays; the titles turn through it
    // Only what falls in the strip being composed is drawn (lg::frame paints a frame a strip at a time); every slot's
    // touch target is registered all the same.
    int32_t cx, cy, cw, ch;
    d.getClipRect(&cx, &cy, &cw, &ch);
    int32_t sx, sy, sw, sh0;                       // the titles' own clip: the slots' span, inside the strip's
    sx = x; sw = w; sy = top > cy ? top : cy; sh0 = (top + sum < cy + ch ? top + sum : cy + ch) - sy;
    if (sh0 > 0) d.setClipRect(sx, sy, sw, sh0);
    int wy = top;
    for (int k = -1; k <= wl.slots; k++) {         // one slot more on each side: what slides in while it turns
        const int sh = wl.heights[k < 0 ? 0 : k >= wl.slots ? wl.slots - 1 : k];
        const int i = wheel_at(wl.sel, wl.n, wl.slots, k);
        const int ry = (k < 0 ? top - sh : wy) + wl.offset;                // where this title is drawn
        const int dist = (abs(ry + sh / 2 - (pill + ph / 2)) * 2 + (ph + sh) / 2) / (ph + sh);   // slots from the pill
        if (i >= 0 && sh0 > 0 && ry + sh > sy && ry < sy + sh0) {
            const bool m = dist == 0;
            const int ix = x + 12 + (dist < 4 ? dist : 4) * 5;             // the wheel's curve: far rows step in
            const char *t = wl.label(i, wl.ud);
            const int iw = wl.icon ? wl.icon(i, ix, ry + sh / 2, m, wl.ud) : 0;
            const int lx = iw ? ix + iw + 6 : ix;
            char fitted[64];
            font(m ? 1 : 0);
            fit(fitted, sizeof fitted, t ? t : "", x + ww - 8 - lx);
            d.setTextDatum(middle_left);
            d.setTextColor(m ? INK : dist == 1 ? FG : MUTED);
            d.drawString(fitted, lx, ry + sh / 2 + (m ? 0 : 1));
            d.setTextDatum(top_left);
            if (wl.mark && wl.mark(i, wl.ud)) d.fillCircle(ix - 6, ry + sh / 2, 2, m ? INK : wl.mark_col);
        }
        if (k >= 0 && k < wl.slots) {
            if (i >= 0) nucleo_touch_add(x, wy, ww, sh, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + k));
            wy += sh;
        }
    }
    if (sh0 > 0) d.setClipRect(cx, cy, cw, ch);    // the strip's clip back
    nucleo_touch_add_scroll(x, y, w, h, 22, NK_UP, 0, NK_DOWN, 0);      // a drag turns it, one title per 22 px
    if (wl.n > 1 && wl.n > wl.slots) {                               // where am I in a long list: a thin rail
        int kh = h * wl.slots / wl.n; if (kh < 10) kh = 10; if (kh > h) kh = h;
        const int ky = y + (h - kh) * wl.sel / (wl.n - 1);
        d.fillRect(x + w - 3, y, 2, h, LINE);
        d.fillRect(x + w - 3, ky, 2, kh, wl.acc);
    }
}

void toggle(int rx, int cy, bool on, bool on_pill)
{
    const int sw = 26, sh = 13, x = rx - sw;
    d.fillRoundRect(x, cy - sh / 2, sw, sh, sh / 2, on ? C_GREEN : (on_pill ? BG : survive(LINE)));
    d.fillCircle(on ? x + sw - sh / 2 - 1 : x + sh / 2 + 1, cy, sh / 2 - 2, on ? INK : (on_pill ? FG : nucleo_theme_ink_on(LINE, MUTED)));
}

void slider(int x, int cy, int w, int pct, unsigned short col, int h)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int r = h / 2 + 3, x0 = x + r, span = w - 2 * r, kx = x0 + span * pct / 100;
    d.fillRoundRect(x0, cy - h / 2, span, h, h / 2, survive(LINE));
    if (kx > x0) d.fillRoundRect(x0, cy - h / 2, kx - x0 + h / 2, h, h / 2, col);
    d.fillCircle(kx, cy, r, col);
    d.fillCircle(kx, cy, r - 2, FG);
}

int slider_at(int tx, int x, int w)
{
    const int r = 6, span = w - 2 * r;
    if (span <= 0) return 0;
    int p = (tx - x - r) * 100 / span;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

void vslider(int cx, int y, int h, int pct, unsigned short col, int w)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int r = w / 2 + 3, y0 = y + r, span = h - 2 * r, ky = y0 + span - span * pct / 100;
    d.fillRoundRect(cx - w / 2, y0, w, span, w / 2, survive(LINE));
    if (ky < y0 + span) d.fillRoundRect(cx - w / 2, ky - w / 2, w, y0 + span - ky + w / 2, w / 2, col);
    d.fillCircle(cx, ky, r, col);
    d.fillCircle(cx, ky, r - 2, FG);
}

int vslider_at(int ty, int y, int h)
{
    const int r = 6, span = h - 2 * r;
    if (span <= 0) return 0;
    int p = 100 - (ty - y - r) * 100 / span;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

struct SetList { setting_fn fn; void *ud; unsigned short acc; };
static void set_row(int i, int x, int y, int w, int h, bool focus, unsigned short, void *p)
{
    const SetList *l = (const SetList *)p;
    Setting s = { "", nullptr, nullptr, ROW_VALUE, -1, -1, false, false, 0, 0, 0 };
    l->fn(i, &s, l->ud);
    setting(x - 4, y, w + 8, h, s, focus, l->acc);
}
int settings_list(int x, int y, int w, int h, int rowh, int n, int sel, int *scroll, setting_fn fn, void *ud, unsigned short acc)
{
    SetList l = { fn, ud, acc };
    return list(x, y, w, h, rowh, n, sel, scroll, set_row, &l, BG);   // the row draws its own pill
}

void setting(int x, int y, int w, int h, const Setting &s, bool foc, unsigned short acc)
{
    const int ch = h - 4, rx = x + w - 12, cy = y + ch / 2;
    const unsigned short bg = foc ? (s.kind == ROW_DANGER ? C_RED : acc) : card(), ink = foc ? INK : (s.dis ? DIM : FG);
    d.fillRoundRect(x, y, w, ch, 9, bg);
    int lx = x + 12;
    if (s.glyph >= 0) {
        ui_glyph(&d, s.glyph, x + 17, cy, 6, foc ? INK : (s.dis ? DIM : (s.lcol ? s.lcol : acc)), bg);
        lx = x + 34;
    }
    // A slider row is two lines — label + value, then the slider — as one block centred in the card, so it
    // looks the same at every row height and focus state.
    const bool two = s.kind == ROW_SLIDER;
    const bool subl = !two && foc && s.sub && s.sub[0];          // focused with a sub line: label on top
    const int ly = two ? y + (ch - 26) / 2 + 9 : subl ? y + 13 : cy;   // the label line's centre (values align on it)
    int vx = rx;
    char v[28];
    switch (s.kind) {
    case ROW_TOGGLE:
        toggle(rx, ly, s.on && !s.dis, foc); vx = rx - 32;
        break;
    case ROW_CYCLE:
        font(1); d.setTextDatum(middle_right); d.setTextColor(ink, bg);
        fit(v, sizeof v, s.value, w / 2); d.drawString(v, rx - 12, ly);
        ui_glyph(&d, UG_NEXT, rx - 4, ly, 4, ink, bg);
        ui_glyph(&d, UG_PREV, rx - 24 - d.textWidth(v), ly, 4, ink, bg);
        vx = rx - 34 - d.textWidth(v);
        break;
    case ROW_NAV: case ROW_DANGER:
        ui_glyph(&d, UG_NEXT, rx, ly, 5, ink, bg); vx = rx - 14;
        break;
    default:                                                     // ROW_VALUE / ROW_SLIDER: a value at the right
        if (s.rglyph >= 0) { ui_glyph(&d, s.rglyph, rx, ly, 5, foc ? INK : (s.vcol ? s.vcol : FG), bg); vx = rx - 16; }
        else if (s.value && s.value[0]) {
            font(1); d.setTextDatum(middle_right); d.setTextColor(foc ? INK : (s.dis ? DIM : (two || !s.vcol ? FG : s.vcol)), bg);
            fit(v, sizeof v, s.value, w * 2 / 3); d.drawString(v, rx, ly);
            vx = rx - d.textWidth(v) - 12;
        }
        break;
    }
    if (two) slider(lx - 3, ly + 12, rx - lx + 3, s.pct, foc ? INK : acc, 4);
    const unsigned short lc = foc ? INK : (s.dis ? DIM : s.kind == ROW_DANGER ? C_RED : (s.lcol ? s.lcol : FG));
    font(1); d.setTextColor(lc, bg);
    char lbl[48]; fit(lbl, sizeof lbl, s.label, vx - lx - 4);
    if (subl) {
        d.setTextDatum(top_left); d.drawString(lbl, lx, y + 5);
        font(0); d.setTextColor(INK, bg);
        char sub[52]; fit(sub, sizeof sub, s.sub, w - (lx - x) - 10);
        d.drawString(sub, lx, y + 23);
    } else {
        d.setTextDatum(middle_left); d.drawString(lbl, lx, ly);
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

// ---- panel screenshots (capture) ----------------------------------------------------------------------------
static char    s_cap_path[96];
static int     s_cap;                                         // capture_state()
static int64_t s_cap_t0;                                      // when it was asked (a frame() never came: fail)
static FILE   *s_cap_f;

void capture(const char *path)
{
    snprintf(s_cap_path, sizeof s_cap_path, "%s", path);
    s_cap = 1; s_cap_t0 = esp_timer_get_time();
}

int capture_state(void)
{
    if (s_cap == 1 && esp_timer_get_time() - s_cap_t0 > 4000000) s_cap = 3;   // this app never composed a frame
    const int st = s_cap;
    if (st >= 2) s_cap = 0;
    return st;
}

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static bool cap_open(int w, int h)
{
    s_cap_f = fopen(s_cap_path, "wb");
    if (!s_cap_f) return false;
    const uint32_t row = (uint32_t)w * 3, img = row * h;          // w * 3 is a multiple of 4 for every panel width
    uint8_t hd[54] = { 'B', 'M' };
    put_le(hd + 2, 54 + img, 4); put_le(hd + 10, 54, 4); put_le(hd + 14, 40, 4);
    put_le(hd + 18, (uint32_t)w, 4); put_le(hd + 22, (uint32_t)-h, 4);   // negative height: rows top-down
    put_le(hd + 26, 1, 2); put_le(hd + 28, 24, 2); put_le(hd + 34, img, 4);
    return fwrite(hd, 1, sizeof hd, s_cap_f) == sizeof hd;
}

// One composed strip -> BGR rows. The canvas holds RGB565 byte-swapped (as the panel wants it) or RGB332.
static bool cap_rows(const uint8_t *buf, int w, int rows, bool exact)
{
    static uint8_t line[320 * 3];
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (exact) {
                const uint8_t *q = buf + ((size_t)y * w + x) * 2;
                const uint16_t v = (uint16_t)(q[0] << 8 | q[1]);
                r = (uint8_t)((v >> 11) * 255 / 31); g = (uint8_t)(((v >> 5) & 63) * 255 / 63); b = (uint8_t)((v & 31) * 255 / 31);
            } else {
                const uint8_t v = buf[(size_t)y * w + x];
                r = (uint8_t)((v >> 5) * 255 / 7); g = (uint8_t)(((v >> 2) & 7) * 255 / 7); b = (uint8_t)((v & 3) * 255 / 3);
            }
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        if (fwrite(line, 1, (size_t)w * 3, s_cap_f) != (size_t)w * 3) return false;
    }
    return true;
}

bool frame(void (*paint)(void *ud), void *ud, int y0, int h, bool exact)
{
    const int W = d.width();
    if (h <= 0) h = d.height() - y0;
    M5Canvas *c = nucleo_screen_wide(W);
    if (!c || c->height() < 8) { paint(ud); return false; }
    LovyanGFX *panel = &d;
    uint8_t *buf = (uint8_t *)c->getBuffer();
    // 8-bit: the canvas as it is (~100 rows of 320). exact: the same bytes as RGB565 (~50 rows), so theme
    // colours land exactly as when drawn straight to the panel — twice the strips, no extra RAM.
    const int bpp = exact ? 2 : 1, rows = c->height() / bpp;
    static M5Canvas s_win;                                   // a window onto the canvas: panel rows [oy, oy+rows)
    static M5Canvas s_out;                                   // the canvas bytes seen at the window's depth (push)
    if (exact) { s_out.setColorDepth(16); s_out.setBuffer(buf, W, rows); }
    M5Canvas *out = exact ? &s_out : c;
    // A pending screenshot rides along on a whole frame (never on a partial repaint of a band).
    bool cap = s_cap == 1 && y0 == 0 && h == d.height() && W <= 320;
    if (cap && !cap_open(W, h)) { cap = false; s_cap = 3; if (s_cap_f) { fclose(s_cap_f); s_cap_f = nullptr; } }
    for (int oy = y0; oy < y0 + h; oy += rows) {
        const int sh = y0 + h - oy < rows ? y0 + h - oy : rows;
        // The window's row oy is the canvas's row 0: a sprite (oy + sh) rows tall whose buffer starts oy rows
        // before the canvas, clipped to its last sh rows, so nothing above the canvas is ever written.
        s_win.setColorDepth(exact ? 16 : c->getColorDepth());
        s_win.setBuffer(buf - (ptrdiff_t)oy * W * bpp, W, oy + sh);
        s_win.setClipRect(0, oy, W, sh);
        panel->waitDMA();                                     // the last strip's push is off the bus: paint() may read the SD
        nucleo_touch_clear();
        nucleo_app_set_gfx(&s_win);
        paint(ud);
        nucleo_app_set_gfx(nullptr);
        panel->setClipRect(0, oy, W, sh);
        out->pushSprite(panel, 0, oy);
        panel->clearClipRect();
        if (cap) {                                            // the SD shares the panel's bus: let the push finish
            panel->waitDMA();
            if (!cap_rows(buf, W, sh, exact)) cap = false, s_cap = 3;
        }
    }
    if (s_cap_f) { const bool ok = fclose(s_cap_f) == 0 && cap; s_cap_f = nullptr; s_cap = ok ? 2 : 3; if (!ok) remove(s_cap_path); }
    return true;
}

void field(int x, int y, int w, int h) { nucleo_touch_add(x, y, w, h, NK_CHAR, NUCLEO_TOUCH_FIELD); }

bool tap(int key, char ch, int *x, int *y)
{
    if (!nucleo_touch_is_field(key, ch)) return false;
    int tx = 0, ty = 0;
    if (!nucleo_touch_last(&tx, &ty)) return false;
    if (x) *x = tx;
    if (y) *y = ty;
    return true;
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
