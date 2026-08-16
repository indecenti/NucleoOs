// Host implementation of esp_timer_get_time(): microseconds since first call.
// Windows: QueryPerformanceCounter. POSIX: clock_gettime(CLOCK_MONOTONIC).
// Good enough for the HDC throughput selftest.
#include "esp_timer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int64_t esp_timer_get_time(void) {
    static LARGE_INTEGER freq;   // ticks/sec, queried once
    static LARGE_INTEGER start;  // origin
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart);
}

#else
#include <time.h>

int64_t esp_timer_get_time(void) {
    static int64_t start;        // origin, ns
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (start == 0) start = now;
    return (now - start) / 1000;
}
#endif
