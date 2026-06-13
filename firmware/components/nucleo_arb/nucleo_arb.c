// nucleo_arb — heavy-work arbiter core (platform-agnostic). See include/nucleo_arb.h.
//
// State machine, all guarded by arb_plat_lock() except the on_busy notify (fired after unlock):
//   s_holder == 0            -> token free
//   s_holder == <id>         -> that token id holds it; s_cls/s_job/s_since describe the holder
//   waiters wait by POLLING  -> a deliberate choice: no condvar to port, and waits are short and
//                               rare. An FG waiter sets s_yield so a BG holder steps aside fast.
#include "nucleo_arb.h"
#include "arb_plat.h"
#include <string.h>

static uint32_t s_holder;       // token id of the current holder, 0 = free
static uint8_t  s_cls;          // arb_class_t of the holder
static char     s_job[16];      // holder label (longest is "transcribe"=10; keep in sync with stat_t.job)
static uint32_t s_since_ms;     // arb_plat_now_ms() at acquire
static uint32_t s_next = 1;     // monotonic token-id generator (never hands out 0)
static uint32_t s_wait_fg;      // FG waiters
static uint32_t s_wait_bg;      // BG waiters
static bool     s_yield;        // an FG request is waiting while a BG job holds -> please release

static uint32_t s_grants;
static uint32_t s_denials;
static uint32_t s_yields;
static size_t   s_heap_min;     // lowest free heap observed at a release (0 = not yet sampled)
static bool     s_ready;        // nucleo_arb_init() ran

void nucleo_arb_init(void)
{
    arb_plat_init();
    arb_plat_lock();
    s_holder = 0; s_cls = 0; s_job[0] = 0; s_since_ms = 0;
    s_next = 1; s_wait_fg = 0; s_wait_bg = 0; s_yield = false;
    s_grants = 0; s_denials = 0; s_yields = 0; s_heap_min = 0;
    s_ready = true;
    arb_plat_unlock();
}

uint32_t nucleo_arb_acquire(arb_class_t cls, const char *job, uint32_t timeout_ms)
{
    if (!s_ready) return 0;                       // never gate before init (fail-open)
    const uint32_t start = arb_plat_now_ms();
    bool counted = false;                         // have we incremented a waiter counter yet
    for (;;) {
        arb_plat_lock();
        if (s_holder == 0) {                      // free -> take it
            uint32_t tk = s_next++;
            if (s_next == 0) s_next = 1;          // skip the reserved 0 on wrap
            s_holder = tk;
            s_cls = (uint8_t)cls;
            if (job) { strncpy(s_job, job, sizeof(s_job) - 1); s_job[sizeof(s_job) - 1] = 0; }
            else s_job[0] = 0;
            s_since_ms = arb_plat_now_ms();
            s_yield = false;
            s_grants++;
            if (counted) { if (cls == ARB_FG) { if (s_wait_fg) s_wait_fg--; }
                           else               { if (s_wait_bg) s_wait_bg--; } }
            arb_plat_unlock();
            arb_plat_on_busy(true, job);          // 0->1 transition, outside the lock (pass caller's
                                                  // label, not the global s_job — no outside-lock read)
            return tk;
        }
        // Busy. Try-only callers (the httpd task) bail immediately — they must never block.
        if (timeout_ms == 0) {
            s_denials++;
            arb_plat_unlock();
            return 0;
        }
        if (!counted) {                           // register as a waiter exactly once
            counted = true;
            if (cls == ARB_FG) s_wait_fg++; else s_wait_bg++;
        }
        // An interactive request must not wait behind a background job: ask it to step aside.
        if (cls == ARB_FG && s_cls == ARB_BG && !s_yield) { s_yield = true; s_yields++; }
        arb_plat_unlock();

        if ((arb_plat_now_ms() - start) >= timeout_ms) {   // wait window elapsed -> give up
            arb_plat_lock();
            if (cls == ARB_FG) { if (s_wait_fg) s_wait_fg--; }
            else               { if (s_wait_bg) s_wait_bg--; }
            s_denials++;
            arb_plat_unlock();
            return 0;
        }
        arb_plat_sleep_ms(2);                     // brief backoff, then re-check
    }
}

void nucleo_arb_release(uint32_t token)
{
    if (token == 0) return;
    bool freed = false;
    arb_plat_lock();
    if (token == s_holder) {                       // only the live holder can release (idempotent)
        s_holder = 0; s_cls = 0; s_job[0] = 0; s_since_ms = 0; s_yield = false;
        size_t fr = arb_plat_heap_free();          // teardown-leak sentinel
        if (s_heap_min == 0 || fr < s_heap_min) s_heap_min = fr;
        freed = true;
    }
    arb_plat_unlock();
    if (freed) arb_plat_on_busy(false, NULL);      // 1->0 transition, outside the lock
}

bool nucleo_arb_should_yield(uint32_t token)
{
    if (token == 0) return false;
    arb_plat_lock();
    bool y = (token == s_holder) && s_yield;
    arb_plat_unlock();
    return y;
}

bool nucleo_arb_busy(void)
{
    arb_plat_lock();
    bool b = (s_holder != 0);
    arb_plat_unlock();
    return b;
}

void nucleo_arb_snapshot(nucleo_arb_stat_t *out)
{
    if (!out) return;
    arb_plat_lock();
    out->busy = (s_holder != 0);
    memcpy(out->job, s_job, sizeof(out->job));
    out->cls = s_cls;
    out->held_ms = (s_holder != 0) ? (arb_plat_now_ms() - s_since_ms) : 0;
    out->waiters_fg = s_wait_fg;
    out->waiters_bg = s_wait_bg;
    out->grants = s_grants;
    out->denials = s_denials;
    out->yields = s_yields;
    out->heap_free_min = s_heap_min;
    arb_plat_unlock();
}
