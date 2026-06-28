// Evil Portal engine — an authorized captive-portal harness for the Cardputer.
//
// PURPOSE & SCOPE. This is a security-testing tool in the same family Bruce/Marauder ship: it
// stands up an open Wi-Fi access point with a chosen SSID, hijacks DNS so every lookup resolves
// to the device, and serves a sign-in page; whatever a connected client submits is written to
// the SD card. It exists for AUTHORIZED use only — security-awareness demos, auditing networks
// you own or have written permission to test, and CTF/lab exercises. The app UI shows a consent
// banner before it will arm. Operating a rogue AP to harvest third parties' credentials without
// authorization is illegal in most jurisdictions.
//
// While the portal runs it OWNS the radio and port 80: it switches the device to AP mode and
// stops the OS web server. nucleo_evilportal_stop() restores the previous network (via
// nucleo_setup_apply_network) and restarts the OS web UI, so the takeover is fully reversible.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- catalogs (for the app's selection UI) ---------------------------------
// Portal templates: built-in clones first, then any *.html dropped in
// /sd/evilportal/portals/ (so brand-accurate clones can be added without a rebuild).
int         nucleo_evilportal_template_count(void);
const char *nucleo_evilportal_template_name(int idx);   // display name (built-in label or filename)

// Credible SSID presets, resolved to a broadcast-ready string (router-style names get a suffix
// derived from this unit's AP MAC, so "TIM-7AC3F1" looks like a real consumer gateway).
int         nucleo_evilportal_ssid_count(void);
const char *nucleo_evilportal_ssid_preset(int idx);

// ---- lifecycle -------------------------------------------------------------
// Arm: open AP `ssid`, captive DNS, and serve template `template_idx` on :80. Stops the OS web
// server first. ESP_OK on success (then nucleo_evilportal_running() is true).
esp_err_t   nucleo_evilportal_start(const char *ssid, int template_idx);

// Arm in EVIL-TWIN mode: clone a real nearby AP on its own `channel`, exact-BSSID, and continuously
// deauth the real `bssid` so its clients fall onto our twin. `authmode` is the real AP's raw
// wifi_auth_mode_t (from the scan). `coherent`=false opens an OPEN AP (captive-portal trap, but a
// visible security downgrade when the real AP is encrypted); `coherent`=true clones the real WPA2
// security into the beacon so the identity matches (no downgrade tell) — at the cost of a usable
// portal, since association needs the real PSK. Same takeover/restore semantics as _start.
esp_err_t   nucleo_evilportal_start_twin(const char *ssid, int template_idx, const uint8_t bssid[6],
                                         int channel, int authmode, bool coherent);

// Disarm: stop the captive server + DNS (+ twin deauth), restore the saved network, restart the OS
// web server. Idempotent; safe to call from an app's on_exit even if start failed.
void        nucleo_evilportal_stop(void);

// ---- live page cloning (F1) ------------------------------------------------
// Clone the captive-portal login page of a nearby OPEN network into a new SD template, at runtime:
// join `open_ssid`, fetch its served page, stream it to /sd/evilportal/portals/cloned.html. The
// portal's wildcard POST handler captures whatever the cloned page submits, so no HTML rewriting is
// needed. Heavy + SERIALIZED (heavy-work arbiter + exclusive mode + streamed to SD); blocks ≤~20 s.
// Returns bytes saved (>0) or a negative error: -1 portal running, -2 already cloning, -3 bad arg,
// -4 arbiter busy, -10 join/IP failed, -13 no captive portal (real internet), -14/-15 save failed.
int         nucleo_evilportal_clone_page(const char *open_ssid);
bool        nucleo_evilportal_cloning(void);
const char *nucleo_evilportal_clone_name(void);   // template display-name to select after a clone

// ---- live status (polled by the app UI) ------------------------------------
bool          nucleo_evilportal_running(void);
bool          nucleo_evilportal_twin(void);          // true when armed in evil-twin (clone-real) mode
bool          nucleo_evilportal_twin_coherent(void); // true when the twin matches the real WPA2 security (no downgrade)
int           nucleo_evilportal_twin_channel(void);  // channel the twin runs on (0 when not twin)
unsigned long nucleo_evilportal_deauth_frames(void); // deauth/disassoc frames sent at the real AP
const char *nucleo_evilportal_ssid(void);     // SSID currently broadcast ("" when stopped)
int         nucleo_evilportal_clients(void);  // stations associated to the AP right now
int         nucleo_evilportal_captures(void); // credential submissions captured this session
int         nucleo_evilportal_confirmed(void);// captures whose password was re-entered identically (high-confidence)
const char *nucleo_evilportal_last_user(void);// last captured identity field ("" if none yet)
const char *nucleo_evilportal_last_pass(void);// last captured secret field ("" if none yet)
const char *nucleo_evilportal_logpath(void);  // SD path of this session's loot file ("" if none)
unsigned    nucleo_evilportal_uptime_s(void); // seconds since armed (0 when stopped)

// Recent-capture ring for the live UI (i=0 is the most recent; up to a small fixed depth).
int         nucleo_evilportal_recent_count(void);
const char *nucleo_evilportal_recent_user(int i);
const char *nucleo_evilportal_recent_pass(int i);

#ifdef __cplusplus
}
#endif
