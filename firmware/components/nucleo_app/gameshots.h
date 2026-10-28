// Screenshots and game covers captured on the device, as 24-bit BMPs on the card (no big allocation).
//   /data/Screenshots/<name>.bmp   Fn+P: the whole screen
//   /data/GameShots/<id>.bmp       a game's cover in the Games gallery (gamefront.cpp), from live play
// The canvas readers read the shared off-screen canvas (every native game draws through it); the panel readers read
// the Cardputer's own panel back (direct-draw and Solo-boot apps).
#pragma once
#include <stdbool.h>
#include "nucleo_board.h"
#ifdef __cplusplus
extern "C" {
#endif

#define GF_DIR   NUCLEO_SD_MOUNT "/data/GameShots"
#define SHOT_DIR NUCLEO_SD_MOUNT "/data/Screenshots"

// Unique name for the next /data/Screenshots capture: shot_YYYYMMDD_HHMMSS once the clock is set, else shot_N one
// past the highest N already on the card (never overwrites an earlier session's shots).
void gameshots_name(char *out, int cap);
bool gameshots_save_canvas(const char *name);   // the shared canvas, full size -> /data/Screenshots/<name>.bmp
bool gameshots_save_panel(const char *name);    // the Cardputer's panel, full size -> /data/Screenshots/<name>.bmp
bool gameshots_panel_cover(const char *id);     // the panel fitted into <id>'s cover box -> /data/GameShots/<id>.bmp
bool gameshots_canvas_cover(const char *id);    // the canvas fitted into <id>'s cover box (cheap RAM read)

#ifdef __cplusplus
}
#endif
