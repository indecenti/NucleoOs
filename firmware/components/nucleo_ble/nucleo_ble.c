// nucleo_ble — see nucleo_ble.h. NimBLE controller+host brought up/down on demand so a BLE app can
// own the radio inside exclusive mode (Wi-Fi down) and give the RAM back on exit.
//
// FOR AUTHORIZED TESTING ONLY. The spam payloads below are transcriptions of the PUBLIC advertising
// formats (Apple Continuity, Microsoft Swift Pair, Google Fast Pair) that the respective OSes turn
// into pairing popups — the same well-documented byte layouts security tools (Flipper/Bruce) use.
#include "nucleo_ble.h"
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nucleo_ble_adv.h"
#include "nucleo_ble_hid.h"

void ble_store_config_init(void);   // NimBLE bonding store (NVS) — provided by the store config component

static const char *TAG = "nucleo_ble";

// ---- RAM reclaim: Bluetooth OFF by default, opt-in + reboot ---------------------------------------
// The BLE controller reserves DRAM even idle. Since BLE is a rarely-used Security tool, we release that
// memory at boot (back to the OS/ANIMA heap) unless the user persisted Bluetooth = ON.
#define BLE_NVS_NS  "ble"
#define BLE_NVS_KEY "on"
static bool s_radio_released = false;

bool nucleo_ble_pref_enabled(void)
{
    nvs_handle_t h; uint8_t on = 0;
    if (nvs_open(BLE_NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, BLE_NVS_KEY, &on); nvs_close(h); }
    return on != 0;
}
void nucleo_ble_set_pref(bool on)
{
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, BLE_NVS_KEY, on ? 1 : 0); nvs_commit(h); nvs_close(h); }
}
void nucleo_ble_boot_reclaim(void)
{
    if (nucleo_ble_pref_enabled()) { ESP_LOGI(TAG, "Bluetooth ON (pref): keeping BLE controller memory"); return; }
    esp_err_t r = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // ~tens of KB back to the heap (irreversible until reboot)
    s_radio_released = (r == ESP_OK);
    ESP_LOGW(TAG, "Bluetooth OFF (saving RAM): BLE mem_release=%s — enable it in the BLE app + reboot to use BLE",
             esp_err_to_name(r));
}
bool nucleo_ble_radio_present(void) { return !s_radio_released; }

typedef enum { MODE_IDLE, MODE_SCAN, MODE_SPAM, MODE_IBEACON, MODE_HID } ble_mode_t;

static volatile bool s_up = false;
static volatile bool s_synced = false;
static volatile ble_mode_t s_mode = MODE_IDLE;
static nucleo_ble_spam_t s_spam_target = NUCLEO_BLE_SPAM_IOS;
static uint32_t s_spam_count = 0;
static uint32_t s_rot = 0;
static esp_timer_handle_t s_spam_timer = NULL;
static uint8_t s_own_addr = BLE_OWN_ADDR_PUBLIC;   // resolved in on_sync via ble_hs_id_infer_auto

// ---- scan table ------------------------------------------------------------------------------------
// Written from the NimBLE host task (GAP callback), read from the launcher task — spinlock-guarded.
#define BLE_MAX_DEV 24
typedef struct { uint8_t addr[6]; int8_t rssi; char name[24]; bool used; } ble_dev_t;
static ble_dev_t s_dev[BLE_MAX_DEV];
static int s_dev_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void dev_upsert(const uint8_t *addr, int8_t rssi, const char *name, int nlen)
{
    portENTER_CRITICAL(&s_mux);
    int slot = -1, freeslot = -1;
    for (int i = 0; i < BLE_MAX_DEV; i++) {
        if (s_dev[i].used) { if (!memcmp(s_dev[i].addr, addr, 6)) { slot = i; break; } }
        else if (freeslot < 0) freeslot = i;
    }
    if (slot < 0) slot = freeslot;
    if (slot >= 0) {
        if (!s_dev[slot].used) { s_dev[slot].used = true; memcpy(s_dev[slot].addr, addr, 6); s_dev[slot].name[0] = 0; s_dev_count++; }
        s_dev[slot].rssi = rssi;
        if (name && nlen > 0) {
            int n = nlen; if (n > (int)sizeof(s_dev[slot].name) - 1) n = sizeof(s_dev[slot].name) - 1;
            memcpy(s_dev[slot].name, name, n); s_dev[slot].name[n] = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static void scan_table_clear(void) { portENTER_CRITICAL(&s_mux); memset(s_dev, 0, sizeof(s_dev)); s_dev_count = 0; portEXIT_CRITICAL(&s_mux); }

int nucleo_ble_scan_count(void) { portENTER_CRITICAL(&s_mux); int c = s_dev_count; portEXIT_CRITICAL(&s_mux); return c; }

bool nucleo_ble_scan_get(int idx, nucleo_ble_dev_t *out)
{
    bool ok = false; int seen = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BLE_MAX_DEV; i++) {
        if (!s_dev[i].used) continue;
        if (seen == idx) {
            memcpy(out->addr, s_dev[i].addr, 6); out->rssi = s_dev[i].rssi;
            memcpy(out->name, s_dev[i].name, sizeof(out->name)); ok = true; break;
        }
        seen++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

// ---- GAP events: scan results (DISC) + the HID keyboard connection lifecycle --------------------
// Spam/iBeacon pass a NULL callback (they never keep a link). HID advertising passes this handler so
// connections are tracked here and the conn-handle/subscription state flows into the HID module.
static void start_hid(void);   // fwd

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *disc = &event->disc;
        char name[24]; int nlen = 0; name[0] = 0;
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 &&
            fields.name != NULL && fields.name_len > 0) {
            nlen = fields.name_len; if (nlen > 23) nlen = 23;
            memcpy(name, fields.name, nlen); name[nlen] = 0;
        }
        dev_upsert(disc->addr.val, disc->rssi, name, nlen);
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->connect.status != 0) {                 // connect failed
            if (s_mode == MODE_HID && nucleo_ble_hidsvc_adv_wanted()) start_hid();
            return 0;
        }
        if (s_mode == MODE_HID)
            nucleo_ble_hidsvc_set_conn(event->connect.conn_handle);
        else
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);  // stray link (a spam
                                                          // popup was tapped) -> drop it so the conn slot frees
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        nucleo_ble_hidsvc_set_conn(BLE_HS_CONN_HANDLE_NONE);
        if (nucleo_ble_hidsvc_adv_wanted())
            start_hid();                                  // peer left -> allow reconnect
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        nucleo_ble_hidsvc_set_subscribed_for(event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;                    // already bonded; drop the stale bond and retry
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {0};
    p.passive = 1;            // observer only — never connect/probe
    p.filter_duplicates = 0;  // keep refreshing RSSI
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
}

// ---- spam advertisement builders -------------------------------------------------------------------
// The payload byte-layouts live in the pure, host-testable nucleo_ble_adv.c (gate: ble-check.mjs). Here
// we only bind them to the hardware RNG and the spam-target enum.
static uint8_t esp_rng_byte(void) { return (uint8_t)esp_random(); }

static int build_adv(nucleo_ble_spam_t t, uint32_t rot, uint8_t *b)
{
    switch (t) {
        case NUCLEO_BLE_SPAM_IOS:     return ble_adv_apple(rot, b, esp_rng_byte);
        case NUCLEO_BLE_SPAM_ANDROID: return ble_adv_android(rot, b);
        case NUCLEO_BLE_SPAM_WINDOWS: return ble_adv_swiftpair(rot, b);
        case NUCLEO_BLE_SPAM_SAMSUNG: return ble_adv_samsung(rot, b);
        default:                      return 0;
    }
}

// Periodic (~40 ms): fresh random static MAC + next payload -> a new "device" each burst.
static void spam_tick(void *arg)
{
    if (s_mode != MODE_SPAM || !s_synced) return;
    ble_gap_adv_stop();

    nucleo_ble_spam_t t = s_spam_target;
    if (t == NUCLEO_BLE_SPAM_ALL) t = (nucleo_ble_spam_t)(s_rot & 0x03);   // cycle IOS/ANDROID/WINDOWS/SAMSUNG

    uint8_t mac[6]; esp_fill_random(mac, 6); mac[5] |= 0xC0;               // static random address (top bits 11)
    ble_hs_id_set_rnd(mac);

    uint8_t adv[31]; int len = build_adv(t, s_rot, adv);
    if (len > 0 && ble_gap_adv_set_data(adv, len) == 0) {
        struct ble_gap_adv_params p = {0};
        p.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable: needed for the popups to fire
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        // Pass gap_event (not NULL) so a tapped spam popup forms a tracked link that gap_event
        // terminates immediately (s_mode != MODE_HID) — otherwise stray connections pile up against
        // the connection cap and silently stall the spam.
        if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &p, gap_event, NULL) == 0)
            s_spam_count++;
    }
    s_rot++;
}

static void ensure_spam_timer(void)
{
    if (s_spam_timer) return;
    const esp_timer_create_args_t a = { .callback = spam_tick, .name = "ble_spam" };
    esp_timer_create(&a, &s_spam_timer);
}

// ---- iBeacon ---------------------------------------------------------------------------------------
static void start_ibeacon(void)
{
    uint8_t b[BLE_ADV_MAX]; int n = ble_adv_ibeacon(b);
    ble_gap_adv_stop();
    if (ble_gap_adv_set_data(b, n) == 0) {
        struct ble_gap_adv_params p = {0};
        p.conn_mode = BLE_GAP_CONN_MODE_NON;   // non-connectable: no stray links possible
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        ble_gap_adv_start(s_own_addr, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    }
}

// ---- HID keyboard --------------------------------------------------------------------------------
static void start_hid(void) { nucleo_ble_hidsvc_start_adv(s_own_addr, gap_event); }

// ---- mode switching --------------------------------------------------------------------------------
static void stop_current(void)
{
    if (s_mode == MODE_SCAN && s_synced) ble_gap_disc_cancel();
    if (s_mode == MODE_SPAM) { if (s_spam_timer) esp_timer_stop(s_spam_timer); if (s_synced) ble_gap_adv_stop(); }
    if (s_mode == MODE_IBEACON && s_synced) ble_gap_adv_stop();
    if (s_mode == MODE_HID) nucleo_ble_hidsvc_stop(s_synced);   // stop adv + terminate the HID link
    s_mode = MODE_IDLE;
}

// ---- host lifecycle --------------------------------------------------------------------------------
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr);   // public if provisioned, else the random identity — so
                                            // connectable HID/iBeacon adv doesn't silently fail (EINVAL)
    s_synced = true;
    // resume whatever the app asked for before sync completed
    if (s_mode == MODE_SCAN) start_scan();
    else if (s_mode == MODE_SPAM) { ensure_spam_timer(); esp_timer_start_periodic(s_spam_timer, 40000); }
    else if (s_mode == MODE_IBEACON) start_ibeacon();
    else if (s_mode == MODE_HID) start_hid();
}
static void on_reset(int reason) { s_synced = false; ESP_LOGW(TAG, "host reset, reason=%d", reason); }

static void host_task(void *param)
{
    nimble_port_run();              // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

bool nucleo_ble_up(void)
{
    if (s_up) return true;
    if (s_radio_released) { ESP_LOGW(TAG, "BLE radio memory was released at boot (Bluetooth off) — enable + reboot"); return false; }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed: 0x%x", err); return false; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Security manager for the HID keyboard: Just Works (no display/keypad), bonding + LE Secure
    // Connections — the combination iOS/Android/Windows accept for a HID peripheral.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;
    // Standard peripheral services + our HID/DIS/Battery GATT (re-registered each up(); the DB is
    // freed on deinit). Must run before the host task starts.
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("NucleoOS");
    ble_svc_gap_device_appearance_set(0x03C1);     // Keyboard
    nucleo_ble_hidsvc_gatt_init();
    static bool s_store_init = false;
    if (!s_store_init) { ble_store_config_init(); s_store_init = true; }
    nucleo_ble_hidsvc_reset();
    s_synced = false; s_mode = MODE_IDLE; s_spam_count = 0; s_rot = 0;
    nimble_port_freertos_init(host_task);
    s_up = true;
    return true;
}

bool nucleo_ble_down(void)
{
    if (!s_up) return true;
    stop_current();                         // for HID this terminates the active link
    if (s_spam_timer) { esp_timer_delete(s_spam_timer); s_spam_timer = NULL; }
    int rc = nimble_port_stop();
    for (int i = 0; i < 3 && rc != 0; i++) { vTaskDelay(pdMS_TO_TICKS(20)); rc = nimble_port_stop(); }
    if (rc != 0) {
        // The host wouldn't stop: the host task is still blocked in nimble_port_run() and the stack is
        // still allocated. Do NOT clear s_up — otherwise the next up() would nimble_port_init() a live
        // stack and spawn a SECOND host task (leak/crash). Stay "up" so a later close can retry. Returning
        // false tells the caller NOT to restart Wi-Fi/httpd onto a heap the live NimBLE stack still owns.
        ESP_LOGE(TAG, "nimble_port_stop failed (rc=%d): stack still up, RAM not reclaimed", rc);
        return false;
    }
    nimble_port_deinit();                    // frees controller+host RAM back to the heap
    scan_table_clear();
    nucleo_ble_hidsvc_reset();
    s_up = false; s_synced = false; s_mode = MODE_IDLE;
    return true;
}

bool nucleo_ble_is_up(void)     { return s_up; }
bool nucleo_ble_is_synced(void) { return s_synced; }

void nucleo_ble_scan_start(void) { if (!s_up) return; stop_current(); scan_table_clear(); s_mode = MODE_SCAN; if (s_synced) start_scan(); }
void nucleo_ble_scan_stop(void)  { if (s_mode == MODE_SCAN) stop_current(); }

void nucleo_ble_spam_start(nucleo_ble_spam_t target)
{
    if (!s_up) return;
    stop_current();
    s_spam_target = target; s_spam_count = 0; s_rot = 0; s_mode = MODE_SPAM;
    if (s_synced) { ensure_spam_timer(); esp_timer_start_periodic(s_spam_timer, 40000); }
}
void     nucleo_ble_spam_stop(void)   { if (s_mode == MODE_SPAM) stop_current(); }
bool     nucleo_ble_spam_active(void) { return s_mode == MODE_SPAM; }
uint32_t nucleo_ble_spam_count(void)  { return s_spam_count; }

void nucleo_ble_ibeacon_start(void)  { if (!s_up) return; stop_current(); s_mode = MODE_IBEACON; if (s_synced) start_ibeacon(); }
void nucleo_ble_ibeacon_stop(void)   { if (s_mode == MODE_IBEACON) stop_current(); }
bool nucleo_ble_ibeacon_active(void) { return s_mode == MODE_IBEACON; }

// ---- HID keyboard public API ---------------------------------------------------------------------
void nucleo_ble_hid_start(void)     { if (!s_up) return; stop_current(); s_mode = MODE_HID; if (s_synced) start_hid(); }
void nucleo_ble_hid_stop(void)      { if (s_mode == MODE_HID) stop_current(); }
bool nucleo_ble_hid_active(void)    { return s_mode == MODE_HID; }
bool nucleo_ble_hid_connected(void) { return nucleo_ble_hidsvc_conn() != BLE_HS_CONN_HANDLE_NONE; }
bool nucleo_ble_hid_ready(void)     { return nucleo_ble_hid_connected() && nucleo_ble_hidsvc_subscribed(); }
void nucleo_ble_hid_type(const char *text) { if (s_mode == MODE_HID) nucleo_ble_hidsvc_type(text); }
void nucleo_ble_hid_key(int mod, int keycode)  { if (s_mode == MODE_HID) nucleo_ble_hidsvc_tap((uint8_t)mod, (uint8_t)keycode); }
void nucleo_ble_hid_media(int action)
{
    static const uint8_t bm[6] = { BLE_HID_MEDIA_PLAYPAUSE, BLE_HID_MEDIA_NEXT, BLE_HID_MEDIA_PREV,
                                   BLE_HID_MEDIA_VOLUP, BLE_HID_MEDIA_VOLDN, BLE_HID_MEDIA_MUTE };
    if (s_mode == MODE_HID && action >= 0 && action < 6) nucleo_ble_hidsvc_media(bm[action]);
}
