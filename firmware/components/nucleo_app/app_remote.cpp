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
#include <M5GFX.h>
#include <stdio.h>
#include "nvs.h"                                // persist the auto-handoff toggle across reboots

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

static int s_sig = -1;

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
    nucleo_app_set_hint(TR("invio handoff auto on/off   esc esci", "enter auto handoff on/off   esc back"));
    s_sig = -1;
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

static void remote_draw(void)
{
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

extern "C" void nucleo_register_remote(void)
{
    static const nucleo_app_def_t app = {
        "remote", "Remote Control", "Web OS", "Web Client server mode: reboots into a fresh heap (max RAM), shows IP + PIN, ready for the web OS",
        'r', C_BLUE, remote_enter, remote_key, remote_tick, remote_draw, remote_exit
    };
    nucleo_app_register(&app);
}
