// ui-host app gallery: the few device services a "return 0" stub cannot stand in for (a NULL pointer the app
// dereferences, a number that must look real). Everything else an app links is auto-stubbed by apps.mjs.
#include "host_state.h"
#include "nucleo_storage.h"
#include <stdint.h>
#include <string.h>

extern "C" {
const nucleo_storage_info_t *nucleo_storage_info(void)
{
    static nucleo_storage_info_t i = { true, "FAT32", 31914983424ull, 21474836480ull, 0 };
    return &i;
}
int nucleo_power_battery_mv(void) { return 3987; }
int nucleo_registry_count(void) { return 64; }
// Deterministic "random": the same frame on every run (the golden gate hashes it).
static uint32_t s_rng = 0x2545F491u;
uint32_t esp_random(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }
void esp_fill_random(void *buf, size_t n) { uint8_t *b = (uint8_t *)buf; while (n--) *b++ = (uint8_t)(esp_random() >> 24); }
}
