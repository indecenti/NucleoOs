// Goniometro (digital angle finder) — Hardware section. Reads the BMI270 (Cardputer ADV) and shows the
// device's roll as a full-screen protractor dial + a plumb needle + big degrees. SPACE zeroes on a
// reference edge (relative corner measurement); F freezes/locks a reading; R clears the zero. ADV-only
// (registered behind nucleo_ui_is_adv). The angle sign mirrors the IMU tilt mapping — flip ROLL_SIGN if
// a unit reads inverted after a flash. The level MATH is shared with the Livella (nucleo_imu_level).
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"       // TR(it,en): hint follows the system language
#include "nucleo_imu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "app_gfx.h"

#define RAD2DEG   57.29578f
#define ROLL_SIGN (+1)
#define FLAT_MIN  8.0f          // below this tilt-from-flat the in-plane gravity is too small to trust

static float s_lx, s_ly, s_deg; // live in-plane gravity + tilt-from-flat
static float s_roll;            // smoothed displayed roll (deg)
static bool  s_seeded;
static bool  s_reliable;        // sticky "tilt big enough to trust" (hysteresis — see poll)
static float s_zero;            // zero reference (deg) for relative mode
static bool  s_frozen;          // reading locked
static float s_frozen_val;
static struct { int units; bool invert; } s_cfg = { 0, false };   // units: 0 = gradi, 1 = percento (pendenza)
static bool  s_settings;        // options overlay open (TAB)
static int   s_set_sel;

// ---- options (TAB) — rendered with the shared focused-list widget, like Files/Music ----
#define GSET_ROWS 3
static const char *gset_label(int i, void *)
{
    static const char *L[GSET_ROWS] = { "Unita", "Inverti segno", "Azzera calib." };
    return (i >= 0 && i < GSET_ROWS) ? L[i] : "";
}
static const char *gset_right(int i, void *)
{
    switch (i) {
        case 0: return s_cfg.units == 1 ? "%" : "gradi";
        case 1: return s_cfg.invert ? "si" : "no";
        case 2: return "INVIO";
    }
    return "";
}
static void gset_change(void)
{
    if (s_set_sel == 0)      s_cfg.units = (s_cfg.units + 1) % 2;
    else if (s_set_sel == 1) s_cfg.invert = !s_cfg.invert;
    else if (s_set_sel == 2) s_zero = 0.f;          // azzera calibrazione
    nucleo_app_request_draw();
}
static void g_tab(void) { s_settings = !s_settings; s_set_sel = 0; nucleo_app_request_draw(); }
static bool g_back(int key)
{
    if (s_settings) {
        if (key == NK_LEFT) { gset_change(); return true; }   // LEFT also flips a toggle
        s_settings = false; nucleo_app_request_draw(); return true;   // BACK closes options (not the app)
    }
    return false;
}

static float wrap180(float a) { while (a > 180.f) a -= 360.f; while (a < -180.f) a += 360.f; return a; }
static unsigned short scl_dim(unsigned short c);   // fwd decl (defined below)

// Update the displayed roll HERE (at the IMU rate) and gate the redraw to a visible change. Doing the
// EMA in poll (not draw) lets the gate compare what the user will actually see; a raw-delta test repainted
// the buffered frame ~50x/s and the needle/number shimmered (ANTI-FLICKER: only blit on a pixel/digit move).
static int s_qa = INT32_MIN, s_qx, s_qy, s_qf;
static bool poll(void)
{
    if (s_frozen || s_settings) return false;             // locked reading / options list: nothing live
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    bool present = nucleo_imu_present();
    // Sticky "reliable" with hysteresis: near flat the in-plane gravity is too small to trust, but a bare
    // threshold chatters and flips the WHOLE readout (number<->"--", needle colour, caption) on/off — the
    // most visible flicker. Engage above FLAT_MIN, drop only 2 deg below it.
    s_reliable = present && (s_reliable ? s_deg > FLAT_MIN - 2.0f : s_deg > FLAT_MIN);
    float rollNow = atan2f(s_lx, s_ly) * RAD2DEG * ROLL_SIGN;
    if (!s_seeded) { s_roll = rollNow; s_seeded = true; }
    else if (s_reliable) s_roll = wrap180(s_roll + wrap180(rollNow - s_roll) * 0.5f);

    float shown = wrap180(s_roll - s_zero);
    if (s_cfg.invert) shown = -shown;
    int qa = (int)lroundf(shown * 10.0f);                 // 0.1 deg — the readout's resolution
    float m = sqrtf(s_lx * s_lx + s_ly * s_ly);
    int qx = m > 0.01f ? (int)lroundf(s_lx / m * 60.0f) : 0;   // needle tip to ~1 px on the dial rim
    int qy = m > 0.01f ? (int)lroundf(s_ly / m * 60.0f) : 0;
    int qf = s_reliable ? 1 : 0;
    if (qa == s_qa && qx == s_qx && qy == s_qy && qf == s_qf) return false;
    s_qa = qa; s_qx = qx; s_qy = qy; s_qf = qf;
    return true;
}

// One tick on the dial at angle `a` (deg, 0 at screen-bottom, CW), length `len` inward from the rim.
static void tick(int cx, int cy, int R, float a, int len, unsigned short col)
{
    float rad = (a + 90.0f) * 0.0174533f;            // 0deg -> bottom (+90 rotates screen-right to bottom)
    float ca = cosf(rad), sa = sinf(rad);
    d.drawLine(cx + (int)(ca * (R - len)), cy + (int)(sa * (R - len)),
               cx + (int)(ca * (R - 1)),   cy + (int)(sa * (R - 1)), col);
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);

    bool present = nucleo_imu_present();
    // s_roll / s_reliable are advanced in poll() (the IMU rate) so the redraw can be gated to a visible
    // change; draw only renders. Seed defensively in case draw runs before the first poll.
    if (!s_seeded) { s_roll = atan2f(s_lx, s_ly) * RAD2DEG * ROLL_SIGN; s_seeded = true; }
    bool reliable = s_reliable && present;
    float shown = s_frozen ? s_frozen_val : wrap180(s_roll - s_zero);
    if (s_cfg.invert) shown = -shown;
    bool onRound = reliable && (fabsf(shown) < 0.4f || fabsf(fabsf(shown) - 90.f) < 0.4f || fabsf(fabsf(shown) - 45.f) < 0.4f || fabsf(fabsf(shown) - 180.f) < 0.4f);

    const char    *rgt = !present ? "NO IMU" : s_frozen ? "BLOCCATO" : (s_zero != 0.f ? "REL" : "LIVE");
    unsigned short acc = !present ? C_YELLOW : s_frozen ? C_GREEN : (onRound ? C_GREEN : C_BLUE);
    int y0 = app_ui_title("Goniometro", acc, rgt);

    if (!present) {
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
        d.setCursor(12, y0 + 18); d.print("Sensore IMU");
        d.setCursor(12, y0 + 40); d.print("non rilevato");
        return;
    }
    if (s_settings) {                                  // TAB options — shared list (no overlap, consistent)
        app_ui_list(y0, top + h - y0, GSET_ROWS, s_set_sel, gset_label, gset_right, nullptr, nullptr);
        return;
    }

    int bottom = top + h;
    int cx = 74, cy = (y0 + bottom) / 2;                      // dial LEFT, big number RIGHT (use the landscape)
    int R = (bottom - y0) / 2 - 3;
    if (R > 60) R = 60;
    if (R < 22) R = 22;

    // dial: rim + graduated ticks (major at 0/90/180/270, medium every 30, minor every 10)
    d.drawCircle(cx, cy, R, MUTED);
    d.drawCircle(cx, cy, R * 2 / 3, scl_dim(MUTED));
    for (int a = 0; a < 360; a += 10) {
        bool major = (a % 90 == 0), med = (a % 30 == 0);
        tick(cx, cy, R, (float)a, major ? 13 : med ? 9 : 5, major ? FG : DIM);
    }
    // zero marker on the rim (relative mode) — a green pip where the reference sits
    if (s_zero != 0.f && !s_frozen) {
        float rad = (s_zero + 90.0f) * 0.0174533f;
        d.fillCircle(cx + (int)(cosf(rad) * R), cy + (int)(sinf(rad) * R), 3, C_GREEN);
    }

    // plumb needle: points to gravity-down (lx,ly) in screen coords (+y = down). Tapered pointer + a
    // counter-tail + a bob at the tip, so it reads as a real plumb hanging to the low side.
    unsigned short ncol = reliable ? (onRound ? C_GREEN : C_YELLOW) : C_GREY;
    float m = sqrtf(s_lx * s_lx + s_ly * s_ly);
    float ux = m > 0.01f ? s_lx / m : 0.f, uy = m > 0.01f ? s_ly / m : 1.f;
    float px = -uy, py = ux;                                  // perpendicular (needle width)
    int tipx = cx + (int)(ux * (R - 6)), tipy = cy + (int)(uy * (R - 6));
    int bx1 = cx + (int)(px * 5),  by1 = cy + (int)(py * 5);
    int bx2 = cx - (int)(px * 5),  by2 = cy - (int)(py * 5);
    d.fillTriangle(tipx, tipy, bx1, by1, bx2, by2, ncol);    // pointer
    d.fillTriangle(cx - (int)(ux * R * 0.32f) + (int)(px * 3), cy - (int)(uy * R * 0.32f) + (int)(py * 3),
                   cx - (int)(ux * R * 0.32f) - (int)(px * 3), cy - (int)(uy * R * 0.32f) - (int)(py * 3),
                   cx, cy, scl_dim(ncol));                    // counter-tail (dim)
    d.fillCircle(tipx, tipy, 4, ncol);                       // plumb bob
    d.fillCircle(cx, cy, 3, FG);                             // hub

    // BIG angle readout on the right, auto-fit to the landscape width. Unit: degrees or slope %.
    char ds[16];
    float val = fabsf(shown);
    if (s_cfg.units == 1) val = tanf(val / RAD2DEG) * 100.0f;          // pendenza (grade %)
    if (reliable) snprintf(ds, sizeof(ds), "%.1f", (double)val);
    else          snprintf(ds, sizeof(ds), "--");
    int nx = 150, ts = 4, tw = (int)strlen(ds) * 6 * ts;
    while (ts > 2 && nx + tw + 18 > W) { ts--; tw = (int)strlen(ds) * 6 * ts; }
    d.setTextSize(ts);
    d.setTextColor(reliable ? (onRound ? C_GREEN : FG) : DIM, BG);
    d.setCursor(nx, cy - 4 * ts); d.print(ds);
    if (reliable && s_cfg.units == 1) { d.setCursor(nx + tw + 3, cy - 4 * ts); d.print("%"); }   // % sign
    else if (reliable) d.drawCircle(nx + tw + 4 + ts, cy - 4 * ts + 3, ts, onRound ? C_GREEN : FG);   // degree mark
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(nx, cy + 4 * ts + 5);
    d.print(!reliable ? "inclina il device" : s_frozen ? "bloccato" : (s_zero != 0.f ? "relativo" : "assoluto"));
}

// dim a colour ~45% for the needle's counter-tail (local helper; fx3d::scl is for the 3D path).
static unsigned short scl_dim(unsigned short c)
{
    int r = ((c >> 11) & 31) * 45 / 100, g = ((c >> 5) & 63) * 45 / 100, b = (c & 31) * 45 / 100;
    return (unsigned short)((r << 11) | (g << 5) | b);
}

static void on_key(int key, char ch)
{
    if (s_settings) {
        if (key == NK_UP)        { s_set_sel = (s_set_sel + GSET_ROWS - 1) % GSET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { s_set_sel = (s_set_sel + 1) % GSET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT || key == NK_ENTER) gset_change();
        return;
    }
    if (ch == ' ') { s_zero = s_roll; s_frozen = false; nucleo_app_request_draw(); }          // zero here
    else if (ch == 'r' || ch == 'R') { s_zero = 0.f; s_frozen = false; nucleo_app_request_draw(); }  // absolute
    else if (ch == 'f' || ch == 'F') {                                                          // freeze toggle
        s_frozen = !s_frozen;
        if (s_frozen) s_frozen_val = wrap180(s_roll - s_zero);
        nucleo_app_request_draw();
    }
}

static void enter(void)
{
    s_zero = 0.f; s_frozen = false; s_seeded = false; s_settings = false;
    s_qa = INT32_MIN;                       // force the first live frame after a (re)open
    nucleo_imu_level(&s_lx, &s_ly, &s_deg);
    s_reliable = nucleo_imu_present() && s_deg > FLAT_MIN;
    nucleo_app_set_hint(TR("spazio azzera   F blocca   R assoluto   TAB opzioni   esc esci", "space zero   F lock   R absolute   TAB options   esc back"));
    nucleo_app_set_tab_handler(g_tab);
    nucleo_app_set_back_handler(g_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_goniometer(void)
{
    static const nucleo_app_def_t app = {
        "goniometer", "Goniometro", "Hardware", "Misura angoli e pendenze (BMI270)",
        'A', C_BLUE, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
