// WebSocket transport: pushes event-bus deltas to connected clients at /ws.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Register the /ws endpoint and wire it to the event bus as the live sink.
esp_err_t nucleo_ws_register(httpd_handle_t server);

// Number of live WebSocket clients (i.e. active web sessions). Counts EVERY /ws client
// (the shell, plus any standalone app page that subscribes to the event bus).
int nucleo_ws_client_count(void);

// Number of live clients that identified themselves as the OS shell (connected to /ws?shell=1).
// The launcher gates the "remote handoff" (suspend UI + screen-off + RAM reclaim) on THIS count, so
// a standalone app page or a probe opening /ws can't trigger the handoff — only the real web OS does.
int nucleo_ws_shell_count(void);

// Called by the HTTP server's close_fn when ANY socket closes, so a dropped browser is
// detected immediately (not only when the next broadcast send fails). Safe for non-WS fds.
void nucleo_ws_notify_close(int fd);
