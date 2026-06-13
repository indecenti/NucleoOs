// Force-included (gcc -include / cl /FI) before any other header, so the *real* ANIMA
// component sources compile unchanged on a Windows host. Under MinGW-w64 GCC nothing is
// needed (it has strcasecmp / __builtin_popcount natively); the bridges below exist only
// for the MSVC compiler, which spells those differently. See build.ps1.
#pragma once

#ifdef _MSC_VER
// CRT: ANIMA uses fopen/snprintf/strcpy on purpose (bounded buffers); silence MSVC's
// "use the _s variant" deprecations rather than touch firmware source.
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1

#include <string.h>
#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef strdup
#define strdup      _strdup
#endif

// GNU popcount builtins -> MSVC intrinsics (used by the HDC/VSA core, if present).
#include <intrin.h>
#ifndef __builtin_popcount
#define __builtin_popcount(x)   ((int)__popcnt((unsigned)(x)))
#endif
#ifndef __builtin_popcountll
#define __builtin_popcountll(x) ((int)__popcnt64((unsigned long long)(x)))
#endif

// ssize_t (MSVC ships SSIZE_T in BaseTsd.h, not the lowercase POSIX name).
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif // _MSC_VER
