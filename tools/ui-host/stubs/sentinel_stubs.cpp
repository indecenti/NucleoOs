// ui-host stub for app_sentinel.cpp — a simulated BLE radio with a handful of nearby trackers so the
// two-screen (list + details) and companion layouts render with real content instead of an empty state.
// FICTIONAL data only (made-up addresses / types). Defensive app: it never transmits.
#include "nucleo_sentinel.h"
#include <string.h>
#include <stdio.h>

extern "C" {

// Radio comes up.
bool nucleo_ble_radio_present(void) { return true; }
bool nucleo_ble_up(void)            { return true; }
bool nucleo_ble_is_synced(void)     { return true; }
void nucleo_ble_down(void)          {}
void nucleo_ble_scan_start(void)    {}
void nucleo_ble_scan_stop(void)     {}

// Tracker engine: four nearby trackers, one of them following.
void sentinel_tracker_start(void) {}
void sentinel_tracker_stop(void)  {}
void sentinel_tracker_tick(void)  {}

struct Fake { int type; unsigned char flags; signed char rssi; unsigned short hits; unsigned age; unsigned char following; };
static const Fake FAKES[] = {
    { 0, 0x00, -42,  58,  642, 1 },   // AirTag / FindMy — following you
    { 1, 0x01, -67,  12,  188, 0 },   // Samsung SmartTag — separated (lost mode)
    { 2, 0x00, -74,   6,   41, 0 },   // Tile
    { 3, 0x00, -81,   3,   12, 0 },   // Chipolo
};
static const int NFAKE = (int)(sizeof(FAKES) / sizeof(FAKES[0]));

int sentinel_tracker_count(void)     { return NFAKE; }
int sentinel_tracker_following(void) { int n = 0; for (auto &f : FAKES) n += f.following; return n; }

bool sentinel_tracker_get(int idx, sentinel_view_t *out)
{
    if (idx < 0 || idx >= NFAKE || !out) return false;
    const Fake &f = FAKES[idx];
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 6; i++) out->addr[i] = (unsigned char)(0x10 * (idx + 1) + i);
    out->type = f.type; out->flags = f.flags; out->rssi = f.rssi;
    out->hits = f.hits; out->age_s = f.age; out->following = f.following;
    return true;
}

const char *sentinel_type_name(int type)
{
    static const char *N[] = { "AirTag/FindMy", "Samsung SmartTag", "Tile", "Chipolo", "BLE tracker" };
    if (type < 0 || type > 3) type = 4;
    return N[type];
}

}  // extern "C"
