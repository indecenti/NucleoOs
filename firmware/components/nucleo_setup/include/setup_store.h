// Three-tier persistence for nucleo_setup's config documents (setup.json, networks.json).
// See docs/setup-wizard.md "Persistence" and "Reset".
//
// Config MUST survive a reboot on ANY install, including the worst case a user can hit: a firmware
// loaded through a third-party launcher that lacks our custom partition table (so the /cfg LittleFS
// store never mounts) AND with no SD card inserted. Every save therefore fans out to three INDEPENDENT
// tiers, each best-effort so one failure never blocks the others:
//   1. /cfg LittleFS  — primary, power-loss-safe, works on an SD-less device (needs our partition table)
//   2. NVS            — present in every ESP-IDF app; the fallback when /cfg and the SD are both absent
//   3. SD mirror      — survives an internal-flash wipe / reflash and is human-visible on the card
// Loads read /cfg -> NVS -> SD; the first tier that answers wins.
//
// SECRETS NEVER REACH THE SD. The card is removable, unencrypted FAT, and /api/fs/read serves it to any
// paired client or app — whoever pulls it (or browses it) would read every saved Wi-Fi password. Each
// document names its secret member (setup.json: "ap_pass"; networks.json: each net's "pass"), and the SD
// copy is the document with every member of that name removed, at any depth. /cfg and NVS keep the full
// document. So the one scenario the mirror exists for — /cfg AND NVS both lost to a flash wipe — restores
// "setup complete", the device name, the mode and every saved SSID + priority, but no password: the user
// re-enters each Wi-Fi password once (both Wi-Fi UIs prompt for a secured saved network that has none)
// and the hotspot mints a fresh random password, shown on the device screen. Encrypting the SD copy with
// a key kept in NVS would buy nothing: that key dies in the very wipe that makes the mirror useful.
//
// Settings ▸ Reset / Factory reset go through setup_store_seal() + setup_store_erase(): every tier, not
// just the SD (which /cfg and NVS would heal right back).
//
// Plain C over stdio + NVS + cJSON (no Wi-Fi; FreeRTOS only for the seal's bounded wait), so the REAL
// store compiles on the PC: tools/anima-host/setup-store-check.mjs (`npm run setupstore:test`).
#pragma once
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETUP_STORE_NVS_NS "nucleocfg"   // NVS namespace holding the documents (<=15 chars)

typedef struct {
    const char *cfg_path;   // tier 1: /cfg LittleFS — full document
    const char *nvs_key;    // tier 2: NVS key in SETUP_STORE_NVS_NS — full document
    const char *sd_path;    // tier 3: SD mirror — document minus `secret`
    const char *secret;     // member name stripped (any depth, case-insensitive) from the SD copy
} setup_doc_t;

typedef enum { SETUP_TIER_NONE = 0, SETUP_TIER_CFG, SETUP_TIER_NVS, SETUP_TIER_SD } setup_tier_t;

// Persistence health of the most recent save, for /api/diag. tiers_ok is -1 until the first save.
typedef struct { bool cfg_ok; bool nvs_ok; bool sd_ok; int tiers_ok; } setup_store_status_t;

// Read a whole small file. Capped at 32 KB: the size comes from ftell on a file that may be corrupt or
// hand-placed on the SD, and a multi-MB malloc on the ~18 KB heap would OOM the boot. Malloc'd and
// NUL-terminated (caller frees), or NULL.
char *setup_store_slurp(const char *path);

// Persist `doc` to every available tier and return how many accepted it (0 = no tier available, or the
// store is sealed by a reset — see setup_store_sealed()), or -1 when the document could not even be
// printed (OOM: no tier touched, status left as it was).
// TAKES OWNERSHIP of `doc`: it is printed in full, redacted in place, printed again for the SD, and
// freed BEFORE any write, so the tree never sits on the heap next to the flash/SD writes. When the
// redacted print or the SD write fails the full text is still never written there, and a card copy that
// an older firmware left holding the secret is deleted instead of kept.
int setup_store_save(const setup_doc_t *d, cJSON *doc);

// Load in reliability order. Returns the malloc'd text (caller frees) or NULL; *tier says which tier
// answered. *sd_dirty is set when the SD mirror still holds the secret in plaintext (written by an older
// firmware, or dropped on the card by hand): the caller must parse the returned text into RAM and then
// re-save, which heals /cfg + NVS and rewrites the card redacted. The returned text keeps the secret, so
// a legacy SD-only recovery still migrates it into the internal tiers before the card is scrubbed.
// The card probe streams the file through a 64 B stack chunk — no heap copy on the boot path. A legacy
// "<sd_path>.tmp" holding the secret (older firmware, power cut mid-write) is removed.
char *setup_store_load(const setup_doc_t *d, setup_tier_t *tier, bool *sd_dirty);

void setup_store_status(setup_store_status_t *out);

// Factory-reset latch. Once sealed, every later setup_store_save() is refused until reboot, so a
// background save (the Wi-Fi supervisor joining a network, the hotspot minting its default password)
// can never re-create a document the reset just erased. Waits for a save already in flight to finish
// (bounded, ~2 s). Returns false if that save was still running when the wait gave up — it may land
// after the caller's erase, so the reset must not claim success. Irreversible by design.
bool setup_store_seal(void);
bool setup_store_sealed(void);

// Erase one document from every tier: the /cfg file, the SD mirror (and their .tmp leftovers) and the
// NVS key. A tier that is absent or already empty counts as erased. Returns true when no tier still
// holds the document afterwards. Call setup_store_seal() first, or a concurrent save may rewrite it.
bool setup_store_erase(const setup_doc_t *d);

// Exposed for the host gate: does `txt` hold a JSON member named `key` (any depth, case-insensitive)?
// A string VALUE equal to the key — an SSID literally named "pass" — is not a member and doesn't match.
bool setup_store_has_member(const char *txt, const char *key);

#ifdef __cplusplus
}
#endif
