// System status app: battery, largest free RAM block, storage, uptime — thin gauges.
#include "nucleo_app.h"
#include "app_ui.h"
#include "app_large.h"          // two screens: metric cards on the 320x240 panel, a glance on the Cardputer
#include "nucleo_touch.h"
#include "launcher_theme.h"     // FG/INK/LINE/DIM for the panel layout
#include "nucleo_theme.h"       // themed palette (was hardcoded classic-theme literals -> broke under other themes)
#include "nucleo_i18n.h"        // TR(it,en): hint follows the system language
extern "C" {
#include "nucleo_storage.h"
#include "nucleo_registry.h"
#include "nucleo_power.h"
}
#include <M5GFX.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// Chrome follows the active theme; the per-metric gauge colors are semantic (named, not bare hex).
#define BG    THEME_BG
#define MUTED THEME_MUTED
static const uint16_t ACCENT  = 0x4D1F;   // app identity accent (matches the registered launcher color)
static const uint16_t BAT_COL = 0x4ED3;   // battery = blue
static const uint16_t RAM_COL = 0x4D9F;   // largest free block = cyan
static const uint16_t STO_COL = 0xFD20;   // storage = orange

static uint32_t s_last_up = 0;
static bool     s_first = true;   // full-clear only once; later ticks repaint just the changing regions (no flicker)
static void enter(void) { nucleo_app_set_hint(TR5("esc esci", "esc back", "esc atras", "esc retour", "esc zurueck")); s_last_up = 0; s_first = true; nucleo_app_request_draw(); }
// The finest-grained value shown is uptime in whole seconds — redraw at 1 Hz, not 5 Hz.
static void tick(void) { uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000); if (up != s_last_up) { s_last_up = up; nucleo_app_request_draw(); } }

#include "app_gfx.h"

// ---- one place to read the live numbers (shared by compact / companion / panel) ------------------
struct Metrics {
    int  bat; int mv; char bat_s[16];                  // battery pct (-1 unknown), terminal mV, display string
    unsigned blk_kb; int blk_pct; char blk_s[16];      // largest internal block
    char sto_s[20]; int sto_used;                      // SD free string + used %
    uint32_t up; int apps;                             // uptime seconds, registered app count
};
static void collect(Metrics &m)
{
    m.bat = nucleo_power_battery_pct();
    m.mv  = nucleo_power_battery_mv();
    if (m.bat < 0) snprintf(m.bat_s, sizeof m.bat_s, "--");
    else if (m.mv > 0) snprintf(m.bat_s, sizeof m.bat_s, "%d%% %d.%02dV", m.bat, m.mv / 1000, (m.mv % 1000) / 10);
    else snprintf(m.bat_s, sizeof m.bat_s, "%d%%", m.bat);

    size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    m.blk_kb = (unsigned)(blk / 1024);
    m.blk_pct = (int)(blk * 100 / (192 * 1024)); if (m.blk_pct > 100) m.blk_pct = 100;
    snprintf(m.blk_s, sizeof m.blk_s, "%u KB", m.blk_kb);

    const nucleo_storage_info_t *st = nucleo_storage_info();
    snprintf(m.sto_s, sizeof m.sto_s, "%u GB", (unsigned)(st->free_bytes / 1000000000ULL));
    m.sto_used = st->total_bytes ? (int)(100 - st->free_bytes * 100 / st->total_bytes) : 0;

    m.up = (uint32_t)(esp_timer_get_time() / 1000000);
    m.apps = nucleo_registry_count();
}

static void draw_large(void);
static void draw_companion(void);

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }   // the panel owns the dials; here: a glance

    int top = nucleo_app_content_top();
    int h = nucleo_app_content_height();
    if (s_first) { d.fillRect(0, top, 240, h, BG); s_first = false; }   // clear once, not every second

    int y0 = app_ui_title("System Status", ACCENT, "Live");

    Metrics m; collect(m);
    // Real cell level off the ADC (nucleo_power). -1 = no reading yet -> show "--" and an empty
    // bar rather than a fake 100 %. When known, append the terminal voltage for the curious.
    app_ui_gauge(y0 + 6, "Battery", m.bat_s, m.bat < 0 ? 0 : m.bat, BAT_COL);

    // Largest contiguous internal block is the number that actually limits allocations on
    // this PSRAM-less chip (heap is fragmented) — far more useful than total free. Scale
    // to 192 KB: roughly the biggest block on a fresh boot, so a full bar means healthy.
    app_ui_gauge(y0 + 32, "Largest block", m.blk_s, m.blk_pct, RAM_COL);

    app_ui_gauge(y0 + 58, "Storage Free", m.sto_s, m.sto_used, STO_COL);

    char us[32]; snprintf(us, sizeof(us), "%lus / %d apps", (unsigned long)m.up, m.apps);
    d.fillRect(10, y0 + 78, 220, 9, BG);                      // clear the uptime line before repaint
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(10, y0 + 78); d.print(us);
}

// ---- two screens ---------------------------------------------------------------------------------
// Panel: three big metric cards (battery / largest block / SD) with a hero number and a gauge, then a
// footer line with uptime + app count. Companion: the same four numbers, glanceable, on the Cardputer.
static void metric_card(int x, int y, int w, int hh, const char *label, const char *val, int pct, unsigned short col)
{
    lg::panel(x, y, w, hh);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(MUTED, cb); d.setTextDatum(top_left);
    d.drawString(label, x + 12, y + 8);
    lg::font(2); d.setTextColor(col, cb); d.setTextDatum(top_right);
    char v[20]; lg::fit(v, sizeof v, val, w - 24);
    d.drawString(v, x + w - 12, y + 6);
    d.setTextDatum(top_left);
    lg::bar(x + 12, y + hh - 14, w - 24, 7, pct, col);
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    char up[20]; { unsigned u = (unsigned)(esp_timer_get_time() / 1000000);
                   if (u >= 3600) snprintf(up, sizeof up, "%uh %um", u / 3600, (u % 3600) / 60);
                   else if (u >= 60) snprintf(up, sizeof up, "%um %us", u / 60, u % 60);
                   else snprintf(up, sizeof up, "%us", u); }
    int y = lg::header("System Status", up, ACCENT);
    d.fillRect(0, y, W, H - y, BG);

    Metrics m; collect(m);
    const int x = 10, w = W - 20, ch = 54, gap = 8;
    int cy = y + 6;
    metric_card(x, cy, w, ch, TR("Batteria", "Battery"), m.bat_s, m.bat < 0 ? 0 : m.bat, BAT_COL);   cy += ch + gap;
    metric_card(x, cy, w, ch, TR("Blocco max", "Largest block"), m.blk_s, m.blk_pct, RAM_COL);       cy += ch + gap;
    metric_card(x, cy, w, ch, TR("Spazio SD", "Storage free"), m.sto_s, 100 - m.sto_used, STO_COL);  cy += ch + gap;

    char foot[40]; snprintf(foot, sizeof foot, TR("%d app registrate", "%d apps registered"), m.apps);
    lg::font(0); d.setTextColor(DIM, BG); d.setTextDatum(top_left);
    d.drawString(foot, x + 2, cy + 2);
    nucleo_touch_commit();                                    // no targets: a tap up top = back (framework fallback)
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("SYSTEM", ACCENT);
    Metrics m; collect(m);
    struct { const char *l; const char *v; unsigned short c; } rows[] = {
        { TR("Batteria", "Battery"),    m.bat_s, BAT_COL },
        { TR("Blocco",   "Block"),      m.blk_s, RAM_COL },
        { TR("SD",       "SD"),         m.sto_s, STO_COL },
    };
    d.setFont(&fonts::Font0); d.setTextSize(1);
    for (int i = 0; i < 3; i++) {
        const int ry = y + 4 + i * 20;
        d.setTextColor(MUTED, BG); d.setCursor(8, ry + 4); d.print(rows[i].l);
        int vw = (int)strlen(rows[i].v) * 12;
        d.setTextSize(2); d.setTextColor(rows[i].c, BG); d.setCursor(232 - vw, ry); d.print(rows[i].v);
        d.setTextSize(1);
    }
    char up[32]; snprintf(up, sizeof up, TR("acceso %lus  %d app", "up %lus  %d apps"), (unsigned long)m.up, m.apps);
    d.setTextColor(DIM, BG); d.setCursor(8, y + 68); d.print(up);
    lg::comp_keys(TR("esc indietro", "esc back"));
}

extern "C" void nucleo_register_sysmon(void)
{
    static const nucleo_app_def_t app = {
        "sysmon", "System Status", "System", "Battery, RAM, storage and uptime",
        'o', 0x4D1F, enter, nullptr, tick, draw, nullptr, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
