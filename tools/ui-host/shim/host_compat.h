// ui-host: force-included first (g++ -include). Bridges MinGW to the POSIX bits the firmware uses and
// makes wall-clock time SCRIPTED (host_time) so screenshots are deterministic.
#pragma once
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 1      // localtime_r / gmtime_r in MinGW's <time.h>
#endif
#include <time.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
time_t host_time(time_t *out);              // host_stubs.cpp: the scene's wall clock (UTC)
#ifdef __cplusplus
}
#endif
#define time(p) host_time(p)
#define mkdir(p, m) _mkdir(p)
