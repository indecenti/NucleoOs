// nucleo_romstore — see include/nucleo_romstore.h.
//
// Layout of the spare OTA slot while it holds a cartridge: its first 64 KB block carries the header (one 4 KB sector
// is written; the rest of the block stays erased), the cartridge starts at the second block, so the mapping begins on
// an MMU page. The header is erased first and written last: a copy cut short (a pulled battery) leaves no header and
// is simply made again. An app image never starts with the header's magic, so a firmware in that slot is never taken
// for a cartridge either.
#include "nucleo_romstore.h"
#include "nucleo_guest.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "romstore";

#define RS_MAGIC   0x4D4F524Eu                         // "NROM"
#define RS_VERSION 1u
#define RS_DATA    0x10000u                            // the cartridge's offset in the slot: one MMU page
#define RS_CHUNK   4096u

typedef struct {
    uint32_t magic, version;
    uint32_t size, mtime;
    uint32_t head_fnv;                                 // FNV-1a of the file's first 4 KB
    char     path[200];
    uint32_t self_fnv;                                 // FNV-1a of everything above: a torn header is no header
} rs_hdr_t;

static esp_partition_mmap_handle_t s_map;
static bool s_mapped;

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// The slot the device is not running, when it may hold a cartridge: our own partition layout (under M5Launcher the
// other slots are other people's apps), and a running firmware that is confirmed (until then the other slot is the
// way back).
static const esp_partition_t *spare_slot(void)
{
    if (nucleo_guest_hosted() || !nucleo_guest_layout_own()) return NULL;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && (st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW)) return NULL;
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p || p == run || p->type != ESP_PARTITION_TYPE_APP) return NULL;
    return p;
}

const uint8_t *nucleo_rom_map(const char *path, uint32_t *size, void (*progress)(uint32_t, uint32_t, void *), void *user)
{
    nucleo_rom_unmap();
    if (size) *size = 0;
    if (!path || strlen(path) >= sizeof(((rs_hdr_t *)0)->path)) return NULL;
    const esp_partition_t *slot = spare_slot();
    if (!slot) return NULL;
    struct stat sb;
    if (stat(path, &sb) != 0 || sb.st_size <= 0 || (uint32_t)sb.st_size > slot->size - RS_DATA) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc(RS_CHUNK);
    if (!buf) { fclose(f); return NULL; }

    rs_hdr_t want = { 0 };
    want.magic = RS_MAGIC; want.version = RS_VERSION;
    want.size = (uint32_t)sb.st_size; want.mtime = (uint32_t)sb.st_mtime;
    snprintf(want.path, sizeof want.path, "%s", path);
    size_t n = fread(buf, 1, RS_CHUNK, f);
    want.head_fnv = fnv(buf, n, 2166136261u);
    want.self_fnv = fnv(&want, offsetof(rs_hdr_t, self_fnv), 2166136261u);

    rs_hdr_t have;
    bool same = esp_partition_read(slot, 0, &have, sizeof have) == ESP_OK && !memcmp(&have, &want, sizeof have);
    if (!same) {
        ESP_LOGW(TAG, "copying %s (%u B) into %s", path, (unsigned)want.size, slot->label);
        // The header goes first, so an interrupted copy is never taken for a good one.
        esp_err_t e = esp_partition_erase_range(slot, 0, RS_CHUNK);
        const uint32_t span = (want.size + RS_CHUNK - 1) & ~(RS_CHUNK - 1);
        for (uint32_t at = 0; e == ESP_OK && at < span; at += 0x10000) {          // erase in 64 KB steps
            const uint32_t step = span - at < 0x10000 ? span - at : 0x10000;
            e = esp_partition_erase_range(slot, RS_DATA + at, step);
            esp_task_wdt_reset();
        }
        fseek(f, 0, SEEK_SET);
        for (uint32_t at = 0; e == ESP_OK && at < want.size; at += RS_CHUNK) {
            n = fread(buf, 1, RS_CHUNK, f);
            if (n == 0) { e = ESP_FAIL; break; }
            e = esp_partition_write(slot, RS_DATA + at, buf, n);
            if ((at & 0xFFFF) == 0) {
                esp_task_wdt_reset();
                if (progress) progress(at, want.size, user);
            }
        }
        if (e == ESP_OK) e = esp_partition_write(slot, 0, &want, sizeof want);
        if (e != ESP_OK) { ESP_LOGE(TAG, "copy failed: 0x%x", (unsigned)e); free(buf); fclose(f); return NULL; }
        if (progress) progress(want.size, want.size, user);
    }
    free(buf);
    fclose(f);
    const void *p = NULL;
    if (esp_partition_mmap(slot, RS_DATA, want.size, ESP_PARTITION_MMAP_DATA, &p, &s_map) != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed");
        return NULL;
    }
    s_mapped = true;
    if (size) *size = want.size;
    return (const uint8_t *)p;
}

void nucleo_rom_unmap(void)
{
    if (!s_mapped) return;
    esp_partition_munmap(s_map);
    s_mapped = false;
}
