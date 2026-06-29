// GameFront — a HyperSpin-style front-end that REPLACES the plain "Games" category carousel.
// Pressing the Games tile opens this full-screen launcher: a large game cover (real on-device
// screenshots, with a procedural poster fallback), the title, a scrollable description and a
// neighbour wheel. It is a launcher STATE (like the Control Center), not a registered app, so it
// stays lightweight and the run loop owns it. See nucleo_app.cpp for the wiring.
//
// Screenshots are captured on-device from the shared back-buffer (every native game draws through
// it), so a freshly-played game self-seeds its cover. Covers live on the SD at
// /sd/data/GameShots/<id>.{bmp,png,jpg}; a per-game blurb may be overridden at <id>.txt.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// gamefront_key return codes (mirrors the Control Center contract).
enum { GF_NONE = 0, GF_REDRAW = 1, GF_CLOSE = 2, GF_LAUNCH = 3 };

void gamefront_open(void);                                  // snapshot the Games list, keep selection
int  gamefront_key(int key, char ch, char *launch_id, int cap);  // GF_LAUNCH fills launch_id
void gamefront_render(void);                               // compose full screen + blit
bool gamefront_step(void);                                 // animation tick; true while moving

// Capture the freshly-composited shared canvas to the SD. cover -> downscaled /data/GameShots/<id>.bmp;
// screenshot -> full-res /data/Screenshots/<name>.bmp. Call right after the frame is pushed.
bool gamefront_save_cover(const char *id);        // true if the BMP was written
bool gamefront_save_screenshot(const char *name); // true if the BMP was written (reads the canvas)
bool gamefront_save_panel_screenshot(const char *name); // reads the PHYSICAL PANEL — works for direct-draw / Solo-mode apps
bool gamefront_save_panel_cover(const char *id);        // full-frame panel shot AS the carousel cover (/data/GameShots/<id>.bmp, overwrites)
bool gamefront_save_canvas_cover(const char *id);       // cheap RAM-read cover from the live canvas, scaled to fit the hero box (overwrites)

// Called when a game closes: refresh its .bmp cover from the last composited frame (overwrite), so
// exiting on the frame you like sets the cover. Skips only if a curated .png/.jpg is present.
void gamefront_seed_cover(const char *id);

#ifdef __cplusplus
}
#endif
