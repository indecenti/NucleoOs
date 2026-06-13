// WASM implementation of esp_timer_get_time(): microseconds since first call, backed by
// clock_gettime(CLOCK_MONOTONIC). The ONLY caller in the offline cascade is the HDC
// self-test's throughput section (nucleo_anima_hdc.c), which is never reached from the
// query path — so this affects no answer. Present purely so the selftest links.
#include "esp_timer.h"
#include <time.h>

int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}
