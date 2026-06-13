// nucleo_gpio — see nucleo_gpio.h. Thin, stateless, allowlist-guarded GPIO over /api/gpio.
#include "nucleo_gpio.h"
#include "nucleo_auth.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SAFE PIN ALLOWLIST — only the Cardputer's user-accessible header/Grove pins. Writing is the most
// dangerous, so it is the tightest set; reading is allowed on a couple more. Everything else (SD,
// display, keyboard matrix, IR LED on 44, mic, strapping/flash pins) is rejected with 403. Override
// for other boards with -DNUCLEO_GPIO_WRITE_PINS / -DNUCLEO_GPIO_READ_PINS.
#ifndef NUCLEO_GPIO_WRITE_PINS
#define NUCLEO_GPIO_WRITE_PINS { 1, 2 }       // Grove G2 (GPIO2), G1 (GPIO1)
#endif
#ifndef NUCLEO_GPIO_READ_PINS
#define NUCLEO_GPIO_READ_PINS  { 0, 1, 2 }     // + GPIO0 (BtnA/G0) for reads
#endif

static const int WRITE_OK[] = NUCLEO_GPIO_WRITE_PINS;
static const int READ_OK[]  = NUCLEO_GPIO_READ_PINS;

static bool allowed(const int *list, int n, int pin) {
    for (int i = 0; i < n; i++) if (list[i] == pin) return true;
    return false;
}

static esp_err_t json(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_type(req, "application/json");
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

// POST /api/gpio  {"pin":2,"value":1,"mode":"out"}  -> set a pin level (auth-gated).
static esp_err_t gpio_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    int len = req->content_len;
    if (len <= 0 || len > 256) return json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"body\"}");
    char buf[257]; int got = 0, r;
    while (got < len) { r = httpd_req_recv(req, buf + got, len - got); if (r <= 0) break; got += r; }
    buf[got > 0 ? got : 0] = 0;
    cJSON *in = cJSON_Parse(buf);
    if (!in) return json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"json\"}");
    cJSON *jp = cJSON_GetObjectItem(in, "pin");
    cJSON *jv = cJSON_GetObjectItem(in, "value");
    int pin = cJSON_IsNumber(jp) ? jp->valueint : -1;
    int val = cJSON_IsNumber(jv) ? (jv->valueint ? 1 : 0) : 0;
    cJSON_Delete(in);

    if (pin < 0) return json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"pin\"}");
    if (!allowed(WRITE_OK, (int)(sizeof WRITE_OK / sizeof WRITE_OK[0]), pin))
        return json(req, "403 Forbidden", "{\"ok\":false,\"err\":\"pin not writable (safe allowlist)\"}");

    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, val);
    char out[64]; snprintf(out, sizeof out, "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, val);
    return json(req, NULL, out);
}

// GET /api/gpio?pin=N -> read a pin level (read-only, no auth — like /api/status).
static esp_err_t gpio_get(httpd_req_t *req) {
    char q[64] = { 0 }, pins[12] = { 0 };
    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "pin", pins, sizeof pins) != ESP_OK)
        return json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"pin\"}");
    int pin = atoi(pins);
    if (!allowed(READ_OK, (int)(sizeof READ_OK / sizeof READ_OK[0]), pin))
        return json(req, "403 Forbidden", "{\"ok\":false,\"err\":\"pin not readable (safe allowlist)\"}");
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    int v = gpio_get_level((gpio_num_t)pin);
    char out[64]; snprintf(out, sizeof out, "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, v);
    return json(req, NULL, out);
}

esp_err_t nucleo_gpio_register(httpd_handle_t server) {
    httpd_uri_t routes[] = {
        { .uri = "/api/gpio", .method = HTTP_POST, .handler = gpio_post },
        { .uri = "/api/gpio", .method = HTTP_GET,  .handler = gpio_get },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        httpd_register_uri_handler(server, &routes[i]);
    return ESP_OK;
}
