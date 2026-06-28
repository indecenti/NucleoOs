#include "nucleo_usbhid.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"

static const char *TAG = "usbhid";
static bool s_installed;
static volatile uint8_t s_host_leds = 0;   // last LED bitmap the host pushed (Num/Caps/Scroll) — real PC state

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
    (void)instance; (void)report_id;
    // The host pushes the keyboard LED state here (Num/Caps/Scroll lock). It is the ONLY channel a
    // standard HID keyboard gets back from the PC — capture it so the Cardputer can show real PC state.
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) s_host_leds = buffer[0];
}

uint8_t nucleo_usbhid_leds(void) { return s_host_leds; }

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

// Wait until the HID endpoint can take a report (up to ~240ms). Returns false only if it never frees.
static bool wait_ready(void)
{
    // Feed the task watchdog: this is called from the launcher task (watchdog-watched, 8s). A host that
    // is mounted but stalls its HID poll would otherwise let the wait spin without resetting the WDT.
    for (int i = 0; i < 120 && !tud_hid_ready(); i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(2)); }
    return tud_hid_ready();
}

// Send one report and WAIT for the host to actually poll it (so the next report can't clobber it before
// delivery). This is what makes press+release atomic — the cause of stuck keys is a release report that
// gets dropped while the press is still in flight, after which the host's own auto-repeat takes over.
static void send_report(uint8_t mod, uint8_t keycode)
{
    if (!wait_ready()) return;
    uint8_t kc[6] = { keycode, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(RID_KEYBOARD, mod, keycode ? kc : NULL);
    wait_ready();                                 // block until the host polled this report
}

void nucleo_usbhid_key(unsigned char modifier, unsigned char keycode)
{
    if (!s_installed || !tud_mounted()) return;
    send_report(0, 0);                            // leading clear: drop any key left stuck by a prior drop
    send_report(modifier, keycode);               // press
    send_report(0, 0);                            // release — guaranteed delivered (wait_ready inside)
}

// Send a raw report: modifier bitmap + up to 6 simultaneous keycodes (NULL/0 = release). For chords
// (e.g. Ctrl+Alt+Del is a modifier combo, but multi-letter chords need this).
void nucleo_usbhid_report(unsigned char modifier, const unsigned char *keys, int n)
{
    if (!s_installed || !tud_mounted()) return;
    uint8_t kc[6] = { 0 };
    for (int i = 0; i < n && i < 6; i++) kc[i] = keys[i];
    if (!wait_ready()) return;
    tud_hid_keyboard_report(RID_KEYBOARD, modifier, kc);
    wait_ready();
    send_report(0, 0);                            // release
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
