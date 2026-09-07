// Force-included (-include) ONLY into app_alarm.cpp: it redirects the absolute "/sd/..." paths the
// firmware writes to a sandbox directory under tools/alarm-host, so the harness can inspect the real
// files (config JSON, NDJSON log, WAV) the app produces without touching a card or the C: root.
#pragma once
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

// MinGW has no POSIX localtime_r; map it onto the MS-safe variant so the firmware source compiles
// unchanged on the host.
#ifdef _WIN32
static inline struct tm *host_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) ? NULL : out;
}
#define localtime_r(t, o) host_localtime_r((t), (o))
#endif

#ifdef __cplusplus
extern "C" {
#endif
FILE *host_fopen(const char *p, const char *m);
int   host_mkdir(const char *p, int mode);
int   host_stat(const char *p, struct stat *st);
int   host_remove(const char *p);
int   host_rename(const char *a, const char *b);
#ifdef __cplusplus
}
#endif

#define fopen(p, m)  host_fopen((p), (m))
#define mkdir(p, m)  host_mkdir((p), (m))
#define stat(p, s)   host_stat((p), (s))
#define remove(p)    host_remove((p))
#define rename(a, b) host_rename((a), (b))
