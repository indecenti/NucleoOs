// nucleo_ir — infrared transmit for NucleoOS.
//
// The M5Stack Cardputer has an IR LED wired to GPIO44 (no built-in receiver). This component
// drives it through the ESP32-S3 RMT peripheral at 1 µs resolution with a per-protocol carrier
// (38/40/36 kHz), turning the pure encoder's mark/space list (nucleo_ir_proto.h) into real light.
//
// Design notes:
//   * Single owner: every transmit goes through one mutex, so the web "send", a TV-B-Gone sweep,
//     and an ANIMA-triggered action can never drive the LED at the same time.
//   * Web-native split: the firmware does the timing-critical work (encode + RMT) and a built-in
//     TV-B-Gone power sweep that runs even with no browser; the rich device/button database and
//     the .ir import live in the web app (apps/ir-remote), so growing it needs no reflash.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include "nucleo_ir_proto.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the RMT TX channel + carrier + mutex. Safe no-op (returns ESP_ERR_INVALID_STATE) if
// the IR GPIO is unavailable. Call once at boot, before the HTTP server starts.
esp_err_t nucleo_ir_init(void);

// Encode (protocol, address, command) and transmit it `repeats` times. repeats<=0 uses the
// protocol's conventional default (e.g. 3 for Sony). Thread-safe; blocks until sent.
esp_err_t nucleo_ir_send_proto(nir_proto_t proto, uint32_t address, uint32_t command, int repeats);

// Transmit a raw alternating mark/space list (µs, starting with a mark) at `carrier_hz`.
// Used by the .ir import path and learned signals. Thread-safe.
esp_err_t nucleo_ir_send_raw(const uint16_t *durations, int count, uint16_t carrier_hz, int repeats);

// TV-B-Gone style power sweep over the built-in code table, optionally filtered by region
// ("us", "eu", "asia", or NULL/"all"). Runs on a background task; returns immediately.
// Only one sweep at a time. Returns ESP_ERR_INVALID_STATE if a sweep is already running.
esp_err_t nucleo_ir_tvbgone_start(const char *region);
void      nucleo_ir_tvbgone_stop(void);
// Progress for the UI. Any out pointer may be NULL. Returns true while a sweep is active.
bool      nucleo_ir_tvbgone_status(int *sent, int *total);

// True while the LED is busy (a send or a sweep is in flight).
bool nucleo_ir_busy(void);

// IR jammer: floods the IR band with carrier noise to block nearby receivers (authorised testing).
// mode = "sweep" | "random" | "constant" (NULL -> sweep). Runs on a background task until stopped.
esp_err_t nucleo_ir_jammer_start(const char *mode);
void      nucleo_ir_jammer_stop(void);
bool      nucleo_ir_jammer_running(void);

// Register /api/ir/* on the HTTP server. Call from nucleo_httpd_start().
esp_err_t nucleo_ir_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
