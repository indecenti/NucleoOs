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

static void on_key(int key, char ch)
{
    if (key == NK_ENTER || ch == '=') { evaluate(); return; }
    // NK_LEFT (the ',' key, or a tap on the left of the panel's display) steps back one character like DEL
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
// The panel is the calculator: the display on top (the expression that gave the result above it), a touch keypad
// below — digits on a 3x4 grid at the left, the operators and '=' in a column at the right. The touch table holds
// 16 targets, exactly the 16 keys; the display itself is left to the panel's own zones, which fit it: its left
// part is NK_LEFT (= erase one character here, drawn as the erase glyph), its centre is Enter (= result), and the
// header band is Back. Every key target tiles its cell edge to edge, so no tap can fall into a gap — the right
// zone's fallback would type '/'. The Cardputer shows what is being typed and the keys that have no touch key.
static const int LG_DISP_Y = lg::HEAD + 4, LG_DISP_H = 60, LG_PAD_Y = LG_DISP_Y + LG_DISP_H + 4, LG_OPS_X = 224;

static void lg_glyph(char k, int cx, int cy, unsigned short c)   // the operator signs, drawn (the GFX fonts are ASCII)
{
    switch (k) {
    case '+': d.fillRect(cx - 9, cy - 1, 19, 3, c); d.fillRect(cx - 1, cy - 9, 3, 19, c); break;
    case '-': d.fillRect(cx - 9, cy - 1, 19, 3, c); break;
    case '*': for (int o = -1; o <= 1; o++) { d.drawLine(cx - 7 + o, cy - 7, cx + 7 + o, cy + 7, c); d.drawLine(cx - 7 + o, cy + 7, cx + 7 + o, cy - 7, c); } break;
    case '/': d.fillRect(cx - 9, cy - 1, 19, 3, c); d.fillCircle(cx, cy - 7, 2, c); d.fillCircle(cx, cy + 7, 2, c); break;
    case '=': d.fillRect(cx - 9, cy - 5, 19, 3, c); d.fillRect(cx - 9, cy + 3, 19, 3, c); break;
    }
}

// One key: the cell [x, x+w) x [y, y+h) is its touch target, the button is drawn inset by 3 px.
static void lg_key(int x, int y, int w, int h, char k, unsigned short fill, unsigned short ink, int ty = -1)
{
    d.fillRect(x, y, w, h, THEME_BG);
    d.fillRoundRect(x + 3, y + 3, w - 6, h - 6, 10, fill);
    if (k >= '0' && k <= '9') {
        char t[2] = { k, 0 };
        lg::font(2); d.setTextDatum(middle_center); d.setTextColor(ink, fill);
        d.drawString(t, x + w / 2, y + h / 2 + 1); d.setTextDatum(top_left);
    } else if (k == '.') d.fillCircle(x + w / 2, y + h / 2 + 6, 3, ink);
    else lg_glyph(k, x + w / 2, y + h / 2, ink);
    const int yt = ty < 0 ? y : ty;                          // a target may reach above its cell (the header's foot)
    nucleo_touch_add(x, yt, w, y + h - yt, k == '=' ? NK_ENTER : NK_CHAR, k == '=' ? 0 : k);
}

static void lg_display(void)
{
    const int x = 8, w = LG_OPS_X - 12, y = LG_DISP_Y, h = LG_DISP_H;
    d.fillRect(0, y - 4, LG_OPS_X, h + 8, THEME_BG);
    d.fillRoundRect(x, y, w, h, 10, THEME_INK);
    d.drawRoundRect(x, y, w, h, 10, s_error ? ERR : THEME_LINE);
    // the erase glyph at the left: the panel's left zone delivers NK_LEFT there, which erases one character
    const int ex = x + 10, ey = y + h - 22;
    d.drawLine(ex, ey + 7, ex + 7, ey, THEME_MUTED); d.drawLine(ex, ey + 7, ex + 7, ey + 14, THEME_MUTED);
    d.drawRect(ex + 7, ey, 18, 15, THEME_MUTED);
    d.drawLine(ex + 12, ey + 4, ex + 19, ey + 10, THEME_MUTED); d.drawLine(ex + 12, ey + 10, ex + 19, ey + 4, THEME_MUTED);
    char upper[72];                                          // the expression that gave the result, or a prompt
    if (s_error) snprintf(upper, sizeof upper, "%s", TR("espressione non valida", "invalid expression"));
    else if (s_prev[0]) snprintf(upper, sizeof upper, "%s =", s_prev);
    else if (!s_expr[0]) snprintf(upper, sizeof upper, "%s", TR("tocca i tasti o scrivi", "tap the keys or type"));
    else upper[0] = 0;
    lg::font(0); d.setTextDatum(top_right); d.setTextColor(s_error ? ERR : THEME_MUTED, THEME_INK);
    char u[72]; lg::fit(u, sizeof u, upper, w - 20); d.drawString(u, x + w - 10, y + 6);
    const char *shown = s_error ? TR("Errore", "Error") : (s_expr[0] ? s_expr : "0");
    char big[72];                                            // the biggest face it fits in, else the tail of it
    const int bw = w - 52;
    int f = 3; lg::font(f);
    while (f > 0 && d.textWidth(shown) > bw) lg::font(f = f == 3 ? 2 : 0);
    if (d.textWidth(shown) > bw) {                           // still too long: keep the end (the part being typed)
        const char *t = shown; while (*t && d.textWidth(t) > bw - 12) t++;
        snprintf(big, sizeof big, "..%s", t);
    } else snprintf(big, sizeof big, "%s", shown);
    d.setTextDatum(bottom_right); d.setTextColor(s_error ? ERR : THEME_FG, THEME_INK);
    d.drawString(big, x + w - 10, y + h - (f == 0 ? 10 : 6));
    d.setTextDatum(top_left);
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    lg::header(TR("Calcolatrice", "Calculator"), TR("tocca qui: esci", "tap here: exit"), ACC, 0);
    lg_display();
    // the digits: a 3x4 grid filling [0, LG_OPS_X) x [LG_PAD_Y, H)
    static const char grid[12] = { '7', '8', '9', '4', '5', '6', '1', '2', '3', '.', '0', '0' };
    const int cw = LG_OPS_X / 3, rh = (H - LG_PAD_Y) / 4;
    const unsigned short kf = lg::card();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) {
            const char k = grid[r * 3 + c];
            if (r == 3 && c == 2) continue;                  // '0' is two cells wide
            const int y = LG_PAD_Y + r * rh, hh = r == 3 ? H - y : rh;
            lg_key(c * cw, y, (r == 3 && c == 1) ? LG_OPS_X - cw : cw, hh, k, kf, THEME_FG);
        }
    // the operators and '=': one column at the right, from the header's foot to the bottom
    static const char ops[5] = { '/', '*', '-', '+', '=' };
    const int oh = (H - lg::HEAD) / 5;
    for (int i = 0; i < 5; i++) {
        const int y = lg::HEAD + i * oh, hh = i == 4 ? H - y : oh;
        const bool eq = ops[i] == '=';
        lg_key(LG_OPS_X, y, W - LG_OPS_X, hh, ops[i], eq ? ACC : THEME_LINE, eq ? THEME_INK : ACC, i == 0 ? 24 : -1);
    }
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the keypad: what is being typed, large, and the keys only it has.
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
    d.setTextColor(THEME_MUTED, THEME_BG);
    d.drawString(TR("( ) e c (azzera) solo da qui", "( ) and c (clear) only here"), 8, y + 54);
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
