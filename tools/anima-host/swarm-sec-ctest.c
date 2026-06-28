// swarm-sec-ctest.c — host gate for nucleo_swarm_sec (the swarm SECURITY gate, P3). Proves the pure
// primitives the validation made MANDATORY before any swarm frame is acted on:
//   - demux routes by magic (NL=link, NM=mesh, else unknown);
//   - seal/open round-trips, and a tampered payload / tampered tag / wrong key / truncated frame
//     all FAIL to open (forgery rejected) — verify is constant-time;
//   - MAC trust-pin: open swarm allows all, enforced swarm allows only pinned MACs.
// The HMAC is INJECTED: here a deterministic mock (proves framing/compare); the device uses mbedtls
// HMAC-SHA256. Run via `npm run swarm:test`.
#include "nucleo_swarm_sec.h"
#include "nucleo_swarm.h"
#include <stdio.h>
#include <string.h>

static int FAILS = 0;
static void check(const char *name, int cond, const char *detail)
{
    printf("  [%s] %s%s%s\n", cond ? "ok" : "FAIL", name, detail ? " — " : "", detail ? detail : "");
    if (!cond) FAILS++;
}

// Deterministic mock MAC: key+msg sensitive, fills 32 bytes. NOT cryptographic — it exercises the
// seal/open framing + constant-time compare. On device this is mbedtls HMAC-SHA256.
static int mock_hmac(void *u, const uint8_t *key, int kl, const uint8_t *msg, int ml, uint8_t out[SW_MAC_OUT])
{
    (void)u;
    for (int i = 0; i < SW_MAC_OUT; i++) {
        uint32_t h = 2166136261u ^ (uint32_t)(i * 2654435761u);
        for (int k = 0; k < kl; k++)  { h ^= key[k]; h *= 16777619u; }
        for (int m = 0; m < ml; m++)  { h ^= msg[m]; h *= 16777619u; }
        out[i] = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    }
    return 0;
}

// Deterministic mock KDF (device uses PBKDF2-HMAC-SHA256). Sensitive to passphrase + salt.
static int mock_kdf(void *u, const char *pass, int pl, const uint8_t *salt, int sl, uint8_t out[SW_PSK_LEN])
{
    (void)u;
    for (int i = 0; i < SW_PSK_LEN; i++) {
        uint32_t h = 2166136261u ^ (uint32_t)(i * 40503u + 7u);
        for (int k = 0; k < pl; k++) { h ^= (uint8_t)pass[k]; h *= 16777619u; }
        for (int s = 0; s < sl; s++) { h ^= salt[s]; h *= 16777619u; }
        out[i] = (uint8_t)(h ^ (h >> 7) ^ (h >> 15) ^ (h >> 23));
    }
    return 0;
}

int main(void)
{
    printf("nucleo_swarm_sec host gate\n");
    const uint8_t KEY[]  = "swarm-psk-001";
    const uint8_t KEY2[] = "swarm-psk-XXX";
    const int KL = (int)sizeof(KEY) - 1, KL2 = (int)sizeof(KEY2) - 1;

    // ---- demux ----
    printf("demux:\n");
    { uint8_t nl[4] = {'N','L',1,5}; uint8_t nm[4] = {'N','M',1,0}; uint8_t x[4] = {'X','Y',0,0};
      check("NL -> link",  sw_demux(nl, 4) == SW_KIND_LINK, NULL);
      check("NM -> mesh",  sw_demux(nm, 4) == SW_KIND_MESH, NULL);
      check("?? -> unknown", sw_demux(x, 4) == SW_KIND_UNKNOWN, NULL);
      check("short -> unknown", sw_demux(nm, 1) == SW_KIND_UNKNOWN, NULL); }

    // ---- seal / open ----
    printf("seal/open:\n");
    {
        uint8_t f[64]; int n0 = 20;
        for (int i = 0; i < n0; i++) f[i] = (uint8_t)(i * 7 + 1);
        f[0] = 'N'; f[1] = 'M';                                  // keep a realistic magic
        int sealed = sw_seal(KEY, KL, f, n0, sizeof f, mock_hmac, NULL);
        check("seal grows by tag", sealed == n0 + SW_TAG_LEN, NULL);
        check("magic intact after seal", sw_demux(f, sealed) == SW_KIND_MESH, NULL);
        int plen = sw_open(KEY, KL, f, sealed, mock_hmac, NULL);
        check("open round-trips", plen == n0, NULL);

        uint8_t t[64]; memcpy(t, f, sealed); t[5] ^= 0x40;       // tamper a payload byte
        check("tampered payload rejected", sw_open(KEY, KL, t, sealed, mock_hmac, NULL) == -1, NULL);

        memcpy(t, f, sealed); t[sealed - 1] ^= 0x01;            // tamper the tag
        check("tampered tag rejected", sw_open(KEY, KL, t, sealed, mock_hmac, NULL) == -1, NULL);

        check("wrong key rejected", sw_open(KEY2, KL2, f, sealed, mock_hmac, NULL) == -1, NULL);
        check("truncated rejected", sw_open(KEY, KL, f, SW_TAG_LEN, mock_hmac, NULL) == -1, NULL);

        uint8_t small[8];                                        // cap too small to hold tag
        check("seal refuses tiny cap", sw_seal(KEY, KL, small, 4, sizeof small, mock_hmac, NULL) == -1, NULL);
        // regression: a huge len must NOT pass via signed overflow of (len + SW_TAG_LEN)
        check("seal rejects overflow len", sw_seal(KEY, KL, f, 2147483640, sizeof f, mock_hmac, NULL) == -1, NULL);
    }

    // ---- MAC trust-pin ----
    printf("trust-pin:\n");
    {
        const uint8_t A[6] = {0xAA,0,0,0,0,1}, B[6] = {0xBB,0,0,0,0,2}, C[6] = {0xCC,0,0,0,0,3};
        sw_pins_t p; sw_pin_reset(&p, false);
        check("open swarm allows any", sw_pin_allowed(&p, A) && sw_pin_allowed(&p, C), NULL);
        sw_pin_reset(&p, true);
        check("enforced empty denies", !sw_pin_allowed(&p, A), NULL);
        sw_pin_add(&p, A); sw_pin_add(&p, B);
        check("pinned allowed",     sw_pin_allowed(&p, A) && sw_pin_allowed(&p, B), NULL);
        check("unpinned denied",    !sw_pin_allowed(&p, C), NULL);
        check("re-add idempotent",  sw_pin_add(&p, A) && p.n == 2, NULL);
    }

    // ---- membership / join (passphrase -> PSK -> admit) ----
    printf("join / membership:\n");
    {
        const uint8_t SALT[] = SW_SALT; const int SL = (int)sizeof(SALT) - 1;
        const uint8_t MAC_A[6] = {0xA,0,0,0,0,1}, MAC_X[6] = {0xE,0,0,0,0,9};

        swarm_member_t a, b;
        swarm_member_init(&a); swarm_member_init(&b);
        check("init -> open swarm", swarm_is_open(&a), NULL);
        check("good passphrase joins", swarm_join(&a, "desk-swarm-42", SALT, SL, mock_kdf, NULL) == 1 && !swarm_is_open(&a), NULL);
        check("same passphrase same swarm", swarm_join(&b, "desk-swarm-42", SALT, SL, mock_kdf, NULL) == 1, NULL);

        // A seals an NM frame; same-passphrase B admits it (payload recovered).
        uint8_t f[64]; int n0 = 24; for (int i = 0; i < n0; i++) f[i] = (uint8_t)(i * 5 + 2);
        f[0] = 'N'; f[1] = 'M';
        int sealed = swarm_seal_out(&a, f, n0, sizeof f, mock_hmac, NULL);
        check("closed seal adds tag", sealed == n0 + SW_TAG_LEN, NULL);
        check("same-swarm admits", swarm_admit(&b, MAC_A, f, sealed, mock_hmac, NULL) == n0, NULL);

        // Different passphrase -> different PSK -> reject the same frame.
        swarm_member_t c; swarm_member_init(&c);
        swarm_join(&c, "other-swarm-99", SALT, SL, mock_kdf, NULL);
        check("foreign passphrase rejected", swarm_admit(&c, MAC_A, f, sealed, mock_hmac, NULL) == -1, NULL);

        // Anti-downgrade: a CLOSED member must reject an UNtagged frame.
        uint8_t bare[24]; memcpy(bare, f, n0); bare[0] = 'N'; bare[1] = 'M';
        check("closed rejects untagged", swarm_admit(&b, MAC_A, bare, n0, mock_hmac, NULL) == -1, NULL);

        // OPEN swarm: seal is a no-op and admit accepts as-is.
        swarm_member_t o; swarm_member_init(&o);
        int on = swarm_seal_out(&o, bare, n0, sizeof bare, mock_hmac, NULL);
        check("open seal is no-op", on == n0, NULL);
        check("open admits untagged", swarm_admit(&o, MAC_A, bare, n0, mock_hmac, NULL) == n0, NULL);

        // Passphrase policy.
        swarm_member_t t; swarm_member_init(&t);
        check("short passphrase -> open", swarm_join(&t, "short", SALT, SL, mock_kdf, NULL) == 0 && swarm_is_open(&t), NULL);
        char longp[80]; memset(longp, 'x', sizeof longp - 1); longp[sizeof longp - 1] = 0;
        check("overlong passphrase -> error", swarm_join(&t, longp, SALT, SL, mock_kdf, NULL) == -1, NULL);

        // MAC pin enforced: a valid-HMAC frame from an UNpinned MAC is still rejected.
        swarm_member_t pn; swarm_member_init(&pn);
        swarm_join(&pn, "desk-swarm-42", SALT, SL, mock_kdf, NULL);   // same key as A
        sw_pin_reset(&pn.pins, true); sw_pin_add(&pn.pins, MAC_A);
        check("pinned MAC + valid HMAC admits", swarm_admit(&pn, MAC_A, f, sealed, mock_hmac, NULL) == n0, NULL);
        check("unpinned MAC rejected despite HMAC", swarm_admit(&pn, MAC_X, f, sealed, mock_hmac, NULL) == -1, NULL);
    }

    printf(FAILS ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", FAILS);
    return FAILS ? 1 : 0;
}
