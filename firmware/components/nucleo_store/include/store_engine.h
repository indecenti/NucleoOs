// App store client — the ENGINE: signed catalog, signed packages, atomic install on the SD card.
// Pure C + stdio, no ESP-IDF: the device glue (nucleo_store.c) plugs in the HTTPS transport of the SD-content
// installer (Range windows, see sdc_engine.h for why) and the clock; the host gate (tools/store-host) plugs in
// a fake GitHub Pages with faults, over a REAL store built and signed by the store's own tools. Same code in
// both. Format: docs/format.md of indecenti/nucleoos-cardputer-store; design + upgrade rules: docs/app-store.md.
//
// RAM: a few KB of buffers, never a whole file, never a JSON tree. The catalog lives on the SD card and is
// read one line at a time.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_LANGS 5          // it en es fr de, the catalog's order
#define ST_ID_MAX 32
#define ST_NAME_MAX 24
#define ST_DESC_MAX 120

typedef struct {
    char     id[ST_ID_MAX + 1];
    char     kind[8];               // web | data | native
    uint32_t code;                  // version_code: only ever goes up
    char     version[16];
    char     min_os[16];
    char     category[12];
    uint32_t size;                  // payload bytes
    char     sig_sha[65];           // SHA-256 of its package.sig
    char     perms[64];             // "mic,net" or ""
    char     name[ST_LANGS][ST_NAME_MAX + 1];
    char     desc[ST_LANGS][ST_DESC_MAX + 1];
    uint32_t updated;               // unix time this version was published (day precision), 0 = unknown
    char     author[41];            // "" in catalogs older than the field
} st_app_t;

typedef enum {
    ST_OK = 0,
    ST_E_NO_SD,         // the SD root does not exist
    ST_E_NET,           // a download failed (st->http: status, 0 = transport/TLS)
    ST_E_SIG,           // a signature did not verify, or was made by a key the firmware does not trust
    ST_E_ROLLBACK,      // the catalog is older than one this device already accepted
    ST_E_EXPIRED,       // the catalog has expired (and the clock is set)
    ST_E_BAD,           // malformed catalog / package, or a path the rules reject
    ST_E_VERIFY,        // a file did not match its signed size + SHA-256
    ST_E_WRITE,         // the SD card refused a write / rename (full, read-only, failing)
    ST_E_NOMEM,
    ST_E_NOTFOUND,      // no such app in the catalog
    ST_E_CONFLICT,      // /sd/apps/<id> exists and was not installed by the store: never touched
    ST_E_UNSUPPORTED,   // a kind this firmware cannot install (native, unknown)
    ST_E_SPACE,         // not enough free space on the SD card for the download + the swap
} st_code_t;

typedef enum { ST_PH_IDLE, ST_PH_CATALOG, ST_PH_PACKAGE, ST_PH_FILES, ST_PH_INSTALL, ST_PH_DONE, ST_PH_FAILED } st_phase_t;

typedef struct {
    st_phase_t phase;
    int      pct;               // 0..100 of the current operation
    uint32_t done, total;       // bytes
    int      files_done, files_total;
    char     cur[40];           // current file
    st_code_t code;
    int      http;
    char     detail[64];
} st_state_t;

typedef struct {
    void *ctx;
    // Same contract as sdc_io_t.get: bytes [off, off+len) of url into buf; returns the count (fewer only at
    // the end), -1 on failure with *status = HTTP status or 0 for transport/TLS.
    int  (*get)(void *ctx, const char *url, uint32_t off, uint32_t len, uint8_t *buf, int *status);
    // ECDSA P-256 over SHA-256: true when `sig` (DER) verifies `hash` with the trusted key `key_id`.
    bool (*verify)(void *ctx, const char *key_id, const uint8_t hash[32], const uint8_t *sig, size_t siglen);
    uint32_t (*now)(void *ctx);                    // unix seconds, 0 = clock not set (expiry not checked)
    void (*progress)(void *ctx, const st_state_t *st);
    void (*tick)(void *ctx);
} st_io_t;

typedef struct {
    const char *root;       // SD mount ("/sd"): installs go to <root>/apps/<id>/, state to <root>/apps/.store/
    const char *base_url;   // "https://indecenti.github.io/nucleoos-cardputer-store/" (trailing '/')
    uint8_t    *buf;        // scratch, >= window bytes (the device lends its download window)
    uint32_t    window;     // bytes per request (6144 on the device)
    int         retries;    // extra attempts per window on a transport failure
} st_cfg_t;

// Download index.txt + index.sig, verify, check serial and expiry; on success the catalog is
// <root>/apps/.store/index.txt and its serial is remembered. On failure the previous catalog stays.
st_code_t st_refresh(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st);

// The local catalog, one app at a time. st_catalog_next skips the header and malformed lines.
FILE *st_catalog_open(const char *root);
bool  st_catalog_next(FILE *f, st_app_t *out);
bool  st_parse_line(const char *line, st_app_t *out);
bool  st_find(const char *root, const char *id, st_app_t *out);
uint32_t st_catalog_serial(const char *root);

// Install or update one app (download, verify everything, swap in atomically, register a web app).
st_code_t st_install(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st, const st_app_t *app);
// Remove an app the store installed (never one it did not: ST_E_CONFLICT).
st_code_t st_remove(const char *root, const char *id);
// version_code installed by the store, 0 = not installed; *system = a folder the store does not own.
uint32_t st_installed(const char *root, const char *id, bool *system);

// "x.y.z" order (missing parts are 0; text after the third number is ignored).
int  st_ver_cmp(const char *a, const char *b);
bool st_path_ok(const char *path);
bool st_id_ok(const char *id);

#ifdef __cplusplus
}
#endif
