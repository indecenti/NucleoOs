// Calculator app: type an expression on the Cardputer keyboard, Enter to evaluate.
// The arithmetic lives in calc_eval.c (shared, unit-tested twin in the simulator);
// this file is only the on-screen keypad-free UI + key handling.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "calc_eval.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, ACC = 0x8FF3, ERR = 0xF96B;

static char s_expr[64];
static char s_prev[64];       // expression that produced the shown result (small upper line)
static bool s_error;

// Characters the calculator accepts. The keyboard delivers '.' as NK_DOWN and '/'
// as NK_RIGHT, but both still carry the right ch, so we filter purely on ch.
static bool is_calc_char(char c) { return c && strchr("0123456789+-*/().", c) != nullptr; }

static void enter(void) { nucleo_app_set_direct_draw(true); s_expr[0] = 0; s_prev[0] = 0; s_error = false; nucleo_app_set_hint("0-9 + - * / ( )   enter =   del   esc back"); }   // static UI: draw direct, free the 32 KB menu buffer
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
    if (key == NK_DEL) { int l = strlen(s_expr); if (l) s_expr[l - 1] = 0; s_error = false; s_prev[0] = 0; }
    else if (ch == 'c' || ch == 'C') { s_expr[0] = 0; s_error = false; s_prev[0] = 0; }
    else if (is_calc_char(ch)) {
        if (s_error) { s_expr[0] = 0; s_error = false; }
        else if (s_prev[0]) { if (strchr("0123456789(", ch)) s_expr[0] = 0; s_prev[0] = 0; }  // digit after '=' starts fresh; operator continues
        int l = strlen(s_expr);
        if (l < (int)sizeof(s_expr) - 1) { s_expr[l] = ch; s_expr[l + 1] = 0; }
    } else return;
    nucleo_app_request_draw();
}

static void draw(void)
{
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

extern "C" void nucleo_register_calc(void)
{
    static const nucleo_app_def_t app = {
        "calc", "Calculator", "Office", "Type sums on the keyboard",
        '=', 0x8FF3, enter, on_key, tick, draw, nullptr
    };
    nucleo_app_register(&app);
}
