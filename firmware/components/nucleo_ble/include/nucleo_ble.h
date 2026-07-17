// nucleo_ble — BLE radio for the Security > BLE suite (scan / spam / iBeacon; HID later).
//
// FOR AUTHORIZED TESTING ONLY. Same posture as the existing Wi-Fi/Ethernet offensive apps: this is a
// security-research tool for networks/devices you own or are permitted to test.
//
// The S3 has a BLE-only controller. Bringing it up claims a sizeable heap chunk, so every BLE app runs
// INSIDE nucleo_exclusive with Wi-Fi torn down (NX_DEEP_OFFLINE): no Wi-Fi/BT coexistence on this
// PSRAM-less chip, all the reclaimed RAM goes to the controller. up() inits the NimBLE controller+host;
// down() stops the host and DEINITS it, returning that RAM so the OS comes back exactly as before.
// Re-entrant: up/down/up is safe (we never mem_release). Only ONE mode (scan OR spam OR iBeacon) runs
// at a time — starting one stops the others.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    char    name[24];   // adv "complete/short name", "" if none
} nucleo_ble_dev_t;

// Spam targets: the OS whose pairing popup the advertisement mimics.
typedef enum {
    NUCLEO_BLE_SPAM_IOS,        // Apple Continuity proximity-pairing ("AirPods" card)
    NUCLEO_BLE_SPAM_ANDROID,    // Google Fast Pair (generic registered model IDs)
    NUCLEO_BLE_SPAM_WINDOWS,    // Microsoft Swift Pair
    NUCLEO_BLE_SPAM_SAMSUNG,    // Fast Pair with Samsung Galaxy Buds model IDs
    NUCLEO_BLE_SPAM_ALL,        // rotate through all of the above
} nucleo_ble_spam_t;

// RAM reclaim. The BLE controller reserves ~tens of KB of DRAM even while idle. Bluetooth is OFF by
// default (a rarely-used Security tool) so the OS/ANIMA gets that RAM back. Call nucleo_ble_boot_reclaim()
// ONCE at boot, before any BLE use: if the persisted preference is OFF it releases the BLE controller
// memory to the heap (IRREVERSIBLE until reboot). Toggling the preference ON + rebooting keeps the radio.
void nucleo_ble_boot_reclaim(void);
bool nucleo_ble_radio_present(void);   // false once boot_reclaim freed the radio (BLE unavailable until enabled + reboot)
bool nucleo_ble_pref_enabled(void);    // persisted on/off preference (NVS), default OFF
void nucleo_ble_set_pref(bool on);     // persist the preference; takes effect on the NEXT boot
// Keep the BLE controller for the NEXT boot ONLY (transient, RTC), without flipping
// the persisted pref. A BLE app that reboots into a Solo session calls this first so
// it comes up with BLE, then the boot after (on exit) releases the RAM again.
void nucleo_ble_keep_next_boot(void);
// True if THIS boot kept BLE via the one-shot (a dedicated BLE Solo boot) — app_main
// then skips Wi-Fi bringup so the NimBLE controller has the whole heap. Valid after
// nucleo_ble_boot_reclaim() has run.
bool nucleo_ble_kept_once(void);

// Controller + NimBLE host lifecycle. up() returns false if the controller won't init (not enough
// contiguous heap, or the radio memory was released for RAM). down() is safe to call when already down.
bool nucleo_ble_up(void);
bool nucleo_ble_down(void);   // false if the stack wouldn't tear down (RAM NOT reclaimed — don't restart Wi-Fi)
bool nucleo_ble_is_up(void);
bool nucleo_ble_is_synced(void);   // host sync done — ops only work once true

// Passive observer scan. Results accumulate in an internal, deduped-by-address table the UI polls.
void nucleo_ble_scan_start(void);
void nucleo_ble_scan_stop(void);
int  nucleo_ble_scan_count(void);
bool nucleo_ble_scan_get(int idx, nucleo_ble_dev_t *out);   // false if idx out of range

// Raw-advertisement observer for DEFENSIVE scanning (Sentinel tracker detect).
// Invoked on every discovery event with the full adv payload while a scan runs,
// so a classifier can spot location trackers. Runs in the NimBLE host task —
// keep the callback short and non-blocking. Pass NULL to detach.
typedef void (*nucleo_ble_adv_cb_t)(const uint8_t addr[6], uint8_t addr_type,
                                    int8_t rssi, const uint8_t *adv, uint8_t adv_len, void *ctx);
void nucleo_ble_set_adv_observer(nucleo_ble_adv_cb_t cb, void *ctx);

// Advertisement spam: rotates payload + a fresh random MAC every ~40 ms (anti-fingerprint).
void     nucleo_ble_spam_start(nucleo_ble_spam_t target);
void     nucleo_ble_spam_stop(void);
bool     nucleo_ble_spam_active(void);
uint32_t nucleo_ble_spam_count(void);   // advertisements emitted this session (UI counter)

// Plain iBeacon broadcast (fixed demo UUID/major/minor) — a benign, standards-compliant beacon.
void nucleo_ble_ibeacon_start(void);
void nucleo_ble_ibeacon_stop(void);
bool nucleo_ble_ibeacon_active(void);

// BLE HID keyboard (HID-over-GATT): advertise as a keyboard, pair (Just Works), then type / send media.
void nucleo_ble_hid_start(void);
void nucleo_ble_hid_stop(void);
bool nucleo_ble_hid_active(void);       // HID mode selected
bool nucleo_ble_hid_connected(void);    // a host is connected
bool nucleo_ble_hid_ready(void);        // connected AND subscribed — keystrokes will land
void nucleo_ble_hid_type(const char *text);   // type an ASCII string (US-QWERTY)
void nucleo_ble_hid_media(int action);  // 0=play/pause 1=next 2=prev 3=vol+ 4=vol- 5=mute
void nucleo_ble_hid_key(int mod, int keycode);   // one raw keystroke (HID modifier bits + usage id)

#ifdef __cplusplus
}
#endif
