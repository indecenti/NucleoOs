// ui-host gallery shim (IDF): NVS always empty on the host.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *n);
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k);
#ifdef __cplusplus
}
#endif
