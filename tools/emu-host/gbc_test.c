/* gbc_test — host gate for the NATIVE Game Boy Color, compiled against the REAL firmware source.
 *
 * firmware/components/nucleo_emu/nucleo_gbc.c (nucleo_gb.c around Walnut-CGB) is compiled with
 * heap_model.h force-included, so every allocation answers from a MODEL of the Solo-boot heap: the
 * 32 KB work RAM, the 16 KB video RAM, the battery RAM banks and the ROM page cache must fit it as on
 * the device. Per cartridge: every frame emits 144 lines, the picture is colour, the cache stays under
 * the SD budget, audio comes every frame, open/close returns every byte, a save state replays exactly,
 * and the last picture is written as <out>/<rom>.png for a human to look at.
 *
 *   gbc_test <out-dir> <rom>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "nucleo_gbc.h"
/* The cartridge from internal flash (firmware nucleo_romstore, ABI 7): not on the host, so the cores keep their
 * RAM / page-cache paths, which is what these tests measure. */
const uint8_t *nucleo_rom_map(const char *p, uint32_t *s, void (*f)(uint32_t, uint32_t, void *), void *u)
{ (void)p; (void)f; (void)u; if (s) *s = 0; return NULL; }
void nucleo_rom_unmap(void) {}
void (*emu_rom_progress)(uint32_t, uint32_t, void *);

#define GB_BAND 1600   /* what play() holds while a game runs: two DMA band buffers */

/* ── the device heap, as a shape (same model as gb_cache_test.c) ─────────────────────────────────── */
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
    size_t take = ((n + 3) & ~(size_t)3) + 8;
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

/* ── sinks ───────────────────────────────────────────────────────────────────────────────────────── */
long g_pcm_writes, g_pcm_loud;
/* The device's I2S channel takes its DMA buffers from the same heap when the speaker opens: 6
 * descriptors x 240 frames x 2 bytes, mono. Charged to the model so the page count is the device's. */
static void *g_dma;
esp_err_t nucleo_audio_pcm_open(int rate, int channels) { (void)rate; g_dma = nucleo_hm_malloc(6 * 240 * 2 * channels + 256); return ESP_OK; }
esp_err_t nucleo_audio_pcm_open_ex(int rate, int channels, int queue_frames)
{   /* the driver: 8 descriptors of at least 240 frames each */
    (void)rate; int fr = (queue_frames + 7) / 8; if (fr < 240) fr = 240; if (fr > 480) fr = 480;
    g_dma = nucleo_hm_malloc((size_t)8 * fr * 2 * channels + 256); return ESP_OK;
}
esp_err_t nucleo_audio_pcm_write(const int16_t *pcm, size_t bytes)
{
    g_pcm_writes++;
    for (size_t i = 0; i < bytes / 2; i++) if (pcm[i] > 800 || pcm[i] < -800) { g_pcm_loud++; break; }
    return ESP_OK;
}
void nucleo_audio_pcm_close(void) { nucleo_hm_free(g_dma); g_dma = NULL; }

static long g_lines;
static unsigned long g_hash;
static uint8_t g_frame[144][160][3];
static void on_line(const uint16_t *px, int line, void *user)
{
    (void)user;
    g_lines++;
    for (int x = 0; x < 160; x++) {
        uint16_t v = (uint16_t)((px[x] >> 8) | (px[x] << 8));     /* panel order -> RGB565 */
        g_hash = g_hash * 31u + v;
        if (line >= 0 && line < 144) {
            g_frame[line][x][0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
            g_frame[line][x][1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            g_frame[line][x][2] = (uint8_t)((v & 31) * 255 / 31);
        }
    }
}
static int frame_colours(void)
{
    uint32_t seen[64]; int n = 0;
    for (int y = 0; y < 144; y += 3)
        for (int x = 0; x < 160; x += 3) {
            uint32_t c = (uint32_t)g_frame[y][x][0] << 16 | g_frame[y][x][1] << 8 | g_frame[y][x][2];
            int k = 0; while (k < n && seen[k] != c) k++;
            if (k == n && n < 64) seen[n++] = c;
        }
    return n;
}
/* Start through the title and menus, then walk right with A now and then. */
static uint8_t demo_buttons(int f)
{
    if (f < 180) return 0;
    int ph = (f / 20) % 6;
    if (f < 900) return ph == 0 ? NUCLEO_GB_START : ph == 3 ? NUCLEO_GB_A : 0;
    return (uint8_t)(NUCLEO_GB_RIGHT | (ph == 5 ? NUCLEO_GB_A : 0));
}

static uint32_t crc_tab[256];
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8]; put32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_upd(0, (const uint8_t *)type, 4); c = crc32_upd(c, d, n);
    uint8_t t[4]; put32(t, c); fwrite(t, 1, 4, f);
}
static int png_write(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) { raw[y * (1 + w * 3)] = 0; memcpy(raw + y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *o++ = (uint8_t)(off + n == raw_n); *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n); o += n;
    }
    put32(o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 0;
}

static int state_roundtrip(void)
{
    enum { WARMUP = 600, REPLAY = 180 };
    for (int f = 0; f < WARMUP; f++) { nucleo_gbc_set_buttons(demo_buttons(f)); nucleo_gbc_run_frame(); }
    if (nucleo_gbc_state_save(0) != ESP_OK) { printf("    FAIL: state save\n"); return 1; }
    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gbc_set_buttons(demo_buttons(WARMUP + f)); nucleo_gbc_run_frame(); }
    unsigned long first = g_hash;
    if (nucleo_gbc_state_load(0) != ESP_OK) { printf("    FAIL: state load\n"); return 1; }
    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gbc_set_buttons(demo_buttons(WARMUP + f)); nucleo_gbc_run_frame(); }
    if (first != g_hash) { printf("    FAIL: state round-trip diverged\n"); return 1; }
    printf("    state: round-trip exact over %d frames\n", REPLAY);
    return 0;
}

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gbc_test <out-dir> <rom>...\n"); return 2; }
    for (int i = 0; i < 256; i++) { uint32_t c = (uint32_t)i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    int failures = 0;
    for (int i = 2; i < argc; i++) {
        const char *rom = argv[i];
        printf("  %s\n", base_name(rom));
        hm_init();
        void *band0 = nucleo_hm_malloc(GB_BAND), *band1 = nucleo_hm_malloc(GB_BAND);
        size_t pre_free = nucleo_host_model_free(), pre_big = nucleo_host_model_largest();
        g_lines = 0; g_pcm_writes = g_pcm_loud = 0;
        esp_err_t e = nucleo_gbc_open(rom, on_line, NULL);
        if (e != ESP_OK) {
            printf("    FAIL: open -> 0x%x on a heap of %u B free, largest %u B\n", (unsigned)e, (unsigned)pre_free, (unsigned)pre_big);
            failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue;
        }
        size_t run_free = nucleo_host_model_free();
        nucleo_gb_stats_t s0; nucleo_gbc_get_stats(&s0);
        enum { FRAMES = 1800 };
        int colourful = 0;
        clock_t c0 = clock();
        for (int f = 0; f < FRAMES; f++) {
            nucleo_gbc_set_buttons(demo_buttons(f));
            nucleo_gbc_run_frame();
            if (f % 60 == 59 && frame_colours() >= 4) colourful++;
        }
        double host_us = (double)(clock() - c0) * 1e6 / CLOCKS_PER_SEC / FRAMES;
        nucleo_gb_stats_t s1; nucleo_gbc_get_stats(&s1);
        char title[48]; snprintf(title, sizeof title, "%s", nucleo_gbc_title());
        char png[700]; snprintf(png, sizeof png, "%s/%s.png", argv[1], base_name(rom));
        png_write(png, &g_frame[0][0][0], 160, 144);
        nucleo_gbc_close();
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        if (nucleo_host_model_free() != pre_free + 2 * (GB_BAND + 8)) {
            printf("    FAIL: the module leaked %d B across open/close\n", (int)(pre_free + 2 * (GB_BAND + 8)) - (int)nucleo_host_model_free());
            failures++; continue;
        }
        double per_frame = (double)s1.bank_misses / FRAMES;
        printf("    '%s' rom=%uKB pages=%d%s heap=%uB | heap %uB free, largest %uB -> %uB left while playing\n",
               title, (unsigned)(s1.rom_bytes / 1024), s1.rom_pages, s1.rom_resident ? " (all of it)" : "",
               (unsigned)s0.heap_bytes, (unsigned)pre_free, (unsigned)pre_big, (unsigned)run_free);
        printf("    lines=%ld colourful=%d/%d s  misses=%u (%.2f/frame)  audio frames=%ld (%ld audible)\n",
               g_lines, colourful, FRAMES / 60, (unsigned)s1.bank_misses, per_frame, g_pcm_writes, g_pcm_loud);
        printf("    host time %.0f us/frame (relative only: the PC, not the ESP32)\n", host_us);
        // A game switches the LCD off between screens (no lines while it is off): most frames, not all.
        if (g_lines < (long)FRAMES * 144 * 8 / 10) { printf("    FAIL: only %ld of %d lines\n", g_lines, FRAMES * 144); failures++; continue; }
        if (colourful < FRAMES / 60 / 3)   { printf("    FAIL: mostly blank pictures\n"); failures++; continue; }
        if (!s1.rom_resident && per_frame > 1.4) { printf("    FAIL: page cache thrashes\n"); failures++; continue; }
        if (g_pcm_writes != FRAMES)        { printf("    FAIL: audio not produced every frame\n"); failures++; continue; }
        hm_init();
        band0 = nucleo_hm_malloc(GB_BAND); band1 = nucleo_hm_malloc(GB_BAND);
        if (nucleo_gbc_open(rom, on_line, NULL) == ESP_OK) {
            int bad = state_roundtrip();
            nucleo_gbc_close();
            if (bad) { failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue; }
        }
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        printf("    PASS\n");
    }
    printf("%s\n", failures ? "gbc-test: FAILED" : "gbc-test: all passed");
    return failures ? 1 : 0;
}
