// Release signatures for what the device takes from the release site: the firmware image and the SD content
// manifest (which in turn pins the web OS, served same-origin with the API). Spec: docs/release-signing.md.
//
// A signature block is FWSIG_BLOCK bytes:
//   [0,16)   "nucleoos-sig-v1\n"
//   [16,32)  key id: 16 lowercase hex = the first 8 bytes of SHA-256(uncompressed P-256 public key)
//   [32,34)  DER signature length, little endian (8..72)
//   [34,106) ECDSA P-256 / SHA-256 signature, DER, zero padded
//   [106,128) zero
// over the message "<domain>\n<version>\n<sha256 hex of the signed bytes>\n<their size>\n".
// The firmware image carries its block appended at the very end (older firmware writes it to the OTA slot
// past the image, where nothing reads it); the SD manifest's block is the file sd-manifest.sig beside it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWSIG_BLOCK       128
#define FWSIG_MAGIC       "nucleoos-sig-v1\n"
#define FWSIG_DOMAIN_OTA  "nucleoos-cardputer-ota-v1"
#define FWSIG_DOMAIN_SD   "nucleoos-cardputer-sd-v1"

typedef enum {
    FWSIG_OK = 0,
    FWSIG_E_MISSING,     // no signature block (an unsigned build, or a release not signed yet)
    FWSIG_E_KEY,         // signed with a key this firmware does not trust
    FWSIG_E_BAD,         // the signature does not match (tampered, or for another version)
} fwsig_rc_t;

typedef struct { const char id[17]; uint8_t q[65]; } fwsig_key_t;
extern const fwsig_key_t FWSIG_KEYS[];
extern const int FWSIG_NKEYS;

// Hash of a stream whose last FWSIG_BLOCK bytes are its signature block (held back, not hashed).
typedef struct {
    mbedtls_sha256_context sha;
    uint8_t  tail[FWSIG_BLOCK];
    size_t   tlen;
    uint32_t body;       // bytes hashed so far = the size of the signed image
} fwsig_stream_t;

void fwsig_stream_init(fwsig_stream_t *s);
void fwsig_stream_update(fwsig_stream_t *s, const uint8_t *p, size_t n);
// Ends the stream: the SHA-256 (hex) and size of everything but the trailing block. False if the stream was
// shorter than one block. The block itself stays in s->tail for fwsig_verify.
bool fwsig_stream_final(fwsig_stream_t *s, char sha_hex[65], uint32_t *size);
void fwsig_stream_free(fwsig_stream_t *s);

fwsig_rc_t fwsig_verify(const uint8_t block[FWSIG_BLOCK], const char *domain, const char *version,
                        const char sha_hex[65], uint32_t size);
const char *fwsig_rc_name(fwsig_rc_t rc);

#ifdef __cplusplus
}
#endif
