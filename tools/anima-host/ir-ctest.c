// Host unit test for the pure IR protocol encoder (nucleo_ir_proto.c).
// Mirrors the ANIMA host-harness pattern: compile the firmware's pure core with plain gcc and
// assert known-good timing vectors from the public protocol specs — no device, no flash.
//
// Build & run (also wired as `npm run ir:test`):
//   gcc -std=gnu11 -O0 -I firmware/components/nucleo_ir/include \
//       tools/anima-host/ir-ctest.c firmware/components/nucleo_ir/nucleo_ir_proto.c \
//       -o build/irctest && build/irctest
#include "nucleo_ir_proto.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int near(int v, int target) { int d = v - target; if (d < 0) d = -d; return d <= 2; }

int main(void) {
    uint16_t d[NIR_MAX_DURATIONS];
    uint16_t carrier = 0;
    int n;

    // ---- NEC: header 9000/4500, 32 bits, stop mark; 67 durations ----------------------------
    n = nir_encode(NIR_PROTO_NEC, 0x04, 0x08, d, NIR_MAX_DURATIONS, &carrier);
    printf("NEC(0x04,0x08) -> %d durations, carrier %u\n", n, carrier);
    CHECK(n == 67, "NEC length expected 67, got %d", n);
    CHECK(carrier == 38000, "NEC carrier expected 38000, got %u", carrier);
    CHECK(d[0] == 9000 && d[1] == 4500, "NEC header expected 9000/4500, got %u/%u", d[0], d[1]);
    // address bit2 set (0x04) -> third bit pair (indices 6,7) is a 'one' space.
    CHECK(d[6] == 560 && d[7] == 1690, "NEC addr bit2 one expected 560/1690, got %u/%u", d[6], d[7]);
    CHECK(d[2] == 560 && d[3] == 560, "NEC addr bit0 zero expected 560/560, got %u/%u", d[2], d[3]);
    CHECK(d[66] == 560, "NEC stop mark expected 560, got %u", d[66]);
    { int ok = 1; for (int i = 2; i < n; i += 2) if (d[i] != 560) ok = 0;   // every bit mark is 560
      CHECK(ok, "NEC bit marks must all be 560"); }

    // ---- NEC inversion check: ~address and ~command bytes ------------------------------------
    // address byte 0x04 -> ~addr 0xFB. byte boundary: addr occupies index 2..17, ~addr 18..33.
    // command 0x08 -> ~cmd 0xF7. cmd 34..49, ~cmd 50..65.
    // Spot-check ~cmd LSB (bit0 of 0xF7 = 1): index 50 mark / 51 space should be 'one'.
    CHECK(d[50] == 560 && d[51] == 1690, "NEC ~cmd bit0 one expected 560/1690, got %u/%u", d[50], d[51]);

    // ---- Sony SIRC 12-bit: header 2400/600, pulse-width, ends on a mark ----------------------
    n = nir_encode(NIR_PROTO_SONY12, 0x01, 0x15, d, NIR_MAX_DURATIONS, &carrier);
    printf("SONY12(addr=1,cmd=0x15) -> %d durations, carrier %u\n", n, carrier);
    CHECK(n == 25, "Sony12 length expected 25, got %d", n);
    CHECK(carrier == 40000, "Sony carrier expected 40000, got %u", carrier);
    CHECK(d[0] == 2400 && d[1] == 600, "Sony header expected 2400/600, got %u/%u", d[0], d[1]);
    CHECK(d[2] == 1200 && d[3] == 600, "Sony cmd bit0=1 expected 1200/600, got %u/%u", d[2], d[3]);
    CHECK(n % 2 == 1, "Sony frame must end on a mark (odd count), got %d", n);

    // ---- JVC: header 8400/4200, 16 bits no inversion, stop mark; 35 durations ----------------
    n = nir_encode(NIR_PROTO_JVC, 0x03, 0x10, d, NIR_MAX_DURATIONS, &carrier);
    printf("JVC(0x03,0x10) -> %d durations\n", n);
    CHECK(n == 35, "JVC length expected 35, got %d", n);
    CHECK(d[0] == 8400 && d[1] == 4200, "JVC header expected 8400/4200, got %u/%u", d[0], d[1]);
    { int ok = 1; for (int i = 2; i < n; i += 2) if (d[i] != 525) ok = 0;
      CHECK(ok, "JVC bit marks must all be 525"); }

    // ---- RC5: Manchester, starts on a mark, runs are 889 or 1778, total = 27 half-bits -------
    n = nir_encode(NIR_PROTO_RC5, 0x00, 0x01, d, NIR_MAX_DURATIONS, &carrier);
    printf("RC5(0x00,0x01) -> %d durations, carrier %u\n", n, carrier);
    CHECK(n > 0, "RC5 produced no output");
    CHECK(carrier == 36000, "RC5 carrier expected 36000, got %u", carrier);
    { int ok = 1, sum = 0;
      for (int i = 0; i < n; i++) { if (!near(d[i], 889) && !near(d[i], 1778)) ok = 0; sum += d[i]; }
      CHECK(ok, "RC5 durations must be ~889 or ~1778");
      CHECK(sum == 27 * 889, "RC5 total time expected %d, got %d", 27 * 889, sum); }

    // ---- Samsung: header 4500/4500 ----------------------------------------------------------
    n = nir_encode(NIR_PROTO_SAMSUNG, 0x07, 0x02, d, NIR_MAX_DURATIONS, &carrier);
    printf("SAMSUNG(0x07,0x02) -> %d durations\n", n);
    CHECK(n == 67, "Samsung length expected 67, got %d", n);
    CHECK(d[0] == 4500 && d[1] == 4500, "Samsung header expected 4500/4500, got %u/%u", d[0], d[1]);

    // ---- CRT-era power vectors (codes the expanded presets.json ships) ----------------------
    // Sony Trinitron power = SIRC12 addr 1 / cmd 21 (0x15): header + odd length, 40 kHz.
    n = nir_encode(NIR_PROTO_SONY12, 1, 21, d, NIR_MAX_DURATIONS, &carrier);
    CHECK(n == 25 && carrier == 40000, "Sony Trinitron power expected 25 durs @40k, got %d @%u", n, carrier);
    // Philips/EU RC5 power = standby cmd 12 (covers most 80s/90s RC5 CRTs), 36 kHz, starts on a mark.
    n = nir_encode(NIR_PROTO_RC5, 0, 12, d, NIR_MAX_DURATIONS, &carrier);
    CHECK(n > 0 && carrier == 36000, "RC5 power(12) expected >0 durs @36k, got %d @%u", n, carrier);
    // NEC LG/Goldstar/Zenith power = addr 4 / cmd 8, 38 kHz, full 67-duration frame.
    n = nir_encode(NIR_PROTO_NEC, 4, 8, d, NIR_MAX_DURATIONS, &carrier);
    CHECK(n == 67 && carrier == 38000, "NEC LG power expected 67 durs @38k, got %d @%u", n, carrier);

    // ---- Panasonic / Kaseikyo: 48 bits, header 3456/1728, XOR checksum byte, 37 kHz, 99 durs ---
    n = nir_encode(NIR_PROTO_PANASONIC, 0x0080, 0x3D, d, NIR_MAX_DURATIONS, &carrier);
    printf("PANASONIC(0x0080,0x3D) -> %d durations, carrier %u\n", n, carrier);
    CHECK(n == 99, "Panasonic length expected 99, got %d", n);
    CHECK(carrier == 37000, "Panasonic carrier expected 37000, got %u", carrier);
    CHECK(d[0] == 3456 && d[1] == 1728, "Panasonic header expected 3456/1728, got %u/%u", d[0], d[1]);
    // vendor low byte 0x02 (LSB-first): bit0=0,bit1=1 -> 2nd bit pair (idx 4,5) is a 'one' space.
    CHECK(d[2] == 432 && d[3] == 432, "Panasonic vendor bit0 zero expected 432/432, got %u/%u", d[2], d[3]);
    CHECK(d[4] == 432 && d[5] == 1296, "Panasonic vendor bit1 one expected 432/1296, got %u/%u", d[4], d[5]);
    CHECK(d[98] == 432, "Panasonic stop mark expected 432, got %u", d[98]);
    { int ok = 1; for (int i = 2; i < n; i += 2) if (d[i] != 432) ok = 0;   // every bit mark is 432
      CHECK(ok, "Panasonic bit marks must all be 432"); }

    // ---- name round-trip + overflow guard ---------------------------------------------------
    CHECK(nir_proto_from_name("NEC") == NIR_PROTO_NEC, "name parse NEC failed");
    CHECK(nir_proto_from_name("sony") == NIR_PROTO_SONY12, "alias sony failed");
    CHECK(nir_proto_from_name("bogus") == NIR_PROTO__COUNT, "bad name should be __COUNT");
    { uint16_t tiny[4]; int r = nir_encode(NIR_PROTO_NEC, 0, 0, tiny, 4, &carrier);
      CHECK(r == -1, "overflow into a tiny buffer must return -1, got %d", r); }

    if (fails == 0) printf("\nIR encoder: ALL PASS\n");
    else            printf("\nIR encoder: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
