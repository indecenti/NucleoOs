// Host stub: RTC_NOINIT memory is plain .bss on the PC; the test simulates "survives a warm reboot"
// by simply not touching it between launcher_reset() and launcher_apply_return().
#pragma once
#define RTC_NOINIT_ATTR
