// System UI language for the NATIVE (C/C++ on-TFT) side of NucleoOS. See docs/i18n.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One global flag is the ENTIRE RAM footprint of native i18n. Every translated string is an inline
// flash literal supplied at the call site (see TR / nucleo_tr), so nothing per-string lives in RAM.
// The single source of truth is settings.json -> ui.language ("it" | "en"), the same key the web
// shell uses, so the two surfaces agree across a reboot.

// Read the saved language from settings.json once at boot (call after the SD is mounted). Default IT.
void nucleo_i18n_load(void);

// Hot accessor: true when the system language is English. Native UIs call this while painting.
// (The native TFT strings are IT/EN only; for es/fr/de this is false → Italian base is painted.)
bool nucleo_i18n_is_en(void);

// The active OS language code verbatim ("it","en","es","fr","de",…). settings.json -> ui.language is
// the single OS-wide signal; this returns exactly what was set, so the web keeps the precise choice.
const char *nucleo_i18n_lang(void);

// Set the OS language to any code and persist ui.language VERBATIM (read-modify-write, other keys
// preserved). The web calls this via POST /api/lang so a language the browser supports (es/fr/de) is
// not collapsed to it/en on the device. On an ACTUAL change it bumps the generation + fires the hook.
void nucleo_i18n_set_lang(const char *code);

// Convenience for the native IT/EN toggle sites: equivalent to set_lang("en"|"it").
void nucleo_i18n_set_en(bool en);

// Monotonic generation counter — bumped on every real language change (from ANY surface: the native
// UI, or the web via POST /api/lang). A native screen compares this against its last-painted value
// each frame and repaints its chrome when it differs, so a language change is reflected LIVE without
// a reboot. Zero at boot.
uint32_t nucleo_i18n_gen(void);

// Optional hook fired AFTER the language changes and settings.json is persisted (on the caller's
// thread), only on an actual change. `code` is the new language verbatim. The HTTP layer registers one
// that publishes a "system.language" event onto the bus -> the WebSocket sink pushes it to every
// connected browser, so a change made on the DEVICE reaches the web OS live. Kept as a callback so this
// low-level storage module needs no dependency on the event bus / httpd. Pass NULL to clear.
void nucleo_i18n_set_on_change(void (*cb)(const char *code));

// Pick the right literal for the current language. Both args are flash literals -> zero RAM cost.
const char *nucleo_tr(const char *it, const char *en);

#ifdef __cplusplus
}
#endif

// Call-site sugar mirroring the inline TR(it,en) idiom already used in app_recorder.cpp et al.
// Guarded so a file with its own local TR macro is never clobbered.
#ifndef TR
#define TR(it_, en_) nucleo_tr((it_), (en_))
#endif
