#include "nucleo_usbhid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"

static const char *TAG = "usbhid";
static bool s_installed;

enum { RID_KEYBOARD = 1 };

// HID report descriptor: a single boot-keyboard.
static const uint8_t s_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD))
};

// Configuration descriptor: ONE HID interface (explicit, so enabling MSC elsewhere can't fold a
// stray storage interface into this device). One IN endpoint.
#define EPNUM_HID 0x81
static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_desc), EPNUM_HID, 16, 10),
};

// String descriptors — how the device shows up on the host.
static const char *s_str_desc[] = {
    (const char[]){ 0x09, 0x04 },   // 0: language id (en-US)
    "NucleoOS",                     // 1: manufacturer
    "NucleoOS Keyboard",            // 2: product
    "000001",                       // 3: serial
};

// ASCII -> {shift, HID keycode}. TinyUSB provides the full table via this macro.
static const uint8_t s_ascii2hid[128][2] = { HID_ASCII_TO_KEYCODE };

// ---- TinyUSB HID callbacks (required when the HID class is compiled) ---------
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_desc;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;   // ignore LED state
}

// -----------------------------------------------------------------------------
esp_err_t nucleo_usbhid_start(void)
{
    if (s_installed) return ESP_OK;
    const tinyusb_config_t cfg = {
        .device_descriptor = NULL,                  // default device descriptor (VID/PID from Kconfig)
        .string_descriptor = s_str_desc,
        .string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = s_cfg_desc,
    };
    esp_err_t e = tinyusb_driver_install(&cfg);
    if (e == ESP_OK) { s_installed = true; ESP_LOGI(TAG, "USB HID keyboard up"); }
    else ESP_LOGE(TAG, "tinyusb install: %s", esp_err_to_name(e));
    return e;
}

bool nucleo_usbhid_ready(void)
{
    return s_installed && tud_mounted() && tud_hid_ready();
}

// Wait (briefly) until the HID endpoint can take a report.
static bool wait_ready(void)
{
    for (int i = 0; i < 25 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    return tud_hid_ready();
}

void nucleo_usbhid_key(unsigned char modifier, unsigned char keycode)
{
    if (!s_installed || !tud_mounted()) return;
    if (!wait_ready()) return;
    uint8_t kc[6] = { keycode, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(RID_KEYBOARD, modifier, kc);    // press

    // The release report MUST get through. If it doesn't, the host keeps the key
    // "held" and its own OS auto-repeat takes over (press 'h' once -> "hhhhhhh").
    // wait_ready() lets the host poll the press first; then we keep retrying the
    // release until the endpoint accepts it rather than dropping it on a timeout.
    wait_ready();
    for (int i = 0; i < 50; i++) {
        if (tud_hid_ready()) { tud_hid_keyboard_report(RID_KEYBOARD, 0, NULL); return; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool nucleo_usbhid_ascii(char c, unsigned char *modifier, unsigned char *keycode)
{
    unsigned char u = (unsigned char)c;
    if (u >= 128) return false;
    uint8_t code = s_ascii2hid[u][1];
    if (!code) return false;
    *keycode = code;
    *modifier = s_ascii2hid[u][0] ? NK_HIDMOD_SHIFT : 0;
    return true;
}
