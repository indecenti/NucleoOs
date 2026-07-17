// fido_store_nvs — NVS-backed resident (discoverable) credential store, plus the
// persisted global signature counter. Records live in the "fidocred" namespace as
// "r<i>" blobs with a "n" count; the private key inside each is keywrap-sealed, so
// even a flash dump yields only ciphertext bound to the eFuse-derived key. Unlike
// Poseidon's store this implements remove() and wipe_all() (reset / manage).
#include "fido_port.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *NS = "fidocred";

static int rec_count(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t n = 0; nvs_get_u32(h, "n", &n); nvs_close(h);
    return (int)n;
}
static void set_count(int n) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "n", (uint32_t)n); nvs_commit(h); nvs_close(h);
}
static bool rec_load(int i, fido_cred_record *r) {
    char k[12]; snprintf(k, sizeof k, "r%d", i);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = sizeof *r;
    esp_err_t e = nvs_get_blob(h, k, r, &sz);
    nvs_close(h);
    return e == ESP_OK && sz == sizeof *r;
}
static bool rec_save(int i, const fido_cred_record *r) {
    char k[12]; snprintf(k, sizeof k, "r%d", i);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, k, r, sizeof *r);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static void rec_erase(int i) {
    char k[12]; snprintf(k, sizeof k, "r%d", i);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, k); nvs_commit(h); nvs_close(h);
}

static int s_add(fido_cred_store *s, const fido_cred_record *r) {
    (void)s;
    int n = rec_count();
    for (int i = 0; i < n; i++) {                 // replace a same-account credential
        fido_cred_record e;
        if (rec_load(i, &e) && memcmp(e.rpIdHash, r->rpIdHash, 32) == 0 &&
            e.userIdLen == r->userIdLen && memcmp(e.userId, r->userId, r->userIdLen) == 0)
            return rec_save(i, r) ? 0 : -1;
    }
    if (!rec_save(n, r)) return -1;
    set_count(n + 1);
    return 0;
}
static int s_find(fido_cred_store *s, const uint8_t rp[32], fido_cred_record *out, int index, int *total) {
    (void)s;
    int n = rec_count(), cnt = 0, ret = -1;
    for (int i = 0; i < n; i++) {
        fido_cred_record e;
        if (!rec_load(i, &e)) continue;
        if (memcmp(e.rpIdHash, rp, 32) == 0) { if (cnt == index) { *out = e; ret = 0; } cnt++; }
    }
    if (total) *total = cnt;
    return ret;
}
static int s_update(fido_cred_store *s, const uint8_t id[32], uint32_t nc) {
    (void)s;
    int n = rec_count();
    for (int i = 0; i < n; i++) {
        fido_cred_record e;
        if (!rec_load(i, &e)) continue;
        if (memcmp(e.id, id, 32) == 0) { e.signCount = nc; return rec_save(i, &e) ? 0 : -1; }
    }
    return -1;
}
static int s_remove(fido_cred_store *s, const uint8_t id[32]) {
    (void)s;
    int n = rec_count();
    for (int i = 0; i < n; i++) {
        fido_cred_record e;
        if (!rec_load(i, &e)) continue;
        if (memcmp(e.id, id, 32) == 0) {
            if (i != n - 1) {                     // move the last record into the hole
                fido_cred_record last;
                if (rec_load(n - 1, &last)) rec_save(i, &last);
            }
            rec_erase(n - 1);
            set_count(n - 1);
            return 0;
        }
    }
    return -1;
}
static int s_count(fido_cred_store *s) { (void)s; return rec_count(); }
static int s_get_at(fido_cred_store *s, int index, fido_cred_record *out) {
    (void)s;
    if (index < 0 || index >= rec_count()) return -1;
    return rec_load(index, out) ? 0 : -1;
}
static int s_wipe(fido_cred_store *s) {
    (void)s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    return 0;
}

static fido_cred_store S = { s_add, s_find, s_update, s_remove, s_count, s_get_at, s_wipe, NULL };
fido_cred_store *fido_cred_store_nvs(void) { return &S; }

// Global signature counter (non-resident credentials), namespace "fido".
uint32_t fido_counter_load(void) {
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0; nvs_get_u32(h, "ctr", &v); nvs_close(h);
    return v;
}
void fido_counter_store(uint32_t v) {
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "ctr", v); nvs_commit(h); nvs_close(h);
}

// On-device PIN verifier state (opaque blob; contains only a salted HMAC verifier).
bool fido_pin_load(fido_pin_state *st) {
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = sizeof *st;
    esp_err_t e = nvs_get_blob(h, "pin", st, &sz);
    nvs_close(h);
    return e == ESP_OK && sz == sizeof *st;
}
void fido_pin_store(const fido_pin_state *st) {
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "pin", st, sizeof *st); nvs_commit(h); nvs_close(h);
}
void fido_pin_erase(void) {
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "pin"); nvs_commit(h); nvs_close(h);
}
