// sentinel_track — persistence table that decides when a tracker is "following".
//
// A single sighting of a Tile in a cafe is noise; the same separated FindMy tag
// seen again and again over many minutes, wherever you go, is the stalking
// signal. This keeps a small deduped-by-address table with first/last-seen and a
// hit count, and raises a "following" flag once a tracker persists past a time
// window and hit threshold. Timestamps are injected so it is fully host-testable.
#pragma once
#include "sentinel_ble.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SENTINEL_TRACK_CAP
#define SENTINEL_TRACK_CAP 32
#endif

typedef struct {
    uint8_t            addr[6];
    sentinel_tracker_t type;
    uint8_t            flags;
    int8_t             rssi;
    uint32_t           first_seen;
    uint32_t           last_seen;
    uint16_t           hits;
    uint8_t            following;    // 1 once persistence crosses the threshold
} sentinel_track_entry;

typedef struct {
    sentinel_track_entry e[SENTINEL_TRACK_CAP];
    int      n;
    uint32_t follow_window;          // seconds a tracker must persist to alert
    uint16_t follow_hits;            // min sightings to alert
} sentinel_tracker_table;

void sentinel_track_init(sentinel_tracker_table *t, uint32_t follow_window_s, uint16_t follow_hits);

// Record a sighting. Returns the entry index, or -1 if the table is full.
// Sets/refreshes the entry and recomputes its "following" flag.
int sentinel_track_seen(sentinel_tracker_table *t, const uint8_t addr[6],
                        sentinel_tracker_t type, uint8_t flags, int8_t rssi, uint32_t now);

// Number of entries currently flagged as following.
int sentinel_track_following(const sentinel_tracker_table *t);

// Drop entries not seen within ttl seconds of now (frees slots when you walk away).
void sentinel_track_expire(sentinel_tracker_table *t, uint32_t now, uint32_t ttl);

#ifdef __cplusplus
}
#endif
