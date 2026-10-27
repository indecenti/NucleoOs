// ui-host gallery shim (IDF): the HTTP server API, declarations only (never runs on the host).
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT = 4, HTTP_DELETE = 0, HTTP_OPTIONS = 6 } httpd_method_t;
typedef struct httpd_req { httpd_handle_t handle; int method; const char uri[513]; size_t content_len; void *aux; void *user_ctx; void *sess_ctx; } httpd_req_t;
typedef struct httpd_uri { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *r); void *user_ctx; } httpd_uri_t;
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_200 "200 OK"
#define HTTPD_400 "400 Bad Request"
#define HTTPD_404 "404 Not Found"
#define HTTPD_500 "500 Internal Server Error"
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
esp_err_t httpd_unregister_uri(httpd_handle_t h, const char *uri);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, ssize_t n);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg);
int httpd_req_recv(httpd_req_t *r, char *b, size_t n);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *b, size_t n);
esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t n);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *f);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *f, char *v, size_t n);
#ifdef __cplusplus
}
#endif
