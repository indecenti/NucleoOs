// Pure probe-request SSID parser. See nucleo_wifiatk_probe.h. No ESP-IDF includes on purpose.
#include "nucleo_wifiatk_probe.h"

int wifiatk_probe_ssid(const uint8_t *f, int len, char *out, int cap)
{
    if (!f || !out || cap < 2 || len < 24) return -1;
    // Frame Control byte 0: protocol=00, type=00 (management), subtype=0100 (probe request) => 0x40.
    if (f[0] != 0x40) return -1;

    // Management body (no HT control on a probe request) begins at offset 24; the first information
    // element should be the SSID (tag 0). Walk the IE list defensively in case it isn't first.
    const uint8_t *ie = f + 24;
    int rem = len - 24;
    while (rem >= 2) {
        int tag = ie[0], tlen = ie[1];
        if (tlen + 2 > rem) return -1;             // element overruns the frame -> malformed
        if (tag == 0) {                            // SSID element
            if (tlen == 0 || tlen > 32) return -1; // wildcard probe / illegal length
            if (tlen > cap - 1) return -1;
            for (int i = 0; i < tlen; i++) {
                char c = (char)ie[2 + i];
                if (c < 32 || c > 126) return -1;  // non-printable -> hidden/garbage, skip
                out[i] = c;
            }
            out[tlen] = 0;
            return tlen;
        }
        ie += tlen + 2;
        rem -= tlen + 2;
    }
    return -1;                                      // no SSID element present
}
