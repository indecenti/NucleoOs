// Clock app: large readable time + date.
#include "nucleo_app.h"
#include <M5GFX.h>
#include <stdio.h>
#include <time.h>

static int s_last_sec = -1;
static void enter(void) { nucleo_app_set_hint("esc back"); s_last_sec = -1; nucleo_app_request_draw(); }
// The clock changes once per second — redraw then, not at the 5 Hz tick (kills the flicker).
static void tick(void) { time_t n = time(NULL); struct tm *tm = localtime(&n); int s = tm ? tm->tm_sec : 0; if (s != s_last_sec) { s_last_sec = s; nucleo_app_request_draw(); } }

#include "app_gfx.h"

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, 0x0841);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char hhmmss[12], date[32];
    snprintf(hhmmss, sizeof(hhmmss), "%02d:%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0);
    if (tm) strftime(date, sizeof(date), "%a %d %b %Y", tm);
    else    snprintf(date, sizeof(date), "----");   // localtime() failed: no valid date to format

    d.setTextColor(0xFFFF, 0x0841);
    d.setTextSize(3); d.setCursor((240 - (int)strlen(hhmmss) * 18) / 2, top + h / 2 - 24); d.print(hhmmss);
    d.setTextSize(1); d.setTextColor(0x8C71, 0x0841);
    d.setCursor((240 - (int)strlen(date) * 6) / 2, top + h / 2 + 8); d.print(date);

    // Before NTP lands the clock sits near the 1970 epoch; flag it so the shown time
    // isn't mistaken for the real wall-clock. 1672531200 = 2023-01-01 UTC.
    if (now < 1672531200) {
        const char *msg = "waiting for NTP sync...";
        d.setTextColor(0xFD20, 0x0841);   // amber
        d.setCursor((240 - (int)strlen(msg) * 6) / 2, top + h / 2 + 24); d.print(msg);
    }
}

extern "C" void nucleo_register_clock(void)
{
    static const nucleo_app_def_t app = {
        "clock", "Clock", "Tools", "Time and date",
        'c', 0x4D1F, enter, nullptr, tick, draw, nullptr
    };
    nucleo_app_register(&app);
}
