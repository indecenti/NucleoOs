// Pure WPA 4-way-handshake (EAPOL-Key) classifier — no ESP-IDF, no hardware, so it compiles into both
// the firmware (handshake capture) and the host gate (eapol-ctest.c). Clean-room from IEEE 802.11i /
// 802.1X: the LLC/SNAP EtherType test and the EAPOL-Key Key-Information bit logic are the published
// standard, not copied from any GPL firmware. FOR AUTHORIZED TESTING ONLY — byte parsing only.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Given a full 802.11 MPDU (as delivered by promiscuous mode), return the 4-way-handshake message
// number (1..4) if it is an EAPOL-Key frame, or 0 otherwise. Handles QoS-data header shift. A pair
// {1,2} or {3,4} for the same AP is a crackable handshake.
int wifiatk_eapol_msg(const uint8_t *frame, int len);

// If `frame` is EAPOL-Key message 1 carrying an RSN PMKID KDE, copy the 16-byte PMKID into `out` and
// return 1; else 0. A PMKID is clientless crackable material (hashcat -m 16800 / 22000).
int wifiatk_eapol_pmkid(const uint8_t *frame, int len, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
