// File API over HTTP: list / read / write / delete / mkdir on the SD card.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Register /api/fs/* handlers on an existing server.
esp_err_t nucleo_fsapi_register(httpd_handle_t server);
