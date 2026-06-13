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
