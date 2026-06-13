// Host shim for ESP-IDF esp_heap_caps.h. Several ANIMA sources (nucleo_anima_l1.c) log heap stats
// via heap_caps_*; the L1 reclaim path also probes them. On a PC there is no constrained internal
// SRAM, so report a generous, stable pool — the host harness only exercises the *logic*, never the
// memory cliff (the online/TLS tier is stubbed out on the host anyway). Device builds use the real
// ESP-IDF implementation; this exists ONLY so the firmware sources compile UNCHANGED in the harness.
#pragma once
#include <stddef.h>

// Capability bits — exact values are irrelevant on the host (the stubs ignore them); they only
// need to exist as compile-time constants where the firmware writes MALLOC_CAP_DEFAULT/INTERNAL/DMA.
#define MALLOC_CAP_DEFAULT   (1 << 0)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_INTERNAL  (1 << 11)

static inline size_t heap_caps_get_free_size(unsigned caps)          { (void)caps; return (size_t)4 * 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return (size_t)1 * 1024 * 1024; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps)  { (void)caps; return (size_t)2 * 1024 * 1024; }
