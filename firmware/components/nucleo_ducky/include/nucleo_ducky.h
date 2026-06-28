// nucleo_ducky — DuckyScript parser/runner core. FOR AUTHORIZED TESTING ONLY.
//
// A pure, transport-agnostic, host-testable engine: it parses a DuckyScript payload and drives an
// INJECTED backend (USB HID over the cable, or BLE HID over the air — same parser). Two things Bruce's
// runner lacks are first-class here: a DRY-RUN preview (validate a payload's actions + estimated time +
// syntax errors before a single keystroke fires) and a selectable keyboard LAYOUT (US/IT — the keycode
// -> glyph mapping is host-layout dependent, so a US-only runner mistypes on an Italian host).
//
// Supported DuckyScript: REM, STRING, STRINGLN, DELAY, DEFAULT_DELAY/DEFAULTDELAY, STRING_DELAY,
// REPEAT, and key-combo lines (modifiers GUI/CTRL/ALT/SHIFT/ALTGR + a named key or a single char),
// named keys (ENTER, ESC, TAB, SPACE, arrows, HOME/END, INSERT/DELETE, PAGEUP/DOWN, F1-F12, ...).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// HID modifier byte bits (USB HID spec). ALTGR = Right Alt, used by the IT layout for @ # [ ] { } ...
#define DUCKY_MOD_CTRL  0x01
#define DUCKY_MOD_SHIFT 0x02
#define DUCKY_MOD_ALT   0x04
#define DUCKY_MOD_GUI   0x08
#define DUCKY_MOD_ALTGR 0x40

typedef enum { DUCKY_LAYOUT_US = 0, DUCKY_LAYOUT_IT = 1 } nucleo_ducky_layout_t;

// The injected output. The whole engine emits only key(mod,keycode) press+release events plus delays,
// so a backend is tiny: wire key() to nucleo_usbhid_key or the BLE HID tap, and you're done.
typedef struct {
    bool (*ready)(void *ctx);                              // host attached / link subscribed?
    void (*key)(void *ctx, uint8_t mod, uint8_t keycode);  // one keystroke, press then release
    void (*delay)(void *ctx, uint32_t ms);                 // wait
    bool (*aborted)(void *ctx);                            // user asked to stop? (polled between lines)
    void (*progress)(void *ctx, int line, int total);      // optional UI callback (may be NULL)
    void *ctx;
} nucleo_ducky_backend_t;

// Static analysis (dry-run) — no backend, no keystrokes. The preview Bruce doesn't have.
typedef struct {
    int      lines;        // executable lines (non-empty, non-REM)
    int      keystrokes;   // total key events that WILL be sent
    int      strings;      // STRING / STRINGLN commands
    int      errors;       // unrecognised commands or key names
    uint32_t est_ms;       // estimated wall-clock (DELAYs + per-key time)
    char     first_error[48];
} nucleo_ducky_stat_t;

// Resolve a key NAME (e.g. "ENTER", "GUI", "F5") under a layout. 1 = real key (keycode set, mod maybe 0),
// 2 = pure modifier (mod set, keycode 0), 0 = unknown.
int  nucleo_ducky_keyname(const char *name, nucleo_ducky_layout_t layout, uint8_t *mod, uint8_t *keycode);
// Resolve a single printable char under a layout. false if not typeable.
bool nucleo_ducky_char(char c, nucleo_ducky_layout_t layout, uint8_t *mod, uint8_t *keycode);

// Dry-run analysis. `out` filled with counts + estimate; never touches hardware.
void nucleo_ducky_analyze(const char *script, size_t len, nucleo_ducky_layout_t layout, nucleo_ducky_stat_t *out);

// Execute the payload. Returns the number of executable lines run (stops early if aborted()/!ready()).
int  nucleo_ducky_run(const char *script, size_t len, nucleo_ducky_layout_t layout,
                      const nucleo_ducky_backend_t *be);

#ifdef __cplusplus
}
#endif
