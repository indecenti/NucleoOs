// ui-host: no real SDL. Defining the include guard makes LovyanGFX pick its "sdl" platform
// DECLARATIONS (millis/delay/gpio/FileWrapper); host_platform.cpp implements the few it links.
// Nothing ever opens a window — every draw lands in an in-memory LGFX_Sprite.
#pragma once
#define SDL_h_
