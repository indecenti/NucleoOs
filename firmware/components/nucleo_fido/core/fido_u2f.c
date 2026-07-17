#include "fido_u2f.h"
#include "fido_keywrap.h"
#include <string.h>

static uint16_t sw(uint8_t *out, uint16_t n, uint16_t code) {
    out[n] = code >> 8; out[n + 1] = code & 0xff; return n + 2;
}

uint16_t fido_u2f_handle(const fido_u2f_cfg *cfg, const uint8_t *apdu, uint16_t len,
                         uint8_t *out, uint16_t cap) {
    if (cap < 2) return 0;                                // no room for a status word
    if (len < 4) return sw(out, 0, 0x6A80);
    uint8_t ins = apdu[1];

    if (ins == 0x03) {                                    // VERSION
        if (cap < 8) return sw(out, 0, 0x6700);
        memcpy(out, "U2F_V2", 6); return sw(out, 6, 0x9000);
    }

    // Data-bearing INS carry an extended-length APDU: a 3-byte Lc must be present
    // and must not point past the packet (guards a short frame with a huge Lc).
    if (len < 7) return sw(out, 0, 0x6700);
    uint32_t lc = (uint32_t)apdu[4] << 16 | (uint32_t)apdu[5] << 8 | apdu[6];
    if (lc > (uint32_t)(len - 7)) return sw(out, 0, 0x6700);
    const uint8_t *data = apdu + 7;

    if (ins == 0x01) {                                    // REGISTER
        if (lc < 64) return sw(out, 0, 0x6A80);
        const uint8_t *chal = data, *appid = data + 32;
        if (!cfg->user_present(cfg->ui)) return sw(out, 0, 0x6985);
        uint8_t priv[32], pub[65];
        if (cfg->cy->p256_keygen(priv, pub, cfg->cy->ctx)) return sw(out, 0, 0x6A80);
        uint8_t handle[FIDO_KW_HANDLE_LEN]; size_t hl = 0;
        if (fido_kw_wrap(cfg->cy, cfg->devkey, priv, appid, handle, &hl)) return sw(out, 0, 0x6A80);
        // Verify the full response (worst-case 72-byte sig) fits before writing.
        uint32_t need = 1u + 65u + 1u + (uint32_t)hl + cfg->att_cert_len + 72u + 2u;
        if (need > cap) return sw(out, 0, 0x6700);
        // Sign: 0x00 || appid(32) || chal(32) || handle || pub(65)  with att key.
        uint8_t msg[1 + 32 + 32 + FIDO_KW_HANDLE_LEN + 65]; uint16_t m = 0;
        msg[m++] = 0x00; memcpy(msg + m, appid, 32); m += 32; memcpy(msg + m, chal, 32); m += 32;
        memcpy(msg + m, handle, hl); m += (uint16_t)hl; memcpy(msg + m, pub, 65); m += 65;
        uint8_t sig[72]; size_t sl = 0;
        if (cfg->cy->p256_sign(cfg->att_priv, msg, m, sig, &sl, cfg->cy->ctx)) return sw(out, 0, 0x6A80);
        uint16_t n = 0;
        out[n++] = 0x05; memcpy(out + n, pub, 65); n += 65;
        out[n++] = (uint8_t)hl; memcpy(out + n, handle, hl); n += (uint16_t)hl;
        memcpy(out + n, cfg->att_cert, cfg->att_cert_len); n += cfg->att_cert_len;
        memcpy(out + n, sig, sl); n += (uint16_t)sl;
        return sw(out, n, 0x9000);
    }
    if (ins == 0x02) {                                    // AUTHENTICATE
        if (lc < 65) return sw(out, 0, 0x6A80);
        const uint8_t *chal = data, *appid = data + 32;
        uint8_t khl = data[64]; const uint8_t *handle = data + 65;
        if ((uint32_t)65 + khl > lc) return sw(out, 0, 0x6A80);
        uint8_t p1 = apdu[2];
        uint8_t priv[32];
        if (fido_kw_unwrap(cfg->cy, cfg->devkey, handle, khl, appid, priv))
            return sw(out, 0, 0x6A80);                    // not ours / wrong app
        if (p1 == 0x07) return sw(out, 0, 0x6985);        // check-only: signal "present"
        if (77u + 2u > cap) return sw(out, 0, 0x6700);    // up(1)+ctr(4)+sig(<=72)+sw(2)
        if (!cfg->user_present(cfg->ui)) return sw(out, 0, 0x6985);
        if (*cfg->counter == 0xFFFFFFFFu) return sw(out, 0, 0x6A80); // refuse to wrap the counter
        uint32_t ctr = ++(*cfg->counter);
        uint8_t up = 0x01;
        uint8_t ctrb[4] = { (uint8_t)(ctr >> 24), (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr };
        // Sign: appid(32) || up(1) || counter(4) || chal(32)  with credential key.
        uint8_t msg[32 + 1 + 4 + 32]; uint16_t m = 0;
        memcpy(msg + m, appid, 32); m += 32; msg[m++] = up;
        memcpy(msg + m, ctrb, 4); m += 4; memcpy(msg + m, chal, 32); m += 32;
        uint8_t sig[72]; size_t sl = 0;
        if (cfg->cy->p256_sign(priv, msg, m, sig, &sl, cfg->cy->ctx)) return sw(out, 0, 0x6A80);
        uint16_t n = 0; out[n++] = up; memcpy(out + n, ctrb, 4); n += 4; memcpy(out + n, sig, sl); n += (uint16_t)sl;
        return sw(out, n, 0x9000);
    }
    return sw(out, 0, 0x6D00);                            // INS not supported
}
