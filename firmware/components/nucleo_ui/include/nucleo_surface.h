// The primary drawing surface: what `d` is right now.
//   compact - the internal 240x135 panel (mirrored 1:1 onto an external panel when one is attached)
//   large   - an external panel used natively (320x240): `d` reports its size and draws on it alone
// A screen that can lay itself out at 320x240 asks for large; any other screen asks for compact before it
// draws. Without an external panel nothing changes: large is never granted and every call is a no-op, so
// callers need no "is a display attached" test. No buffer of its own: see docs/external-display.md.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool nucleo_surface_large_ok(void);       // an external panel can host a large screen right now
bool nucleo_surface_large(void);          // `d` is the external panel, natively
// Ask for large (true) or compact (false), between frames only. Returns true when the surface CHANGED:
// the caller must repaint everything it shows. A large request falls back to compact when not available.
bool nucleo_surface_set_large(bool on);

#ifdef __cplusplus
}
#endif
