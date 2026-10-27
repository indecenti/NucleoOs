// Host stubs for the Ethernet (W5500) gallery: a "return 0" auto-stub would leave the engine absent
// (nucleo_eth_present()==false -> the app stays on the NOMODULE screen), so the menu / host / running
// panels would never render. These give the gallery a believable, FIXED snapshot: a present module, an
// identity, a handful of discovered hosts, and a running op's counters. Pure data for the PC render —
// the real engine lives in firmware/components/nucleo_eth. The engine is stateful here (a start_* sets
// the current op), so the scene scripts reach each screen with the keys the real app uses.
#include "nucleo_eth.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static nucleo_eth_op_t s_cur = ETH_OP_NONE;    // NONE at enter() -> the app opens on the consent gate

struct Host { const char *ip; const char *mac; const char *vendor; bool gw; uint32_t raw; };
static const Host HOSTS[] = {
    { "192.168.1.1",   "a0:63:91:00:11:22", "Netgear",    true,  0x0101A8C0 },
    { "192.168.1.10",  "3c:22:fb:aa:bb:cc", "Apple",      false, 0x0A01A8C0 },
    { "192.168.1.23",  "dc:a6:32:12:34:56", "Raspberry",  false, 0x1701A8C0 },
    { "192.168.1.42",  "e4:5f:01:77:88:99", "Espressif",  false, 0x2A01A8C0 },
    { "192.168.1.88",  "b8:27:eb:de:ad:be", "Raspberry",  false, 0x5801A8C0 },
};
#define HOST_N ((int)(sizeof(HOSTS)/sizeof(HOSTS[0])))

extern "C" {
bool  nucleo_eth_begin(void)            { return true; }
void  nucleo_eth_end(void)              {}
bool  nucleo_eth_present(void)          { return true; }
bool  nucleo_eth_link(void)             { return true; }
uint8_t nucleo_eth_chip_version(void)   { return 0x04; }
const char *nucleo_eth_my_mac_str(void) { return "24:6f:28:aa:bb:cc"; }

bool  nucleo_eth_has_identity(void)     { return true; }
const char *nucleo_eth_my_ip_str(void)  { return "192.168.1.57"; }
const char *nucleo_eth_netmask_str(void){ return "255.255.255.0"; }
const char *nucleo_eth_gateway_str(void){ return "192.168.1.1"; }
uint32_t nucleo_eth_my_ip(void)         { return 0x3901A8C0; }
uint32_t nucleo_eth_gateway_ip(void)    { return 0x0101A8C0; }

int  nucleo_eth_scan_start(void)        { s_cur = ETH_OP_SCAN; return 0; }
int  nucleo_eth_mitm_start(uint32_t, uint32_t) { s_cur = ETH_OP_MITM; return 0; }
int  nucleo_eth_poison_start(uint32_t)  { s_cur = ETH_OP_POISON; return 0; }
int  nucleo_eth_dhcp_starve_start(void) { s_cur = ETH_OP_DHCP_STARVE; return 0; }
int  nucleo_eth_macflood_start(void)    { s_cur = ETH_OP_MACFLOOD; return 0; }
int  nucleo_eth_rogue_dhcp_start(void)  { s_cur = ETH_OP_ROGUE_DHCP; return 0; }
int  nucleo_eth_pcap_start(const char *){ s_cur = ETH_OP_PCAP; return 0; }
void nucleo_eth_stop(void)              { s_cur = ETH_OP_NONE; }

nucleo_eth_op_t nucleo_eth_current(void){ return s_cur; }
unsigned nucleo_eth_uptime_s(void)      { return 125; }
unsigned long nucleo_eth_tx_frames(void){ return 48213; }
unsigned long nucleo_eth_rx_frames(void){ return 15902; }
int  nucleo_eth_scan_progress(void)     { return 62; }
unsigned long nucleo_eth_pcap_bytes(void){ return 892 * 1024; }
int  nucleo_eth_rogue_leases(void)      { return 3; }

int  nucleo_eth_host_count(void)        { return HOST_N; }
const char *nucleo_eth_host_ip(int i)   { return (i >= 0 && i < HOST_N) ? HOSTS[i].ip : ""; }
const char *nucleo_eth_host_mac(int i)  { return (i >= 0 && i < HOST_N) ? HOSTS[i].mac : ""; }
const char *nucleo_eth_host_vendor(int i){ return (i >= 0 && i < HOST_N) ? HOSTS[i].vendor : ""; }
bool nucleo_eth_host_is_gateway(int i)  { return (i >= 0 && i < HOST_N) ? HOSTS[i].gw : false; }
uint32_t nucleo_eth_host_ip_raw(int i)  { return (i >= 0 && i < HOST_N) ? HOSTS[i].raw : 0; }
}
