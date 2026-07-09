// app_ble.cpp — Security > BLE suite (scan / spam / iBeacon / HID keyboard). FOR AUTHORIZED TESTING ONLY.
//
// Declares exclusive_flags = NX_DEEP_OFFLINE: the framework tears Wi-Fi (+httpd/mDNS/voice/L1) down
// BEFORE on_enter and restores it all on close — "all RAM concentrated for BLE, back to normal on
// exit". on_enter brings the NimBLE controller up in that freed space; on_exit gives it back. A menu
// picks the active mode (only one runs at a time).
//
// UI is the OS-standard native look: themed palette (launcher_theme), an app_ui_title accent header on
// every screen, and the shared app_ui_list widget for the mode menu (size-2 focus rows, 1-8 instant
// launch, type-ahead). Back/Esc returns to the menu first, then closes the app.
#include "nucleo_app.h"
#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "app_gfx.h"
#include "app_ui.h"           // shared focused-list widget + quick-select/type-ahead nav
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_restart (apply the Bluetooth on/off preference)
extern "C" {
#include "nucleo_ble.h"
#include "nucleo_exclusive.h"
}

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short ACC = C_BLUE, GRN = C_GREEN, WARN = C_YELLOW, SURF = 0x10A2, CAP = 0x1A8B;

enum Screen { MENU, SCAN, SPAM, IBEACON, HID };
static Screen s_screen = MENU;
static int    s_sel = 0;          // menu cursor
static nucleo_ble_spam_t s_target = NUCLEO_BLE_SPAM_IOS;
static bool   s_up_ok = false;
static bool   s_bt_off = false;       // boot released the radio for RAM -> show the enable prompt

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// Apply the Bluetooth on/off choice: persist the preference and reboot (the release happens at boot).
static void reboot_with_bt(bool on) { nucleo_ble_set_pref(on); esp_restart(); }
static void ble_tab(void) { reboot_with_bt(s_bt_off); }   // TAB toggles: off->on, on->off (then reboots)

struct Item { const char *label; const char *sub; };
static const Item MENU_ITEMS[] = {
    { "Scansione",   "dispositivi BLE vicini" },
    { "Spam iOS",    "popup pairing Apple" },
    { "Spam Android","popup Fast Pair" },
    { "Spam Windows","popup Swift Pair" },
    { "Spam Samsung","popup Galaxy Buds" },
    { "Spam Tutti",  "ruota tutti gli OS" },
    { "iBeacon",     "beacon standard" },
    { "Tastiera HID","keyboard + media" },
};
static const int N_ITEMS = (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
static const char *ble_label(int i, void *) { return MENU_ITEMS[i].label; }

// ---- mode control ----------------------------------------------------------------------------------
static void stop_all(void) { nucleo_ble_scan_stop(); nucleo_ble_spam_stop(); nucleo_ble_ibeacon_stop(); nucleo_ble_hid_stop(); }

static void activate(int idx)
{
    switch (idx) {
        case 0: s_screen = SCAN; nucleo_ble_scan_start(); break;
        case 1: s_target = NUCLEO_BLE_SPAM_IOS;     s_screen = SPAM; nucleo_ble_spam_start(s_target); break;
        case 2: s_target = NUCLEO_BLE_SPAM_ANDROID; s_screen = SPAM; nucleo_ble_spam_start(s_target); break;
        case 3: s_target = NUCLEO_BLE_SPAM_WINDOWS; s_screen = SPAM; nucleo_ble_spam_start(s_target); break;
        case 4: s_target = NUCLEO_BLE_SPAM_SAMSUNG; s_screen = SPAM; nucleo_ble_spam_start(s_target); break;
        case 5: s_target = NUCLEO_BLE_SPAM_ALL;     s_screen = SPAM; nucleo_ble_spam_start(s_target); break;
        case 6: s_screen = IBEACON; nucleo_ble_ibeacon_start(); break;
        case 7: s_screen = HID; nucleo_ble_hid_start(); break;
    }
    nucleo_app_request_draw();
}

// Back/Esc: a running mode returns to the menu; from the menu, let the framework close the app.
static bool on_back(int key)
{
    (void)key;
    if (s_screen != MENU) { stop_all(); s_screen = MENU; nucleo_app_request_draw(); return true; }
    return false;
}

// ---- lifecycle -------------------------------------------------------------------------------------
static void on_enter(void)
{
    // Framework has already entered NX_DEEP_OFFLINE here (Wi-Fi/httpd/mDNS/voice/L1 down).
    s_screen = MENU; s_sel = 0;
    s_bt_off = !nucleo_ble_radio_present();          // radio freed at boot for RAM -> offer to enable
    s_up_ok  = s_bt_off ? false : nucleo_ble_up();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(ble_tab);
    nucleo_app_set_hint(s_bt_off ? "invio: attiva bluetooth e riavvia"
                                 : "1-8 scegli  enter avvia  esc indietro  tab spegni bt");
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    if (s_bt_off) return;     // never brought the radio up -> nothing to tear down
    stop_all();
    nucleo_ble_down();        // RAM back to the heap; framework then restores Wi-Fi/httpd/etc.
}

static void on_key(int key, char ch)
{
    if (s_screen == MENU) {
        if (s_bt_off) { if (key == NK_ENTER) reboot_with_bt(true); return; }   // enable Bluetooth + reboot
        if (ch >= '1' && ch <= '0' + N_ITEMS) { s_sel = ch - '1'; activate(s_sel); return; }   // number = instant launch
        if (app_ui_list_key(key, ch, &s_sel, N_ITEMS, ble_label, nullptr)) { nucleo_app_request_draw(); return; }
        if (key == NK_ENTER) activate(s_sel);
    } else if (s_screen == HID) {
        if (key == NK_ENTER) { if (nucleo_ble_hid_ready()) nucleo_ble_hid_type("NucleoOS BLE HID\n"); }
        else if (ch >= '1' && ch <= '6') nucleo_ble_hid_media(ch - '1');  // 1 play/pause .. 6 mute
        nucleo_app_request_draw();
    }
}

static void on_tick(void) { if (s_screen != MENU) nucleo_app_request_draw(); }

// ---- drawing ---------------------------------------------------------------------------------------
static void draw_menu(int top, int h)
{
    (void)top;
    if (s_bt_off) {   // Bluetooth disabled to reclaim RAM — offer to enable it
        int y0 = app_ui_title("BLE", ACC, nullptr);
        txt(10, y0 + 12, "Bluetooth spento", FG, BG, 2);
        txt(10, y0 + 34, "Liberati ~35 KB per ANIMA e sistema.", MUTED, BG, 1);
        txt(10, y0 + 54, "INVIO: attiva e riavvia.", WARN, BG, 1);
        return;
    }
    int y0 = app_ui_title("BLE", ACC, "solo test autorizzati");
    if (!s_up_ok) { txt(10, y0 + 14, "Controller BLE non avviato.", WARN, BG, 2); return; }
    const int foot = 16;
    app_ui_list(y0, h - y0 - foot, N_ITEMS, s_sel, ble_label, nullptr, nullptr, nullptr);
    // focused-item description on a footer strip (launcher idiom: the instruction line)
    d.fillRoundRect(6, h - foot, W - 12, foot - 2, 5, SURF);
    txt(12, h - foot + 3, MENU_ITEMS[s_sel].sub, MUTED, SURF, 1);
}

static void draw_scan(int top, int h)
{
    (void)top; (void)h;
    char cb[16]; int cnt = nucleo_ble_scan_count();
    snprintf(cb, sizeof cb, "%d device", cnt);
    int y0 = app_ui_title("Scansione", ACC, nucleo_ble_is_synced() ? "attiva" : "avvio...");
    txt(8, y0 + 4, cb, ACC, BG, 2);
    int y = y0 + 26; nucleo_ble_dev_t dv; char ln[56];
    for (int i = 0; i < cnt && i < 8; i++) {
        if (!nucleo_ble_scan_get(i, &dv)) break;
        snprintf(ln, sizeof(ln), "%-12.12s %4d  %02x:%02x:%02x", dv.name[0] ? dv.name : "(senza nome)",
                 dv.rssi, dv.addr[5], dv.addr[4], dv.addr[3]);
        txt(8, y, ln, FG, BG, 1); y += 12;
    }
    if (!cnt) txt(8, y, "Nessun dispositivo ancora.", DIM, BG, 1);
}

static void draw_spam(int top, int h)
{
    (void)top; (void)h;
    static const char *TN[] = { "iOS", "Android", "Windows", "Samsung", "Tutti" };
    int y0 = app_ui_title("Spam", WARN, "burst BLE adv");
    txt(8, y0 + 6, TN[s_target], FG, BG, 2);
    char ln[40];
    snprintf(ln, sizeof(ln), "%lu adv inviati", (unsigned long)nucleo_ble_spam_count());
    txt(8, y0 + 34, ln, GRN, BG, 2);
    txt(8, y0 + 58, "MAC casuale a ogni burst.", MUTED, BG, 1);
    txt(8, y0 + 72, "esc per fermare.", DIM, BG, 1);
}

static void draw_ibeacon(int top, int h)
{
    (void)top; (void)h;
    bool on = nucleo_ble_ibeacon_active();
    int y0 = app_ui_title("iBeacon", ACC, on ? "TX" : "off");
    txt(8, y0 + 6, on ? "In trasmissione" : "Fermo", on ? GRN : MUTED, BG, 2);
    txt(8, y0 + 34, "UUID NUCLEOOS-BLE  major 1 minor 1", MUTED, BG, 1);
    txt(8, y0 + 50, "esc per fermare.", DIM, BG, 1);
}

static void draw_hid(int top, int h)
{
    (void)top; (void)h;
    bool conn = nucleo_ble_hid_connected(), ready = nucleo_ble_hid_ready();
    int y0 = app_ui_title("Tastiera HID", ACC, ready ? "pronto" : conn ? "sub..." : "pairing");
    txt(8, y0 + 6, ready ? "Connesso, pronto" : conn ? "Connesso (subscribe...)" : "Attendo pairing...",
        ready ? GRN : (conn ? WARN : MUTED), BG, 2);
    txt(8, y0 + 32, "Appare come tastiera \"NucleoOS\".", MUTED, BG, 1);
    txt(8, y0 + 48, "ENTER: scrivi una riga demo.", FG, BG, 1);
    txt(8, y0 + 62, "1 play  2 next  3 prev", DIM, BG, 1);
    txt(8, y0 + 74, "4 vol+  5 vol-  6 mute", DIM, BG, 1);
    txt(8, y0 + 90, "esc per fermare.", DIM, BG, 1);
}

static void on_draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    switch (s_screen) {
        case MENU:    draw_menu(top, h); break;
        case SCAN:    draw_scan(top, h); break;
        case SPAM:    draw_spam(top, h); break;
        case IBEACON: draw_ibeacon(top, h); break;
        case HID:     draw_hid(top, h); break;
    }
}

extern "C" void nucleo_register_ble(void)
{
    static const nucleo_app_def_t app = {
        "ble", "BLE", "Security", "Scan / spam pairing / iBeacon (test autorizzati)",
        'B', 0x4DDF, on_enter, on_key, on_tick, on_draw, on_exit, NX_DEEP_OFFLINE
    };
    nucleo_app_register(&app);
}
