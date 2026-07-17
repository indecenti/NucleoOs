#include "fido_cbor.h"
#include <string.h>

void fcw_init(fido_cw *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    cbor_encoder_init(&w->enc, buf, cap, 0);
}
void fcw_map(fido_cw *w, size_t pairs)              { cbor_encoder_create_map(&w->enc, &w->map, pairs); }
void fcw_key(fido_cw *w, int key)                   { cbor_encode_int(&w->map, key); }
void fcw_bytes(fido_cw *w, const uint8_t *p, size_t n) { cbor_encode_byte_string(&w->map, p, n); }
void fcw_text(fido_cw *w, const char *s)            { cbor_encode_text_stringz(&w->map, s); }
void fcw_uint(fido_cw *w, uint64_t v)               { cbor_encode_uint(&w->map, v); }
void fcw_bool(fido_cw *w, bool b)                   { cbor_encode_boolean(&w->map, b); }
CborEncoder *fcw_enc(fido_cw *w)                    { return &w->map; }
size_t fcw_finish(fido_cw *w) {
    cbor_encoder_close_container(&w->enc, &w->map);
    return cbor_encoder_get_buffer_size(&w->enc, w->buf);
}

// Walk key/value pairs; on a matching integer key leave *out on the value.
static int find_int_key(const CborValue *map, int key, CborValue *out) {
    CborValue it;
    if (cbor_value_enter_container(map, &it) != CborNoError) return -1;
    while (!cbor_value_at_end(&it)) {
        bool is_match = false;
        if (cbor_value_is_integer(&it)) {
            int k = 0;
            cbor_value_get_int_checked(&it, &k);
            is_match = (k == key);
        }
        if (cbor_value_advance(&it) != CborNoError) return -1;   // step onto value
        if (cbor_value_at_end(&it)) return -1;
        if (is_match) { *out = it; return 0; }
        if (cbor_value_advance(&it) != CborNoError) return -1;   // skip value
    }
    return -1;
}

int fcbor_get_map(const uint8_t *buf, size_t len, CborParser *p, CborValue *map) {
    if (cbor_parser_init(buf, len, 0, p, map) != CborNoError) return -1;
    if (!cbor_value_is_map(map)) return -1;
    return 0;
}
int fcbor_map_bytes(const CborValue *map, int key, uint8_t *dst, size_t *len) {
    CborValue v;
    if (find_int_key(map, key, &v)) return -1;
    if (!cbor_value_is_byte_string(&v)) return -1;
    return cbor_value_copy_byte_string(&v, dst, len, NULL) == CborNoError ? 0 : -1;
}
int fcbor_map_text(const CborValue *map, int key, char *dst, size_t *len) {
    CborValue v;
    if (find_int_key(map, key, &v)) return -1;
    if (!cbor_value_is_text_string(&v)) return -1;
    return cbor_value_copy_text_string(&v, dst, len, NULL) == CborNoError ? 0 : -1;
}
int fcbor_map_uint(const CborValue *map, int key, uint64_t *out) {
    CborValue v;
    if (find_int_key(map, key, &v)) return -1;
    if (!cbor_value_is_unsigned_integer(&v)) return -1;
    return cbor_value_get_uint64(&v, out) == CborNoError ? 0 : -1;
}
int fcbor_map_bool(const CborValue *map, int key, bool *out) {
    CborValue v;
    if (find_int_key(map, key, &v)) return -1;
    if (!cbor_value_is_boolean(&v)) return -1;
    return cbor_value_get_boolean(&v, out) == CborNoError ? 0 : -1;
}
int fcbor_map_enter(const CborValue *map, int key, CborValue *out) {
    return find_int_key(map, key, out);
}
