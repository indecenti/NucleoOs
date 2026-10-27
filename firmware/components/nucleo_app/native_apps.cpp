// native_apps.cpp — native apps from the store: load a signed .nap from the SD card and run it.
// Spec: docs/native-apps.md. The portable half (validation, relocation) is nucleo_native/nap_loader.c.
//
// One native app is resident at a time. Loading one:
//   1. re-verifies /sd/apps/<id>/app.nap against the signed package it was installed from (store keys), and
//      remembers the signed SHA-256;
//   2. nap_inspect(): the one layout the SDK produces, every relocation and import, ABI level, layout fingerprint;
//      the relocation tables are then kept in RAM for the passes below;
//   3. allocates .data/.bss and maps the flash scratch region (the 256 KB "coredump" partition: core dumps are off)
//      on the instruction bus; the data-bus alias of the same pages serves the module's constants;
//   4. a DRY pass relocates the whole image in RAM, sector by sector, and hashes it: every relocation target is
//      checked here, before the flash is touched;
//   5. writes that image to the region, unless its last sector says it already holds exactly it (module, firmware,
//      the three base addresses and the image hash all equal) - relaunching an app costs no flash wear. Text
//      literal pools hold absolute .data addresses, so the RAM base is part of that key;
//   6. invalidates the data cache over the data-bus alias (flash writes only flush the instruction-bus side),
//      then hashes the image THROUGH THE DATA BUS and compares: a mapping, cache or flash problem stops here;
//   7. relocates .data, re-hashes the file and compares with the signed hash (it was not swapped meanwhile),
//      runs constructors (only pointers into the module's code), then nucleo_native_main(), which registers the
//      app: the firmware captures that nucleo_app_def_t instead of adding it to the registry.
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
#include "esp_task_wdt.h"
#include "hal/cache_hal.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#define EXTRA_RAM_MAX (96u * 1024)                  // largest extra heap a module may ask to find free at launch
#define RELOCS_MAX    (64u * 1024)                  // relocation tables kept in RAM while loading
#define DATA_ALIGN    16u                           // sdk/native/nap.ld aligns the data segment to 16
#define CACHE_MAGIC   0x4e415043u                   // "CPAN"
#define CACHE_VER     2u

// The last sector of the scratch region: what the pages before it hold.
struct CacheHdr {
    uint32_t magic, ver;
    uint8_t  nap_sha[32];                            // the module file
    uint8_t  fw_sha[32];                             // the firmware it was relocated against (import addresses)
    uint8_t  img_sha[32];                            // the relocated image itself
    uint32_t ibus, dbus, ram, size;                  // the addresses it was relocated for, and its length
    uint32_t check;                                  // FNV-1a of everything above
};

static const esp_partition_t *s_part;
static bool s_part_looked;
static char s_id[NAP_ID_MAX + 1];                    // resident module ("" = none)
static const nucleo_app_def_t *s_def;                // what its nucleo_native_main registered
static bool s_capturing;
static bool s_started;                               // constructors ran: destructors are owed on unload
static void *s_map;                                  // esp_mmu_map handle (instruction-bus address)
static uint32_t s_span;                              // bytes mapped
static uint8_t *s_ram_raw, *s_ram;                   // .data + .bss (s_ram = s_ram_raw aligned to DATA_ALIGN)
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
extern "C" unsigned nucleo_native_flags(void) { return s_id[0] ? s_img.info.flags : 0; }   // NAP_FLAG_* of the resident module
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

// Long passes (hashing, relocating, erasing) run on the UI task: keep its watchdog fed and let others run.
static void feed(void *)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    taskYIELD();
}

// A pointer from the module's tables that we are about to CALL must point into its code.
static bool is_code(uint32_t fn)
{
    return fn >= s_bases.ibus && fn < s_bases.ibus + s_img.text_end;
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

static bool file_sha(FileCtx *fc, uint8_t *buf, uint8_t out[32])
{
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    if (fseek(fc->f, 0, SEEK_SET) != 0) { mbedtls_sha256_free(&h); return false; }
    size_t n;
    while ((n = fread(buf, 1, SECTOR, fc->f)) > 0) { mbedtls_sha256_update(&h, buf, n); feed(nullptr); }
    fc->pos = -1;
    mbedtls_sha256_finish(&h, out);
    mbedtls_sha256_free(&h);
    return true;
}

// ── unload ────────────────────────────────────────────────────────────────────
static void unload(void)
{
    char was[NAP_ID_MAX + 1];
    snprintf(was, sizeof was, "%s", s_id);
    if (was[0] && s_unload_cb) s_unload_cb(was);      // the launcher slot goes back to its placeholder first
    if (s_started) {                                   // destructors, in reverse, only after constructors ran
        for (uint32_t i = s_img.n_fini; i-- > 0;) {
            uint32_t fn = ((uint32_t *)(s_ram + (s_img.fini_array - s_img.data_start)))[i];
            if (is_code(fn)) ((void (*)(void))fn)();
        }
    }
    s_started = false;
    if (s_map) {
        cache_hal_invalidate_addr(s_bases.dbus, s_span);   // no stale constants for the next module at this address
        esp_mmu_unmap(s_map);
        s_map = nullptr;
    }
    free(s_ram_raw); s_ram_raw = s_ram = nullptr;
    s_def = nullptr;
    s_id[0] = 0;
    memset(&s_img, 0, sizeof s_img);
    memset(&s_bases, 0, sizeof s_bases);
    if (was[0]) ESP_LOGW(TAG, "unloaded %s", was);
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
// Relocates the whole image in `buf`-sized pieces. With `old` (a second sector buffer), also writes it to the
// region - only the sectors whose content changed: a relaunch with the data block elsewhere changes just the
// sectors whose literal pools point into .data.
static uint32_t s_written;                           // sectors the last write pass erased + wrote
static bool image_pass(const nap_src_t *src, uint8_t *buf, uint8_t *old, uint8_t sha[32])
{
    bool write = old != nullptr;
    s_written = 0;
    char detail[64];
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    bool ok = true;
    for (uint32_t off = 0; ok && off < s_img.image_size; off += SECTOR) {
        uint32_t n = s_img.image_size - off < SECTOR ? s_img.image_size - off : SECTOR;
        nap_rc_t rc = nap_relocate_flash(src, &s_img, &s_bases, off, buf, n, detail, sizeof detail);
        if (rc != NAP_OK) { set_err(nap_rc_name(rc), detail); ok = false; break; }
        mbedtls_sha256_update(&h, buf, n);
        if (write) {
            if (esp_partition_read(s_part, off, old, n) == ESP_OK && !memcmp(old, buf, n)) { feed(nullptr); continue; }
            if (esp_partition_erase_range(s_part, off, SECTOR) != ESP_OK || esp_partition_write(s_part, off, buf, n) != ESP_OK)
                { set_err("flash write", nullptr); ok = false; }
            s_written++;
        }
        feed(nullptr);
    }
    mbedtls_sha256_finish(&h, sha);
    mbedtls_sha256_free(&h);
    return ok;
}

static bool header_matches(const CacheHdr &want)
{
    CacheHdr h;
    if (esp_partition_read(s_part, s_part->size - SECTOR, &h, sizeof h) != ESP_OK) return false;
    bool ok = h.check == fnv(&h, offsetof(CacheHdr, check)) && !memcmp(&h, &want, offsetof(CacheHdr, check));
    if (!ok && h.magic == CACHE_MAGIC)                 // why the region is rewritten: tuning and diagnostics
        ESP_LOGW(TAG, "rewrite: module %s fw %s image %s ibus %08lx/%08lx ram %08lx/%08lx",
                 memcmp(h.nap_sha, want.nap_sha, 32) ? "new" : "same", memcmp(h.fw_sha, want.fw_sha, 32) ? "new" : "same",
                 memcmp(h.img_sha, want.img_sha, 32) ? "new" : "same", (unsigned long)h.ibus, (unsigned long)want.ibus,
                 (unsigned long)h.ram, (unsigned long)want.ram);
    return ok;
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
    uint8_t signed_sha[32];
    if (!nucleo_store_check_installed(id, NAP_FILE, signed_sha)) { set_err("signature", id); return nullptr; }

    char path[96];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/apps/%s/" NAP_FILE, id);
    FileCtx fc = { fopen(path, "rb"), -1 };
    if (!fc.f) { set_err("open", path); return nullptr; }
    fseek(fc.f, 0, SEEK_END);
    long size = ftell(fc.f);
    nap_src_t src = {};
    src.ctx = &fc; src.read = file_read; src.size = size > 0 ? (uint32_t)size : 0; src.tick = feed;
    src.exports = nap_exports; src.n_exports = nap_exports_count;
    src.abi_max = nap_abi_level; src.expect_id = id;
    src.layout = *(const volatile uint32_t *)&nucleo_native_layout_fw;   // a real load: the symbol stays in the image
    src.flash_max = s_part->size - SECTOR; src.ram_max = RAM_MAX;

    uint8_t *buf = nullptr, *relocs = nullptr;
    const nucleo_app_def_t *def = nullptr;
    char detail[64];
    nap_rc_t rc;
    CacheHdr hdr = {};
    uint8_t sha[32];

    // 2. Everything structural checked before anything is allocated, written or run.
    if ((rc = nap_inspect(&src, &s_img, detail, sizeof detail)) != NAP_OK) { set_err(nap_rc_name(rc), detail); goto fail; }
    if (s_img.info.ram > EXTRA_RAM_MAX) { set_err("RAM", "asks for too much"); goto fail; }
    if (nap_relocs_bytes(&s_img) > RELOCS_MAX) { set_err("too many relocations", nullptr); goto fail; }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) <
        (size_t)s_img.data_memsz + DATA_ALIGN + s_img.info.ram + nap_relocs_bytes(&s_img) + 2 * SECTOR + 4096) { set_err("RAM", nullptr); goto fail; }
    buf = (uint8_t *)malloc(SECTOR);
    relocs = (uint8_t *)malloc(nap_relocs_bytes(&s_img) ? nap_relocs_bytes(&s_img) : 4);
    s_ram_raw = (uint8_t *)heap_caps_calloc(1, s_img.data_memsz + DATA_ALIGN, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf || !relocs || !s_ram_raw) { set_err("RAM", nullptr); goto fail; }
    s_ram = (uint8_t *)(((uintptr_t)s_ram_raw + DATA_ALIGN - 1) & ~(uintptr_t)(DATA_ALIGN - 1));
    if ((rc = nap_cache_relocs(&src, &s_img, relocs, nap_relocs_bytes(&s_img))) != NAP_OK) { set_err(nap_rc_name(rc), "relocations"); goto fail; }

    // 3. Map the scratch pages: instruction bus for code, the data-bus alias of the same pages for constants.
    {
        s_span = (s_img.image_size + PAGE - 1) & ~(PAGE - 1);
        esp_err_t e = esp_mmu_map(s_part->address, s_span, MMU_TARGET_FLASH0,
                                  (mmu_mem_caps_t)(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT), 0, &s_map);
        if (e != ESP_OK) { s_map = nullptr; set_err("mmu map", esp_err_to_name(e)); goto fail; }
        uint32_t ibus = (uint32_t)s_map;
        if (ibus < SOC_IROM_LOW || ibus + s_span > SOC_IROM_HIGH) { set_err("mmu map", "not on the instruction bus"); goto fail; }
        s_bases.ibus = ibus;
        s_bases.dbus = ibus - SOC_IROM_LOW + SOC_DROM_LOW;
        s_bases.ram = (uint32_t)s_ram - s_img.data_start;
    }
    snprintf(s_id, sizeof s_id, "%s", id);           // from here on unload() cleans up everything

    // 4. Dry pass: every relocation target checked, the image hashed - nothing written yet.
    if (!image_pass(&src, buf, nullptr, sha)) goto fail;

    // 5. Write it, unless the region already holds exactly this image for these addresses.
    hdr.magic = CACHE_MAGIC; hdr.ver = CACHE_VER;
    memcpy(hdr.nap_sha, signed_sha, 32);
    memcpy(hdr.fw_sha, esp_app_get_description()->app_elf_sha256, 32);
    memcpy(hdr.img_sha, sha, 32);
    hdr.ibus = s_bases.ibus; hdr.dbus = s_bases.dbus; hdr.ram = s_bases.ram; hdr.size = s_img.image_size;
    hdr.check = fnv(&hdr, offsetof(CacheHdr, check));
    if (header_matches(hdr)) ESP_LOGW(TAG, "%s: scratch region already holds this image (no flash write)", id);
    else {
        ESP_LOGW(TAG, "%s: updating the scratch region (%u B image)", id, (unsigned)s_img.image_size);
        // The header goes first and comes back last: an interrupted write never vouches for a half image.
        if (esp_partition_erase_range(s_part, s_part->size - SECTOR, SECTOR) != ESP_OK) { set_err("flash erase", nullptr); goto fail; }
        uint8_t again[32];
        uint8_t *old = (uint8_t *)malloc(SECTOR);
        if (!old) { set_err("RAM", nullptr); goto fail; }
        bool wrote = image_pass(&src, buf, old, again);
        free(old);
        if (!wrote) goto fail;
        if (memcmp(again, sha, 32)) { set_err("image changed while writing", nullptr); goto fail; }
        ESP_LOGW(TAG, "%s: %u of %u sectors rewritten", id, (unsigned)s_written, (unsigned)((s_img.image_size + SECTOR - 1) / SECTOR));
        if (esp_partition_write(s_part, s_part->size - SECTOR, &hdr, sizeof hdr) != ESP_OK) { set_err("flash write", "header"); goto fail; }
    }

    // 6. What the CPU will read is what was relocated: data cache dropped over the alias, then the whole image
    //    hashed through the data bus.
    cache_hal_invalidate_addr(s_bases.dbus, s_span);
    cache_hal_invalidate_addr(s_bases.ibus, s_span);
    {
        uint8_t seen[32];
        mbedtls_sha256_context h;
        mbedtls_sha256_init(&h);
        mbedtls_sha256_starts(&h, 0);
        for (uint32_t off = 0; off < s_img.image_size; off += SECTOR) {
            uint32_t n = s_img.image_size - off < SECTOR ? s_img.image_size - off : SECTOR;
            mbedtls_sha256_update(&h, (const uint8_t *)(s_bases.dbus + off), n);
            feed(nullptr);
        }
        mbedtls_sha256_finish(&h, seen);
        mbedtls_sha256_free(&h);
        if (memcmp(seen, sha, 32)) { set_err("mapped image differs", nullptr); goto fail; }
    }

    // 7. Data; the file is still the signed one; constructors; entry.
    if ((rc = nap_relocate_data(&src, &s_img, &s_bases, s_ram, detail, sizeof detail)) != NAP_OK) { set_err(nap_rc_name(rc), detail); goto fail; }
    if (!file_sha(&fc, buf, sha) || memcmp(sha, signed_sha, 32)) { set_err("signature", "file changed while loading"); goto fail; }
    fclose(fc.f); fc.f = nullptr;
    free(buf); buf = nullptr;
    free(relocs); relocs = nullptr;
    s_img.rela_mem = s_img.jmprel_mem = nullptr;
    for (uint32_t i = 0; i < s_img.n_init + s_img.n_fini; i++) {
        uint32_t at = i < s_img.n_init ? s_img.init_array + 4 * i : s_img.fini_array + 4 * (i - s_img.n_init);
        uint32_t fn = *(uint32_t *)(s_ram + (at - s_img.data_start));
        if (fn && !is_code(fn)) { set_err("constructor outside the code", nullptr); goto fail; }
    }
    if (!is_code(s_bases.ibus + s_img.entry)) { set_err("entry outside the code", nullptr); goto fail; }
    s_started = true;
    for (uint32_t i = 0; i < s_img.n_init; i++) {
        uint32_t fn = ((uint32_t *)(s_ram + (s_img.init_array - s_img.data_start)))[i];
        if (fn) ((void (*)(void))fn)();
    }
    s_def = nullptr;
    s_capturing = true;
    ((void (*)(void))(s_bases.ibus + s_img.entry))();
    s_capturing = false;
    if (!s_def || !s_def->on_draw) { set_err("no app registered", id); goto fail; }
    def = s_def;
    ESP_LOGW(TAG, "%s loaded: abi %u, %u B code+const, %u B RAM", id, s_img.info.abi, (unsigned)s_img.image_size,
             (unsigned)s_img.data_memsz);
    return def;

fail:
    if (fc.f) fclose(fc.f);
    free(buf);
    free(relocs);
    s_img.rela_mem = s_img.jmprel_mem = nullptr;
    if (s_id[0]) unload();                          // mapped: undo everything (slot, destructors if any, map, RAM)
    else { free(s_ram_raw); s_ram_raw = s_ram = nullptr; if (s_map) { esp_mmu_unmap(s_map); s_map = nullptr; } }
    return nullptr;
}
