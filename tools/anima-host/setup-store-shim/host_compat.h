// Force-included (gcc -include) by setup-store-check.mjs: bridges MinGW to the POSIX two-argument
// mkdir() the firmware store uses, so setup_store.c compiles unchanged on a Windows host.
#pragma once
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(p, m) _mkdir(p)
#endif
