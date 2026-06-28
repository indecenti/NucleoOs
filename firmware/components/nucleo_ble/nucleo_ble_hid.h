// nucleo_ble_hid — internal HID-over-GATT (HoGP) helpers for nucleo_ble.c. FOR AUTHORIZED TESTING ONLY.
//
// Exposes a standard BLE HID keyboard + consumer-control peripheral: GATT services (HID 0x1812 /
// Device Info 0x180A / Battery 0x180F), advertising as a keyboard (appearance 0x03C1), and input-report
// notifications. nucleo_ble.c owns the NimBLE lifecycle and the GAP event loop; it drives the connection
// state into here via the setters, and calls the senders. Private to the component (header sits next to
// the .c, found via the including file's directory).
#pragma once
#include <stdint.h>
#include <stdbool.h>

struct ble_gap_event;
typedef int (*nucleo_ble_gap_cb)(struct ble_gap_event *event, void *arg);

// Register HID/DIS/BAS GATT services. Call in nucleo_ble_up() AFTER ble_svc_gap_init/ble_svc_gatt_init,
// BEFORE the host task starts. Re-registers cleanly on every up() (the GATT DB is freed on deinit).
int  nucleo_ble_hidsvc_gatt_init(void);

// Advertise as a BLE keyboard (connectable). `cb` is nucleo_ble.c's GAP event handler. Idempotent-ish:
// re-arming restarts advertising with fresh fields.
void nucleo_ble_hidsvc_start_adv(uint8_t own_addr_type, nucleo_ble_gap_cb cb);

// Stop advertising and terminate any active connection (graceful disconnect). `synced` = is the host
// up yet (skip the ble_gap_* calls before sync). The app-close path lets nimble_port_stop/ble_hs_stop
// own the final connection teardown (it waits internally), so no busy-wait here. Safe when idle.
void nucleo_ble_hidsvc_stop(bool synced);

// Connection/subscription state, driven by nucleo_ble.c's GAP handler.
void     nucleo_ble_hidsvc_set_conn(uint16_t conn);     // BLE_HS_CONN_HANDLE_NONE clears
uint16_t nucleo_ble_hidsvc_conn(void);
void     nucleo_ble_hidsvc_set_subscribed_for(uint16_t attr_handle, bool on);  // matches the keyboard report
bool     nucleo_ble_hidsvc_subscribed(void);
bool     nucleo_ble_hidsvc_adv_wanted(void);            // should DISCONNECT re-advertise?
void     nucleo_ble_hidsvc_reset(void);                 // clear all state (call in up()/down())

// Senders (no-op unless connected AND subscribed). Run from the launcher task; NimBLE host is thread-safe.
void nucleo_ble_hidsvc_type(const char *text);          // press+release each ASCII char (US-QWERTY)
void nucleo_ble_hidsvc_tap(uint8_t mod, uint8_t key);   // single key press+release
void nucleo_ble_hidsvc_media(uint8_t bitmap);           // consumer press+release (see nucleo_ble.h actions)
