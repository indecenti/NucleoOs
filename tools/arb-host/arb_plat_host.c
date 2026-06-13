// Host platform for the arbiter core (ARB_HOST build). Win32 only — matches the rest of the
// host harness, which is Windows-specific (GetModuleFileNameW, CommandLineToArgvW). No pthread
// dependency. Provides a CONTROLLABLE mock heap and records busy transitions so arb_test.c can
// assert the heap-floor sentinel and the busy event edges.
#ifdef ARB_HOST

#include "arb_plat.h"
#include <windows.h>

static CRITICAL_SECTION s_cs;
static int s_inited = 0;

void arb_plat_init(void)
{
    if (!s_inited) { InitializeCriticalSection(&s_cs); s_inited = 1; }
}

void arb_plat_lock(void)   { EnterCriticalSection(&s_cs); }
void arb_plat_unlock(void) { LeaveCriticalSection(&s_cs); }

uint32_t arb_plat_now_ms(void) { return (uint32_t)GetTickCount64(); }

void arb_plat_sleep_ms(uint32_t ms) { Sleep(ms ? ms : 1); }

// --- test-controllable mock heap + busy-event recorder -------------------------------------
static volatile LONG s_mock_free   = 200000;   // pretend free internal heap (bytes)
static volatile LONG s_busy_events = 0;        // count of on_busy() calls
static volatile LONG s_busy_now    = 0;        // last busy state seen (0/1)

void   arb_test_set_free(size_t v)  { InterlockedExchange(&s_mock_free, (LONG)v); }
long   arb_test_busy_events(void)   { return InterlockedCompareExchange(&s_busy_events, 0, 0); }
long   arb_test_busy_now(void)      { return InterlockedCompareExchange(&s_busy_now, 0, 0); }

size_t arb_plat_heap_free(void) { return (size_t)InterlockedCompareExchange(&s_mock_free, 0, 0); }

void arb_plat_on_busy(bool busy, const char *job)
{
    (void)job;
    InterlockedExchange(&s_busy_now, busy ? 1 : 0);
    InterlockedIncrement(&s_busy_events);
}

#endif // ARB_HOST
