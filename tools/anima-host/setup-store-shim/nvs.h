// Host shim for ESP-IDF nvs.h — an in-memory NVS (tools/anima-host/nvs_host.c) with the real API
// semantics the config stores rely on: READONLY open of a missing namespace -> NOT_FOUND, READWRITE
// creates it, get_str with a NULL buffer reports the size (incl. NUL), erase of a missing key -> NOT_FOUND.
// Error codes match components/nvs_flash/include/nvs.h.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_ERR_NVS_BASE            0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND       (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_READ_ONLY       (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_HANDLE  (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH  (ESP_ERR_NVS_BASE + 0x0c)

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

// Test hooks (host only).
void nvs_host_reset(void);                    // drop every namespace/key, NVS initialised
void nvs_host_set_initialized(bool on);       // false = every open fails with NOT_INITIALIZED
int  nvs_host_key_count(const char *name);    // keys stored in a namespace (-1 = namespace absent)
int  nvs_host_open_handles(void);             // leak check: handles opened but never closed
