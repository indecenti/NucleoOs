// Release signatures. See include/fwsig.h and docs/release-signing.md.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "fwsig.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include <stdio.h>
#include <string.h>

void fwsig_stream_init(fwsig_stream_t *s)
{
    memset(s, 0, sizeof *s);
    mbedtls_sha256_init(&s->sha);
    mbedtls_sha256_starts(&s->sha, 0);
}

// Everything but the newest FWSIG_BLOCK bytes is hashed; those wait in tail[] until more data pushes them out.
void fwsig_stream_update(fwsig_stream_t *s, const uint8_t *p, size_t n)
{
    if (n >= FWSIG_BLOCK) {
        if (s->tlen) { mbedtls_sha256_update(&s->sha, s->tail, s->tlen); s->body += s->tlen; }
        mbedtls_sha256_update(&s->sha, p, n - FWSIG_BLOCK);
        s->body += n - FWSIG_BLOCK;
        memcpy(s->tail, p + n - FWSIG_BLOCK, FWSIG_BLOCK);
        s->tlen = FWSIG_BLOCK;
        return;
    }
    size_t over = s->tlen + n > FWSIG_BLOCK ? s->tlen + n - FWSIG_BLOCK : 0;
    if (over) {
        mbedtls_sha256_update(&s->sha, s->tail, over);
        s->body += over;
        memmove(s->tail, s->tail + over, s->tlen - over);
        s->tlen -= over;
    }
    memcpy(s->tail + s->tlen, p, n);
    s->tlen += n;
}

static void to_hex(const uint8_t *d, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * n] = '\0';
}

bool fwsig_stream_final(fwsig_stream_t *s, char sha_hex[65], uint32_t *size)
{
    uint8_t d[32];
    mbedtls_sha256_finish(&s->sha, d);
    to_hex(d, 32, sha_hex);
    *size = s->body;
    return s->tlen == FWSIG_BLOCK;
}

void fwsig_stream_free(fwsig_stream_t *s) { mbedtls_sha256_free(&s->sha); }

static bool ecdsa_ok(const fwsig_key_t *k, const uint8_t hash[32], const uint8_t *sig, size_t siglen)
{
    mbedtls_ecdsa_context c;
    mbedtls_ecdsa_init(&c);
    bool ok = mbedtls_ecp_group_load(&c.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0
           && mbedtls_ecp_point_read_binary(&c.MBEDTLS_PRIVATE(grp), &c.MBEDTLS_PRIVATE(Q), k->q, sizeof k->q) == 0
           && mbedtls_ecp_check_pubkey(&c.MBEDTLS_PRIVATE(grp), &c.MBEDTLS_PRIVATE(Q)) == 0
           && mbedtls_ecdsa_read_signature(&c, hash, 32, sig, siglen) == 0;
    mbedtls_ecdsa_free(&c);
    return ok;
}

fwsig_rc_t fwsig_verify(const uint8_t block[FWSIG_BLOCK], const char *domain, const char *version,
                        const char sha_hex[65], uint32_t size)
{
    if (!block || memcmp(block, FWSIG_MAGIC, 16) != 0) return FWSIG_E_MISSING;
    char id[17];
    memcpy(id, block + 16, 16); id[16] = '\0';
    const fwsig_key_t *k = NULL;
    for (int i = 0; i < FWSIG_NKEYS; i++) if (!strcmp(FWSIG_KEYS[i].id, id)) { k = &FWSIG_KEYS[i]; break; }
    if (!k) return FWSIG_E_KEY;
    size_t dl = (size_t)block[32] | (size_t)block[33] << 8;
    if (dl < 8 || dl > 72) return FWSIG_E_BAD;
    if (!domain || !version || !sha_hex || strlen(sha_hex) != 64 || strlen(version) > 40) return FWSIG_E_BAD;

    char msg[160];
    int n = snprintf(msg, sizeof msg, "%s\n%s\n%s\n%lu\n", domain, version, sha_hex, (unsigned long)size);
    if (n <= 0 || n >= (int)sizeof msg) return FWSIG_E_BAD;
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)msg, (size_t)n, h, 0);
    return ecdsa_ok(k, h, block + 34, dl) ? FWSIG_OK : FWSIG_E_BAD;
}

const char *fwsig_rc_name(fwsig_rc_t rc)
{
    switch (rc) {
    case FWSIG_OK:        return "ok";
    case FWSIG_E_MISSING: return "unsigned";
    case FWSIG_E_KEY:     return "unknown key";
    default:              return "bad signature";
    }
}
