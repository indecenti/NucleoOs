// Time-zone table (see clock_zones.h). Ordered west to east so cycling the Settings row walks the globe.
// Rules are the 2026 ones: Mexico (2022), Paraguay (2024) and most of Brazil keep no DST.
#include "clock_zones.h"
#include <string.h>

static const clock_zone_t ZONES[] = {
    { "Pacific/Honolulu",               "Honolulu",           "HST10" },
    { "America/Anchorage",              "Anchorage",          "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles",            "Los Angeles",        "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Tijuana",                "Tijuana",            "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",                "Phoenix",            "MST7" },
    { "America/Denver",                 "Denver",             "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Chicago",                "Chicago",            "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City",            "CDMX, Monterrey",    "CST6" },
    { "America/Guatemala",              "Guatemala, C. Rica", "CST6" },
    { "America/Bogota",                 "Bogota, Lima, Quito","<-05>5" },
    { "America/Cancun",                 "Cancun, Panama",     "EST5" },
    { "America/Havana",                 "La Habana",          "CST5CDT,M3.2.0/0,M11.1.0/1" },
    { "America/New_York",               "New York, Toronto",  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Caracas",                "Caracas, La Paz",    "<-04>4" },
    { "America/Santo_Domingo",          "S. Domingo, PR",     "AST4" },
    { "America/Santiago",               "Santiago CL",        "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { "America/Argentina/Buenos_Aires", "Buenos Aires",       "<-03>3" },
    { "America/Montevideo",             "Montevideo",         "<-03>3" },
    { "America/Asuncion",               "Asuncion",           "<-03>3" },
    { "America/Sao_Paulo",              "Sao Paulo",          "<-03>3" },
    { "Atlantic/Azores",                "Azores",             "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
    { "UTC",                            "UTC",                "UTC0" },
    { "Europe/London",                  "London, Dublin",     "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon",                  "Lisboa",             "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Canary",                "Canarias",           "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Africa/Casablanca",              "Casablanca",         "<+01>-1" },
    { "Africa/Lagos",                   "Lagos, Malabo",      "WAT-1" },
    { "Europe/Madrid",                  "Madrid",             "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",                   "Paris, Bruxelles",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",                    "Roma",               "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",                  "Berlin, Wien",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",                  "Zurich",             "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",                  "Warszawa, Praha",    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Johannesburg",            "Johannesburg",       "SAST-2" },
    { "Africa/Cairo",                   "Cairo",              "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Europe/Athens",                  "Athens, Bucuresti",  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kyiv",                    "Kyiv",               "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul",                "Istanbul",           "<+03>-3" },
    { "Europe/Moscow",                  "Moskva",             "MSK-3" },
    { "Africa/Nairobi",                 "Nairobi",            "EAT-3" },
    { "Asia/Dubai",                     "Dubai",              "<+04>-4" },
    { "Asia/Karachi",                   "Karachi",            "PKT-5" },
    { "Asia/Kolkata",                   "India",              "IST-5:30" },
    { "Asia/Dhaka",                     "Dhaka",              "<+06>-6" },
    { "Asia/Bangkok",                   "Bangkok, Jakarta",   "<+07>-7" },
    { "Asia/Shanghai",                  "Beijing, Shanghai",  "CST-8" },
    { "Asia/Singapore",                 "Singapore",          "<+08>-8" },
    { "Asia/Manila",                    "Manila",             "PST-8" },
    { "Australia/Perth",                "Perth",              "AWST-8" },
    { "Asia/Tokyo",                     "Tokyo",              "JST-9" },
    { "Asia/Seoul",                     "Seoul",              "KST-9" },
    { "Australia/Brisbane",             "Brisbane",           "AEST-10" },
    { "Australia/Sydney",               "Sydney, Melbourne",  "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland",               "Auckland",           "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define NZONES ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

// Ids a browser may report that share a rule with a listed zone (Intl.DateTimeFormat gives the
// country's own id, not ours). Unknown ids are refused: guessing a rule would set a wrong clock.
static const struct { const char *alias, *id; } ALIASES[] = {
    { "Etc/UTC", "UTC" },                    { "Etc/GMT", "UTC" },                    { "GMT", "UTC" },
    { "Europe/Kiev", "Europe/Kyiv" },        { "Asia/Calcutta", "Asia/Kolkata" },     { "America/Buenos_Aires", "America/Argentina/Buenos_Aires" },
    { "America/Lima", "America/Bogota" },    { "America/Guayaquil", "America/Bogota" },
    { "America/La_Paz", "America/Caracas" }, { "America/Puerto_Rico", "America/Santo_Domingo" },
    { "America/Panama", "America/Cancun" },  { "America/Toronto", "America/New_York" },
    { "America/Costa_Rica", "America/Guatemala" }, { "America/El_Salvador", "America/Guatemala" },
    { "America/Tegucigalpa", "America/Guatemala" }, { "America/Managua", "America/Guatemala" },
    { "America/Monterrey", "America/Mexico_City" }, { "America/Merida", "America/Mexico_City" },
    { "Africa/Malabo", "Africa/Lagos" },     { "Africa/Ceuta", "Europe/Madrid" },     { "Europe/Dublin", "Europe/London" },
    { "Atlantic/Madeira", "Europe/Lisbon" }, { "Europe/Brussels", "Europe/Paris" },   { "Europe/Amsterdam", "Europe/Paris" },
    { "Europe/Luxembourg", "Europe/Paris" }, { "Europe/Monaco", "Europe/Paris" },     { "Europe/Vatican", "Europe/Rome" },
    { "Europe/San_Marino", "Europe/Rome" },  { "Europe/Malta", "Europe/Rome" },       { "Europe/Vienna", "Europe/Berlin" },
    { "Europe/Copenhagen", "Europe/Berlin" },{ "Europe/Stockholm", "Europe/Berlin" }, { "Europe/Oslo", "Europe/Berlin" },
    { "Europe/Busingen", "Europe/Zurich" },  { "Europe/Prague", "Europe/Warsaw" },    { "Europe/Budapest", "Europe/Warsaw" },
    { "Europe/Bratislava", "Europe/Warsaw" },{ "Europe/Ljubljana", "Europe/Warsaw" }, { "Europe/Zagreb", "Europe/Warsaw" },
    { "Europe/Belgrade", "Europe/Warsaw" },  { "Europe/Bucharest", "Europe/Athens" }, { "Europe/Sofia", "Europe/Athens" },
    { "Europe/Helsinki", "Europe/Athens" },  { "Asia/Jakarta", "Asia/Bangkok" },      { "Asia/Ho_Chi_Minh", "Asia/Bangkok" },
    { "Asia/Hong_Kong", "Asia/Shanghai" },   { "Asia/Taipei", "Asia/Shanghai" },      { "Asia/Kuala_Lumpur", "Asia/Singapore" },
    { "Australia/Melbourne", "Australia/Sydney" }, { "Australia/Canberra", "Australia/Sydney" },
    { "America/Vancouver", "America/Los_Angeles" },
};

int clock_zone_count(void) { return NZONES; }

const clock_zone_t *clock_zone_at(int i) { return (i >= 0 && i < NZONES) ? &ZONES[i] : NULL; }

int clock_zone_find(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < NZONES; i++) if (!strcmp(ZONES[i].id, id)) return i;
    for (int i = 0; i < (int)(sizeof(ALIASES) / sizeof(ALIASES[0])); i++)
        if (!strcmp(ALIASES[i].alias, id)) return clock_zone_find(ALIASES[i].id);
    return -1;
}

int clock_zone_default(void) { return clock_zone_find(CLOCK_ZONE_DEFAULT_ID); }
