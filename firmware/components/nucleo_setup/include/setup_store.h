// Three-tier persistence for nucleo_setup's config documents (setup.json, networks.json).
// See docs/setup-wizard.md "Persistence" and "Reset".
//
// Every document is written to three INDEPENDENT tiers, each best-effort so one failure never
// blocks the others, and read back in reliability order (the first tier that answers wins):
//   1. /cfg LittleFS  — primary, power-loss-safe, works on an SD-less device (needs our partition table)
//   2. NVS            — present in every ESP-IDF app; the fallback when /cfg and the SD are both absent
//   3. SD mirror      — survives an internal-flash wipe / reflash and is human-visible on the card
//
// Plain C over stdio + NVS only (no Wi-Fi, no FreeRTOS on the host), so the REAL store compiles on the
// PC: tools/anima-host/setup-store-check.mjs (`npm run setupstore:test`).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETUP_STORE_NVS_NS "nucleocfg"   // NVS namespace holding the documents (<=15 chars)

// Read a whole small file (capped at 32 KB). malloc'd, NUL-terminated (caller frees) or NULL.
char *setup_store_slurp(const char *path);

// Fan one document out to every available tier. Returns how many tiers accepted it [0..3].
// Returns 0 without writing anything once the store is sealed (setup_store_seal).
int setup_store_persist(const char *cfg_path, const char *sd_path, const char *nvs_key, const char *text);

// Load a document in reliability order (/cfg -> NVS -> SD). *from_fallback (optional) is set when it did
// NOT come from /cfg, so the caller can re-persist and heal the other tiers. malloc'd (caller frees).
char *setup_store_load(const char *cfg_path, const char *sd_path, const char *nvs_key, bool *from_fallback);

// Tier health of the most recent setup_store_persist(); tiers_ok is -1 until the first save.
void setup_store_status(bool *cfg_ok, bool *nvs_ok, bool *sd_ok, int *tiers_ok);

// Factory-reset latch. Once sealed, every later setup_store_persist() is refused until reboot, so a
// background save (the Wi-Fi supervisor joining a network, the hotspot minting its default password)
// can never re-create a document the reset just erased. Waits for a save already in flight to finish
// (bounded, ~2 s). Returns false if that save was still running when the wait gave up — it may land
// after the caller's erase, so the reset must not claim success. Irreversible by design.
bool setup_store_seal(void);
bool setup_store_sealed(void);

// Erase one document from every tier: the /cfg file, the SD mirror (and their .tmp leftovers) and the
// NVS key. A tier that is absent or already empty counts as erased. Returns true when no tier still
// holds the document afterwards. Call setup_store_seal() first, or a concurrent save may rewrite it.
bool setup_store_erase(const char *cfg_path, const char *sd_path, const char *nvs_key);

#ifdef __cplusplus
}
#endif
