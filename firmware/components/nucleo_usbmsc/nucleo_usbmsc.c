#include "nucleo_usbmsc.h"
#include "nucleo_storage.h"
#include "nucleo_ui.h"
#include "nucleo_kbd.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

static const char *TAG = "usbmsc";

// Explicit MSC-only descriptor. With the HID class also enabled (USB Keyboard app), the default
// descriptor would fold in a stray keyboard interface, so we spell out a storage-only device.
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81
static const uint8_t s_msc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, 0, 200),
    TUD_MSC_DESCRIPTOR(0, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};
static const char *s_msc_str[] = {
    (const char[]){ 0x09, 0x04 },   // language id (en-US)
    "NucleoOS",                     // manufacturer
    "NucleoOS SD",                  // product
    "000001",                       // serial
};

// Reboot flag in RTC no-init RAM: it survives a software reset (esp_restart) but NOT a cold
// power-on, so a power-cycle always lands in the normal OS — you can never get stuck as a drive.
#define USBMSC_MAGIC 0x05B0DA1Au
RTC_NOINIT_ATTR static uint32_t s_req;

void nucleo_usbmsc_request(void)
{
    s_req = USBMSC_MAGIC;
    esp_restart();
}

bool nucleo_usbmsc_pending(void)
{
    if (s_req == USBMSC_MAGIC) { s_req = 0; return true; }   // consume it
    return false;
}

// Spin until any key, then restart back into the normal OS.
static void wait_then_reboot(void)
{
    for (;;) {
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            tinyusb_msc_storage_deinit();       // best-effort flush/release before the reset
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void nucleo_usbmsc_run(void)
{
    sdmmc_card_t *card = (sdmmc_card_t *)nucleo_storage_card();
    if (!card) {
        const char *err[] = { "No SD card found.", "Reset to exit." };
        nucleo_ui_home("USB Drive", err, 2);
        wait_then_reboot();
    }

    const tinyusb_msc_sdmmc_config_t scfg = { .card = card };
    esp_err_t e = tinyusb_msc_storage_init_sdmmc(&scfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "msc storage init: %s", esp_err_to_name(e));
        const char *err[] = { "USB storage init failed.", "Reset to exit." };
        nucleo_ui_home("USB Drive", err, 2);
        wait_then_reboot();
    }

    const tinyusb_config_t tcfg = {
        .device_descriptor = NULL,              // default device descriptor (VID/PID from Kconfig)
        .string_descriptor = s_msc_str,
        .string_descriptor_count = sizeof(s_msc_str) / sizeof(s_msc_str[0]),
        .external_phy = false,
        .configuration_descriptor = s_msc_cfg,  // explicit storage-only config
    };
    e = tinyusb_driver_install(&tcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install: %s", esp_err_to_name(e));
        const char *err[] = { "USB init failed.", "Reset to exit." };
        nucleo_ui_home("USB Drive", err, 2);
        wait_then_reboot();
    }

    ESP_LOGI(TAG, "USB MSC up: SD exposed to host");
    const char *lines[] = {
        "SD card mounted on PC.",
        "Eject it there first,",
        "then press any key to exit.",
    };
    nucleo_ui_home("USB Drive  *  ON", lines, 3);
    wait_then_reboot();
}
