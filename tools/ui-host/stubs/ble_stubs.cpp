// ui-host stub for app_ble.cpp — a simulated NimBLE radio: a handful of nearby devices for the Scan list,
// a running spam counter, and a connected HID host, so the two-screen menu / scan / spam / iBeacon / HID
// layouts render with real content. FICTIONAL data only (made-up names + addresses).
#include "nucleo_ble.h"
#include <string.h>
#include <stdio.h>

extern "C" {

bool nucleo_ble_radio_present(void) { return true; }
bool nucleo_ble_up(void)            { return true; }
bool nucleo_ble_down(void)          { return true; }
bool nucleo_ble_is_synced(void)     { return true; }

void nucleo_ble_scan_start(void) {}
void nucleo_ble_scan_stop(void)  {}

struct FakeDev { const char *name; signed char rssi; unsigned char a2, a1, a0; };
static const FakeDev DEVS[] = {
    { "Galaxy Buds2",   -48, 0x3A, 0x7C, 0x11 },
    { "",               -55, 0x5E, 0x90, 0x42 },
    { "Mi Band 7",      -61, 0xA1, 0x0B, 0x7F },
    { "JBL Flip 6",     -66, 0xC4, 0x22, 0x18 },
    { "",               -70, 0x09, 0xDD, 0x33 },
    { "Tile Mate",      -77, 0x71, 0x6A, 0x05 },
    { "ESP-Sensor",     -83, 0x24, 0x8E, 0x9C },
};
static const int NDEV = (int)(sizeof(DEVS) / sizeof(DEVS[0]));

int  nucleo_ble_scan_count(void) { return NDEV; }
bool nucleo_ble_scan_get(int idx, nucleo_ble_dev_t *out)
{
    if (idx < 0 || idx >= NDEV || !out) return false;
    const FakeDev &d = DEVS[idx];
    memset(out, 0, sizeof *out);
    out->addr[5] = 0xDE; out->addr[4] = 0xAD; out->addr[3] = 0x00;
    out->addr[2] = d.a2; out->addr[1] = d.a1; out->addr[0] = d.a0;
    out->rssi = d.rssi;
    snprintf(out->name, sizeof out->name, "%s", d.name);
    return true;
}

void     nucleo_ble_spam_start(nucleo_ble_spam_t) {}
void     nucleo_ble_spam_stop(void) {}
uint32_t nucleo_ble_spam_count(void) { return 1287; }

void nucleo_ble_ibeacon_start(void)  {}
void nucleo_ble_ibeacon_stop(void)   {}
bool nucleo_ble_ibeacon_active(void) { return true; }

void nucleo_ble_hid_start(void)      {}
void nucleo_ble_hid_stop(void)       {}
bool nucleo_ble_hid_active(void)     { return true; }
bool nucleo_ble_hid_connected(void)  { return true; }
bool nucleo_ble_hid_ready(void)      { return true; }
void nucleo_ble_hid_type(const char *) {}
void nucleo_ble_hid_media(int)       {}

}  // extern "C"
