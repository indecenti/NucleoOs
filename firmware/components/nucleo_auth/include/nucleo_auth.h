// Pairing + session auth for NucleoOS.
//
// The device requires pairing before it serves user data. A 6-digit PIN is shown on the
// Cardputer SCREEN (proof of physical proximity, Chromecast-style); a client exchanges it
// via POST /api/pair for a session token, delivered as an HttpOnly cookie. Every later
// request — including from app iframes and the WebSocket handshake — carries that cookie,
// so the browser authenticates automatically and the apps need no changes.
//
// Protected: /api/fs/*, /api/ota, /api/rec/*, /ws.  Public: static assets, /api/status,
// /api/apps, /api/associations (needed to load the shell and show the pairing overlay).
//
// Tokens persist in the power-loss-safe config store (/cfg/config/auth.json) so a paired
// browser survives reboots. The PIN is STABLE: generated once on first boot and persisted, so it
// stays the same across restarts (a user-chosen settings.security.pin overrides it). Pairing is
// throttled per source IP with escalating backoff, so a hostile client can't lock everyone out.
#pragma once
#include "esp_http_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generate the boot PIN and load persisted session tokens. Call after the /cfg mount and
// SD mount (it reads settings.security.require_pairing best-effort), before httpd start.
void nucleo_auth_init(void);

// True if the request may proceed: pairing not required, or a valid session cookie present.
bool nucleo_auth_request_ok(httpd_req_t *req);

// Send a 401 JSON body. Returns ESP_FAIL so a guarded handler can `return` it directly.
esp_err_t nucleo_auth_reject(httpd_req_t *req);

// The 6-digit pairing PIN (NUL-terminated), for display on the device screen only.
const char *nucleo_auth_pin(void);

// Register /api/pair (POST) and /api/auth/status (GET) on the HTTP server.
esp_err_t nucleo_auth_register(httpd_handle_t server);

// Drop-in guard for a protected handler: 401s and returns when unpaired.
#define NUCLEO_AUTH_GUARD(req) do { if (!nucleo_auth_request_ok(req)) return nucleo_auth_reject(req); } while (0)

#ifdef __cplusplus
}
#endif
