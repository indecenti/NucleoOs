#include "sentinel_wifi.h"
#include <string.h>

// 802.11 frame control: byte0 = protocol(2) | type(2) | subtype(4).
#define FC_TYPE(b0)    (((b0) >> 2) & 0x3)
#define FC_SUBTYPE(b0) (((b0) >> 4) & 0xF)
#define TYPE_MGMT 0x0

static int is_bcast(const uint8_t *a) {
    for (int i = 0; i < 6; i++) if (a[i] != 0xFF) return 0;
    return 1;
}

int sentinel_wifi_parse(const uint8_t *f, size_t len, sentinel_wifi_frame *out) {
    memset(out, 0, sizeof *out);
    if (len < 24) return 0;                          // shorter than a mgmt header
    uint8_t b0 = f[0];
    if (FC_TYPE(b0) != TYPE_MGMT) return 0;
    uint8_t st = FC_SUBTYPE(b0);
    const uint8_t *addr1 = f + 4;                    // DA/RA
    const uint8_t *addr3 = f + 16;                   // BSSID
    out->to_broadcast = (uint8_t)is_bcast(addr1);
    memcpy(out->bssid, addr3, 6);

    switch (st) {
        case 0x0C: out->kind = SENTINEL_WF_DEAUTH;   return 1;
        case 0x0A: out->kind = SENTINEL_WF_DISASSOC; return 1;
        case 0x04: out->kind = SENTINEL_WF_PROBE_REQ;return 1;
        case 0x08: {                                 // beacon
            out->kind = SENTINEL_WF_BEACON;
            // Fixed params after the 24-byte header: timestamp(8)+interval(2)+cap(2)=12.
            size_t p = 24 + 12;
            while (p + 2 <= len) {                    // tagged parameters
                uint8_t tag = f[p], tl = f[p + 1];
                if (p + 2 + tl > len) break;
                if (tag == 0x00) {                    // SSID
                    uint8_t n = tl < 32 ? tl : 32;
                    memcpy(out->ssid, f + p + 2, n);
                    out->ssid[n] = 0;
                    break;
                }
                p += 2 + tl;
            }
            return 1;
        }
        default: out->kind = SENTINEL_WF_OTHER; return 0;
    }
}

void sentinel_wifi_mon_init(sentinel_wifi_mon *m, uint32_t window_s,
                            uint16_t deauth_thresh, uint16_t beacon_thresh) {
    memset(m, 0, sizeof *m);
    m->window_s = window_s ? window_s : 10;
    m->deauth_thresh = deauth_thresh ? deauth_thresh : 8;
    m->beacon_thresh = beacon_thresh ? beacon_thresh : 40;
}

static void roll_window(sentinel_wifi_mon *m, uint32_t now) {
    if (now - m->win_start >= m->window_s) {
        m->win_start = now;
        m->deauth_ct = 0;
        m->beacon_ct = 0;
    }
}

uint8_t sentinel_wifi_mon_feed(sentinel_wifi_mon *m, const sentinel_wifi_frame *fr, uint32_t now) {
    if (m->win_start == 0) m->win_start = now;
    roll_window(m, now);
    uint8_t raised = 0;

    if (fr->kind == SENTINEL_WF_DEAUTH || fr->kind == SENTINEL_WF_DISASSOC) {
        if (fr->to_broadcast && !(m->alerts & SENTINEL_A_BROADCAST_DEAUTH)) {
            m->alerts |= SENTINEL_A_BROADCAST_DEAUTH; raised |= SENTINEL_A_BROADCAST_DEAUTH;
        }
        if (m->deauth_ct < 0xFFFF) m->deauth_ct++;
        if (m->deauth_ct >= m->deauth_thresh && !(m->alerts & SENTINEL_A_DEAUTH_FLOOD)) {
            m->alerts |= SENTINEL_A_DEAUTH_FLOOD; raised |= SENTINEL_A_DEAUTH_FLOOD;
        }
    } else if (fr->kind == SENTINEL_WF_BEACON) {
        if (m->beacon_ct < 0xFFFF) m->beacon_ct++;
        if (m->beacon_ct >= m->beacon_thresh && !(m->alerts & SENTINEL_A_BEACON_FLOOD)) {
            m->alerts |= SENTINEL_A_BEACON_FLOOD; raised |= SENTINEL_A_BEACON_FLOOD;
        }
        // Evil twin: a known SSID reappearing from a different BSSID.
        if (fr->ssid[0]) {
            int slot = -1;
            for (int i = 0; i < m->nseen; i++) {
                if (strncmp(m->seen[i].ssid, fr->ssid, 33) == 0) { slot = i; break; }
            }
            if (slot >= 0) {
                if (memcmp(m->seen[slot].bssid, fr->bssid, 6) != 0 &&
                    !(m->alerts & SENTINEL_A_EVIL_TWIN)) {
                    m->alerts |= SENTINEL_A_EVIL_TWIN; raised |= SENTINEL_A_EVIL_TWIN;
                }
            } else if (m->nseen < SENTINEL_SSID_TABLE) {
                slot = m->nseen++;
                strncpy(m->seen[slot].ssid, fr->ssid, 32);
                m->seen[slot].ssid[32] = 0;
                memcpy(m->seen[slot].bssid, fr->bssid, 6);
            }
        }
    }
    return raised;
}
