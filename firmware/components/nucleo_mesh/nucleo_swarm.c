// nucleo_swarm — pure SANS-I/O swarm membership/join. Composes nucleo_swarm_sec. No esp_*/mbedtls
// (the KDF + HMAC are injected). Host-tested.
#include "nucleo_swarm.h"
#include <string.h>

void swarm_member_init(swarm_member_t *m)
{
    memset(m, 0, sizeof(*m));
    sw_pin_reset(&m->pins, false);     // open: no MAC enforcement until the user pins
}

bool swarm_is_open(const swarm_member_t *m) { return !m->have_psk; }

int swarm_join(swarm_member_t *m, const char *passphrase, const uint8_t *salt, int slen,
               sw_kdf_fn kdf, void *u)
{
    m->have_psk = false;
    memset(m->psk, 0, SW_PSK_LEN);
    if (!passphrase) return 0;                                   // cleared -> open
    int pl = (int)strlen(passphrase);
    if (pl < SW_PASS_MIN) return 0;                              // too short -> open (honest default)
    if (pl > SW_PASS_MAX || !kdf) return -1;                     // out of policy / no KDF -> fail-safe open
    if (kdf(u, passphrase, pl, salt, slen, m->psk) != 0) { memset(m->psk, 0, SW_PSK_LEN); return -1; }
    m->have_psk = true;
    return 1;
}

int swarm_admit(const swarm_member_t *m, const uint8_t *mac6, const uint8_t *buf, int len,
                sw_mac_fn mac, void *u)
{
    if (sw_demux(buf, len) != SW_KIND_MESH) return -1;           // only mesh frames are swarm traffic
    if (m->pins.enforce && (!mac6 || !sw_pin_allowed(&m->pins, mac6))) return -1;
    if (!m->have_psk) return len;                                // OPEN: whole frame is the payload
    return sw_open(m->psk, SW_PSK_LEN, buf, len, mac, u);        // CLOSED: verify HMAC + strip tag
}

int swarm_seal_out(const swarm_member_t *m, uint8_t *buf, int len, int cap, sw_mac_fn mac, void *u)
{
    if (!m->have_psk) return len;                                // OPEN: send as-is
    return sw_seal(m->psk, SW_PSK_LEN, buf, len, cap, mac, u);   // CLOSED: append HMAC tag
}
