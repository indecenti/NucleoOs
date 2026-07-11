// nucleo_ble_stub — no-op implementation of the nucleo_ble.h API, compiled ONLY when CONFIG_BT_ENABLED
// is off (see CMakeLists.txt). Bluetooth is linked out to reclaim ~23 KB of DRAM on this PSRAM-less
// chip: the BLE controller (libbtdm_app) keeps ~18 KB of non-releasable .text mapped in DRAM even when
// idle, which is exactly the contiguous block ANIMA needs at boot. With BT off the OS and the BLE app
// still build and run — the app simply reports the radio absent. Re-enable CONFIG_BT_ENABLED (and remove
// this stub from the build) to restore the real NimBLE stack in nucleo_ble.c.
#include "nucleo_ble.h"

void     nucleo_ble_boot_reclaim(void)        { }              // nothing to release: no controller linked
bool     nucleo_ble_radio_present(void)       { return false; } // UI shows "BLE unavailable (build without BT)"
bool     nucleo_ble_pref_enabled(void)        { return false; }
void     nucleo_ble_set_pref(bool on)         { (void)on; }

bool     nucleo_ble_up(void)                  { return false; }
bool     nucleo_ble_down(void)                { return true; }  // already down -> safe
bool     nucleo_ble_is_up(void)               { return false; }
bool     nucleo_ble_is_synced(void)           { return false; }

void     nucleo_ble_scan_start(void)          { }
void     nucleo_ble_scan_stop(void)           { }
int      nucleo_ble_scan_count(void)          { return 0; }
bool     nucleo_ble_scan_get(int idx, nucleo_ble_dev_t *out) { (void)idx; (void)out; return false; }

void     nucleo_ble_spam_start(nucleo_ble_spam_t target) { (void)target; }
void     nucleo_ble_spam_stop(void)           { }
bool     nucleo_ble_spam_active(void)         { return false; }
uint32_t nucleo_ble_spam_count(void)          { return 0; }

void     nucleo_ble_ibeacon_start(void)       { }
void     nucleo_ble_ibeacon_stop(void)        { }
bool     nucleo_ble_ibeacon_active(void)      { return false; }

void     nucleo_ble_hid_start(void)           { }
void     nucleo_ble_hid_stop(void)            { }
bool     nucleo_ble_hid_active(void)          { return false; }
bool     nucleo_ble_hid_connected(void)       { return false; }
bool     nucleo_ble_hid_ready(void)           { return false; }
void     nucleo_ble_hid_type(const char *text){ (void)text; }
void     nucleo_ble_hid_media(int action)     { (void)action; }
void     nucleo_ble_hid_key(int mod, int keycode) { (void)mod; (void)keycode; }
