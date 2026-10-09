// native-host: the slice of ESP-IDF's esp_http_server.h a game's web endpoint compiles against (Costellazioni's
// cross-play save). Handlers are registered and kept, so a scenario can call one with a fake request.
#pragma once
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
typedef struct httpd_req { const char *uri; int method; size_t content_len; const char *body; size_t body_off; char out[4096]; size_t out_len; char status[48]; } httpd_req_t;
typedef struct httpd_uri { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *r); void *user_ctx; } httpd_uri_t;
#define HTTPD_RESP_USE_STRLEN (-1)
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, long len);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len);
#ifdef __cplusplus
}
#endif
