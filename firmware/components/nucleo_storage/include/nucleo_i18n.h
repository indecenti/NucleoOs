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
bool nucleo_i18n_is_en(void);

// Switch language: flips the in-RAM flag AND persists ui.language to settings.json (read-modify-
// write, every other key preserved), so the choice survives reboot and reaches the web shell.
// On an ACTUAL change it bumps the generation counter and fires the on-change hook (below).
void nucleo_i18n_set_en(bool en);

// Monotonic generation counter — bumped on every real language change (from ANY surface: the native
// UI, or the web via POST /api/lang). A native screen compares this against its last-painted value
// each frame and repaints its chrome when it differs, so a language change is reflected LIVE without
// a reboot. Zero at boot.
uint32_t nucleo_i18n_gen(void);

// Optional hook fired AFTER the flag flips and settings.json is persisted (on the caller's thread),
// only when the language actually changed. The HTTP layer registers one that publishes a
// "system.language" event onto the bus -> the WebSocket sink pushes it to every connected browser,
// so a change made on the DEVICE reaches the web OS live. Kept as a callback so this low-level
// storage module needs no dependency on the event bus / httpd. Pass NULL to clear.
void nucleo_i18n_set_on_change(void (*cb)(bool en));

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
