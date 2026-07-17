#include "fido_cose.h"

size_t fido_cose_es256(const uint8_t pub[65], uint8_t *out, size_t cap) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, out, cap, 0);
    cbor_encoder_create_map(&enc, &map, 5);
    cbor_encode_int(&map, 1);  cbor_encode_int(&map, 2);        // 1 kty: 2 EC2
    cbor_encode_int(&map, 3);  cbor_encode_int(&map, -7);       // 3 alg: -7 ES256
    cbor_encode_int(&map, -1); cbor_encode_int(&map, 1);        // -1 crv: 1 P-256
    cbor_encode_int(&map, -2); cbor_encode_byte_string(&map, pub + 1, 32);   // -2 x
    cbor_encode_int(&map, -3); cbor_encode_byte_string(&map, pub + 33, 32);  // -3 y
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, out);
}
