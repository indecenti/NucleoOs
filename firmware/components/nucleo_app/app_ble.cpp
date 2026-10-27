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
#include "nucleo_i18n.h"   // TR(it, en) — bilingual hint strings
}
#include "app_large.h"     // two screens: the BLE suite laid out on the 320x240 panel
#include "nucleo_touch.h"

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short ACC = C_BLUE, GRN = C_GREEN, WARN = C_YELLOW, SURF = 0x10A2, CAP = 0x1A8B;

enum Screen { MENU, SCAN, SPAM, IBEACON, HID };
static Screen s_screen = MENU;
static int    s_sel = 0;          // menu cursor
static nucleo_ble_spam_t s_target = NUCLEO_BLE_SPAM_IOS;
static bool   s_up_ok = false;
static int    s_lscroll = 0;                 // menu list scroll (two screens)
static int    s_scan_sel = 0, s_scan_scroll = 0;   // scan device-list selection + scroll

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

struct Item { const char *label; const char *label_en; const char *sub; const char *sub_en; };
static const Item MENU_ITEMS[] = {
    { "Scansione",    "Scan",         "dispositivi BLE vicini", "nearby BLE devices" },
    { "Spam iOS",     "Spam iOS",     "popup pairing Apple",    "Apple pairing popup" },
    { "Spam Android", "Spam Android", "popup Fast Pair",        "Fast Pair popup" },
    { "Spam Windows", "Spam Windows", "popup Swift Pair",       "Swift Pair popup" },
    { "Spam Samsung", "Spam Samsung", "popup Galaxy Buds",      "Galaxy Buds popup" },
    { "Spam Tutti",   "Spam All",     "ruota tutti gli OS",     "rotate every OS" },
    { "iBeacon",      "iBeacon",      "beacon standard",        "standard beacon" },
    { "Tastiera HID", "HID Keyboard", "keyboard + media",       "keyboard + media" },
};
static const int N_ITEMS = (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
static const char *ble_label(int i, void *) { return TR(MENU_ITEMS[i].label, MENU_ITEMS[i].label_en); }

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
    // Seamless: this app is NX_SOLO|NX_DEEP_OFFLINE|NX_BLE, so opening it reboots ONCE
    // into a Solo boot with Wi-Fi skipped and Bluetooth kept — the radio is present and
    // comes straight up. No "enable Bluetooth + reboot + reopen" dance. Esc -> normal OS.
    s_screen = MENU; s_sel = 0;
    s_up_ok  = nucleo_ble_radio_present() && nucleo_ble_up();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("1-8 scegli  enter avvia  esc indietro", "1-8 pick  enter start  esc back", "1-8 elige  intro lanza  esc atr", "1-8 choix  enter lanc  esc ret", "1-8 waehl  eingabe start  esc zu"));
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    if (!s_up_ok) return;     // radio never came up -> nothing to tear down
    stop_all();
    nucleo_ble_down();        // RAM back to the heap; on Esc the framework reboots to the OS.
}

static void on_key(int key, char ch)
{
    if (s_screen == MENU) {
        if (int t = lg::row_tap(key, ch, s_lscroll, N_ITEMS, &s_sel)) {   // panel: tap a row to pick, again to open
            if (t == 2) activate(s_sel); else nucleo_app_request_draw(); return;
        }
        if (ch >= '1' && ch <= '0' + N_ITEMS) { s_sel = ch - '1'; activate(s_sel); return; }   // number = instant launch
        if (app_ui_list_key(key, ch, &s_sel, N_ITEMS, ble_label, nullptr)) { nucleo_app_request_draw(); return; }
        if (key == NK_ENTER) activate(s_sel);
    } else if (s_screen == SCAN) {                                        // panel: walk the device list
        int n = nucleo_ble_scan_count();
        if (n > 0) {
            if (nucleo_touch_is_row(key, ch)) { int i = s_scan_scroll + (ch - NUCLEO_TOUCH_ROW); if (i < n) s_scan_sel = i; }
            else if (key == NK_UP)   s_scan_sel = (s_scan_sel + n - 1) % n;
            else if (key == NK_DOWN) s_scan_sel = (s_scan_sel + 1) % n;
            else return;
            nucleo_app_request_draw();
        }
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
    int y0 = app_ui_title("BLE", ACC, "solo test autorizzati");
    if (!s_up_ok) {   // rare after the BLE-Solo Wi-Fi-skip fix
        txt(8, y0 + 14, TR("Bluetooth non attivo.","Bluetooth is off."), FG, BG, 1);
        txt(8, y0 + 30, "Riprova ad aprire l'app.", MUTED, BG, 1);
        return;
    }
    const int foot = 16;
    app_ui_list(y0, h - y0 - foot, N_ITEMS, s_sel, ble_label, nullptr, nullptr, nullptr);
    // focused-item description on a footer strip (launcher idiom: the instruction line)
    d.fillRoundRect(6, h - foot, W - 12, foot - 2, 5, SURF);
    txt(12, h - foot + 3, TR(MENU_ITEMS[s_sel].sub, MENU_ITEMS[s_sel].sub_en), MUTED, SURF, 1);
}

static void draw_scan(int top, int h)
{
    (void)top; (void)h;
    char cb[16]; int cnt = nucleo_ble_scan_count();
    snprintf(cb, sizeof cb, "%d device", cnt);
    int y0 = app_ui_title(TR("Scansione","Scan"), ACC, nucleo_ble_is_synced() ? TR("attiva","on") : TR("avvio...","starting..."));
    txt(8, y0 + 4, cb, ACC, BG, 2);
    int y = y0 + 26; nucleo_ble_dev_t dv; char ln[56];
    for (int i = 0; i < cnt && i < 8; i++) {
        if (!nucleo_ble_scan_get(i, &dv)) break;
        snprintf(ln, sizeof(ln), "%-12.12s %4d  %02x:%02x:%02x", dv.name[0] ? dv.name : TR("(senza nome)","(no name)"),
                 dv.rssi, dv.addr[5], dv.addr[4], dv.addr[3]);
        txt(8, y, ln, FG, BG, 1); y += 12;
    }
    if (!cnt) txt(8, y, TR("Nessun dispositivo ancora.","No devices yet."), DIM, BG, 1);
}

static void draw_spam(int top, int h)
{
    (void)top; (void)h;
    static const char *TN_IT[] = { "iOS", "Android", "Windows", "Samsung", "Tutti" };
    static const char *TN_EN[] = { "iOS", "Android", "Windows", "Samsung", "All" };
    const char *const *TN = nucleo_i18n_is_it() ? TN_IT : TN_EN;
    int y0 = app_ui_title("Spam", WARN, "burst BLE adv");
    txt(8, y0 + 6, TN[s_target], FG, BG, 2);
    char ln[40];
    snprintf(ln, sizeof(ln), "%lu adv inviati", (unsigned long)nucleo_ble_spam_count());
    txt(8, y0 + 34, ln, GRN, BG, 2);
    txt(8, y0 + 58, "MAC casuale a ogni burst.", MUTED, BG, 1);
    txt(8, y0 + 72, TR("esc per fermare.","esc to stop."), DIM, BG, 1);
}

static void draw_ibeacon(int top, int h)
{
    (void)top; (void)h;
    bool on = nucleo_ble_ibeacon_active();
    int y0 = app_ui_title("iBeacon", ACC, on ? "TX" : "off");
    txt(8, y0 + 6, on ? "In trasmissione" : "Fermo", on ? GRN : MUTED, BG, 2);
    txt(8, y0 + 34, "UUID NUCLEOOS-BLE  major 1 minor 1", MUTED, BG, 1);
    txt(8, y0 + 50, TR("esc per fermare.","esc to stop."), DIM, BG, 1);
}

static void draw_hid(int top, int h)
{
    (void)top; (void)h;
    bool conn = nucleo_ble_hid_connected(), ready = nucleo_ble_hid_ready();
    int y0 = app_ui_title(TR("Tastiera HID","HID Keyboard"), ACC, ready ? TR("pronto","ready") : conn ? "sub..." : "pairing");
    txt(8, y0 + 6, ready ? TR("Connesso, pronto","Connected, ready") : conn ? TR("Connesso (subscribe...)","Connected (subscribe...)") : TR("Attendo pairing...","Waiting for pairing..."),
        ready ? GRN : (conn ? WARN : MUTED), BG, 2);
    txt(8, y0 + 32, "Appare come tastiera \"NucleoOS\".", MUTED, BG, 1);
    txt(8, y0 + 48, TR("ENTER: scrivi una riga demo.","ENTER: type a demo line."), FG, BG, 1);
    txt(8, y0 + 62, "1 play  2 next  3 prev", DIM, BG, 1);
    txt(8, y0 + 74, "4 vol+  5 vol-  6 mute", DIM, BG, 1);
    txt(8, y0 + 90, TR("esc per fermare.","esc to stop."), DIM, BG, 1);
}

// ===== two screens ==============================================================================================
// The panel carries the whole BLE suite: the mode menu (list + description card), the live scan device list, and
// a big status card per running mode. Touch is the content: tap a menu row to pick, again to launch; the header
// chevron steps back to the menu; in Scan a drag walks the device list. The Cardputer becomes the companion:
// which mode is picked or running, the headline counter, and the key hints.
static const char *ble_sub(int i, void *) { return TR(MENU_ITEMS[i].sub, MENU_ITEMS[i].sub_en); }

static void lg_menu_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *) {
    unsigned short col = focus ? INK : (i >= 1 && i <= 5 ? WARN : FG);   // the spam rows warm-tinted
    lg::row_text(x, y, w, h, focus, bg, TR(MENU_ITEMS[i].label, MENU_ITEMS[i].label_en), nullptr, col);
}
static void lg_dev_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *) {
    nucleo_ble_dev_t dv;
    if (!nucleo_ble_scan_get(i, &dv)) { lg::row_text(x, y, w, h, focus, bg, "-", nullptr, MUTED); return; }
    char right[24]; snprintf(right, sizeof right, "%ddB  %02x:%02x", dv.rssi, dv.addr[2], dv.addr[1]);
    lg::row_text(x, y, w, h, focus, bg, dv.name[0] ? dv.name : TR("(senza nome)", "(no name)"), right, focus ? INK : FG);
}

static const char *const SPAM_TN[] = { "iOS", "Android", "Windows", "Samsung", TR("Tutti", "All") };

static void draw_large(void) {
    const int Wp = d.width(), Hp = d.height();
    nucleo_touch_clear();
    if (!s_up_ok) {
        lg::header("BLE", nullptr, ACC, NK_BACK);
        d.fillRect(0, lg::HEAD, Wp, Hp - lg::HEAD, BG);
        lg::message(12, lg::HEAD + 10, Wp - 24, Hp - lg::HEAD - 20, TR("Bluetooth non attivo", "Bluetooth not active"),
                    TR("Riapri l'app per riprovare.", "Reopen the app to retry."), WARN);
        nucleo_touch_commit(); return;
    }
    if (s_screen == MENU) {
        int y = lg::header("BLE", TR("test autorizzati", "authorized"), ACC);
        const int LW = 168;
        lg::list(0, y + 2, LW, Hp - y - 6, lg::ROW, N_ITEMS, s_sel, &s_lscroll, lg_menu_row, nullptr, ACC);
        d.fillRect(LW, y, Wp - LW, Hp - y, BG);
        const int dx = LW + 4, dw = Wp - LW - 10;
        lg::panel(dx, y + 6, dw, Hp - y - 14);
        const unsigned short cb = lg::card();
        lg::font(2); d.setTextColor(s_sel >= 1 && s_sel <= 5 ? WARN : ACC, cb);
        char t[24]; lg::fit(t, sizeof t, TR(MENU_ITEMS[s_sel].label, MENU_ITEMS[s_sel].label_en), dw - 20);
        d.drawString(t, dx + 10, y + 16);
        d.drawFastHLine(dx + 10, y + 44, dw - 20, LINE);
        lg::font(0); d.setTextColor(FG, cb);
        lg::wrap(TR(MENU_ITEMS[s_sel].sub, MENU_ITEMS[s_sel].sub_en), dx + 10, y + 54, dw - 20, 18, 4);
        lg::button(dx + 10, Hp - 48, dw - 20, 34, TR("Avvia", "Start"), ACC, INK, NK_ENTER, 0);
        nucleo_touch_commit(); return;
    }
    if (s_screen == SCAN) {
        int cnt = nucleo_ble_scan_count();
        char rt[16]; snprintf(rt, sizeof rt, TR("%d device", "%d devices"), cnt);
        int y = lg::header(TR("Scansione", "Scan"), rt, ACC, NK_BACK);
        d.fillRect(0, y, Wp, Hp - y, BG);
        if (cnt <= 0) lg::message(0, y + 10, Wp, Hp - y - 20, TR("In ascolto...", "Listening..."), TR("Nessun dispositivo ancora.", "No devices yet."), DIM);
        else lg::list(0, y + 2, Wp, Hp - y - 6, lg::ROW, cnt, s_scan_sel, &s_scan_scroll, lg_dev_row, nullptr, ACC);
        nucleo_touch_commit(); return;
    }
    if (s_screen == SPAM) {
        int t = (int)s_target; if (t < 0 || t > 4) t = 0;
        char ht[24]; snprintf(ht, sizeof ht, "Spam %s", SPAM_TN[t]);
        int y = lg::header(ht, "TX", WARN, NK_BACK);
        d.fillRect(0, y, Wp, Hp - y, BG);
        lg::panel(16, y + 10, Wp - 32, 120);
        const unsigned short cb = lg::card();
        char n[16]; snprintf(n, sizeof n, "%lu", (unsigned long)nucleo_ble_spam_count());
        lg::font(3); d.setTextColor(C_GREEN, cb); d.setTextDatum(middle_center);
        d.drawString(n, Wp / 2, y + 54); d.setTextDatum(top_left);
        lg::font(1); d.setTextColor(MUTED, cb); d.setTextDatum(top_center);
        d.drawString(TR("adv inviati", "adv sent"), Wp / 2, y + 86); d.setTextDatum(top_left);
        lg::font(0); d.setTextColor(DIM, cb);
        d.drawString(TR("MAC casuale a ogni burst", "random MAC each burst"), 28, y + 110);
        lg::button(40, Hp - 54, Wp - 80, 40, TR("Ferma", "Stop"), C_RED, INK, NK_BACK, 0);
        nucleo_touch_commit(); return;
    }
    if (s_screen == IBEACON) {
        bool on = nucleo_ble_ibeacon_active();
        int y = lg::header("iBeacon", on ? "TX" : "off", ACC, NK_BACK);
        d.fillRect(0, y, Wp, Hp - y, BG);
        lg::panel(16, y + 10, Wp - 32, Hp - y - 20);
        const unsigned short cb = lg::card();
        lg::font(2); d.setTextColor(on ? C_GREEN : MUTED, cb);
        d.drawString(on ? TR("In trasmissione", "Broadcasting") : TR("Fermo", "Stopped"), 28, y + 24);
        d.drawFastHLine(28, y + 54, Wp - 56, LINE);
        int ky = y + 64;
        lg::kv(28, ky, Wp - 56, "UUID", "NUCLEOOS-BLE", FG, cb); ky += 26;
        lg::kv(28, ky, Wp - 56, "Major", "1", FG, cb); ky += 26;
        lg::kv(28, ky, Wp - 56, "Minor", "1", FG, cb);
        lg::button(40, Hp - 54, Wp - 80, 40, TR("Ferma", "Stop"), C_RED, INK, NK_BACK, 0);
        nucleo_touch_commit(); return;
    }
    // HID
    bool conn = nucleo_ble_hid_connected(), ready = nucleo_ble_hid_ready();
    int y = lg::header(TR("Tastiera HID", "HID Keyboard"), ready ? TR("pronto", "ready") : conn ? "sub..." : "pairing",
                       ready ? C_GREEN : (conn ? WARN : ACC), NK_BACK);
    d.fillRect(0, y, Wp, Hp - y, BG);
    lg::panel(16, y + 8, Wp - 32, 58);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(ready ? C_GREEN : (conn ? WARN : MUTED), cb);
    d.drawString(ready ? TR("Connesso, pronto", "Connected, ready") : conn ? TR("Connesso (subscribe...)", "Connected (subscribe...)") : TR("Attendo pairing...", "Waiting for pairing..."), 28, y + 20);
    lg::font(0); d.setTextColor(MUTED, cb);
    d.drawString(TR("Appare come tastiera 'NucleoOS'", "Shows up as 'NucleoOS' keyboard"), 28, y + 44);
    lg::button(16, y + 74, Wp - 32, 34, TR("Scrivi riga demo (INVIO)", "Type demo line (ENTER)"), ACC, INK, NK_ENTER, 0);
    static const char *MED[6] = { "play", "next", "prev", "vol+", "vol-", "mute" };
    const int bw = (Wp - 32 - 2 * 8) / 3, bh = 34;
    for (int i = 0; i < 6; i++) {
        int bx = 16 + (i % 3) * (bw + 8), by = y + 116 + (i / 3) * (bh + 8);
        lg::button(bx, by, bw, bh, MED[i], lg::card(), FG, NK_CHAR, (char)('1' + i));
    }
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("BLE", ACC);
    if (!s_up_ok) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(WARN, BG); d.drawString(TR("Bluetooth non attivo", "Bluetooth off"), 8, y + 8);
        d.setFont(&fonts::Font0); return;
    }
    if (s_screen == MENU) {
        d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(s_sel >= 1 && s_sel <= 5 ? WARN : ACC, BG);
        d.drawString(TR(MENU_ITEMS[s_sel].label, MENU_ITEMS[s_sel].label_en), 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        lg::wrap(TR(MENU_ITEMS[s_sel].sub, MENU_ITEMS[s_sel].sub_en), 8, y + 34, 224, 12, 2);
        lg::comp_keys(TR("1-8 scegli  INVIO avvia  ESC esci", "1-8 pick  ENTER start  ESC back"));
        return;
    }
    if (s_screen == SCAN) {
        int cnt = nucleo_ble_scan_count();
        d.setFont(&fonts::FreeSansBold18pt7b); d.setTextColor(ACC, BG);
        char n[8]; snprintf(n, sizeof n, "%d", cnt); d.drawString(n, 8, y + 2);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(TR("dispositivi", "devices"), 58, y + 12);
        nucleo_ble_dev_t dv;
        if (cnt > 0 && nucleo_ble_scan_get(s_scan_sel, &dv)) {
            char ln[40]; snprintf(ln, sizeof ln, "> %s  %ddB", dv.name[0] ? dv.name : TR("(senza nome)", "(no name)"), dv.rssi);
            d.setTextColor(FG, BG); d.drawString(ln, 8, y + 44);
        }
        lg::comp_keys(TR(";/. scorri  ESC lista", ";/. scroll  ESC menu"));
        return;
    }
    const char *name = s_screen == SPAM ? "Spam" : s_screen == IBEACON ? "iBeacon" : "HID";
    unsigned short col = s_screen == SPAM ? WARN : (s_screen == HID && nucleo_ble_hid_ready() ? C_GREEN : ACC);
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(col, BG); d.drawString(name, 8, y + 4);
    d.setFont(&fonts::Font0); d.setTextColor(FG, BG);
    if (s_screen == SPAM) {
        int t = (int)s_target; if (t < 0 || t > 4) t = 0;
        char ln[40]; snprintf(ln, sizeof ln, "%s - %lu adv", SPAM_TN[t], (unsigned long)nucleo_ble_spam_count());
        d.drawString(ln, 8, y + 34);
    } else if (s_screen == IBEACON) {
        d.drawString(nucleo_ble_ibeacon_active() ? TR("in trasmissione", "broadcasting") : TR("fermo", "stopped"), 8, y + 34);
    } else {
        d.drawString(nucleo_ble_hid_ready() ? TR("pronto a scrivere", "ready to type") : TR("in pairing...", "pairing..."), 8, y + 34);
    }
    lg::comp_keys(TR("ESC: ferma e torna al menu", "ESC: stop and back to menu"));
}

static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
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
        'B', 0x4DDF, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_SOLO | NX_DEEP_OFFLINE | NX_BLE, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
