// Private platform interface for the heavy-work arbiter core (nucleo_arb.c).
//
// The core is pure logic; everything OS-specific is one of these five hooks. There are exactly
// two implementations:
//   * nucleo_arb_plat_esp.c  — device: FreeRTOS mutex, esp_timer, heap_caps, eventbus  (#ifndef ARB_HOST)
//   * tools/arb-host/arb_plat_host.c — host: Win32 CRITICAL_SECTION, GetTickCount64, mock heap (#ifdef ARB_HOST)
//
// The lock guards ONLY the arbiter's own fields and is held for microseconds — never across a
// caller's heavy work. So taking it with an unbounded wait on the httpd task cannot stall it.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Create the platform lock (and any other one-time setup). Idempotent.
void arb_plat_init(void);

// Short critical section over the arbiter's state fields.
void arb_plat_lock(void);
void arb_plat_unlock(void);

// Monotonic milliseconds. Wrap is fine: the core only ever uses (now - earlier) deltas.
uint32_t arb_plat_now_ms(void);

// Cooperative sleep used by the bounded acquire() wait loop. ms may be 0 (yield).
void arb_plat_sleep_ms(uint32_t ms);

// Current free internal heap, in bytes. Recorded at release as a teardown-leak sentinel.
size_t arb_plat_heap_free(void);

// Called OUTSIDE the lock on every busy 0->1 (busy=true) and 1->0 (busy=false) transition.
// Device: publishes a "system.busy" event so the shell can show a banner / gate client apps.
// Host: records the transition for the test. `job` is the holder label on acquire, NULL on release.
void arb_plat_on_busy(bool busy, const char *job);
