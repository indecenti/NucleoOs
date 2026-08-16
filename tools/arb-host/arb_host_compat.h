// Tiny OS-compat layer for the arbiter HOST test (ARB_HOST). Lets the SAME arb_test.c +
// arb_plat_host.c compile and run under both Win32 (CreateThread/CRITICAL_SECTION/Interlocked)
// and POSIX (pthreads + GCC __sync builtins), so the concurrency proof runs on Windows AND Linux/CI.
// On Windows every macro expands to the exact Win32 call the harness used before — no behaviour change.
#pragma once
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// threads (entry: THREAD_RET fn(THREAD_PARAM))
#define AH_THREAD_RET            DWORD WINAPI
#define AH_THREAD_PARAM          LPVOID
typedef HANDLE ah_thread_t;
#define AH_THREAD_CREATE(t, fn, arg)  ((t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL))
#define AH_THREAD_JOIN(t)             do { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } while (0)

// mutex
typedef CRITICAL_SECTION ah_mutex_t;
#define AH_MUTEX_INIT(m)         InitializeCriticalSection(&(m))
#define AH_MUTEX_LOCK(m)         EnterCriticalSection(&(m))
#define AH_MUTEX_UNLOCK(m)       LeaveCriticalSection(&(m))

// atomics (signed word)
typedef volatile LONG ah_atomic_t;
#define AH_INC(x)                InterlockedIncrement(&(x))
#define AH_DEC(x)                InterlockedDecrement(&(x))
#define AH_LOAD(x)               InterlockedCompareExchange(&(x), 0, 0)
#define AH_STORE(x, v)           InterlockedExchange(&(x), (LONG)(v))

// time
#define AH_SLEEP_MS(ms)          Sleep((DWORD)(ms))
#define AH_TICK_MS()             ((uint32_t)GetTickCount64())

#else  // ---- POSIX ----
#include <pthread.h>
#include <time.h>

#define AH_THREAD_RET            void *
#define AH_THREAD_PARAM          void *
typedef pthread_t ah_thread_t;
#define AH_THREAD_CREATE(t, fn, arg)  pthread_create(&(t), NULL, (fn), (arg))
#define AH_THREAD_JOIN(t)             pthread_join((t), NULL)

typedef pthread_mutex_t ah_mutex_t;
#define AH_MUTEX_INIT(m)         pthread_mutex_init(&(m), NULL)
#define AH_MUTEX_LOCK(m)         pthread_mutex_lock(&(m))
#define AH_MUTEX_UNLOCK(m)       pthread_mutex_unlock(&(m))

typedef volatile long ah_atomic_t;
#define AH_INC(x)                __sync_add_and_fetch(&(x), 1)
#define AH_DEC(x)                __sync_sub_and_fetch(&(x), 1)
#define AH_LOAD(x)               __sync_add_and_fetch(&(x), 0)
#define AH_STORE(x, v)           __sync_lock_test_and_set(&(x), (long)(v))

static inline void ah_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#define AH_SLEEP_MS(ms)          ah_sleep_ms((uint32_t)(ms))
static inline uint32_t ah_tick_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}
#define AH_TICK_MS()             ah_tick_ms()
#endif
