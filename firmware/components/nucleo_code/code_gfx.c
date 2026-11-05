// code_gfx.c: the scripts' screen. See code_gfx.h. Store-only, like the rest of the runtime.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "code_gfx.h"
#include "code_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRIP     16            // rows a strip
#define MAXSTRIPS 16            // 256 rows at most
#define BAND      8             // rows converted and sent at once

// Sixteen colours, chosen to read well on both panels: the darks are not pure black, the lights are not neon.
const uint32_t GFX_PALETTE[GFX_COLORS] = {
    0x000000, 0xFFFFFF, 0x8C8C8C, 0x3C3C46,   // black, white, gray, dark gray
    0xE23B3B, 0xF28C28, 0xF5D93A, 0x5CC94A,   // red, orange, yellow, green
    0x24703A, 0x3FC8E0, 0x3070E0, 0x1F2A6B,   // dark green, cyan, blue, dark blue
    0x8A4FC8, 0xF08CC0, 0x7A5032, 0xE6CFA0,   // purple, pink, brown, beige
};

struct gfx {
    int w, h, stride, nstrips;
    int y0, y1;                 // rows drawn since the last flush: [y0, y1)
    uint16_t lut[GFX_COLORS];   // the palette in RGB565
    uint16_t *band;             // BAND rows of RGB565
    uint8_t *strip[MAXSTRIPS];
};

static inline uint8_t *rowp(const gfx_t *g, int y) { return g->strip[y / STRIP] + (size_t)(y % STRIP) * (size_t)g->stride; }
static inline void dirty(gfx_t *g, int ya, int yb) { if (ya < g->y0) g->y0 = ya; if (yb > g->y1) g->y1 = yb; }
static inline void put(gfx_t *g, int x, int y, int c)
{
    uint8_t *p = rowp(g, y) + (x >> 1);
    *p = (x & 1) ? (uint8_t)((*p & 0xF0) | c) : (uint8_t)((*p & 0x0F) | (c << 4));
}

gfx_t *gfx_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 640 || h > STRIP * MAXSTRIPS) return NULL;
    gfx_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->w = w; g->h = h; g->stride = (w + 1) / 2; g->nstrips = (h + STRIP - 1) / STRIP;
    for (int i = 0; i < g->nstrips; i++) {
        int rows = i == g->nstrips - 1 ? h - i * STRIP : STRIP;
        if (!(g->strip[i] = calloc(1, (size_t)rows * (size_t)g->stride))) { gfx_free(g); return NULL; }
    }
    if (!(g->band = malloc((size_t)w * BAND * sizeof(uint16_t)))) { gfx_free(g); return NULL; }
    for (int i = 0; i < GFX_COLORS; i++) {
        uint32_t c = GFX_PALETTE[i];
        g->lut[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c & 0xFF) >> 3));
    }
    g->y0 = 0; g->y1 = h;                      // the first flush paints the whole canvas
    return g;
}
void gfx_free(gfx_t *g)
{
    if (!g) return;
    for (int i = 0; i < MAXSTRIPS; i++) free(g->strip[i]);
    free(g->band); free(g);
}
int gfx_w(const gfx_t *g) { return g->w; }
int gfx_h(const gfx_t *g) { return g->h; }

void gfx_clear(gfx_t *g, int c)
{
    c &= 15;
    for (int i = 0; i < g->nstrips; i++) {
        int rows = i == g->nstrips - 1 ? g->h - i * STRIP : STRIP;
        memset(g->strip[i], c | c << 4, (size_t)rows * (size_t)g->stride);
    }
    dirty(g, 0, g->h);
}
void gfx_pixel(gfx_t *g, int x, int y, int c)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) return;
    put(g, x, y, c & 15); dirty(g, y, y + 1);
}
int gfx_get(const gfx_t *g, int x, int y)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) return -1;
    uint8_t b = rowp(g, y)[x >> 1];
    return (x & 1) ? (b & 15) : (b >> 4);
}
static void hline(gfx_t *g, int x0, int x1, int y, int c)     // [x0, x1], clipped, no dirty mark
{
    if (y < 0 || y >= g->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= g->w) x1 = g->w - 1;
    if (x0 > x1) return;
    uint8_t *r = rowp(g, y);
    if (x0 & 1) { put(g, x0, y, c); x0++; }
    if (x0 <= x1 && !(x1 & 1)) { put(g, x1, y, c); x1--; }
    if (x0 <= x1) memset(r + (x0 >> 1), c | c << 4, (size_t)((x1 - x0 + 1) >> 1));
}
void gfx_rect(gfx_t *g, int x, int y, int w, int h, int c)
{
    if (w <= 0 || h <= 0) return;
    c &= 15;
    int ya = y < 0 ? 0 : y, yb = y + h > g->h ? g->h : y + h;
    for (int yy = ya; yy < yb; yy++) hline(g, x, x + w - 1, yy, c);
    if (ya < yb) dirty(g, ya, yb);
}
void gfx_frame(gfx_t *g, int x, int y, int w, int h, int c)
{
    if (w <= 0 || h <= 0) return;
    gfx_rect(g, x, y, w, 1, c); gfx_rect(g, x, y + h - 1, w, 1, c);
    gfx_rect(g, x, y, 1, h, c); gfx_rect(g, x + w - 1, y, 1, h, c);
}
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, int c)
{
    c &= 15;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, ya = y0 < y1 ? y0 : y1, yb = (y0 < y1 ? y1 : y0) + 1;
    for (int guard = 0; guard < 4096; guard++) {
        if (x0 >= 0 && y0 >= 0 && x0 < g->w && y0 < g->h) put(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    if (ya < 0) ya = 0;
    if (yb > g->h) yb = g->h;
    if (ya < yb) dirty(g, ya, yb);
}
void gfx_circle(gfx_t *g, int cx, int cy, int r, int c, bool fill)
{
    if (r < 0) return;
    c &= 15;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (fill) {
            hline(g, cx - x, cx + x, cy + y, c); hline(g, cx - x, cx + x, cy - y, c);
            hline(g, cx - y, cx + y, cy + x, c); hline(g, cx - y, cx + y, cy - x, c);
        } else {
            int px[8] = { cx + x, cx - x, cx + x, cx - x, cx + y, cx - y, cx + y, cx - y };
            int py[8] = { cy + y, cy + y, cy - y, cy - y, cy + x, cy + x, cy - x, cy - x };
            for (int i = 0; i < 8; i++) if (px[i] >= 0 && py[i] >= 0 && px[i] < g->w && py[i] < g->h) put(g, px[i], py[i], c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    int ya = cy - r < 0 ? 0 : cy - r, yb = cy + r + 1 > g->h ? g->h : cy + r + 1;
    if (ya < yb) dirty(g, ya, yb);
}
int gfx_text(gfx_t *g, int x, int y, const char *s, size_t n, int c, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    c &= 15;
    int cx = x, cy = y, wmax = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') { if (cx - x > wmax) wmax = cx - x; cx = x; cy += 8 * scale; continue; }
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *gl = CODE_FONT5X7 + (ch - 32) * 5;
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 8; row++)
                if (gl[col] >> row & 1) {
                    int px = cx + col * scale, py = cy + row * scale;
                    if (scale == 1) { if (px >= 0 && py >= 0 && px < g->w && py < g->h) put(g, px, py, c); }
                    else for (int k = 0; k < scale; k++) hline(g, px, px + scale - 1, py + k, c);
                }
        cx += 6 * scale;
    }
    if (cx - x > wmax) wmax = cx - x;
    int ya = y < 0 ? 0 : y, yb = cy + 8 * scale > g->h ? g->h : cy + 8 * scale;
    if (ya < yb) dirty(g, ya, yb);
    return wmax;
}

int gfx_flush(gfx_t *g, gfx_blit_fn blit, void *op)
{
    int y0 = g->y0 < 0 ? 0 : g->y0, y1 = g->y1 > g->h ? g->h : g->y1, sent = 0;
    g->y0 = g->h; g->y1 = 0;
    if (!blit || y0 >= y1) return 0;
    for (int y = y0; y < y1; y += BAND) {
        int n = y1 - y < BAND ? y1 - y : BAND;
        for (int r = 0; r < n; r++) {
            const uint8_t *s = rowp(g, y + r);
            uint16_t *o = g->band + (size_t)r * (size_t)g->w;
            for (int x = 0; x < g->w; x += 2) {
                uint8_t b = s[x >> 1];
                o[x] = g->lut[b >> 4];
                if (x + 1 < g->w) o[x + 1] = g->lut[b & 15];
            }
        }
        blit(op, 0, y, g->w, n, g->band);
        sent += n;
    }
    return sent;
}
void gfx_touch_all(gfx_t *g) { g->y0 = 0; g->y1 = g->h; }

// ---- PNG: indexed colour, 4 bits a pixel, one fixed-Huffman deflate block --------------------------------------
// A script's screen is flat colour, so two match distances find nearly everything: 1 (a run of one byte) and one
// scanline (the row above, which an upscaled image repeats exactly). No hash tables, no window buffer.
typedef struct { FILE *f; uint32_t crc, bits; int nbits; bool err; } pw_t;

static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}
static void pw_bytes(pw_t *w, const void *p, size_t n)
{
    if (w->err) return;
    if (fwrite(p, 1, n, w->f) != n) w->err = true;
    w->crc = crc32_upd(w->crc, (const uint8_t *)p, n);
}
static void pw_be32(pw_t *w, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; pw_bytes(w, b, 4); }
static void pw_chunk(pw_t *w, const char *type, const uint8_t *data, uint32_t n)
{
    pw_be32(w, n);
    w->crc = 0; pw_bytes(w, type, 4); if (n) pw_bytes(w, data, n);
    pw_be32(w, w->crc);
}
static void bits_put(pw_t *w, uint32_t v, int n)            // deflate: least significant bit first
{
    w->bits |= v << w->nbits; w->nbits += n;
    while (w->nbits >= 8) { uint8_t b = (uint8_t)w->bits; pw_bytes(w, &b, 1); w->bits >>= 8; w->nbits -= 8; }
}
static void huff_put(pw_t *w, uint32_t code, int len)       // a Huffman code goes most significant bit first
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
    bits_put(w, r, len);
}
static void sym_put(pw_t *w, int s)                          // the fixed literal/length code
{
    if (s < 144) huff_put(w, 0x30u + (uint32_t)s, 8);
    else if (s < 256) huff_put(w, 0x190u + (uint32_t)(s - 144), 9);
    else if (s < 280) huff_put(w, (uint32_t)(s - 256), 7);
    else huff_put(w, 0xC0u + (uint32_t)(s - 280), 8);
}
static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                    4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t DEXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static void match_put(pw_t *w, int len, int dist)
{
    int i = 28; while (LBASE[i] > len) i--;
    sym_put(w, 257 + i); if (LEXTRA[i]) bits_put(w, (uint32_t)(len - LBASE[i]), LEXTRA[i]);
    int d = 29; while (DBASE[d] > dist) d--;
    huff_put(w, (uint32_t)d, 5); if (DEXTRA[d]) bits_put(w, (uint32_t)(dist - DBASE[d]), DEXTRA[d]);
}
// one scanline (filter byte included); `prev` = the scanline before it, or NULL for the first
static void row_put(pw_t *w, const uint8_t *cur, const uint8_t *prev, int n)
{
    for (int i = 0; i < n; ) {
        int l1 = 0, l2 = 0;
        if (i >= 1) while (i + l1 < n && l1 < 258 && cur[i + l1] == cur[i - 1]) l1++;
        if (prev) while (i + l2 < n && l2 < 258 && cur[i + l2] == prev[i + l2]) l2++;
        if (l2 >= 3 && l2 >= l1) { match_put(w, l2, n); i += l2; }
        else if (l1 >= 3) { match_put(w, l1, 1); i += l1; }
        else sym_put(w, cur[i++]);
    }
}

bool gfx_png(const gfx_t *g, const char *path, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    const int W = g->w * scale, H = g->h * scale, rb = (W + 1) / 2, n = rb + 1;
    uint8_t *cur = malloc((size_t)n), *prev = malloc((size_t)n);
    FILE *f = (cur && prev) ? fopen(path, "wb") : NULL;
    if (!f) { free(cur); free(prev); return false; }
    pw_t w = { f, 0, 0, 0, false };
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    pw_bytes(&w, SIG, 8);
    uint8_t ihdr[13] = { (uint8_t)(W >> 24), (uint8_t)(W >> 16), (uint8_t)(W >> 8), (uint8_t)W,
                         (uint8_t)(H >> 24), (uint8_t)(H >> 16), (uint8_t)(H >> 8), (uint8_t)H, 4, 3, 0, 0, 0 };
    pw_chunk(&w, "IHDR", ihdr, 13);
    uint8_t plte[GFX_COLORS * 3];
    for (int i = 0; i < GFX_COLORS; i++) { plte[3 * i] = (uint8_t)(GFX_PALETTE[i] >> 16); plte[3 * i + 1] = (uint8_t)(GFX_PALETTE[i] >> 8); plte[3 * i + 2] = (uint8_t)GFX_PALETTE[i]; }
    pw_chunk(&w, "PLTE", plte, sizeof plte);
    // IDAT: its length is known only at the end, so it is written last over a placeholder
    long at = ftell(f);
    pw_be32(&w, 0);
    w.crc = 0; pw_bytes(&w, "IDAT", 4);
    static const uint8_t ZHDR[2] = { 0x78, 0x01 };
    pw_bytes(&w, ZHDR, 2);
    bits_put(&w, 1, 1); bits_put(&w, 1, 2);                // the final block, fixed codes
    uint32_t a1 = 1, a2 = 0;                                // Adler-32 of the raw scanlines
    for (int y = 0; y < H; y++) {
        cur[0] = 0;                                         // filter: none
        const int sy = y / scale;
        for (int j = 0; j < rb; j++) {
            int x0 = (2 * j) / scale, x1 = 2 * j + 1 < W ? (2 * j + 1) / scale : -1;
            int p0 = gfx_get(g, x0, sy), p1 = x1 >= 0 ? gfx_get(g, x1, sy) : 0;
            cur[1 + j] = (uint8_t)((p0 & 15) << 4 | (p1 & 15));
        }
        row_put(&w, cur, y ? prev : NULL, n);
        for (int i = 0; i < n; i++) { a1 = (a1 + cur[i]) % 65521u; a2 = (a2 + a1) % 65521u; }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    sym_put(&w, 256);                                       // end of block
    if (w.nbits) { uint8_t b = (uint8_t)w.bits; pw_bytes(&w, &b, 1); w.bits = 0; w.nbits = 0; }
    pw_be32(&w, (a2 << 16) | a1);
    long end = ftell(f);
    uint32_t crc = w.crc;
    uint8_t cb[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    if (fwrite(cb, 1, 4, f) != 4) w.err = true;
    uint32_t len = (uint32_t)(end - at - 8);
    uint8_t lb[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    if (fseek(f, at, SEEK_SET) || fwrite(lb, 1, 4, f) != 4 || fseek(f, 0, SEEK_END)) w.err = true;
    pw_chunk(&w, "IEND", NULL, 0);
    bool ok = (fclose(f) == 0) && !w.err;
    free(cur); free(prev);
    return ok;
}
#endif
