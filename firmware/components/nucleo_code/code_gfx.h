// code_gfx.h: the scripts' screen (docs/nucleo-code.md 4.5). A 16-colour canvas, 4 bits a pixel, kept in strips of
// 16 rows so no block bigger than ~2.5 KB is needed (240x135 = 16 KB, the ADV's 320x240 panel = 38 KB). Drawing
// marks the rows it touches; a flush converts just those rows to RGB565 and hands them to the platform in bands.
// The same canvas is what a visual test returns: a PNG (indexed, 4 bits, fixed-Huffman deflate) the model can see.
// Platform-neutral: the host gate compiles it as it is.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_COLORS 16
extern const uint32_t GFX_PALETTE[GFX_COLORS];   // 0xRRGGBB: black white gray dark-gray red orange yellow green
                                                 // dark-green cyan blue dark-blue purple pink brown beige

typedef struct gfx gfx_t;
typedef void (*gfx_blit_fn)(void *op, int x, int y, int w, int h, const uint16_t *rgb565);

gfx_t *gfx_new(int w, int h);                    // NULL = no memory (every strip or nothing)
void   gfx_free(gfx_t *g);
int    gfx_w(const gfx_t *g);
int    gfx_h(const gfx_t *g);

void gfx_clear(gfx_t *g, int c);
void gfx_pixel(gfx_t *g, int x, int y, int c);
int  gfx_get(const gfx_t *g, int x, int y);      // -1 outside
void gfx_rect(gfx_t *g, int x, int y, int w, int h, int c);      // filled
void gfx_frame(gfx_t *g, int x, int y, int w, int h, int c);     // outline
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, int c);
void gfx_circle(gfx_t *g, int cx, int cy, int r, int c, bool fill);
int  gfx_text(gfx_t *g, int x, int y, const char *s, size_t n, int c, int scale);   // width in pixels (6 * scale a char)

// The rows drawn since the last flush, in bands of up to 8 rows (blit NULL: only forgets them). Returns rows sent.
int  gfx_flush(gfx_t *g, gfx_blit_fn blit, void *op);
void gfx_touch_all(gfx_t *g);                    // the next flush sends every row (the screen was covered)

// The canvas as a PNG, every pixel `scale` x `scale` (1..3). false = the file could not be written.
bool gfx_png(const gfx_t *g, const char *path, int scale);

#ifdef __cplusplus
}
#endif
