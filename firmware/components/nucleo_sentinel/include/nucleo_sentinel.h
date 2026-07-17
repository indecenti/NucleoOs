// nucleo_sentinel — device-facing defensive service.
//
// Tracker detection (BLE) piggybacks on nucleo_ble's passive scan: every
// advertisement is classified by the pure core (sentinel_ble) and folded into a
// persistence table (sentinel_track) that flags a device as "following" once it
// sticks around too long. Anti-stalking, fully passive — it never transmits.
//
// Because this chip runs no Wi-Fi/BT coexistence, tracker (BLE) mode and the
// Wi-Fi airspace monitor are separate exclusive modes, never both at once.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tracker (BLE) service. Call start after nucleo_ble_up()+scan_start(); the
// observer then fills the table. tick() ages out trackers you walked away from.
void sentinel_tracker_start(void);
void sentinel_tracker_stop(void);
void sentinel_tracker_tick(void);

int  sentinel_tracker_count(void);      // trackers currently in range
int  sentinel_tracker_following(void);  // how many are flagged as following you

typedef struct {
    uint8_t  addr[6];
    int      type;        // sentinel_tracker_t value
    uint8_t  flags;       // SENTINEL_F_*
    int8_t   rssi;
    uint16_t hits;
    uint32_t age_s;       // seconds since first seen
    uint8_t  following;
} sentinel_view_t;

bool sentinel_tracker_get(int idx, sentinel_view_t *out);
const char *sentinel_type_name(int type);

// Airspace (Wi-Fi) monitor. Separate mode from tracker detection because this
// chip runs no Wi-Fi/BT coexistence. Passively watches 802.11 management frames
// (via nucleo_wifiatk's promiscuous path, no .pcap) and flags active attacks.
void    sentinel_airspace_start(void);
void    sentinel_airspace_stop(void);
uint8_t sentinel_airspace_alerts(void);   // SENTINEL_A_* bitmask raised this session

#ifndef SENTINEL_A_DEAUTH_FLOOD   // mirror of sentinel_wifi.h flags for UI code
#define SENTINEL_A_DEAUTH_FLOOD     0x01
#define SENTINEL_A_BROADCAST_DEAUTH 0x02
#define SENTINEL_A_EVIL_TWIN        0x04
#define SENTINEL_A_BEACON_FLOOD     0x08
#endif

#ifdef __cplusplus
}
#endif
