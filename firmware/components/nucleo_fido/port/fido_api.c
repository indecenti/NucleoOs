// fido_api — HTTP endpoints for the FIDO web console (managed from the browser
// shell). Available in normal mode: FIDO keeps Wi-Fi/httpd up, and the resident
// store + PIN load lazily without USB. Every route is auth-gated. The device only
// serves small JSON — the console UI is all in the browser (device stays light).
#include "esp_http_server.h"
#include "nucleo_fido.h"
#include "nucleo_auth.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static int read_body(httpd_req_t *req, char *buf, int cap) {
    int total = 0, r;
    while (total < cap - 1 && (r = httpd_req_recv(req, buf + total, cap - 1 - total)) > 0) total += r;
    if (r < 0) return -1;
    buf[total] = 0;
    return total;
}

static esp_err_t status_get(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    nucleo_fido_admin_open();
    char buf[224];
    snprintf(buf, sizeof buf,
             "{\"keyHardware\":%s,\"pinSet\":%s,\"pinRetries\":%d,\"credCount\":%d,\"connected\":%s}",
             nucleo_fido_key_is_hardware() ? "true" : "false",
             nucleo_fido_pin_is_set() ? "true" : "false",
             nucleo_fido_pin_retries(), nucleo_fido_cred_count(),
             nucleo_fido_ready() ? "true" : "false");
    return send_json(req, buf);
}

static esp_err_t creds_get(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    nucleo_fido_admin_open();
    int n = nucleo_fido_cred_count();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        fido_cred_view_t v;
        if (!nucleo_fido_cred_get(i, &v)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", i);
        cJSON_AddStringToObject(o, "rp", v.rp);
        cJSON_AddStringToObject(o, "user", v.user);
        cJSON_AddNumberToObject(o, "signCount", v.sign_count);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    esp_err_t e = send_json(req, s ? s : "[]");
    if (s) cJSON_free(s);
    cJSON_Delete(arr);
    return e;
}

static esp_err_t cred_delete_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char body[64];
    if (read_body(req, body, sizeof body) < 0) return send_json(req, "{\"ok\":false}");
    int idx = -1;
    cJSON *j = cJSON_Parse(body);
    if (j) {
        cJSON *ix = cJSON_GetObjectItem(j, "index");
        if (cJSON_IsNumber(ix)) idx = ix->valueint;
        cJSON_Delete(j);
    }
    bool ok = (idx >= 0) && nucleo_fido_cred_delete(idx);
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t pin_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char body[128];
    if (read_body(req, body, sizeof body) < 0) return send_json(req, "{\"ok\":false}");
    bool ok = false;
    cJSON *j = cJSON_Parse(body);
    if (j) {
        cJSON *p = cJSON_GetObjectItem(j, "pin");
        if (cJSON_IsString(p) && p->valuestring) ok = nucleo_fido_pin_set(p->valuestring);
        cJSON_Delete(j);
    }
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"pin length 4-63\"}");
}

static esp_err_t reset_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    nucleo_fido_admin_reset();
    return send_json(req, "{\"ok\":true}");
}

// Called by nucleo_httpd when the server starts. Single-line httpd_uri_t inits so
// the api-spec generator (tools/gen-api-spec.mjs) can discover the routes.
esp_err_t nucleo_fido_register_api(httpd_handle_t server) {
    httpd_uri_t u_status = { .uri = "/api/fido/status",      .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t u_creds  = { .uri = "/api/fido/creds",       .method = HTTP_GET,  .handler = creds_get };
    httpd_uri_t u_del    = { .uri = "/api/fido/cred/delete", .method = HTTP_POST, .handler = cred_delete_post };
    httpd_uri_t u_pin    = { .uri = "/api/fido/pin",         .method = HTTP_POST, .handler = pin_post };
    httpd_uri_t u_reset  = { .uri = "/api/fido/reset",       .method = HTTP_POST, .handler = reset_post };
    httpd_register_uri_handler(server, &u_status);
    httpd_register_uri_handler(server, &u_creds);
    httpd_register_uri_handler(server, &u_del);
    httpd_register_uri_handler(server, &u_pin);
    return httpd_register_uri_handler(server, &u_reset);
}
