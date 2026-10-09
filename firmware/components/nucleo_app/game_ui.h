// game_ui — the console kit every native game shares, so the Cardputer reads as ONE small game console:
// the same title screen, the same menu, the same panels and type, drawn the same way in every game.
//
// Built for the 8bpp RGB332 shared canvas (see app_gfx.h): colours are snapped to RGB332 so what you ask
// for is what the panel shows, gradients are Bayer-dithered instead of banding, and every face used here is
// already linked in the firmware (FreeSansBold 9/12 pt, FreeSans 9 pt, Font2, Font4) — zero extra flash.
// Everything draws through the movable `d` target, so it composites into the buffered frame like the rest
// of an app's on_draw(). No heap, no statics beyond a few bytes: a menu's state lives in the game.
//
//   static gui::Menu s_menu;                                 // the game's own state (selection + glide)
//   on_draw:  int y = gui::title(GT("Pong", "Pong"), GT("Gioca contro la CPU", "Play the CPU"), ACCENT);
//             gui::menu(s_menu, items, n, y, nucleo_app_content_height(), ACCENT);
//   on_key:   if (gui::menu_key(s_menu, k, n)) { sfx(NAV); nucleo_app_request_draw(); }
//   poll:     return gui::menu_tick(s_menu, dt_ms);           // true only while the cursor still glides
#pragma once
#include <stdint.h>

namespace gui {

// RGB332-exact colour (the canvas keeps 3/3/2 bits: anything else is rounded by the panel anyway).
uint16_t rgb(int r, int g, int b);
// Blend two RGB565 colours, t = 0 (a) .. 256 (b).
uint16_t mix(uint16_t a, uint16_t b, int t);

// Vertical gradient top -> bottom, ordered-dithered (4x4 Bayer) so 8bpp shows a smooth ramp, not bands.
void vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
// A card: rounded fill, 2 px drop shadow, a 1 px highlight on the top edge and a crisp outline.
void panel(int x, int y, int w, int h, uint16_t fill, uint16_t edge);
// Text with a 1 px drop shadow. datum: 0 = left, 1 = centre, 2 = right (x is that edge / the centre).
// font: 0 = small UI (Font2, 16 px), 1 = body bold (FreeSansBold 9 pt), 2 = title (FreeSansBold 12 pt),
//       3 = big digits/score (Font4, 26 px). Returns the text width in pixels.
enum { F_SMALL = 0, F_BODY = 1, F_TITLE = 2, F_BIG = 3 };
int text(const char *s, int x, int y, int datum, int font, uint16_t col, uint16_t shadow);
int text_width(const char *s, int font);
// Line height of a face, for layout.
int font_h(int font);

// The shared title screen header: dithered backdrop over the content area, the game's name in the title face
// with an accent underline, an optional subtitle. Returns the first free y below it.
int title(const char *name, const char *sub, uint16_t accent);

// The shared menu: a vertical list, the selection on an accent pill that glides (ease-out) to the chosen row,
// rows scrolled into view when they do not all fit between y0 and y1. Labels in the body face; a row that is
// too wide is drawn in the small face rather than clipped.
struct Menu { int8_t sel; float pos; };
void menu(const Menu &m, const char *const *items, int n, int y0, int y1, uint16_t accent);
bool menu_key(Menu &m, int key, int n);        // UP/DOWN (wrapping): true when the selection moved
bool menu_tick(Menu &m, int dt_ms);            // advance the glide: true while it still moves (request a draw)

// A centred modal card (pause / leave / game over): title line + up to 2 text lines + a key hint line.
void dialog(const char *head, const char *l1, const char *l2, const char *keys, uint16_t accent);

}  // namespace gui
