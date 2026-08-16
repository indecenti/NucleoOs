// USB apps — TWO separate launcher entries that share this file (and its render helpers), because
// they are two DIFFERENT things that merely happen to use the same physical port:
//
//   "usb"    USB Drive  (category Connect)  — ENTER expose the SD to a PC as a mass-storage drive.
//                                             The Cardputer becomes a USB disk, like USB Keyboard
//                                             makes it a USB keyboard. Reboots into a dedicated loop
//                                             so the firmware never touches the FAT while the PC owns it.
//   "usbweb" Web via USB (category Web OS)   — ENTER reboot into server-Solo and bring up a USB
//                                             network card (NCM). The PC gets an IP over the cable and
//                                             opens http://192.168.7.1 in ANY browser — plug-and-play,
//                                             no app, no driver, no Wi-Fi. This is the CABLE transport
//                                             to the web OS; Remote Control is the LAN transport.
//
// They were one app with an ENTER/W chooser, which conflated storage with web access and forced the
// whole thing under a single category. Split so each lives where its meaning is.
//
// Either mode reboots; any key / reset / power-cycle returns to NucleoOS (the USB-OTG shares pins with
// the serial console, so these modes can't run alongside the normal OS anyway). Strings are 5-language
// via L5(), ASCII only (the on-TFT font has no accented glyphs).
#include "nucleo_app.h"
#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents
#include "app_gfx.h"
#include "app_ui.h"           // app_ui_title accent header
#include <M5GFX.h>
#include <string.h>
extern "C" {
#include "nucleo_usbmsc.h"
#include "nucleo_usbnet.h"
#include "nucleo_i18n.h"      // nucleo_i18n_lang(): active OS language code
void nucleo_app_solo_request(int app);   // 4 = SOLO_REMOTE (server-Solo: httpd + auth up, max heap)
}
#define SOLO_REMOTE 4

static const unsigned short ACC = C_YELLOW, CAP = 0x1A8B;

// 5-language literal picker (ASCII only - the native font has no accented glyphs). es/fr/de fall back
// to Italian only if the code is unknown; here we always pass all five.
static const char *L5(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang();
    if (!l) return it;
    if (l[0] == 'e' && l[1] == 'n') return en;
    if (l[0] == 'e' && l[1] == 's') return es;
    if (l[0] == 'f' && l[1] == 'r') return fr;
    if (l[0] == 'd' && l[1] == 'e') return de;
    return it;
}

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// One call-to-action row: an accent key chip on the left, its description on the right.
static void row(int x, int y, int w, int hgt, const char *key, const char *desc)
{
    d.fillRoundRect(x, y, w, hgt, 6, CAP);
    int keyw = (int)strlen(key) * 12;                 // size-2 glyph width
    txt(x + 8, y + (hgt - 16) / 2, key, ACC, CAP, 2);
    txt(x + 8 + keyw + 10, y + (hgt - 8) / 2, desc, FG, CAP, 1);
}

// Shared single-action screen: a title, one short intro line, a big ENTER call-to-action row, and an
// optional detail line centered below it (the web address for the web mode). Strings are kept short
// enough to fit 240px at their size; textWrap is OFF so a long translation clips at the edge rather
// than wrapping onto — and colliding with — the row below it (that overlap was the bug).
static void draw_single(const char *title, const char *intro, const char *action, const char *detail)
{
    const int top = nucleo_app_content_top();
    const int h   = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    d.setTextWrap(false);
    const int y0 = app_ui_title(title, ACC, nullptr);
    txt(10, y0 + 6, intro, MUTED, BG, 1);
    const int bx = 8, bw = 224;
    const int ry = y0 + 20;
    int rh = detail ? 34 : ((top + h) - ry - 2);
    if (rh > 40) rh = 40;
    if (rh < 22) rh = 22;
    row(bx, ry, bw, rh, "ENTER", action);
    if (detail) {
        // centered, size-2 accent — the web address is the payoff, so give it weight
        d.setTextSize(2);
        int dw = (int)strlen(detail) * 12;
        int dx = (240 - dw) / 2; if (dx < 4) dx = 4;
        int dy = ry + rh + 10;
        if (dy + 16 <= top + h) { d.setTextColor(ACC, BG); d.setCursor(dx, dy); d.print(detail); }
    }
    d.setTextWrap(true);
}

// ---- USB Drive (SD as a PC disk) ----------------------------------------------------------------
static void drive_enter(void)
{
    nucleo_app_set_direct_draw(true);   // static screen: draw direct, free the 32 KB menu buffer
    nucleo_app_set_hint(L5("invio=collega   esc=esci", "enter=mount   esc=back",
                           "intro=montar   esc=salir", "entree=monter  esc=retour",
                           "enter=einbinden  esc=zurueck"));
    nucleo_app_request_draw();
}
static void drive_key(int key, char ch)
{
    if (key == NK_ENTER || ch == ' ')
        nucleo_usbmsc_request();        // reboot flag -> USB-drive loop (no return)
}
static void drive_draw(void)
{
    draw_single("USB Drive",
        L5("microSD come disco del PC", "microSD as a PC drive", "microSD como disco del PC",
           "microSD comme disque PC", "microSD als PC-Laufwerk"),
        L5("Collega", "Mount", "Montar", "Monter", "Einbinden"),
        nullptr);
}

// ---- Web via USB (web OS over the NCM cable) ----------------------------------------------------
static void web_enter(void)
{
    nucleo_app_set_direct_draw(true);
    nucleo_app_set_hint(L5("invio=avvia   esc=esci", "enter=start   esc=back",
                           "intro=iniciar   esc=salir", "entree=lancer  esc=retour",
                           "enter=starten  esc=zurueck"));
    nucleo_app_request_draw();
}
static void web_key(int key, char ch)
{
    if (key == NK_ENTER || ch == ' ') {
        nucleo_usbnet_web_arm();                // arm the USB-net flag, then reboot into server-Solo
        nucleo_app_solo_request(SOLO_REMOTE);   // httpd + auth up; USB NIC comes up after httpd (no return)
    }
}
static void web_draw(void)
{
    draw_single("Web via USB",
        L5("Web OS via cavo, senza WiFi", "Web OS over cable, no Wi-Fi", "Web OS por cable, sin WiFi",
           "Web OS par cable, sans WiFi", "Web-OS uber Kabel, ohne WLAN"),
        L5("Avvia", "Start", "Iniciar", "Lancer", "Starten"),
        "http://192.168.7.1");
}

extern "C" void nucleo_register_usb(void)
{
    static const nucleo_app_def_t app = {
        "usb", "USB Drive", "Connect", "Expose the SD to a PC as a USB mass-storage drive",
        'U', ACC, drive_enter, drive_key, nullptr, drive_draw, nullptr
    };
    nucleo_app_register(&app);
}

extern "C" void nucleo_register_usbweb(void)
{
    static const nucleo_app_def_t app = {
        "usbweb", "Web via USB", "Web OS", "Serve the web OS to a PC browser over the USB cable (NCM, no Wi-Fi)",
        'W', C_PURPLE, web_enter, web_key, nullptr, web_draw, nullptr
    };
    nucleo_app_register(&app);
}
