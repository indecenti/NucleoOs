// Host test for the nucleo_sentinel defensive core (firmware/components/
// nucleo_sentinel/core/*.c). Compiled + run by sentinel-check.mjs with MinGW gcc
// — no ESP-IDF, no radio. Proves the BLE tracker classifier, the "following"
// persistence logic, the 802.11 frame parser, and the anomaly monitor against
// real advertising/frame byte layouts BEFORE any device build. DEFENSIVE: this
// only classifies signals others broadcast; nothing here transmits.
#include "sentinel_ble.h"
#include "sentinel_track.h"
#include "sentinel_wifi.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

// Build an 802.11 mgmt header (24 bytes) into f; returns pointer past it.
static size_t wifi_hdr(uint8_t *f, uint8_t b0, const uint8_t a1[6], const uint8_t bssid[6]) {
    memset(f, 0, 24);
    f[0] = b0; f[1] = 0x00;
    memcpy(f + 4, a1, 6);                     // addr1
    memset(f + 10, 0xA2, 6);                  // addr2 (transmitter)
    memcpy(f + 16, bssid, 6);                 // addr3 = BSSID
    return 24;
}
static size_t make_beacon(uint8_t *f, const uint8_t bssid[6], const char *ssid) {
    uint8_t bcast_na[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };
    size_t n = wifi_hdr(f, 0x80, bcast_na, bssid);
    memset(f + n, 0, 12); n += 12;            // timestamp+interval+cap
    size_t sl = strlen(ssid);
    f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
    return n;
}

int main(void) {
    printf("== nucleo_sentinel host test ==\n");
    sentinel_ble_class c;

    // --- BLE tracker classification ------------------------------------------
    { // Apple offline-finding (separated FindMy / AirTag): manuf 0x004C, type 0x12
      uint8_t adv[31] = { 0x1E,0xFF,0x4C,0x00,0x12,0x19 };
      for (int i = 6; i < 31; i++) adv[i] = (uint8_t)i;
      CHECK(sentinel_ble_classify(adv, sizeof adv, &c) && c.type==SENTINEL_TRK_FINDMY &&
            (c.flags & SENTINEL_F_SEPARATED), "FindMy separated (Apple 0x12) detected"); }

    { // Apple nearby (type 0x10) must NOT trip (would flag every iPhone otherwise)
      uint8_t adv[6] = { 0x05,0xFF,0x4C,0x00,0x10,0x02 };
      CHECK(!sentinel_ble_classify(adv, sizeof adv, &c), "Apple non-0x12 not a tracker"); }

    { // Samsung SmartTag via service data 0xFD5A
      uint8_t adv[6] = { 0x05,0x16,0x5A,0xFD,0x01,0x02 };
      CHECK(sentinel_ble_classify(adv, sizeof adv, &c) && c.type==SENTINEL_TRK_SMARTTAG, "SmartTag (svc 0xFD5A) detected"); }

    { // Samsung via manufacturer id 0x0075
      uint8_t adv[6] = { 0x05,0xFF,0x75,0x00,0x01,0x02 };
      CHECK(sentinel_ble_classify(adv, sizeof adv, &c) && c.type==SENTINEL_TRK_SMARTTAG, "SmartTag (manuf 0x0075) detected"); }

    { // Tile via complete 16-bit service UUID 0xFEED
      uint8_t adv[4] = { 0x03,0x03,0xED,0xFE };
      CHECK(sentinel_ble_classify(adv, sizeof adv, &c) && c.type==SENTINEL_TRK_TILE, "Tile (uuid 0xFEED) detected"); }

    { // Unknown vendor -> not a tracker
      uint8_t adv[6] = { 0x05,0xFF,0x34,0x12,0xAA,0xBB };
      CHECK(!sentinel_ble_classify(adv, sizeof adv, &c), "unknown vendor not a tracker"); }

    { // Malformed AD (length runs past buffer) must not over-read
      uint8_t adv[2] = { 0xFF, 0x01 };
      CHECK(!sentinel_ble_classify(adv, sizeof adv, &c), "malformed adv handled safely"); }

    // --- following persistence -----------------------------------------------
    { sentinel_tracker_table t; sentinel_track_init(&t, 300, 3);
      uint8_t A[6] = { 1,2,3,4,5,6 };
      sentinel_track_seen(&t, A, SENTINEL_TRK_TILE, 0, -50, 0);
      sentinel_track_seen(&t, A, SENTINEL_TRK_TILE, 0, -50, 10);
      sentinel_track_seen(&t, A, SENTINEL_TRK_TILE, 0, -50, 20);
      CHECK(sentinel_track_following(&t)==0, "brief sighting not flagged following");
      sentinel_track_seen(&t, A, SENTINEL_TRK_TILE, 0, -50, 400);   // span 400 >= 300, hits 4
      CHECK(sentinel_track_following(&t)==1, "persistent tracker flagged following");

      // A separated FindMy tag qualifies at half the window
      uint8_t S[6] = { 9,9,9,9,9,9 };
      sentinel_track_seen(&t, S, SENTINEL_TRK_FINDMY, SENTINEL_F_SEPARATED, -40, 0);
      sentinel_track_seen(&t, S, SENTINEL_TRK_FINDMY, SENTINEL_F_SEPARATED, -40, 80);
      sentinel_track_seen(&t, S, SENTINEL_TRK_FINDMY, SENTINEL_F_SEPARATED, -40, 160); // span160>=150
      CHECK(sentinel_track_following(&t)==2, "separated tag follows faster (half window)");

      sentinel_track_expire(&t, 1000, 100);   // both last seen well before 900
      CHECK(t.n==0, "expire drops stale trackers"); }

    // --- 802.11 parsing ------------------------------------------------------
    sentinel_wifi_frame fr;
    { uint8_t bcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, bssid[6]={0xDE,0xAD,0xBE,0xEF,0,1};
      uint8_t f[26]; size_t n=wifi_hdr(f,0xC0,bcast,bssid); n+=2;   // + seq
      CHECK(sentinel_wifi_parse(f,n,&fr) && fr.kind==SENTINEL_WF_DEAUTH && fr.to_broadcast, "deauth-to-broadcast parsed"); }
    { uint8_t bssid[6]={0xAB,0xCD,0xEF,0,1,2}; uint8_t f[64]; size_t n=make_beacon(f,bssid,"HomeWiFi");
      CHECK(sentinel_wifi_parse(f,n,&fr) && fr.kind==SENTINEL_WF_BEACON && strcmp(fr.ssid,"HomeWiFi")==0, "beacon SSID parsed");
      CHECK(memcmp(fr.bssid,bssid,6)==0, "beacon BSSID parsed"); }

    // --- anomaly monitor -----------------------------------------------------
    { sentinel_wifi_mon m; sentinel_wifi_mon_init(&m, 10, 5, 100);
      uint8_t bcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, bssid[6]={1,1,1,1,1,1};
      uint8_t f[26]; size_t n=wifi_hdr(f,0xC0,bcast,bssid); n+=2;
      sentinel_wifi_parse(f,n,&fr);
      uint8_t r=sentinel_wifi_mon_feed(&m,&fr,0);
      CHECK(r & SENTINEL_A_BROADCAST_DEAUTH, "broadcast deauth raises alert immediately"); }

    { sentinel_wifi_mon m; sentinel_wifi_mon_init(&m, 10, 5, 100);
      uint8_t uni[6]={0x02,0x03,0x04,0x05,0x06,0x07}, bssid[6]={2,2,2,2,2,2};
      uint8_t f[26]; size_t n=wifi_hdr(f,0xC0,uni,bssid); n+=2; sentinel_wifi_parse(f,n,&fr);
      uint8_t last=0; for(int i=0;i<5;i++) last=sentinel_wifi_mon_feed(&m,&fr,0);
      CHECK(last & SENTINEL_A_DEAUTH_FLOOD, "5 unicast deauth reach flood threshold");
      CHECK(!(last & SENTINEL_A_BROADCAST_DEAUTH), "unicast flood is not a broadcast alert"); }

    { sentinel_wifi_mon m; sentinel_wifi_mon_init(&m, 10, 50, 100);
      uint8_t a[6]={0xA0,0,0,0,0,1}, b[6]={0xB0,0,0,0,0,2};
      uint8_t f[64]; size_t n;
      n=make_beacon(f,a,"CorpNet"); sentinel_wifi_parse(f,n,&fr); uint8_t r1=sentinel_wifi_mon_feed(&m,&fr,0);
      n=make_beacon(f,b,"CorpNet"); sentinel_wifi_parse(f,n,&fr); uint8_t r2=sentinel_wifi_mon_feed(&m,&fr,0);
      CHECK(!(r1 & SENTINEL_A_EVIL_TWIN) && (r2 & SENTINEL_A_EVIL_TWIN), "same SSID, new BSSID -> evil twin"); }

    { sentinel_wifi_mon m; sentinel_wifi_mon_init(&m, 10, 100, 3);
      uint8_t f[64]; uint8_t last=0;
      for(int i=0;i<3;i++){ uint8_t bs[6]={(uint8_t)i,0,0,0,0,9}; char ss[8]; snprintf(ss,sizeof ss,"n%d",i);
        size_t n=make_beacon(f,bs,ss); sentinel_wifi_parse(f,n,&fr); last=sentinel_wifi_mon_feed(&m,&fr,0); }
      CHECK(last & SENTINEL_A_BEACON_FLOOD, "beacon flood threshold reached"); }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
