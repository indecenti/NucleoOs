#include "fido_authdata.h"
#include <string.h>

size_t fido_att_cred_data(const uint8_t aaguid[16],
                          const uint8_t *credId, uint16_t credIdLen,
                          const uint8_t *cosePub, size_t coseLen,
                          uint8_t *out, size_t cap) {
    size_t need = 16u + 2u + credIdLen + coseLen;
    if (need > cap) return 0;
    memcpy(out, aaguid, 16);
    out[16] = (uint8_t)(credIdLen >> 8);
    out[17] = (uint8_t)credIdLen;
    memcpy(out + 18, credId, credIdLen);
    memcpy(out + 18 + credIdLen, cosePub, coseLen);
    return need;
}

size_t fido_authdata_build(const uint8_t rpIdHash[32], uint8_t flags, uint32_t signCount,
                           const uint8_t *attCredData, size_t attLen,
                           uint8_t *out, size_t cap) {
    size_t need = 32u + 1u + 4u + attLen;
    if (need > cap) return 0;
    memcpy(out, rpIdHash, 32);
    out[32] = flags;
    out[33] = (uint8_t)(signCount >> 24);
    out[34] = (uint8_t)(signCount >> 16);
    out[35] = (uint8_t)(signCount >> 8);
    out[36] = (uint8_t)signCount;
    if (attLen) memcpy(out + 37, attCredData, attLen);
    return need;
}
