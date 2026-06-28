// USB Mass Storage app: exposes the SD card to a connected PC as a USB drive.
//
// Pressing ENTER switches the device into a DEDICATED USB-drive mode: it reboots and the next
// boot runs ONLY the USB-MSC loop (nucleo_usbmsc), so the firmware never touches the FAT while
// the PC owns it — no corruption (the ESP32-S3's USB-OTG also shares pins with the serial
// console, so it can't run alongside the normal OS anyway). Any key / reset returns to NucleoOS.
#include "nucleo_app.h"
#include <M5GFX.h>
#include <stdio.h>
extern "C" {
#include "nucleo_usbmsc.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, ACC = 0xFE8C, WARN = 0xF96B;

static void enter(void)
{
    nucleo_app_set_direct_draw(true);   // static screen: draw direct, free the 32 KB menu buffer
    nucleo_app_set_hint("enter/space connect (reboots)   esc back");
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

    d.setTextSize(2); d.setTextColor(ACC, BG);
    d.setCursor(54, top + 16); d.print("USB Drive");

    d.setTextSize(1); d.setTextColor(FG, BG);
    d.setCursor(16, top + 44); d.print("Mount the SD card on your PC");
    d.setTextColor(MUTED, BG);
    d.setCursor(16, top + 58); d.print("over the USB-C cable.");
    d.setCursor(16, top + 76); d.print("The device restarts into USB");
    d.setCursor(16, top + 88); d.print("mode; any key there returns.");

    d.setTextColor(ACC, BG);
    d.setCursor(16, top + 108); d.print("ENTER connect");
    d.setTextColor(WARN, BG);
    d.setCursor(150, top + 108); d.print("ESC back");
}

extern "C" void nucleo_register_usb(void)
{
    static const nucleo_app_def_t app = {
        "usb", "USB Drive", "Tools", "Access SD card from PC via USB",
        'U', ACC, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
