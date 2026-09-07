// Host stub — key codes only (see tools/alarm-host/README.md).
#pragma once
enum {
    NK_NONE = 0, NK_CHAR, NK_ENTER, NK_BACK, NK_TAB,
    NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT
};
typedef struct { int key; char ch; } nucleo_key_t;
