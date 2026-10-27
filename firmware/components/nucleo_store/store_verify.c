// App store signatures. See store_verify.h.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "store_verify.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include <string.h>

bool st_verify(const char *key_id, const uint8_t hash[32], const uint8_t *sig, size_t siglen)
{
    const st_key_t *k = NULL;
    for (int i = 0; i < ST_NKEYS; i++) if (!strcmp(ST_KEYS[i].id, key_id)) { k = &ST_KEYS[i]; break; }
    if (!k) return false;
    mbedtls_ecdsa_context c;
    mbedtls_ecdsa_init(&c);
    bool ok = mbedtls_ecp_group_load(&c.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0
           && mbedtls_ecp_point_read_binary(&c.MBEDTLS_PRIVATE(grp), &c.MBEDTLS_PRIVATE(Q), k->q, sizeof k->q) == 0
           && mbedtls_ecp_check_pubkey(&c.MBEDTLS_PRIVATE(grp), &c.MBEDTLS_PRIVATE(Q)) == 0
           && mbedtls_ecdsa_read_signature(&c, hash, 32, sig, siglen) == 0;
    mbedtls_ecdsa_free(&c);
    return ok;
}
