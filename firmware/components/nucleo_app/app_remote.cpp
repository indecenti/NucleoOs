// Built-in "Remote Control" app: the control panel for the web-client handoff.
//
// Opening it (from the launcher list OR the Control Center "Web Client" tile) WARM-REBOOTS the device into
// the Web Client server-Solo profile: a fresh, unfragmented heap with ONLY httpd + auth up (no offline L1,
// voice, TTS, IR, recorder or calendar). That is the fix for "the device reboots when I open NucleoOS web"
// — the first, heaviest shell load used to hit the fragmented full-OS heap and OOM. maybe_solo_launch()
// (nucleo_app.cpp) owns that reboot; the OTA path is the one exception (it opens this app INLINE so an
// in-flight image POST is never reset). Once booted, this app is the server-listening screen. It:
//   • shows HOW to connect — the device IP + pairing PIN, big and readable;
//   • on enter, frees the 32 KB launcher canvas + the ~24 KB ANIMA L1 index (belt-and-braces on top of the
//     Solo boot, and the real teardown on the OTA inline path) so the FIRST shell load already has room;
//   • lets the user toggle the automatic handoff on/off (persisted in NVS);
//   • Esc reboots back to the full OS (standard Solo exit).
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"                        // TR(it,en): hint follows the system language
#include "app_large.h"                          // two screens: status cards + handoff toggle on the panel
#include "nucleo_touch.h"
#include <M5GFX.h>
#include <stdio.h>
#include <string.h>                             // strncmp: the address's "http://" scheme
#include "nvs.h"                                // persist the auto-handoff toggle across reboots
#include "esp_log.h"                            // one breadcrumb per view and visit: which screen drew the first frame

extern "C" int  nucleo_ws_client_count(void);   // resolved at link (no component dep)
extern "C" int  nucleo_ws_shell_count(void);    // shell clients that actually drive the handoff (/ws?shell=1)
extern "C" const char *nucleo_setup_ip(void);   // device IP (empty in AP-only / not associated)
extern "C" const char *nucleo_auth_pin(void);   // pairing PIN (changes each boot)
#include "nucleo_usbnet.h"                       // USB-web active? + the fixed address (NCM)
#include "nucleo_i18n.h"                         // nucleo_i18n_lang() for the 5-language screen

// 5-language literal picker (ASCII — the on-TFT font has no accented glyphs).
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
extern "C" bool nucleo_anima_l1_unload_if_idle(void);  // free the ~24 KB offline index (lazy reload later)
extern "C" void nucleo_voice_suspend(bool suspend);    // free the ~16 KB voice engine (restored on leave if still wanted)

#include "app_gfx.h"

// Auto-handoff is on by default and PERSISTED in NVS: a user who turns it OFF keeps it off across reboots
// (it used to silently re-enable every boot). Lazy-loaded on the first nucleo_remote_enabled() read (NVS is
// up well before the run loop, main.c), so the hot-path read after that is just a bool.
#define REMOTE_NVS_NS  "nucleo"
#define REMOTE_NVS_KEY "remote_en"
static bool s_remote_enabled = true;
static bool s_remote_loaded  = false;

static void remote_load(void)
{
    if (s_remote_loaded) return;
    s_remote_loaded = true;
    nvs_handle_t h;
    if (nvs_open(REMOTE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, REMOTE_NVS_KEY, &v) == ESP_OK) s_remote_enabled = (v != 0);
        nvs_close(h);
    }
}
static void remote_save(void)
{
    nvs_handle_t h;
    if (nvs_open(REMOTE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, REMOTE_NVS_KEY, s_remote_enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

extern "C" bool nucleo_remote_enabled(void) { remote_load(); return s_remote_enabled; }
// Same persisted toggle, driven from Settings > Wi-Fi > Web handoff (the canonical home for it).
extern "C" void nucleo_remote_set_enabled(bool on) { remote_load(); s_remote_enabled = on; remote_save(); }

static int s_sig = -1;
static unsigned s_seen;                         // views that drew since enter (1 compact, 2 panel, 4 companion)
static void remote_seen(unsigned v, const char *name)
{
    if (s_seen & v) return;
    s_seen |= v;
    ESP_LOGW("remote", "first frame: %s (usb=%d)", name, (int)nucleo_usbnet_is_active());
}

static void remote_enter(void)
{
    // Optimized "listening" mode the instant the app opens — BEFORE any browser connects. Hand the web OS
    // every spare byte so the first (heaviest) shell load has room: free the 32 KB shared launcher canvas
    // and draw DIRECT (set_direct_draw stops the run loop from lazily re-acquiring it), and drop the ~24 KB
    // ANIMA L1 working set. mDNS + the HTTP server stay UP, so the device is still discoverable and ready to
    // serve. The framework restores everything on close: the launcher re-acquires the canvas, L1 reloads
    // lazily on the next offline query, set_direct_draw is auto-cleared. Same proven pattern the ANIMA app uses.
    nucleo_app_release_buffers();        // free the 32 KB shared canvas
    nucleo_app_set_direct_draw(true);    // pin direct drawing so the loop never lazily re-acquires it
    nucleo_anima_l1_unload_if_idle();    // free ~24 KB (lazy reload later)
    nucleo_voice_suspend(true);          // free the ~16 KB voice engine — Remote Control is a server-listening
                                         // screen, it never needs the mic; hand that RAM to httpd/OTA/API too.
                                         // Restored in remote_exit (only if a holder still wants it).
    nucleo_app_set_hint(TR5("invio handoff auto on/off   esc esci", "enter auto handoff on/off   esc back", "intro handoff auto on/off   esc atr", "entree handoff auto on/off   esc retour", "enter auto-uberg. an/aus   esc zurueck"));
    s_sig = -1; s_seen = 0;
    nucleo_app_request_draw();
}

static void remote_exit(void)
{
    // Leaving the server-listening screen: let the voice engine come back if anything still wants it
    // (suspend(false) is a no-op when no holder is present, so it never spuriously recreates the engine).
    // The 32 KB canvas + direct-draw pin are restored by the framework on app close.
    nucleo_voice_suspend(false);
}

static void remote_key(int key, char ch)
{
    (void)ch;
    if (key == NK_ENTER) { s_remote_enabled = !s_remote_enabled; remote_save(); nucleo_app_request_draw(); }
}

static void remote_tick(void)   // redraw only when the live state actually changes (direct-draw: avoid flicker)
{
    // Fold the client count, the toggle, AND a coarse hash of IP+PIN so a late DHCP lease or a PIN that
    // lands after boot repaints the card. Cheap djb2 over the two short strings.
    unsigned h = 5381;
    for (const char *p = nucleo_setup_ip();  *p; p++) h = h * 33 + (unsigned char)*p;
    for (const char *p = nucleo_auth_pin();  *p; p++) h = h * 33 + (unsigned char)*p;
    int sig = (int)((h << 2) ^ (unsigned)(nucleo_ws_shell_count() << 1) ^ (s_remote_enabled ? 1u : 0u));
    if (sig != s_sig) { s_sig = sig; nucleo_app_request_draw(); }
}

static void remote_companion(void);
static void remote_draw(void)
{
    if (nucleo_app_two_screens()) { remote_companion(); return; }
    remote_seen(1, "compact");
    int top_y = nucleo_app_content_top();
    d.fillRect(0, top_y, W, nucleo_app_content_height(), BG);
    const int sh = nucleo_ws_shell_count();
    const bool usb = nucleo_usbnet_is_active();     // came up via USB-web (key W) vs Wi-Fi Remote
    const char *ip = nucleo_setup_ip();
    const char *pin = nucleo_auth_pin();
    int y = top_y + 2;

    // Status line: connected vs ready-and-listening (localized).
    d.setTextSize(1); d.setTextColor(C_GREEN, BG); d.setCursor(10, y);
    if (sh > 0) { char s[32]; snprintf(s, sizeof s, "%s %d", L5("CONNESSO", "CONNECTED", "CONECTADO", "CONNECTE", "VERBUNDEN"), sh); d.print(s); }
    else        { d.print(L5("IN ASCOLTO - PRONTO", "LISTENING - READY", "A LA ESCUCHA - LISTO", "EN ECOUTE - PRET", "BEREIT - WARTET")); }
    y += 15;

    if (usb) {
        // USB-web (NCM): the PC gets an IP over the cable; open this ONE fixed address in ANY browser.
        // No app, no driver. Wi-Fi is OFF in this mode, so there is no other/LAN address to show — the
        // cable IS the network (header + /api/status now read "usb", not a phantom access point).
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, y);
        d.print(L5("Nel browser del PC apri:", "In the PC browser open:", "En el navegador del PC:", "Dans le navigateur du PC:", "Im PC-Browser oeffnen:")); y += 12;
        d.setTextSize(2); d.setTextColor(C_GREEN, BG); d.setCursor(10, y); d.print("http://192.168.7.1"); y += 20;
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, y);
        d.print(L5("via cavo USB, niente da installare", "over the USB cable, nothing to install", "por cable USB, nada que instalar", "par cable USB, rien a installer", "uber USB-Kabel, nichts installieren")); y += 14;
    } else {
        // Wi-Fi Remote Control: the device IP.
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, y);
        d.print(L5("Apri nel browser:", "Open in a browser:", "Abre en el navegador:", "Ouvre dans le navigateur:", "Im Browser oeffnen:")); y += 11;
        d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, y);
        d.print((ip && ip[0]) ? ip : L5("(nessuna rete)", "(no network)", "(sin red)", "(pas de reseau)", "(kein Netz)")); y += 20;
    }

    // Pairing PIN (changes each boot) — needed to pair the browser, over USB or LAN.
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, y);
    d.print(L5("PIN accoppiamento:", "Pair PIN:", "PIN de vinculacion:", "PIN d'appairage:", "Kopplungs-PIN:")); y += 11;
    d.setTextSize(2); d.setTextColor(C_BLUE, BG); d.setCursor(10, y); d.print((pin && pin[0]) ? pin : "----"); y += 20;

    // Auto-handoff toggle (Enter) — Wi-Fi Remote only (USB-web has no screen handoff).
    if (!usb) {
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, y);
        d.print(L5("Passaggio auto: ", "Auto handoff: ", "Traspaso auto: ", "Relais auto: ", "Auto-Uebergabe: "));
        d.setTextColor(s_remote_enabled ? C_GREEN : C_RED, BG); d.print(s_remote_enabled ? "ON" : "OFF");
    }
}

// ===== two screens =============================================================================================
// The panel carries the "how to connect" cards: the live link state, the address to open as the hero line, the
// pairing PIN, and the auto-handoff as a settings row (lg::setting) - the row IS the toggle (tap = Enter, the same
// key that flips it on the Cardputer). The transport (Wi-Fi / USB) sits in the header. The address and the PIN are
// typed by hand on another device, so they are never cut: each takes the biggest kit font it fits in WHOLE
// (lg::font_fit) - a 15-digit LAN address or USB-web's "http://192.168.7.1" step down a size instead of ending "..".
// The Cardputer becomes a glance companion: the same facts, the address big enough to read from a desk away.
static const char *remote_addr(bool usb)
{
    const char *ip = nucleo_setup_ip();
    if (usb) return "http://192.168.7.1";          // USB-web (NCM): one fixed address, the cable is the network
    return (ip && ip[0]) ? ip : L5("(nessuna rete)", "(no network)", "(sin red)", "(pas de reseau)", "(kein Netz)");
}
static bool remote_has_addr(bool usb) { const char *ip = nucleo_setup_ip(); return usb || (ip && ip[0]); }
static const char *remote_state(int sh)
{
    static char st[40];
    if (sh > 0) snprintf(st, sizeof st, "%s %d", L5("Connesso", "Connected", "Conectado", "Connecte", "Verbunden"), sh);
    else snprintf(st, sizeof st, "%s", L5("In ascolto", "Listening", "A la escucha", "En ecoute", "Bereit"));
    return st;
}

// An address in the current font and datum: a "http://" scheme muted, the part that is typed in `col`.
static void remote_draw_addr(const char *s, int x, int y, unsigned short col, unsigned short bg)
{
    const char *host = !strncmp(s, "http://", 7) ? s + 7 : s;
    if (host != s) { d.setTextColor(MUTED, bg); d.drawString("http://", x, y); x += d.textWidth("http://"); }
    d.setTextColor(col, bg);
    d.drawString(host, x, y);
}

static void remote_draw_large(void)
{
    const int W_ = d.width(), H_ = d.height();
    const int sh  = nucleo_ws_shell_count();
    const bool usb = nucleo_usbnet_is_active();
    const char *pin = nucleo_auth_pin();
    remote_seen(2, "panel");
    nucleo_touch_clear();
    int y = lg::header("Remote Control", usb ? "USB" : "Wi-Fi", C_BLUE);
    d.fillRect(0, y, W_, H_ - y, BG);
    // Cards inset 10 from the edges and 6 apart, text 14 inside them: the state, the address (the one tall card),
    // the PIN and the last row 38 tall each, so the column reads as one rhythm down to a 6 px bottom margin.
    const int cx = 10, cw = W_ - 20, in = 14, gap = 6, ch = 38, ah = H_ - y - 3 * ch - 5 * gap;   // ah = 66 at 240
    const unsigned short cb = lg::card();

    // link state: a lamp + the state in words
    int cy = y + gap;
    lg::panel(cx, cy, cw, ch);
    d.fillCircle(cx + 22, cy + ch / 2, 7, sh > 0 ? C_GREEN : C_BLUE);
    lg::font(2); d.setTextDatum(middle_left); d.setTextColor(sh > 0 ? C_GREEN : FG, cb);
    d.drawString(remote_state(sh), cx + 40, cy + ch / 2 + 1);

    // the address to open: the hero line, whole
    cy += ch + gap;
    lg::panel(cx, cy, cw, ah);
    lg::font(0); d.setTextDatum(top_left); d.setTextColor(MUTED, cb);
    d.drawString(usb ? L5("Nel browser del PC apri:", "In the PC browser open:", "En el navegador del PC:", "Dans le navigateur du PC:", "Im PC-Browser oeffnen:")
                     : L5("Apri nel browser:", "Open in a browser:", "Abre en el navegador:", "Ouvre dans le navigateur:", "Im Browser oeffnen:"),
                 cx + in, cy + 7);
    const char *addr = remote_addr(usb);
    const bool has = remote_has_addr(usb);
    if (has) lg::font_fit(addr, cw - 2 * in); else lg::font(1);
    char ab[40]; lg::fit(ab, sizeof ab, addr, cw - 2 * in);     // a no-op unless not even Font2 fits
    d.setTextDatum(middle_left);
    remote_draw_addr(ab, cx + in, cy + 24 + (ah - 24) / 2, has ? (usb ? C_GREEN : FG) : MUTED, cb);

    // the pairing PIN (changes each boot): needed over USB and over the LAN alike
    cy += ah + gap;
    lg::panel(cx, cy, cw, ch);
    lg::font(0); d.setTextColor(MUTED, cb); d.setTextDatum(middle_left);
    d.drawString(L5("PIN accoppiamento", "Pairing PIN", "PIN de vinculacion", "PIN d'appairage", "Kopplungs-PIN"), cx + in, cy + ch / 2);
    const char *pn = (pin && pin[0]) ? pin : "------";
    lg::font_fit(pn, cw / 2 - in, 2); d.setTextColor(C_BLUE, cb); d.setTextDatum(middle_right);
    d.drawString(pn, cx + cw - in, cy + ch / 2 + 1);
    d.setTextDatum(top_left);

    // the auto-handoff as a settings row with the kit switch (Wi-Fi Remote only); USB-web has no screen handoff:
    // its last card says what the cable gives instead
    cy += ch + gap;
    if (!usb) {
        const lg::Setting st = { L5("Passaggio auto", "Auto handoff", "Traspaso auto", "Relais auto", "Auto-Uebergabe"), nullptr, nullptr,
                                 lg::ROW_TOGGLE, -1, -1, s_remote_enabled, false, 0, 0, 0 };
        lg::setting(cx, cy, cw, ch + 4, st, false, C_BLUE);   // the kit row draws its card h - 4 tall
        nucleo_touch_add(cx, cy - gap / 2, cw, H_ - cy + gap / 2, NK_ENTER, 0);   // the row IS the switch: tap = ENTER
    } else {
        lg::panel(cx, cy, cw, ch);
        lg::font(0); d.setTextColor(MUTED, cb); d.setTextDatum(middle_center);
        char nb[48]; lg::fit(nb, sizeof nb, L5("Via cavo USB, niente da installare", "Over the USB cable, nothing to install", "Por cable USB, nada que instalar",
                                               "Par cable USB, rien a installer", "Uber USB-Kabel, nichts installieren"), cw - 2 * in);
        d.drawString(nb, cx + cw / 2, cy + ch / 2);
        d.setTextDatum(top_left);
    }
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the cards: state, then the address as big as the 224 px line allows (the
// panel may be across the desk; this is the screen in hand), the PIN with the handoff state on its line, the keys.
static void remote_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    const int sh  = nucleo_ws_shell_count();
    const bool usb = nucleo_usbnet_is_active();
    const char *pin = nucleo_auth_pin();
    remote_seen(4, "companion");
    const int y = lg::comp_head(usb ? "REMOTE  USB" : "REMOTE", C_BLUE);
    const int keys_y = top + h - 11;                 // where lg::comp_keys draws
    // three lines (centres) between the head and the keys: state, address, PIN + handoff; the address, the tallest,
    // in the middle, and the last line kept clear of the key hint
    const int l1 = y + 11, l3 = keys_y - 24, l2 = (l1 + l3) / 2 + 1;
    d.setTextDatum(middle_left);
    lg::font(1); d.setTextColor(sh > 0 ? C_GREEN : C_BLUE, BG);
    d.drawString(remote_state(sh), 8, l1);
    const char *addr = remote_addr(usb);
    const bool has = remote_has_addr(usb);
    if (has) lg::font_fit(addr, 224, 2); else lg::font(0);
    char ab[40]; lg::fit(ab, sizeof ab, addr, 224);
    remote_draw_addr(ab, 8, l2, has ? (usb ? C_GREEN : FG) : MUTED, BG);
    lg::font(0); d.setTextColor(MUTED, BG); d.drawString("PIN", 8, l3);
    const int px = 8 + d.textWidth("PIN") + 8;
    lg::font(1); d.setTextColor(C_BLUE, BG); d.drawString((pin && pin[0]) ? pin : "------", px, l3 + 1);
    if (!usb) {                                      // right-aligned on the PIN line, named as in the key hint
        lg::font(0); d.setTextDatum(middle_right);
        const char *on = s_remote_enabled ? "ON" : "OFF";
        d.setTextColor(s_remote_enabled ? C_GREEN : C_RED, BG); d.drawString(on, 232, l3);
        d.setTextColor(MUTED, BG); d.drawString(L5("Handoff", "Handoff", "Handoff", "Relais", "Ubergabe"), 232 - d.textWidth(on) - 6, l3);
    }
    d.setTextDatum(top_left);
    lg::comp_keys(usb ? L5("ESC esci", "ESC back", "ESC salir", "ESC retour", "ESC zurueck")
                      : L5("INVIO handoff on/off  ESC esci", "ENTER handoff on/off  ESC back", "INTRO handoff  ESC salir", "ENTREE handoff  ESC retour", "ENTER Ubergabe  ESC zurueck"));
}

extern "C" void nucleo_register_remote(void)
{
    static const nucleo_app_def_t app = {
        "remote", "Remote Control", "Web OS", "Web Client server mode: reboots into a fresh heap (max RAM), shows IP + PIN, ready for the web OS",
        'r', C_BLUE, remote_enter, remote_key, remote_tick, remote_draw, remote_exit, 0, nullptr, remote_draw_large
    };
    nucleo_app_register(&app);
}
