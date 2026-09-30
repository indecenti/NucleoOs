// nucleo_guest — NucleoOS running as a guest of M5Launcher (bmorcelli/Launcher).
//
// ONE binary for both worlds. Stand-alone (web flasher / esptool / M5Burner full image) NucleoOS owns
// the whole flash: its own bootloader, partitions.csv, A/B OTA + rollback. Installed BY M5Launcher it
// runs inside Launcher's partition table, next to other apps, under Launcher's bootloader. The mode is
// detected at runtime from the live partition table (guest_policy.c) — nothing to configure, and the
// stand-alone behaviour is untouched. What changes when hosted:
//   - self-OTA is refused (the "next" OTA slot is another app's); updates come from Launcher's OTA
//   - Settings > Device gains "Back to M5Launcher" (a timer deep sleep, which Launcher's bootloader
//     routes to the Launcher, when its settings allow it)
// See docs/m5launcher.md for the Launcher boot model this relies on.
#pragma once
#include <stdbool.h>
#include "guest_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

// True when installed by M5Launcher. Scans the partition table once, then cached. Safe from any task.
bool nucleo_guest_hosted(void);

// Self-OTA (native Updates app, POST /api/ota) is safe only on the stand-alone layout.
bool nucleo_guest_self_ota_allowed(void);

// How the user can get back to the Launcher right now (reads Launcher's own NVS settings, read-only).
// Meaningful only when hosted.
guest_return_t nucleo_guest_return_mode(void);

// Hand the device back to M5Launcher. With GUEST_RET_DEEP_SLEEP it enters a ~100 ms timer deep sleep
// and never returns (the wake reset boots the Launcher). Returns false — doing nothing — when not
// hosted or when the Launcher's settings make that impossible (caller shows the power-cycle / hold-key
// hint from nucleo_guest_return_mode()).
bool nucleo_guest_return_to_launcher(void);

#ifdef __cplusplus
}
#endif
