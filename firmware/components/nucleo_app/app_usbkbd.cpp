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
#include "launcher_theme.h"   // W/H + themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "nucleo_i18n.h"      // TR(it,en): hint follows the system language
#include "app_gfx.h"
#include "app_ui.h"           // shared focused-list widget (macro list) + quick-select nav
#include "app_large.h"        // two screens: the tabs + macro picker on the panel, Cardputer as companion
#include "nucleo_touch.h"
#include "nucleo_exclusive.h" // declarative exclusive mode (NX_NET_APP)
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

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short ACC = C_BLUE, GRN = C_GREEN, WARN = C_YELLOW, HL = C_BLUE,
                            ORG = 0xFD20;   // off-state fill -> LINE, selected capsule -> ACC (both themed)

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
// Last host state painted (see on_tick): repaint only when the link or the PC lock LEDs change.
static unsigned char s_last_leds = 0xFF;
static int  s_last_conn = -1;
static nucleo_ducky_layout_t s_layout = DUCKY_LAYOUT_US;
static int s_macro = 0;
static bool s_usb_ok = false;
static int s_lg_scroll = 0;   // the panel macro list's first visible row (two screens)

struct Macro { const char *name; const char *name_en; uint8_t mod; uint8_t key; };
static const Macro MACROS[] = {
    { "Ctrl+Alt+Canc",        "Ctrl+Alt+Del",       NK_HIDMOD_CTRL | NK_HIDMOD_ALT,   HID_DELfw },
    { "Task Manager",         "Task Manager",       NK_HIDMOD_CTRL | NK_HIDMOD_SHIFT, HID_ESC },
    { "Alt+F4 (chiudi)",      "Alt+F4 (close)",     NK_HIDMOD_ALT,                    HID_F(4) },
    { "Alt+Tab",              "Alt+Tab",            NK_HIDMOD_ALT,                    0x2B },
    { "Win+D (desktop)",      "Win+D (desktop)",    NK_HIDMOD_GUI,                    0x07 },   // d
    { "Win+L (blocca)",       "Win+L (lock)",       NK_HIDMOD_GUI,                    0x0F },   // l
    { "Win+E (esplora)",      "Win+E (explorer)",   NK_HIDMOD_GUI,                    0x08 },   // e
    { "PrintScreen",          "PrintScreen",        0,                                HID_PRTSC },
    { "Esc",                  "Esc",                0,                                HID_ESC },
};
static const int N_MACRO = (int)(sizeof(MACROS) / sizeof(MACROS[0]));
static const char *mac_label(int i, void *) { return TR(MACROS[i].name, MACROS[i].name_en); }   // shared-list provider

// ---- live typing modal ----------------------------------------------------------------------------
static void chip(int x, int y, const char *label, bool on, unsigned short oncol)
{
    int w = (int)strlen(label) * 6 + 8;
    d.fillRoundRect(x, y, w, 13, 3, on ? oncol : LINE);
    d.setTextColor(on ? INK : MUTED, on ? oncol : LINE);
    d.setCursor(x + 4, y + 3); d.print(label);
}

static void type_screen(unsigned char m, bool conn, unsigned char leds, const char *echo)
{
    d.fillScreen(BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, 4); d.print(TR("Digita","Type"));
    d.setTextSize(1);
    d.fillCircle(150, 12, 4, conn ? GRN : WARN);
    d.setTextColor(conn ? GRN : WARN, BG); d.setCursor(160, 8); d.print(conn ? TR("host ok","host ok") : TR("attendo PC","waiting PC"));

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

    d.setTextColor(MUTED, BG); d.setCursor(8, 70); d.print(TR("Inviato:","Sent:"));
    d.setTextColor(FG, BG); d.setCursor(54, 70); d.print(echo && echo[0] ? echo : "-");

    d.setTextColor(DIM, BG);
    d.setCursor(8, 90);  d.print(TR("Fn+1..0=F1..F10  Fn+-=F11 Fn+ ==F12","Fn+1..0=F1..F10  Fn+-=F11 Fn+ ==F12"));
    d.setCursor(8, 102); d.print(TR("Fn+frecce nav  Fn+Canc=Del  Fn+`=Esc","Fn+arrows nav  Fn+Canc=Del  Fn+`=Esc"));
    d.setTextColor(s_layout == DUCKY_LAYOUT_IT ? ORG : ACC, BG);
    d.setCursor(8, 116); d.print(s_layout == DUCKY_LAYOUT_IT ? "Layout IT" : "Layout US");
    d.setTextColor(WARN, BG); d.setCursor(150, 116); d.print(TR("`  esci","`  exit"));
}

static void echo_push(char *echo, size_t n, const char *what)
{
    size_t l = strlen(echo);
    if (l + strlen(what) + 1 >= n) { echo[0] = 0; l = 0; }   // wrap
    snprintf(echo + l, n - l, "%s", what);
}

// Composite the modal frame into the shared back-buffer and blit once — never a direct-to-panel
// fillScreen on the redraw cadence, so the live typing view can't flicker (ANTI-FLICKER.md #1).
static void type_paint(unsigned char m, bool conn, unsigned char leds, const char *echo)
{
    M5Canvas *cv = nucleo_screen();
    if (!cv) { type_screen(m, conn, leds, echo); return; }   // OOM fallback: direct (rare)
    nucleo_app_set_gfx(cv);
    type_screen(m, conn, leds, echo);                        // draws via `d` -> the canvas
    nucleo_app_set_gfx(nullptr);
    cv->pushSprite(0, 0);
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
            type_paint(m, conn, leds, echo);
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
    s_last_conn = -1; s_last_leds = 0xFF;             // stale -> the first tick paints the real host state
    s_usb_ok = (nucleo_usbhid_start() == ESP_OK);     // resident for the session (takes USB PHY)
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("1-4 tab   </> cambia   invio   esc esci", "1-4 tab   </> switch   enter   esc back", "1-4 tab   </> camb.   intro   esc atr", "1-4 tab   </> chang.  entree  esc ret", "1-4 tab   </> wechs.  enter   esc zrck"));
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (ch >= '1' && ch <= '4') { s_tab = (Tab)(ch - '1'); nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT) { s_tab = (Tab)((s_tab + 1) % (T_SET + 1)); nucleo_app_request_draw(); return; }

    switch (s_tab) {
        case T_TYPE: if (key == NK_ENTER) type_loop(); break;
        case T_MACRO:
            if (int t = lg::row_tap(key, ch, s_lg_scroll, N_MACRO, &s_macro)) {   // the panel's macro rows
                if (t == 2) nucleo_usbhid_key(MACROS[s_macro].mod, MACROS[s_macro].key);
                nucleo_app_request_draw();
            }
            else if (app_ui_list_key(key, ch, &s_macro, N_MACRO, mac_label, nullptr)) { nucleo_app_request_draw(); }
            else if (key == NK_ENTER) { nucleo_usbhid_key(MACROS[s_macro].mod, MACROS[s_macro].key); nucleo_app_request_draw(); }
            break;
        case T_SET:
            if (ch == 'u') { s_layout = DUCKY_LAYOUT_US; nucleo_app_request_draw(); }           // the panel's US button
            else if (ch == 'i') { s_layout = DUCKY_LAYOUT_IT; nucleo_app_request_draw(); }       // ...and IT button
            else if (key == NK_ENTER || ch == 'l') { s_layout = (s_layout == DUCKY_LAYOUT_US) ? DUCKY_LAYOUT_IT : DUCKY_LAYOUT_US; nucleo_app_request_draw(); }
            break;
        default: break;
    }
}

// Live host state (link up + the PC's real lock LEDs). Repaint ONLY when one of those actually
// changed: the old unconditional request_draw recomposited + blitted the whole frame 5x/s forever,
// which is the cadence repaint ANTI-FLICKER.md forbids (and it burned battery while idle).
static void on_tick(void)
{
    if (s_tab != T_INFO && s_tab != T_TYPE) return;
    int conn = nucleo_usbhid_ready() ? 1 : 0;
    unsigned char leds = nucleo_usbhid_leds();
    if (conn == s_last_conn && leds == s_last_leds) return;
    s_last_conn = conn; s_last_leds = leds;
    nucleo_app_request_draw();
}

// ---- drawing ---------------------------------------------------------------------------------------
static void tabbar(int top)
{
    static const char *IT[] = { "Digita", "Macro", "Info", "Imp" };
    static const char *EN[] = { "Type", "Macro", "Info", "Set" };
    const char *const *N = nucleo_i18n_is_it() ? IT : EN;
    int x = 6;
    for (int i = 0; i <= T_SET; i++) {
        bool on = (i == s_tab); int w = (int)strlen(N[i]) * 6 + 10;
        d.fillRoundRect(x, top + 2, w, 14, 3, on ? ACC : LINE);
        d.setTextColor(on ? INK : MUTED, on ? ACC : LINE);
        d.setCursor(x + 5, top + 5); d.print(N[i]);
        x += w + 4;
    }
    d.drawFastHLine(0, top + 20, 240, LINE);   // launcher-consistent hairline anchor under the tabs
}

static void draw_companion(void);
static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    tabbar(top);
    int y = top + 24; char ln[48];
    bool conn = nucleo_usbhid_ready();
    unsigned char leds = nucleo_usbhid_leds();

    if (s_tab == T_TYPE) {
        d.setTextColor(s_usb_ok ? FG : WARN, BG); d.setCursor(8, y); d.print(s_usb_ok ? TR("Tastiera USB pronta.","USB keyboard ready.") : TR("USB HID non installata.","USB HID not installed."));
        d.setTextColor(conn ? GRN : MUTED, BG); d.setCursor(8, y + 14); d.print(conn ? TR("Host collegato.","Host connected.") : TR("Collega il cavo USB-C al PC.","Plug the USB-C cable into the PC."));
        // big call-to-action
        d.fillRoundRect(6, y + 34, 228, 26, 8, ACC);
        d.setTextSize(2); d.setTextColor(GRN, ACC); d.setCursor(14, y + 39); d.print("ENTER");
        d.setTextSize(1); d.setTextColor(FG, ACC);  d.setCursor(80, y + 45); d.print(TR("inizia a digitare","start typing"));
        d.setTextColor(DIM, BG); d.setCursor(8, y + 70); d.print(TR("Combo, Fn, frecce, layout IT/US.","Combos, Fn, arrows, IT/US layout."));
    } else if (s_tab == T_MACRO) {
        d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print(TR("ENTER invia la combo al PC","ENTER sends the combo to the PC"));
        app_ui_list(y + 14, h - (y + 14), N_MACRO, s_macro, mac_label, nullptr, nullptr, nullptr);
    } else if (s_tab == T_INFO) {
        d.setTextColor(FG, BG);
        d.setCursor(8, y);      snprintf(ln, sizeof ln, "USB HID: %s", s_usb_ok ? TR("installata","installed") : TR("errore","error")); d.print(ln);
        d.setTextColor(conn ? GRN : WARN, BG);
        d.setCursor(8, y + 14); snprintf(ln, sizeof ln, "Host: %s", conn ? TR("collegato e pronto","connected and ready") : TR("non rilevato","not detected")); d.print(ln);
        d.setTextColor(MUTED, BG); d.setCursor(8, y + 32); d.print(TR("LED del PC (stato reale):","PC LEDs (real state):"));
        int x = 8, yy = y + 46;
        chip(x, yy, "CAPS",   leds & NK_HIDLED_CAPS, GRN);   x += 40;
        chip(x, yy, "NUM",    leds & NK_HIDLED_NUM, GRN);    x += 36;
        chip(x, yy, "SCROLL", leds & NK_HIDLED_SCROLL, GRN);
        d.setTextColor(DIM, BG);
        d.setCursor(8, y + 66); d.print(TR("Si accendono premendo i lock","They light when you press the locks"));
        d.setCursor(8, y + 78); d.print(TR("sul PC: e' il PC che ce lo dice.","on the PC: the PC tells us."));
        d.setTextColor(MUTED, BG); d.setCursor(8, y + 96); d.print(TR("Console seriale sospesa (USB presa).","Serial console suspended (USB taken)."));
    } else { // T_SET
        d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print(TR("ENTER / L cambia layout host","ENTER / L changes host layout"));
        bool us = (s_layout == DUCKY_LAYOUT_US);
        d.fillRoundRect(6, y + 16, 228, 24, 8, us ? ACC : LINE);
        if (us) d.fillRoundRect(6, y + 19, 5, 18, 2, ACC);
        d.setTextSize(2); d.setTextColor(us ? FG : MUTED, us ? ACC : LINE); d.setCursor(16, y + 20); d.print("US  QWERTY");
        d.fillRoundRect(6, y + 44, 228, 24, 8, !us ? ACC : LINE);
        if (!us) d.fillRoundRect(6, y + 47, 5, 18, 2, ACC);
        d.setTextSize(2); d.setTextColor(!us ? FG : MUTED, !us ? ACC : LINE); d.setCursor(16, y + 48); d.print(TR("IT  italiana","IT  Italian"));
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, y + 74); d.print(TR("Mappa i simboli sul layout del PC.","Maps symbols to the PC layout."));
    }
}

// ===== two screens =============================================================================================
// The panel carries the tabbed keyboard: Type is a status card + a big "start typing" button, Macro is the combo
// list on the left with a live preview of the modifier+key it sends (and a Send button) on the right, Info is the
// HID/host/LED state card, Settings is the US/IT layout as two big touch buttons. Tabs tap as 1..4. The Cardputer
// — where you actually type — is the companion: the tab, what is selected, and the keys. Touch is the content.
static const char *const LG_TABS_IT[] = { "Digita", "Macro", "Info", "Imp" };
static const char *const LG_TABS_EN[] = { "Type", "Macro", "Info", "Set" };

// Name the host key a macro sends, so the preview reads like "Ctrl + Alt + Del", not a hex code.
static const char *macro_keyname(uint8_t key)
{
    if (key == HID_DELfw) return "Del";
    if (key == HID_ESC)   return "Esc";
    if (key == HID_PRTSC) return "PrtSc";
    if (key == HID_F(4))  return "F4";
    if (key == 0x2B)      return "Tab";
    if (key == 0x07)      return "D";
    if (key == 0x0F)      return "L";
    if (key == 0x08)      return "E";
    return "Key";
}

static void mac_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    lg::row_text(x, y, w, h, focus, bg, TR(MACROS[i].name, MACROS[i].name_en), nullptr, FG);
}

static void lg_chip(int x, int y, const char *label, bool on, unsigned short oncol, unsigned short off)
{
    lg::font(0);
    const int w = (int)strlen(label) * 6 + 14;
    d.fillRoundRect(x, y, w, 20, 5, on ? oncol : off);
    d.setTextDatum(middle_center); d.setTextColor(on ? INK : MUTED, on ? oncol : off);
    d.drawString(label, x + w / 2, y + 11); d.setTextDatum(top_left);
}

static void kbd_draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    const bool conn = nucleo_usbhid_ready();
    const unsigned char leds = nucleo_usbhid_leds();
    const char *right = conn ? "host ok" : TR("attendo PC", "waiting PC");
    int y = lg::header("USB Keyboard", right, ACC, NK_BACK, 0);
    const char *const *N = nucleo_i18n_is_it() ? LG_TABS_IT : LG_TABS_EN;
    y = lg::tabs(y + 2, N, 4, s_tab, ACC, "1234");
    d.fillRect(0, y, W, H - y, BG);
    const unsigned short cb = lg::card();

    if (s_tab == T_TYPE) {
        const int cx = 16, cw = W - 32, cy = y + 10, chh = 96;
        lg::panel(cx, cy, cw, chh);
        lg::font(1); d.setTextColor(s_usb_ok ? FG : WARN, cb);
        d.drawString(s_usb_ok ? TR("Tastiera USB pronta", "USB keyboard ready") : TR("USB HID non installata", "USB HID not installed"), cx + 14, cy + 12);
        lg::font(0); d.setTextColor(conn ? GRN : MUTED, cb);
        lg::wrap(conn ? TR("Host collegato.", "Host connected.") : TR("Collega il cavo USB-C al PC.", "Plug the USB-C cable into the PC."), cx + 14, cy + 40, cw - 28, 17, 2);
        lg::font(0); d.setTextColor(DIM, cb);
        d.drawString(TR("Combo, Fn, frecce, layout IT/US.", "Combos, Fn, arrows, IT/US layout."), cx + 14, cy + chh - 20);
        const int bh = 56, by = H - bh - 16;
        lg::button(cx, by, cw, bh, TR("Inizia a digitare", "Start typing"), ACC, INK, NK_ENTER, 0);
    } else if (s_tab == T_MACRO) {
        const int lw = 168;
        lg::list(0, y + 2, lw, H - y - 4, lg::ROW, N_MACRO, s_macro, &s_lg_scroll, mac_row, nullptr, ACC);
        const int px = lw + 6, pw = W - lw - 12, py = y + 6, ph = H - y - 12;
        lg::panel(px, py, pw, ph);
        lg::font(1); d.setTextColor(ACC, cb);
        lg::wrap(TR(MACROS[s_macro].name, MACROS[s_macro].name_en), px + 12, py + 12, pw - 24, 20, 2);
        const uint8_t mod = MACROS[s_macro].mod;
        int cyp = py + 58;
        lg_chip(px + 12, cyp, "CTRL", mod & NK_HIDMOD_CTRL, ACC, cb == BG ? LINE : cb);
        lg_chip(px + 12 + 58, cyp, "ALT", mod & NK_HIDMOD_ALT, ACC, LINE); cyp += 26;
        lg_chip(px + 12, cyp, "SHIFT", mod & NK_HIDMOD_SHIFT, ACC, LINE);
        lg_chip(px + 12 + 70, cyp, "GUI", mod & NK_HIDMOD_GUI, ACC, LINE); cyp += 28;
        lg::font(1); d.setTextColor(FG, cb); d.drawString("+", px + 12, cyp);
        lg::font(2); d.setTextColor(GRN, cb); d.drawString(macro_keyname(MACROS[s_macro].key), px + 30, cyp - 4);
        const int bh = 46;
        lg::button(px + 10, py + ph - bh - 10, pw - 20, bh, TR("Invia al PC", "Send to PC"), GRN, INK, NK_ENTER, 0);
    } else if (s_tab == T_INFO) {
        const int cx = 16, cw = W - 32, cy = y + 10, chh = H - y - 20;
        lg::panel(cx, cy, cw, chh);
        int ky = cy + 14;
        lg::kv(cx + 14, ky, cw - 28, "USB HID", s_usb_ok ? TR("installata", "installed") : TR("errore", "error"), s_usb_ok ? GRN : WARN, cb); ky += 26;
        lg::kv(cx + 14, ky, cw - 28, "Host", conn ? TR("collegato", "connected") : TR("non rilevato", "not detected"), conn ? GRN : WARN, cb); ky += 30;
        lg::font(0); d.setTextColor(MUTED, cb); d.drawString(TR("LED del PC (stato reale):", "PC LEDs (real state):"), cx + 14, ky); ky += 20;
        lg_chip(cx + 14, ky, "CAPS", leds & NK_HIDLED_CAPS, GRN, LINE);
        lg_chip(cx + 14 + 66, ky, "NUM", leds & NK_HIDLED_NUM, GRN, LINE);
        lg_chip(cx + 14 + 66 + 60, ky, "SCROLL", leds & NK_HIDLED_SCROLL, GRN, LINE); ky += 30;
        lg::font(0); d.setTextColor(DIM, cb);
        lg::wrap(TR("Console seriale sospesa: la USB e' presa dalla tastiera.", "Serial console suspended: the USB is taken by the keyboard."), cx + 14, ky, cw - 28, 17, 2);
    } else {   // T_SET
        const bool us = (s_layout == DUCKY_LAYOUT_US);
        lg::font(0); d.setTextColor(MUTED, BG); d.drawString(TR("Mappa i simboli sul layout del PC.", "Maps symbols to the PC layout."), 16, y + 10);
        const int bw = W - 32, bh = 60;
        lg::button(16, y + 34, bw, bh, "US  QWERTY", us ? ACC : lg::card(), us ? INK : FG, NK_CHAR, 'u');
        lg::button(16, y + 34 + bh + 12, bw, bh, TR("IT  italiana", "IT  Italian"), !us ? ACC : lg::card(), !us ? INK : FG, NK_CHAR, 'i');
    }
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    const int y = lg::comp_head("USB KEYBOARD", ACC);
    const bool conn = nucleo_usbhid_ready();
    const char *const *N = nucleo_i18n_is_it() ? LG_TABS_IT : LG_TABS_EN;
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(ACC, BG);
    char t[48]; snprintf(t, sizeof t, "%s", N[s_tab]); d.drawString(t, 8, y + 2);
    d.setFont(&fonts::Font0); d.setTextColor(conn ? GRN : WARN, BG);
    d.drawString(conn ? TR("host collegato", "host connected") : TR("attendo il PC", "waiting for the PC"), 120, y + 6);

    if (s_tab == T_MACRO) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
        lg::wrap(TR(MACROS[s_macro].name, MACROS[s_macro].name_en), 8, y + 30, 224, 20, 2);
        lg::comp_keys(TR("su/giu scegli  INVIO invia", "up/dn pick  ENTER send"));
    } else if (s_tab == T_TYPE) {
        d.setFont(&fonts::Font0); d.setTextColor(s_usb_ok ? FG : WARN, BG);
        d.drawString(s_usb_ok ? TR("Tastiera USB pronta.", "USB keyboard ready.") : TR("USB HID non installata.", "USB HID not installed."), 8, y + 34);
        lg::comp_keys(TR("INVIO inizia a digitare", "ENTER start typing"));
    } else if (s_tab == T_INFO) {
        const unsigned char leds = nucleo_usbhid_leds();
        char m[40]; snprintf(m, sizeof m, "CAPS %s  NUM %s  SCRL %s", (leds & NK_HIDLED_CAPS) ? "*" : "-", (leds & NK_HIDLED_NUM) ? "*" : "-", (leds & NK_HIDLED_SCROLL) ? "*" : "-");
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(m, 8, y + 34);
        lg::comp_keys(TR("stato reale del PC", "the PC's real state"));
    } else {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
        d.drawString(s_layout == DUCKY_LAYOUT_US ? "Layout US" : "Layout IT", 8, y + 34);
        lg::comp_keys(TR("INVIO / L cambia layout", "ENTER / L switch layout"));
    }
}

extern "C" void nucleo_register_usbkbd(void)
{
    static const nucleo_app_def_t app = {
        "usbkbd", "USB Keyboard", "Connect", "Tastiera USB pro: layout IT/US, Fn, macro, LED del PC",
        'K', 0x4DDF, on_enter, on_key, on_tick, draw, nullptr,
        // Solo boot, radio down (the instrument policy — see app_goniometer.cpp): the typing modal
        // repaints per keystroke, and on a session whose 32 KB canvas died (~12 KB largest block at
        // runtime) that was a blink per key. A fresh heap guarantees the buffered path; the app is a
        // USB peripheral and uses zero network. Bonus: exiting reboots, which also returns the USB
        // PHY TinyUSB took from the serial console.
        NX_NET_APP | NX_SOLO | NX_WIFI, nullptr, kbd_draw_large
    };
    nucleo_app_register(&app);
}
