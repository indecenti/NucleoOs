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

#define NUCLEO_TOUCH_MAX 20

// Lists: a visible row registers itself with ch = NUCLEO_TOUCH_ROW + its slot on screen (0..15). The list's
// key handler turns it into "select this row", or "open it" when it already is the selection — tap to pick,
// tap again to open, the smartwatch way. Never a printable character, so it cannot collide with typing.
#define NUCLEO_TOUCH_ROW 0x10
static inline bool nucleo_touch_is_row(int key, char ch) { return key == 9 /* NK_CHAR */ && ch >= NUCLEO_TOUCH_ROW && ch < NUCLEO_TOUCH_ROW + 16; }

// Fields: an area whose meaning depends on WHERE it is tapped (a star map, a game board, a playfield). It
// registers as one target with ch = NUCLEO_TOUCH_FIELD; on that key the app reads the tap's position with
// nucleo_touch_last() and hit-tests its own content — any number of things on the field, one target.
#define NUCLEO_TOUCH_FIELD 0x0E
static inline bool nucleo_touch_is_field(int key, char ch) { return key == 9 /* NK_CHAR */ && ch == NUCLEO_TOUCH_FIELD; }
// View scroll: a scroll area (nucleo_touch_add_scroll) that moves the CONTENT, not the selection, delivers these as
// NK_CHAR — one per `step` px of finger travel (BACK = finger down, the content toward its top). The list shifts by
// step px and the focus stays where it was, as on a phone. Never printable.
#define NUCLEO_TOUCH_VIEW_BACK 0x0B
#define NUCLEO_TOUCH_VIEW_FWD  0x0C
// Hold: on a large screen (on_draw_large / the launcher grid) a finger held still on a list row or a tile
// (a NUCLEO_TOUCH_ROW target) for the long-press time delivers NK_CHAR NUCLEO_TOUCH_HOLD instead of nothing; the
// screen reads where with nucleo_touch_last() and opens its menu for what is there. Lifting after it is not a tap.
#define NUCLEO_TOUCH_HOLD 0x0F
static inline bool nucleo_touch_is_hold(int key, char ch) { return key == 9 /* NK_CHAR */ && ch == NUCLEO_TOUCH_HOLD; }
// Where the tap that delivered the last target key landed (the screen's own coordinates). False = none yet.
bool nucleo_touch_last(int *x, int *y);

void nucleo_touch_clear(void);                                   // start this frame's set
void nucleo_touch_add(int x, int y, int w, int h, int key, char ch);   // key = NK_*, ch for NK_CHAR
void nucleo_touch_commit(void);                                  // publish it (the touch task reads this one)
#define NUCLEO_TOUCH_SCROLL_MAX 2
void nucleo_touch_add_scroll(int x, int y, int w, int h, int step,
                             int key_back, char ch_back, int key_fwd, char ch_fwd);

// Drag pads: like a scroll area (a drag that starts inside is the app's, never a long-press Back), but the drag
// moves in the FINGER's direction on both axes: every `step` px of travel delivers NK_RIGHT / NK_LEFT / NK_DOWN /
// NK_UP (finger right = Right, finger up = Up), no fling. For aiming and steering on a playfield. Taps inside still
// hit the targets. Shares the NUCLEO_TOUCH_SCROLL_MAX table with the scroll areas.
void nucleo_touch_add_pad(int x, int y, int w, int h, int step);

#ifdef __cplusplus
}
#endif
