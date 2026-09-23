// Host shim for ESP-IDF esp_task_wdt.h. There is no task watchdog on a PC; firmware that feeds it
// between long SD steps (the Game Boy emulator's saves) compiles unchanged and the feed is a no-op.
#pragma once
#include "esp_err.h"
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
