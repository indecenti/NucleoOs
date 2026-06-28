// Static file server: serves the desktop shell and app UIs from the SD card.
#pragma once
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
