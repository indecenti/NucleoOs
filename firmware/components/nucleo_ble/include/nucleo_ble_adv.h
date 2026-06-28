// nucleo_ble_adv — pure BLE advertisement payload builders (NO NimBLE/ESP deps), shared by the
// firmware engine (nucleo_ble.c) and the host gate (tools/anima-host/ble-ctest.c). FOR AUTHORIZED
// TESTING ONLY. The byte-layouts are transcriptions of the PUBLIC advertising formats the respective
// OSes turn into pairing popups (Apple Continuity / Microsoft Swift Pair / Google Fast Pair). Keeping
// them pure lets the gate assert the AD framing (lengths, company IDs, type bytes, model placement,
// and the hard <=31-byte invariant) on the PC before anything hits the air.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BLE_ADV_MAX 31

// Returns a random byte; pass NULL to zero-fill the randomized tail (deterministic, for tests).
typedef uint8_t (*ble_rng_fn)(void);

// Each writes its AD bytes into b[] (>= BLE_ADV_MAX) and returns the length.
int ble_adv_apple(uint32_t rot, uint8_t *b, ble_rng_fn rng);   // Apple Continuity proximity pairing (31)
int ble_adv_swiftpair(uint32_t rot, uint8_t *b);               // Microsoft Swift Pair (variable)
int ble_adv_fastpair(uint32_t model, uint8_t *b);              // Google Fast Pair, explicit 3-byte model (13)
int ble_adv_android(uint32_t rot, uint8_t *b);                 // Fast Pair, generic model table
int ble_adv_samsung(uint32_t rot, uint8_t *b);                 // Fast Pair, Galaxy Buds model table
int ble_adv_ibeacon(uint8_t *b);                               // standards iBeacon, fixed demo UUID (30)

// Model/name tables exposed so the gate can assert the rotation picks the right entry.
extern const uint16_t      BLE_APPLE_MODELS[];   extern const int BLE_APPLE_MODELS_N;
extern const uint32_t      BLE_ANDROID_MODELS[]; extern const int BLE_ANDROID_MODELS_N;
extern const uint32_t      BLE_SAMSUNG_MODELS[]; extern const int BLE_SAMSUNG_MODELS_N;
extern const char *const   BLE_SWIFT_NAMES[];    extern const int BLE_SWIFT_NAMES_N;

// ---- HID (HoGP) pure data: report map + ASCII->keycode, shared by firmware (nucleo_ble_hid.c) and the
// host gate. The descriptor declares Report ID 1 = boot-layout keyboard, Report ID 2 = consumer/media.
extern const uint8_t BLE_HID_REPORT_MAP[];  extern const int BLE_HID_REPORT_MAP_N;

#define BLE_HID_MOD_SHIFT 0x02   // Left Shift (USB HID modifier bitmap)

typedef struct { uint8_t mod; uint8_t key; } ble_hid_keystroke_t;   // USB HID page 0x07 keycode + modifier
bool ble_hid_ascii(char c, ble_hid_keystroke_t *out);   // US-QWERTY; false if not typeable

// Consumer (media) report bitmap bits, Report ID 2 (single byte).
#define BLE_HID_MEDIA_PLAYPAUSE 0x01
#define BLE_HID_MEDIA_NEXT      0x02
#define BLE_HID_MEDIA_PREV      0x04
#define BLE_HID_MEDIA_VOLUP     0x08
#define BLE_HID_MEDIA_VOLDN     0x10
#define BLE_HID_MEDIA_MUTE      0x20
