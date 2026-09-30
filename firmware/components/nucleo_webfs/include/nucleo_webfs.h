// Static file server: serves the desktop shell and app UIs from the SD card.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Register the catch-all static handler on an existing server.
// Routes:
//   /                 -> /sd/www/shell/index.html
//   /<asset>          -> /sd/www/shell/<asset>
//   /apps/<id>/<rest> -> /sd/apps/<id>/www/<rest> (default index.html)
esp_err_t nucleo_webfs_register(httpd_handle_t server);

// Optional low-heap reclaim hook. When a browser pulls a UI asset (it is loading the web OS) and
// contiguous SRAM is tight, the static handler calls this to free RAM for the single-task server —
// wire it to nucleo_anima_l1_unload_if_idle (drops the offline index, ~31 KB, reloads from SD on the
// next query). NULL by default (no-op). Ungated by any key: a connecting client always gets the RAM.
void nucleo_webfs_set_reclaim_cb(void (*cb)(void));

// Web-OS handoff BEFORE the shell loads. `wanted` (set by nucleo_app) says whether this boot must hand
// serving to the lean server-Solo profile. When a BROWSER navigates to "/" and it returns true, the
// handler answers with a small flash-embedded page ("preparing web mode", it reloads once the device is
// back) instead of the shell, and raises a one-shot flag the app task consumes to warm-reboot into Solo.
// So the shell never loads on the fragmented full-OS heap, and the reboot never cuts a desktop session.
void nucleo_webfs_set_handoff_cb(bool (*wanted)(void));
bool nucleo_webfs_take_handoff(void);
