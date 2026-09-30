// fstwin — keep a served file and its pre-compressed ".gz" twin coherent on write.
//
// webfs serves "<file>.gz" in preference to "<file>" whenever the client accepts gzip (nucleo_webfs.c), so
// writing a new <file> while an old <file>.gz stays behind keeps shipping the OLD code — the project's
// recurring ".gz shadowing" gotcha. And nobody can clean it up afterwards: deleting under /www and /apps is
// refused over the API (nucleo_fsprotect.h). So the write itself drops the stale twin.
//
// Scope: ONLY the trees webfs serves gz-first — /www/shell/** and /apps/<id>/www/**. Anywhere else a
// "<name>.gz" is an independent file (a user's backup.tar next to backup.tar.gz) and is never touched.
// Contract for writers that ship both (push-ota, sd-net-sync, the SD-content installer): write <file>
// FIRST, then <file>.gz — they already do (sorted order puts the raw file first).
//
// POSIX only (stat/remove): host-compiled and gated by tools/anima-host/fstwin-check.mjs.
#pragma once
#include <stdbool.h>

// True when `abs` (an absolute SD path, NUCLEO_SD_MOUNT-prefixed) is a file webfs may serve with a .gz twin.
bool nucleo_fs_twin_scope(const char *abs);

// After `abs` was (re)written: if it is in scope, is not itself a .gz, and "<abs>.gz" is a regular file,
// remove that twin. Returns true when a twin was removed.
bool nucleo_fs_drop_stale_twin(const char *abs);
