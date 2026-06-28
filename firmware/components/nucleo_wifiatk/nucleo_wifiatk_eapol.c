// Pure EAPOL-Key / 4-way-handshake classifier. See nucleo_wifiatk_eapol.h. No ESP-IDF includes.
#include "nucleo_wifiatk_eapol.h"

int wifiatk_eapol_msg(const uint8_t *f, int len)
{
    if (!f || len < 24) return 0;

    const uint8_t fc0 = f[0];
    if (((fc0 >> 2) & 0x3) != 0x2) return 0;     // not a DATA frame (type 2)

    int hdr = 24;                                 // base 802.11 MAC header (3-address)
    if ((fc0 & 0xF0) == 0x80) hdr += 2;           // QoS data subtype (0x08) -> +2 for the QoS control

    // LLC/SNAP + EtherType: AA AA 03 00 00 00 88 8E  (88 8E = EAPOL)
    if (len < hdr + 8 + 4) return 0;
    const uint8_t *llc = f + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
          llc[6] == 0x88 && llc[7] == 0x8E))
        return 0;

    const uint8_t *eapol = llc + 8;               // EAPOL: version(1) type(1) length(2) [body...]
    int eoff = (int)(eapol - f);
    if (len < eoff + 7) return 0;
    if (eapol[1] != 0x03) return 0;               // EAPOL packet type 3 = EAPOL-Key

    // EAPOL-Key body: descriptor type(1) then Key Information(2). Key Info starts at eapol+5.
    uint16_t ki = (uint16_t)((eapol[5] << 8) | eapol[6]);
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
