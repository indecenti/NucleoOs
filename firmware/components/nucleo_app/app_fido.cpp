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
#include "app_large.h"       // two screens: the key-mode status + passkey manager on the panel, Cardputer companion
#include "nucleo_touch.h"
#include "nucleo_extdisp.h"   // nucleo_extdisp_draw_internal: the Cardputer's own screen while a modal owns the panel
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
static int s_lg_scroll = 0;       // passkey list scroll on the panel (two screens)
static bool s_del_armed = false;  // panel passkeys: the picked row was tapped again -> "delete?"
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

// Two screens (defined below): the modals in full on the panel + a summary on the Cardputer.
static void present_large(const char *rp, int sel);
static void pin_large(const char *title, const char *sub, int n);
static void cardputer(const char *l1, const char *l2, const char *keys);

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// Modal approval prompt: drawn directly, blocks polling the keyboard while
// keeping the host alive with CTAPHID KEEPALIVE frames. rp = who is asking.
static int fido_present(const char *rp, void *ui) {
    (void)ui;
    if (nucleo_app_two_screens()) {                    // the panel: who asks + Approve / Deny rows (tap, tap again)
        int sel = -1;
        present_large(rp, sel);
        cardputer(TR("Un sito vuole accedere:", "A site wants to sign in:"), (rp && rp[0]) ? rp : "?",
                  TR("INVIO approva   ESC nega", "ENTER approve   ESC deny"));
        for (;;) {
            esp_task_wdt_reset();
            nucleo_key_t k = nucleo_kbd_read();
            if (k.key == NK_ENTER) { nucleo_app_request_draw(); return 1; }
            if (k.key == NK_BACK)  { nucleo_app_request_draw(); return 0; }
            if (nucleo_touch_is_row(k.key, k.ch)) {
                const int i = k.ch - NUCLEO_TOUCH_ROW;
                if (i == sel) { nucleo_app_request_draw(); return i == 0; }
                sel = i; present_large(rp, sel);
            }
            nucleo_fido_keepalive();
            vTaskDelay(pdMS_TO_TICKS(90));
        }
    }
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
        if (nucleo_app_two_screens()) {                // the panel shows it big; it is typed on the Cardputer
            pin_large(title, sub, n);
            cardputer(title, sub, TR("INVIO ok   ESC annulla", "ENTER ok   ESC cancel"));
        } else {
        int top = nucleo_app_content_top(), h = nucleo_app_content_height();
        d.fillRect(0, top, 240, h, BG);
        int y0 = app_ui_title(title, ACC, sub);
        char mask[40]; int mi = 0; for (int i = 0; i < n && mi < 38; i++) mask[mi++] = '*'; mask[mi] = 0;
        txt(8, y0 + 16, n ? mask : "____", FG, BG, 3);
        txt(8, h - 14, TR5("INVIO ok   ESC annulla", "ENTER ok   ESC cancel", "ENTER ok   ESC cancela", "ENTER ok   ESC annule", "ENTER ok   ESC bricht"), WARN, BG, 1);
        }
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

static bool lg_key(int key, char ch);
static void set_pin(void) {                                                   // set / change the on-device PIN
    char pin[72];
    int n = pin_entry(TR5("Nuovo PIN", "New PIN", "Nuevo PIN", "Nouveau PIN", "Neue PIN"), TR5("4-63 caratteri", "4-63 chars", "4-63 caract.", "4-63 caract.", "4-63 Zeichen"), pin, sizeof pin);
    // pin_set() returns false on a bad length (FIDO caps the PIN at 63): don't leave the user
    // believing UV is configured when it silently didn't take. Tell them via the hint bar.
    if (n >= 4 && !nucleo_fido_pin_set(pin))
        nucleo_app_set_hint(TR5("PIN non valido (4-63 caratteri)", "invalid PIN (4-63 chars)", "PIN invalido (4-63 caract.)", "PIN invalide (4-63 caract.)", "Ung. PIN (4-63 Zeichen)"));
    memset(pin, 0, sizeof pin);
    nucleo_app_request_draw();
}

static void on_key(int key, char ch) {
    if (lg_key(key, ch)) return;                                               // a touch on the panel's content
    if (!s_key_mode) { if (key == NK_ENTER) nucleo_fido_request_mode(true); return; }   // reboot into key mode
    if (s_km == KM_MANAGE) {                                                   // passkey manager
        int cnt = nucleo_fido_cred_count();
        s_del_armed = false;
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
    if (ch == 'p' || ch == 'P') set_pin();
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

// ===== two screens =============================================================================================
// The panel is the key's main screen, built only from the kit (lg::header, lg::settings_list, lg::setting,
// lg::message): before key mode an explanation and the "enter key mode" row; in key mode the live state as
// settings rows (tap = pick, tap again = act: set the PIN, open the passkeys); the passkeys as rows where a
// second tap arms "delete" (in red) and a third deletes. The sign-in prompt and the PIN entry take the whole
// panel too (the site in big type, Approve / Deny as rows), with a summary on the Cardputer, where the PIN is
// typed. Never a mirror.
static int s_arm_sel = -1;           // before key mode: the "enter key mode" row picked (0) or not (-1)
static int s_st_sel = -1;            // key mode status: the picked row
static int s_st_scroll = 0;
enum { ST_LINK = 0, ST_PIN, ST_KEY, ST_CREDS, ST_N };

static void st_row(int i, lg::Setting *o, void *)
{
    const bool ready = nucleo_fido_ready(), pinset = nucleo_fido_pin_is_set(), hw = nucleo_fido_key_is_hardware();
    static char cn[20];
    switch (i) {
    case ST_LINK:  o->label = TR("Collegamento", "Link");
                   o->value = ready ? TR("collegata", "connected") : TR("attesa PC", "waiting for PC");
                   o->vcol = ready ? C_GREEN : WARN; break;
    case ST_PIN:   o->label = TR("PIN - verifica utente", "PIN - user verification");
                   o->value = pinset ? TR("impostato", "set") : TR("nessuno", "none");
                   o->vcol = pinset ? C_GREEN : WARN;
                   o->sub = pinset ? TR("tocca di nuovo: cambia (sul Cardputer)", "tap again: change it (on the Cardputer)")
                                   : TR("tocca di nuovo: imposta (sul Cardputer)", "tap again: set it (on the Cardputer)"); break;
    case ST_KEY:   o->label = TR("Chiave", "Key");
                   o->value = hw ? TR("eFuse hardware", "eFuse hardware") : TR("NVS software", "NVS software");
                   o->vcol = hw ? C_GREEN : MUTED; break;
    default:       snprintf(cn, sizeof cn, TR("%d salvate", "%d stored"), nucleo_fido_cred_count());
                   o->label = TR("Passkey", "Passkeys"); o->value = cn;
                   o->sub = TR("tocca di nuovo: gestisci", "tap again: manage them"); break;
    }
}

static void cred_row(int i, lg::Setting *o, void *)
{
    static char rp[2][40], us[2][24];                 // the focused row may be drawn after another: 2 slots
    fido_cred_view_t v; const int k = i & 1;
    rp[k][0] = us[k][0] = 0;
    if (nucleo_fido_cred_get(i, &v)) { snprintf(rp[k], sizeof rp[k], "%.32s", v.rp[0] ? v.rp : "?"); snprintf(us[k], sizeof us[k], "%.20s", v.user); }
    o->label = rp[k]; o->value = us[k]; o->vcol = MUTED;
    if (i == s_cm_sel && s_del_armed) {
        o->kind = lg::ROW_DANGER;
        o->sub = TR("tocca ancora: elimina per sempre", "tap again: delete it for good");
    }
}

static void draw_large(void) {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();

    if (!s_key_mode) {                                                   // before key mode
        const int y = lg::header("FIDO Key", nullptr, ACC);
        d.fillRect(0, y, W, H - y, BG);
        const int rh = 52, ry = H - rh - 10, cy = y + 10;
        lg::panel(8, cy, W - 16, ry - cy - 10);
        const unsigned short cb = lg::card();
        lg::font(2); d.setTextColor(ACC, cb); d.drawString(TR("Security key", "Security key"), 22, cy + 12);
        lg::font(0); d.setTextColor(MUTED, cb);
        lg::wrap(TR("FIDO2 / U2F passkey. Il modo chiave rende FIDO l'unica interfaccia USB: serve un riavvio. Il sito chiede, tu approvi qui.",
                    "FIDO2 / U2F passkeys. Key mode makes FIDO the sole USB interface, so it reboots. A site asks, you approve here."),
                 22, cy + 42, W - 44, 18, 5);
        lg::Setting s = { TR("Entra in modo chiave", "Enter key mode"), TR("tocca di nuovo: riavvia in modo FIDO", "tap again: reboot into FIDO mode"),
                          nullptr, lg::ROW_NAV, -1, -1, false, false, 0, 0, 0 };
        lg::setting(8, ry, W - 16, rh, s, s_arm_sel == 0, ACC);
        nucleo_touch_add(8, ry, W - 16, rh, NK_CHAR, (char)NUCLEO_TOUCH_ROW);
        nucleo_touch_commit();
        return;
    }

    if (s_km == KM_MANAGE) {                                             // passkeys
        const int cnt = nucleo_fido_cred_count();
        char rb[16]; snprintf(rb, sizeof rb, TR("%d salvate", "%d stored"), cnt);
        const int y = lg::header("Passkey", rb, ACC, NK_BACK);
        d.fillRect(0, y, W, H - y, BG);
        if (cnt == 0) lg::message(16, y + 8, W - 32, H - y - 16, TR("Nessuna passkey", "No passkeys"),
                                  TR("Le passkey residenti compaiono qui", "Resident passkeys show up here"), MUTED);
        else lg::settings_list(4, y + 6, W - 8, H - y - 10, 44, cnt, s_cm_sel, &s_lg_scroll, cred_row, nullptr, ACC);
        nucleo_touch_commit();
        return;
    }

    const bool ready = nucleo_fido_ready();                              // status
    const int y = lg::header("FIDO Key", ready ? TR("collegata", "connected") : TR("attesa PC", "waiting for PC"), ACC);
    d.fillRect(0, y, W, H - y, BG);
    lg::settings_list(4, y + 6, W - 8, ST_N * 44, 44, ST_N, s_st_sel, &s_st_scroll, st_row, nullptr, ACC);
    lg::font(0); d.setTextColor(MUTED, BG); d.setTextDatum(top_center);
    d.drawString(TR("Aggiungila come security key sul sito.", "Add it as a security key on the site."), W / 2, y + 14 + ST_N * 44);
    d.setTextDatum(top_left);
    nucleo_touch_commit();
}

// A touch on the panel: true when handled (row targets; keyboard keys fall through to on_key).
static void set_pin(void);
static bool lg_key(int key, char ch) {
    if (!nucleo_touch_is_row(key, ch)) return false;
    if (!s_key_mode) {
        if (s_arm_sel == 0) nucleo_fido_request_mode(true);                // reboots into key mode
        else { s_arm_sel = 0; nucleo_app_request_draw(); }
        return true;
    }
    if (s_km == KM_MANAGE) {
        const int cnt = nucleo_fido_cred_count(), was = s_cm_sel;
        const int r = lg::row_tap(key, ch, s_lg_scroll, cnt, &s_cm_sel);
        if (r == 1 && s_cm_sel != was) s_del_armed = false;                 // a new row: just pick it
        else if (r == 2 && !s_del_armed) s_del_armed = true;                // tapped again: arm the delete
        else if (r == 2) {                                                   // tapped a third time: delete
            nucleo_fido_cred_delete(s_cm_sel);
            const int nc = nucleo_fido_cred_count();
            if (s_cm_sel >= nc && s_cm_sel > 0) s_cm_sel--;
            s_del_armed = false;
        }
        nucleo_app_request_draw();
        return true;
    }
    if (lg::row_tap(key, ch, s_st_scroll, ST_N, &s_st_sel) == 2) {          // tapped again: act
        if (s_st_sel == ST_PIN) set_pin();
        else if (s_st_sel == ST_CREDS) { s_km = KM_MANAGE; s_cm_sel = 0; s_del_armed = false; }
    }
    nucleo_app_request_draw();
    return true;
}

// ---- the modals on two screens: the panel shows the request in full, the Cardputer a summary ----
static const char *s_cp_l1, *s_cp_l2, *s_cp_keys;                      // what the Cardputer shows meanwhile
static void cp_draw(void *gfx) {
    LovyanGFX *g = (LovyanGFX *)gfx;
    g->fillRect(0, 0, 240, 135, BG);
    g->setFont(&fonts::Font0); g->setTextSize(1); g->setTextDatum(top_left); g->setTextColor(ACC, BG);
    g->drawString(TR("FIDO  -  sul secondo schermo", "FIDO  -  on the second screen"), 8, 6);
    g->setFont(&fonts::FreeSansBold9pt7b); g->setTextColor(FG, BG);
    if (s_cp_l1) g->drawString(s_cp_l1, 8, 30);
    g->setTextColor(ACC, BG);
    if (s_cp_l2) { char b[28]; snprintf(b, sizeof b, "%.24s", s_cp_l2); g->drawString(b, 8, 58); }
    g->setFont(&fonts::Font0); g->setTextColor(WARN, BG);
    if (s_cp_keys) g->drawString(s_cp_keys, 8, 118);
}
static void cardputer(const char *l1, const char *l2, const char *keys) {
    s_cp_l1 = l1; s_cp_l2 = l2; s_cp_keys = keys;
    nucleo_extdisp_draw_internal(cp_draw);
}

// The sign-in request on the panel: who is asking, then Approve / Deny as rows (tap = pick, tap again = do it).
static void present_large(const char *rp, int sel) {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    const int y = lg::header(TR("Accesso", "Sign-in"), nullptr, ACC, 0);
    d.fillRect(0, y, W, H - y, BG);
    lg::panel(8, y + 8, W - 16, 86);
    const unsigned short cb = lg::card();
    lg::font(0); d.setTextColor(MUTED, cb); d.drawString(TR("Un sito vuole accedere:", "A site wants to sign in:"), 22, y + 20);
    char rb[40]; lg::font(2); lg::fit(rb, sizeof rb, (rp && rp[0]) ? rp : "?", W - 44);
    d.setTextColor(ACC, cb); d.drawString(rb, 22, y + 46);
    lg::Setting ok = { TR("Approva", "Approve"), TR("tocca di nuovo per approvare", "tap again to approve"), nullptr, lg::ROW_NAV, -1, -1, false, false, 0, C_GREEN, 0 };
    lg::Setting no = { TR("Nega", "Deny"), TR("tocca di nuovo per negare", "tap again to deny"), nullptr, lg::ROW_DANGER, -1, -1, false, false, 0, 0, 0 };
    const int ry = y + 104;
    lg::setting(8, ry, W - 16, 50, ok, sel == 0, C_GREEN);
    lg::setting(8, ry + 52, W - 16, 50, no, sel == 1, C_RED);
    nucleo_touch_add(8, ry, W - 16, 48, NK_CHAR, (char)NUCLEO_TOUCH_ROW);
    nucleo_touch_add(8, ry + 52, W - 16, 48, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + 1));
    nucleo_touch_commit();
}

// The PIN entry on the panel: the request and the masked PIN in big type; it is typed on the Cardputer.
static void pin_large(const char *title, const char *sub, int n) {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    const int y = lg::header(title, sub, ACC, 0);
    d.fillRect(0, y, W, H - y, BG);
    lg::panel(8, y + 20, W - 16, 90);
    const unsigned short cb = lg::card();
    char mask[24]; int m = 0; for (int i = 0; i < n && m < 20; i++) mask[m++] = '*'; mask[m] = 0;
    lg::font(3); d.setTextColor(n ? FG : DIM, cb); d.setTextDatum(middle_center);
    d.drawString(n ? mask : "____", W / 2, y + 65);
    lg::font(0); d.setTextColor(MUTED, BG);
    d.drawString(TR("Digita il PIN sul Cardputer", "Type the PIN on the Cardputer"), W / 2, y + 140);
    d.setTextColor(WARN, BG);
    d.drawString(TR("INVIO ok   ESC annulla", "ENTER ok   ESC cancel"), W / 2, y + 166);
    d.setTextDatum(top_left);
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("FIDO", ACC);
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
    if (!s_key_mode) {
        lg::wrap(TR("Security key FIDO2 / U2F", "FIDO2 / U2F security key"), 8, y + 4, 224, 20, 2);
        lg::comp_keys(TR("INVIO modo chiave (riavvia)  ESC esci", "ENTER key mode (reboots)  ESC back"));
        return;
    }
    if (s_km == KM_MANAGE) {
        int cnt = nucleo_fido_cred_count();
        char b[32]; snprintf(b, sizeof b, TR("%d passkey salvate", "%d passkeys stored"), cnt);
        d.drawString(b, 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        if (cnt) { fido_cred_view_t v; if (nucleo_fido_cred_get(s_cm_sel, &v)) { char ln[48]; snprintf(ln, sizeof ln, "> %.20s / %.16s", v.rp, v.user); d.drawString(ln, 8, y + 26); } }
        lg::comp_keys(TR("D cancella   ESC indietro", "D delete   ESC back"));
        return;
    }
    const bool ready = nucleo_fido_ready(), pinset = nucleo_fido_pin_is_set();
    d.setTextColor(ready ? C_GREEN : WARN, BG);
    d.drawString(ready ? TR("Chiave collegata", "Key connected") : TR("Attesa del PC", "Waiting for PC"), 8, y + 4);
    d.setFont(&fonts::Font0); d.setTextColor(pinset ? C_GREEN : WARN, BG);
    d.drawString(pinset ? TR("PIN impostato - UV attivo", "PIN set - UV on") : TR("nessun PIN (P imposta)", "no PIN (P sets)"), 8, y + 26);
    lg::comp_keys(TR("P pin  M passkey  ESC esci", "P pin  M passkeys  ESC exit"));
}

static void on_draw(void) {
    s_last_sig = fido_sig();   // record what this frame renders, so on_tick only repaints on a real change
    if (nucleo_app_two_screens()) { draw_companion(); return; }
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
        'F', 0x4D1F, on_enter, on_key, on_tick, on_draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
