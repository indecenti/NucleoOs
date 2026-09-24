// Streaming directory listing for GET /api/fs/list — pure POSIX (opendir/readdir/stat) plus the
// inline protect/factory policy headers, so the host harness compiles the SAME code
// (tools/anima-host/fslist-ctest.c).
//
// Why streaming: the old handler built a cJSON tree and printed it in one go — one node per
// entry plus a single contiguous output buffer, O(entries) heap. A 60-80 entry folder no longer
// fit a ~13 KB largest free block (503 "oom"), so push-ota --sync skipped whole folders. Here
// each entry is serialised into the caller's small buffer and flushed through `sink` as it
// fills: RAM is O(1) in the number of entries. The only per-listing allocation beyond that is
// the factory-name hash set, and only inside /data/DOS or /data/ROMs leaves (4 bytes per
// ".factory" line; falls back to a zero-heap per-entry scan if it can't be allocated).
//
// Output shape (unchanged): {"entries":[{"name":..,"type":"dir"|"file","size":N,
// "protected":true (only when true),"has_subdirs":bool (dirs only)},...]}
#pragma once
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>

// Deliver `len` bytes of the response. false = the client is gone; the stream stops.
typedef bool (*fslist_sink_t)(void *ctx, const char *data, size_t len);

// Stream the listing of the already-open `dir` (absolute path `abs`) through `sink`, using
// `buf[cap]` (cap >= 16) as the only output buffer. Returns false if the sink failed.
bool fslist_stream(DIR *dir, const char *abs, char *buf, size_t cap, fslist_sink_t sink, void *ctx);
