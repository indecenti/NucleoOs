// Host stub for app_weather.cpp: a fetch that "succeeds" with a fictional forecast, so the gallery renders the
// Now / Forecast tabs with data instead of "Unavailable". Fictional place and numbers only.
#include "nucleo_weather.h"
#include <stdio.h>
#include <string.h>

extern "C" {
bool nucleo_weather_cache_load(nucleo_weather_t *w)
{
    snprintf(w->place, sizeof w->place, "Lago Verde");
    w->lat = 45.5; w->lon = 9.2;
    return true;
}
void nucleo_weather_cache_save(const nucleo_weather_t *) {}
bool nucleo_weather_locate_ip(nucleo_weather_t *w) { return nucleo_weather_cache_load(w); }
bool nucleo_weather_locate_city(const char *, nucleo_weather_t *) { return false; }
bool nucleo_weather_fetch(nucleo_weather_t *w)
{
    w->valid = true;
    w->temp = 18.4; w->feels = 17.1; w->wind = 12; w->humidity = 64; w->code = 2; w->is_day = 1;
    snprintf(w->updated, sizeof w->updated, "10:42");
    static const int code[6] = { 2, 61, 3, 0, 95, 71 }, pp[6] = { 20, 80, 40, 0, 90, 60 };
    static const double hi[6] = { 21, 16, 18, 29, 14, 3 }, lo[6] = { 11, 9, 10, 15, 8, -2 };
    w->n_days = 6;
    for (int i = 0; i < 6; i++) {
        w->day[i].code = code[i]; w->day[i].precip_prob = pp[i]; w->day[i].dow = (2 + i) % 7;
        w->day[i].tmax = hi[i]; w->day[i].tmin = lo[i];
    }
    return true;
}
}
