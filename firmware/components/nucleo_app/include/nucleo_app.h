// On-device app framework: single-foreground apps + Wear OS-style launcher.
// Built on M5GFX/M5Unified (assumes nucleo_ui_init() already started M5Cardputer).
// See docs/device-ui.md.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Key events delivered to apps are now defined in nucleo_kbd.h
#include "nucleo_kbd.h"
typedef struct {
    const char *id;
    const char *name;
    const char *category;   // e.g. "Media", "Tools", "System"
    const char *desc;       // short description for the UI
    char icon;              // single glyph letter shown in the launcher
    unsigned short color;   // accent color (RGB565)
    void (*on_enter)(void);          // app becomes foreground
    void (*on_key)(int key, char ch);
    void (*on_tick)(void);           // ~5x/second while foreground
    void (*on_draw)(void);           // draw the content area
    void (*on_exit)(void);           // app closed (Esc) -> back to launcher
    // Declarative exclusive mode (nucleo_exclusive.h). 0 = none. When non-zero the framework enters
    // exclusive on open (before on_enter) and the central safety-net restores on close — for heavy
    // apps that need dedicated RAM for their WHOLE foreground life (e.g. SSH: NX_NET_APP). Apps that
    // want reclaim only DURING a heavy action (Video/Recorder/Music play) leave this 0 and call
    // nucleo_exclusive_enter/exit themselves; the close safety-net still covers early exit.
    unsigned int exclusive_flags;    // trailing field: existing positional initializers zero it
} nucleo_app_def_t;

// Register a native app (call before nucleo_app_run).
void nucleo_app_register(const nucleo_app_def_t *app);

// Register the built-in apps (Clock, System status, ...).
void nucleo_app_register_builtins(void);

// Run the launcher loop forever (call once from app_main after setup).
void nucleo_app_run(void);

// Helpers apps may use while drawing.
void nucleo_app_set_hint(const char *hint);   // bottom hint-bar text
void nucleo_app_set_hint_colors(unsigned short bg, unsigned short fg);  // override footer colors (auto-reset on app open/close)
int  nucleo_app_content_top(void);             // first usable content y
int  nucleo_app_content_height(void);          // usable content height
void nucleo_app_request_draw(void);            // ask for a redraw next loop
void nucleo_app_release_buffers(void);         // free launcher off-screen buffers (reclaim RAM)
void nucleo_app_exit(void);                    // close the current app and return to launcher

// Let the foreground app claim TAB for its own overlay (e.g. ANIMA settings). Set in on_enter;
// auto-cleared when any app opens or closes, so it never leaks across apps. When set (and the
// global Control Center isn't up), TAB calls fn instead of toggling the Control Center.
void nucleo_app_set_tab_handler(void (*fn)(void));

// Let the foreground app intercept the Back/Esc (and Left) key — e.g. ANIMA's settings sheet
// returns to the chat instead of closing the app. Set in on_enter; auto-cleared on app open/close.
// Return true if the app consumed the key; false lets the framework close the app as usual.
void nucleo_app_set_back_handler(bool (*fn)(int key));

// Let the foreground app own a GO-button HOLD (push-to-talk): the framework calls fn(true) when the
// hold engages and fn(false) on release — INSTEAD of the voice recognizer, and WITHOUT blanking the
// screen (the app stays visible). The Voice Recorder uses it to record while GO is held. A quick GO TAP
// still toggles the torch as usual. Set in on_enter; auto-cleared on app open/close (like the Back hook).
void nucleo_app_set_ptt_handler(void (*fn)(bool on));

// Live apps that drive their own data source (e.g. the mic-spectrum DSP) register a per-loop poll.
// The run loop calls fn() every iteration (~50 Hz) while the app is foreground; return true ONLY when
// a new frame is ready so the framework composites+blits at the DATA rate, not the loop rate. This is
// the high-frequency counterpart of on_tick (5 Hz, too slow for a 31 Hz analyzer) and the cure for the
// duplicate-frame redraws ANTI-FLICKER.md #4 warns about. Set in on_enter; auto-cleared on open/close.
void nucleo_app_set_poll_handler(bool (*fn)(void));

// An app that has released the shared canvas (ANIMA frees it for its index + online worker) pins
// itself to DIRECT drawing: the run loop then never lazily re-acquires the 32 KB canvas, which
// would otherwise eat the very RAM the app freed. The app's on_draw must self-clear only the
// regions it changes (ANTI-FLICKER.md, technique 2). Set in on_enter; auto-cleared on open/close.
void nucleo_app_set_direct_draw(bool on);
void nucleo_app_launch_file(const char *path); // open the app associated with a file (open-with)
bool nucleo_app_launch_id(const char *id);     // open a registered app by its id (e.g. ANIMA "open the player"); false if none
// Map an ANIMA/registry app id (app-aliases.json: "media-player","photo-viewer"...) to the NATIVE
// launcher id ("music","photos"...). FONTE UNICA per app ANIMA e voce: "apri musica" apre la app giusta.
// Id gia' nativo o sconosciuto -> ritornato invariato. (Non launchabile -> nucleo_app_launch_id=false.)
const char *nucleo_app_native_id(const char *anima_id);

// Display backlight, 0..100 (%). Centralized so the Control Center and the video player
// share one source of truth (the panel has no brightness getter of its own). Persists
// across apps; clamped to a readable floor.
void nucleo_app_set_brightness(int pct);
int  nucleo_app_brightness(void);

#ifdef __cplusplus
}
#endif
