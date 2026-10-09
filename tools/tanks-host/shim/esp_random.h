// tanks-host: deterministic stand-in for the ESP32 hardware RNG (harness.cpp seeds it per scenario).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);
#ifdef __cplusplus
}
#endif
