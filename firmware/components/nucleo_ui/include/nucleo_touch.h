// Touch targets — how a compact screen (240x135) tells the external panel's touch what is where.
//
// While it draws, a screen marks the rectangles that mean something ("this card = key '3'", "this label =
// UP"). A tap inside one delivers that key; a tap anywhere else stays the generic gesture (Enter), drags
// stay arrows. Coordinates are the screen's own: 240x135 for a compact screen (mirrored 1:1 or fitted 4/3),
// the panel's 320x240 for a large one (on_draw_large), where a tap outside every target falls back to the
// large surface's zones (top = back, sides = left/right, centre = Enter).
// The run loop brackets every foreground frame with clear/commit, so an app only calls nucleo_touch_add()
// from its on_draw. Without a touch panel every call is a no-op and nothing is stored.
//
// Scroll areas: a region whose content follows the finger. A drag that STARTS inside one delivers one key per
// `step` pixels the finger travels (natural scrolling: finger up = `fwd`, toward later content; finger down =
// `back`), and a flick keeps going after the lift, slowing down like a phone list (kinetic scrolling). Inside
// a scroll area a drag never becomes the generic arrows (which carry ; . , / and would TYPE in a text app) and
// a long press is not Back. Taps inside it still hit the targets above. Coordinates as for targets.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_TOUCH_MAX 16

// Lists: a visible row registers itself with ch = NUCLEO_TOUCH_ROW + its slot on screen (0..15). The list's
// key handler turns it into "select this row", or "open it" when it already is the selection — tap to pick,
// tap again to open, the smartwatch way. Never a printable character, so it cannot collide with typing.
#define NUCLEO_TOUCH_ROW 0x10
static inline bool nucleo_touch_is_row(int key, char ch) { return key == 9 /* NK_CHAR */ && ch >= NUCLEO_TOUCH_ROW && ch < NUCLEO_TOUCH_ROW + 16; }

void nucleo_touch_clear(void);                                   // start this frame's set
void nucleo_touch_add(int x, int y, int w, int h, int key, char ch);   // key = NK_*, ch for NK_CHAR
void nucleo_touch_commit(void);                                  // publish it (the touch task reads this one)
#define NUCLEO_TOUCH_SCROLL_MAX 2
void nucleo_touch_add_scroll(int x, int y, int w, int h, int step,
                             int key_back, char ch_back, int key_fwd, char ch_fwd);

#ifdef __cplusplus
}
#endif
