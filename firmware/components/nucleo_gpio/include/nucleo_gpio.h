// nucleo_gpio — raw GPIO pin control over HTTP, the most basic "scriptable" hardware primitive.
//
// Exposes /api/gpio so a browser automation (nucleo-run.js os.hw.gpio.*) or the LLM agent can drive
// a relay/LED/trigger on the Cardputer's Grove header. Writes are restricted to a SAFE PIN ALLOWLIST
// (the user-accessible Grove pins) so a runaway script can never toggle a flash/display/strapping
// pin and brick the board. Tiny: no task, no state, no RAM cost when idle — it just calls the ESP
// gpio driver per request. This is the RAM-frugal answer to "on-device scripting" — the logic runs
// in the browser/agent; the firmware only flips the pin.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register /api/gpio (GET read ?pin=N, POST write {pin,value,mode}) on the HTTP server.
esp_err_t nucleo_gpio_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
