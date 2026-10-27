// tanks-host: the two-screen kit (app_large.h, nucleo_touch.h) as no-ops. The gate drives the Cardputer view
// alone (nucleo_app_two_screens() is false), so the panel painters link but never run; the gallery
// (tools/ui-host/apps.mjs) is what renders and checks the 320x240 view. Include once per executable.
#pragma once
#include "app_large.h"
#include "nucleo_touch.h"
#include "nucleo_theme.h"
uint16_t THEME_BG, THEME_MUTED, THEME_LINE;
bool nucleo_app_two_screens(void) { return false; }
void nucleo_touch_clear(void) {}
void nucleo_touch_commit(void) {}
void nucleo_touch_add(int, int, int, int, int, char) {}
void nucleo_touch_add_scroll(int, int, int, int, int, int, char, int, char) {}
void nucleo_touch_add_pad(int, int, int, int, int) {}
namespace lg {
void font(int) {}
void fit(char *out, size_t n, const char *s, int) { snprintf(out, n, "%s", s ? s : ""); }
int  comp_head(const char *, unsigned short) { return 0; }
void comp_keys(const char *) {}
bool frame(void (*)(void *), void *, int, int) { return false; }
void field(int, int, int, int) {}
bool tap(int, char, int *, int *) { return false; }
}  // namespace lg
