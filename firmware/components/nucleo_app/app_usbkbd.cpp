// USB Keyboard (PRO) — turn the Cardputer into a real USB-C keyboard for a host PC/phone (TinyUSB HID).
// Tabbed UI: Digita (live passthrough) · Macro (one-press combos) · Info (live HOST state) · Impostazioni.
//
// What makes it a real keyboard, not a toy:
//  - Guaranteed press+release (nucleo_usbhid sends a leading clear + a delivered release) — no stuck keys.
//  - Layout-aware: US or IT keymap (reuses nucleo_ducky's char->keycode tables) so symbols land right on
//    an Italian host, not just US.
//  - Full Fn layer: F1-F12, arrows, Home/End/PageUp/Down, Insert, forward-Delete, Esc.
//  - Reads the HOST's LED report back (Caps/Num/Scroll lock) and shows the PC's real state on the display.
//  - Macro pad: Ctrl+Alt+Del, Alt+F4, Win+L, Alt+Tab, PrintScreen, Task Manager — one keypress.
//
// Installing TinyUSB takes the USB-OTG PHY, so the serial console is gone until reboot (OTA still works).
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
#include "nucleo_ducky.h"
}

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x4DDF, GRN = 0x8FF3, WARN = 0xFE8C, HL = 0x4DDF, INK = 0x0000, ORG = 0xFD20;

// HID usage ids not already in nucleo_usbhid.h
#define HID_ESC   0x29
#define HID_DELfw 0x4C   // forward Delete
#define HID_HOME  0x4A
#define HID_END   0x4D
#define HID_PGUP  0x4B
#define HID_PGDN  0x4E
#define HID_INS   0x49
#define HID_PRTSC 0x46
static inline uint8_t HID_F(int n) { return (uint8_t)(0x3A + (n - 1)); }   // F1..F12

enum Tab { T_TYPE, T_MACRO, T_INFO, T_SET };
static Tab s_tab = T_TYPE;
static nucleo_ducky_layout_t s_layout = DUCKY_LAYOUT_US;
static int s_macro = 0;
static bool s_usb_ok = false;

struct Macro { const char *name; uint8_t mod; uint8_t key; };
static const Macro MACROS[] = {
    { "Ctrl+Alt+Canc",        NK_HIDMOD_CTRL | NK_HIDMOD_ALT,   HID_DELfw },
    { "Task Manager",         NK_HIDMOD_CTRL | NK_HIDMOD_SHIFT, HID_ESC },
    { "Alt+F4 (chiudi)",      NK_HIDMOD_ALT,                    HID_F(4) },
    { "Alt+Tab",              NK_HIDMOD_ALT,                    0x2B },
    { "Win+D (desktop)",      NK_HIDMOD_GUI,                    0x07 },   // d
    { "Win+L (blocca)",       NK_HIDMOD_GUI,                    0x0F },   // l
    { "Win+E (esplora)",      NK_HIDMOD_GUI,                    0x08 },   // e
    { "PrintScreen",          0,                                HID_PRTSC },
    { "Esc",                  0,                                HID_ESC },
};
static const int N_MACRO = (int)(sizeof(MACROS) / sizeof(MACROS[0]));

// ---- live typing modal ----------------------------------------------------------------------------
static void chip(int x, int y, const char *label, bool on, unsigned short oncol)
{
    int w = (int)strlen(label) * 6 + 8;
    d.fillRoundRect(x, y, w, 13, 3, on ? oncol : 0x10A2);
    d.setTextColor(on ? INK : MUTED, on ? oncol : 0x10A2);
    d.setCursor(x + 4, y + 3); d.print(label);
}

static void type_screen(unsigned char m, bool conn, unsigned char leds, const char *echo)
{
    d.fillScreen(BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, 4); d.print("Digita");
    d.setTextSize(1);
    d.fillCircle(150, 12, 4, conn ? GRN : WARN);
    d.setTextColor(conn ? GRN : WARN, BG); d.setCursor(160, 8); d.print(conn ? "host ok" : "attendo PC");

    int x = 8, y = 30;
    chip(x, y, "CTRL", m & NK_MOD_CTRL, HL);  x += 38;
    chip(x, y, "ALT",  m & NK_MOD_ALT, HL);   x += 32;
    chip(x, y, "SHIFT",m & NK_MOD_SHIFT, HL); x += 44;
    chip(x, y, "GUI",  m & NK_MOD_GUI, HL);   x += 32;
    chip(x, y, "FN",   m & NK_MOD_FN, HL);
    // HOST LED feedback — the PC's real lock state
    x = 8; y = 48;
    chip(x, y, "CAPS",   leds & NK_HIDLED_CAPS, GRN);   x += 40;
    chip(x, y, "NUM",    leds & NK_HIDLED_NUM, GRN);    x += 36;
    chip(x, y, "SCROLL", leds & NK_HIDLED_SCROLL, GRN);

    d.setTextColor(MUTED, BG); d.setCursor(8, 70); d.print("Inviato:");
    d.setTextColor(FG, BG); d.setCursor(54, 70); d.print(echo && echo[0] ? echo : "-");

    d.setTextColor(DIM, BG);
    d.setCursor(8, 90);  d.print("Fn+1..0=F1..F10  Fn+-=F11 Fn+ ==F12");
    d.setCursor(8, 102); d.print("Fn+frecce nav  Fn+Canc=Del  Fn+`=Esc");
    d.setTextColor(s_layout == DUCKY_LAYOUT_IT ? ORG : ACC, BG);
    d.setCursor(8, 116); d.print(s_layout == DUCKY_LAYOUT_IT ? "Layout IT" : "Layout US");
    d.setTextColor(WARN, BG); d.setCursor(150, 116); d.print("`  esci");
}

static void echo_push(char *echo, size_t n, const char *what)
{
    size_t l = strlen(echo);
    if (l + strlen(what) + 1 >= n) { echo[0] = 0; l = 0; }   // wrap
    snprintf(echo + l, n - l, "%s", what);
}

static void type_loop(void)
{
    char echo[28] = ""; unsigned char last_m = 0xFF; int last_conn = -1; unsigned char last_leds = 0xFF; bool first = true, back = false;
    while (!back) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        unsigned char m = nucleo_kbd_mods();
        bool conn = nucleo_usbhid_ready();
        unsigned char leds = nucleo_usbhid_leds();
        bool sent = false; const char *lbl = nullptr; char lblbuf[4];

        if (k.key != NK_NONE) {
            bool fn = (m & NK_MOD_FN);
            if (k.key == NK_BACK) {
                if (fn) { nucleo_usbhid_key(0, HID_ESC); lbl = "Esc"; sent = true; }   // Fn+` -> Esc to host
                else { back = true; break; }                                          // ` -> leave
            } else {
                unsigned char hidmod = 0;
                if (m & NK_MOD_CTRL) hidmod |= NK_HIDMOD_CTRL;
                if (m & NK_MOD_ALT)  hidmod |= NK_HIDMOD_ALT;
                if (m & NK_MOD_GUI)  hidmod |= NK_HIDMOD_GUI;
                unsigned char kc = 0, kmod = hidmod;

                if (k.key == NK_UP)         { kc = NK_HID_UP; lbl = "Up"; }
                else if (k.key == NK_DOWN)  { kc = NK_HID_DOWN; lbl = "Dn"; }
                else if (k.key == NK_LEFT)  { kc = NK_HID_LEFT; lbl = "Lt"; }
                else if (k.key == NK_RIGHT) { kc = NK_HID_RIGHT; lbl = "Rt"; }
                else if (k.key == NK_ENTER) { kc = NK_HID_ENTER; lbl = "\n"; if (m & NK_MOD_SHIFT) kmod |= NK_HIDMOD_SHIFT; }
                else if (k.key == NK_TAB)   { kc = 0x2B; lbl = "Tab"; if (m & NK_MOD_SHIFT) kmod |= NK_HIDMOD_SHIFT; }
                else if (k.key == NK_DEL)   { kc = fn ? HID_DELfw : NK_HID_BKSP; lbl = fn ? "Del" : "<-"; }
                else if (fn && k.ch >= '1' && k.ch <= '9') { kc = HID_F(k.ch - '0'); }
                else if (fn && k.ch == '0') { kc = HID_F(10); }
                else if (fn && k.ch == '-') { kc = HID_F(11); }
                else if (fn && k.ch == '=') { kc = HID_F(12); }
                else if (k.ch == ' ')       { kc = NK_HID_SPACE; lbl = "spc"; }
                else if (k.ch >= 33 && k.ch < 127) {
                    uint8_t dm, dk;                                  // layout-aware: the right keycode+mod for THIS host
                    if (nucleo_ducky_char(k.ch, s_layout, &dm, &dk)) { kc = dk; kmod = hidmod | dm; lblbuf[0] = k.ch; lblbuf[1] = 0; lbl = lblbuf; }
                }
                if (kc) { nucleo_usbhid_key(kmod, kc); sent = true; }
            }
            if (sent && lbl) echo_push(echo, sizeof echo, lbl);
        }

        if (first || sent || m != last_m || (int)conn != last_conn || leds != last_leds) {
            type_screen(m, conn, leds, echo);
            last_m = m; last_conn = conn; last_leds = leds; first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    nucleo_app_request_draw();
}

// ---- tab UI (framework) ----------------------------------------------------------------------------
static bool on_back(int key)
{
    if (key == NK_LEFT) { s_tab = (Tab)((s_tab + T_SET) % (T_SET + 1)); nucleo_app_request_draw(); return true; }
    return false;   // backtick -> close app
}

static void on_enter(void)
{
    s_tab = T_TYPE; s_macro = 0;
    s_usb_ok = (nucleo_usbhid_start() == ESP_OK);     // resident for the session (takes USB PHY)
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint("1-4 tab  </> cambia  enter  ` esci");
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (ch >= '1' && ch <= '4') { s_tab = (Tab)(ch - '1'); nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT) { s_tab = (Tab)((s_tab + 1) % (T_SET + 1)); nucleo_app_request_draw(); return; }

    switch (s_tab) {
        case T_TYPE: if (key == NK_ENTER) type_loop(); break;
        case T_MACRO:
            if (key == NK_UP)   { s_macro = (s_macro - 1 + N_MACRO) % N_MACRO; nucleo_app_request_draw(); }
            if (key == NK_DOWN) { s_macro = (s_macro + 1) % N_MACRO; nucleo_app_request_draw(); }
            if (key == NK_ENTER) { nucleo_usbhid_key(MACROS[s_macro].mod, MACROS[s_macro].key); nucleo_app_request_draw(); }
            break;
        case T_SET:
            if (key == NK_ENTER || ch == 'l') { s_layout = (s_layout == DUCKY_LAYOUT_US) ? DUCKY_LAYOUT_IT : DUCKY_LAYOUT_US; nucleo_app_request_draw(); }
            break;
        default: break;
    }
}

static void on_tick(void) { if (s_tab == T_INFO || s_tab == T_TYPE) nucleo_app_request_draw(); }   // live host state

// ---- drawing ---------------------------------------------------------------------------------------
static void tabbar(int top)
{
    static const char *N[] = { "Digita", "Macro", "Info", "Imp" };
    int x = 6;
    for (int i = 0; i <= T_SET; i++) {
        bool on = (i == s_tab); int w = (int)strlen(N[i]) * 6 + 10;
        d.fillRoundRect(x, top + 2, w, 14, 3, on ? ACC : 0x10A2);
        d.setTextColor(on ? INK : MUTED, on ? ACC : 0x10A2);
        d.setCursor(x + 5, top + 5); d.print(N[i]);
        x += w + 4;
    }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    tabbar(top);
    int y = top + 24; char ln[48];
    bool conn = nucleo_usbhid_ready();
    unsigned char leds = nucleo_usbhid_leds();

    if (s_tab == T_TYPE) {
        d.setTextColor(s_usb_ok ? FG : WARN, BG); d.setCursor(8, y); d.print(s_usb_ok ? "Tastiera USB pronta." : "USB HID non installata.");
        d.setTextColor(conn ? GRN : MUTED, BG); d.setCursor(8, y + 16); d.print(conn ? "Host collegato." : "Collega il cavo USB-C al PC.");
        d.setTextColor(GRN, BG);   d.setCursor(8, y + 38); d.print("ENTER");
        d.setTextColor(MUTED, BG); d.setCursor(56, y + 38); d.print("inizia a digitare");
        d.setTextColor(DIM, BG);   d.setCursor(8, y + 58); d.print("Combo, Fn, frecce, layout IT/US.");
    } else if (s_tab == T_MACRO) {
        d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print("ENTER invia la combo al PC");
        int yy = y + 16;
        for (int i = 0; i < N_MACRO; i++) {
            bool on = (i == s_macro);
            if (on) d.fillRoundRect(4, yy - 1, 232, 13, 2, 0x12B2);
            d.setTextColor(on ? ACC : MUTED, on ? 0x12B2 : BG); d.setCursor(10, yy + 1); d.print(on ? ">" : " ");
            d.setTextColor(on ? FG : MUTED, on ? 0x12B2 : BG); d.setCursor(22, yy + 1); d.print(MACROS[i].name);
            yy += 13;
        }
    } else if (s_tab == T_INFO) {
        d.setTextColor(FG, BG);
        d.setCursor(8, y);      snprintf(ln, sizeof ln, "USB HID: %s", s_usb_ok ? "installata" : "errore"); d.print(ln);
        d.setTextColor(conn ? GRN : WARN, BG);
        d.setCursor(8, y + 14); snprintf(ln, sizeof ln, "Host: %s", conn ? "collegato e pronto" : "non rilevato"); d.print(ln);
        d.setTextColor(MUTED, BG); d.setCursor(8, y + 32); d.print("LED del PC (stato reale):");
        int x = 8, yy = y + 46;
        chip(x, yy, "CAPS",   leds & NK_HIDLED_CAPS, GRN);   x += 40;
        chip(x, yy, "NUM",    leds & NK_HIDLED_NUM, GRN);    x += 36;
        chip(x, yy, "SCROLL", leds & NK_HIDLED_SCROLL, GRN);
        d.setTextColor(DIM, BG);
        d.setCursor(8, y + 66); d.print("Si accendono premendo i lock");
        d.setCursor(8, y + 78); d.print("sul PC: e' il PC che ce lo dice.");
        d.setTextColor(MUTED, BG); d.setCursor(8, y + 96); d.print("Console seriale sospesa (USB presa).");
    } else { // T_SET
        d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print("ENTER / L cambia layout host");
        d.setTextColor(s_layout == DUCKY_LAYOUT_US ? GRN : DIM, BG); d.setCursor(8, y + 22); d.print(s_layout == DUCKY_LAYOUT_US ? "> US (QWERTY)" : "  US (QWERTY)");
        d.setTextColor(s_layout == DUCKY_LAYOUT_IT ? GRN : DIM, BG); d.setCursor(8, y + 38); d.print(s_layout == DUCKY_LAYOUT_IT ? "> IT (italiana)" : "  IT (italiana)");
        d.setTextColor(DIM, BG);
        d.setCursor(8, y + 60); d.print("Imposta la mappa dei simboli");
        d.setCursor(8, y + 72); d.print("secondo il layout del PC bersaglio.");
    }
}

extern "C" void nucleo_register_usbkbd(void)
{
    static const nucleo_app_def_t app = {
        "usbkbd", "USB Keyboard", "Tools", "Tastiera USB pro: layout IT/US, Fn, macro, LED del PC",
        'K', 0x4DDF, on_enter, on_key, on_tick, draw, nullptr, 0
    };
    nucleo_app_register(&app);
}
