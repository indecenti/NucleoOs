#include "sentinel_ble.h"

// AD types we care about (Bluetooth Core Supplement, Part A).
enum { AD_UUID16_INC = 0x02, AD_UUID16_CMP = 0x03, AD_SVC_DATA16 = 0x16, AD_MANUF = 0xFF };

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }

// True if a 16-bit service UUID appears in a UUID list or service-data field.
static int has_uuid16(uint16_t want, uint8_t type, const uint8_t *d, uint8_t dl) {
    if (type == AD_UUID16_INC || type == AD_UUID16_CMP) {
        for (uint8_t i = 0; i + 2 <= dl; i += 2)
            if (le16(d + i) == want) return 1;
    } else if (type == AD_SVC_DATA16) {
        if (dl >= 2 && le16(d) == want) return 1;
    }
    return 0;
}

int sentinel_ble_classify(const uint8_t *adv, size_t len, sentinel_ble_class *out) {
    out->type = SENTINEL_TRK_NONE;
    out->flags = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t l = adv[i];
        if (l == 0) break;                          // end of significant data
        if (i + 1 + l > len) break;                 // truncated AD structure
        uint8_t type = adv[i + 1];
        const uint8_t *d = adv + i + 2;
        uint8_t dl = (uint8_t)(l - 1);

        if (type == AD_MANUF && dl >= 3) {
            uint16_t company = le16(d);
            if (company == 0x004C && dl >= 4) {     // Apple
                uint8_t apple_type = d[2];
                // 0x12 = offline finding ("Find My" separated/lost broadcast) — the
                // signal an unpaired AirTag/FindMy tag emits so anyone can locate it.
                if (apple_type == 0x12) {
                    out->type = SENTINEL_TRK_FINDMY;
                    out->flags |= SENTINEL_F_SEPARATED;
                    return 1;
                }
            } else if (company == 0x0075) {         // Samsung — SmartTag family
                out->type = SENTINEL_TRK_SMARTTAG;
                return 1;
            }
        }
        // Service UUIDs: Samsung SmartTag (0xFD5A/0xFD59), Tile (0xFEED), Chipolo (0xFE33).
        if (has_uuid16(0xFD5A, type, d, dl) || has_uuid16(0xFD59, type, d, dl)) {
            out->type = SENTINEL_TRK_SMARTTAG; return 1;
        }
        if (has_uuid16(0xFEED, type, d, dl)) { out->type = SENTINEL_TRK_TILE; return 1; }
        if (has_uuid16(0xFE33, type, d, dl)) { out->type = SENTINEL_TRK_CHIPOLO; return 1; }

        i += 1 + l;
    }
    return out->type != SENTINEL_TRK_NONE;
}

const char *sentinel_tracker_name(sentinel_tracker_t t) {
    switch (t) {
        case SENTINEL_TRK_FINDMY:   return "Find My (AirTag)";
        case SENTINEL_TRK_SMARTTAG: return "Samsung SmartTag";
        case SENTINEL_TRK_TILE:     return "Tile";
        case SENTINEL_TRK_CHIPOLO:  return "Chipolo";
        default:                    return "unknown";
    }
}
