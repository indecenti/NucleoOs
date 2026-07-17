// fido_ctap2 — the CTAP2 authenticator command layer (the FIDO2 brain).
//
// P1 implements authenticatorGetInfo / MakeCredential / GetAssertion. The config
// carries hooks for internal user verification (user_verify) and a resident
// store so later phases can add getNextAssertion, reset, credentialManagement and
// the hmac-secret extension without reshaping callers.
#pragma once
#include "fido_crypto.h"
#include "fido_cred_store.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CTAP2 command bytes (first byte of a CBOR request).
enum {
    FIDO_CTAP2_MAKE_CRED       = 0x01,
    FIDO_CTAP2_GET_ASSERT      = 0x02,
    FIDO_CTAP2_GET_INFO        = 0x04,
    FIDO_CTAP2_CLIENT_PIN      = 0x06,
    FIDO_CTAP2_RESET           = 0x07,
    FIDO_CTAP2_GET_NEXT_ASSERT = 0x08,
    FIDO_CTAP2_CRED_MGMT       = 0x0A,
    FIDO_CTAP2_SELECTION       = 0x0B,
};

typedef struct {
    const fido_crypto_t *cy;
    const uint8_t       *devkey;          // 32-byte wrapping key
    const uint8_t       *aaguid;          // 16-byte authenticator model id
    uint32_t            *counter;         // global counter for non-resident creds
    fido_cred_store     *store;           // resident (discoverable) credentials

    // User presence: block for a physical approval (button). The relying-party id
    // is passed so the device can SHOW who is asking to sign in — a real security
    // advantage of a screen+keyboard authenticator. 0 = denied.
    int  (*user_present)(void *ui, const char *rp);
    // Internal user verification: block for the on-device PIN. Returns 1 if the
    // user verified. NULL until wired — when NULL the authenticator reports no UV.
    int  (*user_verify)(void *ui, const char *rp);
    // authenticatorReset side-effects the core can't do itself (clear the device
    // PIN, etc.). Optional. The core already wipes the resident store + counter.
    void (*on_reset)(void *ui);
    // True once an on-device PIN/UV is configured (advertised in getInfo).
    bool  uv_configured;
    void *ui;
} fido_ctap2_cfg;

// Handle a CTAP2 request (req[0] = command). Writes status byte + CBOR response
// into out[cap]; returns the total response length (>=1).
uint16_t fido_ctap2_handle(const fido_ctap2_cfg *cfg, const uint8_t *req, uint16_t len,
                           uint8_t *out, uint16_t cap);

#ifdef __cplusplus
}
#endif
