// First-run setup wizard + network bring-up. See docs/setup-wizard.md.
#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// True once the wizard has been completed (reads /system/config/setup.json).
bool nucleo_setup_is_complete(void);

// Run the on-device wizard (blocking, uses nucleo_ui) and persist the result.
void nucleo_setup_run(void);

// Bring up networking from the saved config: STA if configured, else an AP.
esp_err_t nucleo_setup_apply_network(void);

// Fast boot variant: returns in < 200 ms (no blocking connect). Starts the AP immediately
// and lets the background supervisor handle the STA join. Use on the main boot path so
// httpd can start right after and the API is reachable within ~1 s. apply_network() is
// kept for the runtime reconfigure path (Wi-Fi app, first-run wizard).
void nucleo_setup_fast_start(void);

// Stop the background STA auto-reconnect. Security apps that take over the radio call this so the
// disconnect caused by their mode switch doesn't make the device keep probing/re-associating to the
// saved network with its real MAC mid-attack. nucleo_setup_apply_network() restores the intent.
void nucleo_setup_suspend(void);

// Configured device name (hostname / mDNS). Defaults to "nucleo-01".
const char *nucleo_setup_device_name(void);

// Live network status (for /api/status). mode is "sta" | "ap"; ip is "" when not on STA.
const char *nucleo_setup_mode(void);
const char *nucleo_setup_ssid(void);
const char *nucleo_setup_ip(void);

// True once the clock has been set from NTP (for /api/status and the clock UI).
bool nucleo_setup_time_synced(void);

// Set the device clock from an external source (e.g. browser push). No-op if NTP already synced.
void nucleo_setup_set_time(time_t t);

// Live link quality of the joined AP. rssi in dBm (negative; 0 = not associated); channel 1-13.
int nucleo_setup_rssi(void);
int nucleo_setup_channel(void);

// Draw the post-setup "home" screen (connection + app-download info).
void nucleo_setup_show_home(void);

// Choose AP or join an existing Wi-Fi (usable anytime, not just first boot).
void nucleo_setup_choose_network(void);

// Register the network setup apps to the native OS launcher
void nucleo_setup_register_apps(void);

// ---- Non-blocking Wi-Fi control for the native Wi-Fi app (app_wifi.cpp) ------
// scan/join BLOCK briefly (driver calls), so the app runs them on a worker task. Scan results are
// cached; read them via the getters. join() persists the network on success; forget() wipes it.
int         nucleo_setup_scan(void);              // active scan -> cached count (blocking ~1-2s)
int         nucleo_setup_scan_count(void);
const char *nucleo_setup_scan_ssid(int i);
int         nucleo_setup_scan_rssi(int i);        // dBm (negative)
int         nucleo_setup_scan_channel(int i);
int         nucleo_setup_scan_secure(int i);      // 1 = encrypted (not OPEN)
const char *nucleo_setup_scan_auth_label(int i);  // "Open"/"WPA2"/"WPA3"/... (for the web scanner)
bool        nucleo_setup_join(const char *ssid, const char *pass);  // blocking; true if it got an IP
void        nucleo_setup_start_ap(void);          // switch to hotspot (AP) mode now
void        nucleo_setup_stop_ap(void);           // turn AP OFF -> rejoin client (STA) mode (Settings toggle)
void        nucleo_setup_forget(void);            // wipe ALL saved networks, drop to AP

// ---- Known-networks store (multi-network, "real OS" Wi-Fi) -------------------
// NucleoOS remembers every Wi-Fi it has joined (SSID+password, on power-safe flash) and, at boot
// and on any drop, scans and auto-joins the BEST in-range known network (manual priority first,
// then strongest signal) instead of only ever trying the last one. These expose that store to the
// native Wi-Fi app and the web manager.
int         nucleo_setup_net_count(void);            // number of saved networks
const char *nucleo_setup_net_ssid(int i);            // SSID of saved network i
int         nucleo_setup_net_priority(int i);        // 0 = normal; higher = preferred
void        nucleo_setup_net_set_priority(const char *ssid, int prio);  // pin/unpin a saved net
bool        nucleo_setup_net_is_known(const char *ssid);  // true if this SSID is saved (for the scan UI)
bool        nucleo_setup_net_has_password(const char *ssid);  // saved AND has a stored (non-empty) password
void        nucleo_setup_forget_ssid(const char *ssid);   // forget one saved network (reselect/AP if current)
void        nucleo_setup_reconnect_best(void);       // rescan + join the best known network now (blocking)
void        nucleo_setup_set_device_name(const char *name);
const char *nucleo_setup_ap_ssid(void);
const char *nucleo_setup_ap_pass(void);
bool        nucleo_setup_ap_secure(void);                 // true iff the AP is ACTUALLY WPA2 (pass >= 8); UIs must use this, not pass[0]
bool        nucleo_setup_ap_intended(void);               // true iff the hotspot is the USER'S choice (not a transient STA fallback); the Settings toggle must read THIS
void        nucleo_setup_set_ap_ssid(const char *ssid);   // edit hotspot name (persists; applies live)
void        nucleo_setup_set_ap_pass(const char *pass);   // "" = open AP, else WPA2 (8..63 chars)

// Persistence health for diagnostics (/api/diag). Reports which of the three config tiers
// (/cfg LittleFS, NVS, SD mirror) accepted the most recent save — so "settings not saved"
// reports are instantly triageable. tiers_ok is the count [0..3]; 0 means nothing persisted.
typedef struct { bool cfg_ok; bool nvs_ok; bool sd_ok; int tiers_ok; } nucleo_persist_status_t;
void        nucleo_setup_persist_status(nucleo_persist_status_t *out);

#ifdef __cplusplus
}
#endif
