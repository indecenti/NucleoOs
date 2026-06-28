// Pure EAPOL-Key / 4-way-handshake classifier + PMKID extractor. See header. No ESP-IDF includes.
#include "nucleo_wifiatk_eapol.h"

// Locate the EAPOL header inside an 802.11 data MPDU; returns its byte offset, or -1 if the frame is
// not an EAPOL-Key data frame. Shared by the classifier and the PMKID extractor.
static int eapol_off(const uint8_t *f, int len)
{
    if (!f || len < 24) return -1;
    if (((f[0] >> 2) & 0x3) != 0x2) return -1;        // not a DATA frame (type 2)
    int hdr = 24;
    if ((f[0] & 0xF0) == 0x80) hdr += 2;              // QoS data subtype -> +2 (QoS control)
    if (len < hdr + 8 + 4) return -1;
    const uint8_t *llc = f + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
          llc[6] == 0x88 && llc[7] == 0x8E))           // EtherType 0x888E = EAPOL
        return -1;
    int eo = hdr + 8;
    if (len < eo + 7) return -1;
    if (f[eo + 1] != 0x03) return -1;                 // EAPOL packet type 3 = EAPOL-Key
    return eo;
}

int wifiatk_eapol_msg(const uint8_t *f, int len)
{
    int eo = eapol_off(f, len);
    if (eo < 0) return 0;
    uint16_t ki = (uint16_t)((f[eo + 5] << 8) | f[eo + 6]);   // Key Information
    int install = (ki & (1 << 6)) != 0;
    int ack     = (ki & (1 << 7)) != 0;
    int mic     = (ki & (1 << 8)) != 0;
    int secure  = (ki & (1 << 9)) != 0;
    if ( ack && !mic && !install)            return 1;   // M1: AP->STA, ANonce, no MIC
    if (!ack &&  mic && !install && !secure) return 2;   // M2: STA->AP, SNonce, MIC
    if ( ack &&  mic &&  install)            return 3;   // M3: AP->STA, GTK, MIC, install
    if (!ack &&  mic && !install &&  secure) return 4;   // M4: STA->AP, MIC, secure
    return 0;
}

int wifiatk_eapol_pmkid(const uint8_t *f, int len, uint8_t out[16])
{
    if (wifiatk_eapol_msg(f, len) != 1) return 0;     // PMKID rides in message 1's key data
    int eo = eapol_off(f, len);
    if (eo < 0) return 0;
    int body = eo + 4;                                 // EAPOL-Key body (descriptor type byte)
    // key data length is at body+93; key data follows at body+95.
    if (len < body + 95) return 0;
    int kdlen = (f[body + 93] << 8) | f[body + 94];
    int kd = body + 95;
    if (kd + kdlen > len) kdlen = len - kd;
    if (kdlen < 0) return 0;
    // Scan the key-data KDEs for the RSN PMKID KDE: type 0xDD, OUI 00-0F-AC, data type 0x04, 16 bytes.
    int i = 0;
    while (i + 2 <= kdlen) {
        uint8_t t = f[kd + i], l = f[kd + i + 1];
        if (i + 2 + l > kdlen) break;
        if (t == 0xDD && l >= 20 &&
            f[kd + i + 2] == 0x00 && f[kd + i + 3] == 0x0F &&
            f[kd + i + 4] == 0xAC && f[kd + i + 5] == 0x04) {
            for (int k = 0; k < 16; k++) out[k] = f[kd + i + 6 + k];
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}
