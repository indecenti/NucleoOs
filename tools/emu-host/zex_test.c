/* zex_test — the vendored z80emu, built with OUR z80config.h (HALT and EI caught), against Frank
 * Cringle's instruction exercisers zexdoc/zexall under a minimal CP/M: the program is loaded at 0100h,
 * BDOS calls (CALL 0005h) are trapped through an IN, and OUT at 0000h ends the run.
 *
 *   zex_test <zexdoc.com|zexall.com>      prints the exerciser's report; exit 0 when no test says ERROR
 *
 * The .com files are fetched into tools/emu-host/testroms/zex/ (gitignored) — see gg-check.mjs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char mem[65536]; int done, errors; } zex_t;

static inline unsigned char z80u_rd(void *ctx, unsigned a) { return ((zex_t *)ctx)->mem[a & 0xFFFF]; }
static inline void z80u_wr(void *ctx, unsigned a, unsigned v) { ((zex_t *)ctx)->mem[a & 0xFFFF] = (unsigned char)v; }
static unsigned char z80u_in(void *ctx, unsigned port, int elapsed);
static int z80u_out(void *ctx, unsigned port, unsigned v, int elapsed)
{
    (void)port; (void)v; (void)elapsed;
    ((zex_t *)ctx)->done = 1;
    return 1;                                   /* stop after this instruction */
}

#include "../../firmware/components/nucleo_emu/vendor/z80emu/z80emu.c"

static Z80_STATE Z;
static char line[256]; static int llen;
static void put(zex_t *z, int c)
{
    putchar(c);
    if (c == '\n' || llen == (int)sizeof line - 1) { line[llen] = 0; if (strstr(line, "ERROR")) z->errors++; llen = 0; }
    else if (c != '\r') line[llen++] = (char)c;
}
/* BDOS: C=2 print E, C=9 print the '$'-terminated string at DE. */
static unsigned char z80u_in(void *ctx, unsigned port, int elapsed)
{
    (void)port; (void)elapsed;
    zex_t *z = (zex_t *)ctx;
    int c = Z.registers.byte[Z80_C];
    if (c == 2) put(z, Z.registers.byte[Z80_E]);
    else if (c == 9) for (unsigned a = Z.registers.word[Z80_DE]; z->mem[a & 0xFFFF] != '$'; a++) put(z, z->mem[a & 0xFFFF]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: zex_test <zexdoc.com>\n"); return 2; }
    static zex_t z;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(z.mem + 0x100, 1, 65536 - 0x100, f);
    fclose(f);
    if (!n) return 2;
    z.mem[0] = 0xD3; z.mem[1] = 0x00;           /* 0000: OUT (0),A -> end */
    z.mem[5] = 0xDB; z.mem[6] = 0x00;           /* 0005: IN A,(0)  -> BDOS trap */
    z.mem[7] = 0xC9;                            /* 0007: RET */
    Z80Reset(&Z);
    Z.pc = 0x100;
    while (!z.done) Z80Emulate(&Z, 1 << 20, &z);   /* HALT/EI are caught and simply resumed */
    printf("\nzex_test: %s\n", z.errors ? "ERRORS" : "all tests OK");
    return z.errors ? 1 : 0;
}
