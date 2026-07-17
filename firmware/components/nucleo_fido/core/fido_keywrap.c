#include "fido_keywrap.h"
#include <string.h>

int fido_kw_wrap(const fido_crypto_t *cy, const uint8_t devkey[32],
                 const uint8_t priv[32], const uint8_t rpIdHash[32],
                 uint8_t *handle, size_t *handle_len) {
    uint8_t iv[12];
    if (cy->rand(iv, 12, cy->ctx)) return -1;
    uint8_t tag[16], ct[32];
    if (cy->aes_gcm_seal(devkey, iv, rpIdHash, 32, priv, 32, ct, tag, cy->ctx)) return -1;
    memcpy(handle, iv, 12);
    memcpy(handle + 12, tag, 16);
    memcpy(handle + 28, ct, 32);
    *handle_len = FIDO_KW_HANDLE_LEN;
    return 0;
}

int fido_kw_unwrap(const fido_crypto_t *cy, const uint8_t devkey[32],
                   const uint8_t *handle, size_t handle_len,
                   const uint8_t rpIdHash[32], uint8_t priv[32]) {
    if (handle_len != FIDO_KW_HANDLE_LEN) return -1;
    const uint8_t *iv = handle, *tag = handle + 12, *ct = handle + 28;
    return cy->aes_gcm_open(devkey, iv, rpIdHash, 32, ct, 32, tag, priv, cy->ctx);
}
