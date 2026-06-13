// Notification history journal — append-only, size-rotated, ~zero RAM.
//
// Each notification appends one compact JSON line. When the file would cross `cap_bytes` it is
// ROTATED first: rename path -> path".0" (overwriting the previous backup) and start fresh. That
// is O(1) — a rename, no read, no parse, no buffer — so history stays bounded at ~2*cap on disk
// while costing essentially no RAM and no CPU beyond the append itself. The reader (web or the
// native center) parses on demand, never the writer. CPU/SD over RAM, by design.
//
// Pure C: only stdio/stat, so the host harness exercises it directly.
#pragma once
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Append `line` (no trailing newline; one is added) to `path`, rotating if it would exceed
// `cap_bytes` (<=0 disables rotation). Returns 0 on success.
static inline int notify_journal_append(const char *path, const char *line, long cap_bytes)
{
    struct stat st;
    long sz = (stat(path, &st) == 0) ? (long)st.st_size : 0;
    long add = (long)strlen(line) + 1;

    if (cap_bytes > 0 && sz > 0 && sz + add > cap_bytes) {
        char bak[300];
        snprintf(bak, sizeof bak, "%s.0", path);
        remove(bak);
        rename(path, bak);
    }
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}
