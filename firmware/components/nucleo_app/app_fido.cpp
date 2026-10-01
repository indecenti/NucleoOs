// app_fido.cpp — Security > FIDO Key: turn the Cardputer into a real FIDO2 / U2F
// hardware security key (passkeys + second factor). FOR THE OWNER'S OWN ACCOUNTS.
//
// A FIDO device must be the sole USB HID interface, so entering key mode reboots
// into a dedicated FIDO USB personality (RTC flag). Once active, the on-device
// prompt SHOWS which site is asking before you approve with ENTER — a security
// advantage of a screen+keyboard authenticator. The credential wrapping key is
// derived from the S3 eFuse (hardware) when available, so a flash dump can't
// clone the key.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
extern "C" {
#include "nucleo_fido.h"
#include "nucleo_kbd.h"
#include "nucleo_i18n.h"   // TR(it, en)
}

static const unsigned short ACC = C_BLUE, WARN = C_YELLOW, SURF2 = 0x10A2;
static bool s_key_mode = false;
enum KM { KM_STATUS, KM_MANAGE };
static int s_km = KM_STATUS;
static int s_cm_sel = 0;
static uint32_t s_last_sig = 0;   // ANTI-FLICKER: what on_draw last rendered (see on_tick/fido_sig)

// Signature of everything the status/manage screen shows. In key mode the canvas can't be acquired
// (the USB HID + CTAP2 stack leaves no room for the 32 KB buffer), so the app draws DIRECT — where the
// framework's idle-reblit hash guard does NOT apply. An unconditional 5 Hz on_tick redraw therefore
// clears-to-BG and repaints the panel five times a second = visible flicker. Gate the redraw on this
// signature so we only repaint when something actually changed (PC connect, PIN/cred edit).
static uint32_t fido_sig(void) {
    uint32_t s = 2166136261u;
    #define MIX(v) do { s = (s ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(s_key_mode ? 1 : 0); MIX(s_km); MIX(s_cm_sel);
    if (s_key_mode) {
        MIX(nucleo_fido_ready() ? 1 : 0);
        MIX(nucleo_fido_pin_is_set() ? 1 : 0);
        MIX(nucleo_fido_key_is_hardware() ? 1 : 0);
        if (s_km == KM_MANAGE) MIX(nucleo_fido_cred_count());
    }
    #undef MIX
    return s;
}

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// Modal approval prompt: drawn directly, blocks polling the keyboard while
// keeping the host alive with CTAPHID KEEPALIVE frames. rp = who is asking.
static int fido_present(const char *rp, void *ui) {
    (void)ui;
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y0 = app_ui_title("Sign-in", ACC, nullptr);
    txt(8, y0 + 8, TR5("Un sito vuole accedere:", "A site wants to sign in:", "Un sitio desea acceder:", "Un site veut acceder:", "Website benoetigt Zugriff:"), FG, BG, 1);
    char rb[40]; snprintf(rb, sizeof rb, "%.32s", (rp && rp[0]) ? rp : "?");
    txt(8, y0 + 26, rb, ACC, BG, 2);
    txt(8, h - 14, TR5("INVIO conferma   ESC nega", "ENTER approve   ESC deny", "ENTER aprob.   ESC niega", "ENTER aprob.   ESC refuse", "ENTER bestaet.   ESC ableh."), WARN, BG, 1);
    while (true) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        // This modal painted over the whole screen; force one repaint of the status UI underneath on
        // exit, since the signature-gated on_tick won't otherwise notice the screen needs restoring.
        if (k.key == NK_ENTER) { nucleo_app_request_draw(); return 1; }
        if (k.key == NK_BACK)  { nucleo_app_request_draw(); return 0; }
        nucleo_fido_keepalive();
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

// Modal PIN entry (masked). Returns the entered length; 0 = cancelled. Keeps the
// host alive while the user types. The PIN never leaves the device.
static int pin_entry(const char *title, const char *sub, char *buf, int cap) {
    int n = 0; buf[0] = 0;
    for (;;) {
        int top = nucleo_app_content_top(), h = nucleo_app_content_height();
        d.fillRect(0, top, 240, h, BG);
        int y0 = app_ui_title(title, ACC, sub);
        char mask[40]; int mi = 0; for (int i = 0; i < n && mi < 38; i++) mask[mi++] = '*'; mask[mi] = 0;
        txt(8, y0 + 16, n ? mask : "____", FG, BG, 3);
        txt(8, h - 14, TR5("INVIO ok   ESC annulla", "ENTER ok   ESC cancel", "ENTER ok   ESC cancela", "ENTER ok   ESC annule", "ENTER ok   ESC bricht"), WARN, BG, 1);
        for (;;) {
            esp_task_wdt_reset();
            nucleo_key_t k = nucleo_kbd_read();
            // Repaint the screen underneath on exit (see fido_present): the modal covered it and the
            // signature-gated on_tick won't restore it on its own.
            if (k.key == NK_ENTER) { nucleo_app_request_draw(); return n; }
            if (k.key == NK_BACK) { if (n > 0) { buf[--n] = 0; break; } nucleo_app_request_draw(); return 0; }
            if (k.key == NK_DEL)  { if (n > 0) { buf[--n] = 0; break; } }
            if (k.ch >= 32 && k.ch < 127 && n < cap - 1) { buf[n++] = k.ch; buf[n] = 0; break; }
            nucleo_fido_keepalive();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// Internal user verification: prompt for the on-device PIN, verify locally.
static int fido_verify(const char *rp, void *ui) {
    (void)ui;
    if (!nucleo_fido_pin_is_set()) return 0;
    char sub[40]; snprintf(sub, sizeof sub, TR5("per %.18s", "for %.18s", "para %.18s", "pour %.18s", "fuer %.18s"), (rp && rp[0]) ? rp : "?");
    char pin[72];
    int n = pin_entry("PIN", sub, pin, sizeof pin);
    int r = (n > 0) ? nucleo_fido_pin_check(pin) : 0;
    memset(pin, 0, sizeof pin);
    return r == 1;
}

static const nucleo_fido_ui_t UI = { fido_present, fido_verify, nullptr };

static bool poll_cb(void) { nucleo_fido_poll(); return false; }

// Back: from the passkey manager return to the status screen; from status let the
// framework close the app (which reboots out of key mode).
static bool on_back(int key) {
    (void)key;
    if (s_key_mode && s_km == KM_MANAGE) { s_km = KM_STATUS; nucleo_app_request_draw(); return true; }
    return false;
}

static void on_enter(void) {
    s_key_mode = nucleo_fido_boot_mode();
    s_km = KM_STATUS; s_cm_sel = 0;
    if (s_key_mode) {
        nucleo_fido_start(&UI);
        nucleo_app_set_poll_handler(poll_cb);
        nucleo_app_set_back_handler(on_back);
        nucleo_app_set_hint(TR5("P pin   M passkey   esc esci", "P pin   M passkeys   esc exit", "P pin   M llave   esc sal.", "P pin   M cle    esc sort", "P pin   M passkey   esc aus."));
    } else {
        nucleo_app_set_hint(TR5("invio: modo chiave (riavvia)", "enter: key mode (reboots)", "intro: modo llave (reinicia)", "entree: mode cle (redemarr.)", "EINGABE: Modus (neustart)"));
    }
    nucleo_app_request_draw();
}

static void on_exit(void) {
    if (s_key_mode) { nucleo_fido_stop(); nucleo_fido_request_mode(false); }   // reboot back to normal
}

static void on_key(int key, char ch) {
    if (!s_key_mode) { if (key == NK_ENTER) nucleo_fido_request_mode(true); return; }   // reboot into key mode
    if (s_km == KM_MANAGE) {                                                   // passkey manager
        int cnt = nucleo_fido_cred_count();
        if (key == NK_UP && s_cm_sel > 0) s_cm_sel--;
        else if (key == NK_DOWN && s_cm_sel < cnt - 1) s_cm_sel++;
        else if ((ch == 'd' || ch == 'D' || key == NK_DEL) && cnt > 0) {
            nucleo_fido_cred_delete(s_cm_sel);
            int nc = nucleo_fido_cred_count();
            if (s_cm_sel >= nc && s_cm_sel > 0) s_cm_sel--;
        }
        nucleo_app_request_draw();
        return;
    }
    if (ch == 'm' || ch == 'M') { s_km = KM_MANAGE; s_cm_sel = 0; nucleo_app_request_draw(); return; }
    if (ch == 'p' || ch == 'P') {                                             // set / change the on-device PIN
        char pin[72];
        int n = pin_entry(TR5("Nuovo PIN", "New PIN", "Nuevo PIN", "Nouveau PIN", "Neue PIN"), TR5("4-63 caratteri", "4-63 chars", "4-63 caract.", "4-63 caract.", "4-63 Zeichen"), pin, sizeof pin);
        // pin_set() returns false on a bad length (FIDO caps the PIN at 63): don't leave the user
        // believing UV is configured when it silently didn't take. Tell them via the hint bar.
        if (n >= 4 && !nucleo_fido_pin_set(pin))
            nucleo_app_set_hint(TR5("PIN non valido (4-63 caratteri)", "invalid PIN (4-63 chars)", "PIN invalido (4-63 caract.)", "PIN invalide (4-63 caract.)", "Ung. PIN (4-63 Zeichen)"));
        memset(pin, 0, sizeof pin);
        nucleo_app_request_draw();
    }
}

static void on_tick(void) {
    // Only repaint when the visible state actually changed — critical because key mode draws DIRECT
    // (no canvas / no hash guard), so a blind 5 Hz redraw would flicker. See fido_sig().
    if (fido_sig() != s_last_sig) nucleo_app_request_draw();
}

// On-device passkey manager: list resident credentials and delete them. No host
// tool, no clientPIN dance — just the device screen (Poseidon has no delete).
static void draw_manage(int h) {
    int cnt = nucleo_fido_cred_count();
    int y0 = app_ui_title("Passkey", ACC, nullptr);
    char cb[24]; snprintf(cb, sizeof cb, TR5("%d salvate", "%d stored", "%d guardadas", "%d sauvegard.", "%d gespeich."), cnt);
    txt(8, y0 + 4, cb, ACC, BG, 1);
    if (!cnt) txt(8, y0 + 22, TR5("Nessuna passkey.", "No passkeys.", "Ninguna llave.", "Aucune cle.", "Keine Passkeys."), DIM, BG, 1);
    int y = y0 + 20; fido_cred_view_t v;
    for (int i = 0; i < cnt && i < 6; i++) {
        if (!nucleo_fido_cred_get(i, &v)) break;
        bool sel = (i == s_cm_sel);
        if (sel) d.fillRect(4, y - 1, 232, 12, SURF2);
        char ln[48]; snprintf(ln, sizeof ln, "%-16.16s %.12s", v.rp[0] ? v.rp : "?", v.user);
        txt(8, y, ln, sel ? ACC : FG, sel ? SURF2 : BG, 1);
        y += 12;
    }
    txt(8, h - 14, TR5("D cancella   esc indietro", "D delete   esc back", "D elimina   esc atr.", "D efface   esc retour", "D loesch.  esc zurueck"), DIM, BG, 1);
}

static void on_draw(void) {
    s_last_sig = fido_sig();   // record what this frame renders, so on_tick only repaints on a real change
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_key_mode && s_km == KM_MANAGE) { draw_manage(h); return; }
    if (!s_key_mode) {
        int y0 = app_ui_title("FIDO Key", ACC, nullptr);
        txt(8, y0 + 10, TR5("Security key", "Security key", "Security key", "Cle de securite", "Sicherheits."), FG, BG, 2);
        txt(8, y0 + 26, TR5("FIDO2 / U2F (passkey)", "FIDO2 / U2F (passkeys)", "FIDO2 / U2F (llave)", "FIDO2 / U2F (cle)", "FIDO2 / U2F (Schluessel)"), MUTED, BG, 1);
        txt(8, y0 + 34, TR5("Il modo chiave rende FIDO l'unica", "Key mode makes FIDO the sole", "El modo llave hace FIDO unica", "Mode cle rend FIDO seule", "Schluesselmod macht FIDO allein"), MUTED, BG, 1);
        txt(8, y0 + 46, TR5("interfaccia USB (serve riavvio).", "USB interface (needs a reboot).", "interfaz USB (necesita reinicio).", "interface USB (necessite reboot).", "USB-Interface (Neustart noetig)."), MUTED, BG, 1);
        txt(8, y0 + 64, TR5("INVIO per entrare.", "ENTER to enter.", "INTRO para entrar.", "ENTREE entrer.", "EINGABE Eintritt."), WARN, BG, 1);
        return;
    }
    int y0 = app_ui_title("FIDO Key", ACC, nucleo_fido_ready() ? TR5("collegata", "connected", "conectada", "connectee", "verbunden") : TR5("attesa PC", "waiting for PC", "esperando PC", "en attente du PC", "wartet auf PC"));
    txt(8, y0 + 6, TR5("Chiave attiva", "Key active", "Llave activa", "Cle active", "Schluessel an"), FG, BG, 2);
    bool pinset = nucleo_fido_pin_is_set();
    txt(8, y0 + 30, pinset ? TR5("PIN impostato - UV attivo", "PIN set - UV on", "PIN asign. - UV on", "PIN def. - UV on", "PIN gest. - UV an")
                           : TR5("nessun PIN (P = imposta)", "no PIN (P = set)", "sin PIN (P=asign.)", "pas PIN (P=def.)", "kein PIN (P=setz.)"),
        pinset ? C_GREEN : WARN, BG, 1);
    txt(8, y0 + 44, nucleo_fido_key_is_hardware() ? TR5("chiave: eFuse hardware", "key: eFuse hardware", "llave: eFuse hardware", "cle: eFuse materiel", "Schl.: eFuse hardware")
                                                  : TR5("chiave: NVS software", "key: NVS software", "llave: NVS software", "cle: NVS logiciel", "Schl.: NVS software"),
        nucleo_fido_key_is_hardware() ? C_GREEN : MUTED, BG, 1);
    txt(8, y0 + 58, TR5("Aggiungila come security key sul sito.", "Add it as a security key on the site.", "Agreguala como security key en sitio.", "Ajoutez-le comme cle de securite.", "Fuegen Sie es als Sicherheitsschl. ein."), MUTED, BG, 1);
    txt(8, h - 14, TR5("P imposta PIN   esc esce (riavvia)", "P set PIN   esc exits (reboots)", "P asigna PIN   esc sal. (reinicia)", "P definir PIN esc quit.(redemarr.)", "P PIN setzen   esc aus. (neustart)"), DIM, BG, 1);
}

extern "C" void nucleo_register_fido(void) {
    static const nucleo_app_def_t app = {
        "fido", "FIDO Key", "Security",
        "FIDO2/U2F hardware security key (passkeys)",
        'F', 0x4D1F, on_enter, on_key, on_tick, on_draw, on_exit, 0
    };
    nucleo_app_register(&app);
}
