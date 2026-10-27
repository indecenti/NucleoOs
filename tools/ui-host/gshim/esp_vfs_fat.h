// ui-host gallery shim (IDF)
#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_vfs_fat_info(const char *base, uint64_t *total, uint64_t *free_bytes);
#ifdef __cplusplus
}
#endif
