// nucleo_weather — geolocated weather for the device, free & key-less (Open-Meteo + ip-api geolocation).
//
// Split like the other features: a PURE core (WMO weather-code -> icon/label, day-of-week) that is
// host-tested, and the device-only fetch (HTTP/TLS + cJSON) that fills the model. The app draws the
// model with vector icons. Energy-first: nothing polls in the background; a fetch happens only on open
// or on the user's refresh, and the resolved location is cached to SD so a cold open is one call.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- pure: WMO weather code -> icon class + Italian label, and date -> weekday -----------------
typedef enum {
    WX_SUN, WX_PARTLY, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM, WX_UNKNOWN
} wx_icon_t;

wx_icon_t   weather_wmo_icon(int code);
const char *weather_wmo_label(int code);                 // Italian, short
int         weather_dow(int year, int month, int day);   // 0=Sun..6=Sat (Sakamoto)
const char *weather_dow_short(int dow);                  // "Dom".."Sab"
bool        weather_parse_date(const char *iso, int *y, int *m, int *d);   // "YYYY-MM-DD"

// ---- model (filled by the device fetch) --------------------------------------------------------
#define WX_MAX_DAYS 6
typedef struct {
    bool   valid;
    char   place[40];
    double lat, lon;
    // current
    double temp, feels, wind;     // C, C, km/h
    int    humidity, wind_dir;    // %, deg
    int    code, is_day;          // WMO code, 1=day
    double precip_mm;             // current precipitation
    char   updated[6];            // "HH:MM" local
    // daily
    int    n_days;
    struct { int code, precip_prob, dow; double tmax, tmin; } day[WX_MAX_DAYS];
} nucleo_weather_t;

// ---- device fetch (no-op stubs on host) --------------------------------------------------------
// Resolve location from the public IP (ip-api, plain HTTP) into place/lat/lon. Cached to SD.
bool nucleo_weather_locate_ip(nucleo_weather_t *w);
// Resolve a typed city name via Open-Meteo geocoding (HTTPS) into place/lat/lon.
bool nucleo_weather_locate_city(const char *city, nucleo_weather_t *w);
// Fetch current + daily forecast for w->lat/lon (HTTPS, Open-Meteo) into the model. Keeps place.
bool nucleo_weather_fetch(nucleo_weather_t *w);
// Load/save the resolved location (place+lat+lon) to /sd so a cold open is one fetch, not two.
bool nucleo_weather_cache_load(nucleo_weather_t *w);
void nucleo_weather_cache_save(const nucleo_weather_t *w);

#ifdef __cplusplus
}
#endif
