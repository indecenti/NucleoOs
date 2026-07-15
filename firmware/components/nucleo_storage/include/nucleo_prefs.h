// Persistent device preferences (brightness / volume / mute) that must survive a reboot, mirroring
// the way nucleo_i18n persists the system language. The single source of truth is the shared SD
// settings.json — the SAME keys the web Settings app already writes (power.display_brightness,
// power.volume) plus power.muted — so the native and web surfaces agree across a reboot.
//
// This module is a storage LEAF: it only touches the SD + cJSON and exchanges plain values by
// pointer, so it never depends on nucleo_app (brightness owner) or nucleo_audio (volume/mute owner).
// The caller reads the values at boot and pushes them into those owners' live setters, and calls the
// save function from the deliberate user-facing controls (Control Center, native settings, ANIMA).
//
// Persistence survives a SYSTEM UPDATE by construction: an OTA rewrites only the flash app image and
// never the SD, and tools/sd-sync.ps1 excludes settings.json (and all of system/config) from the SD
// payload, so a release never clobbers the user's saved prefs. Provision only creates directories +
// volume.json — it never writes settings.json. The only writer of these keys is nucleo_prefs_save.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the saved prefs from settings.json (call after the SD is mounted). Each out-param is written
// ONLY when its key is present and valid, so pass the current defaults in — a missing key leaves the
// default untouched. Returns true if the file parsed (even if some keys were absent), false if there
// is no readable/parseable settings.json. NULL out-params are allowed.
bool nucleo_prefs_load(int *brightness, int *volume, bool *muted);

// Persist the given prefs into settings.json under the power.* section, read-modify-write so every
// other key the web Settings app owns is preserved. Atomic-ish tmp+rename swap, same as nucleo_i18n.
void nucleo_prefs_save(int brightness, int volume, bool muted);

#ifdef __cplusplus
}
#endif
