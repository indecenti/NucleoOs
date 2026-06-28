// nucleo_swarm_sec — pure SANS-I/O swarm security gate. No esp_*/mbedtls here: the hash fn is
// injected (mbedtls HMAC-SHA256 on device, a mock in the host gate). Host-tested.
#include "nucleo_swarm_sec.h"
#include <string.h>

int sw_demux(const uint8_t *buf, int len)
{
    if (!buf || len < 2) return SW_KIND_UNKNOWN;
    if (buf[0] == 'N' && buf[1] == 'L') return SW_KIND_LINK;
    if (buf[0] == 'N' && buf[1] == 'M') return SW_KIND_MESH;
    return SW_KIND_UNKNOWN;
}

int sw_seal(const uint8_t *key, int klen, uint8_t *buf, int len, int cap, sw_mac_fn mac, void *u)
{
    // Non-overflowing bounds check: `len + SW_TAG_LEN > cap` can overflow signed int for a huge len
    // and wrongly pass. cap is a real buffer size (>= 0), so `len > cap - SW_TAG_LEN` is safe.
    if (!buf || !mac || len < 0 || len > cap - SW_TAG_LEN) return -1;
    uint8_t t[SW_MAC_OUT];
    if (mac(u, key, klen, buf, len, t) != 0) return -1;
    memcpy(buf + len, t, SW_TAG_LEN);
    return len + SW_TAG_LEN;
}

// Constant-time equality: fold all byte diffs into one accumulator, branch only on the final result.
static int ct_eq(const uint8_t *a, const uint8_t *b, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

int sw_open(const uint8_t *key, int klen, const uint8_t *buf, int len, sw_mac_fn mac, void *u)
{
    if (!buf || !mac || len <= SW_TAG_LEN) return -1;
    int plen = len - SW_TAG_LEN;
    uint8_t t[SW_MAC_OUT];
    if (mac(u, key, klen, buf, plen, t) != 0) return -1;
    if (!ct_eq(t, buf + plen, SW_TAG_LEN)) return -1;
    return plen;
}

void sw_pin_reset(sw_pins_t *p, bool enforce) { p->n = 0; p->enforce = enforce; }

bool sw_pin_add(sw_pins_t *p, const uint8_t mac[6])
{
    for (int i = 0; i < p->n; i++) if (!memcmp(p->mac[i], mac, 6)) return true;  // already pinned
    if (p->n >= SW_PINS) return false;
    memcpy(p->mac[p->n++], mac, 6);
    return true;
}

bool sw_pin_allowed(const sw_pins_t *p, const uint8_t mac[6])
{
    if (!p->enforce) return true;
    for (int i = 0; i < p->n; i++) if (!memcmp(p->mac[i], mac, 6)) return true;
    return false;
}
