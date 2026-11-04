// vnc_des.h — DES for the VNC password challenge only (see vnc_des.c). Not a general cipher API: VNC's
// authentication is weak by design (8 characters, no encryption of the session) and is used on the LAN.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// One DES block, encryption (FIPS 46-3). For the self-test with the standard's vectors.
void vnc_des_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
// RFC 6143 §7.2.2: encrypt the server's 16-byte challenge in place with the password.
void vnc_des_response(const char *password, uint8_t challenge[16]);

#ifdef __cplusplus
}
#endif
