// SD system-file protection — single source of truth.
//
// Returns true for paths on the SD that are REQUIRED for NucleoOS / ANIMA / the shell
// and the bundled apps to boot and run. Protected paths may NOT be deleted, moved, nor
// renamed-away by anyone: the web file manager (File Commander), the ANIMA copilot /
// workspace, the JS app runtime, any app talking to /api/fs/*, AND the native on-device
// Files app. Every one of those funnels through one of two call sites that consult this
// predicate (nucleo_fsapi.c for the HTTP API, app_files.cpp for the native browser), so
// this header is the ONE place the policy lives.
//
// Deliberately NOT protected, so the system can keep updating itself and the user keeps
// control of their stuff:
//   • in-place OVERWRITE via /api/fs/write is allowed (OTA app install rewrites
//     apps.json, config re-saves, etc. replace content without making the file vanish);
//     only delete / move-away is blocked.
//   • device STATE (settings, keys, sessions, logs, the user's Groq key, ANIMA's learned
//     cards / profile / telemetry / vectors) stays freely deletable — it is regenerable
//     and belongs to the user, mirroring sd_deploy.py's DEVICE_STATE list.
//   • everything under /data EXCEPT ANIMA's offline brain (media, documents, recordings,
//     downloads, ROMs…) stays freely deletable.
//
// Pure string logic: zero heap, no SD I/O — safe to call from the httpd task and the UI
// task on this PSRAM-less, ~18 KB-heap board. Keep it allocation-free.
//
// The protected SET mirrors the deploy classification in tools/nucleo-sd-deploy/sd_deploy.py
// (SOURCE_MAP = system, DEVICE_STATE = user state). If you add a new top-level system tree
// there, add it here too.
#pragma once
#include <string.h>
#include <strings.h>   // strncasecmp / strcasecmp — FATFS is case-insensitive, so must we be
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_board.h"   // NUCLEO_SD_MOUNT

// True when `rel` (an SD-relative path with a leading '/') is exactly "/<seg>" or begins
// "/<seg>/…". Case-insensitive: a request for /APPS or /Data/Anima must not slip past.
static inline bool nucleo_fs__seg(const char *rel, const char *seg)
{
    if (rel[0] != '/') return false;
    size_t n = strlen(seg);
    if (strncasecmp(rel + 1, seg, n) != 0) return false;
    char after = rel[1 + n];
    return after == '\0' || after == '/';
}

// Final path component of `rel` (no slash). For "/apps/x/y.bin" -> "y.bin".
static inline const char *nucleo_fs__base(const char *rel)
{
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

// Is `abs` (an absolute "/sd"-rooted path, as built by the fs API and the Files app) a
// protected system file/dir?  Normalizes first so trivial evasions can't dodge the check.
static inline bool nucleo_fs_is_protected(const char *abs)
{
    if (!abs || !*abs) return false;

    // 1) Strip the SD mount prefix ("/sd"). The mount is always written literally by the
    //    firmware, so an exact compare is correct; only the part AFTER it is client-supplied.
    const char *p = abs;
    size_t mlen = sizeof(NUCLEO_SD_MOUNT) - 1;
    if (strncmp(abs, NUCLEO_SD_MOUNT, mlen) == 0) p = abs + mlen;

    // 2) Normalize into rel[]: force a leading '/', fold backslashes, collapse runs of '/',
    //    and drop "/./" segments. Truncation on an over-long path is harmless: classification
    //    is by the HEAD segment, which survives. ('..' is already rejected upstream.)
    char rel[256];
    size_t j = 0;
    if (*p != '/') rel[j++] = '/';
    for (size_t i = 0; p[i] && j < sizeof(rel) - 1; i++) {
        char c = (p[i] == '\\') ? '/' : p[i];
        if (c == '/') {
            if (j > 0 && rel[j - 1] == '/') continue;                       // collapse "//"
            if (p[i + 1] == '.' && (p[i + 2] == '/' || p[i + 2] == '\0')) { // drop "/./"
                i++;
                continue;
            }
        }
        rel[j++] = c;
    }
    if (j == 0) rel[j++] = '/';
    if (j > 1 && rel[j - 1] == '/') j--;   // drop a trailing '/' so the dir node itself matches
    rel[j] = '\0';

    // 3) Device STATE under /system stays deletable (settings/keys/sessions/logs are
    //    regenerable and user-owned). Checked BEFORE the broad /system protect below.
    if (nucleo_fs__seg(rel, "system/config")   || nucleo_fs__seg(rel, "system/keys") ||
        nucleo_fs__seg(rel, "system/sessions") || nucleo_fs__seg(rel, "system/log")  ||
        nucleo_fs__seg(rel, "system/logs"))
        return false;

    // 4) Protected system trees: the launcher/registry, the shell UI, every bundled app.
    if (nucleo_fs__seg(rel, "system")) return true;   // registry, web, … (state subdirs excluded above)
    if (nucleo_fs__seg(rel, "apps"))   return true;
    if (nucleo_fs__seg(rel, "www"))    return true;

    // 5) ANIMA's offline brain lives under /data/anima. This block fully owns that subtree:
    //    the heavy immutable knowledge is protected, everything else there (learned cards,
    //    teacher.json key, sessions, telemetry, *.vec, profile) is user state and stays free.
    if (nucleo_fs__seg(rel, "data/anima")) {
        if (nucleo_fs__seg(rel, "data/anima/akb5")) return true;        // 47 knowledge shards
        const char *b = nucleo_fs__base(rel);
        if (strncasecmp(b, "anima-", 6) == 0) return true;             // encoder/index/akb5 .bin/.json/.prov
        if (strncasecmp(b, "dict-",  5) == 0) return true;             // translation dictionaries
        if (strncasecmp(b, "commands", 8) == 0) return true;           // ANIMA command map
        return false;                                                  // learned/, teacher.json, *.vec, …
    }

    // Everything else (the rest of /data, wallpapers, evilportal, README, config, backups,
    // journal, the user's media and documents) is freely deletable.
    return false;
}
