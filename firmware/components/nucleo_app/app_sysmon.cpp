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
#include <stdlib.h>
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
static void sample(void);
static void tick(void) { uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000); if (up != s_last_up) { s_last_up = up; sample(); nucleo_app_request_draw(); } }

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
// Panel (lg::frame, no flicker on the 1 Hz refresh): three ring gauges across the top - battery, largest free
// block, SD used - each with its hero number inside, then a live graph of the last minute of RAM (free heap as an
// area, largest block as a line), sampled once a second from tick() so it keeps running with no draw at all.
// Companion: the same numbers, glanceable, on the Cardputer. No touch content: the chevron is Back.
// lg::frame composes in the shared 8-bit canvas (RGB332): a slate that survives it for the card and tracks.
static const unsigned short CARD = 0x212A;
#define NHIST 60
static uint8_t s_blk_h[NHIST];                       // largest block, KB / 2, oldest first (s_hn samples)
static int s_hn = 0;
static void sample(void)
{
    if (s_hn == NHIST) memmove(s_blk_h, s_blk_h + 1, --s_hn);
    const unsigned bk = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 2048);
    s_blk_h[s_hn++] = (uint8_t)(bk > 255 ? 255 : bk);
}

static void ring(int cx, int cy, int pct, unsigned short col, const char *val, const char *unit, const char *label)
{
    d.fillArc(cx, cy, 40, 32, 0, 360, CARD);
    if (pct > 0) d.fillArc(cx, cy, 40, 32, 270, 270 + (pct > 100 ? 100 : pct) * 36 / 10, col);
    d.setTextDatum(middle_center);
    lg::font(2); d.setTextColor(col, BG); d.drawString(val, cx, cy - 4);
    lg::font(4); d.setTextColor(MUTED, BG); d.drawString(unit, cx, cy + 14);
    lg::font(0); d.setTextColor(FG, BG); d.drawString(label, cx, cy + 54);
    d.setTextDatum(top_left);
}

static void paint_large(void *)
{
    char up[20]; { unsigned u = (unsigned)(esp_timer_get_time() / 1000000);
                   if (u >= 3600) snprintf(up, sizeof up, "%uh %um", u / 3600, (u % 3600) / 60);
                   else if (u >= 60) snprintf(up, sizeof up, "%um %us", u / 60, u % 60);
                   else snprintf(up, sizeof up, "%us", u); }
    d.fillRect(0, 0, 320, 240, BG);
    lg::header("System Status", up, ACCENT);
    Metrics m; collect(m);
    char v[3][8];
    if (m.bat < 0) snprintf(v[0], 8, "--"); else snprintf(v[0], 8, "%d%%", m.bat);
    snprintf(v[1], 8, "%u", m.blk_kb);
    snprintf(v[2], 8, "%d", atoi(m.sto_s));
    ring(56,  80, m.bat, BAT_COL, v[0], m.bat >= 0 && m.mv > 0 ? m.bat_s + strlen(v[0]) + 1 : "", TR("Batteria", "Battery"));
    ring(160, 80, m.blk_pct, RAM_COL, v[1], "KB", TR("Blocco max", "Largest block"));
    ring(264, 80, 100 - m.sto_used, STO_COL, v[2], "GB", TR("SD libera", "SD free"));

    // the last minute of the largest block (the number that limits every allocation here)
    const int gx = 10, gy = 148, gw = 300, gh = 84, px = gx + 8, py = gy + 22, pw = gw - 16, ph = gh - 30;
    lg::panel(gx, gy, gw, gh, CARD);
    lg::font(0); d.setTextColor(MUTED, CARD); d.drawString(TR("Blocco max, ultimo minuto", "Largest block, last minute"), px, gy + 4);
    int top = 1; for (int i = 0; i < s_hn; i++) if (s_blk_h[i] > top) top = s_blk_h[i];
    d.drawFastHLine(px, py + ph, pw, LINE);
    for (int i = 0; i < s_hn; i++) {
        const int x0 = px + i * pw / NHIST, hh = s_blk_h[i] * ph / top;
        d.fillRect(x0, py + ph - hh, px + (i + 1) * pw / NHIST - x0 - 1, hh, RAM_COL);
    }
}

static void draw_large(void)
{
    if (!s_hn) sample();
    lg::frame(paint_large, nullptr, 0, 0, true);   // exact theme colours (RGB565 strips)
    nucleo_touch_commit();
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
