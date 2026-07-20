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
// Force a FULL panel re-blit next loop (not just the band-dirty diff). Call after an app has drawn
// DIRECT to the panel behind the run loop's back — e.g. a blocking playback/preview modal that freed
// and re-acquired the shared canvas: the per-band hashes still describe the PRE-modal frame, so the
// normal diff would leave the stale direct-drawn pixels on screen. This invalidates that cache.
void nucleo_app_force_repaint(void);
void nucleo_app_release_buffers(void);         // free launcher off-screen buffers (reclaim RAM)
// Heavy one-shot HTTP downloads (OTA) raise this so the run loop LAUNCHES Remote Control from the UI task
// — the RAM-listening posture (frees the 32 KB canvas + L1) that lets a flash through, but automatic.
// on=false restores. _ready() reports once the posture is actually live, so the caller can wait for it.
void nucleo_app_request_remote_listen(bool on);
bool nucleo_app_remote_listen_ready(void);
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
// Fullscreen: an app (e.g. a game's action screen) reclaims the bottom hint bar — the whole 240x135
// is its content area, the blit is clipped to H (not H-HINT) and the footer is NOT drawn. content_height()
// then returns H. Set/clear it per screen; auto-cleared on app open/close (the footer comes back).
void nucleo_app_set_fullscreen(bool on);
bool nucleo_app_launch_file(const char *path); // open the app associated with a file (open-with); false if no viewer for the type
// Viewer on_enter helper: the EXACT file Files asked to open ("open with"), consumed once — NULL
// when the app was opened normally. Lets Notes/Music/Photos open that file, not their own folder.
const char *nucleo_app_take_open_file(void);
// True if an image is too large for the on-device (no-PSRAM) decoder, so the file manager warns and
// points to the web app instead of crashing. Reads ONLY the header (no decode). Unknown type -> false.
bool nucleo_app_image_oversize(const char *path);
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

// Persist the current brightness + volume + mute to settings.json (survives reboot; boot re-applies).
// Call from the DELIBERATE user controls only (Control Center, native settings, ANIMA), at edit-mode
// exit or on a toggle — never from the transient game/torch/QR/video boosts.
void nucleo_app_persist_prefs(void);

// ─── ANIMA Solo mode ─────────────────────────────────────────────────────────
// Reboot into a dedicated "ANIMA Solo" personality: the SAME firmware image, but main() brings up only
// display + SD + Wi-Fi + arbiter + ANIMA/TTS/voice and SKIPS httpd / mDNS / recorder / auth / IR. The
// assistant then owns a large, UNFRAGMENTED heap (online TLS + L1 + voice all fit) instead of the ~4 KB
// it gets while the full OS runs. The flag lives in RTC no-init RAM: it survives the warm esp_restart but
// NOT a cold power-on, so a power-cycle always lands in the normal OS — you can never get stuck in Solo.
void nucleo_anima_solo_request(void);   // set the flag + esp_restart() into Solo (NEVER returns)
bool nucleo_anima_solo_pending(void);   // main(): consume the RTC flag once + latch "active" for this boot
bool nucleo_anima_solo_active(void);    // true for the whole boot if we came up in Solo

#ifdef __cplusplus
}
#endif
