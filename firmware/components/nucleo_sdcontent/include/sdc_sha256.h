// Portable SHA-256 (FIPS 180-4) for the SD-content installer. Pure C, no ESP-IDF: the SAME code verifies
// every downloaded file on the device and in the host gate (tools/anima-host/sdcontent-e2e.mjs), which
// checks it against the FIPS test vectors and against real release files hashed by Python's hashlib.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t bits;      // message length so far, in bits
    uint8_t  buf[64];
    uint32_t n;         // bytes buffered in buf
} sdc_sha256_t;

void sdc_sha256_init(sdc_sha256_t *c);
void sdc_sha256_update(sdc_sha256_t *c, const void *data, size_t len);
void sdc_sha256_final(sdc_sha256_t *c, uint8_t out[32]);
// 32-byte digest -> 64 lowercase hex chars + NUL.
void sdc_sha256_hex(const uint8_t dig[32], char out[65]);

#ifdef __cplusplus
}
#endif
