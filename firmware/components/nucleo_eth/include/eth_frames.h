// eth_frames — pure, host-portable Ethernet/ARP/DHCP/TCP frame craft for the wired offensive engine.
//
// This is the CERTAIN, TESTABLE core of the Ethernet attack app. It has ZERO ESP-IDF / FreeRTOS /
// hardware dependencies: it only reads and writes byte buffers. Everything that can be wrong in a
// pen-test tool (header layout, byte order, checksums, subnet math, MAC randomisation) lives here so
// it can be exercised on the host with gcc (tools/eth-host) before a single frame ever hits a wire.
//
// Design note — why we can do L2 AND L3 attacks with NO TCP/IP stack: a W5500 in MACRAW mode hands us
// raw Ethernet frames and lets us transmit raw Ethernet frames. An attack tool fabricates malicious
// frames anyway, so we hand-build ARP / DHCP (UDP) / TCP-SYN frames here and never link lwIP. That is
// the whole reason NucleoOS's Ethernet suite fits the no-PSRAM Cardputer where a lwIP-based port can't.
//
// AUTHORIZED USE ONLY — see nucleo_eth.h for the scope/consent policy.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- fixed sizes ------------------------------------------------------------
#define ETH_HDR_LEN     14
#define ARP_PAYLOAD_LEN 28
#define ETH_ARP_LEN     (ETH_HDR_LEN + ARP_PAYLOAD_LEN)   // 42
#define ETH_FRAME_MAX   1518                              // 1514 + FCS room; W5500 strips FCS

#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

// DHCP message types (option 53)
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6
#define DHCP_RELEASE    7

// ---- small value helpers ----------------------------------------------------
static inline void eth_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void eth_be32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static inline uint16_t eth_rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t eth_rd32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

extern const uint8_t ETH_BCAST[6];   // ff:ff:ff:ff:ff:ff
extern const uint8_t ETH_ZERO[6];    // 00:00:00:00:00:00

bool eth_mac_eq(const uint8_t a[6], const uint8_t b[6]);
bool eth_mac_is_zero(const uint8_t a[6]);
bool eth_mac_is_bcast(const uint8_t a[6]);
bool eth_mac_is_mcast(const uint8_t a[6]);   // group bit (LSB of first octet) set
// Format "AA:BB:CC:DD:EE:FF" into out (>=18). Returns out.
char *eth_mac_str(const uint8_t mac[6], char *out, size_t n);
// Parse "a.b.c.d" -> host-order uint32. Returns false on malformed input.
bool  eth_ip_parse(const char *s, uint32_t *out);
// Format host-order IPv4 as "a.b.c.d" into out (>=16). Returns out.
char *eth_ip_str(uint32_t ip, char *out, size_t n);

// ---- random source MAC ------------------------------------------------------
// RNG callback returns a 32-bit random word (esp_random on device, a seeded PRNG on host so tests are
// deterministic). Produces a locally-administered, unicast MAC (LAA bit set, multicast bit clear) so
// the address is valid-looking but never a real vendor address. Used by ARP poison + MAC flood.
typedef uint32_t (*eth_rng_fn)(void);
void eth_rand_mac(uint8_t out[6], eth_rng_fn rng);

// ---- ARP --------------------------------------------------------------------
// Build a complete Ethernet+ARP frame into buf (>= ETH_ARP_LEN). Returns the byte length (42).
//   op           : ARP_OP_REQUEST / ARP_OP_REPLY
//   eth_dst      : Ethernet destination MAC (broadcast for a who-has request)
//   src_mac/src_ip : our sender hardware/protocol address
//   tgt_mac/tgt_ip : target hardware/protocol address (tgt_mac ignored/zero for a request)
size_t eth_build_arp(uint8_t *buf,
                     uint16_t op,
                     const uint8_t eth_dst[6],
                     const uint8_t src_mac[6], uint32_t src_ip,
                     const uint8_t tgt_mac[6], uint32_t tgt_ip);

typedef struct {
    uint16_t op;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
    uint8_t  eth_src[6];        // Ethernet header source (may differ from ARP sender on a spoof)
} eth_arp_t;

// Parse a received frame as ARP. Returns true and fills out only for a well-formed IPv4-over-Ethernet
// ARP request/reply; false otherwise. Safe on short/garbage buffers.
bool eth_parse_arp(const uint8_t *frame, size_t len, eth_arp_t *out);

// ---- IPv4 / UDP checksums ---------------------------------------------------
uint16_t eth_ip_checksum(const uint8_t *ip_hdr, size_t hdr_len);
// UDP checksum over the pseudo-header (src/dst host-order IP) + UDP header + payload.
uint16_t eth_udp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *udp, size_t udp_len);

// ---- DHCP -------------------------------------------------------------------
// Build a DHCP DISCOVER (client->255.255.255.255) for starvation: a full Ethernet/IP/UDP/BOOTP frame
// with the given client hardware address as both Ethernet source and BOOTP chaddr, a random-ish xid.
// Returns total frame length. For starvation, call repeatedly with eth_rand_mac() chaddrs.
size_t eth_build_dhcp_discover(uint8_t *buf, const uint8_t chaddr[6], uint32_t xid);

// Build a DHCP REQUEST (selecting a server/offered address) — the second half of a real handshake,
// used by the bootstrap mini-client to actually take the lease it was offered.
size_t eth_build_dhcp_request(uint8_t *buf, const uint8_t chaddr[6], uint32_t xid,
                              uint32_t requested_ip, uint32_t server_ip);

// Build a DHCP OFFER or ACK as a ROGUE server: assign `yiaddr` to the client, advertise `server_ip`
// as the router/DNS (the MITM redirect). msg_type = DHCP_OFFER or DHCP_ACK.
size_t eth_build_dhcp_reply(uint8_t *buf, uint8_t msg_type,
                            const uint8_t client_mac[6], uint32_t xid,
                            uint32_t yiaddr, uint32_t server_ip, uint32_t netmask,
                            uint32_t router_ip, uint32_t dns_ip, uint32_t lease_secs,
                            const uint8_t server_mac[6]);

typedef struct {
    uint8_t  msg_type;        // DHCP_* (0 if not a DHCP packet)
    uint32_t xid;
    uint8_t  chaddr[6];       // client hardware address
    uint32_t yiaddr;          // 'your' address (in OFFER/ACK)
    uint32_t server_id;       // option 54 (0 if absent)
    uint32_t netmask;         // option 1
    uint32_t router;          // option 3
    uint32_t dns;             // option 6
    uint32_t lease;           // option 51
    uint8_t  src_mac[6];      // Ethernet source
} eth_dhcp_t;

// Parse a frame as DHCP-over-UDP/IP. Returns true (and fills out) for any BOOTP/DHCP message; used by
// the bootstrap client (read the OFFER) and the rogue server (see the DISCOVER to answer).
bool eth_parse_dhcp(const uint8_t *frame, size_t len, eth_dhcp_t *out);

// ---- TCP SYN (port scan) ----------------------------------------------------
// Build a TCP SYN probe to dst_ip:dst_port from src. Returns frame length. A returned SYN-ACK means
// the port is open; RST means closed. (Only needs L2 reach; no stack.)
size_t eth_build_tcp_syn(uint8_t *buf,
                         const uint8_t src_mac[6], uint32_t src_ip, uint16_t src_port,
                         const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t dst_port,
                         uint32_t seq);
// Classify a received frame as a TCP response to our probe. Returns: 1 = SYN-ACK (open),
// -1 = RST (closed), 0 = not a matching TCP segment. Fills *port with the source port.
int eth_parse_tcp_reply(const uint8_t *frame, size_t len,
                        uint32_t our_ip, uint32_t peer_ip, uint16_t *port);

// ---- subnet iteration -------------------------------------------------------
// Given our IP and netmask (host order), compute the first/last usable host and step through them.
// eth_subnet_first returns the first host IP; eth_subnet_next returns the next, or 0 past the last.
uint32_t eth_subnet_network(uint32_t ip, uint32_t mask);
uint32_t eth_subnet_bcast(uint32_t ip, uint32_t mask);
uint32_t eth_subnet_first_host(uint32_t ip, uint32_t mask);
uint32_t eth_subnet_next_host(uint32_t cur, uint32_t ip, uint32_t mask);  // 0 when exhausted
uint32_t eth_subnet_size(uint32_t mask);   // number of usable hosts (capped sanity at /16)

// ---- OUI vendor fingerprint -------------------------------------------------
// Resolve the 3-byte OUI of a MAC to a short vendor label using a small built-in table (the common
// IoT/phone/PC vendors). Returns a static string; "?" if unknown. An optional SD-backed bigger DB can
// be layered in nucleo_eth.c; this gives a useful answer offline with zero allocation.
const char *eth_oui_vendor(const uint8_t mac[6]);

// ---- host table (discovery results, dedup by IP) ----------------------------
#define ETH_MAX_HOSTS 64
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  vendor_id;     // index into a UI cache (filled by caller); 0 = unknown
    bool     is_gateway;
    uint32_t last_seen_ms;  // caller-stamped
} eth_host_t;

typedef struct {
    eth_host_t h[ETH_MAX_HOSTS];
    int        n;
} eth_hostlist_t;

void eth_hostlist_clear(eth_hostlist_t *l);
// Insert or refresh by IP. Returns the slot index, or -1 if full. *added set true on first insert.
int  eth_hostlist_upsert(eth_hostlist_t *l, uint32_t ip, const uint8_t mac[6], uint32_t now_ms, bool *added);

#ifdef __cplusplus
}
#endif
