// eth_frames — implementation. Pure byte-buffer frame craft; no ESP/FreeRTOS/hardware. See header.
#include "eth_frames.h"
#include <string.h>
#include <stdio.h>

const uint8_t ETH_BCAST[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
const uint8_t ETH_ZERO[6]  = { 0,0,0,0,0,0 };

bool eth_mac_eq(const uint8_t a[6], const uint8_t b[6])    { return memcmp(a, b, 6) == 0; }
bool eth_mac_is_zero(const uint8_t a[6])                   { return memcmp(a, ETH_ZERO, 6) == 0; }
bool eth_mac_is_bcast(const uint8_t a[6])                  { return memcmp(a, ETH_BCAST, 6) == 0; }
bool eth_mac_is_mcast(const uint8_t a[6])                  { return (a[0] & 0x01) != 0; }

char *eth_mac_str(const uint8_t mac[6], char *out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return out;
}

bool eth_ip_parse(const char *s, uint32_t *out)
{
    unsigned a,b,c,d; char extra;
    if (!s) return false;
    if (sscanf(s, "%u.%u.%u.%u%c", &a,&b,&c,&d,&extra) != 4) return false;
    if (a>255||b>255||c>255||d>255) return false;
    *out = (a<<24)|(b<<16)|(c<<8)|d;
    return true;
}

char *eth_ip_str(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)((ip>>24)&0xFF),(unsigned)((ip>>16)&0xFF),(unsigned)((ip>>8)&0xFF),(unsigned)(ip&0xFF));
    return out;
}

void eth_rand_mac(uint8_t out[6], eth_rng_fn rng)
{
    uint32_t r0 = rng(), r1 = rng();
    out[0] = (uint8_t)((r0 & 0xFE) | 0x02);   // clear multicast bit, set locally-administered bit
    out[1] = (uint8_t)(r0 >> 8);
    out[2] = (uint8_t)(r0 >> 16);
    out[3] = (uint8_t)(r1);
    out[4] = (uint8_t)(r1 >> 8);
    out[5] = (uint8_t)(r1 >> 16);
}

// ---- Ethernet header --------------------------------------------------------
static size_t eth_hdr(uint8_t *b, const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype)
{
    memcpy(b, dst, 6);
    memcpy(b + 6, src, 6);
    eth_be16(b + 12, ethertype);
    return ETH_HDR_LEN;
}

// ---- ARP --------------------------------------------------------------------
size_t eth_build_arp(uint8_t *buf, uint16_t op, const uint8_t eth_dst[6],
                     const uint8_t src_mac[6], uint32_t src_ip,
                     const uint8_t tgt_mac[6], uint32_t tgt_ip)
{
    uint8_t *p = buf + eth_hdr(buf, eth_dst, src_mac, ETHERTYPE_ARP);
    eth_be16(p + 0, 1);              // htype: Ethernet
    eth_be16(p + 2, ETHERTYPE_IPV4); // ptype: IPv4
    p[4] = 6;                        // hlen
    p[5] = 4;                        // plen
    eth_be16(p + 6, op);
    memcpy(p + 8, src_mac, 6);       // sender hw
    eth_be32(p + 14, src_ip);        // sender proto
    memcpy(p + 18, tgt_mac ? tgt_mac : ETH_ZERO, 6); // target hw
    eth_be32(p + 24, tgt_ip);        // target proto
    return ETH_ARP_LEN;
}

bool eth_parse_arp(const uint8_t *frame, size_t len, eth_arp_t *out)
{
    if (len < ETH_ARP_LEN || eth_rd16(frame + 12) != ETHERTYPE_ARP) return false;
    const uint8_t *p = frame + ETH_HDR_LEN;
    if (eth_rd16(p) != 1 || eth_rd16(p + 2) != ETHERTYPE_IPV4 || p[4] != 6 || p[5] != 4) return false;
    uint16_t op = eth_rd16(p + 6);
    if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY) return false;
    out->op = op;
    memcpy(out->sender_mac, p + 8, 6);
    out->sender_ip = eth_rd32(p + 14);
    memcpy(out->target_mac, p + 18, 6);
    out->target_ip = eth_rd32(p + 24);
    memcpy(out->eth_src, frame + 6, 6);
    return true;
}

// ---- checksums --------------------------------------------------------------
static uint32_t csum_add(const uint8_t *p, size_t n, uint32_t sum)
{
    while (n > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n)        { sum += (uint16_t)(p[0] << 8); }
    return sum;
}
static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

uint16_t eth_ip_checksum(const uint8_t *ip_hdr, size_t hdr_len)
{
    return csum_fold(csum_add(ip_hdr, hdr_len, 0));
}

// Pseudo-header checksum for UDP(17)/TCP(6).
static uint16_t l4_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const uint8_t *seg, size_t seg_len)
{
    uint8_t ph[12];
    eth_be32(ph + 0, src_ip);
    eth_be32(ph + 4, dst_ip);
    ph[8] = 0; ph[9] = proto;
    eth_be16(ph + 10, (uint16_t)seg_len);
    uint32_t sum = csum_add(ph, sizeof ph, 0);
    sum = csum_add(seg, seg_len, sum);
    uint16_t c = csum_fold(sum);
    return c ? c : 0xFFFF;   // UDP: a zero checksum means "none", so transmit 0xFFFF instead
}
uint16_t eth_udp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *udp, size_t udp_len)
{
    return l4_checksum(src_ip, dst_ip, 17, udp, udp_len);
}

// ---- IPv4 header writer (shared by DHCP/TCP builders) -----------------------
// Writes a 20-byte IPv4 header at p; checksum field left for the caller to fill after total_len known.
static void ip_hdr(uint8_t *p, uint8_t proto, uint32_t src, uint32_t dst, uint16_t total_len, uint16_t id)
{
    p[0] = 0x45;            // version 4, IHL 5
    p[1] = 0x00;            // DSCP/ECN
    eth_be16(p + 2, total_len);
    eth_be16(p + 4, id);
    eth_be16(p + 6, 0x4000);// don't fragment
    p[8] = 64;              // TTL
    p[9] = proto;
    eth_be16(p + 10, 0);    // checksum (filled below)
    eth_be32(p + 12, src);
    eth_be32(p + 16, dst);
    eth_be16(p + 10, eth_ip_checksum(p, 20));
}

// ---- DHCP -------------------------------------------------------------------
#define BOOTP_FIXED 236     // op..file
#define DHCP_MAGIC  0x63825363u

// Assemble Ethernet/IP/UDP/BOOTP with the given options appended after the magic cookie.
static size_t dhcp_assemble(uint8_t *buf,
                            const uint8_t eth_dst[6], const uint8_t eth_src[6],
                            uint32_t ip_src, uint32_t ip_dst,
                            uint16_t udp_sport, uint16_t udp_dport,
                            uint8_t bootp_op, uint32_t xid, uint16_t flags,
                            uint32_t ciaddr, uint32_t yiaddr, uint32_t siaddr, uint32_t giaddr,
                            const uint8_t chaddr[6],
                            const uint8_t *opts, size_t opts_len)
{
    uint8_t *e = buf;
    eth_hdr(e, eth_dst, eth_src, ETHERTYPE_IPV4);
    uint8_t *ip = e + ETH_HDR_LEN;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;

    size_t bootp_len = BOOTP_FIXED + 4 /*magic*/ + opts_len;
    size_t udp_len   = 8 + bootp_len;
    size_t ip_len    = 20 + udp_len;

    // BOOTP fixed
    memset(bootp, 0, BOOTP_FIXED);
    bootp[0] = bootp_op;     // 1=request, 2=reply
    bootp[1] = 1;            // htype Ethernet
    bootp[2] = 6;            // hlen
    bootp[3] = 0;            // hops
    eth_be32(bootp + 4, xid);
    eth_be16(bootp + 8, 0);  // secs
    eth_be16(bootp + 10, flags);
    eth_be32(bootp + 12, ciaddr);
    eth_be32(bootp + 16, yiaddr);
    eth_be32(bootp + 20, siaddr);
    eth_be32(bootp + 24, giaddr);
    memcpy(bootp + 28, chaddr, 6);   // chaddr (16 bytes, rest already zero)
    eth_be32(bootp + BOOTP_FIXED, DHCP_MAGIC);
    memcpy(bootp + BOOTP_FIXED + 4, opts, opts_len);

    // UDP
    eth_be16(udp + 0, udp_sport);
    eth_be16(udp + 2, udp_dport);
    eth_be16(udp + 4, (uint16_t)udp_len);
    eth_be16(udp + 6, 0);
    // IP (also fills its own checksum)
    ip_hdr(ip, 17, ip_src, ip_dst, (uint16_t)ip_len, (uint16_t)(xid & 0xFFFF));
    // UDP checksum needs the IP addrs + full UDP segment
    eth_be16(udp + 6, eth_udp_checksum(ip_src, ip_dst, udp, udp_len));

    return ETH_HDR_LEN + ip_len;
}

size_t eth_build_dhcp_discover(uint8_t *buf, const uint8_t chaddr[6], uint32_t xid)
{
    uint8_t opts[16]; size_t o = 0;
    opts[o++] = 53; opts[o++] = 1; opts[o++] = DHCP_DISCOVER;  // message type
    opts[o++] = 55; opts[o++] = 4; opts[o++] = 1; opts[o++] = 3; opts[o++] = 6; opts[o++] = 51; // param request list
    opts[o++] = 255;                                           // end
    return dhcp_assemble(buf, ETH_BCAST, chaddr, 0u, 0xFFFFFFFFu, 68, 67,
                         1 /*request*/, xid, 0x8000 /*broadcast reply*/,
                         0,0,0,0, chaddr, opts, o);
}

size_t eth_build_dhcp_request(uint8_t *buf, const uint8_t chaddr[6], uint32_t xid,
                              uint32_t requested_ip, uint32_t server_ip)
{
    uint8_t opts[24]; size_t o = 0;
    opts[o++] = 53; opts[o++] = 1; opts[o++] = DHCP_REQUEST;
    opts[o++] = 50; opts[o++] = 4; eth_be32(opts + o, requested_ip); o += 4;   // requested IP
    opts[o++] = 54; opts[o++] = 4; eth_be32(opts + o, server_ip);    o += 4;   // server identifier
    opts[o++] = 255;
    return dhcp_assemble(buf, ETH_BCAST, chaddr, 0u, 0xFFFFFFFFu, 68, 67,
                         1, xid, 0x8000, 0,0,0,0, chaddr, opts, o);
}

size_t eth_build_dhcp_reply(uint8_t *buf, uint8_t msg_type,
                            const uint8_t client_mac[6], uint32_t xid,
                            uint32_t yiaddr, uint32_t server_ip, uint32_t netmask,
                            uint32_t router_ip, uint32_t dns_ip, uint32_t lease_secs,
                            const uint8_t server_mac[6])
{
    uint8_t opts[40]; size_t o = 0;
    opts[o++] = 53; opts[o++] = 1; opts[o++] = msg_type;
    opts[o++] = 54; opts[o++] = 4; eth_be32(opts + o, server_ip);  o += 4;     // server identifier
    opts[o++] = 51; opts[o++] = 4; eth_be32(opts + o, lease_secs); o += 4;     // lease time
    opts[o++] = 1;  opts[o++] = 4; eth_be32(opts + o, netmask);    o += 4;     // subnet mask
    opts[o++] = 3;  opts[o++] = 4; eth_be32(opts + o, router_ip);  o += 4;     // router (MITM redirect)
    opts[o++] = 6;  opts[o++] = 4; eth_be32(opts + o, dns_ip);     o += 4;     // DNS (MITM redirect)
    opts[o++] = 255;
    // Unicast to the client at L2; siaddr = our server IP.
    return dhcp_assemble(buf, client_mac, server_mac, server_ip, 0xFFFFFFFFu, 67, 68,
                         2 /*reply*/, xid, 0x8000, 0, yiaddr, server_ip, 0, server_mac, opts, o);
}

// Walk DHCP options, populating *out. opts points at the first option (after the magic cookie).
static void dhcp_scan_opts(const uint8_t *opts, size_t n, eth_dhcp_t *out)
{
    size_t i = 0;
    while (i < n) {
        uint8_t code = opts[i++];
        if (code == 255) break;          // end
        if (code == 0) continue;         // pad
        if (i >= n) break;
        uint8_t len = opts[i++];
        if (i + len > n) break;
        const uint8_t *v = opts + i;
        switch (code) {
            case 53: if (len >= 1) out->msg_type = v[0]; break;
            case 54: if (len >= 4) out->server_id = eth_rd32(v); break;
            case 1:  if (len >= 4) out->netmask   = eth_rd32(v); break;
            case 3:  if (len >= 4) out->router    = eth_rd32(v); break;
            case 6:  if (len >= 4) out->dns       = eth_rd32(v); break;
            case 51: if (len >= 4) out->lease     = eth_rd32(v); break;
            default: break;
        }
        i += len;
    }
}

bool eth_parse_dhcp(const uint8_t *frame, size_t len, eth_dhcp_t *out)
{
    if (len < ETH_HDR_LEN + 20 + 8 + BOOTP_FIXED + 4) return false;
    if (eth_rd16(frame + 12) != ETHERTYPE_IPV4) return false;
    const uint8_t *ip = frame + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) return false;
    size_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || ip[9] != 17) return false;                 // UDP
    const uint8_t *udp = ip + ihl;
    if (udp + 8 > frame + len) return false;
    uint16_t sp = eth_rd16(udp), dp = eth_rd16(udp + 2);
    if (!((sp == 67 && dp == 68) || (sp == 68 && dp == 67))) return false;
    const uint8_t *bootp = udp + 8;
    if (bootp + BOOTP_FIXED + 4 > frame + len) return false;
    if (eth_rd32(bootp + BOOTP_FIXED) != DHCP_MAGIC) return false;

    memset(out, 0, sizeof *out);
    out->xid    = eth_rd32(bootp + 4);
    out->yiaddr = eth_rd32(bootp + 16);
    memcpy(out->chaddr, bootp + 28, 6);
    memcpy(out->src_mac, frame + 6, 6);
    const uint8_t *opts = bootp + BOOTP_FIXED + 4;
    size_t opts_n = (size_t)(frame + len - opts);
    dhcp_scan_opts(opts, opts_n, out);
    return true;
}

// ---- TCP SYN ----------------------------------------------------------------
size_t eth_build_tcp_syn(uint8_t *buf,
                         const uint8_t src_mac[6], uint32_t src_ip, uint16_t src_port,
                         const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t dst_port,
                         uint32_t seq)
{
    eth_hdr(buf, dst_mac, src_mac, ETHERTYPE_IPV4);
    uint8_t *ip = buf + ETH_HDR_LEN;
    uint8_t *tcp = ip + 20;
    size_t tcp_len = 20;
    ip_hdr(ip, 6, src_ip, dst_ip, (uint16_t)(20 + tcp_len), (uint16_t)(seq & 0xFFFF));
    eth_be16(tcp + 0, src_port);
    eth_be16(tcp + 2, dst_port);
    eth_be32(tcp + 4, seq);
    eth_be32(tcp + 8, 0);          // ack
    tcp[12] = 0x50;                // data offset 5 words, no flags in reserved
    tcp[13] = 0x02;                // SYN
    eth_be16(tcp + 14, 0x4000);    // window
    eth_be16(tcp + 16, 0);         // checksum (below)
    eth_be16(tcp + 18, 0);         // urgent
    eth_be16(tcp + 16, l4_checksum(src_ip, dst_ip, 6, tcp, tcp_len));
    return ETH_HDR_LEN + 20 + tcp_len;
}

int eth_parse_tcp_reply(const uint8_t *frame, size_t len, uint32_t our_ip, uint32_t peer_ip, uint16_t *port)
{
    if (len < ETH_HDR_LEN + 20 + 20) return 0;
    if (eth_rd16(frame + 12) != ETHERTYPE_IPV4) return 0;
    const uint8_t *ip = frame + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) return 0;
    size_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || ip[9] != 6) return 0;
    if (eth_rd32(ip + 12) != peer_ip || eth_rd32(ip + 16) != our_ip) return 0;
    const uint8_t *tcp = ip + ihl;
    if (tcp + 20 > frame + len) return 0;
    if (port) *port = eth_rd16(tcp);
    uint8_t flags = tcp[13];
    if ((flags & 0x04)) return -1;                      // RST -> closed
    if ((flags & 0x12) == 0x12) return 1;               // SYN+ACK -> open
    return 0;
}

// ---- subnet -----------------------------------------------------------------
uint32_t eth_subnet_network(uint32_t ip, uint32_t mask)    { return ip & mask; }
uint32_t eth_subnet_bcast(uint32_t ip, uint32_t mask)      { return (ip & mask) | (~mask); }
uint32_t eth_subnet_first_host(uint32_t ip, uint32_t mask)
{
    uint32_t net = ip & mask, bc = net | (~mask);
    if (bc - net < 2) return 0;     // /31 or /32: no usable host range
    return net + 1;
}
uint32_t eth_subnet_next_host(uint32_t cur, uint32_t ip, uint32_t mask)
{
    uint32_t net = ip & mask, bc = net | (~mask);
    if (cur + 1 >= bc) return 0;    // reached broadcast or past it
    if (cur + 1 <= net) return net + 1;
    return cur + 1;
}
uint32_t eth_subnet_size(uint32_t mask)
{
    uint32_t hosts = (~mask);
    if (hosts >= 2) hosts -= 1; else hosts = 0;   // exclude network + broadcast
    if (hosts > 65534) hosts = 65534;             // sanity cap (>/16 makes no sense to sweep)
    return hosts;
}

// ---- OUI vendor table -------------------------------------------------------
// Small, hand-picked set of the most common IoT/phone/PC/router vendors. Not exhaustive — a fuller
// IEEE OUI DB can be layered from SD in nucleo_eth.c; this gives a useful offline hint with no alloc.
typedef struct { uint8_t oui[3]; const char *name; } oui_ent_t;
static const oui_ent_t OUI[] = {
    {{0x24,0x0A,0xC4}, "Espressif"}, {{0x7C,0xDF,0xA1}, "Espressif"}, {{0x30,0xAE,0xA4}, "Espressif"},
    {{0xB8,0x27,0xEB}, "RaspberryPi"},{{0xDC,0xA6,0x32}, "RaspberryPi"},{{0xE4,0x5F,0x01}, "RaspberryPi"},
    {{0x3C,0x07,0x54}, "Apple"},     {{0xAC,0xBC,0x32}, "Apple"},      {{0xF0,0x18,0x98}, "Apple"},
    {{0x00,0x1C,0xB3}, "Apple"},     {{0xA4,0x83,0xE7}, "Apple"},
    {{0x34,0x23,0x87}, "Samsung"},   {{0x00,0x16,0x32}, "Samsung"},    {{0x8C,0x77,0x12}, "Samsung"},
    {{0x50,0xC7,0xBF}, "TP-Link"},   {{0xC4,0x6E,0x1F}, "TP-Link"},    {{0x14,0xCC,0x20}, "TP-Link"},
    {{0x00,0x1B,0x21}, "Intel"},     {{0x3C,0x97,0x0E}, "Intel"},      {{0xA0,0x88,0xB4}, "Intel"},
    {{0x00,0x1A,0x11}, "Google"},    {{0xF4,0xF5,0xE8}, "Google"},     {{0x18,0xB4,0x30}, "Nest"},
    {{0xFC,0xA6,0x67}, "Amazon"},    {{0x44,0x65,0x0D}, "Amazon"},     {{0x68,0x37,0xE9}, "Amazon"},
    {{0x00,0x50,0x56}, "VMware"},    {{0x08,0x00,0x27}, "VirtualBox"}, {{0x52,0x54,0x00}, "QEMU/KVM"},
    {{0x00,0x0C,0x29}, "VMware"},    {{0x00,0x1D,0x0F}, "Cisco"},      {{0x00,0x18,0x0A}, "Cisco-Meraki"},
    {{0xB4,0x2E,0x99}, "Huawei"},    {{0x28,0x6C,0x07}, "Xiaomi"},     {{0x50,0xEC,0x50}, "Sony"},
    {{0x00,0x21,0x9B}, "Dell"},      {{0x18,0x66,0xDA}, "Dell"},       {{0x00,0x1E,0xC2}, "HP"},
    {{0x00,0x17,0x88}, "PhilipsHue"},{{0xEC,0xFA,0xBC}, "Broadlink"},
};
const char *eth_oui_vendor(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return "(random)";   // locally-administered: randomised/private MAC
    for (size_t i = 0; i < sizeof(OUI)/sizeof(OUI[0]); i++)
        if (memcmp(OUI[i].oui, mac, 3) == 0) return OUI[i].name;
    return "?";
}

// ---- host table -------------------------------------------------------------
void eth_hostlist_clear(eth_hostlist_t *l) { l->n = 0; }

int eth_hostlist_upsert(eth_hostlist_t *l, uint32_t ip, const uint8_t mac[6], uint32_t now_ms, bool *added)
{
    for (int i = 0; i < l->n; i++) {
        if (l->h[i].ip == ip) {
            memcpy(l->h[i].mac, mac, 6);
            l->h[i].last_seen_ms = now_ms;
            if (added) *added = false;
            return i;
        }
    }
    if (l->n >= ETH_MAX_HOSTS) { if (added) *added = false; return -1; }
    int i = l->n++;
    l->h[i].ip = ip;
    memcpy(l->h[i].mac, mac, 6);
    l->h[i].vendor_id = 0;
    l->h[i].is_gateway = false;
    l->h[i].last_seen_ms = now_ms;
    if (added) *added = true;
    return i;
}
