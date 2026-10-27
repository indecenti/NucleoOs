// Contapassi (pedometer) — Hardware section. Counts walking steps from the BMI270 (Cardputer ADV) with
// a peak detector (nk_step, host-tested), and shows a dotted progress ring toward a daily goal, the live
// step total, cadence (passi/min) and estimated distance. TAB opens options (obiettivo, lunghezza passo,
// azzera). Counts while the app is foreground — open it and carry the device while you walk. ADV-only.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"       // TR(it,en): hint follows the system language
#include "nucleo_imu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app_gfx.h"
#include "app_large.h"      // two screens: ring + count native on the panel, the Cardputer the companion
#include "nucleo_touch.h"

static unsigned s_goal = 6000;
static int      s_stride = 70;          // cm per step (for distance)
static unsigned s_steps;                // mirror of nucleo_imu_steps()
static int64_t  s_last_step_us;
static float    s_cad;                  // smoothed cadence (passi/min)
static bool     s_settings;
static int      s_set_sel;

// ---- options (TAB) — shared focused-list widget, consistent with Files/Music ----
#define PSET_ROWS 3
static const char *pset_label(int i, void *)
{
    switch (i) {
        case 0: return TR5("Obiettivo", "Goal", "Objetivo", "Objectif", "Ziel");
        case 1: return TR5("Passo (cm)", "Stride (cm)", "Paso (cm)", "Pas (cm)", "Schritt (cm)");
        case 2: return TR5("Azzera passi", "Reset steps", "Poner a cero", "Remise zero", "Zuruecksetzen");
    }
    return "";
}
static const char *pset_right(int i, void *)
{
    static char b[12];
    switch (i) {
        case 0: snprintf(b, sizeof b, "%u", s_goal);   return b;
        case 1: snprintf(b, sizeof b, "%d", s_stride); return b;
        case 2: return TR("INVIO", "ENTER");
    }
    return "";
}
// An option row on the panel (lg::settings_list): the Settings look.
static void lg_pset_row(int i, lg::Setting *s, void *)
{
    s->label = pset_label(i, nullptr); s->value = pset_right(i, nullptr); s->kind = i == 2 ? lg::ROW_DANGER : lg::ROW_CYCLE;
}
static void pset_change(int dir)
{
    switch (s_set_sel) {
        case 0:
            s_goal = (unsigned)((int)s_goal + dir * 1000);
            if ((int)s_goal < 1000) s_goal = 1000;
            if (s_goal > 30000) s_goal = 30000;
            break;
        case 1:
            s_stride += dir * 5;
            if (s_stride < 40) s_stride = 40;
            if (s_stride > 110) s_stride = 110;
            break;
        case 2:
            nucleo_imu_steps_reset(); s_steps = 0; s_cad = 0; s_last_step_us = 0;
            break;
    }
    nucleo_app_request_draw();
}
static void p_tab(void) { s_settings = !s_settings; s_set_sel = 0; nucleo_app_request_draw(); }
static bool p_back(int key)
{
    if (s_settings) {
        if (key == NK_LEFT) { pset_change(-1); return true; }
        s_settings = false; nucleo_app_request_draw(); return true;
    }
    return false;
}

static bool poll(void)
{
    if (s_settings || !nucleo_imu_present()) return false;
    nucleo_imu_sample();                                  // feed the step detector at the loop rate
    unsigned s = nucleo_imu_steps();
    int64_t now = esp_timer_get_time();
    bool redraw = false;
    if (s != s_steps) {
        if (s_last_step_us) {
            float dt = (float)(now - s_last_step_us) / 1000000.0f;
            if (dt > 0.20f && dt < 2.5f) { float c = 60.0f / dt; s_cad += (c - s_cad) * 0.4f; }
        }
        s_last_step_us = now;
        s_steps = s;
        redraw = true;
    } else if (s_cad > 0.5f && s_last_step_us && now - s_last_step_us > 2500000) {
        s_cad = 0;                                        // stopped walking -> cadence fades to 0
        redraw = true;
    }
    return redraw;
}

static const char *p_title(void) { return TR("Contapassi", "Pedometer"); }
static const char *p_hint(void)
{
    // <= 39 chars: the bar shows no more (the old 54-char hint lost "TAB opzioni / esc")
    return TR5("cammina  r azzera  tab menu  esc esci", "walk  r reset  tab menu  esc back", "camina  r cero  tab menu  esc salir",
               "marche  r zero  tab menu  esc sortir", "gehen  r null  tab Menue  esc zur.");
}
static float p_prog(void)
{
    float prog = nucleo_imu_present() && s_goal ? (float)s_steps / (float)s_goal : 0.0f;
    return prog > 1.0f ? 1.0f : prog;
}

// The dotted goal ring centred on (cx, cy) with the % inside, and the column at nx: the step total, cadence,
// distance. Laid out for the screen [y0, bottom) x [0, vw); k = 1 on the Cardputer, 2 on the panel.
static void body(int cx, int cy, int R, int nx, int y0, int bottom, int vw, int k)
{
    float prog = p_prog();
    bool done = prog >= 1.0f;
    const int ND = 36;
    int litn = (int)(prog * ND + 0.5f);
    for (int i = 0; i < ND; i++) {
        float rad = (-90.0f + i * 360.0f / ND) * 0.0174533f;
        int dx = cx + (int)(cosf(rad) * R), dy = cy + (int)(sinf(rad) * R);
        bool on = i < litn;
        d.fillCircle(dx, dy, (on ? 3 : 2) * k, on ? (done ? C_GREEN : C_BLUE) : DIM);
    }
    char pc[8]; snprintf(pc, sizeof(pc), "%d%%", (int)(prog * 100));
    d.setTextSize(2 * k); d.setTextColor(done ? C_GREEN : FG, BG);
    d.setCursor(cx - (int)strlen(pc) * 6 * k, cy - 8 * k); d.print(pc);

    // RIGHT column: big step total + cadence + distance (use the landscape)
    char sb[12]; snprintf(sb, sizeof(sb), "%u", s_steps);
    int ts = 4, tw = (int)strlen(sb) * 6 * ts;
    while (ts > 2 && nx + tw > vw - 4) { ts--; tw = (int)strlen(sb) * 6 * ts; }
    d.setTextSize(ts); d.setTextColor(done ? C_GREEN : FG, BG);
    d.setCursor(nx, y0 + 6); d.print(sb);
    const lgfx::IFont *cf = k > 1 ? (const lgfx::IFont *)&fonts::Font2 : &fonts::Font0;   // captions: 16 px on the panel
    d.setFont(cf);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(nx, y0 + 8 + 8 * ts); d.print(TR("passi", "steps"));

    float dist_m = s_steps * (s_stride / 100.0f);
    char ln[24];
    if (dist_m >= 1000.0f) snprintf(ln, sizeof(ln), "%.2f km", (double)(dist_m / 1000.0f));
    else                   snprintf(ln, sizeof(ln), "%d m", (int)dist_m);
    d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(FG, BG);
    d.setCursor(nx, bottom - 38 * k); d.print(ln);
    d.setFont(cf);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    char cb[24]; snprintf(cb, sizeof(cb), TR("%d passi/min", "%d steps/min"), (int)(s_cad + 0.5f));
    d.setCursor(nx, bottom - 16 * k); d.print(cb);
    d.setFont(&fonts::Font0);
}

// The header's right text ("62%" / NO IMU) and accent.
static unsigned short p_right(char *rgt, int n)
{
    float prog = p_prog();
    if (!nucleo_imu_present()) { snprintf(rgt, n, "NO IMU"); return C_YELLOW; }
    snprintf(rgt, n, "%d%%", (int)(prog * 100));
    return prog >= 1.0f ? C_GREEN : C_BLUE;
}

// The panel (lg::frame): header (chevron = back, menu = options), the goal ring big on the left (tap it = the
// options, where the goal is), the count on the right. Options: rows, tap again = change (wraps at the top).
static int s_lsc;
static void lg_paint(void *)
{
    d.fillRect(0, 0, 320, 240, BG);
    char rgt[16]; unsigned short acc = p_right(rgt, sizeof rgt);
    int y0 = lg::header(p_title(), rgt, acc, NK_BACK, NK_TAB);
    d.setFont(&fonts::Font0);                          // the views size the classic font themselves
    if (s_settings) {
        lg::settings_list(8, y0 + 4, 304, 240 - y0 - 4, 34, PSET_ROWS, s_set_sel, &s_lsc, lg_pset_row, nullptr, acc);
        return;
    }
    body(96, 136, 84, 200, y0 + 6, 236, 320, 2);
    nucleo_touch_add(10, y0, 180, 240 - y0, NK_TAB, 0);
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr, 0, 0, true);   // exact theme colours (RGB565 strips)
    nucleo_touch_commit();
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    d.fillRect(0, top, W, h, BG);
    char rgt[16]; unsigned short acc = p_right(rgt, sizeof rgt);
    if (nucleo_app_two_screens()) {                       // the companion: the count, the keys
        int y = lg::comp_head(p_title(), acc);
        char sb[12]; snprintf(sb, sizeof(sb), "%u", s_steps);
        lg::message(0, y, W, 90, sb, rgt, acc);
        lg::comp_keys(p_hint());
        return;
    }
    int y0 = app_ui_title(p_title(), acc, rgt);

    if (!nucleo_imu_present()) {
        lg::message(0, y0, W, bottom - y0, TR("Sensore IMU", "IMU sensor"), TR("non rilevato", "not found"), C_YELLOW);
        return;
    }
    if (s_settings) {
        app_ui_list(y0, bottom - y0, PSET_ROWS, s_set_sel, pset_label, pset_right, nullptr, nullptr);
        return;
    }

    // dotted progress ring on the LEFT, with the % in the middle
    int R = (bottom - y0) / 2 - 3; if (R > 40) R = 40; if (R < 22) R = 22;
    body(58, (y0 + bottom) / 2, R, 122, y0, bottom, W, 1);
}

static void on_key(int key, char ch)
{
    if (s_settings) {
        switch (lg::row_tap(key, ch, s_lsc, PSET_ROWS, &s_set_sel)) {   // an option row on the panel
            case 1: nucleo_app_request_draw(); return;
            case 2:                                                      // tap again = up, wrapping at the top
                if (s_set_sel == 0 && s_goal >= 30000) s_goal = 0;
                if (s_set_sel == 1 && s_stride >= 110) s_stride = 35;
                pset_change(+1); return;
        }
        if (key == NK_UP)        { s_set_sel = (s_set_sel + PSET_ROWS - 1) % PSET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { s_set_sel = (s_set_sel + 1) % PSET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT || key == NK_ENTER) pset_change(+1);
        return;
    }
    if (ch == 'r' || ch == 'R') { nucleo_imu_steps_reset(); s_steps = 0; s_cad = 0; s_last_step_us = 0; nucleo_app_request_draw(); }
}

static void enter(void)
{
    s_settings = false;
    nucleo_imu_steps_reset(); s_steps = 0; s_cad = 0; s_last_step_us = 0;
    nucleo_app_set_hint(p_hint());
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(p_tab);
    nucleo_app_set_back_handler(p_back);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_pedometer(void)
{
    static const nucleo_app_def_t app = {
        "pedometer", "Contapassi", "Measure", "Conta i passi (BMI270)",
        'P', C_GREEN, enter, on_key, nullptr, draw, nullptr, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
