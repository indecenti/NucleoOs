// nucleo_eth — wired (Ethernet/W5500) Layer-2/3 offensive engine for NucleoOS.
//
// Clean-room, MIT: built on our own MACRAW driver (nucleo_w5500) + pure frame craft (eth_frames),
// NOT on Bruce/lwIP source. It does, better, what Bruce's "Ethernet" menu does — host scan, ARP
// spoof/poison, DHCP starvation, MAC flooding — plus things Bruce doesn't: a hand-rolled DHCP client
// to self-configure with no TCP/IP stack, OUI vendor fingerprinting, a rogue-DHCP MITM, on-SD PCAP
// capture, and REVERSIBLE attacks that heal the ARP caches they poisoned on exit.
//
// AUTHORIZED USE ONLY. Host scanning and especially ARP/DHCP attacks disrupt real networks and are
// illegal without permission. The app UI gates every active attack behind a consent screen. Scanning
// and capture are passive-ish; spoof/poison/starve/flood are loud and the UI says so.
//
// DISCIPLINE (enforced here): exactly ONE operation runs at a time. Starting an op stops and frees the
// previous one first (heal-then-free), so the Cardputer is never driving two attack loops at once.
// The owning app declares NX_NET_APP so the OS reclaims ~70KB for the duration.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ETH_OP_NONE = 0,
    ETH_OP_SCAN,        // ARP sweep of the subnet -> host list (+OUI fingerprint)
    ETH_OP_MITM,        // bidirectional ARP spoof victim<->gateway (reversible)
    ETH_OP_POISON,      // broadcast random-MAC ARP for the gateway IP (network chaos)
    ETH_OP_DHCP_STARVE, // exhaust the DHCP pool with random-chaddr DISCOVERs
    ETH_OP_MACFLOOD,    // flood random source MACs to overflow the switch CAM table
    ETH_OP_ROGUE_DHCP,  // answer DISCOVERs as a rogue server -> redirect router/DNS to us
    ETH_OP_PCAP,        // passive capture to an SD .pcap
} nucleo_eth_op_t;

// ---- lifecycle --------------------------------------------------------------
// Initialise the W5500 (uses the board's default shared-SPI wiring). Returns true only if a module
// answered. nucleo_eth_present() stays false if no W5500 is connected (app shows connect screen).
bool            nucleo_eth_begin(void);
void            nucleo_eth_end(void);           // stop any op (healing first) and release the chip
bool            nucleo_eth_present(void);
bool            nucleo_eth_link(void);          // PHY link up (cable in)
uint8_t         nucleo_eth_chip_version(void);  // VERSIONR (0x04 = genuine W5500)
const char     *nucleo_eth_my_mac_str(void);    // our wired MAC

// ---- L3 identity (from the hand-rolled DHCP bootstrap, or learned passively) -------------------
bool            nucleo_eth_has_identity(void);
const char     *nucleo_eth_my_ip_str(void);
const char     *nucleo_eth_netmask_str(void);
const char     *nucleo_eth_gateway_str(void);
uint32_t        nucleo_eth_my_ip(void);
uint32_t        nucleo_eth_gateway_ip(void);

// ---- operations (single active; each start stops+heals+frees the previous) ----------------------
// All return 0 on success, <0 if the module is absent/busy. Run asynchronously in the worker task.
int             nucleo_eth_scan_start(void);                                   // bootstraps identity if needed, then sweeps
int             nucleo_eth_mitm_start(uint32_t victim_ip, uint32_t gateway_ip);// needs both seen in a prior scan (for healing)
int             nucleo_eth_poison_start(uint32_t gateway_ip);
int             nucleo_eth_dhcp_starve_start(void);
int             nucleo_eth_macflood_start(void);
int             nucleo_eth_rogue_dhcp_start(void);                             // needs identity (our IP = router/DNS)
int             nucleo_eth_pcap_start(const char *sd_path);
void            nucleo_eth_stop(void);                                         // stop current op, heal, free

// ---- live status (polled by the app UI, ~5Hz) ---------------------------------------------------
nucleo_eth_op_t nucleo_eth_current(void);
unsigned        nucleo_eth_uptime_s(void);
unsigned long   nucleo_eth_tx_frames(void);
unsigned long   nucleo_eth_rx_frames(void);
int             nucleo_eth_scan_progress(void);   // 0..100 during ETH_OP_SCAN
unsigned long   nucleo_eth_pcap_bytes(void);      // bytes written during ETH_OP_PCAP
int             nucleo_eth_rogue_leases(void);     // clients we've answered as rogue DHCP

// Discovered hosts (snapshot-safe getters).
int             nucleo_eth_host_count(void);
const char     *nucleo_eth_host_ip(int i);
const char     *nucleo_eth_host_mac(int i);
const char     *nucleo_eth_host_vendor(int i);
bool            nucleo_eth_host_is_gateway(int i);
uint32_t        nucleo_eth_host_ip_raw(int i);

#ifdef __cplusplus
}
#endif
