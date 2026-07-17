// fido_cose — encode a P-256 public key as a COSE_Key map (RFC 8152), the form
// WebAuthn embeds in attestedCredentialData.
#pragma once
#include "cbor.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// pub[65] = 0x04 || X(32) || Y(32). Writes a COSE_Key (kty EC2, alg ES256,
// crv P-256, x, y) into out[cap]; returns bytes written (0 on overflow).
size_t fido_cose_es256(const uint8_t pub[65], uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
