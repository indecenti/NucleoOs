// Host shim for ESP-IDF esp_attr.h. The firmware marks its hottest callbacks IRAM_ATTR so they are
// not fetched through the instruction cache; on a PC there is no such distinction, so the attributes
// collapse to nothing. This exists ONLY so firmware sources compile UNCHANGED in the host harness.
#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define NOINLINE_ATTR   __attribute__((noinline))
#define FORCE_INLINE_ATTR inline __attribute__((always_inline))
