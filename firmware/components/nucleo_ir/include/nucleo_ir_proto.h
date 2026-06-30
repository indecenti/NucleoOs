// IR protocol encoder — PURE C, no ESP-IDF dependency, so it compiles & runs on the host
// (mirrors the ANIMA host-harness pattern: prove the hard logic off-device before flashing).
//
// Clean-room: the carrier frequencies and pulse timings below are public, factual protocol
// specifications (NEC/Sony/Philips/JVC/Samsung datasheets and the de-facto IR literature). No
// code or data was copied from any GPL/AGPL project — this is an independent implementation.
//
// The encoder turns a logical (protocol, address, command) triple into a flat list of
// alternating mark/space durations in MICROSECONDS, always starting with a mark (carrier ON).
// The firmware side (nucleo_ir.c) modulates that list onto the 38 kHz-class carrier via RMT;
// the host side just inspects the numbers. Keeping the two apart is what makes it testable.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NIR_PROTO_RAW = 0,   // address/command ignored; caller supplies durations directly
    NIR_PROTO_NEC,       // 8-bit address + 8-bit command (classic NEC, with inversion bytes)
    NIR_PROTO_NECEXT,    // 16-bit address + 8-bit command (extended NEC, no address inversion)
    NIR_PROTO_SAMSUNG,   // Samsung32 (4.5 ms header, NEC-like bits)
    NIR_PROTO_SONY12,    // Sony SIRC 12-bit (7 cmd + 5 addr)
    NIR_PROTO_SONY15,    // Sony SIRC 15-bit (7 cmd + 8 addr)
    NIR_PROTO_SONY20,    // Sony SIRC 20-bit (7 cmd + 5 addr + 8 extended)
    NIR_PROTO_RC5,       // Philips RC5 (14-bit Manchester)
    NIR_PROTO_JVC,       // JVC (16-bit, no inversion)
    NIR_PROTO_PANASONIC, // Panasonic / Kaseikyo 48-bit (vendor 0x2002 + dev/sub/fun + XOR check)
    NIR_PROTO__COUNT
} nir_proto_t;

// Largest duration list any supported protocol can produce (NEC = 67; leave generous headroom
// so RAW payloads and future protocols fit). A caller-supplied buffer must be at least this big.
#define NIR_MAX_DURATIONS 700

// Encode one IR frame.
//   proto      — one of nir_proto_t (RAW returns 0; the caller already has the durations)
//   address    — protocol-dependent address field
//   command    — protocol-dependent command field
//   out        — buffer that receives alternating mark/space durations (µs), starting with a mark
//   cap        — capacity of `out` in elements
//   carrier_hz — out: carrier frequency to modulate with (e.g. 38000); set even on the RAW path
// Returns the number of durations written (>0), or -1 on bad arguments / buffer overflow.
int nir_encode(nir_proto_t proto, uint32_t address, uint32_t command,
               uint16_t *out, int cap, uint16_t *carrier_hz);

// Default carrier for a protocol (Hz). Useful for the RAW path and for UI display.
uint16_t nir_proto_carrier(nir_proto_t proto);

// How many times a single frame is conventionally repeated for a reliable keypress
// (Sony wants 3; NEC/others 1 frame + the receiver's own repeat tolerance). UI/firmware hint.
int nir_proto_default_repeats(nir_proto_t proto);

// Name <-> enum, for the JSON API ("nec", "sony12", ...). Case-insensitive parse.
nir_proto_t nir_proto_from_name(const char *name);
const char *nir_proto_name(nir_proto_t proto);

#ifdef __cplusplus
}
#endif
