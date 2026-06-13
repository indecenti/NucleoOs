// WebSocket transport: pushes event-bus deltas to connected clients at /ws.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Register the /ws endpoint and wire it to the event bus as the live sink.
esp_err_t nucleo_ws_register(httpd_handle_t server);

// Number of live WebSocket clients (i.e. active web sessions). The launcher uses this to
// hand the device over to a connected browser ("remote handoff") and reclaim it on 0.
int nucleo_ws_client_count(void);

// Called by the HTTP server's close_fn when ANY socket closes, so a dropped browser is
// detected immediately (not only when the next broadcast send fails). Safe for non-WS fds.
void nucleo_ws_notify_close(int fd);
