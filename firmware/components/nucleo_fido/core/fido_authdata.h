// fido_authdata — assemble the WebAuthn authenticatorData structure and its
// attestedCredentialData sub-block (byte-exact, big-endian per spec).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// authenticatorData flag bits (WebAuthn §6.1).
#define FIDO_AD_UP 0x01   // user present
#define FIDO_AD_UV 0x04   // user verified
#define FIDO_AD_AT 0x40   // attestedCredentialData included
#define FIDO_AD_ED 0x80   // extension data included

// aaguid(16) || credIdLen(2 BE) || credId || COSE pubkey. Returns bytes written.
size_t fido_att_cred_data(const uint8_t aaguid[16],
                          const uint8_t *credId, uint16_t credIdLen,
                          const uint8_t *cosePub, size_t coseLen,
                          uint8_t *out, size_t cap);

// rpIdHash(32) || flags(1) || signCount(4 BE) || [attCredData]. Returns bytes.
size_t fido_authdata_build(const uint8_t rpIdHash[32], uint8_t flags, uint32_t signCount,
                           const uint8_t *attCredData, size_t attLen,
                           uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
