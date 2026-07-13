// System status app: battery, largest free RAM block, storage, uptime — thin gauges.
#include "nucleo_app.h"
#include "app_ui.h"
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
static void enter(void) { nucleo_app_set_hint(TR("esc esci", "esc back")); s_last_up = 0; s_first = true; nucleo_app_request_draw(); }
// The finest-grained value shown is uptime in whole seconds — redraw at 1 Hz, not 5 Hz.
static void tick(void) { uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000); if (up != s_last_up) { s_last_up = up; nucleo_app_request_draw(); } }

#include "app_gfx.h"

static void draw(void)
{
    int top = nucleo_app_content_top();
    int h = nucleo_app_content_height();
    if (s_first) { d.fillRect(0, top, 240, h, BG); s_first = false; }   // clear once, not every second

    int y0 = app_ui_title("System Status", ACCENT, "Live");

    // Real cell level off the ADC (nucleo_power). -1 = no reading yet -> show "--" and an empty
    // bar rather than a fake 100 %. When known, append the terminal voltage for the curious.
    int bat = nucleo_power_battery_pct();
    int mv  = nucleo_power_battery_mv();
    char bs[16];
    if (bat < 0) snprintf(bs, sizeof(bs), "--");
    else if (mv > 0) snprintf(bs, sizeof(bs), "%d%% %d.%02dV", bat, mv / 1000, (mv % 1000) / 10);
    else snprintf(bs, sizeof(bs), "%d%%", bat);
    app_ui_gauge(y0 + 6, "Battery", bs, bat < 0 ? 0 : bat, BAT_COL);

    // Largest contiguous internal block is the number that actually limits allocations on
    // this PSRAM-less chip (heap is fragmented) — far more useful than total free. Scale
    // to 192 KB: roughly the biggest block on a fresh boot, so a full bar means healthy.
    size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    char hs[16]; snprintf(hs, sizeof(hs), "%u KB", (unsigned)(maxBlock / 1024));
    app_ui_gauge(y0 + 32, "Largest block", hs, (int)(maxBlock * 100 / (192 * 1024)), RAM_COL);

    const nucleo_storage_info_t *st = nucleo_storage_info();
    char ss[20]; snprintf(ss, sizeof(ss), "%u GB", (unsigned)(st->free_bytes / 1000000000ULL));
    int used = st->total_bytes ? (int)(100 - st->free_bytes * 100 / st->total_bytes) : 0;
    app_ui_gauge(y0 + 58, "Storage Free", ss, used, STO_COL);

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    char us[32]; snprintf(us, sizeof(us), "%lus / %d apps", (unsigned long)up, nucleo_registry_count());
    d.fillRect(10, y0 + 78, 220, 9, BG);                      // clear the uptime line before repaint
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(10, y0 + 78); d.print(us);
}

extern "C" void nucleo_register_sysmon(void)
{
    static const nucleo_app_def_t app = {
        "sysmon", "System Status", "System", "Battery, RAM, storage and uptime",
        'o', 0x4D1F, enter, nullptr, tick, draw, nullptr
    };
    nucleo_app_register(&app);
}
