// ANIMA Local — the browser's per-user state lives in the persisted subtree.
//
// The engine keeps what a USER teaches it in small files beside the knowledge pack (firmware paths):
//   user.tsv / user.vec   learn tier ("ricorda che ...")      profile.tsv   personal profile
//   units.txt             custom units ("1 spanna = 22 cm")   (+ each file's ".tmp" twin: atomic rewrite)
// In the browser the pack is a read-only MEMFS copy, rebuilt on every page load, and only
// /sd/data/anima/rw is backed by IndexedDB (IDBFS, mounted by apps/anima/www/local/engine.js). So these
// files must live under rw/ or everything the user taught is forgotten on reload.
//
// The firmware sources stay untouched: build.ps1 links with -Wl,--wrap=fopen,remove,rename, so every
// call from the engine lands here first, and ONLY the paths above are re-rooted into rw/ (everything else
// passes through verbatim). The redirect is symmetric — reads, writes, the tmp twin and the rename all map
// the same way — so the engine sees one consistent file, just in a different directory.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "nucleo_board.h"

FILE *__real_fopen(const char *path, const char *mode);
int   __real_remove(const char *path);
int   __real_rename(const char *from, const char *to);

#define STATE_DIR NUCLEO_SD_MOUNT "/data/anima/"
#define RW_DIR    NUCLEO_SD_MOUNT "/data/anima/rw"

static const char *const k_user_state[] = { "user.tsv", "user.vec", "profile.tsv", "units.txt" };

// "<STATE_DIR><name>[.tmp]" -> "<RW_DIR>/<name>[.tmp]" for a user-state name; any other path unchanged.
static const char *rw_path(const char *path, char *buf, size_t cap) {
    const size_t n = sizeof STATE_DIR - 1;
    if (!path || strncmp(path, STATE_DIR, n) != 0) return path;
    const char *base = path + n;
    for (size_t i = 0; i < sizeof k_user_state / sizeof k_user_state[0]; i++) {
        const size_t l = strlen(k_user_state[i]);
        if (strncmp(base, k_user_state[i], l) == 0 && (base[l] == 0 || strcmp(base + l, ".tmp") == 0)) {
            mkdir(RW_DIR, 0777);   // no-op (EEXIST) once engine.js mounted IDBFS there; creates it headless
            int w = snprintf(buf, cap, RW_DIR "/%s", base);
            return (w > 0 && (size_t)w < cap) ? buf : path;
        }
    }
    return path;
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    char b[256];
    return __real_fopen(rw_path(path, b, sizeof b), mode);
}

int __wrap_remove(const char *path) {
    char b[256];
    return __real_remove(rw_path(path, b, sizeof b));
}

int __wrap_rename(const char *from, const char *to) {
    char b1[256], b2[256];
    return __real_rename(rw_path(from, b1, sizeof b1), rw_path(to, b2, sizeof b2));
}
