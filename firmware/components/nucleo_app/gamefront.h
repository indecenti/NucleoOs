// GameFront — the Games gallery, HyperSpin style. The launcher's Games tile opens it instead of a plain category.
//
// Two levels: the SYSTEMS (native games — built in or from the Store — then one entry per console whose emulator is
// installed and whose cartridges are on the card: Game Boy, Game Boy Color, Game Gear, Master System), then the
// TITLES of the chosen one. A title wheel on the right (the selection in the middle, its neighbours fading), the
// picture and the whole title on the left, the live search on top; the same look as the emulators' own shelf.
// A cartridge opens straight in its emulator ("open with", carried across the game's Solo reboot), and quitting the
// game comes back here on it. With the external panel the gallery lives there and the Cardputer is its companion.
//
// It is a launcher STATE (like the Control Center), not a registered app: the run loop owns it (nucleo_app.cpp).
// Pictures: native games' covers /sd/data/GameShots/<id>.{png,jpg,bmp}; a cartridge's screenshot
// <rom folder>/.snaps/<rom file>.snp (tools/fetch-rom-snaps.py); the consoles' photos (gbemu_art.h). Each is turned
// once into a raw .pic beside it (or in GameShots/.cache) and streamed from the card while a frame is composed.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// gamefront_key return codes (mirrors the Control Center contract).
enum { GF_NONE = 0, GF_REDRAW = 1, GF_CLOSE = 2, GF_LAUNCH = 3 };

void gamefront_open(void);                                       // list the systems and titles, keep the selection
int  gamefront_key(int key, char ch, char *launch_id, int cap);  // GF_LAUNCH fills launch_id (the app to open)
const char *gamefront_launch_file(void);                         // ...and the cartridge it must open, or NULL
void gamefront_render(void);                                     // the whole screen (+ the companion on two screens)
void gamefront_invalidate(void);                                 // the surface changed: repaint everything
// In the full OS boot after a game launched from the gallery ran in its Solo boot: true = reopen the gallery (on
// that game). One-shot, and only after our own restart.
bool gamefront_take_return(void);
// The Cardputer's screen while the gallery is on the panel (gamefront_render calls it; exposed for the PC gallery).
void gamefront_draw_companion(void *gfx);

#ifdef __cplusplus
}
#endif
