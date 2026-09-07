// Host stub of the draw target. The fake GFX does not paint: it RECORDS every text box and rectangle
// so the harness can assert on layout — text inside the content area, and no label/value overlap.
#pragma once
#include <stddef.h>

#define MAXG 128
typedef struct { int x, y, w, h; } g_box_t;

struct HostGfx {
    int size = 1, cx = 0, cy = 0;
    unsigned short col = 0, bg = 0;
    void setTextSize(int s) { size = s; }
    void setTextColor(unsigned short c, unsigned short b) { col = c; bg = b; }
    void setCursor(int x, int y) { cx = x; cy = y; }
    void print(const char *t);
    void fillRect(int x, int y, int w, int h, unsigned short c);
    void drawRect(int x, int y, int w, int h, unsigned short c);
    void fillRoundRect(int x, int y, int w, int h, int r, unsigned short c);
    void fillCircle(int x, int y, int r, unsigned short c);
    void drawCircle(int x, int y, int r, unsigned short c);
    void drawFastHLine(int x, int y, int w, unsigned short c);
    void fillScreen(unsigned short c);
};

extern HostGfx g_gfx;
// What the last frame drew (reset by the harness before each on_draw).
extern g_box_t g_text[MAXG];
extern char    g_txt[MAXG][48];
extern int     g_texts;

#define d g_gfx
