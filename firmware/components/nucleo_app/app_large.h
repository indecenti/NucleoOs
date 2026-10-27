// Two-screen kit: the shared look of an app on the external 320x240 touch panel (its on_draw_large) and of its
// Cardputer companion (its on_draw while nucleo_app_two_screens()). CLAUDE.md: "Two screens: never a mirror".
//
// Everything draws straight to `d` (during on_draw_large that IS the panel: no buffer), so every helper paints its
// whole box over itself — a frame is never cleared first. Touch targets are registered in panel coordinates as the
// content is drawn: the app brackets on_draw_large with nucleo_touch_clear() / nucleo_touch_commit().
//
//   static void draw_large(void) {
//       nucleo_touch_clear();
//       int y = lg::header("Notes", "12", ACC);                       // the back chevron closes the app
//       lg::text_list(0, y, d.width(), d.height() - y, s_n, s_sel, &s_lg_scroll, label, right, nullptr, nullptr, ACC);
//       nucleo_touch_commit();
//   }
//   on_key:  switch (lg::row_tap(key, ch, s_lg_scroll, s_n, &s_sel)) { case 1: redraw; return; case 2: open(); return; }
//
// Games draw their large view NATIVELY for 320x240 (lg::frame for animated screens, lg::field / nucleo_touch_add_pad
// for direct touch on the playfield); lg::game() — the compact frame scaled up — is a mirror (CLAUDE.md) and only
// stays for apps not converted yet. The Cardputer shows the game's companion (score, lives, keys).
#pragma once
#include <stddef.h>
#include "app_ui.h"     // app_ui_text_fn / app_ui_color_fn row providers

namespace lg {

enum { HEAD = 30, ROW = 26, PAD = 10 };

unsigned short card(void);                    // card fill for the active theme (a step off BG)
void fit(char *out, size_t n, const char *s, int maxw);                    // cut with ".." to maxw px (current font)
int  wrap(const char *s, int x, int y, int maxw, int lh, int maxl);        // word-wrap; returns the lines drawn

// Fonts on the panel: 0 = Font2 (16 px, rows / body), 1 = FreeSansBold9pt (titles), 2 = FreeSansBold12pt (big
// values), 3 = FreeSansBold18pt (hero numbers), 4 = Font0 (8 px, captions only).
void font(int f);

// The header band [0, HEAD): a back chevron at the left (tap -> back_key; 0 = none), the title in `acc`, a muted
// `right` text, and a menu glyph at the right when `menu_key` != 0 (tap -> that key, usually NK_TAB). Returns HEAD.
int header(const char *title, const char *right, unsigned short acc, int back_key = 6 /* NK_BACK */, int menu_key = 0);
// A row of tabs (pills) at y, full width, 28 px tall; tab i taps as NK_CHAR keys[i] (or NK_TAB when keys is null:
// one TAB press per step is the app's job then, so prefer keys). Returns the first y below.
int tabs(int y, const char *const *names, int n, int active, unsigned short acc, const char *keys);

// A list in [x, x+w) x [y, y+h): `rowh`-tall rows, the selection on an `acc` pill, kept in view through *scroll.
// Every visible row registers a touch target NUCLEO_TOUCH_ROW + slot (slot = index - *scroll), and the band is a
// scroll area (a drag walks the selection: NK_UP / NK_DOWN). A scroll knob shows when it does not all fit.
// fn draws one row's content over the already-painted pill/background; `bg` is the colour under it.
typedef void (*row_fn)(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *ud);
int  list(int x, int y, int w, int h, int rowh, int n, int sel, int *scroll, row_fn fn, void *ud, unsigned short acc);
// The common row: label left (Font2), `right` muted at the right edge, label in `col` when not focused.
void row_text(int x, int y, int w, int h, bool focus, unsigned short bg, const char *label, const char *right, unsigned short col);
// list() with app_ui-style providers (label required; right / color may be null).
int  text_list(int x, int y, int w, int h, int n, int sel, int *scroll, app_ui_text_fn label, app_ui_text_fn right,
               app_ui_color_fn color, void *ud, unsigned short acc, int rowh = ROW);
// A tapped list row (from list()): 1 = it is now the selection, 2 = it already was (open it), 0 = not a row key.
int  row_tap(int key, char ch, int scroll, int n, int *sel);

// A rounded button with a centred label; tapping it delivers (key, ch).
void button(int x, int y, int w, int h, const char *label, unsigned short fill, unsigned short ink, int key, char ch);
// A rounded card fill (lg::card() colour by default).
void panel(int x, int y, int w, int h, unsigned short fill = 0);
// "label .......... value" on one Font2 line, value right-aligned in vcol.
void kv(int x, int y, int w, const char *k, const char *v, unsigned short vcol, unsigned short bg = 0xFFFF);
// A gauge: a LINE track with pct (0..100) filled in col.
void bar(int x, int y, int w, int h, int pct, unsigned short col);
// A centred message in a box (empty states, "settings open on the Cardputer"): a head line + a muted sub line.
void message(int x, int y, int w, int h, const char *head, const char *sub, unsigned short col);

// ---- shared controls: ONE look for every app — use these, never redraw a switch / slider / settings row ----
// A switch whose right edge is rx, centred on cy (on = GOOD track). on_pill: drawn on an acc-coloured pill.
void toggle(int rx, int cy, bool on, bool on_pill = false);
// A slider in [x, x+w) centred on cy: a rounded track, the filled part in col and a round knob at pct (0..100).
// h = track thickness (the knob is h + 6 tall). Tap it through a field and map the tap with slider_at().
void slider(int x, int cy, int w, int pct, unsigned short col, int h = 6);
int  slider_at(int tx, int x, int w);                          // the pct a tap at tx means on that slider
// The same, upright (a volume column, a gain fader): in [y, y+h) centred on cx, 100 % at the top.
void vslider(int cx, int y, int h, int pct, unsigned short col, int w = 6);
int  vslider_at(int ty, int y, int h);
// A settings / options row — the look of Settings, for every app's options list: a card at (x, y, w, h - 4),
// focused = an acc pill; an optional ui_glyph at the left; the label; the control at the right:
//   ROW_NAV    chevron (opens a page)          ROW_VALUE  a value text (vcol), or the glyph rglyph
//   ROW_TOGGLE a switch (on)                   ROW_SLIDER the value text + a slider (pct) under the label
//   ROW_CYCLE  < value > (left/right cycles)   ROW_DANGER label in BAD, chevron
// A focused non-slider row with a sub line shows the label on top and sub below it. Register the row's
// touch target yourself (row targets or a field).
enum { ROW_NAV = 0, ROW_VALUE, ROW_TOGGLE, ROW_SLIDER, ROW_CYCLE, ROW_DANGER };
struct Setting {
    const char *label, *sub, *value;   // sub / value may be null
    unsigned char kind;
    signed char glyph, rglyph;         // ui_glyph ids, -1 = none
    bool on, dis;                      // switch state · unavailable (dim)
    int pct;                           // slider position 0..100
    unsigned short lcol, vcol;         // label colour when not focused (0 = FG) · value colour (0 = FG)
};
void setting(int x, int y, int w, int h, const Setting &s, bool focus, unsigned short acc);
// A scrolling list of settings rows (lg::list underneath: row targets + drag scroll + tap / tap again):
// fn fills row i's Setting (pre-set: kind ROW_VALUE, glyphs -1, everything else 0).
typedef void (*setting_fn)(int i, Setting *out, void *ud);
int  settings_list(int x, int y, int w, int h, int rowh, int n, int sel, int *scroll, setting_fn fn, void *ud, unsigned short acc);

// ---- the Cardputer companion (240x135, drawn in on_draw while nucleo_app_two_screens()) ----------------------
// The top line: "<APP>  -  on the second screen" in acc. Returns the first y below it.
int  comp_head(const char *app, unsigned short acc);
// A muted key-hint line at the bottom of the content area.
void comp_keys(const char *keys);

// ---- games -----------------------------------------------------------------------------------------------------
// A whole animated frame, composed off-screen: paint() draws the frame in PANEL coordinates into `d` — which is then
// the shared canvas seen through a window (320 wide, ~100 rows at a time) — once per strip, and every finished strip
// is pushed in one blit, so the panel never shows a cleared-then-redrawn frame. No extra RAM. Inside paint() `d` is
// buffered (tile_blit / stipple work) and d.width() is the panel's; use the [y0, y0+h) band, never d.height().
// The touch set is cleared before every strip, so register the frame's targets inside paint() (or after frame()).
// The window's rows above the strip lie BEFORE the canvas in memory and only its clip keeps them untouched: inside
// paint() never call clearClipRect (a narrower setClipRect is fine if the strip's clip is restored after it), and
// never drawWideLine / drawWedgeLine / drawGradientLine: they replace the clip with the whole sprite and would
// write outside the canvas.
// Without a canvas (fragmented heap) paint() runs once straight onto the panel. Returns true when composed.
// exact = true composes in RGB565 instead (≈50-row strips, ~5 per frame): theme colours exactly as drawn
// straight to the panel — use it for tool apps with themed cards / rows (on the 8-bit canvas dark greys turn
// olive); keep the 8-bit default for games (tile_blit / stipple need it, and it is faster).
bool frame(void (*paint)(void *ud), void *ud, int y0 = 0, int h = 0 /* 0 = to the bottom */, bool exact = false);
// A field: one target over [x, x+w) x [y, y+h) whose taps the app hit-tests itself (nucleo_touch.h).
void field(int x, int y, int w, int h);
// The tap that delivered (key, ch), if it was a field tap: its panel position. False for anything else.
bool tap(int key, char ch, int *x, int *y);

// The game's frame on the panel: draw_game() (the game's compact renderer) runs into the shared canvas, which is
// pushed scaled to the panel's width at the top; the band below it is the HUD: hud(y, h) paints it (or a plain
// band with the game's name when null) and registers its touch targets. Call between touch clear/commit.
void game(void (*draw_game)(void), void (*hud)(int y, int h), const char *name, unsigned short acc);

}  // namespace lg
