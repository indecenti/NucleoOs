// presence-host: the few platform symbols nucleo_theme.cpp / nucleo_i18n.c reach for on the device.
#include <time.h>
extern "C" time_t host_time(time_t *out) { time_t t = 1767225600; if (out) *out = t; return t; }
