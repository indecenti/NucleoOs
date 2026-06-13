// Foreground-app draw indirection (double buffering).
//
// Every native app draws with `d.…` against the global display. That clears-then-draws
// directly on screen, which flickers. Instead we route all app drawing through a movable
// target: normally the real display, but during a buffered repaint it points at an
// off-screen content canvas. The framework composites the whole frame off-screen and
// blits it in ONE pushSprite — so the screen never shows an intermediate cleared state.
//
// Implementation: keep the familiar `d.foo()` call sites, but make `d` resolve to whatever
// the target currently is. Defined in nucleo_ui.cpp; driven by nucleo_app.cpp's run loop.
#pragma once
#include <M5GFX.h>
#include <stddef.h>

LovyanGFX *nucleo_app_gfx(void);          // current draw target (display or content buffer)
bool       nucleo_app_is_buffered(void);  // true while drawing into the off-screen buffer
void       nucleo_app_set_gfx(LovyanGFX *g);  // NULL restores the real display

// Shared off-screen back-buffer (defined in nucleo_ui.cpp). ONE canvas (240x135 @ 8bpp = 32400 B)
// reused by the launcher list band and every foreground app. Media/decoder paths release it to
// hand the contiguous block to the codec, then it re-acquires lazily (timed retry in the getter).
M5Canvas *nucleo_screen(void);            // shared back-buffer, or NULL while OOM (draw direct)
bool      nucleo_screen_acquire(void);    // explicit (re)allocate — always attempts
void      nucleo_screen_release(void);    // FREES the 32 KB canvas (deleteSprite); re-acquired lazily

// Zero-churn redirect: existing `d.<method>()` / `&d` call sites now follow the target.
// (Only affects the bare token `d`; text inside string literals like "%d" is untouched.)
#define d (*nucleo_app_gfx())
