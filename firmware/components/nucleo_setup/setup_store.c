// Three-tier persistence for nucleo_setup's config documents. See setup_store.h.
#include "setup_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>   // mkdir() for the SD mirror subtree
#include "esp_log.h"
#include "nvs.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define STORE_WAIT_TICK() vTaskDelay(pdMS_TO_TICKS(10))
#else
#define STORE_WAIT_TICK() ((void)0)   // host: single-threaded, nothing can be in flight
#endif

static const char *TAG = "setup";

// Read a whole small config file. CAPS the allocation: `n` comes from ftell on a file that may be
// corrupt or attacker-placed (SD legacy path), and a multi-MB malloc on the ~18 KB heap would OOM the
// boot. Config docs here are < 4 KB; 32 KB is a generous ceiling that rejects the pathological case.
#define SLURP_MAX (32 * 1024)
char *setup_store_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > SLURP_MAX) { fclose(f); return NULL; }
    char *b = malloc(n + 1);
    if (b && fread(b, 1, n, f) == (size_t)n) b[n] = '\0'; else { free(b); b = NULL; }
    fclose(f);
    return b;
}

// Atomic write via temp+rename. Parent dirs must already exist. Returns false — never crashes — if the
// path's filesystem is absent. Works on BOTH stores: LittleFS rename overwrites the destination in place
// (atomic, power-loss-safe), but FATFS (the SD) rename FAILS if the destination already exists — so on a
// rename failure we remove the old file and retry. tmp is only dropped if the retry also fails, so the
// destination is never left both-gone by the common (dest-exists) case.
static bool write_file_atomic(const char *path, const char *text)
{
    char tmp[160];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = (fputs(text, f) >= 0);
    fflush(f);
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    if (rename(tmp, path) != 0) {          // LittleFS: overwrites -> done. FATFS: fails if dest exists...
        remove(path);                      // ...so clear the old file and retry (SD mirror path).
        if (rename(tmp, path) != 0) { remove(tmp); return false; }
    }
    return true;
}

// Create every directory below the mount root on the way to `path` (e.g. /sd/system and
// /sd/system/config; never the mount itself). Its own non-inlined frame: the persist chain also runs on
// the 6 KB Wi-Fi supervisor task, so this buffer is popped before write_file_atomic's tmp[] is pushed.
__attribute__((noinline)) static bool mkdir_parents(const char *path)
{
    char dir[96];                          // SD config paths are < 48 chars
    if ((size_t)snprintf(dir, sizeof dir, "%s", path) >= sizeof dir) return false;
    char *first = strchr(dir + 1, '/');    // end of the mount-root component
    for (char *p = first ? strchr(first + 1, '/') : NULL; p; p = strchr(p + 1, '/')) {
        *p = '\0'; mkdir(dir, 0775); *p = '/';
    }
    return true;
}

// SD mirror write: create the parent subtree first. Best-effort no-op if no card is mounted.
static bool write_sd_backup(const char *sd_path, const char *text)
{
    return mkdir_parents(sd_path) && write_file_atomic(sd_path, text);
}

static bool nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(SETUP_STORE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// Read an NVS string. Returns a malloc'd, NUL-terminated buffer (caller frees) or NULL if absent.
static char *nvs_read_str(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(SETUP_STORE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t sz = 0;
    if (nvs_get_str(h, key, NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return NULL; }
    char *buf = malloc(sz);
    if (!buf) { nvs_close(h); return NULL; }
    esp_err_t e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    if (e != ESP_OK) { free(buf); return NULL; }
    return buf;
}

// Persistence health of the most recent persist, surfaced via nucleo_setup_persist_status() for
// /api/diag so "settings not saved" reports are instantly triageable.
static bool s_cfg_ok = false, s_nvs_ok = false, s_sd_ok = false;
static int  s_tiers_ok = -1;   // -1 until the first save

// Factory-reset latch + in-flight counter. A persist announces itself (inflight++) BEFORE it reads the
// seal; the seal is raised BEFORE it reads inflight. With sequentially-consistent atomics one of the two
// always sees the other: either the persist sees the seal and backs out, or the seal sees the persist
// and waits for it. Costs one int + one bool of .bss.
static atomic_bool s_sealed;
static atomic_int  s_inflight;

int setup_store_persist(const char *cfg_path, const char *sd_path, const char *nvs_key, const char *text)
{
    atomic_fetch_add(&s_inflight, 1);
    if (atomic_load(&s_sealed)) {
        atomic_fetch_sub(&s_inflight, 1);
        ESP_LOGW(TAG, "save of '%s' refused: config store sealed by a reset (reboot pending)", nvs_key);
        return 0;
    }
    bool cfg = write_file_atomic(cfg_path, text);     // 1. /cfg LittleFS (power-loss-safe, SD-independent)
    bool nvs = nvs_write_str(nvs_key, text);          // 2. NVS (always present — the guaranteed fallback)
    bool sd  = write_sd_backup(sd_path, text);        // 3. SD mirror (survives a flash wipe)
    s_cfg_ok = cfg; s_nvs_ok = nvs; s_sd_ok = sd;
    s_tiers_ok = (cfg ? 1 : 0) + (nvs ? 1 : 0) + (sd ? 1 : 0);
    atomic_fetch_sub(&s_inflight, 1);
    return s_tiers_ok;
}

char *setup_store_load(const char *cfg_path, const char *sd_path, const char *nvs_key, bool *from_fallback)
{
    if (from_fallback) *from_fallback = false;
    char *txt = setup_store_slurp(cfg_path);
    if (txt) return txt;
    txt = nvs_read_str(nvs_key);
    if (txt) { if (from_fallback) *from_fallback = true; return txt; }
    txt = setup_store_slurp(sd_path);
    if (txt) { if (from_fallback) *from_fallback = true; return txt; }
    return NULL;
}

void setup_store_status(bool *cfg_ok, bool *nvs_ok, bool *sd_ok, int *tiers_ok)
{
    if (cfg_ok) *cfg_ok = s_cfg_ok;
    if (nvs_ok) *nvs_ok = s_nvs_ok;
    if (sd_ok)  *sd_ok  = s_sd_ok;
    if (tiers_ok) *tiers_ok = s_tiers_ok;
}

bool setup_store_seal(void)
{
    atomic_store(&s_sealed, true);
    // A persist that got past the seal check before we raised it is still writing: let it land now, so
    // the caller's erase runs strictly after it. Bounded (~2 s) — three small writes take milliseconds.
    for (int i = 0; i < 200 && atomic_load(&s_inflight) > 0; i++) STORE_WAIT_TICK();
    if (atomic_load(&s_inflight) == 0) return true;
    ESP_LOGE(TAG, "config store sealed with a save still in flight: it may land after the erase");
    return false;
}

bool setup_store_sealed(void) { return atomic_load(&s_sealed); }

// Absent only when the filesystem SAYS so (no entry / no such directory — an unmounted store answers the
// same). Any other failure (EMFILE, an I/O error) is not proof of erasure.
static bool file_absent(const char *path)
{
    struct stat st;
    return stat(path, &st) != 0 && (errno == ENOENT || errno == ENOTDIR);
}

// Remove a file and its write_file_atomic() temp. true when neither exists afterwards.
static bool remove_with_tmp(const char *path)
{
    char tmp[160];
    bool tmp_ok = (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) < sizeof tmp;
    remove(path);
    if (tmp_ok) remove(tmp);
    return file_absent(path) && (!tmp_ok || file_absent(tmp));
}

bool setup_store_erase(const char *cfg_path, const char *sd_path, const char *nvs_key)
{
    bool cfg = remove_with_tmp(cfg_path);
    bool sd  = remove_with_tmp(sd_path);
    bool nvs;
    nvs_handle_t h;
    esp_err_t oe = nvs_open(SETUP_STORE_NVS_NS, NVS_READWRITE, &h);
    if (oe == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, nvs_key);
        if (e == ESP_OK) e = nvs_commit(h);
        else if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "erase NVS '%s' failed (0x%x)", nvs_key, (unsigned)e);
        size_t sz = 0;                                 // verify: the loader must find nothing in NVS either
        nvs = (nvs_get_str(h, nvs_key, NULL, &sz) == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(h);
    } else {
        // No namespace (never written / no room to create it) holds nothing. NVS not initialised this boot
        // is the one case where the old copy may still sit in flash: report it, don't claim success.
        nvs = (oe != ESP_ERR_NVS_NOT_INITIALIZED);
    }
    if (!(cfg && sd && nvs))
        ESP_LOGE(TAG, "reset: '%s' still present (cfg=%d nvs=%d sd=%d erased)", nvs_key, cfg, nvs, sd);
    return cfg && sd && nvs;
}
