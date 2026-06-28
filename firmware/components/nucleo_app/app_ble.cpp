// app_ble.cpp — Security > BLE suite (scan / spam / iBeacon / HID keyboard). FOR AUTHORIZED TESTING ONLY.
//
// Declares exclusive_flags = NX_DEEP_OFFLINE: the framework tears Wi-Fi (+httpd/mDNS/voice/L1) down
// BEFORE on_enter and restores it all on close — "all RAM concentrated for BLE, back to normal on
// exit". on_enter brings the NimBLE controller up in that freed space; on_exit gives it back. A menu
// picks the active mode (only one runs at a time). Big fonts + number-key shortcuts per the native-UI
// rule. Back/Esc returns to the menu first, then closes the app.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_restart (apply the Bluetooth on/off preference)
extern "C" {
#include "nucleo_ble.h"
#include "nucleo_exclusive.h"
}

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x4DDF, GRN = 0x8FF3, WARN = 0xFE8C, HL = 0x4DDF, INK = 0x0000;

enum Screen { MENU, SCAN, SPAM, IBEACON, HID };
static Screen s_screen = MENU;
static int    s_sel = 0;          // menu cursor
static nucleo_ble_spam_t s_target = NUCLEO_BLE_SPAM_IOS;
static bool   s_up_ok = false;
static bool   s_bt_off = false;       // boot released the radio for RAM -> show the enable prompt

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
        if (ch >= '1' && ch <= '0' + N_ITEMS) { s_sel = ch - '1'; activate(s_sel); return; }
        if (key == NK_UP)    { s_sel = (s_sel - 1 + N_ITEMS) % N_ITEMS; nucleo_app_request_draw(); }
        if (key == NK_DOWN)  { s_sel = (s_sel + 1) % N_ITEMS; nucleo_app_request_draw(); }
        if (key == NK_ENTER) activate(s_sel);
    } else if (s_screen == HID) {
        if (key == NK_ENTER) { if (nucleo_ble_hid_ready()) nucleo_ble_hid_type("NucleoOS BLE HID\n"); }
        else if (ch >= '1' && ch <= '6') nucleo_ble_hid_media(ch - '1');  // 1 play/pause .. 6 mute
        nucleo_app_request_draw();
    }
}

static void on_tick(void) { if (s_screen != MENU) nucleo_app_request_draw(); }

// ---- drawing ---------------------------------------------------------------------------------------
static void draw_header(int top, const char *title, unsigned short col)
{
    d.setTextSize(2); d.setTextColor(col, BG); d.setCursor(8, top + 6); d.print(title);
    d.setTextSize(1);
}

static void draw_menu(int top, int h)
{
    draw_header(top, "BLE", ACC);
    if (s_bt_off) {   // Bluetooth disabled to reclaim RAM — offer to enable it
        d.setTextColor(WARN, BG); d.setCursor(8, top + 30); d.print("Bluetooth spento per liberare RAM");
        d.setTextColor(MUTED, BG); d.setCursor(8, top + 46); d.print("(~35 KB dati ad ANIMA e al sistema)");
        d.setTextColor(FG, BG); d.setCursor(8, top + 72); d.print("INVIO: attiva e riavvia");
        return;
    }
    d.setTextColor(WARN, BG); d.setCursor(70, top + 12); d.print("solo test autorizzati");
    if (!s_up_ok) { d.setTextColor(WARN, BG); d.setCursor(8, top + 30); d.print("controller BLE non avviato"); return; }

    int y = top + 30, rowh = 20;
    for (int i = 0; i < N_ITEMS; i++) {
        bool on = (i == s_sel);
        if (on) d.fillRoundRect(4, y - 2, 232, rowh - 2, 3, 0x12B2);
        d.setTextSize(1);
        d.setTextColor(on ? ACC : DIM, on ? 0x12B2 : BG); d.setCursor(8, y + 1);
        char num[3]; snprintf(num, sizeof(num), "%d", i + 1); d.print(num);
        d.setTextColor(on ? FG : MUTED, on ? 0x12B2 : BG); d.setCursor(22, y + 1); d.print(MENU_ITEMS[i].label);
        d.setTextColor(on ? MUTED : DIM, on ? 0x12B2 : BG); d.setCursor(120, y + 1); d.print(MENU_ITEMS[i].sub);
        y += rowh;
    }
}

static void draw_scan(int top, int h)
{
    draw_header(top, "Scansione", ACC);
    int cnt = nucleo_ble_scan_count();
    char ln[56];
    d.setTextColor(nucleo_ble_is_synced() ? GRN : MUTED, BG); d.setCursor(120, top + 12);
    d.print(nucleo_ble_is_synced() ? "attiva" : "avvio...");
    d.setTextColor(ACC, BG); snprintf(ln, sizeof(ln), "%d device", cnt); d.setCursor(8, top + 28); d.print(ln);

    int y = top + 44; nucleo_ble_dev_t dv;
    d.setTextColor(FG, BG);
    for (int i = 0; i < cnt && i < 9; i++) {
        if (!nucleo_ble_scan_get(i, &dv)) break;
        snprintf(ln, sizeof(ln), "%-12.12s %4d  %02x:%02x:%02x", dv.name[0] ? dv.name : "(senza nome)",
                 dv.rssi, dv.addr[5], dv.addr[4], dv.addr[3]);
        d.setCursor(8, y); d.print(ln); y += 12;
    }
}

static void draw_spam(int top, int h)
{
    static const char *TN[] = { "iOS", "Android", "Windows", "Samsung", "Tutti" };
    draw_header(top, "Spam", WARN);
    d.setTextColor(FG, BG); d.setTextSize(2);
    d.setCursor(8, top + 34); d.print(TN[s_target]);
    d.setTextSize(1);
    char ln[40];
    d.setTextColor(GRN, BG); snprintf(ln, sizeof(ln), "%lu adv inviati", (unsigned long)nucleo_ble_spam_count());
    d.setCursor(8, top + 64); d.print(ln);
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 80); d.print("MAC casuale a ogni burst");
    d.setCursor(8, top + 92); d.print("esc per fermare");
}

static void draw_ibeacon(int top, int h)
{
    draw_header(top, "iBeacon", ACC);
    d.setTextColor(nucleo_ble_ibeacon_active() ? GRN : MUTED, BG); d.setCursor(8, top + 34);
    d.print(nucleo_ble_ibeacon_active() ? "in trasmissione" : "fermo");
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 54); d.print("UUID NUCLEOOS-BLE  1/1");
    d.setCursor(8, top + 70); d.print("esc per fermare");
}

static void draw_hid(int top, int h)
{
    draw_header(top, "Tastiera HID", ACC);
    bool conn = nucleo_ble_hid_connected(), ready = nucleo_ble_hid_ready();
    d.setTextColor(ready ? GRN : (conn ? WARN : MUTED), BG); d.setCursor(8, top + 30);
    d.print(ready ? "connesso - pronto" : conn ? "connesso (subscribe...)" : "in attesa di pairing...");
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 46); d.print("appari come \"NucleoOS\" keyboard");
    d.setTextColor(FG, BG);   d.setCursor(8, top + 64); d.print("ENTER: scrivi riga demo");
    d.setCursor(8, top + 80); d.print("1 play  2 next  3 prev");
    d.setCursor(8, top + 92); d.print("4 vol+  5 vol-  6 mute");
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 108); d.print("esc per fermare");
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
