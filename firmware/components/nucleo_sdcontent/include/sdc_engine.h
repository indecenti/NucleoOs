// SD content self-install — the ENGINE: manifest fetch, per-file plan, windowed download, SHA-256
// verification, atomic install, resume. Pure C + stdio, no ESP-IDF: the device glue (nucleo_sdcontent.c)
// plugs in its HTTPS transport, the host gate (tools/anima-host/sdcontent-e2e.mjs) plugs in a fake server
// that enforces the device's TLS limit and injects faults. Same code in both, so what the gate proves is
// what the device runs.
//
// Why windows: on this PSRAM-less chip mbedTLS receives TLS records of at most 8 KB
// (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192, kept low so ANIMA online survives in the full OS). GitHub Pages
// sends long bodies in 16 KB records (measured: 16401-byte records for the manifest), so a plain GET dies
// right after "HTTP 200". Every request here asks for at most `window` bytes (Range), and a response that
// small can never produce a record over the cap.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDC_IDLE = 0,
    SDC_CHECKING,       // probing the SD, fetching the manifest
    SDC_DOWNLOADING,    // installing files
    SDC_DONE,           // every selected file installed; content.json written
    SDC_FAILED,         // see code / detail
} sdc_run_phase_t;

typedef enum {
    SDC_OK = 0,
    SDC_E_NO_SD,        // the SD root does not exist (no card / not mounted)
    SDC_E_SD_RO,        // the write probe failed (read-only / locked / failing card)
    SDC_E_MANIFEST,     // the manifest could not be downloaded (http / net detail)
    SDC_E_MANIFEST_BAD, // malformed header/line, or a path outside the write allow-list
    SDC_E_TAG,          // the manifest is for another firmware version
    SDC_E_FETCH,        // a file could not be downloaded (network)
    SDC_E_VERIFY,       // a file never matched its size + SHA-256
    SDC_E_WRITE,        // writing / renaming on the SD failed (full card, bad path on the card)
    SDC_E_NOMEM,        // the download buffer could not be allocated
} sdc_code_t;

typedef struct {
    sdc_run_phase_t phase;
    int  files_total;     // selected files in the manifest
    int  files_done;      // installed + already present (verified)
    int  files_written;   // actually downloaded this run
    int  pct;             // 0..100 by file count (-1 unknown)
    int  recv_kb;         // bytes downloaded this run / 1024
    int  requests;        // HTTP requests made (diagnostics, tests)
    char cur[40];         // current file (basename), for the progress line
    sdc_code_t code;      // why it failed (SDC_OK while fine)
    int  http;            // HTTP status of the failing request (0 = transport/TLS)
    char detail[64];      // the failing path (or a short note)
    char err[72];         // human-readable message (the device glue localizes it)
} sdc_state_t;

// Transport + UI hooks the engine calls. All may be NULL except get.
typedef struct {
    void *ctx;
    // GET bytes [off, off+len) of `url` into buf. Returns the number of bytes copied (<= len; fewer only at
    // the end of the resource) and sets *status (206, or 200 when the server answered with the whole,
    // small resource). Returns -1 on failure with *status = the HTTP status (404, 416 past-the-end, ...) or
    // 0 for a transport / TLS failure. Must never make the server send more than `len` body bytes.
    int  (*get)(void *ctx, const char *url, uint32_t off, uint32_t len, uint8_t *buf, int *status);
    void (*progress)(void *ctx, const sdc_state_t *st);
    void (*tick)(void *ctx);   // called often: feed the watchdog / yield
} sdc_io_t;

typedef struct {
    const char *root;      // SD mount ("/sd"); every write lands under it, at an allow-listed path
    const char *base_url;  // "https://.../sd/<x.y.z>/" (with the trailing '/')
    const char *ver3;      // firmware "x.y.z" the manifest must name
    const char *packs;     // comma list of packs to install, e.g. "core"
    uint32_t    window;    // bytes per request (6144 on the device)
    int         retries;   // extra attempts per window on a transport failure (3)
} sdc_cfg_t;

// Run the whole install. true = complete (content.json written). On false, st->code/http/detail say why;
// files already installed stay (a rerun skips them), no ".part" file is left behind for the failing file.
bool sdc_engine_run(const sdc_cfg_t *cfg, const sdc_io_t *io, sdc_state_t *st);

// content.json under root/system/: {"tag":"x.y.z","complete":bool,"declined":bool}.
bool sdc_content_read(const char *root, char *tag, size_t tagcap, bool *complete, bool *declined);
void sdc_content_write(const char *root, const char *tag, bool complete, bool declined);

// Should the device offer the download? true when content.json is missing, for another firmware, or an
// interrupted run; false when complete for ver3 or when the user chose "Skip" for ver3. (Card presence is
// the caller's check.)
bool sdc_content_needed(const char *root, const char *ver3);

#ifdef __cplusplus
}
#endif
