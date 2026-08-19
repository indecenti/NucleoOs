/* gb_test — run the VENDORED Game Boy core on the PC, against real ROMs from the SD library.
 *
 * Host-first, per CLAUDE.md: the point is to prove the core and our host callbacks before anything
 * is flashed. This links the SAME peanut_gb.h the firmware links, with the same build switches, and
 * exercises exactly the two things that decide whether the app works on device:
 *
 *   1. MEMORY  — sizeof(struct gb_s) is the number the whole design rests on. Assert it, so a future
 *                core update that quietly doubles it fails here instead of on a board with 60 KB.
 *   2. RUNNING — boot each ROM, run frames, and count the scanlines the PPU actually emits. A core
 *                that loads but renders nothing is the failure mode a "does it compile" test misses.
 *
 * Usage: gb_test <rom> [rom...]      (exit 0 = all passed)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENABLE_LCD   1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#include "../../firmware/components/nucleo_emu/vendor/peanut_gb.h"

#define FRAMES 180              /* 3 seconds of emulated time — past the boot logo of most carts */

struct host {
    uint8_t *rom;
    size_t   rom_bytes;
    uint8_t *ram;
    size_t   ram_bytes;
    unsigned lines;             /* scanlines the PPU handed us */
    unsigned nonblank;          /* ...of which had at least one non-zero pixel */
    unsigned errors;
};

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    struct host *h = (struct host *)gb->direct.priv;
    return addr < h->rom_bytes ? h->rom[addr] : 0xFF;
}
static uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    struct host *h = (struct host *)gb->direct.priv;
    return (h->ram && addr < h->ram_bytes) ? h->ram[addr] : 0xFF;
}
static void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t v)
{
    struct host *h = (struct host *)gb->direct.priv;
    if (h->ram && addr < h->ram_bytes) h->ram[addr] = v;
}
static void on_err(struct gb_s *gb, const enum gb_error_e e, const uint16_t addr)
{
    struct host *h = (struct host *)gb->direct.priv;
    h->errors++;
    fprintf(stderr, "    core error %d @ %04X\n", (int)e, addr);
}
static void on_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t line)
{
    struct host *h = (struct host *)gb->direct.priv;
    (void)line;
    h->lines++;
    for (int i = 0; i < LCD_WIDTH; i++)
        if (px[i] & 3) { h->nonblank++; break; }
}

static int run_one(const char *path)
{
    printf("  %s\n", path);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("    FAIL: cannot open\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); printf("    FAIL: empty\n"); return 1; }

    struct host h;
    memset(&h, 0, sizeof h);
    h.rom_bytes = (size_t)sz;
    h.rom = (uint8_t *)malloc(h.rom_bytes);
    if (!h.rom || fread(h.rom, 1, h.rom_bytes, f) != h.rom_bytes) {
        fclose(f); free(h.rom); printf("    FAIL: short read\n"); return 1;
    }
    fclose(f);

    struct gb_s gb;
    enum gb_init_error_e ie = gb_init(&gb, rom_read, ram_read, ram_write, on_err, &h);
    if (ie != GB_INIT_NO_ERROR) { printf("    FAIL: gb_init=%d\n", (int)ie); free(h.rom); return 1; }

    size_t need = 0;
    if (gb_get_save_size_s(&gb, &need) == 0 && need) {
        h.ram = (uint8_t *)calloc(1, need);
        h.ram_bytes = need;
    }
    gb_init_lcd(&gb, on_line);

    char title[17] = {0};
    gb_get_rom_name(&gb, title);

    clock_t t0 = clock();
    for (int i = 0; i < FRAMES; i++) gb_run_frame(&gb);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    unsigned want = (unsigned)FRAMES * LCD_HEIGHT;
    int bad = 0;
    printf("    title='%s' rom=%luKB save=%luB\n", title,
           (unsigned long)(h.rom_bytes / 1024), (unsigned long)h.ram_bytes);
    printf("    lines=%u/%u (%.0f%%) nonblank=%u errors=%u  %.0f fps on host\n",
           h.lines, want, 100.0 * h.lines / want, h.nonblank, h.errors,
           secs > 0 ? FRAMES / secs : 0);

    /* A game may switch the LCD OFF (LCDC bit 7) — during boot logos, VRAM setup, or a deliberate
     * blank — and the PPU legitimately emits nothing while it is off. So "exactly FRAMES*144 lines"
     * is NOT an invariant; demanding it flagged every healthy ROM. What a working core does
     * guarantee is that most of the time the picture is being produced, and that it is not blank. */
    if (h.lines < want * 7 / 10) { printf("    FAIL: only %u of %u scanlines — the LCD is mostly dark\n", h.lines, want); bad = 1; }
    if (h.nonblank == 0)         { printf("    FAIL: every scanline was blank — nothing rendered\n"); bad = 1; }
    if (h.errors)                { printf("    FAIL: %u core errors\n", h.errors); bad = 1; }
    if (!bad) printf("    PASS\n");

    free(h.rom); free(h.ram);
    return bad;
}

int main(int argc, char **argv)
{
    int fail = 0;

    /* The design assumption, asserted rather than assumed. */
    printf("sizeof(struct gb_s) = %zu B (%.1f KB)\n", sizeof(struct gb_s), sizeof(struct gb_s) / 1024.0);
    if (sizeof(struct gb_s) > 20 * 1024) {
        printf("FAIL: core state grew past the 20 KB the device budget allows\n");
        fail = 1;
    }
    printf("  wram=%d vram=%d oam=%d hram_io=%d  lcd=%dx%d\n\n",
           WRAM_SIZE, VRAM_SIZE, OAM_SIZE, HRAM_IO_SIZE, LCD_WIDTH, LCD_HEIGHT);

    if (argc < 2) { printf("usage: gb_test <rom> [rom...]\n"); return fail; }
    for (int i = 1; i < argc; i++) fail |= run_one(argv[i]);

    printf("\n%s\n", fail ? "GATE FAILED" : "all green");
    return fail;
}
