// Launcher rendering: status/hint chrome, the animated Wear OS list band, the Control
// Center overlay and the remote-session screen. Draws straight to the real display.
//
// Tearing fix: the list band is the ONLY animated region and is the ONLY thing buffered.
// It is composited into a persistent off-screen canvas and pushed in a single blit, so the
// screen never shows a cleared-then-redrawn intermediate. The shared canvas is 8bpp (RGB332):
// 240x135 at 16bpp would be ~65 KB and won't fit the no-PSRAM heap, so 8bpp (~32 KB) is the
// standing choice, not a fallback — the fine mix565() gradients in the carousel are dithered
// down to 256 colours on push. Direct drawing is the last-resort path when not even the 8bpp
// canvas fits (heap fragmented after a media app). The static chrome is drawn directly (it does
// not animate), which avoids per-frame sprite churn that previously fragmented the heap.
#pragma once

// Apps are laid out as a single horizontal icon carousel (smartwatch idiom) on the Cardputer: one big
// centred badge with smaller dimmer neighbours. On an external 320x240 panel the same apps become a
// touch GRID instead (a real home screen). Navigation is one-dimensional on the carousel (every arrow
// steps by one); on the grid the run loop moves up/down by a whole row, so it asks for the column count.
// 0 = the compact carousel is live (no grid); >0 = the grid's column count.
int launcher_render_large_cols(void);
// First item index whose tile is on screen on the large grid, after keeping the focused tile in view: the run
// loop maps a tapped tile (NUCLEO_TOUCH_ROW + slot) to the item at first + slot.
int launcher_render_grid_first(void);
// A drag on the large grid (NUCLEO_TOUCH_VIEW_*): the content moves one step (dir +1 = toward the end), the
// focus stays. True when it moved (repaint).
bool launcher_render_grid_view(int dir);
// The panel's quick menu for one tile (a long press, or '*'): Open, Pin to Home / Unpin, Info, Uninstall (store apps).
int  launcher_render_grid_item_at(int px, int py);   // the item under a panel point, -1 = none
void launcher_qm_open(int item);                     // open it for that visible-list item
bool launcher_qm_active(void);
int  launcher_qm_key(int key, char ch);              // 0 = not open, 1 = handled (repaint), 2 = open the focused app

// Bottom hint-bar text (also the public nucleo_app_set_hint).
void        launcher_render_set_hint(const char *h);
const char *launcher_render_hint(void);
void        launcher_render_update_chrome(void);   // recompute the hint from the focused row

// Hint-bar colors. Default is the dark chrome (INK bg / MUTED text); an app may override them
// (e.g. Torch wants a white footer so the whole panel is lit). Auto-reset on app open/close.
void        launcher_render_set_hint_colors(unsigned short bg, unsigned short fg);
void        launcher_render_reset_hint_colors(void);

// Direct draws (no animation -> no buffering needed).
void launcher_render_status_bar(void);   // top bar: clock/breadcrumb + filter + battery
void launcher_render_instr_bar(void);     // focused-item description line
void launcher_render_hint_bar(void);      // bottom hint bar
void launcher_render_chrome(void);        // status + instruction + hint together
void launcher_render_clock_tick(void);    // 1 Hz: overwrite only the clock digits (no bar wipe)
// True ONLY when the chrome's visible content actually changed (depth/category, filter, focused kind,
// Wi-Fi level) — so the caller marks the chrome dirty on navigation WITHOUT re-wiping the static bars on
// every key (anti-flicker). Latches the new signature on each true. See ANTI-FLICKER.md.
bool launcher_render_chrome_changed(void);

// Draw a launcher app/category icon (the hand-kept vector glyph set) onto a sprite. Exposed so the
// Game front-end can reuse the real vector icons for its procedural poster fallback. M5Canvas is a
// `using` alias of m5gfx::M5Canvas, so forward-declare it in its real namespace (a bare global
// `class M5Canvas;` would shadow the alias and break every TU that also includes <M5GFX.h>).
namespace m5gfx { class M5Canvas; class M5GFX; }
using m5gfx::M5Canvas;
using m5gfx::M5GFX;
// Two overloads so the front-end can draw to the off-screen canvas (M5Canvas) OR straight to the
// display (M5GFX) when the heap can't spare the canvas (low-heap direct path).
void launcher_draw_icon(M5Canvas *c, int cx, int cy, int r, const char *id, char letter,
                        unsigned short col, unsigned short bg);
void launcher_draw_icon(M5GFX *c, int cx, int cy, int r, const char *id, char letter,
                        unsigned short col, unsigned short bg);
// ...or to whatever `d` is at the moment (a strip of lg::frame).
namespace lgfx { inline namespace v1 { class LovyanGFX; } }
void launcher_draw_icon(lgfx::LovyanGFX *c, int cx, int cy, int r, const char *id, char letter,
                        unsigned short col, unsigned short bg);

// The one buffered, animated region.
void launcher_render_list(void);          // composite + blit the scrolling list band

// Smooth-scroll animation toward the focused row. Returns true while still moving (the
// caller should keep requesting redraws).
bool launcher_render_step_scroll(void);

// Full-screen overlays.
// Control Center (TAB): one-screen quick panel — status strip, 4 toggle tiles, brightness/volume
// sliders, 5 shortcuts and a context line. Focus is kept across opens (resume).
void launcher_render_control_center(void);             // compose + blit the panel
void launcher_render_control_center_open(void);        // disarm any pending action (focus is kept)
void launcher_render_control_center_reset(void);       // forget the remembered focus + scroll (first-boot state)
void launcher_render_control_center_close(void);       // persist a brightness/volume change once
// Handle a key. Returns CC_NONE(0)/CC_REDRAW(1)/CC_CLOSE(2)/CC_SCREEN_OFF(3)/CC_LAUNCH(4)/CC_TORCH(5).
// When CC_LAUNCH: call launcher_render_control_center_launch_id() for the app id to open.
int         launcher_render_control_center_key(int key, char ch);
const char *launcher_render_control_center_launch_id(void);
bool        launcher_render_control_center_tick(void);  // 1 Hz: true when what the panel shows changed
bool        launcher_render_control_center_animating(void);   // every loop: true while its scroll eases
// Without the back-buffer the panel repaints only the elements that changed (flicker-free); call this
// when something else drew over it, so the next paint is a full one.
void        launcher_render_control_center_invalidate(void);

// Companion: the Cardputer's own panel while the launcher home is on an external 320x240 panel (duplicate
// mode). Draw it with launcher_render_companion(); _changed() is true (once) when what it shows has moved.
namespace lgfx { inline namespace v1 { class LovyanGFX; } }
void launcher_render_companion(lgfx::LovyanGFX *g);
bool launcher_render_companion_changed(void);
void launcher_render_companion_invalidate(void);
