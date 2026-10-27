// native_apps.cpp — native apps from the store: load a signed .nap from the SD card and run it.
// Spec: docs/native-apps.md. The portable half (validation, relocation) is nucleo_native/nap_loader.c.
//
// One native app is resident at a time. Loading one:
//   1. re-verifies /sd/apps/<id>/app.nap against the signed package it was installed from (store keys);
//   2. nap_inspect(): the one layout the SDK produces, every relocation and import, ABI level, layout fingerprint;
//   3. maps the flash scratch region (the 256 KB "coredump" partition: core dumps are off in this firmware) on the
//      instruction bus; the data-bus alias of the same pages serves the module's constants;
//   4. writes the relocated text + rodata there, unless the region already holds exactly this module relocated
//      for this firmware at these addresses (a header in its last sector says so) - a relaunch costs no flash wear;
//   5. reads back through the data bus and compares: a mapping or cache problem stops here, before any jump;
//   6. relocates .data/.bss into a heap block, runs the module's constructors and nucleo_native_main(), which
//      registers the app: the firmware captures that nucleo_app_def_t instead of adding it to the registry.
// The app then runs like any other: same callbacks, same framework. It stays loaded until another native app
// needs the region or the store replaces / removes it (nucleo_native_forget).
#include "nucleo_native.h"
#include "nucleo_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "nap_loader.h"
#include "nucleo_store.h"
#include "nucleo_board.h"
#include "esp_partition.h"
#include "esp_mmu_map.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "soc/soc.h"
}

#if !CONFIG_IDF_TARGET_ESP32S3
#error "native apps rely on the ESP32-S3 MMU: one table shared by the instruction and data buses"
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#error "the coredump partition is the native-app scratch region: core dumps to flash must stay off"
#endif

static const char *TAG = "native";

extern "C" const nap_export_t nap_exports[];        // nap_exports.S (tools/native/gen-exports.mjs)
extern "C" const uint32_t nap_exports_count;
extern "C" const uint32_t nap_abi_level;

#define NAP_FILE      "app.nap"
#define SECTOR        4096u
#define PAGE          0x10000u                      // MMU page
#define RAM_MAX       (48u * 1024)                  // largest .data + .bss accepted
#define CACHE_MAGIC   0x4e415043u                   // "CPAN"
#define CACHE_VER     1u

// The last sector of the scratch region: what the pages before it hold.
struct CacheHdr {
    uint32_t magic, ver;
    uint8_t  nap_sha[32];                            // the module file
    uint8_t  fw_sha[32];                             // the firmware it was relocated against (import addresses)
    uint32_t ibus, dbus, size;                       // where, and how much
    uint32_t check;                                  // FNV-1a of everything above
};

static const esp_partition_t *s_part;
static bool s_part_looked;
static char s_id[NAP_ID_MAX + 1];                    // resident module ("" = none)
static const nucleo_app_def_t *s_def;                // what its nucleo_native_main registered
static bool s_capturing;
static bool s_started;                               // constructors ran: destructors are owed on unload
static void *s_map;                                  // esp_mmu_map handle (instruction-bus address)
static uint8_t *s_ram;                               // .data + .bss
static nap_image_t s_img;
static nap_bases_t s_bases;
static char s_err[80];
static void (*s_unload_cb)(const char *id);

static const esp_partition_t *scratch(void)
{
    if (!s_part_looked) {
        s_part_looked = true;
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (p && p->address % PAGE == 0 && p->size >= 2 * PAGE && p->size % SECTOR == 0 && !p->encrypted) s_part = p;
        else ESP_LOGW(TAG, "no scratch region: native apps unavailable (layout without a coredump partition)");
    }
    return s_part;
}

extern "C" uint32_t nucleo_native_abi_level(void) { return scratch() ? nap_abi_level : 0; }
extern "C" const char *nucleo_native_error(void) { return s_err; }
extern "C" const char *nucleo_native_loaded(void) { return s_id; }
extern "C" void nucleo_native_on_unload(void (*cb)(const char *id)) { s_unload_cb = cb; }

// The ABI's "nucleo_app_register" (abi/v1.txt): a module registers its app exactly like a built-in does; the
// firmware keeps the definition for the launcher slot instead of adding a second registry entry.
extern "C" void nucleo_native_capture_register(const nucleo_app_def_t *def)
{
    if (!s_capturing || !def) { ESP_LOGE(TAG, "nucleo_app_register outside nucleo_native_main: ignored"); return; }
    s_def = def;
}

static constexpr uint32_t FW_LAYOUT = nucleo_native_layout();
// The same value as data, for tools: npm run native:test reads it from the built ELF and checks it against the
// layout of every native app in the store (a mismatch would be refused at load on the device).
extern "C" __attribute__((used)) const uint32_t nucleo_native_layout_fw = FW_LAYOUT;

static uint32_t fnv(const void *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    return h;
}

static void set_err(const char *what, const char *detail)
{
    snprintf(s_err, sizeof s_err, "%s%s%s", what, detail && detail[0] ? ": " : "", detail ? detail : "");
    ESP_LOGE(TAG, "%s", s_err);
}

// ── the .nap file ─────────────────────────────────────────────────────────────
struct FileCtx { FILE *f; long pos; };
static int file_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    FileCtx *c = (FileCtx *)ctx;
    if (c->pos != (long)off && fseek(c->f, (long)off, SEEK_SET) != 0) { c->pos = -1; return -1; }
    size_t n = fread(buf, 1, len, c->f);
    c->pos = (long)off + (long)n;
    return n == len ? 0 : -1;
}

// ── unload ────────────────────────────────────────────────────────────────────
static void unload(void)
{
    if (!s_id[0]) return;
    char was[NAP_ID_MAX + 1];
    snprintf(was, sizeof was, "%s", s_id);
    if (s_unload_cb) s_unload_cb(was);                // the launcher slot goes back to its placeholder first
    for (uint32_t i = s_started ? s_img.n_fini : 0; i-- > 0;) {        // destructors, in reverse
        uint32_t fn = ((uint32_t *)(s_ram + (s_img.fini_array - s_img.data_start)))[i];
        if (fn) ((void (*)(void))fn)();
    }
    s_started = false;
    if (s_map) { esp_mmu_unmap(s_map); s_map = nullptr; }
    free(s_ram); s_ram = nullptr;
    s_def = nullptr;
    s_id[0] = 0;
    memset(&s_img, 0, sizeof s_img);
    ESP_LOGI(TAG, "unloaded %s", was);
}

extern "C" void nucleo_native_forget(const char *id)
{
    if (id && s_id[0] && !strcmp(id, s_id)) {
        const char *fg = nucleo_app_active_id();
        if (fg && !strcmp(fg, id)) { ESP_LOGE(TAG, "refusing to unload %s: it is in the foreground", id); return; }
        unload();
    }
}

// ── flash image ───────────────────────────────────────────────────────────────
static bool cache_valid(const CacheHdr &want)
{
    CacheHdr h;
    if (esp_partition_read(s_part, s_part->size - SECTOR, &h, sizeof h) != ESP_OK) return false;
    return h.check == fnv(&h, offsetof(CacheHdr, check)) && !memcmp(&h, &want, offsetof(CacheHdr, check));
}

static bool write_image(const nap_src_t *src, CacheHdr &hdr)
{
    // Invalidate first: an interrupted write must never leave a header that vouches for a half-written image.
    if (esp_partition_erase_range(s_part, s_part->size - SECTOR, SECTOR) != ESP_OK) return false;
    uint32_t span = (s_img.image_size + SECTOR - 1) & ~(SECTOR - 1);
    if (esp_partition_erase_range(s_part, 0, span) != ESP_OK) { set_err("flash erase", nullptr); return false; }
    uint8_t *buf = (uint8_t *)malloc(SECTOR);
    if (!buf) { set_err("RAM", nullptr); return false; }
    char detail[64];
    bool ok = true;
    for (uint32_t off = 0; ok && off < s_img.image_size; off += SECTOR) {
        uint32_t n = s_img.image_size - off < SECTOR ? s_img.image_size - off : SECTOR;
        nap_rc_t rc = nap_relocate_flash(src, &s_img, &s_bases, off, buf, n, detail, sizeof detail);
        if (rc != NAP_OK) { set_err(nap_rc_name(rc), detail); ok = false; break; }
        if (esp_partition_write(s_part, off, buf, n) != ESP_OK) { set_err("flash write", nullptr); ok = false; }
    }
    free(buf);
    if (!ok) return false;
    hdr.check = fnv(&hdr, offsetof(CacheHdr, check));
    return esp_partition_write(s_part, s_part->size - SECTOR, &hdr, sizeof hdr) == ESP_OK;
}

// ── load ──────────────────────────────────────────────────────────────────────
extern "C" const nucleo_app_def_t *nucleo_native_load(const char *id)
{
    s_err[0] = 0;
    if (!id || !id[0] || strlen(id) > NAP_ID_MAX) { set_err("id", id); return nullptr; }
    if (s_id[0] && !strcmp(s_id, id) && s_def) return s_def;           // already resident
    if (!scratch()) { set_err("no scratch region", nullptr); return nullptr; }
    unload();

    // 1. The file is exactly what the store signed.
    uint8_t sha[32];
    if (!nucleo_store_check_installed(id, NAP_FILE, sha)) { set_err("signature", id); return nullptr; }

    char path[96];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/apps/%s/" NAP_FILE, id);
    FileCtx fc = { fopen(path, "rb"), -1 };
    if (!fc.f) { set_err("open", path); return nullptr; }
    fseek(fc.f, 0, SEEK_END);
    long size = ftell(fc.f);
    nap_src_t src = {};
    src.ctx = &fc; src.read = file_read; src.size = size > 0 ? (uint32_t)size : 0;
    src.exports = nap_exports; src.n_exports = nap_exports_count;
    src.abi_max = nap_abi_level; src.expect_id = id;
    src.layout = *(const volatile uint32_t *)&nucleo_native_layout_fw;   // a real load: the symbol stays in the image
    src.flash_max = s_part->size - SECTOR; src.ram_max = RAM_MAX;

    // 2. Everything checked before anything is written or run.
    char detail[64];
    nap_rc_t rc = nap_inspect(&src, &s_img, detail, sizeof detail);
    if (rc != NAP_OK) { fclose(fc.f); set_err(nap_rc_name(rc), detail); return nullptr; }
    size_t need = s_img.data_memsz + s_img.info.ram;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) < need + 4096) {
        fclose(fc.f); set_err("RAM", nullptr); return nullptr;
    }
    s_ram = (uint8_t *)heap_caps_calloc(1, s_img.data_memsz ? s_img.data_memsz : 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_ram) { fclose(fc.f); set_err("RAM", nullptr); return nullptr; }

    // 3. Map the scratch pages: instruction bus for code, the data-bus alias of the same pages for constants.
    uint32_t span = (s_img.image_size + PAGE - 1) & ~(PAGE - 1);
    esp_err_t e = esp_mmu_map(s_part->address, span, MMU_TARGET_FLASH0,
                              (mmu_mem_caps_t)(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT), 0, &s_map);
    if (e != ESP_OK) { fclose(fc.f); free(s_ram); s_ram = nullptr; set_err("mmu map", esp_err_to_name(e)); return nullptr; }
    snprintf(s_id, sizeof s_id, "%s", id);           // from here on unload() cleans up
    uint32_t ibus = (uint32_t)s_map;
    if (ibus < SOC_IROM_LOW || ibus >= SOC_IROM_HIGH) { fclose(fc.f); unload(); set_err("mmu map", "not on the instruction bus"); return nullptr; }
    s_bases.ibus = ibus;
    s_bases.dbus = ibus - SOC_IROM_LOW + SOC_DROM_LOW;
    s_bases.ram = (uint32_t)s_ram - s_img.data_start;

    // 4. The relocated image, unless the region already holds exactly it.
    CacheHdr hdr = {};
    hdr.magic = CACHE_MAGIC; hdr.ver = CACHE_VER;
    memcpy(hdr.nap_sha, sha, 32);
    memcpy(hdr.fw_sha, esp_app_get_description()->app_elf_sha256, 32);
    hdr.ibus = s_bases.ibus; hdr.dbus = s_bases.dbus; hdr.size = s_img.image_size;
    if (!cache_valid(hdr)) {
        ESP_LOGI(TAG, "%s: writing %u B to the scratch region", id, (unsigned)s_img.image_size);
        if (!write_image(&src, hdr)) { fclose(fc.f); if (!s_err[0]) set_err("flash", nullptr); unload(); return nullptr; }
    }

    // 5. Read back through the data bus: the ELF header has no relocations, so it must equal the file's.
    uint8_t head[16], mapped[16];
    if (file_read(&fc, 0, head, sizeof head) != 0) { fclose(fc.f); unload(); set_err("read", nullptr); return nullptr; }
    memcpy(mapped, (const void *)s_bases.dbus, sizeof mapped);
    if (memcmp(head, mapped, sizeof head)) { fclose(fc.f); unload(); set_err("mapped image differs", nullptr); return nullptr; }

    // 6. Data, constructors, entry.
    rc = nap_relocate_data(&src, &s_img, &s_bases, s_ram, detail, sizeof detail);
    fclose(fc.f);
    if (rc != NAP_OK) { unload(); set_err(nap_rc_name(rc), detail); return nullptr; }
    s_started = true;
    for (uint32_t i = 0; i < s_img.n_init; i++) {
        uint32_t fn = ((uint32_t *)(s_ram + (s_img.init_array - s_img.data_start)))[i];
        if (fn) ((void (*)(void))fn)();
    }
    s_def = nullptr;
    s_capturing = true;
    ((void (*)(void))(s_bases.ibus + s_img.entry))();
    s_capturing = false;
    if (!s_def || !s_def->on_draw) { unload(); set_err("no app registered", id); return nullptr; }
    ESP_LOGI(TAG, "%s loaded: abi %u, %u B code+const, %u B RAM", id, s_img.info.abi, (unsigned)s_img.image_size,
             (unsigned)s_img.data_memsz);
    return s_def;
}
