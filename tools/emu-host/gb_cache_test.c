/* gb_cache_test — host gate for the ROM PAGE CACHE, compiled against the REAL firmware source.
 *
 * WHY A SECOND GATE
 * gb_test.c proves the vendored core renders. It does that by including peanut_gb.h and supplying
 * its own callbacks — which means it never touches nucleo_gb.c, the file that decides where a
 * cartridge actually lives. That gap has already cost a debugging session once: the gate was green
 * while the device answered "invalid ROM", because the two used different callback wiring.
 *
 * This gate compiles firmware/components/nucleo_emu/nucleo_gb.c ITSELF and drives it through its
 * public API against a MODEL of the device heap (heap_model.h: free blocks, best fit, the Solo-boot
 * shape), so the paged path and every allocation the device could refuse are the paths under test.
 *
 * WHAT IT MEASURES
 * Misses per frame. Each miss is one 1 KB SD read, and on the device that costs roughly 1-3 ms against a
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

/* ── the device heap, as a SHAPE ────────────────────────────────────────────────────────────────────
 * nucleo_gb.c is compiled with heap_model.h force-included, so its malloc/calloc/free land here and
 * heap_caps_* read the same numbers. NUCLEO_HOST_HEAP_BLOCKS is a comma list of free-block sizes —
 * gb-check passes the emulator's Solo-boot profile (largest 32 KB, ~90 KB total). Best fit, like the
 * device's TLSF; a freed allocation returns to the block it came from. Unset = one generous block. */
#define HM_MAX 64
static size_t hm_blk[HM_MAX];
static int    hm_n;
typedef struct { size_t take; int blk; } hm_hdr;

static void hm_init(void)
{
    const char *e = getenv("NUCLEO_HOST_HEAP_BLOCKS");
    hm_n = 0;
    if (!e || !*e) { hm_blk[hm_n++] = (size_t)64 * 1024 * 1024; return; }
    while (*e && hm_n < HM_MAX) {
        hm_blk[hm_n++] = (size_t)strtoul(e, (char **)&e, 0);
        while (*e == ',' || *e == ' ') e++;
    }
}
size_t nucleo_host_model_free(void)    { size_t t = 0; for (int i = 0; i < hm_n; i++) t += hm_blk[i]; return t; }
size_t nucleo_host_model_largest(void) { size_t m = 0; for (int i = 0; i < hm_n; i++) if (hm_blk[i] > m) m = hm_blk[i]; return m; }

void *nucleo_hm_malloc(size_t n)
{
    size_t take = ((n + 3) & ~(size_t)3) + 8;            /* 4-byte granules + a TLSF-sized header */
    int best = -1;
    for (int i = 0; i < hm_n; i++)
        if (hm_blk[i] >= take && (best < 0 || hm_blk[i] < hm_blk[best])) best = i;
    if (best < 0) return NULL;
    hm_hdr *h = (hm_hdr *)malloc(sizeof(hm_hdr) + n);
    if (!h) return NULL;
    hm_blk[best] -= take;
    h->take = take; h->blk = best;
    return h + 1;
}
void *nucleo_hm_calloc(size_t n, size_t m)
{
    void *p = nucleo_hm_malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}
void nucleo_hm_free(void *p)
{
    if (!p) return;
    hm_hdr *h = (hm_hdr *)p - 1;
    hm_blk[h->blk] += h->take;
    free(h);
}

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
        hm_init();                                   /* every launch starts from the Solo-boot heap */
        /* ...minus what the app holds before it opens a cartridge: two 4.8 KB DMA band buffers. */
        void *band0 = nucleo_hm_malloc(4800), *band1 = nucleo_hm_malloc(4800);

        /* A Game Boy Color-ONLY cartridge must be REFUSED, by the header probe and by open alike: a
         * DMG core would boot it into garbage. For these the refusal IS the pass condition. */
        nucleo_gb_info_t inf;
        if (nucleo_gb_probe(rom, &inf) == ESP_ERR_INVALID_VERSION) {
            esp_err_t e = nucleo_gb_open(rom, on_line, NULL);
            if (e == ESP_ERR_INVALID_VERSION && !nucleo_gb_is_open()) printf("    refused: Game Boy Color only - PASS\n");
            else { printf("    FAIL: GBC-only cartridge was not refused (open -> %d)\n", (int)e); failures++; nucleo_gb_close(); }
            nucleo_hm_free(band0); nucleo_hm_free(band1);
            continue;
        }

        /* "mooneye:<rom>" — a mooneye MBC test run through THIS module's banked, swapped battery RAM on
         * the device-shaped heap, judged by the Fibonacci registers it leaves behind. ram_256kb walks
         * all four 8 KB SRAM banks with two resident, so it exercises every swap path. */
        if (!strncmp(rom, "mooneye:", 8)) {
            const char *path = rom + 8;
            esp_err_t e = nucleo_gb_open(path, on_line, NULL);
            if (e != ESP_OK) { printf("    FAIL: open -> %d\n", (int)e); failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue; }
            for (int f = 0; f < 600; f++) nucleo_gb_run_frame();
            nucleo_gb_regs_t r; nucleo_gb_get_regs(&r);
            nucleo_gb_stats_t st; nucleo_gb_get_stats(&st);
            nucleo_gb_close();
            nucleo_hm_free(band0); nucleo_hm_free(band1);
            char sp[400]; snprintf(sp, sizeof sp, "%s.sav", path); remove(sp);   /* the test's RAM, not a save */
            bool ok = r.r_b == 3 && r.r_c == 5 && r.r_d == 8 && r.r_e == 13;
            printf("    %s  (SRAM %s, %u bank swaps)\n", ok ? "PASS" : "FAIL: registers are not 3/5/8/13",
                   st.ram_banked ? "banked+swapped" : "resident", (unsigned)st.ram_swaps);
            if (!ok) failures++;
            continue;
        }

        size_t pre_free = nucleo_host_model_free(), pre_big = nucleo_host_model_largest();
        esp_err_t err = nucleo_gb_open(rom, on_line, NULL);
        if (err != 0) {
            printf("    FAIL: open -> %d on a heap of %u B free, largest %u B\n", (int)err, (unsigned)pre_free, (unsigned)pre_big);
            failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue;
        }
        size_t run_free = nucleo_host_model_free();

        nucleo_gb_stats_t s0; nucleo_gb_get_stats(&s0);
        char title[24]; snprintf(title, sizeof title, "%s", nucleo_gb_title());
        for (int f = 0; f < FRAMES; f++) {
            nucleo_gb_set_buttons(demo_buttons(f));
            nucleo_gb_run_frame();
        }
        nucleo_gb_stats_t s1; nucleo_gb_get_stats(&s1);
        nucleo_gb_close();
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        /* Everything the module took must come back: on the device a leak here is a smaller cache
         * for the next cartridge of the session, and eventually a failed launch. */
        if (nucleo_host_model_free() != pre_free + 2 * (4800 + 8)) {
            printf("    FAIL: the module leaked %d B of heap across open/close\n",
                   (int)(pre_free + 2 * (4800 + 8)) - (int)nucleo_host_model_free());
            failures++; continue;
        }

        double per_frame = (double)s1.bank_misses / FRAMES;
        printf("    title='%s' rom=%uKB cache=%s(%d slots) heap=%uB | device heap %uB free, largest %uB -> %uB left while playing\n",
               title, (unsigned)(s1.rom_bytes / 1024),
               s1.rom_resident ? "resident" : "paged", s1.rom_pages, (unsigned)s0.heap_bytes,
               (unsigned)pre_free, (unsigned)pre_big, (unsigned)run_free);
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
