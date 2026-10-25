// See extdisp_chrome.hpp. Platform-neutral LovyanGFX code: the host gate compiles this file unchanged.
#include "extdisp_chrome.hpp"

using namespace lgfx;

namespace {

const uint16_t BG      = 0x0000;
const uint16_t KEY_BG  = color565(34, 42, 58);
const uint16_t KEY_EDGE= color565(70, 84, 110);
const uint16_t KEY_ACT = color565(52, 132, 255);
const uint16_t INK     = 0xFFFF;
const uint16_t DIM     = color565(150, 160, 178);
const uint16_t TARGET  = color565(255, 64, 64);

void key_face(LovyanGFX *g, const extdisp_rect_t &k, bool pressed)
{
    g->fillRect(k.x, k.y, k.w, k.h, BG);
    const int x = k.x + 3, y = k.y + 4, w = k.w - 6, h = k.h - 8;
    g->fillRoundRect(x, y, w, h, 8, pressed ? KEY_ACT : KEY_BG);
    if (!pressed) g->drawRoundRect(x, y, w, h, 8, KEY_EDGE);
}

void arrow(LovyanGFX *g, int cx, int cy, int s, int dir)   // dir: 0 left, 1 up, 2 down, 3 right
{
    switch (dir) {
        case 0: g->fillTriangle(cx - s, cy, cx + s / 2, cy - s, cx + s / 2, cy + s, INK); break;
        case 1: g->fillTriangle(cx, cy - s, cx - s, cy + s / 2, cx + s, cy + s / 2, INK); break;
        case 2: g->fillTriangle(cx, cy + s, cx - s, cy - s / 2, cx + s, cy - s / 2, INK); break;
        default: g->fillTriangle(cx + s, cy, cx - s / 2, cy - s, cx - s / 2, cy + s, INK); break;
    }
}

void status(LovyanGFX *g, const extdisp_rect_t &k, bool pressed, const char *l1, const char *l2, bool swatches)
{
    g->fillRect(k.x, k.y, k.w, k.h, BG);
    const int cx = k.x + k.w / 2;
    g->setTextDatum(textdatum_t::top_center);
    g->setFont(&lgfx::fonts::Font2);
    g->setTextColor(pressed ? KEY_ACT : INK, BG);
    if (l1) g->drawString(l1, cx, k.y + 4);
    g->setTextColor(pressed ? KEY_ACT : DIM, BG);
    if (l2) g->drawString(l2, cx, k.y + 20);
    if (swatches && k.h >= 46) {
        static const uint16_t SW[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        for (int i = 0; i < 4; i++) g->fillRect(cx - 26 + i * 14, k.y + k.h - 12, 10, 8, SW[i]);
    }
}

} // namespace

void extdisp_chrome_key(LovyanGFX *g, const extdisp_rect_t r[SK_COUNT], int key, bool pressed,
                        const char *l1, const char *l2, bool swatches)
{
    if (key <= SK_NONE || key >= SK_FRAME) return;
    const extdisp_rect_t &k = r[key];
    if (k.w <= 0) return;
    if (key == SK_STATUS) { status(g, k, pressed, l1, l2, swatches); return; }
    key_face(g, k, pressed);
    const int cx = k.x + k.w / 2, cy = k.y + k.h / 2, s = (k.h < k.w ? k.h : k.w) / 5;
    g->setTextDatum(textdatum_t::middle_center);
    g->setTextColor(INK);
    switch (key) {
        case SK_LEFT:  arrow(g, cx, cy, s, 0); break;
        case SK_UP:    arrow(g, cx, cy, s, 1); break;
        case SK_DOWN:  arrow(g, cx, cy, s, 2); break;
        case SK_RIGHT: arrow(g, cx, cy, s, 3); break;
        case SK_OK:    g->setFont(&lgfx::fonts::FreeSansBold12pt7b); g->drawString("OK", cx, cy + 1); break;
        case SK_ESC:   g->setFont(&lgfx::fonts::FreeSansBold9pt7b);  g->drawString("ESC", cx, cy + 1); break;
        case SK_MENU:                                               // three bars: the Control Center
            for (int i = -1; i <= 1; i++) g->fillRoundRect(cx - s - 2, cy + i * (s / 2 + 2) - 2, 2 * s + 4, 4, 2, INK);
            break;
    }
}

void extdisp_chrome_draw(LovyanGFX *g, const extdisp_rect_t r[SK_COUNT], const extdisp_frame_t &fr,
                         const char *l1, const char *l2, bool swatches)
{
    const int W = g->width(), H = g->height(), fb = fr.oy + fr.fh;
    g->startWrite();
    g->fillRect(0, 0, W, fr.oy, BG);
    g->fillRect(0, fb, W, H - fb, BG);
    g->fillRect(0, fr.oy, fr.ox, fr.fh, BG);
    g->fillRect(fr.ox + fr.fw, fr.oy, W - fr.ox - fr.fw, fr.fh, BG);
    for (int k = SK_ESC; k <= SK_STATUS; k++) extdisp_chrome_key(g, r, k, false, l1, l2, swatches);
    g->endWrite();
}

void extdisp_chrome_calib(LovyanGFX *g, const extdisp_frame_t &fr, int step, const char *l1, const char *l2)
{
    const int W = g->width(), H = g->height(), fb = fr.oy + fr.fh;
    int t[4][2];
    extdisp_cal_targets(W, H, t);
    g->startWrite();
    g->fillRect(0, 0, W, fr.oy, BG);
    g->fillRect(0, fb, W, H - fb, BG);
    if (step >= 0 && step < 4) {
        const int x = t[step][0], y = t[step][1];
        g->drawCircle(x, y, 10, TARGET);
        g->drawCircle(x, y, 9, TARGET);
        g->drawFastHLine(x - 16, y, 33, INK);
        g->drawFastVLine(x, y - 16, 33, INK);
        g->fillCircle(x, y, 2, TARGET);
    }
    g->setTextDatum(textdatum_t::top_center);
    g->setFont(&lgfx::fonts::Font2);
    g->setTextColor(INK, BG);
    if (l1) g->drawString(l1, W / 2, 6);
    g->setTextColor(DIM, BG);
    if (l2) g->drawString(l2, W / 2, 24);
    g->endWrite();
}
