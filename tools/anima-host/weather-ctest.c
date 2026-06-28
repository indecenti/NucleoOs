// Host test for the pure weather core (firmware/components/nucleo_weather/weather_wmo.c): WMO code ->
// icon class + Italian label, and the date -> weekday helper. Compiled by weather-check.mjs (MinGW gcc),
// no ESP-IDF / no network. The fetch/parse is device-only and reviewed separately.
#include "nucleo_weather.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

int main(void)
{
    printf("== nucleo_weather host test ==\n");

    // WMO -> icon class
    CHECK(weather_wmo_icon(0)  == WX_SUN,     "wmo 0 sun");
    CHECK(weather_wmo_icon(2)  == WX_PARTLY,  "wmo 2 partly");
    CHECK(weather_wmo_icon(3)  == WX_CLOUD,   "wmo 3 cloud");
    CHECK(weather_wmo_icon(45) == WX_FOG,     "wmo 45 fog");
    CHECK(weather_wmo_icon(51) == WX_DRIZZLE, "wmo 51 drizzle");
    CHECK(weather_wmo_icon(61) == WX_RAIN,    "wmo 61 rain");
    CHECK(weather_wmo_icon(82) == WX_RAIN,    "wmo 82 showers->rain");
    CHECK(weather_wmo_icon(71) == WX_SNOW,    "wmo 71 snow");
    CHECK(weather_wmo_icon(95) == WX_STORM,   "wmo 95 storm");
    CHECK(weather_wmo_icon(123) == WX_UNKNOWN,"wmo unknown");

    // labels (Italian)
    CHECK(strcmp(weather_wmo_label(0), "Sereno") == 0,      "label 0");
    CHECK(strcmp(weather_wmo_label(95), "Temporale") == 0,  "label 95");
    CHECK(weather_wmo_label(123)[0] != 0,                   "label unknown nonempty");

    // weekday (Sakamoto): 2026-06-24 = Wednesday(3), 2026-01-01 = Thursday(4)
    CHECK(weather_dow(2026, 6, 24) == 3, "dow wed");
    CHECK(weather_dow(2026, 1, 1)  == 4, "dow thu");
    CHECK(strcmp(weather_dow_short(3), "Mer") == 0, "dow short Mer");
    CHECK(strcmp(weather_dow_short(0), "Dom") == 0, "dow short Dom");

    // date parse
    {
        int y = 0, m = 0, d = 0;
        CHECK(weather_parse_date("2026-06-24", &y, &m, &d) && y == 2026 && m == 6 && d == 24, "parse date ok");
        CHECK(!weather_parse_date("nope", &y, &m, &d), "parse date reject");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
