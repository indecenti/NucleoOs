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
    static const char *L[PSET_ROWS] = { "Obiettivo", "Passo (cm)", "Azzera passi" };
    return (i >= 0 && i < PSET_ROWS) ? L[i] : "";
}
static const char *pset_right(int i, void *)
{
    static char b[12];
    switch (i) {
        case 0: snprintf(b, sizeof b, "%u", s_goal);   return b;
        case 1: snprintf(b, sizeof b, "%d", s_stride); return b;
        case 2: return "INVIO";
    }
    return "";
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

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    d.fillRect(0, top, W, h, BG);

    bool present = nucleo_imu_present();
    float prog = present && s_goal ? (float)s_steps / (float)s_goal : 0.0f;
    if (prog > 1.0f) prog = 1.0f;
    bool done = prog >= 1.0f;

    char rgt[16];
    if (!present) snprintf(rgt, sizeof(rgt), "NO IMU");
    else snprintf(rgt, sizeof(rgt), "%d%%", (int)(prog * 100));
    int y0 = app_ui_title("Contapassi", !present ? C_YELLOW : done ? C_GREEN : C_BLUE, rgt);

    if (!present) {
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
        d.setCursor(12, y0 + 18); d.print("Sensore IMU");
        d.setCursor(12, y0 + 40); d.print("non rilevato");
        return;
    }
    if (s_settings) {
        app_ui_list(y0, bottom - y0, PSET_ROWS, s_set_sel, pset_label, pset_right, nullptr, nullptr);
        return;
    }

    // dotted progress ring on the LEFT, with the % in the middle
    int cx = 58, cy = (y0 + bottom) / 2;
    int R = (bottom - y0) / 2 - 3; if (R > 40) R = 40; if (R < 22) R = 22;
    const int ND = 36;
    int litn = (int)(prog * ND + 0.5f);
    for (int i = 0; i < ND; i++) {
        float rad = (-90.0f + i * 360.0f / ND) * 0.0174533f;
        int dx = cx + (int)(cosf(rad) * R), dy = cy + (int)(sinf(rad) * R);
        bool on = i < litn;
        d.fillCircle(dx, dy, on ? 3 : 2, on ? (done ? C_GREEN : C_BLUE) : DIM);
    }
    char pc[8]; snprintf(pc, sizeof(pc), "%d%%", (int)(prog * 100));
    d.setTextSize(2); d.setTextColor(done ? C_GREEN : FG, BG);
    d.setCursor(cx - (int)strlen(pc) * 6, cy - 8); d.print(pc);

    // RIGHT column: big step total + cadence + distance (use the landscape)
    int nx = 122;
    char sb[12]; snprintf(sb, sizeof(sb), "%u", s_steps);
    int ts = 4, tw = (int)strlen(sb) * 6 * ts;
    while (ts > 2 && nx + tw > W - 4) { ts--; tw = (int)strlen(sb) * 6 * ts; }
    d.setTextSize(ts); d.setTextColor(done ? C_GREEN : FG, BG);
    d.setCursor(nx, y0 + 6); d.print(sb);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(nx, y0 + 8 + 8 * ts); d.print("passi");

    float dist_m = s_steps * (s_stride / 100.0f);
    char ln[24];
    if (dist_m >= 1000.0f) snprintf(ln, sizeof(ln), "%.2f km", (double)(dist_m / 1000.0f));
    else                   snprintf(ln, sizeof(ln), "%d m", (int)dist_m);
    d.setTextSize(2); d.setTextColor(FG, BG);
    d.setCursor(nx, bottom - 38); d.print(ln);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    char cb[24]; snprintf(cb, sizeof(cb), "%d passi/min", (int)(s_cad + 0.5f));
    d.setCursor(nx, bottom - 16); d.print(cb);
}

static void on_key(int key, char ch)
{
    if (s_settings) {
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
    nucleo_app_set_hint(TR("cammina col device   R azzera   TAB opzioni   esc esci", "walk with device   R reset   TAB options   esc back"));
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(p_tab);
    nucleo_app_set_back_handler(p_back);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_pedometer(void)
{
    static const nucleo_app_def_t app = {
        "pedometer", "Contapassi", "Hardware", "Conta i passi (BMI270)",
        'P', C_GREEN, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
