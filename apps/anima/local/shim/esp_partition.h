// Host shim for ESP-IDF esp_partition.h. The only consumer is nucleo_anima_l1.c,
// which tries to memory-map an "anima_enc" flash partition as a fast path and falls
// back to the SD file when it is absent. On the host there is no flash partition, so
// find_first() returns NULL and L1 transparently uses the file at ENC_PATH — exactly
// the firmware's own behaviour on a device whose encoder lives only on the SD card.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef int esp_partition_mmap_memory_t;
typedef uint32_t esp_partition_mmap_handle_t;

#define ESP_PARTITION_TYPE_DATA     0x01
#define ESP_PARTITION_SUBTYPE_ANY   0xff
#define ESP_PARTITION_MMAP_DATA     1

typedef struct {
    esp_partition_type_t    type;
    esp_partition_subtype_t subtype;
    uint32_t                address;
    uint32_t                size;
    char                    label[17];
    int                     encrypted;
} esp_partition_t;

static inline const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label) {
    (void)type; (void)subtype; (void)label;
    return NULL;   // no flash partitions on host -> L1 uses the SD/file path
}

static inline esp_err_t esp_partition_mmap(
    const esp_partition_t *part, size_t offset, size_t size,
    esp_partition_mmap_memory_t memory, const void **out_ptr,
    esp_partition_mmap_handle_t *out_handle) {
    (void)part; (void)offset; (void)size; (void)memory; (void)out_ptr; (void)out_handle;
    return ESP_FAIL;   // never reached (find_first returns NULL), present for linking
}

static inline void esp_partition_munmap(esp_partition_mmap_handle_t handle) {
    (void)handle;
}
