#pragma once
#include <stddef.h>

// Size of the in-RAM log ring (a permanent .bss buffer — see nucleo_log.c). Public so that the
// /api/logs reader (nucleo_httpd.c) sizes its snapshot buffer to match and the two shrink in
// lockstep. Override with -DNUCLEO_LOG_RING_SZ=<bytes> to trade diagnostic history for heap.
// 2048 still captures a full boot + a query cycle; recovered SRAM is handed to the ANIMA L1 cache.
#ifndef NUCLEO_LOG_RING_SZ
#define NUCLEO_LOG_RING_SZ 2048
#endif

#ifdef __cplusplus
extern "C" {
#endif

// System logger. Captures all ESP_LOGx output into an in-RAM ring buffer (readable over
// HTTP via /api/logs, even when the SD is not mounted) and, best-effort, mirrors it to a
// file on the SD card. Safe to call before or without an SD card.
void nucleo_log_init(void);

// Copy the captured log (oldest→newest) into out (NUL-terminated). Returns bytes written.
size_t nucleo_log_get(char *out, size_t cap);

// Out-of-memory watermark. nucleo_log_init() registers a heap failed-alloc hook that records every
// rejected allocation in static counters (alloc-free, reentrancy-safe — no journaling, no SD). The
// diagnostics surface (/api/diag) reads it: a non-zero count is the smoking gun behind the device's
// OOM-race reboots on this PSRAM-less chip. Any out-param may be NULL.
void nucleo_log_oom(unsigned *count, unsigned *last_size, unsigned *last_caps);

#ifdef __cplusplus
}
#endif
