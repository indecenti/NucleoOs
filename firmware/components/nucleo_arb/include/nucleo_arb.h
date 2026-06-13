// nucleo_arb — device-authoritative heavy-work arbiter.
//
// WHY THIS EXISTS
//   The Cardputer is a PSRAM-less ESP32-S3: ~18 KB runtime heap, fragmented, one SD bus,
//   and a SINGLE-TASK httpd. The existing dual-bar TLS guard (largest_free_block >= 18 KB
//   AND free >= 40 KB) is a POINT check: two heavy consumers can BOTH pass it, then BOTH
//   allocate an mbedTLS context (~40 KB each) and the second OOM-crashes the server. That is
//   a TOCTOU race between the httpd task and a background task (transcribe, model pull, the
//   native ANIMA worker, audio decode).
//
//   This module is the fix: a SINGLE "heavy-work" token. At most one heavy job holds it at a
//   time, and the dual-bar heap check runs INSIDE the critical section the token guards — so
//   the check-then-allocate window is no longer racy. It is fail-safe by construction:
//
//     * acquire(timeout=0) NEVER blocks  -> the httpd task can ask and instantly fall back to
//       its existing 503 / offline-degrade path if the token is busy. Worst case == today.
//     * foreground (FG) preempts background (BG): a BG holder polls should_yield() between
//       work chunks and releases promptly when an interactive request is waiting.
//     * BG never preempts FG.
//     * release records the post-teardown free-heap floor — a sentinel that catches a job
//       that yielded the token but failed to free its TLS/buffers (the make-or-break detail).
//
// The token is ADVISORY: a caller that ignores it loses no correctness, it only loses the
// OOM protection. Every heavy site is expected to wrap its allocate->use->free window in
// acquire()/release().
//
// Testability: the core (nucleo_arb.c) is platform-agnostic; locking/time/heap/notify live
// behind arb_plat_* (FreeRTOS on device, Win32 on the host). tools/anima-host/arb-check.mjs
// proves mutual exclusion under real thread contention, FG-preempts-BG, the never-block
// guarantee, idempotent release, and the heap-floor sentinel.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARB_BG = 0,   // background: model pull, cloud transcribe, prefetch, native-app worker
    ARB_FG = 1,   // foreground: a user-initiated web request / interactive UI query
} arb_class_t;

typedef struct {
    bool     busy;           // is the token held right now
    char     job[16];        // short label of the current holder (e.g. "transcribe"); keep == s_job
    uint8_t  cls;            // arb_class_t of the holder
    uint32_t held_ms;        // how long the current holder has held it
    uint32_t waiters_fg;     // FG requests currently waiting for the token
    uint32_t waiters_bg;     // BG requests currently waiting
    uint32_t grants;         // lifetime successful acquisitions
    uint32_t denials;        // lifetime acquire() calls that gave up (timeout / try-fail)
    uint32_t yields;         // lifetime times a BG holder was asked to step aside for FG
    size_t   heap_free_min;  // lowest free heap ever observed at a release (teardown floor)
} nucleo_arb_stat_t;

// Create the internal lock and zero the state. Call ONCE, early in boot, before httpd_start()
// and before any task that might acquire. Idempotent.
void nucleo_arb_init(void);

// Acquire the single heavy-work token.
//   cls        : ARB_FG (interactive) or ARB_BG (background).
//   job        : short static label for diagnostics / the busy banner (copied, truncated).
//   timeout_ms : 0 = try once, NEVER block (MANDATORY on the httpd task). >0 = wait up to this
//                long for a current holder to release; an FG waiter also asks a BG holder to yield.
// Returns a non-zero token on success, or 0 if the token is busy / the wait timed out. On 0 the
// caller MUST degrade gracefully (the same 503 / force-offline path it already has).
uint32_t nucleo_arb_acquire(arb_class_t cls, const char *job, uint32_t timeout_ms);

// Release a token from nucleo_arb_acquire(). Passing 0, or a token that is no longer the holder
// (already released / superseded), is a safe no-op — release is idempotent per token id.
void nucleo_arb_release(uint32_t token);

// True if `token` is the current holder AND a higher-priority (FG) request is waiting. A BG
// holder of a long/chunked job (model pull, big upload) should poll this between chunks and
// release as soon as it returns true, so an interactive request is not starved.
bool nucleo_arb_should_yield(uint32_t token);

// True if any heavy job currently holds the token (cheap; for the /api/status flag).
bool nucleo_arb_busy(void);

// Snapshot the arbiter state for /api/status and diagnostics. `out` must be non-NULL.
void nucleo_arb_snapshot(nucleo_arb_stat_t *out);

#ifdef __cplusplus
}
#endif
