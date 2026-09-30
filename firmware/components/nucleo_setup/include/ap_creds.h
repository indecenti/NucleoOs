// Hotspot (soft-AP) credential lifecycle: per-device defaults, the deliberate OPEN choice, and their
// setup.json members. Pure C (cJSON only; MAC + RNG injected), so the host gate compiles THIS file:
// tools/anima-host/setup-store-check.mjs (`npm run setupstore:test`).
//
// An empty password used to mean two things at once — "not initialised yet" (mint a per-device random
// WPA2 key) and "the user chose an open hotspot" — and ensure() could not tell them apart, so every AP
// (re)start re-minted a password over the user's open choice. `open` now carries that choice
// explicitly and is persisted as "ap_open". It is not a secret, so it also survives into the SD mirror
// (which never holds "ap_pass"): a flash-wipe recovery keeps an open hotspot open, and re-mints a
// secured one.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AP_CREDS_SSID_PREFIX "NucleoOS"         // factory default name prefix; a per-device suffix is appended
#define AP_CREDS_PASS_LEGACY "nucleoos"         // the OLD shared default password — upgraded to a per-device random one
#define AP_CREDS_SSID_LEGACY "NucleoOS-Setup"   // the OLD shared default SSID — upgraded to a per-device name

typedef struct {
    char ssid[33];
    char pass[64];   // "" && !open: not initialised (ensure() mints) — "" && open: deliberately OPEN
    bool open;       // the user chose an open hotspot; never true while pass is non-empty
} ap_creds_t;

// Fill per-device factory defaults on first use and upgrade the old shared defaults. `mac` is the
// SoftAP MAC (its last two bytes suffix the SSID — not secret, just avoids two units colliding);
// `rnd` is the hardware RNG. The password is RANDOM, never MAC-derived: the firmware is open source and
// the AP broadcasts its MAC as the BSSID. Leaves a deliberately open hotspot open. Returns true when
// anything changed, so the caller persists it once.
bool ap_creds_ensure(ap_creds_t *c, const uint8_t mac[6], uint32_t (*rnd)(void));

// "" = deliberately OPEN; 8..63 chars = WPA2-PSK. 1..7 (the driver would silently start OPEN) or
// > 63 (would be truncated into a key nobody typed) is rejected: returns false, *c unchanged.
bool ap_creds_set_pass(ap_creds_t *c, const char *p);

// True iff the AP is ACTUALLY secured (WPA2 needs 8..63 chars). UIs must read this, not pass[0].
bool ap_creds_secure(const ap_creds_t *c);

// setup.json members: "ap_ssid", "ap_pass" (stripped from the SD mirror), "ap_open". load() only takes
// a non-empty SSID and keeps RAM values for absent members, except `open`, which is re-derived every
// time (absent = false; a stored password always wins over a stale open flag). save() returns false
// on OOM (the caller then persists nothing).
void ap_creds_load(ap_creds_t *c, const cJSON *doc);
bool ap_creds_save(const ap_creds_t *c, cJSON *doc);

#ifdef __cplusplus
}
#endif
