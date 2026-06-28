// Host test for the BLE advertisement payload core (firmware/components/nucleo_ble/nucleo_ble_adv.c).
// Compiled + run by ble-check.mjs with MinGW gcc — no ESP-IDF, no NimBLE, no hardware. Proves the AD
// framing (lengths, company IDs, Continuity/Swift Pair/Fast Pair type bytes, model placement, iBeacon
// layout) and the hard <=31-byte invariant BEFORE any advertisement hits the air. FOR AUTHORIZED
// TESTING ONLY — this only validates byte structure, not whether a given OS shows the popup.
#include "nucleo_ble_adv.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// Deterministic byte source so the Apple randomized tail is reproducible in tests.
static uint8_t g = 7;
static uint8_t det_rng(void) { g = (uint8_t)(g * 31u + 17u); return g; }

int main(void)
{
    uint8_t b[64];
    printf("== nucleo_ble_adv host test ==\n");

    // 1) Apple Continuity proximity pairing: fixed structure, rotating model, fixed 31-byte length.
    for (int r = 0; r < BLE_APPLE_MODELS_N * 2; r++) {
        memset(b, 0xAA, sizeof b);
        int n = ble_adv_apple((uint32_t)r, b, det_rng);
        CHECK(n == 31, "apple len 31");
        CHECK(b[0] == 0x1E, "apple AD len 0x1E");
        CHECK(b[1] == 0xFF, "apple manuf-data type");
        CHECK(b[2] == 0x4C && b[3] == 0x00, "apple company id");
        CHECK(b[4] == 0x07 && b[5] == 0x19, "apple continuity proximity-pairing TLV");
        CHECK(b[6] == 0x07, "apple prefix");
        uint16_t model = BLE_APPLE_MODELS[r % BLE_APPLE_MODELS_N];
        CHECK(b[7] == ((model >> 8) & 0xFF) && b[8] == (model & 0xFF), "apple model placement");
        CHECK(b[9] == 0x55, "apple status");
    }
    // tail is zero-filled when rng == NULL (deterministic builds)
    {
        int n = ble_adv_apple(0, b, NULL);
        int tail_zero = 1; for (int i = 10; i < n; i++) if (b[i] != 0) tail_zero = 0;
        CHECK(tail_zero, "apple zero tail when rng NULL");
    }

    // 2) Microsoft Swift Pair: FF 06 00 03 00 80 + display name, AD len consistent.
    for (int r = 0; r < BLE_SWIFT_NAMES_N; r++) {
        int n = ble_adv_swiftpair((uint32_t)r, b);
        const char *nm = BLE_SWIFT_NAMES[r];
        int nl = (int)strlen(nm);
        CHECK(n == 7 + nl, "swiftpair total len");
        CHECK(b[0] == (uint8_t)(6 + nl), "swiftpair AD len");
        CHECK(b[1] == 0xFF, "swiftpair manuf-data type");
        CHECK(b[2] == 0x06 && b[3] == 0x00, "swiftpair microsoft vendor id");
        CHECK(b[4] == 0x03, "swiftpair beacon scenario");
        CHECK(b[6] == 0x80, "swiftpair flags");
        CHECK(memcmp(&b[7], nm, nl) == 0, "swiftpair display name");
    }

    // 3) Google Fast Pair (explicit model): flags + FE2C service data + 3-byte model + tx power.
    {
        uint32_t m = 0xABCDEF;
        int n = ble_adv_fastpair(m, b);
        CHECK(n == 13, "fastpair len 13");
        CHECK(b[0] == 0x02 && b[1] == 0x01 && b[2] == 0x06, "fastpair flags");
        CHECK(b[3] == 0x06 && b[4] == 0x16, "fastpair service-data header");
        CHECK(b[5] == 0x2C && b[6] == 0xFE, "fastpair UUID 0xFE2C");
        CHECK(b[7] == 0xAB && b[8] == 0xCD && b[9] == 0xEF, "fastpair model 3 bytes");
        CHECK(b[10] == 0x02 && b[11] == 0x0A, "fastpair tx power header");
    }

    // 4) Android / Samsung rotate through their model tables (via Fast Pair).
    for (int r = 0; r < BLE_ANDROID_MODELS_N; r++) {
        ble_adv_android((uint32_t)r, b);
        uint32_t m = BLE_ANDROID_MODELS[r];
        CHECK(b[7] == ((m >> 16) & 0xFF) && b[8] == ((m >> 8) & 0xFF) && b[9] == (m & 0xFF), "android model placement");
    }
    for (int r = 0; r < BLE_SAMSUNG_MODELS_N; r++) {
        ble_adv_samsung((uint32_t)r, b);
        uint32_t m = BLE_SAMSUNG_MODELS[r];
        CHECK(b[7] == ((m >> 16) & 0xFF) && b[8] == ((m >> 8) & 0xFF) && b[9] == (m & 0xFF), "samsung model placement");
    }

    // 5) iBeacon: flags + Apple iBeacon prefix + 16-byte UUID + major/minor + measured power.
    {
        int n = ble_adv_ibeacon(b);
        CHECK(n == 30, "ibeacon len 30");
        CHECK(b[0] == 0x02 && b[1] == 0x01 && b[2] == 0x06, "ibeacon flags");
        CHECK(b[3] == 0x1A && b[4] == 0xFF && b[5] == 0x4C && b[6] == 0x00, "ibeacon manuf apple");
        CHECK(b[7] == 0x02 && b[8] == 0x15, "ibeacon type+len");
        CHECK(b[25] == 0x00 && b[26] == 0x01, "ibeacon major");
        CHECK(b[27] == 0x00 && b[28] == 0x01, "ibeacon minor");
        CHECK(b[29] == 0xC5, "ibeacon measured power");
    }

    // 6) HID report map + ASCII->keycode (HoGP keyboard/consumer).
    {
        CHECK(BLE_HID_REPORT_MAP_N == 102, "hid report map 102 bytes");
        CHECK(BLE_HID_REPORT_MAP[BLE_HID_REPORT_MAP_N - 1] == 0xC0, "hid report map ends with End Collection");
        const uint8_t *m = BLE_HID_REPORT_MAP;
        CHECK(m[0] == 0x05 && m[1] == 0x01, "hid map usage page generic desktop");
        CHECK(m[2] == 0x09 && m[3] == 0x06, "hid map usage keyboard");
        CHECK(m[4] == 0xA1 && m[5] == 0x01, "hid map collection app");
        CHECK(m[6] == 0x85 && m[7] == 0x01, "hid map keyboard report id 1");
        // consumer collection must declare Report ID 2 somewhere after the keyboard collection
        int has_consumer_id = 0;
        for (int i = 0; i + 1 < BLE_HID_REPORT_MAP_N; i++)
            if (m[i] == 0x85 && m[i + 1] == 0x02) has_consumer_id = 1;
        CHECK(has_consumer_id, "hid map consumer report id 2");

        ble_hid_keystroke_t k;
        CHECK(ble_hid_ascii('a', &k) && k.mod == 0 && k.key == 0x04, "ascii a");
        CHECK(ble_hid_ascii('z', &k) && k.key == 0x1D, "ascii z");
        CHECK(ble_hid_ascii('A', &k) && k.mod == BLE_HID_MOD_SHIFT && k.key == 0x04, "ascii A shift");
        CHECK(ble_hid_ascii('1', &k) && k.key == 0x1E, "ascii 1");
        CHECK(ble_hid_ascii('0', &k) && k.key == 0x27, "ascii 0");
        CHECK(ble_hid_ascii(' ', &k) && k.key == 0x2C, "ascii space");
        CHECK(ble_hid_ascii('\n', &k) && k.key == 0x28, "ascii newline=enter");
        CHECK(ble_hid_ascii('!', &k) && k.mod == BLE_HID_MOD_SHIFT && k.key == 0x1E, "ascii ! shift1");
        CHECK(!ble_hid_ascii((char)0x01, &k), "ascii control char rejected");
    }

    // 7) HARD invariant: no builder ever exceeds the 31-byte BLE adv limit, across all rotations.
    for (int r = 0; r < 64; r++) {
        CHECK(ble_adv_apple((uint32_t)r, b, det_rng) <= BLE_ADV_MAX, "apple <=31");
        CHECK(ble_adv_swiftpair((uint32_t)r, b)     <= BLE_ADV_MAX, "swiftpair <=31");
        CHECK(ble_adv_android((uint32_t)r, b)       <= BLE_ADV_MAX, "android <=31");
        CHECK(ble_adv_samsung((uint32_t)r, b)       <= BLE_ADV_MAX, "samsung <=31");
    }
    CHECK(ble_adv_ibeacon(b) <= BLE_ADV_MAX, "ibeacon <=31");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
