// Host platform for the arbiter core (ARB_HOST build). Portable across Win32 and POSIX via
// arb_host_compat.h (CRITICAL_SECTION/Interlocked on Windows, pthread mutex + GCC __sync on POSIX).
// Provides a CONTROLLABLE mock heap and records busy transitions so arb_test.c can assert the
// heap-floor sentinel and the busy event edges.
#ifdef ARB_HOST

#include "arb_plat.h"
#include "arb_host_compat.h"

static ah_mutex_t s_mtx;
static int s_inited = 0;

void arb_plat_init(void)
{
    if (!s_inited) { AH_MUTEX_INIT(s_mtx); s_inited = 1; }
}

void arb_plat_lock(void)   { AH_MUTEX_LOCK(s_mtx); }
void arb_plat_unlock(void) { AH_MUTEX_UNLOCK(s_mtx); }

uint32_t arb_plat_now_ms(void) { return AH_TICK_MS(); }

void arb_plat_sleep_ms(uint32_t ms) { AH_SLEEP_MS(ms ? ms : 1); }

// --- test-controllable mock heap + busy-event recorder -------------------------------------
static ah_atomic_t s_mock_free   = 200000;   // pretend free internal heap (bytes)
static ah_atomic_t s_busy_events = 0;         // count of on_busy() calls
static ah_atomic_t s_busy_now    = 0;         // last busy state seen (0/1)

void   arb_test_set_free(size_t v)  { AH_STORE(s_mock_free, (long)v); }
long   arb_test_busy_events(void)   { return AH_LOAD(s_busy_events); }
long   arb_test_busy_now(void)      { return AH_LOAD(s_busy_now); }

size_t arb_plat_heap_free(void) { return (size_t)AH_LOAD(s_mock_free); }

void arb_plat_on_busy(bool busy, const char *job)
{
    (void)job;
    AH_STORE(s_busy_now, busy ? 1 : 0);
    AH_INC(s_busy_events);
}

#endif // ARB_HOST
