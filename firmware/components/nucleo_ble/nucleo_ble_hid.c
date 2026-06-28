// nucleo_ble_hid — see nucleo_ble_hid.h. Standard BLE HID keyboard + consumer-control peripheral over
// NimBLE GATT. FOR AUTHORIZED TESTING ONLY. The report-map bytes and the ASCII->keycode table are the
// pure, host-tested data in nucleo_ble_adv.c; this file is the GATT service + notification plumbing.
#include "nucleo_ble_hid.h"
#include "nucleo_ble_adv.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "nucleo_ble_hid";

// ---- served attribute values ----------------------------------------------------------------------
static const uint8_t hid_info[4]  = { 0x11, 0x01, 0x00, 0x03 };  // bcdHID 1.11, country 0, RemoteWake|NormallyConnectable
static const uint8_t pnp_id[7]    = { 0x02, 0xE5, 0x02, 0xA1, 0x00, 0x01, 0x00 };  // src=USB, vid 0x02E5, pid 0x00A1, ver 0x0001
static const char    manuf_name[] = "NucleoOS";
static uint8_t protocol_mode = 0x01;   // report protocol
static uint8_t battery_level = 100;
static uint8_t kbd_report[8];          // last keyboard input report (also served on READ)
static uint8_t consumer_report[1];     // last consumer input report
static uint8_t led_state = 0;          // host-written LED bitmap

// Report Reference descriptor payloads: {Report ID, Report Type(1=Input,2=Output)}
static const uint8_t rr_kbd_in[2]      = { 0x01, 0x01 };
static const uint8_t rr_kbd_out[2]     = { 0x01, 0x02 };
static const uint8_t rr_consumer_in[2] = { 0x02, 0x01 };

// value handles filled at registration
static uint16_t s_kbd_in_h, s_kbd_out_h, s_consumer_in_h, s_batt_h;

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_subscribed = false;
static volatile bool     s_adv_wanted = false;

// ---- GATT access callback -------------------------------------------------------------------------
static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
        if (ble_uuid_u16(ctxt->dsc->uuid) == 0x2908 && arg)   // Report Reference
            return os_mbuf_append(ctxt->om, arg, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const void *p = NULL; int n = 0;
        switch (u) {
            case 0x2A4B: p = BLE_HID_REPORT_MAP; n = BLE_HID_REPORT_MAP_N; break;
            case 0x2A4A: p = hid_info;  n = sizeof hid_info; break;
            case 0x2A4E: p = &protocol_mode; n = 1; break;
            case 0x2A50: p = pnp_id;    n = sizeof pnp_id; break;
            case 0x2A29: p = manuf_name; n = (int)strlen(manuf_name); break;
            case 0x2A19: p = &battery_level; n = 1; break;
            case 0x2A4D:                                   // a report — disambiguate by value handle
                if (attr == s_kbd_in_h)        { p = kbd_report;      n = 8; }
                else if (attr == s_consumer_in_h) { p = consumer_report; n = 1; }
                else                           { p = &led_state;     n = 1; }
                break;
            default: return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, p, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t got = 0;
        switch (u) {
            case 0x2A4C: return 0;                                              // HID control point: accept+ignore
            case 0x2A4E: ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode, 1, &got); return 0;
            case 0x2A4D: ble_hs_mbuf_to_flat(ctxt->om, &led_state, 1, &got); return 0;  // LED output
            default: return BLE_ATT_ERR_UNLIKELY;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// ---- GATT service table ---------------------------------------------------------------------------
static const struct ble_gatt_svc_def hid_svcs[] = {
    { // HID Service 0x1812
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ },        // Report Map
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ },        // HID Information
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },// HID Control Point
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access,                                        // Protocol Mode
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access,                                        // Keyboard Input Report
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_kbd_in_h,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = hid_access, .arg = (void *)rr_kbd_in }, { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access,                                        // Keyboard Output Report (LED)
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &s_kbd_out_h,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = hid_access, .arg = (void *)rr_kbd_out }, { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access,                                        // Consumer Input Report
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_consumer_in_h,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = hid_access, .arg = (void *)rr_consumer_in }, { 0 } } },
            { 0 }
        }
    },
    { // Device Information Service 0x180A
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ },   // PnP ID
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ },   // Manufacturer
            { 0 }
        }
    },
    { // Battery Service 0x180F
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_h },               // Battery Level
            { 0 }
        }
    },
    { 0 }
};

int nucleo_ble_hidsvc_gatt_init(void)
{
    int rc = ble_gatts_count_cfg(hid_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return rc; }
    rc = ble_gatts_add_svcs(hid_svcs);
    if (rc != 0) ESP_LOGE(TAG, "add_svcs rc=%d", rc);
    return rc;
}

// ---- advertising ----------------------------------------------------------------------------------
void nucleo_ble_hidsvc_start_adv(uint8_t own_addr_type, nucleo_ble_gap_cb cb)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name; fields.name_len = (uint8_t)strlen(name); fields.name_is_complete = 1;
    fields.appearance = 0x03C1; fields.appearance_is_present = 1;        // Keyboard
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0x1812) };
    fields.num_uuids16 = 1; fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    s_adv_wanted = true;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "adv_start rc=%d", rc);
}

void nucleo_ble_hidsvc_stop(bool synced)
{
    s_adv_wanted = false;                  // so a DISCONNECT during teardown does NOT re-advertise
    if (synced) {
        ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);   // mode-switch: drop the HID link
    }
    s_subscribed = false;
    // No busy-wait: on app close, nucleo_ble_down()'s nimble_port_stop() -> ble_hs_stop() terminates
    // any remaining link and waits internally before the stack is freed; on a mode-switch the host
    // keeps running and processes the disconnect asynchronously.
}

// ---- state driven by nucleo_ble.c's GAP handler ---------------------------------------------------
void     nucleo_ble_hidsvc_set_conn(uint16_t conn) { s_conn = conn; if (conn == BLE_HS_CONN_HANDLE_NONE) s_subscribed = false; }
uint16_t nucleo_ble_hidsvc_conn(void)              { return s_conn; }
bool     nucleo_ble_hidsvc_subscribed(void)        { return s_subscribed; }
bool     nucleo_ble_hidsvc_adv_wanted(void)        { return s_adv_wanted; }
void     nucleo_ble_hidsvc_set_subscribed_for(uint16_t attr_handle, bool on) { if (attr_handle == s_kbd_in_h) s_subscribed = on; }
void     nucleo_ble_hidsvc_reset(void) { s_conn = BLE_HS_CONN_HANDLE_NONE; s_subscribed = false; s_adv_wanted = false; led_state = 0; protocol_mode = 0x01; }

// ---- senders --------------------------------------------------------------------------------------
static void notify(uint16_t handle, const uint8_t *data, int len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om) ble_gatts_notify_custom(s_conn, handle, om);
}

void nucleo_ble_hidsvc_tap(uint8_t mod, uint8_t key)
{
    kbd_report[0] = mod; kbd_report[1] = 0; kbd_report[2] = key;
    memset(&kbd_report[3], 0, 5);
    notify(s_kbd_in_h, kbd_report, 8);
    vTaskDelay(pdMS_TO_TICKS(8));
    memset(kbd_report, 0, 8);
    notify(s_kbd_in_h, kbd_report, 8);              // release
}

void nucleo_ble_hidsvc_type(const char *text)
{
    if (!text || s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    for (const char *c = text; *c; c++) {
        ble_hid_keystroke_t k;
        if (!ble_hid_ascii(*c, &k)) continue;       // skip non-typeable chars
        nucleo_ble_hidsvc_tap(k.mod, k.key);
        vTaskDelay(pdMS_TO_TICKS(6));
    }
}

void nucleo_ble_hidsvc_media(uint8_t bitmap)
{
    consumer_report[0] = bitmap;
    notify(s_consumer_in_h, consumer_report, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    consumer_report[0] = 0;
    notify(s_consumer_in_h, consumer_report, 1);    // release
}
