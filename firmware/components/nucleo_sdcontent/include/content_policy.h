// SD content self-install — the PURE decision core (no ESP-IDF, no I/O), host-testable exactly like
// wifi_policy. It parses the release manifest and decides, per line, whether a path may be written and
// whether the on-disk copy already matches. The device glue (nucleo_sdcontent.c) owns the HTTP fetch,
// the SHA-256 and the FAT writes; it NEVER writes a path this module did not approve. See
// docs/sd-content-install.md §4 (safety rules) — this is the defense-in-depth enforcement on the device.
//
// Held to tools/anima-host/sdcontent-check.mjs (`npm run sdcontent:test`): a bad or hostile manifest must
// never write outside the allow-list, and every ownership row in the design doc is a test vector here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Write mode of a manifest line (docs/sd-content-install.md §2.3).
typedef enum {
    SDC_MODE_WRITE = 0,   // 'w': write if the on-disk bytes differ (size+sha)
    SDC_MODE_CREATE,      // 'c': create only if absent (seeds — never clobber the user's edits)
    SDC_MODE_MERGE,       // 'm': merge (apps.json) — handled specially by the device, never a blind write
} sdc_mode_t;

// One parsed file line: "<sha256-lowercase> <size> <pack> <mode> <sd/relative/path>".
typedef struct {
    char     sha[65];     // 64 lowercase hex + NUL
    uint32_t size;        // bytes
    char     pack[16];    // pack name (core / arcade / ...)
    sdc_mode_t mode;
    char     path[208];   // SD-relative path (<=200 chars + NUL + margin)
} sdc_file_t;

// The manifest header: "#nucleoos-sd 1 <tag> <total_files> <total_bytes>".
typedef struct {
    int      version;     // format version (1)
    char     tag[24];     // firmware tag this payload matches
    uint32_t files;
    uint64_t bytes;
} sdc_header_t;

// Parse the "#nucleoos-sd ..." header line. Returns true on a well-formed v1 header.
bool sdc_parse_header(const char *line, sdc_header_t *out);

// Parse one file line (not a '#...' comment). Returns true only if every field is well-formed AND the
// path passes hygiene + the writable allow-list (a refused path fails the whole run, loudly). A '#pack'
// or other comment line returns false with *is_comment=true so the caller skips it without erroring.
bool sdc_parse_file(const char *line, sdc_file_t *out, bool *is_comment);

// Path hygiene: SD-relative, no "..", no backslash, no leading '/', 1..200 chars, printable ASCII only.
bool sdc_path_hygienic(const char *path);

// May the installer write this SD-relative path? A strict allow-list of roots with carve-outs
// (docs/sd-content-install.md §4): www/shell/, apps/<id>/ (never apps/theme.cfg, never apps/<id>/data/),
// system/registry/, system/ir/, data/anima/ brain files only (anima-*, dict-*, commands*, akb5/),
// data/anima/learned/facets.{it,en}.jsonl, wallpapers/, evilportal/. Everything else is refused.
// Implies sdc_path_hygienic.
bool sdc_path_writable(const char *path);

// Does the firmware tag match the manifest's? Both are the semver+build triplet; compared on the first
// three dotted numbers so a local "+gitsha" suffix never blocks a match.
bool sdc_tag_matches(const char *firmware_tag, const char *manifest_tag);

// Plan one file: given the manifest line and what is on disk (exists / size / lowercase sha, which the
// device fills), decide. have_sha may be NULL when the device skipped hashing (size differs already).
typedef enum { SDC_PLAN_SKIP = 0, SDC_PLAN_WRITE, SDC_PLAN_CREATE_SKIP, SDC_PLAN_MERGE } sdc_plan_t;
sdc_plan_t sdc_plan(const sdc_file_t *f, bool on_disk, uint32_t disk_size, const char *disk_sha);

#ifdef __cplusplus
}
#endif
