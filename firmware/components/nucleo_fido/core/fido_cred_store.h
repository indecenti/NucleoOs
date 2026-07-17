// fido_cred_store — persistence interface for resident (discoverable) credentials.
//
// Non-resident credentials need no storage (the handle is the key). Resident
// credentials — the ones that power usernameless passkey login — must be kept:
// their private key stays keywrap-sealed inside the record, but the record also
// carries the user handle/name so getAssertion can return an account with an
// empty allowList. The core talks only to this vtable; the device fills it with
// an NVS backend (port/fido_store_nvs.c) and the host gate with a RAM backend.
//
// Beyond Poseidon's add/find/update this interface also exposes remove() and
// wipe_all() so authenticatorReset and credentialManagement can actually delete.
#pragma once
#include "fido_keywrap.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  id[32];                        // credential id (random for resident)
    uint8_t  rpIdHash[32];
    uint8_t  wrappedKey[FIDO_KW_HANDLE_LEN];// sealed private key
    uint8_t  userId[64];
    uint8_t  userIdLen;
    uint8_t  credProtect;                   // 0 = unset, else CTAP2.1 policy 1..3
    uint32_t signCount;
    char     userName[65];                  // optional, for the credential manager
    char     rpId[65];                      // optional, for the credential manager
} fido_cred_record;

typedef struct fido_cred_store_s {
    // Insert (or replace an existing record with the same rpIdHash+userId).
    int (*add)(struct fido_cred_store_s *s, const fido_cred_record *r);
    // Return the index-th record whose rpIdHash matches rp; *total gets the count
    // of matches. Returns 0 on hit, non-zero when index is out of range.
    int (*find_by_rp)(struct fido_cred_store_s *s, const uint8_t rp[32],
                      fido_cred_record *out, int index, int *total);
    // Bump the persisted signature counter for the credential with this id.
    int (*update_counter)(struct fido_cred_store_s *s, const uint8_t id[32], uint32_t counter);
    // Delete the credential with this id. Returns 0 if one was removed.
    int (*remove)(struct fido_cred_store_s *s, const uint8_t id[32]);
    // Total resident credentials across all RPs (for credentialManagement / getInfo).
    int (*count)(struct fido_cred_store_s *s);
    // Fetch the index-th credential in global order (0..count-1). 0 on success.
    // Powers the on-device credential manager (list/delete passkeys, no host tool).
    int (*get_at)(struct fido_cred_store_s *s, int index, fido_cred_record *out);
    // Erase every resident credential (authenticatorReset).
    int (*wipe_all)(struct fido_cred_store_s *s);
    void *ctx;
} fido_cred_store;

// A fixed-capacity in-RAM backend (host gate + volatile device fallback).
fido_cred_store *fido_cred_store_ram(void);

#ifdef __cplusplus
}
#endif
