// System status app: battery, largest free RAM block, storage, uptime — thin gauges.
#include "nucleo_app.h"
#include "app_ui.h"
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

static uint32_t s_last_up = 0;
static void enter(void) { nucleo_app_set_hint("esc back"); s_last_up = 0; nucleo_app_request_draw(); }
// The finest-grained value shown is uptime in whole seconds — redraw at 1 Hz, not 5 Hz.
static void tick(void) { uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000); if (up != s_last_up) { s_last_up = up; nucleo_app_request_draw(); } }

#include "app_gfx.h"

static void gauge(int y, const char *label, const char *val, int pct, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(0x8C71, 0x0841);
    d.setCursor(10, y); d.print(label);
    
    d.setTextSize(2); d.setTextColor(col, 0x0841);
    int vw = strlen(val) * 12;
    d.setCursor(230 - vw, y - 6); d.print(val);
    
    d.drawRoundRect(10, y + 12, 220, 5, 2, 0x2945);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    d.fillRoundRect(10, y + 12, 220 * pct / 100, 5, 2, col);
}

static void draw(void)
{
    int top = nucleo_app_content_top();
    int h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, 0x0841);

    int y0 = app_ui_title("System Status", 0x4ED3, "Live");

    // Real cell level off the ADC (nucleo_power). -1 = no reading yet -> show "--" and an empty
    // bar rather than a fake 100 %. When known, append the terminal voltage for the curious.
    int bat = nucleo_power_battery_pct();
    int mv  = nucleo_power_battery_mv();
    char bs[16];
    if (bat < 0) snprintf(bs, sizeof(bs), "--");
    else if (mv > 0) snprintf(bs, sizeof(bs), "%d%% %d.%02dV", bat, mv / 1000, (mv % 1000) / 10);
    else snprintf(bs, sizeof(bs), "%d%%", bat);
    gauge(y0 + 6, "Battery", bs, bat < 0 ? 0 : bat, 0x4ED3);

    // Largest contiguous internal block is the number that actually limits allocations on
    // this PSRAM-less chip (heap is fragmented) — far more useful than total free. Scale
    // to 192 KB: roughly the biggest block on a fresh boot, so a full bar means healthy.
    size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    char hs[16]; snprintf(hs, sizeof(hs), "%u KB", (unsigned)(maxBlock / 1024));
    gauge(y0 + 32, "Largest block", hs, (int)(maxBlock * 100 / (192 * 1024)), 0x4D9F);

    const nucleo_storage_info_t *st = nucleo_storage_info();
    char ss[20]; snprintf(ss, sizeof(ss), "%u GB", (unsigned)(st->free_bytes / 1000000000ULL));
    int used = st->total_bytes ? (int)(100 - st->free_bytes * 100 / st->total_bytes) : 0;
    gauge(y0 + 58, "Storage Free", ss, used, 0xFD20);

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    char us[32]; snprintf(us, sizeof(us), "%lus / %d apps", (unsigned long)up, nucleo_registry_count());
    d.setTextSize(1); d.setTextColor(0x8C71, 0x0841);
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
