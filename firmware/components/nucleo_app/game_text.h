// game_text — the native games' text in all five OS languages (it/en/es/fr/de) at ~zero flash cost.
//
// A game writes every user-visible string as GT("Italian", "English"): those two stay flash literals,
// exactly like TR(). Spanish, French and German come from a small per-game pack on the SD card,
// /sd/system/i18n/games/<game>.<lang>, keyed by the English string. The pack is read on game_text_open()
// (on_enter) and freed on game_text_close() (on_exit): zero RAM while no game is open, and a language
// without a pack (or a string missing from it) falls back to English. The game follows the OS language
// (settings.json -> ui.language) and picks up a live change on the next GT() call.
//
// Packs are built from tools/game-i18n/<game>.json by tools/game-i18n/build.mjs; tools/game-i18n/
// check.mjs fails when a GT() English literal in a game source has no es/fr/de translation.
// Strings are ASCII only: the on-TFT fonts have no accented glyphs.
#pragma once

void game_text_open(const char *game);                 // game = the app's registry id ("pong", "tanks", ...)
void game_text_close(void);
const char *game_text(const char *it, const char *en);

#define GT(it_, en_) game_text((it_), (en_))
// A pair kept in a const table and translated where it is shown: { GTK("Salta", "Jump") } stores both
// literals; draw with game_text(row.it, row.en). tools/game-i18n/check.mjs collects GTK pairs like GT.
#define GTK(it_, en_) (it_), (en_)
