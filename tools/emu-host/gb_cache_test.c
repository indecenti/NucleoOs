/* gb_cache_test — host gate for the ROM PAGE CACHE, compiled against the REAL firmware source.
 *
 * WHY A SECOND GATE
 * gb_test.c proves the vendored core renders. It does that by including peanut_gb.h and supplying
 * its own callbacks — which means it never touches nucleo_gb.c, the file that decides where a
 * cartridge actually lives. That gap has already cost a debugging session once: the gate was green
 * while the device answered "invalid ROM", because the two used different callback wiring.
 *
 * This gate compiles firmware/components/nucleo_emu/nucleo_gb.c ITSELF and drives it through its
 * public API. NUCLEO_HOST_HEAP shrinks the heap the module believes it has, so the paged path — the
 * only one a 512 KB board ever takes for a real cartridge — is the path under test.
 *
 * WHAT IT MEASURES
 * Misses per frame. Each miss is one SD read, and on the device that costs roughly 2-4 ms against a
 * 16.7 ms frame budget. That single number decides whether the cartridge cache is the emulator's
 * bottleneck or a rounding error, and it is measured rather than argued about — it is also the rig
 * that CHOSE the cache geometry (see the table in docs/native-emulation.md).
 *
 * It then checks save states by replaying the emulator against itself. See state_roundtrip below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nucleo_gb.h"

/* The emulator hands finished scanlines to the app; here we only count them, so a cartridge that
 * loads and produces nothing cannot pass. */
static long g_lines, g_nonblank;
static unsigned long g_hash;            /* rolling checksum of everything drawn — see state_roundtrip */
static void on_line(const uint8_t *px, int line, void *user)
{
    (void)line; (void)user;
    g_lines++;
    for (int i = 0; i < NUCLEO_GB_W; i++) if (px[i] & 3) { g_nonblank++; break; }
    for (int i = 0; i < NUCLEO_GB_W; i++) g_hash = g_hash * 31u + (px[i] & 3);   /* FNV-ish, order matters */
}

/* Audio sink stub: the module opens a PCM channel and pushes a frame of samples per video frame.
 * There is no I2S on a PC, so accept and drop — but count, because "sound was never opened" and
 * "sound was opened and silent" are different bugs. */
long g_pcm_writes;
esp_err_t nucleo_audio_pcm_open(int rate, int channels) { (void)rate; (void)channels; return ESP_OK; }
esp_err_t nucleo_audio_pcm_write(const int16_t *pcm, size_t bytes) { (void)pcm; (void)bytes; g_pcm_writes++; return ESP_OK; }
void      nucleo_audio_pcm_close(void) { }

/* 15 emulated seconds, not 3. A title screen sits in one ROM bank and would let ANY cache design
 * look perfect; the cache only earns its keep once a game is running code, streaming level data and
 * switching banks. So the run is long enough to get in, and it presses buttons to get there. */
#define FRAMES 900

/* Mash START, then A, the way a person would to clear a title screen and a menu. Held for several
 * frames because a single-frame press is below most games' input polling. */
static uint8_t demo_buttons(int f)
{
    int phase = (f / 20) % 4;
    if (f < 120) return 0;                       /* let the boot logo finish undisturbed */
    if (phase == 0) return NUCLEO_GB_START;
    if (phase == 2) return NUCLEO_GB_A;
    return 0;
}

/* SAVE STATES — the one feature here whose failure mode is silent data loss.
 *
 * A state is only correct if the console it restores is INDISTINGUISHABLE from the one that was
 * saved, and "it loaded without crashing" does not test that at all: a state that drops the PPU
 * registers or the timer still loads, still runs, and quietly plays a different game.
 *
 * So this checks the emulator's actual output. Run to a fixed point, save, then run N more frames
 * with a fixed input sequence and hash every pixel of every scanline. Load the state, replay exactly
 * the same N frames, and hash again. The emulator is deterministic, so the two hashes must be equal
 * bit for bit — if any part of the console failed to round-trip, the picture diverges and the numbers
 * will not match.
 */
static int state_roundtrip(const char *rom)
{
    enum { WARMUP = 300, REPLAY = 120 };
    for (int f = 0; f < WARMUP; f++) { nucleo_gb_set_buttons(demo_buttons(f)); nucleo_gb_run_frame(); }

    if (nucleo_gb_state_save(0) != ESP_OK) { printf("    FAIL: state save\n"); return 1; }

    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gb_set_buttons(demo_buttons(WARMUP + f)); nucleo_gb_run_frame(); }
    unsigned long first = g_hash;

    if (nucleo_gb_state_load(0) != ESP_OK) { printf("    FAIL: state load\n"); return 1; }

    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gb_set_buttons(demo_buttons(WARMUP + f)); nucleo_gb_run_frame(); }
    unsigned long second = g_hash;

    /* Leave no litter in the user's ROM folder. */
    char sp[400]; snprintf(sp, sizeof sp, "%s.st0", rom); remove(sp);

    if (first != second) {
        printf("    FAIL: state round-trip diverged (%lu vs %lu)\n", first, second);
        return 1;
    }
    printf("    state: round-trip exact over %d frames\n", REPLAY);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: gb_cache_test <rom> [rom...]\n"); return 2; }

    int failures = 0;
    for (int a = 1; a < argc; a++) {
        const char *rom = argv[a];
        printf("  %s\n", rom);

        g_lines = g_nonblank = g_pcm_writes = 0;
        esp_err_t err = nucleo_gb_open(rom, on_line, NULL);
        if (err != 0) { printf("    FAIL: open -> %d\n", (int)err); failures++; continue; }

        nucleo_gb_stats_t s0; nucleo_gb_get_stats(&s0);
        char title[24]; snprintf(title, sizeof title, "%s", nucleo_gb_title());
        for (int f = 0; f < FRAMES; f++) {
            nucleo_gb_set_buttons(demo_buttons(f));
            nucleo_gb_run_frame();
        }
        nucleo_gb_stats_t s1; nucleo_gb_get_stats(&s1);
        nucleo_gb_close();

        double per_frame = (double)s1.bank_misses / FRAMES;
        printf("    title='%s' rom=%uKB cache=%s(%d slots) heap=%uB\n",
               title, (unsigned)(s1.rom_bytes / 1024),
               s1.rom_resident ? "resident" : "paged", s1.rom_pages, (unsigned)s0.heap_bytes);
        printf("    lines=%ld nonblank=%ld  misses=%u (%.1f/frame -> ~%.1f ms/frame of SD)\n",
               g_lines, g_nonblank, (unsigned)s1.bank_misses, per_frame, per_frame * 3.0);

        /* NOT 144 lines per frame. Switching the LCD off is a normal thing for a Game Boy game to do —
         * loading screens, scene transitions and some menus all blank the panel deliberately, and
         * Konami Golf spends a third of a fifteen-second run that way. The floor here only has to
         * separate "rendering" from "rendering nothing", so it is set well below any real game. */
        if (g_lines < FRAMES * 50)   { printf("    FAIL: too few scanlines\n");  failures++; continue; }
        if (g_nonblank < FRAMES)     { printf("    FAIL: rendered nothing\n");   failures++; continue; }

        /* The budget. A frame is 16.7 ms; anything that spends a quarter of it waiting on the card
         * is the bottleneck, whatever else is optimised. At ~3 ms per read that is ~1.4 misses/frame,
         * so the gate draws the line there and reports the measurement either way. */
        if (!s1.rom_resident && per_frame > 1.4) {
            printf("    FAIL: page cache thrashes (%.1f misses/frame)\n", per_frame);
            failures++; continue;
        }
        /* Re-open rather than continue: the round-trip needs a clean, known starting point, and it
         * would otherwise inherit whatever the cache measurement left the console in. */
        if (nucleo_gb_open(rom, on_line, NULL) == 0) {
            int bad = state_roundtrip(rom);
            nucleo_gb_close();
            if (bad) { failures++; continue; }
        }
        printf("    PASS\n");
    }
    return failures ? 1 : 0;
}
