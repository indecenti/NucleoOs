// Store-only: MicroQuickJS (vendor/mquickjs, MIT) compiled into the Code module, never into the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "vendor/mquickjs/libm.c"
#endif
