// Wi-Fi offensive toolkit — clean-room ESP-IDF re-implementation of the "WiFi Atks" family that
// firmwares like Bruce/Marauder ship. Built only on the public esp_wifi / esp_netif primitives so
// NucleoOS stays MIT (no GPL/AGPL source is copied — Bruce is used only as a behaviour reference).
//
// PURPOSE & SCOPE. AUTHORIZED USE ONLY: security-awareness demos, auditing networks you own or
// have written permission to test, and CTF/lab exercises. Jamming/deauthing third-party networks
// without authorization is illegal in most jurisdictions; the app UI gates every attack behind a
// consent screen.
//
// First feature: Deauth Flood. While running it OWNS the radio — it switches to STA + promiscuous,
// channel-hops across the targeted APs and TX-floods 802.11 deauth/disassoc management frames, and
// it stops the OS web server first. nucleo_wifiatk_deauth_stop() restores the saved network (via
// nucleo_setup_apply_network) and restarts the OS web UI, so the takeover is fully reversible.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- target discovery (for the app's selection UI) -------------------------
// Run a blocking AP scan and cache the result. Returns the number of unique APs found (capped).
int         nucleo_wifiatk_scan(void);
int         nucleo_wifiatk_target_count(void);
const char *nucleo_wifiatk_target_ssid(int i);    // "" out of range; "(hidden)" for cloaked APs
int         nucleo_wifiatk_target_channel(int i);  // 1..14, 0 out of range
int         nucleo_wifiatk_target_rssi(int i);     // dBm (negative), 0 out of range
const char *nucleo_wifiatk_target_bssid(int i);    // "AA:BB:CC:DD:EE:FF" (shared static buffer)
void        nucleo_wifiatk_target_bssid_bytes(int i, unsigned char out[6]);  // raw BSSID (zeros if out of range)
const char *nucleo_wifiatk_target_auth(int i);     // short auth label: "open","WPA2","WPA3","OWE"...
int         nucleo_wifiatk_target_authmode(int i); // raw wifi_auth_mode_t (so a twin can clone the security)
const char *nucleo_wifiatk_target_fingerprint(int i);// "WPA2 CCMP bgn WPS" — beacon identity for twin coherence
bool        nucleo_wifiatk_target_protected(int i);// true = PMF/WPA3 => deauth-immune (flood-all skips it)
int         nucleo_wifiatk_targets_vulnerable(void);// count of scanned APs that are NOT PMF-protected

// ---- deauth flood lifecycle ------------------------------------------------
// Arm. target_idx >= 0 floods a single AP; target_idx < 0 floods ALL scanned APs (channel-hopping).
// Stops the OS web server first. ESP_OK on success (then nucleo_wifiatk_deauth_running() is true);
// ESP_ERR_INVALID_STATE if no APs have been scanned yet.
esp_err_t   nucleo_wifiatk_deauth_start(int target_idx);

// Disarm: stop the flood task, leave promiscuous mode, restore the saved network, restart the OS
// web server. Idempotent; safe to call from an app's on_exit even if start failed.
void        nucleo_wifiatk_deauth_stop(void);

// ---- live status (polled by the app UI) ------------------------------------
bool          nucleo_wifiatk_deauth_running(void);
unsigned long nucleo_wifiatk_frames(void);        // total mgmt frames TX'd this session
int           nucleo_wifiatk_clients(void);       // associated stations discovered (deauth targets)
int           nucleo_wifiatk_clients_active(void);// stations still on the air (seen recently); the rest are likely kicked
int           nucleo_wifiatk_inject_health(void); // % of TX attempts the driver accepted (low = chip dropping frames)
unsigned      nucleo_wifiatk_reconnects(void);    // observed forced reconnects (a silenced client came back)
int           nucleo_wifiatk_cur_rssi(void);      // live signal (dBm) of the AP being hit (proximity feedback)
int           nucleo_wifiatk_targets_active(void);// APs being hit (1 for single, N for flood-all)
int           nucleo_wifiatk_cur_channel(void);   // channel currently being flooded
const char   *nucleo_wifiatk_cur_ssid(void);      // SSID currently being flooded ("" when idle)
unsigned      nucleo_wifiatk_uptime_s(void);      // seconds since armed (0 when stopped)

// ---- beacon spam -----------------------------------------------------------
// Flood the air with fake beacon frames (a wall of bogus SSIDs). Owns the radio like the deauth
// flood. ESP_ERR_INVALID_STATE if the deauth flood is already running. Three modes:
#define NUCLEO_BEACON_FUNNY  0   // curated wall of plausible/joke SSIDs (mixed open + WPA2)
#define NUCLEO_BEACON_RANDOM 1   // a session of randomly generated, real-looking SSIDs
#define NUCLEO_BEACON_CLONE  2   // twins of the real APs around you (runs a scan first)
esp_err_t     nucleo_wifiatk_beacon_start(int mode);
void          nucleo_wifiatk_beacon_stop(void);
bool          nucleo_wifiatk_beacon_running(void);
unsigned long nucleo_wifiatk_beacon_frames(void);   // beacon frames TX'd this session
int           nucleo_wifiatk_beacon_count(void);    // number of fake SSIDs broadcast
int           nucleo_wifiatk_beacon_mode(void);     // active mode (NUCLEO_BEACON_*)
int           nucleo_wifiatk_beacon_health(void);   // % of beacon TX attempts the driver accepted
unsigned      nucleo_wifiatk_beacon_uptime_s(void); // seconds since armed (0 when stopped)

#ifdef __cplusplus
}
#endif
