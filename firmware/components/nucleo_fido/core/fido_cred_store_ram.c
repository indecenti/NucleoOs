// fido_cred_store_ram — a fixed-capacity in-RAM resident-credential store.
//
// Used by the host gate to exercise the discoverable-credential paths without
// NVS, and available on device as a volatile fallback. The device's durable
// backend is port/fido_store_nvs.c.
#include "fido_cred_store.h"
#include <string.h>

#ifndef FIDO_RAM_STORE_CAP
#define FIDO_RAM_STORE_CAP 24
#endif

typedef struct {
    fido_cred_record rec[FIDO_RAM_STORE_CAP];
    int n;
} ram_store_t;

static ram_store_t s_ram;

static int ram_add(fido_cred_store *s, const fido_cred_record *r) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    // Replace a same-account credential (same RP + same userId) if present.
    for (int i = 0; i < st->n; i++) {
        if (memcmp(st->rec[i].rpIdHash, r->rpIdHash, 32) == 0 &&
            st->rec[i].userIdLen == r->userIdLen &&
            memcmp(st->rec[i].userId, r->userId, r->userIdLen) == 0) {
            st->rec[i] = *r;
            return 0;
        }
    }
    if (st->n >= FIDO_RAM_STORE_CAP) return -1;
    st->rec[st->n++] = *r;
    return 0;
}

static int ram_find(fido_cred_store *s, const uint8_t rp[32],
                    fido_cred_record *out, int index, int *total) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    int count = 0, ret = -1;
    for (int i = 0; i < st->n; i++) {
        if (memcmp(st->rec[i].rpIdHash, rp, 32) == 0) {
            if (count == index) { *out = st->rec[i]; ret = 0; }
            count++;
        }
    }
    if (total) *total = count;
    return ret;
}

static int ram_update(fido_cred_store *s, const uint8_t id[32], uint32_t counter) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    for (int i = 0; i < st->n; i++)
        if (memcmp(st->rec[i].id, id, 32) == 0) { st->rec[i].signCount = counter; return 0; }
    return -1;
}

static int ram_remove(fido_cred_store *s, const uint8_t id[32]) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    for (int i = 0; i < st->n; i++)
        if (memcmp(st->rec[i].id, id, 32) == 0) {
            st->rec[i] = st->rec[--st->n];
            return 0;
        }
    return -1;
}

static int ram_count(fido_cred_store *s) { return ((ram_store_t *)s->ctx)->n; }

static int ram_get_at(fido_cred_store *s, int index, fido_cred_record *out) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    if (index < 0 || index >= st->n) return -1;
    *out = st->rec[index];
    return 0;
}

static int ram_wipe(fido_cred_store *s) {
    ram_store_t *st = (ram_store_t *)s->ctx;
    memset(st, 0, sizeof *st);
    return 0;
}

static fido_cred_store s_store = {
    ram_add, ram_find, ram_update, ram_remove, ram_count, ram_get_at, ram_wipe, &s_ram,
};

fido_cred_store *fido_cred_store_ram(void) { return &s_store; }
