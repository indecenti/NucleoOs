/* gg_test — host gate for the NATIVE Game Gear emulator, compiled against the REAL firmware source.
 *
 * firmware/components/nucleo_emu/nucleo_gg.c is compiled with heap_model.h force-included, so every
 * allocation it makes answers from a MODEL of the Solo-boot heap (largest block 32 KB) — an open that
 * could not succeed on the device cannot succeed here. Two modes:
 *
 *   gg_test <rom>...                       the GATE: per cartridge, frames really render, the page cache
 *                                          stays under the SD budget, open/close returns every byte,
 *                                          and a save state replays bit-exactly.
 *   gg_test --sweep <out-dir> <rom>...     the LIBRARY run: every cartridge for 30 s of demo input,
 *                                          one line of verdict each, and contact sheets (PNG, 5x5
 *                                          screenshots) so a human can SEE what "renders" means.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nucleo_gg.h"

/* What play() holds while a Game Gear runs: two DMA band buffers of 1600 B (app_gbemu.cpp:
 * band_bytes(): 5 rows x 160 px, or 3 x 240 / 3 x 180 for the filled and Master System views). */
#define GG_BAND 1600

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
esp_err_t nucleo_audio_pcm_write(const int16_t *pcm, size_t bytes)
{
    g_pcm_writes++;
    for (size_t i = 0; i < bytes / 2; i++) if (pcm[i] > 800 || pcm[i] < -800) { g_pcm_loud++; break; }
    return ESP_OK;
}
void nucleo_audio_pcm_close(void) { nucleo_hm_free(g_dma); g_dma = NULL; }

static long g_lines;
static unsigned long g_hash;
static uint8_t g_frame[192][256][3];    /* last picture, RGB888 (Master System mode is 256x192) */
static int g_w = NUCLEO_GG_W, g_h = NUCLEO_GG_H;   /* the session's geometry, set after each open */
static void on_line(const uint16_t *px, int line, void *user)
{
    (void)user;
    g_lines++;
    for (int x = 0; x < g_w; x++) {
        uint16_t v = (uint16_t)((px[x] >> 8) | (px[x] << 8));     /* panel order -> RGB565 */
        g_hash = g_hash * 31u + v;
        if (line >= 0 && line < g_h) {
            g_frame[line][x][0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
            g_frame[line][x][1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            g_frame[line][x][2] = (uint8_t)((v & 31) * 255 / 31);
        }
    }
}

/* Distinct colours in the last picture — "rendered something" rather than "rendered a flat field". */
static int frame_colours(void)
{
    uint32_t seen[64]; int n = 0;
    for (int y = 0; y < g_h; y += 3)
        for (int x = 0; x < g_w; x += 3) {
            uint32_t c = (uint32_t)g_frame[y][x][0] << 16 | g_frame[y][x][1] << 8 | g_frame[y][x][2];
            int k = 0; while (k < n && seen[k] != c) k++;
            if (k == n && n < 64) seen[n++] = c;
        }
    return n;
}

/* Start, then button 2, then walk right holding 1 — what a person does to get past a title screen
 * and into a level, which is where a cartridge's banks, sprites and scrolling actually get used. */
static int g_idle = -1;
static uint8_t demo_buttons(int f)
{
    if (g_idle < 0) g_idle = getenv("GG_IDLE") != NULL;
    if (f < 180 || g_idle) return 0;
    int ph = (f / 20) % 6;
    if (f < 900) return ph == 0 ? NUCLEO_GB_START : ph == 3 ? NUCLEO_GB_A : 0;
    return (uint8_t)(NUCLEO_GB_RIGHT | ((f / 30) % 3 == 0 ? NUCLEO_GB_B : 0) | (ph == 5 ? NUCLEO_GB_A : 0));
}

/* ── a PNG with stored (uncompressed) deflate blocks: no zlib, still opens everywhere ────────────── */
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

/* ── save states replay bit-exactly (the one feature whose failure is silent) ──────────────────────── */
static int state_roundtrip(void)
{
    enum { WARMUP = 600, REPLAY = 180 };
    for (int f = 0; f < WARMUP; f++) { nucleo_gg_set_buttons(demo_buttons(f)); nucleo_gg_run_frame(); }
    if (nucleo_gg_state_save(0) != ESP_OK) { printf("    FAIL: state save\n"); return 1; }
    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gg_set_buttons(demo_buttons(WARMUP + f)); nucleo_gg_run_frame(); }
    unsigned long first = g_hash;
    if (nucleo_gg_state_load(0) != ESP_OK) { printf("    FAIL: state load\n"); return 1; }
    g_hash = 1469598103u;
    for (int f = 0; f < REPLAY; f++) { nucleo_gg_set_buttons(demo_buttons(WARMUP + f)); nucleo_gg_run_frame(); }
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

/* The round-trip's state file lives beside the game's saves ("<root>/Saves/<sys>/<name>.st0" for
 * "<root>/ROMs/<sys>/<name>.<ext>"): leave no litter in the library the simulator serves. */
static void remove_state(const char *rom)
{
    char p[600];
    const char *r = strstr(rom, "ROMs");
    const char *dot = strrchr(rom, '.');
    if (!r || !dot || dot < r) return;
    snprintf(p, sizeof p, "%.*sSaves%.*s.st0", (int)(r - rom), rom, (int)(dot - (r + 4)), r + 4);
    remove(p);
}

static int gate(int argc, char **argv)
{
    int failures = 0;
    for (int i = 0; i < argc; i++) {
        const char *rom = argv[i];
        printf("  %s\n", base_name(rom));
        hm_init();
        void *band0 = nucleo_hm_malloc(GG_BAND), *band1 = nucleo_hm_malloc(GG_BAND);   /* what play() holds */
        size_t pre_free = nucleo_host_model_free(), pre_big = nucleo_host_model_largest();
        g_lines = 0; g_pcm_writes = g_pcm_loud = 0;
        esp_err_t e = nucleo_gg_open(rom, on_line, NULL);
        if (e != ESP_OK) {
            printf("    FAIL: open -> 0x%x on a heap of %u B free, largest %u B\n", (unsigned)e, (unsigned)pre_free, (unsigned)pre_big);
            failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue;
        }
        size_t run_free = nucleo_host_model_free();
        nucleo_gg_geometry(&g_w, &g_h);
        nucleo_gb_stats_t s0; nucleo_gg_get_stats(&s0);
        enum { FRAMES = 1800 };
        int colourful = 0;
        for (int f = 0; f < FRAMES; f++) {
            nucleo_gg_set_buttons(demo_buttons(f));
            nucleo_gg_run_frame();
            if (f % 60 == 59 && frame_colours() >= 4) colourful++;
        }
        nucleo_gb_stats_t s1; nucleo_gg_get_stats(&s1);
        char title[48]; snprintf(title, sizeof title, "%s", nucleo_gg_title());
        nucleo_gg_close();
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        if (nucleo_host_model_free() != pre_free + 2 * (GG_BAND + 8)) {
            printf("    FAIL: the module leaked %d B across open/close\n", (int)(pre_free + 2 * (GG_BAND + 8)) - (int)nucleo_host_model_free());
            failures++; continue;
        }
        double per_frame = (double)s1.bank_misses / FRAMES;
        printf("    '%s' rom=%uKB pages=%d%s heap=%uB | device heap %uB free, largest %uB -> %uB left while playing\n",
               title, (unsigned)(s1.rom_bytes / 1024), s1.rom_pages, s1.rom_resident ? " (all of it)" : "",
               (unsigned)s0.heap_bytes, (unsigned)pre_free, (unsigned)pre_big, (unsigned)run_free);
        printf("    lines=%ld colourful=%d/%d s  misses=%u (%.2f/frame)  audio frames=%ld (%ld audible)\n",
               g_lines, colourful, FRAMES / 60, (unsigned)s1.bank_misses, per_frame, g_pcm_writes, g_pcm_loud);
        if (g_lines != (long)FRAMES * g_h) { printf("    FAIL: expected %d lines\n", FRAMES * g_h); failures++; continue; }
        if (colourful < FRAMES / 60 / 3)          { printf("    FAIL: mostly blank pictures\n"); failures++; continue; }
        if (!s1.rom_resident && per_frame > 1.4)   { printf("    FAIL: page cache thrashes\n"); failures++; continue; }
        if (g_pcm_writes != FRAMES)                { printf("    FAIL: audio not produced every frame\n"); failures++; continue; }
        hm_init();
        band0 = nucleo_hm_malloc(GG_BAND); band1 = nucleo_hm_malloc(GG_BAND);
        if (nucleo_gg_open(rom, on_line, NULL) == ESP_OK) {
            int bad = state_roundtrip();
            nucleo_gg_close();
            remove_state(rom);
            if (bad) { failures++; nucleo_hm_free(band0); nucleo_hm_free(band1); continue; }
        }
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        printf("    PASS\n");
    }
    return failures;
}

/* ── the library, all of it ──────────────────────────────────────────────────────────────────────── */
#define SHEET_N 5
static int sweep(const char *out_dir, int argc, char **argv)
{
    enum { FRAMES = 1800, SHOT = 1500 };
    int W = SHEET_N * (NUCLEO_GG_W + 4), H = SHEET_N * (NUCLEO_GG_H + 4);
    uint8_t *sheet = calloc((size_t)W * H, 3);
    int on_sheet = 0, sheets = 0, bad = 0;
    char idx_path[600]; snprintf(idx_path, sizeof idx_path, "%s/index.txt", out_dir);
    FILE *ix = fopen(idx_path, "w");
    for (int i = 0; i < argc; i++) {
        hm_init();
        void *band0 = nucleo_hm_malloc(GG_BAND), *band1 = nucleo_hm_malloc(GG_BAND);
        g_lines = 0; g_pcm_writes = g_pcm_loud = 0;
        memset(g_frame, 0, sizeof g_frame);
        esp_err_t e = nucleo_gg_open(argv[i], on_line, NULL);
        int colourful = 0, changes = 0; unsigned long last = 0; double mpf = 0; nucleo_gb_stats_t st = { 0 };
        uint8_t shot[NUCLEO_GG_H][NUCLEO_GG_W][3];
        memset(shot, 0, sizeof shot);
        if (e == ESP_OK) {
            nucleo_gg_geometry(&g_w, &g_h);
            for (int f = 0; f < FRAMES; f++) {
                g_hash = 1469598103u;
                nucleo_gg_set_buttons(demo_buttons(f));
                nucleo_gg_run_frame();
                if (f % 60 == 59) { if (frame_colours() >= 4) colourful++; if (g_hash != last) changes++; last = g_hash; }
                if (f == SHOT)          /* a Master System picture is shrunk into the 160x144 cell */
                    for (int y = 0; y < NUCLEO_GG_H; y++) for (int x = 0; x < NUCLEO_GG_W; x++)
                        memcpy(shot[y][x], g_frame[y * g_h / NUCLEO_GG_H][x * g_w / NUCLEO_GG_W], 3);
            }
            nucleo_gg_get_stats(&st);
            mpf = (double)st.bank_misses / FRAMES;
            nucleo_gg_close();
        }
        nucleo_hm_free(band0); nucleo_hm_free(band1);
        const char *verdict = e != ESP_OK ? "OPEN-FAIL" : colourful < 5 ? "BLANK" : changes < 4 ? "STATIC" : "ok";
        if (strcmp(verdict, "ok")) bad++;
        int cell = on_sheet;
        int ox = (cell % SHEET_N) * (NUCLEO_GG_W + 4) + 2, oy = (cell / SHEET_N) * (NUCLEO_GG_H + 4) + 2;
        for (int y = 0; y < NUCLEO_GG_H; y++) memcpy(sheet + ((size_t)(oy + y) * W + ox) * 3, shot[y], NUCLEO_GG_W * 3);
        if (strcmp(verdict, "ok"))                          /* a red frame around anything suspicious */
            for (int y = oy - 2; y < oy + NUCLEO_GG_H + 2; y++) for (int x = ox - 2; x < ox + NUCLEO_GG_W + 2; x++)
                if (y < oy || y >= oy + NUCLEO_GG_H || x < ox || x >= ox + NUCLEO_GG_W) { uint8_t *p = sheet + ((size_t)y * W + x) * 3; p[0] = 255; p[1] = p[2] = 0; }
        printf("%-10s sheet%02d#%02d misses/f=%.2f colourful=%2d changes=%2d audio=%4ld  %s\n",
               verdict, sheets, cell + 1, mpf, colourful, changes, g_pcm_loud, base_name(argv[i]));
        if (ix) fprintf(ix, "%s\tsheet%02d#%02d\t%.2f\t%d\t%d\t%ld\t%s\n", verdict, sheets, cell + 1, mpf, colourful, changes, g_pcm_loud, base_name(argv[i]));
        fflush(stdout);
        if (++on_sheet == SHEET_N * SHEET_N || i == argc - 1) {
            char p[600]; snprintf(p, sizeof p, "%s/sheet%02d.png", out_dir, sheets);
            png_write(p, sheet, W, H);
            memset(sheet, 0, (size_t)W * H * 3);
            on_sheet = 0; sheets++;
        }
    }
    if (ix) fclose(ix);
    free(sheet);
    printf("\n%d cartridges, %d flagged, %d contact sheets in %s\n", argc, bad, sheets, out_dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--sweep")) return sweep(argv[2], argc - 3, argv + 3);
    if (argc >= 4 && !strcmp(argv[1], "--shot")) {          /* --shot <out.png> <frames> <rom> */
        hm_init();
        if (nucleo_gg_open(argv[4], on_line, NULL) != ESP_OK) return 1;
        nucleo_gg_geometry(&g_w, &g_h);
        int n = atoi(argv[3]);
        for (int f = 0; f < n; f++) { nucleo_gg_set_buttons(demo_buttons(f)); nucleo_gg_run_frame(); }
        nucleo_gg_close();
        static uint8_t out[192 * 256 * 3];
        for (int y = 0; y < g_h; y++) memcpy(out + (size_t)y * g_w * 3, g_frame[y], (size_t)g_w * 3);
        return png_write(argv[2], out, g_w, g_h);
    }
    if (argc < 2) { fprintf(stderr, "usage: gg_test <rom>... | --sweep <dir> <rom>... | --shot <png> <frames> <rom>\n"); return 2; }
    return gate(argc - 1, argv + 1) ? 1 : 0;
}
