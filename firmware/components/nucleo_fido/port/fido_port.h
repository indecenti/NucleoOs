// fido_port.h — internal glue declarations shared between the port .c files.
#pragma once
#include "fido_crypto.h"
#include "fido_cred_store.h"
#include "fido_pin.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// mbedTLS-backed crypto vtable (deterministic ECDSA per RFC 6979).
const fido_crypto_t *fido_mbedtls_crypto(void);

// Derive the 32-byte credential wrapping key. Prefers the S3 eFuse HMAC key
// block (hardware, never in flash); falls back to a random NVS-stored key when
// no HMAC key is burned. *hardware reports which path was used. Never burns eFuse.
int fido_derive_devkey(uint8_t out[32], bool *hardware);

// NVS-backed resident-credential store (namespace "fidocred").
fido_cred_store *fido_cred_store_nvs(void);

// Persisted global signature counter (for stateless credentials).
uint32_t fido_counter_load(void);
void     fido_counter_store(uint32_t v);

// Persisted on-device PIN verifier state (NVS namespace "fido", key "pin").
bool fido_pin_load(fido_pin_state *st);
void fido_pin_store(const fido_pin_state *st);
void fido_pin_erase(void);

// FIDO USB personality boot flag (RTC_NOINIT).
bool fido_boot_key_mode(void);
void fido_request_key_mode(bool on);   // sets the flag and reboots

#ifdef __cplusplus
}
#endif
