// fido_keywrap — stateless credential handles (the Yubico "key wrapping" design).
//
// A credential's private key is AES-256-GCM sealed under the device wrapping key,
// with the RP's id-hash as additional authenticated data. The sealed blob IS the
// credential id, so a non-resident credential needs zero on-device storage and a
// handle only decrypts under the RP it was minted for. On device the wrapping key
// is derived from the S3 eFuse HMAC block (port/fido_efuse_key.c) so it never
// exists in flash.
#pragma once
#include "fido_crypto.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// handle = iv(12) || tag(16) || ciphertext(32)
#define FIDO_KW_HANDLE_LEN 60

// Seal priv[32] bound to rpIdHash[32] into handle[FIDO_KW_HANDLE_LEN].
int fido_kw_wrap(const fido_crypto_t *cy, const uint8_t devkey[32],
                 const uint8_t priv[32], const uint8_t rpIdHash[32],
                 uint8_t *handle, size_t *handle_len);

// Verify+decrypt a handle back to priv[32]. Non-zero if it is not ours, was
// tampered with, or was minted for a different RP.
int fido_kw_unwrap(const fido_crypto_t *cy, const uint8_t devkey[32],
                   const uint8_t *handle, size_t handle_len,
                   const uint8_t rpIdHash[32], uint8_t priv[32]);

#ifdef __cplusplus
}
#endif
