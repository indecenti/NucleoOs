#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);
uint32_t esp_get_free_heap_size(void);
#ifdef __cplusplus
}
#endif
