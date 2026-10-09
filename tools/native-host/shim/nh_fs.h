// native-host: force-included first. The games keep their files under "/sd/data/<game>"; on the PC that
// absolute path is redirected into the scenario's sandbox folder (the process cwd), so runs never share
// state and never touch the host's drive root. Function-like macros only: `struct stat` stays intact.
#pragma once
#ifdef __cplusplus
#include <cstdio>      // first: libstdc++'s <cstdio> #undefs fopen/remove/rename, so it must precede the macros
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
int   host_remove(const char *p);
int   host_rename(const char *a, const char *b);
#ifdef __cplusplus
}
#endif
#define fopen(p, m)   host_fopen(p, m)
#define stat(p, st)   host_stat(p, st)
#define mkdir(p, m)   host_mkdir(p)
#define remove(p)     host_remove(p)
#define rename(a, b)  host_rename(a, b)
