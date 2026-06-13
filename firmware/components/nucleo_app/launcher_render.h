// Launcher rendering: status/hint chrome, the animated Wear OS list band, the Control
// Center overlay and the remote-session screen. Draws straight to the real display.
//
// Tearing fix: the list band is the ONLY animated region and is the ONLY thing buffered.
// It is composited into a persistent off-screen canvas and pushed in a single blit, so the
// screen never shows a cleared-then-redrawn intermediate. The canvas is allocated at full
// colour (16bpp) when heap allows and falls back to a half-size 8bpp (RGB332) canvas under
// memory pressure (e.g. Wi-Fi STA up) — either way the scroll stays flicker-free. Direct
// drawing is only the last-resort path when not even the small canvas fits. The static
// chrome is drawn directly (it does not animate), which avoids per-frame sprite churn that
// previously fragmented the heap and made the band canvas fail to allocate.
#pragma once

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

// The one buffered, animated region.
void launcher_render_list(void);          // composite + blit the scrolling list band

// Smooth-scroll animation toward the focused row. Returns true while still moving (the
// caller should keep requesting redraws).
bool launcher_render_step_scroll(void);

// Full-screen overlays.
void launcher_render_control_center(void);             // draw the interactive quick-settings sheet
void launcher_render_control_center_open(void);        // reset selection + free the band when raised
void launcher_render_control_center_close(void);       // release the sheet's off-screen canvas
// Handle a key. Returns 0 = ignored, 1 = consumed (redraw), 2 = sheet should close (hierarchical
// Back popped past the top level — same Esc/Left semantics as the Music/Video settings sheets).
int  launcher_render_control_center_key(int key, char ch);
