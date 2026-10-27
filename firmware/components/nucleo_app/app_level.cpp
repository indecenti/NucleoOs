// Livella PRO — Hardware section. A complete, customizable spirit level on the BMI270 (Cardputer ADV).
// Multiple views (TAB cycles): Mira (bullseye), Tubo (classic horizontal vial), Piombo (vertical vial),
// Digitale (big X/Y/total), Impostazioni (settings). Calibrate/zero with the central G key (a centre key
// so the press doesn't nudge the reading). Settings let you flip the X/Y axes (fix sign on YOUR unit
// without a reflash), pick units (°/%), set the "in bolla" tolerance, and hold a reading. ADV-only.
// Level MATH shared with the Goniometro (nucleo_imu_level → lx,ly = screen-plane gravity, deg = tilt).
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"        // TR(it,en): hint follows the system language
#include "nucleo_imu.h"
#include "nucleo_exclusive.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "app_gfx.h"
#include "app_large.h"      // two screens: the views native on the panel, the Cardputer the companion
#include "nucleo_touch.h"

#define RAD2DEG 57.29578f

// Specular glint on the vial bubble: genuine content (a white highlight), named per the UI-kit rule.
static const unsigned short GLINT = 0xFFFF;

enum { V_BULLSEYE, V_TUBE_H, V_TUBE_V, V_DIGITAL, V_SETTINGS, V_COUNT };
static const char *view_name(int i)
{
    switch (i) {
        case V_BULLSEYE: return TR5("Mira", "Bullseye", "Mira", "Mire", "Zielscheibe");
        case V_TUBE_H:   return TR5("Tubo", "Tube", "Tubo", "Tube", "Roehr");
        case V_TUBE_V:   return TR5("Piombo", "Plumb", "Plomada", "Aplomb", "Lot");
        case V_DIGITAL:  return TR5("Digitale", "Digital", "Digital", "Numerique", "Digital");
        default:         return TR5("Impostazioni", "Settings", "Configurac.", "Parametres", "Einstellung");
    }
}

static int   s_view;
static float s_lx, s_ly, s_deg;     // smoothed in-plane gravity + tilt-from-flat
static float s_ozx, s_ozy;          // calibration zero (subtracted from lx/ly)
static bool  s_hold;                // freeze the reading
static float s_hx, s_hy;            // held snapshot (the held tilt is derived from it)

// customizable settings (persist within a boot; reset on reboot)
static struct { int units; bool flipx, flipy; float tol; } s_cfg = { 0, false, false, 0.4f };
static int s_set_sel;               // selected settings row
// The screen the views lay out for: the Cardputer (240 wide, k = 1) or the panel (320 wide, k = 2: bigger
// vial, bigger type). s_nx/s_ny = the top-left of the readouts drawn on the panel (a tap there = hold).
static int s_vw = W, s_k = 1, s_nx = 999, s_ny = 999, s_lsc;

// ---- helpers ------------------------------------------------------------------------------------
static unsigned short dim(unsigned short c, int pct)
{
    int r = ((c >> 11) & 31) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
    return (unsigned short)((r << 11) | (g << 5) | b);
}
// format an inclination (degrees in) per the unit setting into buf; returns the suffix glyph kind
static void fmt_val(float deg, char *buf, int n)
{
    if (s_cfg.units == 1) {                                            // slope % — tan explodes toward 90 deg
        float p = (deg > 84.0f) ? 999.9f : tanf(deg / RAD2DEG) * 100.0f;
        if (p > 999.9f) p = 999.9f;                                    // cap: the old code printed "inf"
        snprintf(buf, n, "%.1f", (double)p);
    }
    else                  snprintf(buf, n, "%.1f", (double)deg);
}
// big centred number with auto-fit size; draws a degree ring or % sign after it
static void big_num(int x, int y, float deg, unsigned short col, int maxw)
{
    char b[16]; fmt_val(deg, b, sizeof(b));
    int sz = 2 + 2 * s_k; while (sz > 1 && (int)strlen(b) * 6 * sz + 8 * sz > maxw) sz--;
    if (s_k > 1) { if (x < s_nx) s_nx = x; if (y < s_ny) s_ny = y; }
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
static int s_qx = INT32_MIN, s_qy = INT32_MIN, s_qd = INT32_MIN;
static int64_t s_frame_us;                                // repaint cadence cap (app_ui_frame_due)
static bool poll(void)
{
    if (s_hold || s_view == V_SETTINGS) return false;     // frozen / settings list: nothing live to animate
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    float ax, ay; axes(&ax, &ay);
    // app_ui_step = Schmitt-triggered quantizer. Plain rounding chattered between two steps on the hand
    // tremor a level is always held in, so "nothing visibly moved" still reported a change and the frame
    // recomposited at the 50 Hz loop rate — that is the bubble/number shimmer.
    int qx = app_ui_step(s_qx, ax * 120.0f);              // ~1 px of bubble travel across the vial
    int qy = app_ui_step(s_qy, ay * 120.0f);
    int qd = app_ui_step(s_qd, deg_now() * 10.0f);        // 0.1 deg — the readout's resolution
    if (qx == s_qx && qy == s_qy && qd == s_qd) return false;
    // Real motion — but cap the blit cadence at 20 fps (no vsync: a loop-rate full-frame push shimmers
    // and drains the battery). Values are committed only on a frame we actually draw, so the next due
    // frame still renders the newest reading.
    if (!app_ui_frame_due(&s_frame_us, 20)) return false;
    s_qx = qx; s_qy = qy; s_qd = qd;
    return true;
}

// Display accessors — the state poll() COMMITTED, which is the only thing draw may render from.
// The views used to re-read the raw floats at draw time, so every 0.1-deg commit re-evaluated the
// in-tolerance colour boundaries against unquantized noise and the green/yellow accents (bubble,
// digits, "IN BOLLA") flipped frame to frame: that strobing is what read as flicker. A pure
// function of committed state can only change when the quantizer actually moved.
static void disp_axes(float *ax, float *ay)
{
    *ax = s_qx == INT32_MIN ? 0.f : (float)s_qx / 120.0f;
    *ay = s_qy == INT32_MIN ? 0.f : (float)s_qy / 120.0f;
}
static float disp_deg(void) { return s_qd == INT32_MIN ? 0.f : (float)s_qd / 10.0f; }

// current axes after calibration + flip (RAW — poll's input; never rendered directly)
static void axes(float *ax, float *ay)
{
    float x = (s_hold ? s_hx : s_lx) - s_ozx, y = (s_hold ? s_hy : s_ly) - s_ozy;
    if (s_cfg.flipx) x = -x;
    if (s_cfg.flipy) y = -y;
    *ax = x; *ay = y;
}
// Tilt from level, measured from the CALIBRATED axes so G zeroes the total too (the raw tilt-from-flat
// kept reading the table's slope after a zero: the bullseye said 0.0 while the title/TOTAL stayed off).
static float deg_now(void)
{
    float ax, ay; axes(&ax, &ay);
    float m = sqrtf(ax * ax + ay * ay); if (m > 1) m = 1;
    return asinf(m) * RAD2DEG;
}

// ---- views --------------------------------------------------------------------------------------
static void view_bullseye(int top, int h, int y0)
{
    float ax, ay; disp_axes(&ax, &ay);
    float deg = disp_deg();
    bool level = deg < s_cfg.tol;

    int bottom = top + h;
    // vial on the LEFT, big number on the RIGHT (use the landscape width)
    int vcx = 66, vcy = (y0 + bottom) / 2;
    int R = (bottom - y0) / 2 - 2;
    if (R > 56 + 30 * (s_k - 1)) R = 56 + 30 * (s_k - 1);
    if (R < 22) R = 22;
    if (s_k > 1) vcx = 96;

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
    d.fillCircle(bx, by, 10 * s_k, level ? C_GREEN : C_YELLOW);
    d.drawCircle(bx, by, 10 * s_k, INK);
    d.fillCircle(bx - 3, by - 3, 2, GLINT);

    // big number beside it on the right
    int nx = s_k > 1 ? 196 : 138;
    big_num(nx, vcy - 20 * s_k, deg, level ? C_GREEN : FG, s_vw - nx - 4);
    d.setTextSize(s_k); d.setTextColor(MUTED, BG);
    char bx2[24]; snprintf(bx2, sizeof(bx2), "X %+5.1f", (double)(asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG));
    d.setCursor(nx, vcy + 16 * s_k); d.print(bx2);
    snprintf(bx2, sizeof(bx2), "Y %+5.1f", (double)(asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG));
    d.setCursor(nx, vcy + 28 * s_k); d.print(bx2);
    if (level) { d.setTextColor(C_GREEN, BG); d.setCursor(nx, vcy + 42 * s_k); d.print(TR5("IN BOLLA", "LEVEL", "NIVELADO", "A NIVEAU", "EBEN")); }
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
        d.fillCircle(bx - 2, y + thick / 2 - 2, 2, GLINT);
    } else {
        d.fillRoundRect(x, y, thick, len, thick / 2, INK);
        d.fillRoundRect(x + 2, y + 2, thick - 4, len - 4, (thick - 4) / 2, liquid);
        int cym = y + len / 2;
        d.drawLine(x, cym - thick / 2, x + thick, cym - thick / 2, dim(FG, 70));
        d.drawLine(x, cym + thick / 2, x + thick, cym + thick / 2, dim(FG, 70));
        int by = cym + (int)(t * (len / 2 - thick));
        int br = thick / 2 - 2;
        d.fillCircle(x + thick / 2, by, br, level ? C_GREEN : C_YELLOW);
        d.fillCircle(x + thick / 2 - 2, by - 2, 2, GLINT);
    }
}

static void view_tube_h(int top, int h, int y0)
{
    float ax, ay; disp_axes(&ax, &ay);
    float angle = asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG;     // long-edge inclination
    bool level = fabsf(angle) < s_cfg.tol;
    int bottom = top + h, mid = (y0 + bottom) / 2;
    int thick = 20 + 14 * s_k, len = s_vw - 16;
    capillary(8, mid - 6 - thick / 2, len, thick, true, -ax * 3.0f, level);   // bubble to the HIGH side
    big_num(8, mid + 18 * s_k, fabsf(angle), level ? C_GREEN : FG, s_vw - 16);
}

static void view_tube_v(int top, int h, int y0)
{
    float ax, ay; disp_axes(&ax, &ay);
    float angle = asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG;     // short-edge inclination (plumb)
    bool level = fabsf(angle) < s_cfg.tol;
    int bottom = top + h;
    int thick = 20 + 14 * s_k, len = bottom - y0 - 6, nx = 36 + thick;
    capillary(20, y0 + 3, len, thick, false, -ay * 3.0f, level);              // bubble to the HIGH side
    big_num(nx, (y0 + bottom) / 2 - 16 * s_k, fabsf(angle), level ? C_GREEN : FG, s_vw - nx - 4);
}

static void view_digital(int top, int h, int y0)
{
    float ax, ay; disp_axes(&ax, &ay);
    float aX = asinf(fminf(fmaxf(ax, -1), 1)) * RAD2DEG;
    float aY = asinf(fminf(fmaxf(ay, -1), 1)) * RAD2DEG;
    float tot = disp_deg();
    bool level = tot < s_cfg.tol;
    int bottom = top + h, third = (bottom - y0) / 3;
    int nx = s_k > 1 ? 130 : 96;
    d.setTextSize(s_k); d.setTextColor(MUTED, BG);
    d.setCursor(8, y0 + 4);  d.print(TR5("X (lungo)", "X (long)", "X (largo)", "X (long)", "X (lang)"));
    d.setCursor(8, y0 + third + 2); d.print(TR5("Y (corto)", "Y (short)", "Y (corto)", "Y (court)", "Y (kurz)"));
    d.setCursor(8, y0 + 2 * third); d.print(TR5("TOTALE", "TOTAL", "TOTAL", "TOTAL", "GESAMT"));
    big_num(nx, y0 + 2,            fabsf(aX), fabsf(aX) < s_cfg.tol ? C_GREEN : FG, s_vw - nx - 4);
    big_num(nx, y0 + third,        fabsf(aY), fabsf(aY) < s_cfg.tol ? C_GREEN : FG, s_vw - nx - 4);
    big_num(nx, y0 + 2 * third - 2, tot,      level ? C_GREEN : C_BLUE, s_vw - nx - 4);
}

// Settings rows rendered with the SHARED focused-list widget (app_ui_list) — same Wear-OS look as
// Files/Music/Photos, so it scrolls instead of overflowing and matches every other app's lists.
#define SET_ROWS 5
static const char *set_label(int i, void *)
{
    switch (i) {
        case 0: return TR5("Unita", "Units", "Unidad", "Unites", "Einheit");
        case 1: return TR5("Inverti X", "Flip X", "Invertir X", "Inverser X", "X umkeh.");
        case 2: return TR5("Inverti Y", "Flip Y", "Invertir Y", "Inverser Y", "Y umkeh.");
        case 3: return TR5("Tolleranza", "Tolerance", "Tolerancia", "Tolerance", "Toleranz");
        case 4: return TR5("Azzera calib.", "Reset zero", "Restab. cero", "Reinit. zero", "Kal. zurck.");
    }
    return "";
}
static const char *set_right(int i, void *)
{
    static char b[24];
    switch (i) {
        case 0: return s_cfg.units == 1 ? "%" : TR5("gradi", "deg", "grados", "deg", "grad");
        case 1: return s_cfg.flipx ? TR5("si", "yes", "si", "oui", "ja") : "no";
        case 2: return s_cfg.flipy ? TR5("si", "yes", "si", "oui", "ja") : "no";
        // the "in bubble" band is always in degrees (also with % units): say so, a bare "0.4" read as a bug
        case 3: snprintf(b, sizeof b, "+/-%.1f %s", (double)s_cfg.tol, TR5("gradi", "deg", "grados", "deg", "grad")); return b;
        case 4: return TR5("INVIO", "ENTER", "INTRO", "ENTREE", "EINGABE");
    }
    return "";
}
static void view_settings(int top, int h, int y0)
{
    if (s_k > 1) lg::text_list(0, y0 + 4, 320, 240 - y0 - 4, SET_ROWS, s_set_sel, &s_lsc, set_label, set_right, nullptr, nullptr, C_GREEN, 34);
    else         app_ui_list(y0, top + h - y0, SET_ROWS, s_set_sel, set_label, set_right, nullptr, nullptr);
}

// ---- main draw ----------------------------------------------------------------------------------
static const char *l_title(void) { return TR5("Livella", "Level", "Nivel", "Niveau", "Libelle"); }
static const char *l_hint(void)
{
    // <= 39 chars: the hint bar shows no more (a longer one silently lost its last keys)
    return TR5("tab vista  g azzera  f blocca  esc esci", "tab view   g zero   f hold   esc back", "tab vista  g cero  f fijar  esc salir",
               "tab vue  g zero  f figer  esc quitter", "tab Ansicht  g Null  f Halt  esc zur.");
}
// The title's right text (view, " *" = held) and accent, on both screens.
static unsigned short l_head(char *rt, int n)
{
    bool present = nucleo_imu_present();
    bool level = present && disp_deg() < s_cfg.tol;
    // "!D" = the frame is going STRAIGHT to the panel (no 32 KB back-buffer): a clear-then-draw on a
    // no-vsync display flickers no matter how the repaint is gated — OS contiguous-RAM problem, not ours.
    snprintf(rt, n, "%s%s%s", present ? view_name(s_view) : "NO IMU", s_hold ? " *" : "",
             nucleo_app_is_buffered() || s_k > 1 ? "" : " !D");
    return !present ? C_YELLOW : s_hold ? C_PURPLE : level ? C_GREEN : C_BLUE;
}
static void view(int top, int h, int y0)
{
    switch (s_view) {
        case V_BULLSEYE: view_bullseye(top, h, y0); break;
        case V_TUBE_H:   view_tube_h(top, h, y0);   break;
        case V_TUBE_V:   view_tube_v(top, h, y0);   break;
        case V_DIGITAL:  view_digital(top, h, y0);  break;
        case V_SETTINGS: view_settings(top, h, y0); break;
    }
}

// The panel (lg::frame, 320x240): header (chevron = back, menu = next view), the view laid out big. Tap the
// vial = zero here, tap a reading = hold; settings rows: tap, tap again = change.
static void lg_paint(void *)
{
    s_vw = 320; s_k = 2; s_nx = s_ny = 999;
    d.fillRect(0, 0, 320, 240, BG);
    char rt[32]; unsigned short acc = l_head(rt, sizeof rt);
    int y0 = lg::header(l_title(), rt, acc, NK_BACK, NK_TAB);
    d.setFont(&fonts::Font0);                          // the views size the classic font themselves
    if (nucleo_imu_present()) {
        if (s_view != V_SETTINGS) lg::field(0, y0, 320, 240 - y0);
        view(0, 240, y0);
    }
    s_vw = W; s_k = 1;
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);

    char rt[32]; unsigned short acc = l_head(rt, sizeof rt);
    if (nucleo_app_two_screens()) {                       // the companion: the tilt, the view, the keys
        int y = lg::comp_head(l_title(), acc);
        big_num(16, y + 24, disp_deg(), acc, W - 32);
        lg::comp_keys(l_hint());
        return;
    }
    int y0 = app_ui_title(l_title(), acc, rt);

    if (!nucleo_imu_present()) {
        lg::message(0, y0, W, top + h - y0, TR5("Sensore IMU non rilevato", "IMU sensor not detected", "Sensor IMU no detectado",
                    "Capteur IMU non detectable", "IMU-Sensor nicht erkannt"), nucleo_imu_debug(), C_YELLOW);
        return;
    }
    view(top, h, y0);
}

static void calibrate(void)   // capture the current tilt as the new "level" zero
{
    s_ozx = s_lx; s_ozy = s_ly; s_hold = false;
    s_qx = s_qy = s_qd = INT32_MIN; s_frame_us = 0;   // force a fresh commit: display snaps to the new zero
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
    int tx, ty;
    if (lg::tap(key, ch, &tx, &ty)) ch = tx >= s_nx && ty >= s_ny ? 'f' : 'g';   // the panel: a reading = hold, the vial = zero
    if (s_view == V_SETTINGS) {
        switch (lg::row_tap(key, ch, s_lsc, SET_ROWS, &s_set_sel)) {    // a settings row on the panel
            case 1: nucleo_app_request_draw(); return;
            case 2: if (s_set_sel == 3 && s_cfg.tol > 1.95f) s_cfg.tol = 0.f;   // tap again = up, wrapping
                    change_setting(+1); return;
        }
    }
    if (ch == 'g' || ch == 'G') { calibrate(); return; }            // G = zero (central, press-safe)
    if (ch == 'f' || ch == 'F') {                                   // F = hold/freeze toggle
        s_hold = !s_hold;
        if (s_hold) { s_hx = s_lx; s_hy = s_ly; }
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
    s_qx = s_qy = s_qd = INT32_MIN; s_frame_us = 0;   // force the first live frame after a (re)open
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    nucleo_app_set_hint(l_hint());
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_level(void)
{
    static const nucleo_app_def_t app = {
        "level", "Livella", "Measure", "Livella pro multi-vista (BMI270)",
        'L', C_GREEN, enter, on_key, nullptr, draw, nullptr,
        NX_NET_APP | NX_SOLO | NX_WIFI,  // Solo boot: guaranteed buffered path — see app_goniometer.cpp
        nullptr, draw_large
    };
    nucleo_app_register(&app);
}
