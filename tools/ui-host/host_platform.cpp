// ui-host: the handful of LovyanGFX platform hooks the core links against (its "sdl" platform
// declarations, see shim/SDL2/SDL.h). No real timing, no GPIO: everything renders into memory.
#include "lgfx/v1/platforms/common.hpp"
namespace lgfx { inline namespace v1 {
unsigned long millis(void) { return 0; }
unsigned long micros(void) { return 0; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
void gpio_hi(uint32_t) {}
void gpio_lo(uint32_t) {}
bool gpio_in(uint32_t) { return false; }
void pinMode(int_fast16_t, pin_mode_t) {}
void lgfxPinMode(int_fast16_t, pin_mode_t) {}
}}
