// Power management for the battery-powered Cardputer. See docs/memory-budget.md.
#pragma once

#include <stdbool.h>   // bool used in the battery-gauge API below (C translation units need it)

#ifdef __cplusplus
extern "C" {
#endif

// Enable dynamic frequency scaling (DFS): the CPU runs at full speed under load and
// drops to a low idle frequency automatically, cutting draw on the 120 mAh cell without
// hurting responsiveness. Safe to call once at boot, after Wi-Fi is up. Light sleep is
// intentionally left off (the device must stay reachable over HTTP).
void nucleo_power_init(void);

// ---- Battery gauge (M5Cardputer: VBAT through a 2:1 divider on G10 / ADC1) --------------
// Bare M5GFX gives us no power-management IC, so — like Bruce — we read the cell voltage off the
// ADC ourselves and map it LINEARLY to a percentage (0% at 3300 mV, 100% at 4100 mV, same as
// Bruce's getBattery()). Deliberately simple: a calibrated multi-sample read (the IDF equivalent of
// Bruce's analogReadMilliVolts), a ~2 s cache so the UI can poll every frame for free, and an honest
// "unknown" (-1) state before the first reading. No SoC curve or smoothing — with no fuel-gauge IC
// and no charge-detect line this is a coarse estimate by nature (and reads ~full on USB).

// True once the ADC channel has been brought up successfully and at least one reading exists.
bool nucleo_power_battery_available(void);

// Smoothed state-of-charge, 0..100. Returns -1 when no battery reading is available yet
// (ADC not ready, or the very first sample hasn't landed) so callers can draw an honest
// "unknown" glyph rather than inventing a level.
int nucleo_power_battery_pct(void);

// Smoothed battery terminal voltage in millivolts (already de-divided). -1 if unavailable.
int nucleo_power_battery_mv(void);

#ifdef __cplusplus
}
#endif
