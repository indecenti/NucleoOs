/* gb_accuracy — run ONE test ROM through the vendored Game Boy core, built with the firmware's own
 * switches, and print the verdict the test ROM's author defined.
 *
 * gb_test.c proves the core renders real cartridges; gb_cache_test.c proves where the cartridge lives.
 * Neither says whether the console is RIGHT. This does, against the community's reference suites:
 *
 *   blargg   the ROM prints "Passed"/"Failed" over the serial port        -> we capture the port
 *   mooneye  on completion the ROM executes LD B,B with the Fibonacci      -> we read the registers
 *            registers B/C/D/E = 3/5/8/13 on pass (0x42 everywhere on fail)
 *   acid2    a static picture; correct iff pixel-identical to the         -> we hash the frame
 *            reference image (the expected hash is in gb-accuracy.mjs)
 *
 * Usage: gb_accuracy <rom> <frames>      prints one line: "@@ serial=<...> regs=<...> fb=<hash>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The firmware's switches (firmware/components/nucleo_emu/nucleo_gb.c). The APU is off here: the
 * suites below test the CPU, timers, MBCs and PPU, and ENABLE_SOUND 0 gives the core's own register
 * read-back table instead of minigb_apu's. */
#define ENABLE_LCD   1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_12_COLOUR 0
#include "../../firmware/components/nucleo_emu/vendor/peanut_gb.h"

static uint8_t *rom;
static size_t   rom_sz;
static uint8_t  cram[128 * 1024];
static uint8_t  fb[LCD_HEIGHT][LCD_WIDTH];
static char     serial[512];
static size_t   serial_n;
static unsigned errors;

static uint8_t rd(struct gb_s *gb, const uint_fast32_t a) { (void)gb; return a < rom_sz ? rom[a] : 0xFF; }
static uint8_t rr(struct gb_s *gb, const uint_fast32_t a) { (void)gb; return a < sizeof cram ? cram[a] : 0xFF; }
static void rw(struct gb_s *gb, const uint_fast32_t a, const uint8_t v) { (void)gb; if (a < sizeof cram) cram[a] = v; }
static void er(struct gb_s *gb, const enum gb_error_e e, const uint16_t a) { (void)gb; (void)e; (void)a; errors++; }
static void ln(struct gb_s *gb, const uint8_t *px, const uint_fast8_t line)
{
    (void)gb;
    if (line < LCD_HEIGHT) for (int x = 0; x < LCD_WIDTH; x++) fb[line][x] = px[x] & 3;
}
static void stx(struct gb_s *gb, const uint8_t tx)
{
    (void)gb;
    if (serial_n < sizeof serial - 1) serial[serial_n++] = (tx >= 32 && tx < 127) ? (char)tx : ' ';
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gb_accuracy <rom> <frames>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("@@ open-failed\n"); return 2; }
    fseek(f, 0, SEEK_END); rom_sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    rom = (uint8_t *)malloc(rom_sz);
    if (!rom || fread(rom, 1, rom_sz, f) != rom_sz) { printf("@@ read-failed\n"); return 2; }
    fclose(f);
    memset(cram, 0xFF, sizeof cram);

    static struct gb_s gb;
    enum gb_init_error_e e = gb_init(&gb, rd, rr, rw, er, NULL);
    if (e != GB_INIT_NO_ERROR) { printf("@@ init-error=%d\n", (int)e); return 0; }
    gb_init_lcd(&gb, ln);
    gb_init_serial(&gb, stx, NULL);

    long frames = atol(argv[2]);
    for (long i = 0; i < frames; i++) gb_run_frame(&gb);

    /* FNV-1a over the shade indices of the last frame. */
    uint32_t h = 2166136261u;
    for (int y = 0; y < LCD_HEIGHT; y++) for (int x = 0; x < LCD_WIDTH; x++) { h ^= fb[y][x]; h *= 16777619u; }
    serial[serial_n] = '\0';
    printf("@@ regs=%02X%02X%02X%02X fb=%08X errors=%u serial=%s\n",
           gb.cpu_reg.bc.bytes.b, gb.cpu_reg.bc.bytes.c, gb.cpu_reg.de.bytes.d, gb.cpu_reg.de.bytes.e,
           (unsigned)h, errors, serial);
    free(rom);
    return 0;
}
