// fido_pin — on-device PIN verifier for INTERNAL user verification.
//
// The innovation over clientPIN-over-USB: the PIN is entered on the Cardputer and
// only a verifier leaves this module — never the PIN, and never over USB. The
// verifier is HMAC(wrapping-key, salt || pin) with the wrapping key derived from
// the S3 eFuse (fido_efuse_key), so a flash dump can't brute-force the PIN offline
// without the hardware. A retry counter locks the key after too many wrong tries.
#pragma once
#include "fido_crypto.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIDO_PIN_RETRIES 8
#define FIDO_PIN_MIN     4      // CTAP minimum: 4 code points
#define FIDO_PIN_MAX     63

typedef struct {
    uint8_t set;               // 1 once a PIN is configured
    uint8_t retries;           // tries left before lockout
    uint8_t salt[16];
    uint8_t verifier[32];      // HMAC(key, salt || pin)
} fido_pin_state;

void fido_pin_init(fido_pin_state *st);   // unset, retries full

// Configure / change the PIN. Returns 0 on success, -1 on bad length or crypto.
int  fido_pin_set(fido_pin_state *st, const fido_crypto_t *cy, const uint8_t key[32], const char *pin);

// Verify a PIN attempt. 1 = correct (retries reset), 0 = wrong (retries--),
// -1 = locked out (retries == 0), -2 = no PIN set.
int  fido_pin_check(fido_pin_state *st, const fido_crypto_t *cy, const uint8_t key[32], const char *pin);

bool fido_pin_is_set(const fido_pin_state *st);
int  fido_pin_retries(const fido_pin_state *st);

#ifdef __cplusplus
}
#endif
