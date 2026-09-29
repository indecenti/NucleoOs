// Host stub: only the reset-reason query launcher_menu.cpp gates the return-cursor on.
#pragma once
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
