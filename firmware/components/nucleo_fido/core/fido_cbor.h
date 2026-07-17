// fido_cbor — thin helpers over vendored tinycbor for the constrained CBOR
// profile CTAP uses (definite-length maps keyed by small integers). Keeps the
// ctap2/cose encoders readable and the parsers guarded against adversarial host
// input (every getter type-checks before copying).
#pragma once
#include "cbor.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- writer ----------------------------------------------------------------
typedef struct {
    uint8_t    *buf;
    CborEncoder enc;
    CborEncoder map;
} fido_cw;

void   fcw_init(fido_cw *w, uint8_t *buf, size_t cap);
void   fcw_map(fido_cw *w, size_t pairs);
void   fcw_key(fido_cw *w, int key);
void   fcw_bytes(fido_cw *w, const uint8_t *p, size_t n);
void   fcw_text(fido_cw *w, const char *s);
void   fcw_uint(fido_cw *w, uint64_t v);
void   fcw_bool(fido_cw *w, bool b);
CborEncoder *fcw_enc(fido_cw *w);   // encoder to nest sub-containers under
size_t fcw_finish(fido_cw *w);      // close map, return bytes written

// ---- reader ----------------------------------------------------------------
// Parse buf[len] and require the top level to be a map. Returns 0 on success.
int fcbor_get_map(const uint8_t *buf, size_t len, CborParser *p, CborValue *map);
// Copy the byte/text value at integer key. *len is in/out (buffer cap / actual).
int fcbor_map_bytes(const CborValue *map, int key, uint8_t *dst, size_t *len);
int fcbor_map_text(const CborValue *map, int key, char *dst, size_t *len);
int fcbor_map_uint(const CborValue *map, int key, uint64_t *out);
int fcbor_map_bool(const CborValue *map, int key, bool *out);
// Leave *out positioned on the value at integer key (for sub-maps / arrays).
int fcbor_map_enter(const CborValue *map, int key, CborValue *out);

#ifdef __cplusplus
}
#endif
