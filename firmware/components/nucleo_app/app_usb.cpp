// USB Mass Storage app: exposes the SD card to a connected PC as a USB drive.
//
// Pressing ENTER switches the device into a DEDICATED USB-drive mode: it reboots and the next
// boot runs ONLY the USB-MSC loop (nucleo_usbmsc), so the firmware never touches the FAT while
// the PC owns it — no corruption (the ESP32-S3's USB-OTG also shares pins with the serial
// console, so it can't run alongside the normal OS anyway). Any key / reset returns to NucleoOS.
//
// UI: themed palette + app_ui_title accent header + a big size-2 call-to-action, matching the rest
// of the OS. Static single screen, so it draws direct (the 32 KB menu buffer is freed) — no cadence,
// no flicker.
#include "nucleo_app.h"
#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents
#include "app_gfx.h"
#include "app_ui.h"           // app_ui_title accent header
#include <M5GFX.h>
#include <stdio.h>
extern "C" {
#include "nucleo_usbmsc.h"
}

// BG/FG/MUTED/DIM come from launcher_theme.h.
static const unsigned short ACC = C_YELLOW, CAP = 0x1A8B;

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

static void enter(void)
{
    nucleo_app_set_direct_draw(true);   // static screen: draw direct, free the 32 KB menu buffer
    nucleo_app_set_hint("INVIO collega (riavvia)   esc indietro");
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (key == NK_ENTER || ch == ' ')   // ENTER or space: enter USB-drive mode
        nucleo_usbmsc_request();   // sets the reboot flag and restarts into USB-drive mode (no return;
                                   // any init error is shown by nucleo_usbmsc_run after the reboot)
}

static void draw(void)
{
    int top = nucleo_app_content_top();
    int h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);

    int y0 = app_ui_title("USB Drive", ACC, nullptr);

    txt(12, y0 + 12, "Monta la SD sul PC", FG, BG, 2);
    txt(12, y0 + 32, "col cavo USB-C, riavvia in USB.", MUTED, BG, 1);

    // big call-to-action
    int cy = y0 + 50;
    d.fillRoundRect(8, cy, 224, 26, 8, CAP);
    txt(16, cy + 5, "ENTER", ACC, CAP, 2);
    txt(84, cy + 11, "collega il drive", FG, CAP, 1);
    txt(12, cy + 34, "Un tasto sul PC torna a NucleoOS.", MUTED, BG, 1);
}

extern "C" void nucleo_register_usb(void)
{
    static const nucleo_app_def_t app = {
        "usb", "USB Drive", "Connect", "Access SD card from PC via USB",
        'U', ACC, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
