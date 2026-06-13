// WASM shim for ESP-IDF esp_attr.h. nucleo_anima.c uses RTC_NOINIT_ATTR for a diagnostic
// breadcrumb that survives a warm reboot on the device. In the browser there is no RTC
// memory section — make every placement attribute a no-op so the variable becomes a plain
// global (its reboot-persistence semantics are device-only and irrelevant here).
#pragma once

#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#ifndef EXT_RAM_ATTR
#define EXT_RAM_ATTR
#endif
#ifndef WORD_ALIGNED_ATTR
#define WORD_ALIGNED_ATTR
#endif
