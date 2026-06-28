// Livella PRO — Hardware section. A complete, customizable spirit level on the BMI270 (Cardputer ADV).
// Multiple views (TAB cycles): Mira (bullseye), Tubo (classic horizontal vial), Piombo (vertical vial),
// Digitale (big X/Y/total), Impostazioni (settings). Calibrate/zero with the central G key (a centre key
// so the press doesn't nudge the reading). Settings let you flip the X/Y axes (fix sign on YOUR unit
// without a reflash), pick units (°/%), set the "in bolla" tolerance, and hold a reading. ADV-only.
// Level MATH shared with the Goniometro (nucleo_imu_level → lx,ly = screen-plane gravity, deg = tilt).
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "app_gfx.h"

#define RAD2DEG 57.29578f

enum { V_BULLSEYE, V_TUBE_H, V_TUBE_V, V_DIGITAL, V_SETTINGS, V_COUNT };
static const char *VIEW_NAME[V_COUNT] = { "Mira", "Tubo", "Piombo", "Digitale", "Impostazioni" };

static int   s_view;
static float s_lx, s_ly, s_deg;     // smoothed in-plane gravity + tilt-from-flat
static float s_ozx, s_ozy;          // calibration zero (subtracted from lx/ly)
static bool  s_hold;                // freeze the reading
static float s_hx, s_hy, s_hdeg;    // held snapshot

// customizable settings (persist within a boot; reset on reboot)
static struct { int units; bool flipx, flipy; float tol; } s_cfg = { 0, false, false, 0.4f };
static const char *UNIT_NAME[2] = { "gradi", "percento" };
static int s_set_sel;               // selected settings row

// ---- helpers ------------------------------------------------------------------------------------
static unsigned short dim(unsigned short c, int pct)
{
    int r = ((c >> 11) & 31) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
    return (unsigned short)((r << 11) | (g << 5) | b);
}
// format an inclination (degrees in) per the unit setting into buf; returns the suffix glyph kind
static void fmt_val(float deg, char *buf, int n)
{
    if (s_cfg.units == 1) snprintf(buf, n, "%.1f", (double)(tanf(deg / RAD2DEG) * 100.0f));  // grade %
    else                  snprintf(buf, n, "%.1f", (double)deg);
}
// big centred number with auto-fit size; draws a degree ring or % sign after it
static void big_num(int x, int y, float deg, unsigned short col, int maxw)
{
    char b[16]; fmt_val(deg, b, sizeof(b));
    int sz = 4; while (sz > 1 && (int)strlen(b) * 6 * sz + 8 * sz > maxw) sz--;
    d.setTextSize(sz); d.setTextColor(col, BG);
    d.setCursor(x, y); d.print(b);
    int tw = (int)strlen(b) * 6 * sz;
    if (s_cfg.units == 1) { d.setCursor(x + tw + 2, y); d.print("%"); }
    else d.drawCircle(x + tw + 3 + sz, y + 3, sz, col);   // degree ring scaled to text size
}

static void  axes(float *ax, float *ay);   // fwd decl (defined below; poll uses the calibrated axes)
static float deg_now(void);

// Redraw GATE (ANTI-FLICKER): the IMU EMA jitters by sub-degree noise, so a raw delta test repaints
// the buffered frame ~50x/s and the bubble/number shimmer on the panel. Read the sensor every loop but
// request a blit ONLY when a pixel/digit the user can actually see would move — quantize the in-plane
// gravity to ~1 px of bubble travel and the tilt to the 0.1 deg readout. A still device → static frame.
static int s_qx = INT32_MIN, s_qy, s_qd;
static bool poll(void)
{
    if (s_hold || s_view == V_SETTINGS) return false;     // frozen / settings list: nothing live to animate
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    float ax, ay; axes(&ax, &ay);
    int qx = (int)lroundf(ax * 120.0f);                   // ~1 px of bubble travel across the vial
    int qy = (int)lroundf(ay * 120.0f);
    int qd = (int)lroundf(deg_now() * 10.0f);             // 0.1 deg — the readout's resolution
    if (qx == s_qx && qy == s_qy && qd == s_qd) return false;
    s_qx = qx; s_qy = qy; s_qd = qd;
    return true;
}

// current axes after calibration + flip
static void axes(float *ax, float *ay)
{
    float x = (s_hold ? s_hx : s_lx) - s_ozx, y = (s_hold ? s_hy : s_ly) - s_ozy;
    if (s_cfg.flipx) x = -x;
    if (s_cfg.flipy) y = -y;
    *ax = x; *ay = y;
}
static float deg_now(void) { return s_hold ? s_hdeg : s_deg; }

// ---- views --------------------------------------------------------------------------------------
static void view_bullseye(int top, int h, int y0)
{
    float ax, ay; axes(&ax, &ay);
    float m = sqrtf(ax * ax + ay * ay); if (m > 1) m = 1;
    float deg = asinf(m) * RAD2DEG;
    bool level = deg < s_cfg.tol;

    int bottom = top + h;
    // vial on the LEFT, big number on the RIGHT (use the landscape width)
    int vcx = 66, vcy = (y0 + bottom) / 2;
    int R = (bottom - y0) / 2 - 2;
    if (R > 56) R = 56;
    if (R < 22) R = 22;

    // graduated rings + crosshair
    d.drawCircle(vcx, vcy, R, MUTED);
    d.drawCircle(vcx, vcy, R * 2 / 3, dim(MUTED, 60));
    d.drawCircle(vcx, vcy, R / 3, dim(MUTED, 60));
    d.drawLine(vcx - R, vcy, vcx + R, vcy, dim(MUTED, 45));
    d.drawLine(vcx, vcy - R, vcx, vcy + R, dim(MUTED, 45));
    int rT = R / 6; if (rT < 5) rT = 5;
    d.drawCircle(vcx, vcy, rT, level ? C_GREEN : C_GREY);

    // bubble to the HIGH side (opposite gravity), clamped inside the vial
    float span = R * 2.1f, ox = -ax * span, oy = -ay * span;
    float dd = sqrtf(ox * ox + oy * oy), lim = (float)(R - 10);
    if (dd > lim && dd > 0.001f) { ox = ox * lim / dd; oy = oy * lim / dd; }
    int bx = vcx + (int)ox, by = vcy + (int)oy;
    d.fillCircle(bx, by, 10, level ? C_GREEN : C_YELLOW);
    d.drawCircle(bx, by, 10, INK);
    d.fillCircle(bx - 3, by - 3, 2, 0xFFFF);

    // big number beside it on the right
    int nx = 138;
    big_num(nx, vcy - 20, deg, level ? C_GREEN : FG, W - nx - 4);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    char bx2[24]; snprintf(bx2, sizeof(bx2), "X %+5.1f", (double)(asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG));
    d.setCursor(nx, vcy + 16); d.print(bx2);
    snprintf(bx2, sizeof(bx2), "Y %+5.1f", (double)(asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG));
    d.setCursor(nx, vcy + 28); d.print(bx2);
    if (level) { d.setTextColor(C_GREEN, BG); d.setCursor(nx, vcy + 42); d.print("IN BOLLA"); }
}

// a horizontal/vertical capillary vial with a bubble driven by `t` in [-1..1] (0 = centred)
static void capillary(int x, int y, int len, int thick, bool horiz, float t, bool level)
{
    if (t > 1) t = 1;
    if (t < -1) t = -1;
    unsigned short liquid = dim(C_GREEN, level ? 60 : 26);
    if (horiz) {
        d.fillRoundRect(x, y, len, thick, thick / 2, INK);
        d.fillRoundRect(x + 2, y + 2, len - 4, thick - 4, (thick - 4) / 2, liquid);
        int cxm = x + len / 2;
        d.drawLine(cxm - thick / 2, y, cxm - thick / 2, y + thick, dim(FG, 70));
        d.drawLine(cxm + thick / 2, y, cxm + thick / 2, y + thick, dim(FG, 70));
        int bx = cxm + (int)(t * (len / 2 - thick));
        int br = thick / 2 - 2;
        d.fillCircle(bx, y + thick / 2, br, level ? C_GREEN : C_YELLOW);
        d.fillCircle(bx - 2, y + thick / 2 - 2, 2, 0xFFFF);
    } else {
        d.fillRoundRect(x, y, thick, len, thick / 2, INK);
        d.fillRoundRect(x + 2, y + 2, thick - 4, len - 4, (thick - 4) / 2, liquid);
        int cym = y + len / 2;
        d.drawLine(x, cym - thick / 2, x + thick, cym - thick / 2, dim(FG, 70));
        d.drawLine(x, cym + thick / 2, x + thick, cym + thick / 2, dim(FG, 70));
        int by = cym + (int)(t * (len / 2 - thick));
        int br = thick / 2 - 2;
        d.fillCircle(x + thick / 2, by, br, level ? C_GREEN : C_YELLOW);
        d.fillCircle(x + thick / 2 - 2, by - 2, 2, 0xFFFF);
    }
}

static void view_tube_h(int top, int h, int y0)
{
    float ax, ay; axes(&ax, &ay);
    float angle = asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG;     // long-edge inclination
    bool level = fabsf(angle) < s_cfg.tol;
    int bottom = top + h, mid = (y0 + bottom) / 2;
    int thick = 34, len = W - 16;
    capillary(8, mid - 6 - thick / 2, len, thick, true, ax * 3.0f, level);
    big_num(8, mid + 18, fabsf(angle), level ? C_GREEN : FG, W - 16);
}

static void view_tube_v(int top, int h, int y0)
{
    float ax, ay; axes(&ax, &ay);
    float angle = asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG;     // short-edge inclination (plumb)
    bool level = fabsf(angle) < s_cfg.tol;
    int bottom = top + h;
    int thick = 34, len = bottom - y0 - 6;
    capillary(20, y0 + 3, len, thick, false, ay * 3.0f, level);
    big_num(70, (y0 + bottom) / 2 - 16, fabsf(angle), level ? C_GREEN : FG, W - 74);
}

static void view_digital(int top, int h, int y0)
{
    float ax, ay; axes(&ax, &ay);
    float aX = asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG;
    float aY = asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG;
    float tot = deg_now();
    bool level = tot < s_cfg.tol;
    int bottom = top + h, third = (bottom - y0) / 3;
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(8, y0 + 4);  d.print("X (lungo)");
    d.setCursor(8, y0 + third + 2); d.print("Y (corto)");
    d.setCursor(8, y0 + 2 * third); d.print("TOTALE");
    big_num(96, y0 + 2,            fabsf(aX), fabsf(aX) < s_cfg.tol ? C_GREEN : FG, W - 100);
    big_num(96, y0 + third,        fabsf(aY), fabsf(aY) < s_cfg.tol ? C_GREEN : FG, W - 100);
    big_num(96, y0 + 2 * third - 2, tot,      level ? C_GREEN : C_BLUE, W - 100);
}

// Settings rows rendered with the SHARED focused-list widget (app_ui_list) — same Wear-OS look as
// Files/Music/Photos, so it scrolls instead of overflowing and matches every other app's lists.
#define SET_ROWS 5
static const char *set_label(int i, void *)
{
    static const char *L[SET_ROWS] = { "Unita", "Inverti X", "Inverti Y", "Tolleranza", "Azzera calib." };
    return (i >= 0 && i < SET_ROWS) ? L[i] : "";
}
static const char *set_right(int i, void *)
{
    static char b[12];
    switch (i) {
        case 0: return s_cfg.units == 1 ? "%" : "gradi";
        case 1: return s_cfg.flipx ? "si" : "no";
        case 2: return s_cfg.flipy ? "si" : "no";
        case 3: snprintf(b, sizeof b, "%.1f", (double)s_cfg.tol); return b;
        case 4: return "INVIO";
    }
    return "";
}
static void view_settings(int top, int h, int y0)
{
    app_ui_list(y0, top + h - y0, SET_ROWS, s_set_sel, set_label, set_right, nullptr, nullptr);
}

// ---- main draw ----------------------------------------------------------------------------------
static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);

    bool present = nucleo_imu_present();
    float pd = present ? deg_now() : 0;
    bool level = present && pd < s_cfg.tol;
    char rt[20]; snprintf(rt, sizeof(rt), "%s%s", VIEW_NAME[s_view], s_hold ? " *" : "");
    unsigned short acc = !present ? C_YELLOW : s_hold ? C_PURPLE : level ? C_GREEN : C_BLUE;
    int y0 = app_ui_title("Livella", acc, rt);

    if (!present) {
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
        d.setCursor(12, y0 + 14); d.print("Sensore IMU");
        d.setCursor(12, y0 + 36); d.print("non rilevato");
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(12, y0 + 60); d.print(nucleo_imu_debug());
        return;
    }
    switch (s_view) {
        case V_BULLSEYE: view_bullseye(top, h, y0); break;
        case V_TUBE_H:   view_tube_h(top, h, y0);   break;
        case V_TUBE_V:   view_tube_v(top, h, y0);   break;
        case V_DIGITAL:  view_digital(top, h, y0);  break;
        case V_SETTINGS: view_settings(top, h, y0); break;
    }
}

static void calibrate(void)   // capture the current tilt as the new "level" zero
{
    s_ozx = s_lx; s_ozy = s_ly; s_hold = false;
    nucleo_app_request_draw();
}

// change the selected settings row; dir = +1/-1 (toggles ignore the sign, ranges use it)
static void change_setting(int dir)
{
    switch (s_set_sel) {
        case 0: s_cfg.units = (s_cfg.units + 1) % 2; break;
        case 1: s_cfg.flipx = !s_cfg.flipx; break;
        case 2: s_cfg.flipy = !s_cfg.flipy; break;
        case 3:
            s_cfg.tol += dir * 0.1f;
            if (s_cfg.tol < 0.1f) s_cfg.tol = 0.1f;
            if (s_cfg.tol > 2.0f) s_cfg.tol = 2.0f;
            break;
        case 4: s_ozx = s_ozy = 0; break;   // azzera calibrazione
    }
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (ch == 'g' || ch == 'G') { calibrate(); return; }            // G = zero (central, press-safe)
    if (ch == 'f' || ch == 'F') {                                   // F = hold/freeze toggle
        s_hold = !s_hold;
        if (s_hold) { s_hx = s_lx; s_hy = s_ly; s_hdeg = s_deg; }
        nucleo_app_request_draw(); return;
    }
    if (key == NK_RIGHT) {                                          // RIGHT = next view / increase setting
        if (s_view == V_SETTINGS) change_setting(+1);
        else { s_view = (s_view + 1) % V_COUNT; nucleo_app_request_draw(); }
        return;
    }
    if (s_view == V_SETTINGS) {
        if (key == NK_UP)         { s_set_sel = (s_set_sel + 4) % 5; nucleo_app_request_draw(); }
        else if (key == NK_DOWN)  { s_set_sel = (s_set_sel + 1) % 5; nucleo_app_request_draw(); }
        else if (key == NK_ENTER) change_setting(+1);
    }
}

// NK_LEFT and NK_BACK route here (NOT to on_key). LEFT = previous view / decrease setting (consumed);
// Esc/Back falls through (return false) so the framework closes the app as usual.
static bool back(int key)
{
    if (key == NK_LEFT) {
        if (s_view == V_SETTINGS) change_setting(-1);
        else { s_view = (s_view + V_COUNT - 1) % V_COUNT; nucleo_app_request_draw(); }
        return true;
    }
    return false;
}

static void tab(void) { s_view = (s_view + 1) % V_COUNT; nucleo_app_request_draw(); }

static void enter(void)
{
    s_hold = false;
    s_qx = INT32_MIN;                       // force the first live frame after a (re)open
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    nucleo_app_set_hint("TAB vista   G azzera   F blocca   <-/-> sfoglia   esc esci");
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_level(void)
{
    static const nucleo_app_def_t app = {
        "level", "Livella", "Hardware", "Livella pro multi-vista (BMI270)",
        'L', C_GREEN, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
