#pragma once
// registry_scan — walk the objects of apps.json's "installed" array WITHOUT parsing the whole document.
//
// cJSON_Parse of the full registry (49 apps, ~11 KB pretty-printed) builds every node at once: ~25-30 KB
// of small allocations. In the web profile the heap has ~35 KB free, so the runtime reload after an app
// install failed ("invalid JSON in apps.json" — really an OOM) and /api/apps kept the old table: the new
// app was installed on the SD but missing from the launcher until a reboot. The loader now parses ONE app
// object at a time (a few hundred bytes of nodes). Pure C, no ESP-IDF: host-tested by
// tools/anima-host/regscan-ctest.c (npm run regscan:test, in the gate).
#include <stddef.h>

// Next object of the "installed" array in s[0..len). *cursor is 0 on the first call and is advanced by
// each call. On success returns 1 with s[*start .. *end) = one complete {...} (strings and escapes
// respected); returns 0 at the end of the array, -1 if there is no "installed" array or the text is
// malformed / truncated.
int registry_scan_installed(const char *s, size_t len, size_t *cursor, size_t *start, size_t *end);
