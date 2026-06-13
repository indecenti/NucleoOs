// Host shim for ESP-IDF esp_log.h — routes the device log macros to stderr so the
// firmware's ESP_LOGx lines show up in the terminal exactly where /api/logs used to.
// Needs cl /Zc:preprocessor for conforming ##__VA_ARGS__.
#pragma once
#include <stdio.h>

typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
    ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fprintf(stderr, "D (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

static inline void esp_log_level_set(const char *tag, esp_log_level_t level) {
    (void)tag; (void)level;
}
