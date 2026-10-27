// ui-host gallery shim (IDF)
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH, ESP_MAC_BASE } esp_mac_type_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
#ifdef __cplusplus
}
#endif
