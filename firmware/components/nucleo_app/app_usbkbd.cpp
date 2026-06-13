// USB Keyboard app: type on the Cardputer, the keystrokes go to the connected PC/phone over
// USB-C (TinyUSB HID — see nucleo_usbhid). Turns the device into a real hardware keyboard,
// modifiers and combos included (Ctrl/Alt/Win + key, e.g. Ctrl+C, Alt+Tab).
//
// It runs as a blocking modal (like the media players) that reads the keyboard DIRECTLY, so it
// sees every key — including the ones the launcher loop would otherwise eat (',' and the arrow
// legends). The backtick (top-left, "ESC") leaves the keyboard. Installing USB HID takes over the
// USB-OTG PHY, so the serial console is gone until the next reboot (OTA over Wi-Fi still works).
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nucleo_kbd.h"
#include "nucleo_usbhid.h"
}

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x4DDF, GRN = 0x8FF3, WARN = 0xFE8C, HL = 0x4DDF, INK = 0x0000;

// One modifier chip in the live indicator row.
static void chip(int x, int y, const char *label, bool on)
{
    int w = (int)strlen(label) * 6 + 8;
    d.fillRoundRect(x, y, w, 13, 3, on ? HL : 0x10A2);
    d.setTextColor(on ? INK : MUTED, on ? HL : 0x10A2);
    d.setCursor(x + 4, y + 3); d.print(label);
}

// Full status screen for the typing loop. `m` = held modifiers, `conn` = host ready, `echo` = last
// thing sent (shown so you can see it's working).
static void kbd_screen(unsigned char m, bool conn, const char *echo)
{
    d.fillScreen(BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, 6); d.print("USB Keyboard");

    d.setTextSize(1);
    d.fillCircle(15, 32, 4, conn ? GRN : WARN);
    d.setTextColor(conn ? GRN : WARN, BG); d.setCursor(26, 28);
    d.print(conn ? "Connected to host" : "Waiting for PC...");

    // Live modifier indicators (also a handy map check: press a modifier, see which lights).
    int x = 8, y = 52;
    chip(x, y, "CTRL", m & NK_MOD_CTRL);  x += 38;
    chip(x, y, "ALT",  m & NK_MOD_ALT);   x += 32;
    chip(x, y, "SHIFT",m & NK_MOD_SHIFT); x += 44;
    chip(x, y, "GUI",  m & NK_MOD_GUI);   x += 32;
    chip(x, y, "FN",   m & NK_MOD_FN);

    d.setTextColor(MUTED, BG); d.setCursor(8, 74); d.print("Sent:");
    d.setTextColor(FG, BG); d.setCursor(46, 74); d.print(echo && echo[0] ? echo : "-");

    d.setTextColor(DIM, BG);
    d.setCursor(8, 96);  d.print("Ctrl/Alt/Win + key = combo");
    d.setCursor(8, 108); d.print("Fn + ; . , / = arrows");
    d.setTextColor(WARN, BG);
    d.setCursor(8, 122); d.print("`  exit keyboard");
}

// Friendly label for the just-sent key (for the on-screen echo).
static void echo_for(char ch, int special, char *out, size_t n)
{
    if (special == NK_HID_ENTER) snprintf(out, n, "Enter");
    else if (special == NK_HID_BKSP) snprintf(out, n, "Backspace");
    else if (special == NK_HID_TAB) snprintf(out, n, "Tab");
    else if (special == NK_HID_SPACE) snprintf(out, n, "Space");
    else if (special == NK_HID_UP) snprintf(out, n, "Up");
    else if (special == NK_HID_DOWN) snprintf(out, n, "Down");
    else if (special == NK_HID_LEFT) snprintf(out, n, "Left");
    else if (special == NK_HID_RIGHT) snprintf(out, n, "Right");
    else if (ch) snprintf(out, n, "%c", ch);
    else out[0] = 0;
}

// Honest failure screen if the USB HID driver can't be installed (e.g. the PHY is already owned
// by another USB class). Without this the user would stare at "Waiting for PC..." forever.
static void err_screen(esp_err_t e)
{
    d.fillScreen(BG);
    d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, 6); d.print("USB Keyboard");
    d.setTextSize(1); d.setTextColor(WARN, BG);
    d.setCursor(8, 36); d.print("USB HID install failed.");
    d.setTextColor(MUTED, BG);
    d.setCursor(8, 52); d.print(esp_err_to_name(e));
    d.setCursor(8, 76); d.print("Reboot may be needed to free");
    d.setCursor(8, 88); d.print("the USB port (serial console).");
    d.setTextColor(WARN, BG);
    d.setCursor(8, 112); d.print("`  back");
}

static void kbd_loop(void)
{
    esp_err_t e = nucleo_usbhid_start();         // install USB HID (idempotent; takes the USB PHY)
    if (e != ESP_OK) {                           // surface the failure instead of a perennial "Waiting"
        err_screen(e);
        while (true) {                           // wait for the backtick, then return to the intro
            esp_task_wdt_reset();
            nucleo_key_t k = nucleo_kbd_read();
            if (k.key == NK_BACK) break;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        d.fillScreen(BG);
        nucleo_app_request_draw();
        return;
    }

    char echo[12] = ""; unsigned char last_m = 0xFF; int last_conn = -1; bool first = true;
    bool back = false;
    while (!back) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        unsigned char m = nucleo_kbd_mods();
        bool conn = nucleo_usbhid_ready();
        bool sent = false;

        if (k.key != NK_NONE) {
            if (k.key == NK_BACK) { back = true; break; }   // backtick leaves the keyboard

            unsigned char hidmod = 0;
            if (m & NK_MOD_CTRL) hidmod |= NK_HIDMOD_CTRL;
            if (m & NK_MOD_ALT)  hidmod |= NK_HIDMOD_ALT;
            if (m & NK_MOD_GUI)  hidmod |= NK_HIDMOD_GUI;
            bool fn = (m & NK_MOD_FN);

            unsigned char kc = 0, kmod = hidmod;
            int special = 0;
            if (fn && (k.key == NK_UP || k.key == NK_DOWN || k.key == NK_LEFT || k.key == NK_RIGHT)) {
                kc = (k.key == NK_UP) ? NK_HID_UP : (k.key == NK_DOWN) ? NK_HID_DOWN
                   : (k.key == NK_LEFT) ? NK_HID_LEFT : NK_HID_RIGHT;
                kmod = hidmod; special = kc;
            } else if (k.key == NK_ENTER) { kc = NK_HID_ENTER; special = kc; kmod |= (m & NK_MOD_SHIFT) ? NK_HIDMOD_SHIFT : 0; }
            else if (k.key == NK_DEL)     { kc = NK_HID_BKSP;  special = kc; }
            else if (k.key == NK_TAB)     { kc = NK_HID_TAB;   special = kc; kmod |= (m & NK_MOD_SHIFT) ? NK_HIDMOD_SHIFT : 0; }
            else if (k.ch == ' ')         { kc = NK_HID_SPACE; special = kc; }
            else if (k.ch >= 33 && k.ch < 127) {
                unsigned char am, ak;
                if (nucleo_usbhid_ascii(k.ch, &am, &ak)) { kc = ak; kmod |= am; }
            }

            if (kc) {
                nucleo_usbhid_key(kmod, kc);
                echo_for(k.ch, special, echo, sizeof echo);
                sent = true;
            }
        }

        if (first || sent || m != last_m || (int)conn != last_conn) {
            kbd_screen(m, conn, echo);
            last_m = m; last_conn = conn; first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    d.fillScreen(BG);
    nucleo_app_request_draw();                    // back to the intro screen
}

// ---- intro screen (before you start typing) ---------------------------------
static void enter(void)
{
    nucleo_app_set_hint("enter start typing   esc back");
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (key == NK_ENTER || ch == ' ') { kbd_loop(); nucleo_app_request_draw(); }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 12); d.print("USB Keyboard");
    d.setTextSize(1); d.setTextColor(FG, BG);
    d.setCursor(10, top + 42); d.print("Type on the Cardputer, the");
    d.setCursor(10, top + 54); d.print("keys go to the PC over USB-C.");
    d.setTextColor(MUTED, BG);
    d.setCursor(10, top + 72); d.print("Ctrl/Alt/Win combos supported.");
    d.setTextColor(GRN, BG);  d.setCursor(10, top + 96); d.print("ENTER");
    d.setTextColor(MUTED, BG); d.setCursor(58, top + 96); d.print("start typing");
}

extern "C" void nucleo_register_usbkbd(void)
{
    static const nucleo_app_def_t app = {
        "usbkbd", "USB Keyboard", "Tools", "Type from the Cardputer into a PC over USB",
        'K', 0x4DDF, enter, on_key, nullptr, draw, nullptr
    };
    nucleo_app_register(&app);
}
