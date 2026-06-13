// Host shim for ESP-IDF esp_timer.h — microseconds since process start, backed by
// QueryPerformanceCounter (see esp_timer_host.c). Used by the HDC throughput selftest.
#pragma once
#include <stdint.h>

int64_t esp_timer_get_time(void);
