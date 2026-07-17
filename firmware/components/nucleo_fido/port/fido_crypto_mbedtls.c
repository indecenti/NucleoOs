// fido_crypto_mbedtls — fill the FIDO core crypto vtable with mbedTLS (the copy
// bundled with ESP-IDF): CTR_DRBG, SHA-256, HMAC-SHA-256, P-256 keygen + ECDSA
// sign, and AES-256-GCM (the keywrap primitive). ECDSA is deterministic (RFC
// 6979) when the mbedTLS build enables MBEDTLS_ECDSA_DETERMINISTIC.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "fido_port.h"
#include <string.h>
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"

static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_seeded = false;

static int ensure_drbg(void) {
    if (s_seeded) return 0;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char *pers = "nucleo-fido";
    if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return -1;
    s_seeded = true;
    return 0;
}

static int c_rand(uint8_t *dst, size_t n, void *ctx) {
    (void)ctx;
    if (ensure_drbg()) return -1;
    return mbedtls_ctr_drbg_random(&s_drbg, dst, n);
}

static int c_sha256(const uint8_t *m, size_t len, uint8_t out[32], void *ctx) {
    (void)ctx;
    return mbedtls_sha256(m, len, out, 0);
}

static int c_hmac(const uint8_t *key, size_t kl, const uint8_t *m, size_t ml, uint8_t out[32], void *ctx) {
    (void)ctx;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    return mbedtls_md_hmac(md, key, kl, m, ml, out);
}

static int c_keygen(uint8_t priv[32], uint8_t pub[65], void *ctx) {
    (void)ctx;
    if (ensure_drbg()) return -1;
    mbedtls_ecp_keypair kp; mbedtls_ecp_keypair_init(&kp);
    int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp, mbedtls_ctr_drbg_random, &s_drbg);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&kp.d, priv, 32);
    size_t olen = 0;
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&kp.grp, &kp.Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, 65);
    if (rc == 0 && olen != 65) rc = -1;
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

static int c_sign(const uint8_t priv[32], const uint8_t *m, size_t len,
                  uint8_t *sig, size_t *sl, void *ctx) {
    (void)ctx;
    if (ensure_drbg()) return -1;
    uint8_t hash[32];
    int rc = mbedtls_sha256(m, len, hash, 0);
    if (rc) return rc;
    mbedtls_ecp_keypair kp; mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecdsa_context ec; mbedtls_ecdsa_init(&ec);
    rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, priv, 32);
    if (rc == 0) rc = mbedtls_ecdsa_from_keypair(&ec, &kp);
    if (rc == 0) rc = mbedtls_ecdsa_write_signature(&ec, MBEDTLS_MD_SHA256, hash, 32,
                          sig, 72, sl, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ecdsa_free(&ec);
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

static int c_seal(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], void *ctx) {
    (void)ctx;
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, iv, 12, aad, al, in, out, 16, tag);
    mbedtls_gcm_free(&g);
    return rc;
}

static int c_open(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out, void *ctx) {
    (void)ctx;
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, len, iv, 12, aad, al, tag, 16, in, out);
    mbedtls_gcm_free(&g);
    return rc;
}

static const fido_crypto_t S_CRYPTO = {
    c_rand, c_sha256, c_hmac, c_keygen, c_sign, c_seal, c_open, NULL
};

const fido_crypto_t *fido_mbedtls_crypto(void) { return &S_CRYPTO; }
