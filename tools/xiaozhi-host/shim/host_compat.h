#pragma once
// Forced into every host compile (-include): what newlib has and MinGW does not.
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#define mkdir(p, m) _mkdir(p)
static inline char *strcasestr(const char *h, const char *n)
{
    const size_t l = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)h;
    return 0;
}
