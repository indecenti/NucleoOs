// tanks-host: force-included first. The game stores its config under "/sd/data/tanks"; on the PC that
// absolute path is redirected into the scenario's sandbox folder (the process cwd) so runs never share
// state and never touch the host's drive root. Function-like macros only: `struct stat` stays intact.
#pragma once
#ifdef __cplusplus
#include <cstdio>      // first: libstdc++'s <cstdio> #undefs fopen, so it must not come after the macro below
#endif
#include <stdio.h>
#include <sys/stat.h>
#include <direct.h>
#ifdef __cplusplus
extern "C" {
#endif
FILE *host_fopen(const char *p, const char *m);
int   host_stat(const char *p, struct stat *st);
int   host_mkdir(const char *p);
#ifdef __cplusplus
}
#endif
#define fopen(p, m)  host_fopen(p, m)
#define stat(p, st)  host_stat(p, st)
#define mkdir(p, m)  host_mkdir(p)
