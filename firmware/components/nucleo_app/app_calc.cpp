// Calculator app: type an expression on the Cardputer keyboard, Enter to evaluate.
// The arithmetic lives in calc_eval.c (shared, unit-tested twin in the simulator);
// this file is only the on-screen keypad-free UI + key handling.
#include "nucleo_app.h"
#include "app_ui.h"
#include "nucleo_i18n.h"       // TR(it,en): hint follows the system language
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "calc_eval.h"
}
#include "nucleo_theme.h"      // THEME_*: the panel view follows the OS theme like the kit's header
#include "nucleo_touch.h"
#include "app_large.h"         // two screens: a touch keypad on the 320x240 panel, the Cardputer as companion

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, ACC = 0x8FF3, ERR = 0xF96B;

static char s_expr[64];
static char s_prev[64];       // expression that produced the shown result (small upper line)
static bool s_error;

// Characters the calculator accepts. The keyboard delivers '.' as NK_DOWN and '/'
// as NK_RIGHT, but both still carry the right ch, so we filter purely on ch.
static bool is_calc_char(char c) { return c && strchr("0123456789+-*/().", c) != nullptr; }

static void enter(void) { nucleo_app_set_direct_draw(true); s_expr[0] = 0; s_prev[0] = 0; s_error = false; nucleo_app_set_hint(TR5("0-9 + - * / ( )  invio =  canc  esc", "0-9 + - * / ( )  enter =  del  esc", "0-9 + - * / ( )  intro =  supr  esc", "0-9 + - * / ( )  entree =  suppr  esc", "0-9 + - * / ( )  eingabe =  entf  esc")); }   // static UI: draw direct, free the 32 KB menu buffer
static void tick(void) {}

static void evaluate(void)
{
    if (!s_expr[0]) return;                 // nothing typed: leave the "0"
    bool ok; double r = calc_eval(s_expr, &ok);
    if (ok) { snprintf(s_prev, sizeof(s_prev), "%s", s_expr); snprintf(s_expr, sizeof(s_expr), "%.10g", r); s_error = false; }
    else    { s_error = true; s_prev[0] = 0; }
    nucleo_app_request_draw();
}

static bool lg_tap_key(int key, char ch);   // the panel's keypad (two screens)
static void on_key(int key, char ch)
{
    if (lg_tap_key(key, ch)) return;
    if (key == NK_ENTER || ch == '=') { evaluate(); return; }
    // NK_LEFT (the ',' key) steps back one character like DEL
    if (key == NK_DEL || key == NK_LEFT) { int l = strlen(s_expr); if (l) s_expr[l - 1] = 0; s_error = false; s_prev[0] = 0; }
    else if (ch == 'c' || ch == 'C') { s_expr[0] = 0; s_error = false; s_prev[0] = 0; }
    else if (is_calc_char(ch)) {
        if (s_error) { s_expr[0] = 0; s_error = false; }
        else if (s_prev[0]) { if (strchr("0123456789(", ch)) s_expr[0] = 0; s_prev[0] = 0; }  // digit after '=' starts fresh; operator continues
        int l = strlen(s_expr);
        if (l < (int)sizeof(s_expr) - 1) { s_expr[l] = ch; s_expr[l + 1] = 0; }
    } else return;
    nucleo_app_request_draw();
}

static void draw_companion(void);

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);   // direct draw: clear the whole content area (no framebuffer wipes it for us)
    app_ui_title("Calculator", ACC, nullptr);

    // small upper line: the source expression after '=', else a gentle prompt
    char upper[72];
    if (s_error) upper[0] = 0;
    else if (s_prev[0]) snprintf(upper, sizeof(upper), "%s =", s_prev);
    else if (!s_expr[0]) snprintf(upper, sizeof(upper), "type a sum, Enter = result");
    else upper[0] = 0;
    if (upper[0]) { d.setTextSize(1); d.setTextColor(MUTED, BG); int ul = strlen(upper); d.setCursor(238 - ul * 6, top + 30); d.print(upper); }

    // big lower line: current input or result, auto-fit, right-aligned
    const char *shown = s_error ? "Error" : (s_expr[0] ? s_expr : "0");
    int len = (int)strlen(shown);
    int size = len <= 8 ? 3 : len <= 16 ? 2 : 1;
    int chw = size * 6;
    int x = 240 - 10 - len * chw; if (x < 6) x = 6;
    int y = top + h - (size == 3 ? 30 : size == 2 ? 22 : 14);
    d.setTextSize(size); d.setTextColor(s_error ? ERR : FG, BG);
    d.setCursor(x, y); d.print(shown);
}


// ===== two screens ==============================================================================================
// The panel is the calculator: a tall display on top (the expression that gave the result above the big value, the
// back chevron at its left) and a 4x5 keypad filling the rest — C ( ) / , the digits with * - + beside them, and
// 0 . erase = at the bottom — so every key of the Cardputer has its key here. The whole surface is one field the
// app hit-tests (no 16-target limit, no gap that could fall into the panel's zones); the chevron alone is a
// target of its own. Composed with lg::frame: a key press never flickers. The Cardputer shows what is typed.
static const int LG_DISP_H = 76, LG_ROWS = 5, LG_COLS = 4;
static const char LG_KEYS[LG_ROWS * LG_COLS + 1] = "C()/789*456-123+0.<=";   // '<' = erase one character

static void lg_glyph(char k, int cx, int cy, unsigned short c)   // the signs, drawn (the GFX fonts are ASCII)
{
    switch (k) {
    case '+': d.fillRect(cx - 9, cy - 1, 19, 3, c); d.fillRect(cx - 1, cy - 9, 3, 19, c); break;
    case '-': d.fillRect(cx - 9, cy - 1, 19, 3, c); break;
    case '*': for (int o = -1; o <= 1; o++) { d.drawLine(cx - 7 + o, cy - 7, cx + 7 + o, cy + 7, c); d.drawLine(cx - 7 + o, cy + 7, cx + 7 + o, cy - 7, c); } break;
    case '/': d.fillRect(cx - 9, cy - 1, 19, 3, c); d.fillCircle(cx, cy - 7, 2, c); d.fillCircle(cx, cy + 7, 2, c); break;
    case '=': d.fillRect(cx - 9, cy - 5, 19, 3, c); d.fillRect(cx - 9, cy + 3, 19, 3, c); break;
    case '.': d.fillCircle(cx, cy + 6, 3, c); break;
    case '<':                                                    // the erase key: a tag with an x
        for (int o = 0; o < 2; o++) { d.drawLine(cx - 13, cy + o, cx - 6, cy - 7 + o, c); d.drawLine(cx - 13, cy - o, cx - 6, cy + 7 - o, c); }
        d.drawRect(cx - 6, cy - 7, 19, 15, c);
        d.drawLine(cx - 1, cy - 3, cx + 7, cy + 3, c); d.drawLine(cx - 1, cy + 3, cx + 7, cy - 3, c); break;
    default: {                                                   // digits, C and the parentheses: the big face
        char t[2] = { k, 0 };
        lg::font(2); d.setTextDatum(middle_center); d.setTextColor(c); d.drawString(t, cx, cy + 1); d.setTextDatum(top_left);
    }
    }
}

static void lg_paint(void *)
{
    const int W = 320, H = 240;
    d.fillRect(0, 0, W, H, THEME_BG);
    // the display, with the back chevron at its left
    const int x = 6, y = 4, w = W - 12, h = LG_DISP_H - 8;
    d.fillRoundRect(x, y, w, h, 10, THEME_INK);
    d.drawRoundRect(x, y, w, h, 10, s_error ? ERR : THEME_LINE);
    for (int o = 0; o < 2; o++) { d.drawLine(x + 18 + o, y + 12, x + 11 + o, y + 19, ACC); d.drawLine(x + 11 + o, y + 19, x + 18 + o, y + 26, ACC); }
    nucleo_touch_add(0, 0, 48, 40, NK_BACK, 0);
    char upper[72];                                              // the expression that gave the result, or a prompt
    if (s_error) snprintf(upper, sizeof upper, "%s", TR("espressione non valida", "invalid expression"));
    else if (s_prev[0]) snprintf(upper, sizeof upper, "%s =", s_prev);
    else if (!s_expr[0]) snprintf(upper, sizeof upper, "%s", TR("tocca i tasti o scrivi", "tap the keys or type"));
    else upper[0] = 0;
    lg::font(0); d.setTextDatum(top_right); d.setTextColor(s_error ? ERR : THEME_MUTED, THEME_INK);
    char u[72]; lg::fit(u, sizeof u, upper, w - 50); d.drawString(u, x + w - 12, y + 6);
    const char *shown = s_error ? TR("Errore", "Error") : (s_expr[0] ? s_expr : "0");
    const int bw = w - 44;                                       // the biggest face it fits in, else the tail of it
    int f = 3; lg::font(f);
    while (f > 0 && d.textWidth(shown) > bw) lg::font(f = f == 3 ? 2 : 0);
    char big[72];
    if (d.textWidth(shown) > bw) { const char *t = shown; while (*t && d.textWidth(t) > bw - 12) t++; snprintf(big, sizeof big, "..%s", t); }
    else snprintf(big, sizeof big, "%s", shown);
    d.setTextDatum(bottom_right); d.setTextColor(s_error ? ERR : THEME_FG, THEME_INK);
    d.drawString(big, x + w - 12, y + h - (f == 0 ? 10 : 6));
    d.setTextDatum(top_left);
    // the keypad
    const int cw = W / LG_COLS, rh = (H - LG_DISP_H) / LG_ROWS;
    for (int i = 0; i < LG_ROWS * LG_COLS; i++) {
        const char k = LG_KEYS[i];
        const int kx = (i % LG_COLS) * cw, ky = LG_DISP_H + (i / LG_COLS) * rh;
        const bool op = strchr("()/*-+", k) != nullptr, eq = k == '=';
        d.fillRoundRect(kx + 3, ky + 3, cw - 6, rh - 6, 10, eq ? ACC : THEME_LINE);
        lg_glyph(k, kx + cw / 2, ky + rh / 2, eq ? THEME_INK : k == 'C' ? ERR : op ? ACC : THEME_FG);
    }
    lg::field(0, 0, W, H);                                       // everything but the chevron: hit-tested in on_key
}

static void draw_large(void) { lg::frame(lg_paint, nullptr); }

// A tap on the panel: the key under it, through the keyboard's own path.
static bool lg_tap_key(int key, char ch)
{
    int x, y;
    if (!lg::tap(key, ch, &x, &y)) return false;
    if (y < LG_DISP_H) return true;                              // the display: nothing to press
    const int c = x * LG_COLS / 320, r = (y - LG_DISP_H) * LG_ROWS / (240 - LG_DISP_H);
    const char k = LG_KEYS[(r < LG_ROWS ? r : LG_ROWS - 1) * LG_COLS + (c < LG_COLS ? c : LG_COLS - 1)];
    if (k == '=') on_key(NK_ENTER, 0);
    else if (k == '<') on_key(NK_DEL, 0);
    else on_key(NK_CHAR, k == 'C' ? 'c' : k);
    return true;
}

// The Cardputer while the panel shows the keypad: what is being typed, large, and its keys.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, THEME_BG);
    int y = lg::comp_head(TR("CALCOLATRICE", "CALCULATOR"), ACC);
    const char *shown = s_error ? TR("Errore", "Error") : (s_expr[0] ? s_expr : "0");
    d.setFont(&fonts::Font0);
    if (s_prev[0] && !s_error) { d.setTextColor(THEME_MUTED, THEME_BG); char u[48]; snprintf(u, sizeof u, "%.36s =", s_prev); d.drawString(u, 8, y + 2); }
    const int len = (int)strlen(shown), sz = len <= 12 ? 3 : len <= 18 ? 2 : 1;
    const char *tail = len * 6 * sz > 224 ? shown + len - 224 / (6 * sz) : shown;
    d.setTextSize(sz); d.setTextColor(s_error ? ERR : THEME_FG, THEME_BG);
    d.setCursor(8, y + 16); d.print(tail); d.setTextSize(1);
    lg::comp_keys(TR("INVIO =  CANC o , cancella  c azzera", "ENTER =  DEL or , erase  c clear"));
}

extern "C" void nucleo_register_calc(void)
{
    static const nucleo_app_def_t app = {
        "calc", "Calculator", "Office", "Type sums on the keyboard",
        '=', 0x8FF3, enter, on_key, tick, draw, nullptr, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
