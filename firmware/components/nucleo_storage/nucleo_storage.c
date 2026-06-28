#include "nucleo_storage.h"
#include "nucleo_board.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include "esp_log.h"
#include "esp_app_desc.h"   // esp_app_get_description(): stamp the real firmware version into volume.json
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "ff.h"
#include "esp_littlefs.h"

static const char *TAG = "storage";
static sdmmc_card_t *s_card;
static nucleo_storage_info_t s_info;

#define VOLUME_JSON NUCLEO_SD_MOUNT "/system/volume.json"

esp_err_t nucleo_storage_mount_cfg(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = NUCLEO_CFG_MOUNT,
        .partition_label = NUCLEO_CFG_LABEL,
        .format_if_mount_failed = true,   // blank partition on first boot -> format it
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cfg littlefs mount: %s", esp_err_to_name(err));
        return err;
    }
    // Config subtree (mirrors the SD layout so callers use a familiar /config path).
    if (mkdir(NUCLEO_CFG_MOUNT "/config", 0775) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "mkdir %s/config failed (errno %d)", NUCLEO_CFG_MOUNT, errno);
    ESP_LOGI(TAG, "cfg littlefs mounted at %s", NUCLEO_CFG_MOUNT);
    return ESP_OK;
}

esp_err_t nucleo_storage_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = NUCLEO_SD_SPI_HOST;
    host.max_freq_khz = 15000; // Abbassiamo a 15MHz per stabilità su ESP32-S3

    spi_bus_config_t bus = {
        .mosi_io_num = NUCLEO_SD_PIN_MOSI,
        .miso_io_num = NUCLEO_SD_PIN_MISO,
        .sclk_io_num = NUCLEO_SD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    // INVALID_STATE just means the bus is already initialised (shared) — fine, proceed.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus init: %s", esp_err_to_name(err));
        s_info.mounted = false; s_info.mount_error = err;   // record it so /api/status is honest
        return err;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = NUCLEO_SD_PIN_CS;
    dev.host_id = host.slot;

    // Do NOT auto-format: an unreadable card must surface as an error, not be wiped.
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    
    // Ciclo di retry per dare tempo alla SD di stabilizzarsi
    for (int i = 0; i < 5; i++) {
        err = esp_vfs_fat_sdspi_mount(NUCLEO_SD_MOUNT, &host, &dev, &mcfg, &s_card);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(100)); // Pausa 100ms prima di riprovare
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (no card / unformatted / unsupported FS)", esp_err_to_name(err));
        s_info.mounted = false;
        s_info.mount_error = err;
        return err;
    }
    s_info.mounted = true;
    s_info.mount_error = ESP_OK;
    ESP_LOGI(TAG, "SD mounted at %s", NUCLEO_SD_MOUNT);
    return ESP_OK;
}

esp_err_t nucleo_storage_refresh(void)
{
    if (!s_info.mounted) return ESP_ERR_INVALID_STATE;

    // Throttle: the free-space query (f_getfree) scans the FAT and is slow on large cards.
    // Doing it on every /api/status call blocks the single httpd task and resets browser
    // connections. Cache the result for 30 s — plenty fresh for a status readout.
    static uint32_t s_last_ms = 0;
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_info.total_bytes != 0 && (uint32_t)(now_ms - s_last_ms) < 30000) return ESP_OK;
    s_last_ms = now_ms;

    // Official, drive-agnostic capacity query.
    esp_err_t err = esp_vfs_fat_info(NUCLEO_SD_MOUNT, &s_info.total_bytes, &s_info.free_bytes);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fat_info: %s", esp_err_to_name(err)); return err; }

    // Best-effort filesystem type for display purposes only.
    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK)
        strncpy(s_info.fs_type, fs->fs_type == FS_EXFAT ? "exFAT" : "FAT32", sizeof(s_info.fs_type) - 1);
    else
        strncpy(s_info.fs_type, "?", sizeof(s_info.fs_type) - 1);

    ESP_LOGI(TAG, "%s: %llu MB total, %llu MB free", s_info.fs_type,
             s_info.total_bytes >> 20, s_info.free_bytes >> 20);
    return ESP_OK;
}

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "mkdir %s failed (errno %d)", path, errno);
}

esp_err_t nucleo_storage_provision(void)
{
    if (!s_info.mounted) return ESP_ERR_INVALID_STATE;
    static const char *dirs[] = {
        NUCLEO_SD_MOUNT "/system", NUCLEO_SD_MOUNT "/system/config",
        NUCLEO_SD_MOUNT "/system/web", NUCLEO_SD_MOUNT "/system/log",
        NUCLEO_SD_MOUNT "/system/registry", NUCLEO_SD_MOUNT "/system/sessions",
        NUCLEO_SD_MOUNT "/system/keys", NUCLEO_SD_MOUNT "/system/logs",
        NUCLEO_SD_MOUNT "/journal", NUCLEO_SD_MOUNT "/apps",
        NUCLEO_SD_MOUNT "/data", NUCLEO_SD_MOUNT "/data/shared",
        NUCLEO_SD_MOUNT "/data/imports", NUCLEO_SD_MOUNT "/data/exports",
        NUCLEO_SD_MOUNT "/data/Documents", NUCLEO_SD_MOUNT "/data/Pictures",
        NUCLEO_SD_MOUNT "/data/Music", NUCLEO_SD_MOUNT "/data/Videos",
        NUCLEO_SD_MOUNT "/data/Recordings",
        NUCLEO_SD_MOUNT "/backups", NUCLEO_SD_MOUNT "/www",
        NUCLEO_SD_MOUNT "/www/shell",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) ensure_dir(dirs[i]);

    struct stat st;
    if (stat(VOLUME_JSON, &st) == 0) return ESP_OK;  // already provisioned

    FILE *f = fopen(VOLUME_JSON, "w");
    if (!f) { ESP_LOGE(TAG, "cannot write %s", VOLUME_JSON); return ESP_FAIL; }
    const esp_app_desc_t *appd = esp_app_get_description();
    fprintf(f,
        "{\n  \"fs\": \"%s\",\n  \"label\": \"NUCLEO\",\n  \"total_bytes\": %llu,\n"
        "  \"provisioned_at\": %lld,\n  \"os_version\": \"%s\",\n  \"device_id\": \"nucleo-01\"\n}\n",
        s_info.fs_type, s_info.total_bytes, (long long)time(NULL), appd ? appd->version : "?");
    fclose(f);
    ESP_LOGI(TAG, "provisioned: system tree + %s", VOLUME_JSON);
    return ESP_OK;
}

void nucleo_storage_sync(void)
{
    // Runs in the esp_restart() shutdown path (scheduler still up), so the SPI unmount can talk to
    // the card. FATFS buffers FAT + directory writes; unmounting flushes them. Without this, a
    // reboot right after a file write could leave a truncated file or a stale free-cluster count.
    ESP_LOGW(TAG, "system going down — syncing filesystems");
    if (s_info.mounted && s_card) {
        esp_err_t e = esp_vfs_fat_sdcard_unmount(NUCLEO_SD_MOUNT, s_card);
        ESP_LOGI(TAG, "SD unmounted: %s", esp_err_to_name(e));
        s_info.mounted = false;
        s_card = NULL;
    }
    esp_vfs_littlefs_unregister(NUCLEO_CFG_LABEL);   // flush the power-loss-safe config store too
}

const nucleo_storage_info_t *nucleo_storage_info(void) { return &s_info; }

void *nucleo_storage_card(void) { return s_info.mounted ? s_card : NULL; }
