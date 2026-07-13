// Torch app: turn the Cardputer into a flashlight.
//
// The only human-visible light source on the device is the LCD itself, so "max light" means:
// fill the whole content area with pure white (every subpixel fully on -> max backlight
// transmission) AND drive the panel backlight to 100%. The standard Cardputer has no RGB/
// status LED; its only other emitter is the IR transmitter, which is invisible and useless
// for lighting, so there is nothing else to switch on.
//
// Controls: SPACE / ENTER toggle on/off, ESC exits (handled by the framework, which restores
// the previous brightness via on_exit).
//
// Global shortcut: a single quick tap on the G0 side button toggles this flashlight from
// ANYWHERE on the device (launcher, any app, Control Center) -- you don't have to open this app
// first. Holding G0 for ~1.5 s instead opens ANIMA. See toggle_torch() in nucleo_app.cpp.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include "nucleo_i18n.h"     // TR(it,en): hint follows the system language

static bool s_on;            // torch lit?
static int  s_prev_bright;   // brightness to restore when leaving the app

// Lit: backlight to 100% AND the footer turns white with dark text — so the whole panel emits,
// not just the content area (the dark hint bar was eating a strip of light). Off: restore the
// user's brightness and the default dark footer.
static void apply_state(void)
{
    nucleo_app_set_brightness(s_on ? 100 : s_prev_bright);
    if (s_on) nucleo_app_set_hint_colors(0xFFFF, 0x0000);   // white footer, dark legible text
    else      nucleo_app_set_hint_colors(0x0000, 0x8C71);   // default dark chrome
}

static void enter(void)
{
    s_prev_bright = nucleo_app_brightness();   // remember so ESC restores the user's setting
    s_on = true;                               // a flashlight should light up the moment you open it
    apply_state();
    nucleo_app_set_hint(TR("spazio on/off   esc esci", "space on/off   esc back"));
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (ch == ' ' || key == NK_ENTER) {        // ESC/back never reaches here: the framework closes the app
        s_on = !s_on;
        apply_state();
        nucleo_app_request_draw();
    }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    if (s_on) {
        // Pure white, no glyphs: any dark text would be light NOT emitted. Controls stay
        // legible in the dark hint bar below, so the whole panel can stay maximally bright.
        d.fillRect(0, top, 240, h, 0xFFFF);
        return;
    }
    // Off: dark screen with a clear prompt to switch the light on.
    d.fillRect(0, top, 240, h, 0x0841);
    const char *t1 = "TORCH OFF";
    const char *t2 = "press SPACE to turn on";
    const char *t3 = "tip: quick-tap the G0 side button";
    const char *t4 = "to toggle the torch from anywhere";
    d.setTextSize(3); d.setTextColor(0x8C71, 0x0841);
    d.setCursor((240 - (int)strlen(t1) * 18) / 2, top + h / 2 - 30); d.print(t1);
    d.setTextSize(1); d.setTextColor(0x4410, 0x0841);
    d.setCursor((240 - (int)strlen(t2) * 6) / 2, top + h / 2 + 4);  d.print(t2);
    d.setCursor((240 - (int)strlen(t3) * 6) / 2, top + h / 2 + 20); d.print(t3);
    d.setCursor((240 - (int)strlen(t4) * 6) / 2, top + h / 2 + 32); d.print(t4);
}

static void on_exit(void) { nucleo_app_set_brightness(s_prev_bright); }

extern "C" void nucleo_register_torch(void)
{
    static const nucleo_app_def_t app = {
        "torch", "Torch", "Tools", "Full-bright white screen flashlight",
        'T', 0xFE8C /* C_YELLOW */, enter, on_key, nullptr, draw, on_exit
    };
    nucleo_app_register(&app);
}
