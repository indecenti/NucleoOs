// Beacon Spam app — on-device UI for the beacon-flood engine (nucleo_wifiatk beacon_*).
//
// AUTHORIZED USE ONLY. Broadcasts a wall of fake open-network beacons (the same "Beacon Spam" Bruce
// and Marauder ship) so bogus SSIDs flood everyone's Wi-Fi list. For security-awareness demos and
// CTF/lab work. The app opens on a consent screen and will not arm until the operator acknowledges.
//
// Controls (the launcher eats Left/Back to leave the app):
//   Consent : Enter = accept, Esc = leave
//   Running : Enter stops the spam (Esc also stops, via on_exit)
#include "nucleo_app.h"
#include "nucleo_exclusive.h"   // NX_SOLO: beacon runs in a fresh, unfragmented heap (see registration)
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"

extern "C" {
// Engine API (declared here, not via REQUIRES — symbols resolve at final link, like app_wifiatk).
// Mode codes mirror NUCLEO_BEACON_* in nucleo_wifiatk.h.
int           nucleo_wifiatk_beacon_start(int mode); // esp_err_t (0 == ok)
void          nucleo_wifiatk_beacon_stop(void);
bool          nucleo_wifiatk_beacon_running(void);
unsigned long nucleo_wifiatk_beacon_frames(void);
int           nucleo_wifiatk_beacon_count(void);
int           nucleo_wifiatk_beacon_mode(void);
int           nucleo_wifiatk_beacon_health(void);
unsigned      nucleo_wifiatk_beacon_uptime_s(void);
void          nucleo_wifiatk_beacon_custom_clear(void);
bool          nucleo_wifiatk_beacon_custom_add(const char *ssid);
int           nucleo_wifiatk_beacon_custom_count(void);
const char   *nucleo_wifiatk_beacon_custom_ssid(int i);
}

enum { MODE_FUNNY = 0, MODE_RANDOM = 1, MODE_CLONE = 2, MODE_CUSTOM = 3, N_MODE = 4 };
static const char *MODE_NAME[N_MODE]  = { "Divertenti", "Casuali", "Clona vicine", "Personalizzate" };
static const char *MODE_BLURB[N_MODE] = {
    "muro di SSID buffi/plausibili",
    "reti finte generate a caso",
    "gemelle delle reti reali qui",
    "scrivi tu i nomi delle reti",
};

#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "app_gfx.h"
#include "nucleo_i18n.h"      // TR(it,en): hints follow the system language

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short BCN = 0xAD5F /*violet*/, GRN = C_GREEN, YEL = C_YELLOW;

enum { ST_CONSENT, ST_CUSTOM, ST_ARMING, ST_RUNNING, ST_STOPPING };
static int  s_state;
static bool s_consented;
static bool s_arm_armed;          // set once the "Avvio..." screen has been painted
static bool s_stop_armed;
static char s_err[40];
static uint32_t s_last_refresh;
static bool s_pulse;
static int  s_mode = MODE_FUNNY;  // which beacon mode the operator picked on the consent screen
static int  s_sel  = MODE_FUNNY;  // highlighted mode in the selector
static char s_line[33];           // CUSTOM: the SSID being typed
static int  s_linelen;

static void set_hint(void)
{
    if (s_state == ST_CONSENT)       nucleo_app_set_hint(TR("su/giu scegli   invio avvia   esc esci", "up/dn pick   enter start   esc back"));
    else if (s_state == ST_CUSTOM)   nucleo_app_set_hint(TR("invio aggiungi   canc togli   tab avvia   esc esci", "enter add   del remove   tab start   esc back"));
    else if (s_state == ST_ARMING)   nucleo_app_set_hint(TR("avvio in corso...", "starting..."));
    else if (s_state == ST_STOPPING) nucleo_app_set_hint(TR("arresto in corso...", "stopping..."));
    else                             nucleo_app_set_hint(TR("invio: ferma   esc: lascia attivo", "enter: stop   esc: leave on"));
}

static void enter_custom(void)
{
    nucleo_wifiatk_beacon_custom_clear();
    s_line[0] = 0; s_linelen = 0;
    s_state = ST_CUSTOM; set_hint(); nucleo_app_request_draw();
}

static void enter(void)
{
    s_err[0] = 0;
    if (nucleo_wifiatk_beacon_running()) s_state = ST_RUNNING;
    else if (s_consented)               { s_state = ST_ARMING; s_arm_armed = false; }
    else                                s_state = ST_CONSENT;
    set_hint();
    nucleo_app_request_draw();
}

static void tick(void)
{
    if (s_state == ST_ARMING) {
        if (!s_arm_armed) return;
        int rc = nucleo_wifiatk_beacon_start(s_mode);
        if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; }
        else { snprintf(s_err, sizeof s_err, "Avvio fallito (err %d)", rc); s_state = ST_CONSENT; }
        set_hint();
        nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;
        nucleo_wifiatk_beacon_stop();
        nucleo_app_exit();
        return;
    }
    if (s_state != ST_RUNNING && s_state != ST_CUSTOM) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last_refresh) { s_last_refresh = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
}

static void arm_selected(void)
{
    s_mode = s_sel; s_consented = true;
    s_state = ST_ARMING; s_arm_armed = false;
    set_hint(); nucleo_app_request_draw();
}

// Enter the highlighted mode: CUSTOM opens the type-your-SSIDs screen, the rest arm straight away.
static void pick_selected(void)
{
    if (s_sel == MODE_CUSTOM) enter_custom();
    else                      arm_selected();
}

static void on_key(int key, char ch)
{
    if (s_state == ST_CONSENT) {
        if (key == NK_UP)        { s_sel = (s_sel + N_MODE - 1) % N_MODE; nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { s_sel = (s_sel + 1) % N_MODE;          nucleo_app_request_draw(); }
        else if (ch >= '1' && ch <= '4') { s_sel = ch - '1'; pick_selected(); }   // 1-9 quick-select
        else if (key == NK_ENTER) pick_selected();
        return;
    }
    if (s_state == ST_CUSTOM) {
        if (key == NK_ENTER) {                       // commit the typed SSID, ready the next
            if (s_linelen > 0 && nucleo_wifiatk_beacon_custom_add(s_line)) { s_line[0] = 0; s_linelen = 0; }
        } else if (key == NK_DEL) {                  // backspace (NK_BACK is eaten by the launcher)
            if (s_linelen > 0) s_line[--s_linelen] = 0;
        } else if (key == NK_TAB) {                  // arm with the list you've built
            if (nucleo_wifiatk_beacon_custom_count() > 0) { s_sel = MODE_CUSTOM; arm_selected(); return; }
        } else if (ch >= 32 && ch < 127 && s_linelen < 32) {   // printable -> append
            s_line[s_linelen++] = ch; s_line[s_linelen] = 0;
        }
        nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_ARMING || s_state == ST_STOPPING) return;
    if (s_state == ST_RUNNING && key == NK_ENTER) {
        s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw();
    }
}

// ---- drawing ----------------------------------------------------------------
static const unsigned short HLBG = 0x10A2;   // selected-row background (dark blue-grey)

static void draw_consent(void)
{
    app_ui_title("Beacon Spam", BCN, "AUTH");
    d.setTextSize(1); d.setTextColor(YEL, BG); d.setCursor(10, 26); d.print("Solo test autorizzati (lab/CTF).");
    d.setTextColor(MUTED, BG); d.setCursor(10, 38); d.print("Trasmette molte reti WiFi finte.");

    for (int i = 0; i < N_MODE; i++) {
        int y = 54 + i * 21;
        bool sel = (i == s_sel);
        unsigned short bg = sel ? HLBG : BG;
        if (sel) d.fillRect(6, y - 2, 228, 19, HLBG);
        char head[26]; snprintf(head, sizeof head, "%d  %s", i + 1, MODE_NAME[i]);
        d.setTextSize(2); d.setTextColor(sel ? FG : MUTED, bg); d.setCursor(12, y); d.print(head);
    }

    d.setTextSize(1);
    if (s_err[0]) { d.setTextColor(BCN, BG); d.setCursor(10, 122); d.print(s_err); }
    else          { d.setTextColor(GRN, BG); d.setCursor(10, 122); d.print(MODE_BLURB[s_sel]); }
}

static void draw_custom(void)
{
    int h = nucleo_app_content_height();
    int cnt = nucleo_wifiatk_beacon_custom_count();
    app_ui_title("Personalizzate", BCN, "AUTH");
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 26);
    char hdr[28]; snprintf(hdr, sizeof hdr, "Reti aggiunte: %d", cnt); d.print(hdr);

    // The line being typed, with a blinking-ish caret.
    d.drawRect(8, 38, 224, 18, LINE);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(12, 40);
    d.print(s_line[0] ? s_line : "");
    if (s_pulse) { int cx = 12 + s_linelen * 12; d.fillRect(cx, 40, 9, 14, BCN); }

    // The last few committed names, newest at the bottom.
    d.setTextSize(1);
    int show = cnt < 5 ? cnt : 5;
    for (int i = 0; i < show; i++) {
        int idx = cnt - show + i;
        d.setTextColor(GRN, BG); d.setCursor(12, 62 + i * 11);
        char row[40]; snprintf(row, sizeof row, "%d. %s", idx + 1, nucleo_wifiatk_beacon_custom_ssid(idx));
        d.print(row);
    }
    d.setTextColor(YEL, BG); d.setCursor(10, h - 9);
    d.print(cnt > 0 ? "tab avvia   invio aggiungi" : "scrivi un nome + invio");
}

static void draw_busy(const char *title, const char *msg)
{
    int h = nucleo_app_content_height();
    app_ui_title("Beacon Spam", BCN, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print(title);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6); d.print(msg);
}

static void tile(int cx, const char *label, const char *val, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(cx - (int)strlen(label) * 3, 40); d.print(label);
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(cx - (int)strlen(val) * 9, 52); d.print(val);
}

static void draw_running(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("Beacon Spam", BCN, "");
    if (s_pulse) d.fillCircle(150, 9, 4, BCN);
    d.setTextSize(1); d.setTextColor(BCN, BG); d.setCursor(160, 7); d.print("TX");
    unsigned up = nucleo_wifiatk_beacon_uptime_s();
    char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(el) * 6, 7); d.print(el);

    d.setTextSize(1); d.setTextColor(GRN, BG); d.setCursor(10, 28);
    char ss[40]; snprintf(ss, sizeof ss, "%d reti finte - %s",
                          nucleo_wifiatk_beacon_count(), MODE_NAME[nucleo_wifiatk_beacon_mode() % N_MODE]);
    d.print(ss);

    char fr[12]; unsigned long f = nucleo_wifiatk_beacon_frames();
    if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);
    char sc[8]; snprintf(sc, sizeof sc, "%d", nucleo_wifiatk_beacon_count());
    tile(62,  "BEACON", fr, BCN);
    tile(178, "SSID",   sc, GRN);

    d.drawFastHLine(10, 84, 220, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 92); d.print("canali 1 / 6 / 11");
    // Injection health: the honest "are the frames actually leaving the radio" readout. Green = the
    // chip is accepting the flood; low/red = it's dropping frames (then bad reception, not a bug here).
    int hp = nucleo_wifiatk_beacon_health();
    unsigned short hc = hp >= 80 ? GRN : (hp >= 50 ? YEL : BCN);
    char hs[16]; snprintf(hs, sizeof hs, "inj %d%%", hp);
    d.setTextColor(hc, BG); d.setCursor(238 - (int)strlen(hs) * 6, 92); d.print(hs);
    d.setTextColor(YEL, BG); d.setCursor(10, h - 9); d.print("invio: ferma   esc: lascia attivo");
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_state == ST_CONSENT)       draw_consent();
    else if (s_state == ST_CUSTOM)   draw_custom();
    else if (s_state == ST_ARMING)   { draw_busy("Avvio...", "Genero le reti finte."); s_arm_armed = true; }
    else if (s_state == ST_STOPPING) { draw_busy("Arresto...", "Ripristino rete OS."); s_stop_armed = true; }
    else                             draw_running();
}

static void leave(void)
{
    // The spam keeps running in the background after you leave (the launcher shows a red alert bar);
    // reopen and press Enter to stop it. Reset consent so re-arming ALWAYS re-asks for authorization
    // (s_consented was a per-boot latch: without this, reopening after a stop re-armed silently).
    s_consented = false;
}

extern "C" void nucleo_register_beacon(void)
{
    static const nucleo_app_def_t app = {
        "beacon", "Beacon Spam", "Security", "Fake-SSID beacon flood for authorized Wi-Fi testing",
        'B', BCN, enter, on_key, tick, draw, leave,
        // SOLO BOOT: reboot into a FRESH, unfragmented heap before arming. The raw-TX/promiscuous path
        // needs contiguous DMA-capable buffers from the Wi-Fi driver; on the live (fragmented) OS heap
        // those allocations can fail and panic the device the instant you arm. A clean boot makes the
        // injection rock-solid — same trick the games use. Esc reboots back to the full OS.
        NX_SOLO
    };
    nucleo_app_register(&app);
}
