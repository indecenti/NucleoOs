// Small vector glyph set for the SYSTEM surfaces — the Control Center (TAB) and the native Settings
// app. Separate from the launcher's app-icon set (ui_icon in launcher_render.cpp, mirrored 1:1 by
// web/device/icons.js and policed by icons:gate): these are control glyphs (Wi-Fi fan, sun, speaker,
// power...) with no web twin, keyed by an enum instead of an app id so they never touch that gate.
//
// Every glyph is drawn from filled primitives inside a box of half-size `r` centred at (cx,cy), in one
// ink colour `col` plus the backdrop colour `bg` for cut-outs — the same recipe as the launcher icons,
// so both sets read as one family. Takes the LovyanGFX base, so it draws to the real display (M5GFX),
// the shared back-buffer (M5Canvas) or an app's `&d` alike. Zero RAM: pure code.
#pragma once
#include <M5GFX.h>
#include <stdint.h>

enum {
    UG_WIFI = 0, UG_HOTSPOT, UG_SUN, UG_SPEAKER, UG_MUTE, UG_GLOBE, UG_CLOCK, UG_CHIP,
    UG_RESET, UG_POWER, UG_MOON, UG_TORCH, UG_GEAR, UG_KEYBOARD, UG_USB, UG_MONITOR,
    UG_PALETTE, UG_MIC, UG_STAR, UG_LOCK, UG_UPDATE, UG_KEY, UG_NEXT, UG_PREV,
    UG_CHECK, UG_SEARCH, UG_BELL, UG_BATTERY, UG_BLUETOOTH, UG_INFO,
};

void ui_glyph(LovyanGFX *g, int glyph, int cx, int cy, int r, uint16_t col, uint16_t bg);
