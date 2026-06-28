// Host test for the KARMA probe-request SSID parser (firmware/components/nucleo_wifiatk/
// nucleo_wifiatk_probe.c). Compiled + run by probe-check.mjs with MinGW gcc — no ESP-IDF, no radio.
// Proves the 802.11 probe-request parsing (subtype gate, IE walk, SSID extraction, the broadcast /
// non-printable / malformed rejections) BEFORE any of it drives the live KARMA sniffer. FOR
// AUTHORIZED TESTING ONLY — this validates byte parsing, not on-air behaviour.
#include "nucleo_wifiatk_probe.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// Build a probe-request frame: 24-byte mgmt header + an SSID IE (tag 0) + an optional trailing
// supported-rates IE. `ssid`==NULL or ssidlen==0 makes a broadcast (wildcard) probe. Returns length.
static int build_probe(uint8_t *f, uint8_t fc0, const char *ssid, int ssidlen, int trailing_rates)
{
    int n = 0;
    f[n++] = fc0;                          // frame control byte 0 (0x40 = probe request)
    f[n++] = 0x00;                         // flags
    f[n++] = 0x00; f[n++] = 0x00;          // duration
    for (int i = 0; i < 6; i++) f[n++] = 0xFF;   // DA = broadcast
    for (int i = 0; i < 6; i++) f[n++] = 0x02;   // SA
    for (int i = 0; i < 6; i++) f[n++] = 0xFF;   // BSSID = broadcast (probe)
    f[n++] = 0x00; f[n++] = 0x00;          // seq/frag  -> n == 24 here
    f[n++] = 0x00;                         // SSID element tag
    f[n++] = (uint8_t)ssidlen;             // SSID length
    for (int i = 0; i < ssidlen; i++) f[n++] = ssid ? (uint8_t)ssid[i] : 0x20;
    if (trailing_rates) {                  // supported rates IE after the SSID
        f[n++] = 0x01; f[n++] = 0x04; f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    }
    return n;
}

int main(void)
{
    uint8_t f[256]; char out[40];
    printf("== nucleo_wifiatk_probe host test ==\n");

    // 1) Directed probe for "HomeNet" -> length 7, exact copy.
    {
        int len = build_probe(f, 0x40, "HomeNet", 7, 0);
        int r = wifiatk_probe_ssid(f, len, out, sizeof out);
        CHECK(r == 7, "directed probe length");
        CHECK(strcmp(out, "HomeNet") == 0, "directed probe ssid");
    }
    // 2) SSID followed by a rates IE still parses (SSID is first IE).
    {
        int len = build_probe(f, 0x40, "Cafe Free", 9, 1);
        int r = wifiatk_probe_ssid(f, len, out, sizeof out);
        CHECK(r == 9 && strcmp(out, "Cafe Free") == 0, "ssid before rates IE");
    }
    // 3) Broadcast/wildcard probe (empty SSID) -> rejected.
    {
        int len = build_probe(f, 0x40, NULL, 0, 1);
        CHECK(wifiatk_probe_ssid(f, len, out, sizeof out) == -1, "broadcast probe rejected");
    }
    // 4) Not a probe request (beacon subtype 0x80) -> rejected.
    {
        int len = build_probe(f, 0x80, "HomeNet", 7, 0);
        CHECK(wifiatk_probe_ssid(f, len, out, sizeof out) == -1, "non-probe rejected");
    }
    // 5) Control byte in SSID -> rejected (hidden/garbage).
    {
        char bad[4] = { 'A', 0x01, 'C', 0 };
        int len = build_probe(f, 0x40, bad, 3, 0);
        CHECK(wifiatk_probe_ssid(f, len, out, sizeof out) == -1, "control byte rejected");
    }
    // 5b) UTF-8 SSID (high bytes) -> ACCEPTED (was wrongly dropped before).
    {
        unsigned char utf[] = { 'C', 'a', 'f', 'f', 0xC3, 0xA8 };   // "Caffè"
        int len = build_probe(f, 0x40, (const char *)utf, 6, 0);
        int r = wifiatk_probe_ssid(f, len, out, sizeof out);
        CHECK(r == 6, "utf8 ssid accepted");
        CHECK((unsigned char)out[4] == 0xC3 && (unsigned char)out[5] == 0xA8, "utf8 bytes preserved");
    }
    // 6) Max-length 32-char SSID -> full copy.
    {
        const char *s32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";   // exactly 32
        int len = build_probe(f, 0x40, s32, 32, 0);
        int r = wifiatk_probe_ssid(f, len, out, sizeof out);
        CHECK(r == 32 && strcmp(out, s32) == 0, "32-char ssid");
    }
    // 7) Truncated frame (header only) -> rejected.
    CHECK(wifiatk_probe_ssid(f, 10, out, sizeof out) == -1, "short frame rejected");
    // 8) Malformed IE: SSID length claims more than the frame holds -> rejected.
    {
        int len = build_probe(f, 0x40, "Hi", 2, 0);
        f[25] = 30;                          // overstate the SSID length
        CHECK(wifiatk_probe_ssid(f, len, out, sizeof out) == -1, "overlong IE rejected");
    }
    // 9) Output buffer too small -> rejected (no overflow).
    {
        int len = build_probe(f, 0x40, "HomeNet", 7, 0);
        char tiny[4];
        CHECK(wifiatk_probe_ssid(f, len, tiny, sizeof tiny) == -1, "tiny buffer rejected");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
