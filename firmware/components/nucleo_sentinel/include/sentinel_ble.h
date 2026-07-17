// sentinel_ble — classify a raw BLE advertisement as a known location tracker.
//
// Pure parsing over the public advertising formats (Apple FindMy / offline
// finding, Samsung SmartTag service UUID, Tile service UUID). DEFENSIVE ONLY:
// this reads advertisements that trackers broadcast to anyone in range so the
// operator can spot a device that may be following them. It never transmits.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENTINEL_TRK_NONE = 0,
    SENTINEL_TRK_FINDMY,    // Apple offline-finding accessory (AirTag or FindMy tag)
    SENTINEL_TRK_SMARTTAG,  // Samsung SmartTag / SmartTag2
    SENTINEL_TRK_TILE,      // Tile
    SENTINEL_TRK_CHIPOLO,   // Chipolo (own service uuid)
} sentinel_tracker_t;

// Classification flags.
#define SENTINEL_F_SEPARATED 0x01   // broadcasting in lost/separated mode (findable = stalking risk)

typedef struct {
    sentinel_tracker_t type;
    uint8_t            flags;
} sentinel_ble_class;

// Classify a concatenated AD-structure payload (adv[len]). Returns 1 and fills
// out when a tracker is recognised, 0 otherwise.
int sentinel_ble_classify(const uint8_t *adv, size_t len, sentinel_ble_class *out);

// Human-readable tracker name.
const char *sentinel_tracker_name(sentinel_tracker_t t);

#ifdef __cplusplus
}
#endif
