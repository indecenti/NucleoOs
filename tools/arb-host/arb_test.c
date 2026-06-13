// arb_test — host concurrency proof for the heavy-work arbiter core (nucleo_arb.c).
//
// Compiled with -DARB_HOST against nucleo_arb.c + arb_plat_host.c (Win32). Drives the SAME core
// that runs on device, under REAL OS thread contention, and asserts the properties the device
// relies on for stability. Prints "[arb-check] N/N tests pass" and exits 0 (all pass) or 1.
//
//   T1 logic         try-acquire never blocks; busy -> 0 + denial; release idempotent; snapshot sane
//   T2 fg-preempts   a BG holder sees should_yield()==true once an FG request waits; FG then wins
//   T3 mutual-excl   12 threads hammer acquire/release; a shared sentinel proves only ONE holder ever
//   T4 never-block   under a held token, 5000 try-acquires all return 0 and finish ~instantly
//   T5 heap-floor    the lowest free heap at release is recorded for the /api/status teardown sentinel
#include "nucleo_arb.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// hooks from arb_plat_host.c
void arb_test_set_free(size_t v);
long arb_test_busy_events(void);
long arb_test_busy_now(void);

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

// ---------------------------------------------------------------------------- T1: logic
static void t1_logic(void)
{
    uint32_t a = nucleo_arb_acquire(ARB_FG, "alpha", 0);
    CHECK(a != 0, "T1 first acquire should succeed");
    CHECK(nucleo_arb_busy(), "T1 busy after acquire");

    uint32_t b = nucleo_arb_acquire(ARB_FG, "beta", 0);   // try-only on a held token
    CHECK(b == 0, "T1 try-acquire on held token must return 0 (never block)");

    CHECK(!nucleo_arb_should_yield(a), "T1 no FG waiter -> holder need not yield");

    nucleo_arb_release(a);
    CHECK(!nucleo_arb_busy(), "T1 free after release");
    nucleo_arb_release(a);                                 // idempotent: stale token, no-op
    nucleo_arb_release(0);                                 // 0 is a safe no-op

    uint32_t c = nucleo_arb_acquire(ARB_BG, "gamma", 0);   // token recycled after release
    CHECK(c != 0, "T1 acquire after release should succeed");
    nucleo_arb_release(c);

    nucleo_arb_stat_t s; nucleo_arb_snapshot(&s);
    CHECK(!s.busy, "T1 snapshot busy=false at rest");
    CHECK(s.grants >= 2, "T1 grants counted (a + c)");   // b was denied, not granted
    CHECK(s.denials >= 1, "T1 denials counted");
}

// ---------------------------------------------------------------------------- T2: FG preempts BG
static volatile uint32_t t2_fg_token = 0;
static DWORD WINAPI t2_fg_thread(LPVOID p)
{
    (void)p;
    // Wait up to 1s for the BG holder to step aside; this also raises the yield flag.
    t2_fg_token = nucleo_arb_acquire(ARB_FG, "interactive", 1000);
    return 0;
}
static void t2_fg_preempts_bg(void)
{
    uint32_t bg = nucleo_arb_acquire(ARB_BG, "model-pull", 0);
    CHECK(bg != 0, "T2 BG acquire");
    CHECK(!nucleo_arb_should_yield(bg), "T2 no yield requested yet");

    HANDLE th = CreateThread(NULL, 0, t2_fg_thread, NULL, 0, NULL);

    // The FG thread is now waiting -> the BG holder must be asked to yield within a short window.
    int saw_yield = 0;
    for (int i = 0; i < 200; i++) {           // up to ~600 ms
        if (nucleo_arb_should_yield(bg)) { saw_yield = 1; break; }
        Sleep(3);
    }
    CHECK(saw_yield, "T2 BG holder must see should_yield()==true once FG waits");

    nucleo_arb_release(bg);                    // good citizen yields
    WaitForSingleObject(th, 2000);
    CloseHandle(th);

    CHECK(t2_fg_token != 0, "T2 FG must obtain the token after BG yields");
    nucleo_arb_release(t2_fg_token);
}

// ---------------------------------------------------------------------------- T3: mutual exclusion
#define T3_THREADS 12
#define T3_ITERS   400
static volatile LONG t3_inside = 0;            // must NEVER exceed 1 -> proves single holder
static volatile LONG t3_violations = 0;
static volatile LONG t3_acquired = 0;
static DWORD WINAPI t3_worker(LPVOID p)
{
    (void)p;
    for (int i = 0; i < T3_ITERS; i++) {
        uint32_t tk = nucleo_arb_acquire(ARB_FG, "x", 1000);
        if (!tk) continue;                     // timed out under heavy contention -> retry next iter
        InterlockedIncrement(&t3_acquired);
        LONG now = InterlockedIncrement(&t3_inside);
        if (now != 1) InterlockedIncrement(&t3_violations);   // a second holder got in -> RACE
        // tiny hold to widen any race window
        for (volatile int s = 0; s < 50; s++) { }
        InterlockedDecrement(&t3_inside);
        nucleo_arb_release(tk);
    }
    return 0;
}
static void t3_mutual_exclusion(void)
{
    HANDLE th[T3_THREADS];
    for (int i = 0; i < T3_THREADS; i++) th[i] = CreateThread(NULL, 0, t3_worker, NULL, 0, NULL);
    WaitForMultipleObjects(T3_THREADS, th, TRUE, 30000);
    for (int i = 0; i < T3_THREADS; i++) CloseHandle(th[i]);
    CHECK(t3_violations == 0, "T3 mutual exclusion: two threads must never hold the token at once");
    CHECK(t3_acquired > 0, "T3 some acquisitions happened");
    printf("  T3: %ld grants across %d threads, 0 overlap\n", (long)t3_acquired, T3_THREADS);
}

// ---------------------------------------------------------------------------- T4: never block
static void t4_never_block(void)
{
    uint32_t held = nucleo_arb_acquire(ARB_FG, "holder", 0);
    CHECK(held != 0, "T4 setup acquire");
    DWORD t0 = GetTickCount();
    int zeros = 0;
    for (int i = 0; i < 5000; i++) if (nucleo_arb_acquire(ARB_FG, "probe", 0) == 0) zeros++;
    DWORD dt = GetTickCount() - t0;
    CHECK(zeros == 5000, "T4 every try-acquire on a held token returns 0");
    CHECK(dt < 1000, "T4 5000 try-acquires must be near-instant (never block the httpd task)");
    printf("  T4: 5000 try-acquires in %lu ms, all denied (non-blocking)\n", (unsigned long)dt);
    nucleo_arb_release(held);
}

// ---------------------------------------------------------------------------- T5: heap floor
static void t5_heap_floor(void)
{
    arb_test_set_free(123456);                 // plenty
    uint32_t a = nucleo_arb_acquire(ARB_BG, "j", 0);
    nucleo_arb_release(a);                      // samples 123456
    arb_test_set_free(30000);                  // a tight teardown
    uint32_t b = nucleo_arb_acquire(ARB_BG, "j", 0);
    nucleo_arb_release(b);                      // samples 30000 -> new min
    nucleo_arb_stat_t s; nucleo_arb_snapshot(&s);
    CHECK(s.heap_free_min == 30000, "T5 release records the lowest post-teardown free heap");
}

int main(void)
{
    nucleo_arb_init();
    printf("[arb-check] heavy-work arbiter concurrency proof\n");

    t1_logic();
    t2_fg_preempts_bg();
    t3_mutual_exclusion();
    t4_never_block();
    t5_heap_floor();

    // sanity: busy events fire on every edge and we end at rest
    CHECK(arb_test_busy_now() == 0, "end at rest (busy=0)");
    CHECK(arb_test_busy_events() > 0, "busy transitions were emitted");

    int total = 5;
    int passed = total - (g_fail ? 1 : 0);     // any failed CHECK fails the suite
    if (g_fail) {
        printf("[arb-check] FAIL: %d assertion(s) failed\n", g_fail);
        return 1;
    }
    printf("[arb-check] %d/%d tests pass (mutual-exclusion, fg-preempt, never-block, heap-floor)\n",
           total, total);
    (void)passed;
    return 0;
}
