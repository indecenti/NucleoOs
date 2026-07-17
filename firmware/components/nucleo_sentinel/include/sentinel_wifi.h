// sentinel_wifi — parse 802.11 management frames and flag active attacks around
// you. DEFENSIVE ONLY: fed from a passive promiscuous capture, it never injects.
// Detects deauth floods, broadcast deauth, evil-twin APs (same SSID from a new
// BSSID), and beacon floods.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SENTINEL_WF_OTHER = 0,
    SENTINEL_WF_DEAUTH,
    SENTINEL_WF_DISASSOC,
    SENTINEL_WF_BEACON,
    SENTINEL_WF_PROBE_REQ,
};

typedef struct {
    uint8_t kind;          // SENTINEL_WF_*
    uint8_t to_broadcast;  // addr1 is ff:ff:ff:ff:ff:ff
    uint8_t bssid[6];      // addr3
    char    ssid[33];      // beacon SSID (empty if none / hidden)
} sentinel_wifi_frame;

// Parse one raw 802.11 frame (starting at the frame-control field). Returns 1 on
// a recognised management frame, 0 otherwise.
int sentinel_wifi_parse(const uint8_t *f, size_t len, sentinel_wifi_frame *out);

// Anomaly flags (sticky, raised by the monitor).
#define SENTINEL_A_DEAUTH_FLOOD     0x01
#define SENTINEL_A_BROADCAST_DEAUTH 0x02
#define SENTINEL_A_EVIL_TWIN        0x04
#define SENTINEL_A_BEACON_FLOOD     0x08

#ifndef SENTINEL_SSID_TABLE
#define SENTINEL_SSID_TABLE 24
#endif

typedef struct {
    uint32_t window_s;
    uint16_t deauth_thresh;      // deauth frames per window that count as a flood
    uint16_t beacon_thresh;      // distinct beacons per window that count as a flood
    uint32_t win_start;
    uint16_t deauth_ct;
    uint16_t beacon_ct;
    struct { char ssid[33]; uint8_t bssid[6]; } seen[SENTINEL_SSID_TABLE];
    int      nseen;
    uint8_t  alerts;             // accumulated SENTINEL_A_* flags
} sentinel_wifi_mon;

void sentinel_wifi_mon_init(sentinel_wifi_mon *m, uint32_t window_s,
                            uint16_t deauth_thresh, uint16_t beacon_thresh);

// Feed a parsed frame with the current time. Returns the flags newly raised by
// this frame (0 if none); m->alerts accumulates every flag ever raised.
uint8_t sentinel_wifi_mon_feed(sentinel_wifi_mon *m, const sentinel_wifi_frame *fr, uint32_t now);

#ifdef __cplusplus
}
#endif
