// Host test for firmware/components/nucleo_app/game_text.cpp (run by game-text.test.mjs): the real module
// against packs built by build.mjs, plus damaged packs it must refuse without reading out of bounds.
#include "game_text.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *s_lang = "it";
static uint32_t s_gen = 1;
extern "C" const char *nucleo_i18n_lang(void) { return s_lang; }
extern "C" uint32_t nucleo_i18n_gen(void) { return s_gen; }
static void set_lang(const char *l) { s_lang = l; s_gen++; }

static int fails;
static void eq(const char *got, const char *want, const char *what) {
    if (strcmp(got, want)) { printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); fails++; }
}

int main(void) {
    game_text_open("demo");
    eq(GT("Gioca", "Play"), "Gioca", "it from flash");
    set_lang("en"); eq(GT("Gioca", "Play"), "Play", "en from flash");
    set_lang("es"); eq(GT("Gioca", "Play"), "Jugar", "es from the pack (live language change)");
    eq(GT("Livello %d", "Level %d"), "Nivel %d", "es format string");
    eq(GT("Nuovo", "Brand new"), "Brand new", "es: a string missing from the pack falls back to English");
    set_lang("de"); eq(GT("Gioca", "Play"), "Spielen", "de from the pack");
    set_lang("fr"); eq(GT("Gioca", "Play"), "Jouer", "fr from the pack");
    set_lang("pt"); eq(GT("Gioca", "Play"), "Play", "an unshipped language reads English");
    game_text_close();
    set_lang("es"); eq(GT("Gioca", "Play"), "Play", "after close: no pack, English");
    // damaged packs (written by the .mjs runner): each must be refused -> English, never a crash
    static const char *bad[] = { "truncated", "badmagic", "offsetout", "nonul", "huge" };
    for (const char *b : bad) {
        game_text_open(b);
        char what[64]; snprintf(what, sizeof what, "damaged pack '%s' is refused", b);
        eq(GT("Gioca", "Play"), "Play", what);
        game_text_close();
    }
    printf("game_text: %s\n", fails ? "FAILED" : "all green");
    return fails ? 1 : 0;
}
