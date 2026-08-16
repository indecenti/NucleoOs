#include "nucleo_usbnet.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "dhcpserver/dhcpserver_options.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "usbnet";

// ── opt-in flag in RTC no-init RAM (survives warm reboot, not cold power-on) ──
#define NET_MAGIC 0x0C0DE7E7u
RTC_NOINIT_ATTR static uint32_t s_arm;
void nucleo_usbnet_web_arm(void)   { s_arm = NET_MAGIC; }
bool nucleo_usbnet_web_pending(void) { if (s_arm == NET_MAGIC) { s_arm = 0; return true; } return false; }
// Non-consuming peek: lets main() decide EARLY (before Wi-Fi bringup) whether this is a USB-web boot,
// so it can skip Wi-Fi (~48 KB) — the PC reaches the web OS over the cable, Wi-Fi is dead weight here.
bool nucleo_usbnet_web_armed(void) { return s_arm == NET_MAGIC; }

static bool s_active = false;
bool nucleo_usbnet_is_active(void) { return s_active; }

static bool s_failed = false;
bool nucleo_usbnet_failed(void) { return s_failed; }

static esp_netif_t *s_netif = NULL;

// ── explicit NCM-only USB descriptor ─────────────────────────────────────────
// MSC stays enabled in Kconfig (for the SD-drive mode), so the DEFAULT descriptor would fold in a
// phantom drive here. We spell out an NCM-only device instead (same trick nucleo_usbmsc uses).
// The MAC string (index 5) is the 12-hex network address NCM requires; we fill it from the real MAC.
enum { ITF_NET = 0, ITF_NET_DATA, ITF_TOTAL };
#define EPNUM_NET_NOTIF 0x81
#define EPNUM_NET_OUT   0x02
#define EPNUM_NET_IN    0x82
static const uint8_t s_net_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NET, 4, 5, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN, 64, CFG_TUD_NET_MTU),
};
static char s_mac_str[13];                 // 12 uppercase hex + NUL
static const char *s_net_str[6];           // 0 lang,1 mfr,2 product,3 serial,4 iface,5 MAC

// The USB "client" MAC (host side) and the device-lwip MAC must differ (two ends of a virtual link).
static const uint8_t s_usb_mac[6]  = { 0x02, 0x02, 0x11, 0x22, 0x33, 0x01 };
static const uint8_t s_lwip_mac[6] = { 0x02, 0x02, 0x11, 0x22, 0x33, 0x02 };

// ── esp_netif <-> tinyusb_net glue (mirrors examples/network/sta2eth) ─────────
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    return ESP_OK;                          // best-effort; a dropped frame is retransmitted by TCP
}
static void netif_free_rx(void *h, void *buffer) { (void)h; free(buffer); }
static esp_err_t usb_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif) return ESP_OK;
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_netif, copy, len, NULL);
}

void nucleo_usbnet_start(void)
{
    // Heap snapshot BEFORE any bring-up. This log reaches the JTAG serial console (USB is still in
    // JTAG mode here; tinyusb_driver_install below is what switches the port to OTG). Bringing up
    // TinyUSB + esp_netif + a DHCP server needs a big contiguous block; on this no-PSRAM chip a
    // starved server-Solo heap is the prime suspect for the crash, so guard it instead of aborting.
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t large = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "usbnet bring-up: free=%u largest=%u", (unsigned)freeb, (unsigned)large);
    // On this no-PSRAM chip the largest contiguous block after httpd is ~15 KB. NCM bring-up does NOT
    // need one huge block — its biggest single alloc is the ~4 KB USB task stack, the rest are small
    // (netif structs, DHCP, ~1.5 KB MTU buffers). So the floor only rejects a truly starved heap that
    // can't even fit the USB task; anything above attempts, and a crash (if any) is caught by coredump
    // and recovered by the next clean boot.
    if (large < 6000 || freeb < 16000) {
        ESP_LOGE(TAG, "usbnet: heap too low (free=%u largest=%u), skipping (no crash)", (unsigned)freeb, (unsigned)large);
        s_failed = true;
        return;
    }

    // MAC string for the NCM descriptor (12 uppercase hex of the client MAC).
    snprintf(s_mac_str, sizeof s_mac_str, "%02X%02X%02X%02X%02X%02X",
             s_usb_mac[0], s_usb_mac[1], s_usb_mac[2], s_usb_mac[3], s_usb_mac[4], s_usb_mac[5]);
    s_net_str[0] = (const char[]){ 0x09, 0x04 };   // en-US
    s_net_str[1] = "NucleoOS";
    s_net_str[2] = "NucleoOS Web (USB)";
    s_net_str[3] = "000001";
    s_net_str[4] = "NucleoOS NCM";
    s_net_str[5] = s_mac_str;

    const tinyusb_config_t tcfg = {
        .device_descriptor = NULL,
        .string_descriptor = s_net_str,
        .string_descriptor_count = sizeof(s_net_str) / sizeof(s_net_str[0]),
        .external_phy = false,
        .configuration_descriptor = s_net_cfg,
    };
    ESP_LOGW(TAG, "usbnet: installing tinyusb driver...");
    if (tinyusb_driver_install(&tcfg) != ESP_OK) { ESP_LOGE(TAG, "tinyusb install failed"); s_failed = true; return; }

    ESP_LOGW(TAG, "usbnet: tinyusb_net_init...");
    tinyusb_net_config_t net_cfg = { .on_recv_callback = usb_recv_cb };
    memcpy(net_cfg.mac_addr, s_usb_mac, 6);
    if (tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg) != ESP_OK) { ESP_LOGE(TAG, "tinyusb_net init failed"); s_failed = true; return; }
    ESP_LOGW(TAG, "usbnet: net_init ok, creating esp_netif...");
    // Ensure the TCP/IP stack + default event loop exist. When Wi-Fi is up these are already done;
    // in USB-web mode we SKIP Wi-Fi (to reclaim ~48 KB), so usbnet must bring them up itself. Both are
    // safe to call again: esp_netif_init() is idempotent, and a second event-loop create returns
    // ESP_ERR_INVALID_STATE (harmless — it just means Wi-Fi already made it).
    esp_netif_init();
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(le)); s_failed = true; return; }

    // A static /24 with a DHCP server — the PC gets 192.168.7.x and reaches http://192.168.7.1.
    static const esp_netif_ip_info_t ip = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
    };
    esp_netif_inherent_config_t base = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ip,
        .if_key = "usbnet",
        .if_desc = "usb ncm",
        .route_prio = 15,                    // above WiFi so the USB link wins when both are up
    };
    esp_netif_driver_ifconfig_t drv = {
        .handle = (void *)1,                 // static singleton (must be non-NULL)
        .transmit = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx,
    };
    esp_netif_config_t cfg = { .base = &base, .driver = &drv, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) { ESP_LOGE(TAG, "esp_netif_new failed"); s_failed = true; return; }
    esp_netif_set_mac(s_netif, (uint8_t *)s_lwip_mac);

    uint32_t lease = 1;                       // minimum lease time (minutes)
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease, sizeof lease);
    esp_netif_action_start(s_netif, NULL, 0, NULL);   // driver already up -> start the netif + DHCP server

    s_active = true;
    ESP_LOGI(TAG, "USB-net up: NCM device, DHCP server, http://192.168.7.1");
}
