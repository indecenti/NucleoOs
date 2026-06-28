// weather_wmo — pure WMO weather-code mapping + calendar helpers. No deps, host-tested (weather-check.mjs).
#include "nucleo_weather.h"
#include <stdio.h>

wx_icon_t weather_wmo_icon(int code)
{
    switch (code) {
        case 0:                                   return WX_SUN;
        case 1: case 2:                           return WX_PARTLY;
        case 3:                                   return WX_CLOUD;
        case 45: case 48:                         return WX_FOG;
        case 51: case 53: case 55: case 56: case 57: return WX_DRIZZLE;
        case 61: case 63: case 65: case 66: case 67:
        case 80: case 81: case 82:                return WX_RAIN;
        case 71: case 73: case 75: case 77: case 85: case 86: return WX_SNOW;
        case 95: case 96: case 99:                return WX_STORM;
        default:                                  return WX_UNKNOWN;
    }
}

const char *weather_wmo_label(int code)
{
    switch (code) {
        case 0:  return "Sereno";
        case 1:  return "Quasi sereno";
        case 2:  return "Poco nuvoloso";
        case 3:  return "Nuvoloso";
        case 45: return "Nebbia";
        case 48: return "Nebbia gelata";
        case 51: case 53: case 55: return "Pioviggine";
        case 56: case 57: return "Pioviggine gelata";
        case 61: return "Pioggia debole";
        case 63: return "Pioggia";
        case 65: return "Pioggia forte";
        case 66: case 67: return "Pioggia gelata";
        case 71: return "Neve debole";
        case 73: return "Neve";
        case 75: return "Neve forte";
        case 77: return "Nevischio";
        case 80: case 81: return "Rovesci";
        case 82: return "Rovesci forti";
        case 85: case 86: return "Rovesci di neve";
        case 95: return "Temporale";
        case 96: case 99: return "Temporale e grandine";
        default: return "—";
    }
}

// Sakamoto's algorithm: weekday for a Gregorian date. 0=Sunday..6=Saturday.
int weather_dow(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

const char *weather_dow_short(int dow)
{
    static const char *N[] = { "Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab" };
    return (dow >= 0 && dow < 7) ? N[dow] : "—";
}

bool weather_parse_date(const char *iso, int *y, int *m, int *d)
{
    return iso && sscanf(iso, "%d-%d-%d", y, m, d) == 3;
}
