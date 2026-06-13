// Minimal HTTP server exposing the OS status. See docs/event-protocol.md.
#pragma once
#include "esp_err.h"

// Start the HTTP server and register GET /api/status. Idempotent.
esp_err_t nucleo_httpd_start(void);

// Stop the HTTP server and free port 80 (idempotent). The Evil Portal uses this to take over
// port 80 with its own captive server, then restarts the OS server on exit.
esp_err_t nucleo_httpd_stop(void);
