// The Games gallery (gamefront.cpp) in the app gallery: a launcher state, not an app, so it is wrapped in an app
// definition here, over a registry of real game ids (built in, from the Store, the two emulators, a non-game).
#include <M5GFX.h>
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "gamefront.h"
#include <stdio.h>

static void nop(void) {}
static void nokey(int, char) {}
static const nucleo_app_def_t REG[] = {
    { "stelle",  "Constellations", "Games", "", 'S', 0x4D1F, nop, nokey, nullptr, nop, nullptr },
    { "pong",    "Pong",           "Games", "", 'P', 0x8FF3, nop, nokey, nullptr, nop, nullptr },
    { "calc",    "Calculator",     "Tools", "", 'C', 0x4D1F, nop, nokey, nullptr, nop, nullptr },
    { "pinball", "Pinball",        "Games", "", 'F', 0xFE8C, nop, nokey, nullptr, nop, nullptr },
    { "gbemu",   "Game Boy",       "Games", "", 'G', 0x8FF3, nop, nokey, nullptr, nop, nullptr },
    { "tanks",   "Tanks",          "Games", "", 'T', 0xF96B, nop, nokey, nullptr, nop, nullptr },
    { "poker",   "Poker",          "Games", "", 'K', 0xC5F5, nop, nokey, nullptr, nop, nullptr },
    { "ggemu",   "Game Gear/Master System", "Games", "", 'G', 0x5D7F, nop, nokey, nullptr, nop, nullptr },
    { "nesemu",  "NES",            "Games", "", 'N', 0xAD55, nop, nokey, nullptr, nop, nullptr },
    { "brawler", "Brawler",        "Games", "", 'B', 0xFBB6, nop, nokey, nullptr, nop, nullptr },
    { "reactor", "Reactor",        "Games", "", 'R', 0xFE8C, nop, nokey, nullptr, nop, nullptr },
};
int nucleo_app_count(void) { return (int)(sizeof REG / sizeof REG[0]); }
const nucleo_app_def_t *nucleo_app_at(int i) { return &REG[i]; }

static void enter(void)
{
    gamefront_open();
    while (gamefront_scanning()) gamefront_step();   // the walk for cartridges, all at once on the PC
}
static void key(int k, char ch)
{
    char id[40];
    const int r = gamefront_key(k, ch, id, sizeof id);
    if (r == GF_LAUNCH) {
        const char *f = gamefront_launch_file();
        fprintf(stderr, "launch %s %s\n", id, f ? f : "");
        gamefront_open();                           // back from the game, as the launcher does
    }
}
extern M5GFX d;
// Each look is a whole frame on the PC (the device repaints only the wheel's band between the frames of a turn).
static void draw(void)
{
    gamefront_invalidate();
    if (nucleo_app_two_screens()) gamefront_draw_companion(&d);
    else gamefront_render();
}
static void draw_large(void) { gamefront_invalidate(); gamefront_render(); }

extern "C" void nucleo_register_gamefront(void)
{
    static const nucleo_app_def_t app = { "gamefront", "Games", "Games", "", 'G', 0xFB4D, enter, key, nullptr, draw, nullptr,
                                          0, nullptr, draw_large };
    nucleo_app_register(&app);
}
