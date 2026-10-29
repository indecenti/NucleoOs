// gameshots.cpp — see gameshots.h. Screenshots (Fn+P) and game covers captured on the device, as 24-bit BMPs
// streamed row by row (no big allocation). The Games gallery (gamefront.cpp) shows the covers.
#include "gameshots.h"
#include "nucleo_ui.h"      // panel readback for Solo-mode / direct-draw screenshots
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
extern "C" {
#include "nucleo_board.h"
}

extern M5GFX d;                    // the real display (nucleo_ui.cpp)
M5Canvas *nucleo_screen(void);     // the shared off-screen canvas


// A cover is captured at the box the old carousel showed it in: 128x72 (16:9), or 44x72 for a game whose
// table is portrait (pinball). The gallery fits either into its own picture tile.
#define COVER_W      128
#define COVER_W_PORT 44
#define COVER_H      72
static bool cover_portrait(const char *id) { return !strcmp(id, "pinball"); }
static int  cover_w(const char *id) { return cover_portrait(id) ? COVER_W_PORT : COVER_W; }

// ---- on-device capture (24-bit BMP, streamed row-by-row; no big alloc) -------
// Recursively create every component of a directory path (FATFS mkdir only makes the last level;
// if /sd/data didn't exist, a single mkdir("/sd/data/GameShots") fails and the cover never gets
// written — which is exactly why captured covers weren't showing up).
static void ensure_dir(const char *path)
{
    char t[192]; size_t n = strlen(path); if (n == 0 || n >= sizeof t) return;
    memcpy(t, path, n + 1);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
    mkdir(t, 0777);
}

static void put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }

// Row scratch for the BMP writers. Screenshots and covers are rare, so it lives on the heap only while
// a file is written (it was 4.9 KB of .bss, resident even with GameFront closed). Allocated before the
// file is opened: no memory means no truncated file.
struct BmpRows {
    union { lgfx::rgb888_t rgb[320]; uint16_t raw[320]; } in;
    uint8_t out[320 * 3 + 4];
};

// Save a 24-bit BMP of dstW x dstH, sampled from the source crop rect (sx0,sy0,cw,ch). The crop lets
// the cover preserve the game's aspect (centre-crop) instead of squashing the whole frame.
static bool save_bmp(LovyanGFX *src, const char *path, int dstW, int dstH,
                     int sx0, int sy0, int cw, int ch)
{
    int sw = src->width(), sh = src->height();
    if (sw <= 0 || sh <= 0 || dstW <= 0 || dstH <= 0 || dstW > 320) return false;
    if (cw <= 0 || ch <= 0) { sx0 = 0; sy0 = 0; cw = sw; ch = sh; }   // no crop -> full frame
    BmpRows *r = (BmpRows *)malloc(sizeof *r); if (!r) return false;
    FILE *f = fopen(path, "wb"); if (!f) { free(r); return false; }

    int rowSize = (dstW * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * dstH;
    uint8_t h[54]; memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M'; put32(h + 2, 54 + imgSize); put32(h + 10, 54);
    put32(h + 14, 40); put32(h + 18, dstW); put32(h + 22, dstH);
    put16(h + 26, 1); put16(h + 28, 24); put32(h + 38, 2835); put32(h + 42, 2835);
    fwrite(h, 1, 54, f);

    // Use rgb888_t so M5GFX handles hardware-display endianness and RGB/BGR conversion
    // internally. Reading as uint16_t from a hardware panel returns big-endian bytes
    // that get misinterpreted as little-endian RGB565, corrupting all colours.
    lgfx::rgb888_t *prow = r->in.rgb;
    uint8_t *orow = r->out;
    for (int oy = 0; oy < dstH; oy++) {
        int sy = sy0 + oy * ch / dstH; if (sy >= sh) sy = sh - 1; if (sy < 0) sy = 0;
        src->readRect(0, sy, sw, 1, prow);
        memset(orow, 0, rowSize);
        for (int ox = 0; ox < dstW; ox++) {
            int sx = sx0 + ox * cw / dstW; if (sx >= sw) sx = sw - 1; if (sx < 0) sx = 0;
            orow[ox * 3 + 0] = prow[sx].b;   // BMP stores BGR
            orow[ox * 3 + 1] = prow[sx].g;
            orow[ox * 3 + 2] = prow[sx].r;
        }
        fseek(f, 54 + (long)(dstH - 1 - oy) * rowSize, SEEK_SET);   // BMP rows are bottom-up
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    free(r);
    return true;
}

void gameshots_name(char *out, int cap)
{
    static int s_next = 0;                        // 0 = card not scanned yet this boot
    time_t now = time(NULL);
    if (now >= 1672531200) {                      // clock set (NTP/manual): a sortable, meaningful name
        struct tm tmv; localtime_r(&now, &tmv);
        snprintf(out, cap, "shot_%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        struct stat st; char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, out);
        if (stat(p, &st) != 0) return;            // two shots in the same second fall through to shot_N
    }
    if (s_next == 0) {
        s_next = 1;
        DIR *dp = opendir(SHOT_DIR);
        if (dp) {
            struct dirent *e;
            while ((e = readdir(dp)) != nullptr) {
                const char *n = e->d_name;
                if (strncmp(n, "shot_", 5) || !isdigit((unsigned char)n[5]) || strchr(n + 5, '_')) continue;
                int v = atoi(n + 5);
                if (v >= s_next) s_next = v + 1;
            }
            closedir(dp);
        }
    }
    snprintf(out, cap, "shot_%d", s_next++);
}

bool gameshots_save_canvas(const char *name)
{
    M5Canvas *c = nucleo_screen(); if (!c) return false;
    ensure_dir(SHOT_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, name && name[0] ? name : "shot");
    return save_bmp(c, p, c->width(), c->height(), 0, 0, c->width(), c->height());
}

// Read the PHYSICAL PANEL (nucleo_ui_read_row) into a full-frame 24-bit BMP at `path`. Works with no
// network and no off-screen canvas (Solo boot, direct-draw apps) — a local SD write. Overwrites.
static bool panel_bmp_to(const char *path)
{
    int w = 0, h = 0; nucleo_ui_panel_size(&w, &h);
    if (w <= 0 || h <= 0 || w > 320) return false;
    BmpRows *r = (BmpRows *)malloc(sizeof *r); if (!r) return false;
    FILE *f = fopen(path, "wb"); if (!f) { free(r); return false; }
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * h;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, w); put32(hd + 22, h);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);
    uint16_t *prow = r->in.raw;
    uint8_t *orow = r->out;
    for (int y = h - 1; y >= 0; y--) {                 // BMP rows bottom-up
        if (!nucleo_ui_read_row(y, w, prow)) memset(prow, 0, (size_t)w * 2);
        memset(orow, 0, rowSize);
        for (int x = 0; x < w; x++) {
            uint16_t raw = prow[x];
            uint16_t pp = (uint16_t)((raw >> 8) | (raw << 8));   // un-swap the panel's readback byte order
            orow[x * 3 + 0] = (uint8_t)(( pp        & 0x1F) * 255 / 31);   // BMP is BGR
            orow[x * 3 + 1] = (uint8_t)(((pp >> 5)  & 0x3F) * 255 / 63);
            orow[x * 3 + 2] = (uint8_t)(((pp >> 11) & 0x1F) * 255 / 31);
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    free(r);
    return true;
}

// Full-frame panel screenshot -> /data/Screenshots/<name>.bmp (direct-draw / Solo-boot apps).
bool gameshots_save_panel(const char *name)
{
    ensure_dir(SHOT_DIR);
    char p[192]; snprintf(p, sizeof p, "%s/%s.bmp", SHOT_DIR, name && name[0] ? name : "shot");
    return panel_bmp_to(p);
}

// Full-frame panel shot used AS the carousel cover -> /data/GameShots/<id>.bmp (overwrites; no history).
// The whole panel is FIT (letterboxed, aspect preserved) into the exact hero‑box size, so it drops in
// 1:1 and always sits perfectly inside the carousel frame — never cropped, never overflowing.
bool gameshots_panel_cover(const char *id)
{
    if (!id || !id[0]) return false;
    int pw = 0, ph = 0; nucleo_ui_panel_size(&pw, &ph);
    if (pw <= 0 || ph <= 0 || pw > 320) return false;
    ensure_dir(GF_DIR);
    char path[192]; snprintf(path, sizeof path, "%s/%s.bmp", GF_DIR, id);
    BmpRows *r = (BmpRows *)malloc(sizeof *r); if (!r) return false;
    FILE *f = fopen(path, "wb"); if (!f) { free(r); return false; }

    const bool portrait = cover_portrait(id);
    const int W = cover_w(id), H = COVER_H;   // output = the cover's box for this game's shape
    float sx = (float)W / pw, sy = (float)H / ph, s = sx < sy ? sx : sy;   // fit (letterbox)
    int dw = (int)(pw * s + 0.5f), dh = (int)(ph * s + 0.5f);
    if (dw > W) dw = W;
    if (dh > H) dh = H;
    int offX = (W - dw) / 2, offY = (H - dh) / 2;     // centred; black bars fill the rest

    int rowSize = (W * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * H;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, W); put32(hd + 22, H);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);

    uint16_t *prow = r->in.raw;
    uint8_t *orow = r->out;

    // PORTRAIT box (pinball): un-rotate the 90°-rotated portrait playfield so the cover is UPRIGHT and
    // fills the tall box (see the matching branch in gameshots_canvas_cover). Column-by-column.
    if (portrait) {
        for (int oy = H - 1; oy >= 0; oy--) {           // BMP bottom-up; oy = row-from-top (0 = table top)
            int cx = (pw - 1) - oy * pw / H; if (cx < 0) cx = 0; if (cx >= pw) cx = pw - 1;
            d.readRect(cx, 0, 1, ph, prow);             // one panel column (byte-swapped RGB565)
            memset(orow, 0, rowSize);
            for (int ox = 0; ox < W; ox++) {
                int cy = ox * ph / W; if (cy < 0) cy = 0; if (cy >= ph) cy = ph - 1;
                uint16_t raw = prow[cy], pp = (uint16_t)((raw >> 8) | (raw << 8));
                orow[ox * 3 + 0] = (uint8_t)(( pp        & 0x1F) * 255 / 31);   // BGR
                orow[ox * 3 + 1] = (uint8_t)(((pp >> 5)  & 0x3F) * 255 / 63);
                orow[ox * 3 + 2] = (uint8_t)(((pp >> 11) & 0x1F) * 255 / 31);
            }
            fwrite(orow, 1, rowSize, f);
        }
        fclose(f);
        free(r);
        return true;
    }

    for (int vy = H - 1; vy >= 0; vy--) {             // BMP rows bottom-up; vy = row-from-top
        memset(orow, 0, rowSize);                     // letterbox bars stay black
        if (vy >= offY && vy < offY + dh) {
            int srcY = (int)((vy - offY) / s); if (srcY < 0) srcY = 0; if (srcY >= ph) srcY = ph - 1;
            if (nucleo_ui_read_row(srcY, pw, prow)) {
                for (int ix = 0; ix < dw; ix++) {
                    int srcX = (int)(ix / s); if (srcX < 0) srcX = 0; if (srcX >= pw) srcX = pw - 1;
                    uint16_t raw = prow[srcX], pp = (uint16_t)((raw >> 8) | (raw << 8));
                    int dx = offX + ix;
                    orow[dx * 3 + 0] = (uint8_t)(( pp        & 0x1F) * 255 / 31);   // BGR
                    orow[dx * 3 + 1] = (uint8_t)(((pp >> 5)  & 0x3F) * 255 / 63);
                    orow[dx * 3 + 2] = (uint8_t)(((pp >> 11) & 0x1F) * 255 / 31);
                }
            }
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    free(r);
    return true;
}

// Carousel cover from the OFF-SCREEN CANVAS (RAM read — cheap, no slow panel readback). Scales the
// live frame to the exact hero box for this game's shape (full frame, aspect preserved → fills the
// box with no bars). Used to refresh a game's cover from live gameplay while it runs. Overwrites;
// no history kept.
bool gameshots_canvas_cover(const char *id)
{
    if (!id || !id[0]) return false;
    M5Canvas *c = nucleo_screen();
    if (!c) return false;
    int pw = c->width(), ph = c->height();
    if (pw <= 0 || ph <= 0 || pw > 320) return false;
    ensure_dir(GF_DIR);
    char path[192]; snprintf(path, sizeof path, "%s/%s.bmp", GF_DIR, id);
    BmpRows *r = (BmpRows *)malloc(sizeof *r); if (!r) return false;
    FILE *f = fopen(path, "wb"); if (!f) { free(r); return false; }

    const bool portrait = cover_portrait(id);
    const int W = cover_w(id), H = COVER_H;
    float sx = (float)W / pw, sy = (float)H / ph, s = sx < sy ? sx : sy;
    int dw = (int)(pw * s + 0.5f), dh = (int)(ph * s + 0.5f);
    if (dw > W) dw = W;
    if (dh > H) dh = H;
    int offX = (W - dw) / 2, offY = (H - dh) / 2;

    int rowSize = (W * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * H;
    uint8_t hd[54]; memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M'; put32(hd + 2, 54 + imgSize); put32(hd + 10, 54);
    put32(hd + 14, 40); put32(hd + 18, W); put32(hd + 22, H);
    put16(hd + 26, 1); put16(hd + 28, 24); put32(hd + 38, 2835); put32(hd + 42, 2835);
    fwrite(hd, 1, 54, f);

    lgfx::rgb888_t *prow = r->in.rgb;
    uint8_t *orow = r->out;

    // PORTRAIT box (pinball): the live playfield is a 90°-rotated portrait table on the landscape
    // panel (app_pinball's SX/SY: screen_x = (pw-1)-ly, screen_y = lx). Un-rotate it so the cover is
    // UPRIGHT and fills the tall box. Each output row maps to a fixed source COLUMN, so read the frame
    // column-by-column. Verified end-to-end (round-trips the logical layout to an upright cover).
    if (portrait) {
        for (int oy = H - 1; oy >= 0; oy--) {           // BMP bottom-up; oy = row-from-top (0 = table top)
            int cx = (pw - 1) - oy * pw / H; if (cx < 0) cx = 0; if (cx >= pw) cx = pw - 1;
            c->readRect(cx, 0, 1, ph, prow);            // one source column, top→bottom
            memset(orow, 0, rowSize);
            for (int ox = 0; ox < W; ox++) {
                int cy = ox * ph / W; if (cy < 0) cy = 0; if (cy >= ph) cy = ph - 1;
                orow[ox * 3 + 0] = prow[cy].b;          // BGR
                orow[ox * 3 + 1] = prow[cy].g;
                orow[ox * 3 + 2] = prow[cy].r;
            }
            fwrite(orow, 1, rowSize, f);
        }
        fclose(f);
        free(r);
        return true;
    }

    for (int vy = H - 1; vy >= 0; vy--) {
        memset(orow, 0, rowSize);
        if (vy >= offY && vy < offY + dh) {
            int srcY = (int)((vy - offY) / s); if (srcY < 0) srcY = 0; if (srcY >= ph) srcY = ph - 1;
            c->readRect(0, srcY, pw, 1, prow);          // RAM read, M5GFX expands 8bpp -> rgb888
            for (int ix = 0; ix < dw; ix++) {
                int srcX = (int)(ix / s); if (srcX < 0) srcX = 0; if (srcX >= pw) srcX = pw - 1;
                int dx = offX + ix;
                orow[dx * 3 + 0] = prow[srcX].b;        // BMP is BGR
                orow[dx * 3 + 1] = prow[srcX].g;
                orow[dx * 3 + 2] = prow[srcX].r;
            }
        }
        fwrite(orow, 1, rowSize, f);
    }
    fclose(f);
    free(r);
    return true;
}

