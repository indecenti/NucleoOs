// fido_crypto — crypto primitives injected into the FIDO2 core as a vtable.
//
// The core (ctap2/u2f/keywrap) never calls a crypto library directly: it calls
// through this table. That keeps the whole protocol layer portable and, more
// importantly, HOST-TESTABLE — the anima-host gate plugs in a deterministic mock
// so the CBOR/flow logic is proven on the PC before any device build. On device
// the table is filled by port/fido_crypto_mbedtls.c (mbedTLS 3.x on the S3).
//
// All functions return 0 on success, non-zero on failure. No allocation.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fido_crypto_s {
    // Fill dst with n cryptographically-strong random bytes.
    int (*rand)(uint8_t *dst, size_t n, void *ctx);

    // SHA-256 of msg[len] -> out[32].
    int (*sha256)(const uint8_t *msg, size_t len, uint8_t out[32], void *ctx);

    // HMAC-SHA-256(key[key_len], msg[len]) -> out[32]. Used by the internal-UV
    // token, the hmac-secret extension, and (later) clientPIN. May be NULL in a
    // build that wires none of those; the core checks before use.
    int (*hmac_sha256)(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t len, uint8_t out[32], void *ctx);

    // Generate a P-256 keypair. priv[32] raw scalar big-endian, pub[65] is the
    // uncompressed point 0x04 || X(32) || Y(32).
    int (*p256_keygen)(uint8_t priv[32], uint8_t pub[65], void *ctx);

    // ECDSA-P256 over SHA-256(msg[len]) with priv[32]. Writes a DER-encoded
    // signature to sig (room for 72 bytes) and sets *sig_len. Implementations
    // SHOULD be deterministic (RFC 6979) to avoid nonce-reuse on a weak RNG.
    int (*p256_sign)(const uint8_t priv[32], const uint8_t *msg, size_t len,
                     uint8_t *sig, size_t *sig_len, void *ctx);

    // AES-256-GCM. key[32], iv[12]. Encrypt: in[len] -> out[len], tag[16] out.
    int (*aes_gcm_seal)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len,
                        uint8_t *out, uint8_t tag[16], void *ctx);
    // Decrypt: verifies tag first. Returns 0 only if the tag is valid.
    int (*aes_gcm_open)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, const uint8_t tag[16],
                        uint8_t *out, void *ctx);

    void *ctx;
} fido_crypto_t;

#ifdef __cplusplus
}
#endif
