// Time-zone table for the device clock (pure C, no ESP-IDF: host-tested by `npm run clockzones:test`).
// Each zone is an IANA id (what browsers report and what /api speaks), an ASCII label for the on-device
// font (no accented glyphs) and the POSIX TZ rule newlib's tzset() understands, DST included.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;      // IANA id, e.g. "Europe/Madrid"
    const char *label;   // ASCII cities, <= 20 chars; the part before the first comma (<= 12) is the row value
    const char *posix;   // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
} clock_zone_t;

#define CLOCK_ZONE_DEFAULT_ID "Europe/Rome"   // what every device ran before the zone could be chosen

int                 clock_zone_count(void);
const clock_zone_t *clock_zone_at(int i);           // NULL when out of range
int                 clock_zone_find(const char *id);   // index of an IANA id (exact, then alias), -1 if unknown
int                 clock_zone_default(void);       // index of CLOCK_ZONE_DEFAULT_ID

#ifdef __cplusplus
}
#endif
