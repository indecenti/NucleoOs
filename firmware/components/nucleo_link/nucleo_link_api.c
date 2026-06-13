// nucleo_link_api.c — /api/link/* HTTP surface for the web twin of "Vicino".
//
// Thin JSON wrapper over nucleo_link_espnow.h so the browser app (apps/nearby) and the ANIMA skill can
// drive ESP-NOW transfers. Mirrors nucleo_ir.c's handler style (read_body/json + NUCLEO_AUTH_GUARD on
// mutating POSTs). The engine is started lazily here so the web path works WITHOUT the native app — note
// the two front-ends are mutually exclusive in practice: the native app declares NX_NET_APP and stops the
// HTTP server while it is foreground, so these routes only serve when the native app is closed.
#include "nucleo_link_espnow.h"
#include "nucleo_auth.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void ensure_started(void) { nlink_svc_start(); }   // idempotent

static char *read_body(httpd_req_t *req, int max) {
    int len = req->content_len;
    if (len <= 0 || len > max) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) { int r = httpd_req_recv(req, buf + got, len - got); if (r <= 0) { free(buf); return NULL; } got += r; }
    buf[len] = 0;
    return buf;
}
static esp_err_t json_send(httpd_req_t *req, const char *body) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

// ---- GET /api/link/peers : discovered devices + this device's link info ----
static esp_err_t peers_get(httpd_req_t *req) {
    ensure_started();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", nlink_svc_name());
    cJSON_AddNumberToObject(root, "channel", nlink_svc_channel());
    cJSON_AddStringToObject(root, "inbox", nlink_svc_inbox());
    cJSON *arr = cJSON_AddArrayToObject(root, "peers");
    for (int i = 0; i < nlink_svc_peer_count(); i++) {
        const uint8_t *m = nlink_svc_peer_mac(i);
        char mac[18]; snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "i", i);
        cJSON_AddStringToObject(p, "name", nlink_svc_peer_name(i));
        cJSON_AddStringToObject(p, "mac", mac);
        cJSON_AddStringToObject(p, "proto", nlink_svc_peer_proto(i) == NLINK_PROTO_BRUCE ? "bruce" : "nucleo");
        cJSON_AddItemToArray(arr, p);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    json_send(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

// ---- GET /api/link/status : live transfer status ---------------------------
static esp_err_t status_get(httpd_req_t *req) {
    nlink_status_t s; nlink_svc_status(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", s.active);
    cJSON_AddBoolToObject(root, "sending", s.sending);
    cJSON_AddStringToObject(root, "proto", s.proto == NLINK_PROTO_BRUCE ? "bruce" : "nucleo");
    cJSON_AddNumberToObject(root, "state", s.state);
    cJSON_AddNumberToObject(root, "reason", s.reason);
    cJSON_AddNumberToObject(root, "done", s.done);
    cJSON_AddNumberToObject(root, "total", s.total);
    cJSON_AddNumberToObject(root, "rate", s.rate_bps);
    cJSON_AddStringToObject(root, "name", s.name);
    cJSON_AddStringToObject(root, "peer", s.peer);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    json_send(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

// ---- GET /api/link/offer : pending incoming offer --------------------------
static esp_err_t offer_get(httpd_req_t *req) {
    char name[64] = "", from[40] = ""; uint32_t sz = 0;
    bool pending = nlink_svc_offer_pending(name, sizeof name, from, sizeof from, &sz);
    char out[160];
    snprintf(out, sizeof out, "{\"pending\":%s,\"name\":\"%.40s\",\"from\":\"%.30s\",\"size\":%u}",
             pending ? "true" : "false", name, from, (unsigned)sz);
    return json_send(req, out);
}

// ---- GET /api/link/cmd : pending incoming command --------------------------
static esp_err_t cmd_get(httpd_req_t *req) {
    char cmd[160] = "", from[40] = "";
    bool pending = nlink_svc_cmd_pending(cmd, sizeof cmd, from, sizeof from);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "pending", pending);
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "from", from);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    json_send(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

static nlink_proto_t proto_of(cJSON *in) {
    cJSON *p = in ? cJSON_GetObjectItem(in, "proto") : NULL;
    return (cJSON_IsString(p) && strcasecmp(p->valuestring, "bruce") == 0) ? NLINK_PROTO_BRUCE : NLINK_PROTO_NUCLEO;
}

// ---- POST /api/link/discover {proto} ---------------------------------------
static esp_err_t discover_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    ensure_started();
    char *body = read_body(req, 128); cJSON *in = body ? cJSON_Parse(body) : NULL;
    nlink_svc_discover(proto_of(in));
    if (in) cJSON_Delete(in);
    if (body) free(body);
    return json_send(req, "{\"ok\":true}");
}

// ---- POST /api/link/listen {proto} -----------------------------------------
static esp_err_t listen_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    ensure_started();
    char *body = read_body(req, 128); cJSON *in = body ? cJSON_Parse(body) : NULL;
    nlink_svc_listen(proto_of(in));
    if (in) cJSON_Delete(in);
    if (body) free(body);
    return json_send(req, "{\"ok\":true}");
}

// ---- POST /api/link/send {peer, path, proto} -------------------------------
static esp_err_t send_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    ensure_started();
    char *body = read_body(req, 512);
    bool ok = false;
    if (body) {
        cJSON *in = cJSON_Parse(body); free(body);
        if (in) {
            cJSON *peer = cJSON_GetObjectItem(in, "peer");
            cJSON *path = cJSON_GetObjectItem(in, "path");
            if (cJSON_IsNumber(peer) && cJSON_IsString(path))
                ok = nlink_svc_send_file((int)peer->valuedouble, path->valuestring, proto_of(in));
            cJSON_Delete(in);
        }
    }
    return json_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- POST /api/link/cmd {peer, command, proto} -----------------------------
static esp_err_t cmd_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    ensure_started();
    char *body = read_body(req, 512);
    bool ok = false;
    if (body) {
        cJSON *in = cJSON_Parse(body); free(body);
        if (in) {
            cJSON *peer = cJSON_GetObjectItem(in, "peer");
            cJSON *cmd  = cJSON_GetObjectItem(in, "command");
            if (cJSON_IsNumber(peer) && cJSON_IsString(cmd))
                ok = nlink_svc_send_cmd((int)peer->valuedouble, cmd->valuestring, proto_of(in));
            cJSON_Delete(in);
        }
    }
    return json_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- POST /api/link/offer {accept} : answer the pending offer --------------
static esp_err_t offer_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char *body = read_body(req, 128); bool accept = false;
    if (body) { cJSON *in = cJSON_Parse(body); free(body);
        if (in) { cJSON *a = cJSON_GetObjectItem(in, "accept"); accept = cJSON_IsTrue(a); cJSON_Delete(in); } }
    nlink_svc_offer_answer(accept);
    return json_send(req, "{\"ok\":true}");
}

// ---- POST /api/link/cmd/confirm {ok} ---------------------------------------
static esp_err_t cmd_confirm_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char *body = read_body(req, 128); bool ok = false;
    if (body) { cJSON *in = cJSON_Parse(body); free(body);
        if (in) { cJSON *a = cJSON_GetObjectItem(in, "ok"); ok = cJSON_IsTrue(a); cJSON_Delete(in); } }
    nlink_svc_cmd_confirm(ok);
    return json_send(req, "{\"ok\":true}");
}

// ---- POST /api/link/cancel -------------------------------------------------
static esp_err_t cancel_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    nlink_svc_cancel();
    return json_send(req, "{\"ok\":true}");
}

esp_err_t nucleo_link_api_register(httpd_handle_t server) {
    httpd_uri_t routes[] = {
        { .uri = "/api/link/peers",       .method = HTTP_GET,  .handler = peers_get },
        { .uri = "/api/link/status",      .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/link/offer",       .method = HTTP_GET,  .handler = offer_get },
        { .uri = "/api/link/offer",       .method = HTTP_POST, .handler = offer_post },
        { .uri = "/api/link/cmd",         .method = HTTP_GET,  .handler = cmd_get },
        { .uri = "/api/link/cmd",         .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/api/link/cmd/confirm", .method = HTTP_POST, .handler = cmd_confirm_post },
        { .uri = "/api/link/discover",    .method = HTTP_POST, .handler = discover_post },
        { .uri = "/api/link/listen",      .method = HTTP_POST, .handler = listen_post },
        { .uri = "/api/link/send",        .method = HTTP_POST, .handler = send_post },
        { .uri = "/api/link/cancel",      .method = HTTP_POST, .handler = cancel_post },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        httpd_register_uri_handler(server, &routes[i]);
    return ESP_OK;
}
