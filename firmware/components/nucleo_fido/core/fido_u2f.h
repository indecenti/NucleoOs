// fido_u2f — CTAP1/U2F (FIDO v1) REGISTER + AUTHENTICATE over the raw message
// channel. Browsers still accept a U2F authenticator as a second factor, so this
// is the widest-compatibility path. Credentials are stateless keywrap handles.
#pragma once
#include "fido_crypto.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const fido_crypto_t *cy;
    const uint8_t       *devkey;        // 32-byte wrapping key
    const uint8_t       *att_cert;      // DER attestation cert (batch identity)
    uint16_t             att_cert_len;
    const uint8_t       *att_priv;      // 32-byte attestation private key
    uint32_t            *counter;       // global signature counter (persisted by caller)
    int  (*user_present)(void *ui);     // block for a physical approval; 0 = denied
    void                *ui;
} fido_u2f_cfg;

// Handle one U2F request APDU (apdu[len]); write the response (data + 2-byte SW)
// into out[cap]. Returns the response length.
uint16_t fido_u2f_handle(const fido_u2f_cfg *cfg, const uint8_t *apdu, uint16_t len,
                         uint8_t *out, uint16_t cap);

#ifdef __cplusplus
}
#endif
