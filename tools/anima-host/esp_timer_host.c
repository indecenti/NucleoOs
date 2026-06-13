// Host implementation of esp_timer_get_time(): microseconds since first call,
// backed by QueryPerformanceCounter. Good enough for the HDC throughput selftest.
#include "esp_timer.h"
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
