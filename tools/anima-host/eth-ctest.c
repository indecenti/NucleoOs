// Host test for the wired-attack frame core (firmware/components/nucleo_eth/eth_frames.c).
// Compiled + run by build.ps1 with MinGW gcc — no ESP-IDF, no hardware. Proves header layout, byte
// order, checksums, subnet math, MAC randomisation and parsing BEFORE any frame hits a real wire.
#include "eth_frames.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// Deterministic PRNG so MAC randomisation is reproducible in tests.
static uint32_t g_seed = 0x12345678u;
static uint32_t prng(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// Verify an L4 segment's checksum: recomputing the Internet checksum over a segment that already
// carries its checksum must fold to 0.
static uint16_t verify_csum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    while (n > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

int main(void)
{
    uint8_t buf[ETH_FRAME_MAX];
    const uint8_t MY_MAC[6]  = {0x24,0x0A,0xC4,0x11,0x22,0x33};
    const uint8_t TGT_MAC[6] = {0x3C,0x07,0x54,0xAA,0xBB,0xCC};

    printf("== eth_frames host test ==\n");

    // 1) IP parse / format roundtrip
    {
        uint32_t ip; char s[16];
        CHECK(eth_ip_parse("192.168.1.10", &ip) && ip == 0xC0A8010Au, "ip parse");
        CHECK(!eth_ip_parse("192.168.1.300", &ip), "ip parse reject >255");
        CHECK(!eth_ip_parse("1.2.3", &ip), "ip parse reject short");
        eth_ip_str(0xC0A8010Au, s, sizeof s); CHECK(strcmp(s, "192.168.1.10") == 0, "ip format");
    }

    // 2) ARP request: broadcast eth dst, well-formed, parses back
    {
        uint32_t my_ip = 0xC0A8010Au, tgt_ip = 0xC0A80101u;
        size_t n = eth_build_arp(buf, ARP_OP_REQUEST, ETH_BCAST, MY_MAC, my_ip, ETH_ZERO, tgt_ip);
        CHECK(n == ETH_ARP_LEN, "arp len 42");
        CHECK(eth_mac_is_bcast(buf), "arp req eth dst broadcast");
        eth_arp_t a;
        CHECK(eth_parse_arp(buf, n, &a), "arp req parses");
        CHECK(a.op == ARP_OP_REQUEST && a.sender_ip == my_ip && a.target_ip == tgt_ip, "arp req fields");
        CHECK(eth_mac_eq(a.sender_mac, MY_MAC), "arp req sender mac");
    }

    // 3) ARP reply (spoof): claim tgt_ip is at MY_MAC, sent to a victim
    {
        uint32_t spoof_ip = 0xC0A80101u /*gateway*/;
        size_t n = eth_build_arp(buf, ARP_OP_REPLY, TGT_MAC, MY_MAC, spoof_ip, TGT_MAC, 0xC0A80105u);
        eth_arp_t a;
        CHECK(eth_parse_arp(buf, n, &a), "arp reply parses");
        CHECK(a.op == ARP_OP_REPLY && a.sender_ip == spoof_ip && eth_mac_eq(a.sender_mac, MY_MAC),
              "arp spoof advertises our mac for gateway ip");
        CHECK(eth_mac_eq(buf, TGT_MAC), "arp spoof unicast to victim");
    }
    // reject garbage
    {
        uint8_t junk[60]; memset(junk, 0xAB, sizeof junk); eth_arp_t a;
        CHECK(!eth_parse_arp(junk, sizeof junk, &a), "arp rejects garbage");
        CHECK(!eth_parse_arp(buf, 10, &a), "arp rejects short");
    }

    // 4) DHCP DISCOVER: well-formed, checksums valid, parses back
    {
        uint8_t chaddr[6]; eth_rand_mac(chaddr, prng);
        uint32_t xid = prng();
        size_t n = eth_build_dhcp_discover(buf, chaddr, xid);
        CHECK(n > ETH_HDR_LEN + 20 + 8 + 236 + 4, "dhcp discover length sane");
        CHECK(eth_mac_is_bcast(buf), "dhcp discover broadcast");
        // IP header checksum verifies to 0
        CHECK(verify_csum(buf + ETH_HDR_LEN, 20) == 0, "dhcp ip checksum valid");
        // UDP checksum valid: ones-complement over [pseudo-header || udp segment(with cksum)] folds to 0
        uint16_t iplen = (uint16_t)((buf[ETH_HDR_LEN + 2] << 8) | buf[ETH_HDR_LEN + 3]);
        size_t udp_len = iplen - 20;
        uint8_t chk[12 + 600];
        uint8_t ph[12] = {0,0,0,0, 255,255,255,255, 0,17, (uint8_t)(udp_len>>8),(uint8_t)udp_len};
        memcpy(chk, ph, 12);
        memcpy(chk + 12, buf + ETH_HDR_LEN + 20, udp_len);
        CHECK(verify_csum(chk, 12 + udp_len) == 0, "dhcp udp checksum valid");
        eth_dhcp_t d;
        CHECK(eth_parse_dhcp(buf, n, &d), "dhcp discover parses");
        CHECK(d.msg_type == DHCP_DISCOVER && d.xid == xid, "dhcp discover type+xid");
        CHECK(eth_mac_eq(d.chaddr, chaddr), "dhcp discover chaddr");
    }

    // 5) Rogue DHCP OFFER: redirect router/DNS to us, parses back
    {
        uint8_t cli[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
        uint32_t srv = 0xC0A801FEu, yi = 0xC0A80164u, mask = 0xFFFFFF00u, lease = 3600;
        size_t n = eth_build_dhcp_reply(buf, DHCP_OFFER, cli, 0xDEADBEEF, yi, srv, mask, srv, srv, lease, MY_MAC);
        CHECK(verify_csum(buf + ETH_HDR_LEN, 20) == 0, "rogue offer ip checksum valid");
        eth_dhcp_t d;
        CHECK(eth_parse_dhcp(buf, n, &d), "rogue offer parses");
        CHECK(d.msg_type == DHCP_OFFER && d.yiaddr == yi && d.router == srv && d.dns == srv && d.netmask == mask,
              "rogue offer redirects router+dns to attacker");
        CHECK(d.server_id == srv && d.lease == lease, "rogue offer server id + lease");
        CHECK(eth_mac_eq(buf, cli), "rogue offer unicast to client");
    }

    // 6) TCP SYN -> craft a SYN-ACK back -> classified open
    {
        uint32_t my_ip = 0xC0A8010Au, peer = 0xC0A80101u;
        size_t n = eth_build_tcp_syn(buf, MY_MAC, my_ip, 40000, TGT_MAC, peer, 80, 0xABCDEF01);
        CHECK(n == ETH_HDR_LEN + 20 + 20, "tcp syn len 54");
        CHECK(verify_csum(buf + ETH_HDR_LEN, 20) == 0, "tcp syn ip checksum valid");
        // forge a SYN-ACK from peer:80 -> us
        uint8_t r[ETH_FRAME_MAX];
        size_t rn = eth_build_tcp_syn(r, TGT_MAC, peer, 80, MY_MAC, my_ip, 40000, 0x10000000);
        r[ETH_HDR_LEN + 20 + 13] = 0x12;   // set SYN+ACK flags
        uint16_t port = 0;
        CHECK(eth_parse_tcp_reply(r, rn, my_ip, peer, &port) == 1 && port == 80, "tcp syn-ack -> open:80");
        r[ETH_HDR_LEN + 20 + 13] = 0x04;   // RST
        CHECK(eth_parse_tcp_reply(r, rn, my_ip, peer, &port) == -1, "tcp rst -> closed");
    }

    // 7) random MAC properties + distinctness
    {
        uint8_t m1[6], m2[6]; eth_rand_mac(m1, prng); eth_rand_mac(m2, prng);
        CHECK((m1[0] & 0x01) == 0, "rand mac unicast (group bit clear)");
        CHECK((m1[0] & 0x02) == 0x02, "rand mac locally-administered");
        CHECK(!eth_mac_eq(m1, m2), "rand mac distinct");
        CHECK(strcmp(eth_oui_vendor(m1), "(random)") == 0, "rand mac flagged random by oui");
    }

    // 8) subnet math /24
    {
        uint32_t ip = 0xC0A8010Au, mask = 0xFFFFFF00u;
        CHECK(eth_subnet_network(ip, mask) == 0xC0A80100u, "subnet network .0");
        CHECK(eth_subnet_bcast(ip, mask) == 0xC0A801FFu, "subnet bcast .255");
        CHECK(eth_subnet_first_host(ip, mask) == 0xC0A80101u, "subnet first .1");
        CHECK(eth_subnet_size(mask) == 254, "subnet /24 = 254 hosts");
        int count = 0; uint32_t h = eth_subnet_first_host(ip, mask);
        while (h) { count++; h = eth_subnet_next_host(h, ip, mask); if (count > 1000) break; }
        CHECK(count == 254, "subnet iterate 254 hosts");
    }
    // /30 edge
    {
        uint32_t ip = 0x0A000005u, mask = 0xFFFFFFFCu;   // 10.0.0.5/30 -> hosts .5,.6
        CHECK(eth_subnet_size(mask) == 2, "subnet /30 = 2 hosts");
        CHECK(eth_subnet_first_host(ip, mask) == 0x0A000005u, "subnet /30 first host .5");
    }

    // 9) OUI vendor lookup
    {
        uint8_t esp[6] = {0x24,0x0A,0xC4,0,0,0};
        CHECK(strcmp(eth_oui_vendor(esp), "Espressif") == 0, "oui espressif");
        uint8_t unk[6] = {0x00,0xAB,0xCD,0,0,0};   // globally-unique bit set, not in table
        CHECK(strcmp(eth_oui_vendor(unk), "?") == 0, "oui unknown -> ?");
    }

    // 10) host table upsert / dedupe
    {
        eth_hostlist_t l; eth_hostlist_clear(&l); bool added;
        eth_hostlist_upsert(&l, 0xC0A80101u, MY_MAC, 100, &added);  CHECK(added, "host first insert");
        eth_hostlist_upsert(&l, 0xC0A80101u, TGT_MAC, 200, &added); CHECK(!added, "host dedupe by ip");
        CHECK(l.n == 1 && eth_mac_eq(l.h[0].mac, TGT_MAC) && l.h[0].last_seen_ms == 200, "host refresh updates mac+seen");
        eth_hostlist_upsert(&l, 0xC0A80102u, MY_MAC, 300, &added);  CHECK(added && l.n == 2, "host second insert");
    }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
