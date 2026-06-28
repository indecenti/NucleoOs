// Host test for the WPA 4-way-handshake classifier (firmware/components/nucleo_wifiatk/
// nucleo_wifiatk_eapol.c). Compiled + run by eapol-check.mjs with MinGW gcc — no ESP-IDF, no radio.
// Proves the data-frame gate, QoS header shift, LLC/SNAP EtherType test, and the EAPOL-Key
// Key-Information bit logic that labels messages 1..4 — BEFORE any of it drives handshake capture.
// FOR AUTHORIZED TESTING ONLY — byte parsing only.
#include "nucleo_wifiatk_eapol.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// Build an 802.11 DATA frame carrying an EAPOL-Key with the given Key Information word.
// fc0 = 0x08 (data) or 0x88 (QoS data). Returns the frame length.
static int build_eapol(uint8_t *f, uint8_t fc0, unsigned ki)
{
    int n = 0;
    f[n++] = fc0; f[n++] = 0x01;                 // frame control (toDS)
    f[n++] = 0; f[n++] = 0;                       // duration
    for (int i = 0; i < 6; i++) f[n++] = 0xBB;    // addr1
    for (int i = 0; i < 6; i++) f[n++] = 0xCC;    // addr2
    for (int i = 0; i < 6; i++) f[n++] = 0xBB;    // addr3
    f[n++] = 0; f[n++] = 0;                        // seq/frag  -> n == 24
    if ((fc0 & 0xF0) == 0x80) { f[n++] = 0; f[n++] = 0; }   // QoS control (+2)
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
    memcpy(f + n, snap, 8); n += 8;               // LLC/SNAP + EtherType (EAPOL)
    f[n++] = 0x02; f[n++] = 0x03; f[n++] = 0x00; f[n++] = 0x5F;   // EAPOL ver2, type3 (Key), len
    f[n++] = 0x02;                                // descriptor type
    f[n++] = (uint8_t)((ki >> 8) & 0xFF); f[n++] = (uint8_t)(ki & 0xFF);   // Key Information
    for (int k = 0; k < 32; k++) f[n++] = 0;      // rest of the key frame (ignored by the classifier)
    return n;
}

// Build a proper EAPOL-Key MESSAGE 1 (AP->STA), optionally with an RSN PMKID KDE in the key data.
static int build_m1(uint8_t *f, int with_pmkid)
{
    int n = 0;
    f[n++] = 0x08; f[n++] = 0x02;                 // data frame, fromDS (AP->STA)
    f[n++] = 0; f[n++] = 0;
    for (int i = 0; i < 6; i++) f[n++] = 0xCC;    // addr1 = STA
    for (int i = 0; i < 6; i++) f[n++] = 0xBB;    // addr2 = AP (BSSID)
    for (int i = 0; i < 6; i++) f[n++] = 0xBB;    // addr3
    f[n++] = 0; f[n++] = 0;                        // seq  -> n == 24
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
    memcpy(f + n, snap, 8); n += 8;
    f[n++] = 0x02; f[n++] = 0x03; f[n++] = 0x00; f[n++] = 0x5F;   // EAPOL ver2 type3 len
    f[n++] = 0x02;                                // descriptor type
    f[n++] = 0x00; f[n++] = 0x8A;                 // key info: M1 (ack set, no mic/install/secure)
    f[n++] = 0x00; f[n++] = 0x10;                 // key length 16
    memset(f + n, 0, 88); n += 88;                // replay(8)+nonce(32)+iv(16)+rsc(8)+id(8)+mic(16)
    if (with_pmkid) {
        f[n++] = 0x00; f[n++] = 0x16;             // key data length = 22
        f[n++] = 0xDD; f[n++] = 0x14;             // KDE: type 0xDD, len 0x14 (20)
        f[n++] = 0x00; f[n++] = 0x0F; f[n++] = 0xAC; f[n++] = 0x04;   // OUI 00-0F-AC, data type 04 = PMKID
        for (int k = 0; k < 16; k++) f[n++] = (uint8_t)(0xA0 + k);    // the PMKID
    } else {
        f[n++] = 0x00; f[n++] = 0x00;             // no key data
    }
    return n;
}

int main(void)
{
    uint8_t f[256];
    printf("== nucleo_wifiatk_eapol host test ==\n");

    CHECK(wifiatk_eapol_msg(f, build_eapol(f, 0x08, 0x0089)) == 1, "M1 (ack)");
    CHECK(wifiatk_eapol_msg(f, build_eapol(f, 0x08, 0x0109)) == 2, "M2 (mic)");
    CHECK(wifiatk_eapol_msg(f, build_eapol(f, 0x08, 0x01C9)) == 3, "M3 (ack+mic+install)");
    CHECK(wifiatk_eapol_msg(f, build_eapol(f, 0x08, 0x0309)) == 4, "M4 (mic+secure)");
    // QoS-data header shift still parses.
    CHECK(wifiatk_eapol_msg(f, build_eapol(f, 0x88, 0x0109)) == 2, "QoS M2");
    // Not a data frame (beacon, type 0) -> 0.
    {
        int len = build_eapol(f, 0x08, 0x0109); f[0] = 0x80;
        CHECK(wifiatk_eapol_msg(f, len) == 0, "non-data rejected");
    }
    // Data frame but not EAPOL (break the EtherType) -> 0.
    {
        int len = build_eapol(f, 0x08, 0x0109); f[24 + 7] = 0x00;   // 0x8E -> 0x00
        CHECK(wifiatk_eapol_msg(f, len) == 0, "non-eapol rejected");
    }
    // Truncated frame -> 0.
    CHECK(wifiatk_eapol_msg(f, 20) == 0, "short frame rejected");

    // PMKID: a real M1 frame is classified as message 1 and the PMKID is extracted.
    {
        uint8_t pm[16];
        int len = build_m1(f, 1);
        CHECK(wifiatk_eapol_msg(f, len) == 1, "M1 frame classified");
        CHECK(wifiatk_eapol_pmkid(f, len, pm) == 1, "pmkid extracted");
        int ok = 1; for (int k = 0; k < 16; k++) if (pm[k] != (uint8_t)(0xA0 + k)) ok = 0;
        CHECK(ok, "pmkid bytes correct");
    }
    // M1 without a PMKID KDE -> no PMKID.
    {
        uint8_t pm[16];
        int len = build_m1(f, 0);
        CHECK(wifiatk_eapol_pmkid(f, len, pm) == 0, "no pmkid when absent");
    }
    // A non-M1 EAPOL (M2) never yields a PMKID.
    {
        uint8_t pm[16];
        CHECK(wifiatk_eapol_pmkid(f, build_eapol(f, 0x08, 0x0109), pm) == 0, "no pmkid on M2");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
