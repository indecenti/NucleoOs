#include "fido_pin.h"
#include <string.h>

static int compute(const fido_crypto_t *cy, const uint8_t key[32],
                   const uint8_t salt[16], const char *pin, uint8_t out[32]) {
    if (!cy->hmac_sha256) return -1;
    size_t pl = strlen(pin);
    if (pl > FIDO_PIN_MAX) return -1;
    uint8_t buf[16 + FIDO_PIN_MAX];
    memcpy(buf, salt, 16);
    memcpy(buf + 16, pin, pl);
    return cy->hmac_sha256(key, 32, buf, 16 + pl, out, cy->ctx);
}

void fido_pin_init(fido_pin_state *st) {
    memset(st, 0, sizeof *st);
    st->retries = FIDO_PIN_RETRIES;
}

int fido_pin_set(fido_pin_state *st, const fido_crypto_t *cy, const uint8_t key[32], const char *pin) {
    size_t pl = strlen(pin);
    if (pl < FIDO_PIN_MIN || pl > FIDO_PIN_MAX) return -1;
    if (cy->rand(st->salt, 16, cy->ctx)) return -1;
    if (compute(cy, key, st->salt, pin, st->verifier)) return -1;
    st->set = 1;
    st->retries = FIDO_PIN_RETRIES;
    return 0;
}

int fido_pin_check(fido_pin_state *st, const fido_crypto_t *cy, const uint8_t key[32], const char *pin) {
    if (!st->set) return -2;
    if (st->retries == 0) return -1;
    uint8_t v[32];
    if (compute(cy, key, st->salt, pin, v)) return -1;
    uint8_t diff = 0;                                   // constant-time compare
    for (int i = 0; i < 32; i++) diff |= v[i] ^ st->verifier[i];
    if (diff == 0) { st->retries = FIDO_PIN_RETRIES; return 1; }
    st->retries--;
    return 0;
}

bool fido_pin_is_set(const fido_pin_state *st) { return st->set != 0; }
int  fido_pin_retries(const fido_pin_state *st) { return st->retries; }
