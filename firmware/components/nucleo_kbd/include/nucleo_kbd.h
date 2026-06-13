// Cardputer integrated keyboard, read as a GPIO matrix (pure ESP-IDF, no Arduino/M5 lib).
// Hardware facts (M5Cardputer, MIT): 74HC138 address GPIO {8,9,11}, 7 row inputs
// {13,15,3,4,5,6,7}, active-low with pull-ups; 8 columns x 7 rows -> 4x14 logical layout.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

enum { NK_NONE = 0, NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT, NK_ENTER, NK_BACK, NK_DEL, NK_TAB, NK_CHAR };

// Held-modifier bitmask (see nucleo_kbd_mods). Mapping of the Cardputer's modifier keys is
// best-effort and may need a per-board tweak (verify with the keyboard diagnostic).
#define NK_MOD_SHIFT 0x01
#define NK_MOD_CTRL  0x02
#define NK_MOD_ALT   0x04
#define NK_MOD_GUI   0x08   // Win / Cmd
#define NK_MOD_FN    0x10

typedef struct {
    int key;    // one of the NK_* codes
    char ch;    // printable character when applicable (also set for arrow keys)
} nucleo_key_t;

void nucleo_kbd_init(void);

// Non-blocking: returns a freshly pressed key, or {NK_NONE,0} if nothing new.
nucleo_key_t nucleo_kbd_read(void);

// Modifier keys held *right now* (NK_MOD_* bitmask). Valid alongside the last nucleo_kbd_read():
// e.g. Ctrl held + 'c' pressed reads as ch='c' with NK_MOD_CTRL set. Used by the USB Keyboard app
// to forward combos. Shift is also reflected here (the char is already shifted in `ch`).
unsigned char nucleo_kbd_mods(void);

#ifdef __cplusplus
}
#endif
