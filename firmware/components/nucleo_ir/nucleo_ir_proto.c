// IR protocol encoder — pure C (see nucleo_ir_proto.h for the clean-room note and contract).
#include "nucleo_ir_proto.h"
#include <string.h>
#include <strings.h>  // strcasecmp

// ---- small append helper: a bounded cursor into the caller's duration buffer ----------------
typedef struct { uint16_t *buf; int cap; int n; int ok; } emit_t;

static void emit(emit_t *e, uint32_t us) {
    if (!e->ok) return;
    if (e->n >= e->cap) { e->ok = 0; return; }       // overflow -> mark the whole encode failed
    e->buf[e->n++] = (us > 0xFFFF) ? 0xFFFF : (uint16_t)us;
}

// A pulse-distance bit: fixed mark, then a wide or narrow space depending on the bit value.
// Used by NEC / Samsung / JVC (LSB-first is applied by the caller via bit ordering).
static void emit_pd_bit(emit_t *e, int bit, uint16_t mark, uint16_t zero_space, uint16_t one_space) {
    emit(e, mark);
    emit(e, bit ? one_space : zero_space);
}

// Send `nbits` of `value`, LSB first, as pulse-distance bits.
static void emit_pd_bits_lsb(emit_t *e, uint32_t value, int nbits,
                             uint16_t mark, uint16_t zero_space, uint16_t one_space) {
    for (int i = 0; i < nbits; i++)
        emit_pd_bit(e, (value >> i) & 1, mark, zero_space, one_space);
}

// ---- timings (microseconds) — public protocol facts -----------------------------------------
// NEC / Samsung share the same bit cell; only the header differs.
#define NEC_HDR_MARK   9000
#define NEC_HDR_SPACE  4500
#define NEC_BIT_MARK    560
#define NEC_ZERO_SPACE  560
#define NEC_ONE_SPACE  1690

#define SAM_HDR_MARK   4500
#define SAM_HDR_SPACE  4500

#define SONY_HDR_MARK  2400
#define SONY_SPACE      600
#define SONY_ONE_MARK  1200
#define SONY_ZERO_MARK  600

#define RC5_HALF        889   // half bit-period; full RC5 bit = 1778 µs

#define JVC_HDR_MARK   8400
#define JVC_HDR_SPACE  4200
#define JVC_BIT_MARK    525
#define JVC_ZERO_SPACE  525
#define JVC_ONE_SPACE  1575

// ---- NEC family (pulse-distance, LSB-first, trailing stop mark) ------------------------------
static int enc_nec(emit_t *e, uint32_t address, uint32_t command, int extended) {
    emit(e, NEC_HDR_MARK);
    emit(e, NEC_HDR_SPACE);
    if (extended) {
        // 16-bit address sent low byte then high byte; command + its inversion.
        emit_pd_bits_lsb(e, address & 0xFF,        8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
        emit_pd_bits_lsb(e, (address >> 8) & 0xFF, 8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    } else {
        emit_pd_bits_lsb(e, address & 0xFF,          8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
        emit_pd_bits_lsb(e, (~address) & 0xFF,       8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    }
    emit_pd_bits_lsb(e, command & 0xFF,          8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit_pd_bits_lsb(e, (~command) & 0xFF,       8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit(e, NEC_BIT_MARK);   // stop bit (final mark so the last space is framed)
    return e->ok ? e->n : -1;
}

static int enc_samsung(emit_t *e, uint32_t address, uint32_t command) {
    emit(e, SAM_HDR_MARK);
    emit(e, SAM_HDR_SPACE);
    // Samsung32: address byte twice, then command + ~command.
    emit_pd_bits_lsb(e, address & 0xFF, 8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit_pd_bits_lsb(e, address & 0xFF, 8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit_pd_bits_lsb(e, command & 0xFF, 8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit_pd_bits_lsb(e, (~command) & 0xFF, 8, NEC_BIT_MARK, NEC_ZERO_SPACE, NEC_ONE_SPACE);
    emit(e, NEC_BIT_MARK);
    return e->ok ? e->n : -1;
}

static int enc_jvc(emit_t *e, uint32_t address, uint32_t command) {
    emit(e, JVC_HDR_MARK);
    emit(e, JVC_HDR_SPACE);
    emit_pd_bits_lsb(e, address & 0xFF, 8, JVC_BIT_MARK, JVC_ZERO_SPACE, JVC_ONE_SPACE);
    emit_pd_bits_lsb(e, command & 0xFF, 8, JVC_BIT_MARK, JVC_ZERO_SPACE, JVC_ONE_SPACE);
    emit(e, JVC_BIT_MARK);
    return e->ok ? e->n : -1;
}

// ---- Sony SIRC (pulse-WIDTH, LSB-first: 7 command bits then N address bits) ------------------
static int enc_sony(emit_t *e, uint32_t address, uint32_t command, int nbits) {
    emit(e, SONY_HDR_MARK);
    emit(e, SONY_SPACE);
    int addr_bits = nbits - 7;              // 12->5, 15->8, 20->13 (5 addr + 8 extended)
    // command: 7 bits LSB-first
    for (int i = 0; i < 7; i++) {
        emit(e, ((command >> i) & 1) ? SONY_ONE_MARK : SONY_ZERO_MARK);
        emit(e, SONY_SPACE);
    }
    // address (+ extended for 20-bit): remaining bits LSB-first
    for (int i = 0; i < addr_bits; i++) {
        emit(e, ((address >> i) & 1) ? SONY_ONE_MARK : SONY_ZERO_MARK);
        emit(e, SONY_SPACE);
    }
    // Sony frames end on a space; the list already alternates mark/space. Drop the dangling
    // trailing space so the buffer ends on a mark (our contract), it carries no information.
    if (e->ok && e->n > 0 && (e->n % 2 == 0)) e->n--;
    return e->ok ? e->n : -1;
}

// ---- Philips RC5 (Manchester, MSB-first, 14 bits) --------------------------------------------
// Build a per-half-bit level array, then run-length encode it. A logical '1' is space-then-mark
// (low->high), a '0' is mark-then-space (high->low). We drop a leading space (transmission must
// start with the carrier ON) and a trailing space (idle), so the list starts and ends on a mark.
static int enc_rc5(emit_t *e, uint32_t address, uint32_t command) {
    // 14 bits MSB-first: S1=1, S2 (=inverted command bit6, the "field"/RC5X), Toggle=0,
    // 5 address bits, 6 command bits. We keep toggle 0 (stateless single press) and S2=1
    // (commands 0..63), which is the classic RC5.
    uint16_t frame = 0;
    frame |= 1 << 13;                       // S1
    frame |= 1 << 12;                       // S2
    // toggle bit 11 = 0
    frame |= (address & 0x1F) << 6;         // 5 address bits
    frame |= (command & 0x3F);              // 6 command bits

    // 28 half-bit levels.
    uint8_t lvl[28];
    int h = 0;
    for (int b = 13; b >= 0; b--) {
        int bit = (frame >> b) & 1;
        if (bit) { lvl[h++] = 0; lvl[h++] = 1; }   // '1' : low then high
        else     { lvl[h++] = 1; lvl[h++] = 0; }   // '0' : high then low
    }
    // Run-length encode equal-level runs into durations of k*RC5_HALF.
    int i = 0;
    if (lvl[0] == 0) {                       // leading space -> drop it, start on the mark
        while (i < 28 && lvl[i] == 0) i++;
    }
    while (i < 28) {
        int j = i;
        while (j < 28 && lvl[j] == lvl[i]) j++;
        uint32_t dur = (uint32_t)(j - i) * RC5_HALF;
        if (lvl[i] == 1 || e->n > 0) emit(e, dur);   // never lead with a space
        i = j;
    }
    if (e->ok && e->n > 0 && (e->n % 2 == 0)) e->n--; // trailing space is idle -> drop
    return e->ok ? e->n : -1;
}

// ---- public API ------------------------------------------------------------------------------
int nir_encode(nir_proto_t proto, uint32_t address, uint32_t command,
               uint16_t *out, int cap, uint16_t *carrier_hz) {
    if (!out || cap <= 0) return -1;
    emit_t e = { out, cap, 0, 1 };
    if (carrier_hz) *carrier_hz = nir_proto_carrier(proto);
    switch (proto) {
        case NIR_PROTO_NEC:     return enc_nec(&e, address, command, 0);
        case NIR_PROTO_NECEXT:  return enc_nec(&e, address, command, 1);
        case NIR_PROTO_SAMSUNG: return enc_samsung(&e, address, command);
        case NIR_PROTO_SONY12:  return enc_sony(&e, address, command, 12);
        case NIR_PROTO_SONY15:  return enc_sony(&e, address, command, 15);
        case NIR_PROTO_SONY20:  return enc_sony(&e, address, command, 20);
        case NIR_PROTO_RC5:     return enc_rc5(&e, address, command);
        case NIR_PROTO_JVC:     return enc_jvc(&e, address, command);
        case NIR_PROTO_RAW:     return 0;   // caller supplies durations
        default:                return -1;
    }
}

uint16_t nir_proto_carrier(nir_proto_t proto) {
    switch (proto) {
        case NIR_PROTO_SONY12:
        case NIR_PROTO_SONY15:
        case NIR_PROTO_SONY20: return 40000;
        case NIR_PROTO_RC5:    return 36000;
        default:               return 38000;
    }
}

int nir_proto_default_repeats(nir_proto_t proto) {
    switch (proto) {
        case NIR_PROTO_SONY12:
        case NIR_PROTO_SONY15:
        case NIR_PROTO_SONY20: return 3;   // SIRC requires at least 3 frames per press
        default:               return 1;
    }
}

static const char *const NAMES[NIR_PROTO__COUNT] = {
    "raw", "nec", "necext", "samsung", "sony12", "sony15", "sony20", "rc5", "jvc",
};

nir_proto_t nir_proto_from_name(const char *name) {
    if (!name) return NIR_PROTO__COUNT;
    for (int i = 0; i < NIR_PROTO__COUNT; i++)
        if (strcasecmp(name, NAMES[i]) == 0) return (nir_proto_t)i;
    // friendly aliases
    if (strcasecmp(name, "sony") == 0) return NIR_PROTO_SONY12;
    if (strcasecmp(name, "nec-ext") == 0 || strcasecmp(name, "nec_ext") == 0) return NIR_PROTO_NECEXT;
    return NIR_PROTO__COUNT;
}

const char *nir_proto_name(nir_proto_t proto) {
    if (proto < 0 || proto >= NIR_PROTO__COUNT) return "?";
    return NAMES[proto];
}
