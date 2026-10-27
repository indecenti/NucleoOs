// WiFi Sniffer app — on-device UI for the promiscuous packet capture engine (nucleo_wifiatk sniffer_*).
//
// AUTHORIZED USE ONLY. Captures 802.11 frames off the air to a standard .pcap on the SD card (open in
// Wireshark / hcxpcapngtool). For security-awareness demos, auditing networks you own or have written
// permission to test, and CTF/lab work. Opens on a consent screen; will not start until acknowledged.
//
// Modes: Tutti / Beacon / Probe / Handshake (EAPOL) / Deauth. Channel: Hop the band or a fixed 1..13.
#include "nucleo_app.h"
#include "app_ui.h"
#include "nucleo_i18n.h"      // TR(it,en): hints follow the system language
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
int         nucleo_wifiatk_sniffer_start(int mode, int channel);
void        nucleo_wifiatk_sniffer_stop(void);
bool        nucleo_wifiatk_sniffer_running(void);
unsigned    nucleo_wifiatk_sniffer_pkts(void);
unsigned    nucleo_wifiatk_sniffer_drops(void);
int         nucleo_wifiatk_sniffer_channel(void);
const char *nucleo_wifiatk_sniffer_path(void);
unsigned    nucleo_wifiatk_sniffer_beacons(void);
unsigned    nucleo_wifiatk_sniffer_probes(void);
unsigned    nucleo_wifiatk_sniffer_data(void);
unsigned    nucleo_wifiatk_sniffer_eapol(void);
unsigned    nucleo_wifiatk_sniffer_deauth(void);
}

#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "app_gfx.h"
#include "app_large.h"        // two screens: the capture laid out on the 320x240 panel
#include "nucleo_touch.h"

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short SNF = 0x07FF /*cyan*/, GRN = C_GREEN, YEL = C_YELLOW, ERR = 0xF800 /*error text*/;

enum { ST_CONSENT, ST_CONFIG, ST_RUNNING, ST_ARMING, ST_STOPPING };
enum { R_MODE, R_CHAN, R_GO, R_NROWS };
static const char *MODE_NAME_IT[] = { "Tutti", "Beacon", "Probe", "Handshake", "Deauth" };
static const char *MODE_NAME_EN[] = { "All", "Beacon", "Probe", "Handshake", "Deauth" };
#define MODE_NAME (nucleo_i18n_is_it() ? MODE_NAME_IT : MODE_NAME_EN)
#define N_MODE 5

static int  s_state, s_row, s_mode, s_chan;   // s_chan: 0 = hop, 1..13 fixed
static bool s_consented, s_arm_armed, s_stop_armed;
static char s_err[40];
static uint32_t s_last;
static bool s_pulse;

static void set_hint(void)
{
    if (s_state == ST_CONSENT)       nucleo_app_set_hint(TR5("invio accetto   esc esci", "enter accept   esc back", "intro acepta   esc atras", "entrer accepter   esc ret", "enter akzept   esc zurueck"));
    else if (s_state == ST_ARMING)   nucleo_app_set_hint(TR5("avvio...", "starting...", "iniciando...", "demarrage...", "start..."));
    else if (s_state == ST_STOPPING) nucleo_app_set_hint(TR5("salvo e chiudo...", "saving & closing...", "guardo y cierro...", "enregistre & ferme...", "speicher & schliess..."));
    else if (s_state == ST_RUNNING)  nucleo_app_set_hint(TR5("invio ferma e salva", "enter stop & save", "intro para y guarda", "entree stop & enreg", "enter stopp+sichern"));
    else                             nucleo_app_set_hint(TR5("su/giu   > cambia   invio avvia", "up/dn   > change   enter start", "ar/ab   > cambia   intro inicia", "haut/bas   > change   entrer dema", "auf/ab   > aendern   enter start"));
}

static void enter(void)
{
    s_err[0] = 0;
    if (nucleo_wifiatk_sniffer_running()) s_state = ST_RUNNING;
    else if (s_consented)                 s_state = ST_CONFIG;
    else                                  s_state = ST_CONSENT;
    set_hint(); nucleo_app_request_draw();
}

// The capture queue needs ~6KB CONTIGUOUS; on the ADV the live heap leaves only ~3KB free at app
// open, so free the 32KB launcher canvas first (same trick KARMA/Recorder use) — the running screen
// draws direct meanwhile. Restored on stop / start-failure.
static bool s_screen_freed;
static void free_screen(void)
{
    if (s_screen_freed) return;
    nucleo_app_set_direct_draw(true);
    nucleo_screen_release();
    s_screen_freed = true;
}
static void restore_screen(void)
{
    if (!s_screen_freed) return;
    s_screen_freed = false;
    nucleo_app_set_direct_draw(false);
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_request_draw();
}

static void tick(void)
{
    if (s_state == ST_ARMING) {
        if (!s_arm_armed) { s_arm_armed = true; return; }   // first tick arms (draw also does): busy screen paints first
        free_screen();                       // +32KB contiguous BEFORE the queue allocs
        int rc = nucleo_wifiatk_sniffer_start(s_mode, s_chan);
        if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; }
        else { restore_screen(); snprintf(s_err, sizeof s_err, TR("Avvio fallito (%d)","Start failed (%d)"), rc); s_state = ST_CONFIG; }
        set_hint(); nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) { s_stop_armed = true; return; }
        nucleo_wifiatk_sniffer_stop();
        restore_screen();
        nucleo_app_exit();
        return;
    }
    if (s_state != ST_RUNNING) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last) { s_last = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
}

static void on_key(int key, char ch)
{
    (void)ch;
    if (s_state == ST_CONSENT) {
        if (key == NK_ENTER) { s_consented = true; s_state = ST_CONFIG; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    if (s_state == ST_ARMING || s_state == ST_STOPPING) return;
    if (s_state == ST_RUNNING) {
        if (key == NK_ENTER) { s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    // ST_CONFIG
    if (nucleo_touch_is_row(key, ch)) {        // panel: tap a row to cycle it, or GO to arm
        int i = ch - NUCLEO_TOUCH_ROW; if (i >= R_NROWS) return; s_row = i;
        if (i == R_GO) { s_state = ST_ARMING; s_arm_armed = false; set_hint(); nucleo_app_request_draw(); return; }
        if (i == R_MODE) s_mode = (s_mode + 1) % N_MODE; else if (i == R_CHAN) s_chan = (s_chan + 1) % 14;
        nucleo_app_request_draw(); return;
    }
    if (key == NK_UP)        s_row = (s_row + R_NROWS - 1) % R_NROWS;
    else if (key == NK_DOWN) s_row = (s_row + 1) % R_NROWS;
    else if (key == NK_RIGHT) {
        if (s_row == R_MODE) s_mode = (s_mode + 1) % N_MODE;
        else if (s_row == R_CHAN) s_chan = (s_chan + 1) % 14;   // 0=hop, 1..13
    } else if (key == NK_ENTER) {
        if (s_row == R_GO) { s_state = ST_ARMING; s_arm_armed = false; set_hint(); nucleo_app_request_draw(); return; }
        if (s_row == R_MODE) s_mode = (s_mode + 1) % N_MODE;
        else if (s_row == R_CHAN) s_chan = (s_chan + 1) % 14;
    } else return;
    nucleo_app_request_draw();
}

// ---- drawing ----------------------------------------------------------------
static void row(int y, bool focus, const char *label, const char *val)
{
    if (focus) {
        d.fillRoundRect(6, y, 228, 22, 6, SNF);
        d.setTextSize(2); d.setTextColor(INK, SNF); d.setCursor(12, y + 4); d.print(label);
        int w = (int)strlen(val) * 6; d.setTextSize(1); d.setTextColor(INK, SNF);
        d.setCursor(226 - w, y + 13); d.print(val);
    } else {
        d.fillCircle(13, y + 11, 3, SNF);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 3); d.print(label);
        int w = (int)strlen(val) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 13); d.print(val);
    }
}

static void draw_consent(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, "AUTH");
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *L[] = { TR("Cattura i pacchetti WiFi","Captures WiFi packets"), TR("in aria e li salva in .pcap","over the air into a .pcap"), TR("su SD (Wireshark/hashcat).","on SD (Wireshark/hashcat)."),
                        "", TR("Solo reti tue o con permesso.","Only your own networks or with permission.") };
    unsigned short C[] = { FG, FG, FG, BG, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 5; i++) { d.setTextColor(C[i], BG); d.setCursor(10, 52 + i * 11); d.print(L[i]); }
    if (s_err[0]) { d.setTextColor(ERR, BG); d.setCursor(10, h - 10); d.print(s_err); }
}

static void draw_config(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, s_err[0] ? "ERR" : "");
    char cv[12]; if (s_chan == 0) snprintf(cv, sizeof cv, "%s", TR("Hop banda","Band hop")); else snprintf(cv, sizeof cv, TR("Canale %d","Channel %d"), s_chan);
    row(24, s_row == R_MODE, TR("Modo","Mode"), MODE_NAME[s_mode]);
    row(50, s_row == R_CHAN, TR("Canale","Channel"), cv);
    bool go = (s_row == R_GO);
    if (go) { d.fillRoundRect(6, 76, 228, 22, 6, GRN); d.setTextSize(2); d.setTextColor(INK, GRN); d.setCursor(88, 80); d.print(TR("AVVIA","START")); }
    else    { d.drawRoundRect(6, 76, 228, 22, 6, GRN); d.setTextSize(2); d.setTextColor(GRN, BG); d.setCursor(88, 80); d.print(TR("AVVIA","START")); }
    // Error / hint line, kept INSIDE the content area (above the footer).
    d.setTextSize(1);
    if (s_err[0]) { d.setTextColor(ERR, BG); d.setCursor(10, h - 10); d.print(s_err); }
    else          { d.setTextColor(DIM, BG);    d.setCursor(10, h - 10); d.print(TR("salva in /sd/sniffer/*.pcap","saves to /sd/sniffer/*.pcap")); }
}

static void draw_busy(const char *t, const char *m)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print(t);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6); d.print(m);
}

static void draw_running(void)
{
    int h = nucleo_app_content_height();
    // No mode in the title-right here: the live channel readout below owns that top-right slot, and a
    // longer mode name ("Handshake") drawn under a short channel ("ch 6") left a garbled overlap. The
    // capture mode is shown on the config screen where it's chosen.
    app_ui_title("WiFi Sniffer", SNF, "");
    if (s_pulse) d.fillCircle(150, 9, 4, SNF);
    d.setTextSize(1); d.setTextColor(SNF, BG); d.setCursor(160, 7); d.print("REC");
    int cc = nucleo_wifiatk_sniffer_channel();
    char cb[10]; snprintf(cb, sizeof cb, s_chan == 0 ? "hop c%d" : "ch %d", cc);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(cb) * 6, 7); d.print(cb);

    unsigned pk = nucleo_wifiatk_sniffer_pkts(), dr = nucleo_wifiatk_sniffer_drops();
    char ps[16]; snprintf(ps, sizeof ps, "%u", pk);
    // The "pacchetti" label trails the count, so when the number gains a digit its old x moves and
    // (on the DIRECT path with the once-only band clear) would leave a stale label behind. Wipe the
    // count+label box only on that rare digit-count change — imperceptible, and residue-proof.
    static int s_pslen = -1; int pl = (int)strlen(ps);
    if (pl != s_pslen) { s_pslen = pl; d.fillRect(10, 32, 130, 28, BG); }
    d.setTextSize(3); d.setTextColor(FG, BG); d.setCursor(10, 34); d.print(ps);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(12 + (int)strlen(ps) * 18, 48); d.print("pacchetti");

    char ds[20]; snprintf(ds, sizeof ds, "persi %u", dr);
    d.setTextColor(dr ? YEL : DIM, BG); d.setCursor(150, 40); d.print(ds);

    // Live breakdown: beacons / probes / data + EAPOL + deauth. EAPOL highlighted (crackable material).
    unsigned ea = nucleo_wifiatk_sniffer_eapol();
    char bd[44]; snprintf(bd, sizeof bd, "B%u P%u D%u X%u", nucleo_wifiatk_sniffer_beacons(),
                          nucleo_wifiatk_sniffer_probes(), nucleo_wifiatk_sniffer_data(),
                          nucleo_wifiatk_sniffer_deauth());
    d.setTextColor(MUTED, BG); d.setCursor(10, 64); d.print(bd);
    if (ea) { char es[16]; snprintf(es, sizeof es, "HS %u", ea);
              d.setTextColor(GRN, BG); d.setCursor(170, 64); d.print(es); }

    d.drawFastHLine(10, 78, 220, LINE);
    const char *p = nucleo_wifiatk_sniffer_path(); const char *base = p;
    for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
    d.setTextColor(SNF, BG); d.setCursor(10, 86); d.print(base[0] ? base : "...");
    d.setTextColor(YEL, BG); d.setCursor(10, h - 9); d.print(TR("invio: ferma  (B/P/D/X/HS = tipi)","enter: stop  (B/P/D/X/HS = types)"));
}

// ===== two screens ==============================================================================================
// The panel carries the capture: the consent card with Accept/Back buttons, the config as tappable rows (Mode,
// Channel) plus a big START button, and the running capture as a big packet counter with the per-type breakdown
// (EAPOL highlighted — crackable material), the drop count and the .pcap name, plus a STOP button. The Cardputer
// becomes the companion: the packet count big, the breakdown, and the key hints.
static void lg_cfg_row(int y, int w, const char *label, const char *val, bool focus, int slot) {
    const int x = 16;
    d.fillRoundRect(x, y, w, 36, 9, focus ? SNF : lg::card());
    lg::font(1); d.setTextColor(focus ? INK : FG, focus ? SNF : lg::card()); d.setTextDatum(middle_left);
    d.drawString(label, x + 14, y + 18);
    d.setTextColor(focus ? INK : SNF, focus ? SNF : lg::card()); d.setTextDatum(middle_right);
    d.drawString(val, x + w - 14, y + 18); d.setTextDatum(top_left);
    nucleo_touch_add(x, y, w, 36, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + slot));
}

static void draw_large(void) {
    const int Wp = d.width(), Hp = d.height();
    nucleo_touch_clear();
    if (s_state == ST_CONSENT) {
        int y = lg::header("WiFi Sniffer", "AUTH", SNF, NK_BACK);
        d.fillRect(0, y, Wp, Hp - y, BG);
        lg::panel(12, y + 8, Wp - 24, Hp - y - 70);
        const unsigned short cb = lg::card();
        lg::font(1); d.setTextColor(YEL, cb); d.drawString(TR("Test autorizzati", "Authorized testing"), 24, y + 18);
        lg::font(0); d.setTextColor(FG, cb);
        lg::wrap(TR("Cattura i pacchetti Wi-Fi in aria e li salva in .pcap su SD (Wireshark / hashcat).",
                    "Captures Wi-Fi packets over the air into a .pcap on SD (Wireshark / hashcat)."), 24, y + 44, Wp - 48, 19, 3);
        d.setTextColor(MUTED, cb);
        lg::wrap(TR("Solo reti tue o con permesso.", "Only your own networks or with permission."), 24, y + 108, Wp - 48, 19, 1);
        const int bw = (Wp - 36) / 2;
        lg::button(12, Hp - 54, bw, 42, TR("Indietro", "Back"), lg::card(), FG, NK_BACK, 0);
        lg::button(24 + bw, Hp - 54, bw, 42, TR("Accetto", "Accept"), SNF, INK, NK_ENTER, 0);
        nucleo_touch_commit(); return;
    }
    if (s_state == ST_ARMING || s_state == ST_STOPPING) {
        lg::header("WiFi Sniffer", nullptr, SNF, NK_BACK);
        d.fillRect(0, lg::HEAD, Wp, Hp - lg::HEAD, BG);
        lg::message(0, lg::HEAD, Wp, Hp - lg::HEAD,
                    s_state == ST_ARMING ? TR("Avvio...", "Starting...") : TR("Salvo...", "Saving..."),
                    s_state == ST_ARMING ? TR("Apro la cattura.", "Opening the capture.") : TR("Chiudo il .pcap.", "Closing the .pcap."), SNF);
        if (s_state == ST_ARMING) s_arm_armed = true; else s_stop_armed = true;
        nucleo_touch_commit(); return;
    }
    if (s_state == ST_RUNNING) {
        char cb2[12]; int cc = nucleo_wifiatk_sniffer_channel();
        snprintf(cb2, sizeof cb2, s_chan == 0 ? "hop c%d" : "ch %d", cc);
        int y = lg::header("WiFi Sniffer", cb2, SNF, NK_BACK);
        d.fillRect(0, y, Wp, Hp - y, BG);
        unsigned pk = nucleo_wifiatk_sniffer_pkts(), dr = nucleo_wifiatk_sniffer_drops();
        lg::panel(12, y + 2, Wp - 24, 60);
        const unsigned short cbg = lg::card();
        char ps[16]; snprintf(ps, sizeof ps, "%u", pk);
        lg::font(3); d.setTextColor(FG, cbg); d.setTextDatum(middle_left); d.drawString(ps, 26, y + 30); d.setTextDatum(top_left);
        lg::font(0); d.setTextColor(MUTED, cbg); d.drawString(TR("pacchetti", "packets"), 26, y + 46);
        char ds[20]; snprintf(ds, sizeof ds, TR("persi %u", "dropped %u"), dr);
        d.setTextColor(dr ? YEL : DIM, cbg); d.setTextDatum(top_right); d.drawString(ds, Wp - 24, y + 14); d.setTextDatum(top_left);
        // per-type breakdown, two columns; EAPOL on its own highlighted row (crackable material)
        unsigned ea = nucleo_wifiatk_sniffer_eapol();
        const int gx = 16, gw = (Wp - 32 - 12) / 2; char v[12];
        snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_beacons()); lg::kv(gx, y + 76, gw, "Beacon", v, SNF, BG);
        snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_data());    lg::kv(gx + gw + 12, y + 76, gw, "Data", v, SNF, BG);
        snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_probes());  lg::kv(gx, y + 100, gw, "Probe", v, SNF, BG);
        snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_deauth());  lg::kv(gx + gw + 12, y + 100, gw, "Deauth", v, YEL, BG);
        snprintf(v, sizeof v, "%u", ea); lg::kv(gx, y + 124, Wp - 32, TR("EAPOL (handshake)", "EAPOL (handshake)"), v, ea ? GRN : MUTED, BG);
        lg::font(0); d.setTextColor(SNF, BG);
        const char *p = nucleo_wifiatk_sniffer_path(); const char *base = p; for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
        d.drawString(base[0] ? base : "...", gx, y + 150);
        lg::button(40, Hp - 46, Wp - 80, 38, TR("FERMA e salva", "STOP and save"), C_RED, INK, NK_ENTER, 0);
        nucleo_touch_commit(); return;
    }
    // ST_CONFIG
    int y = lg::header("WiFi Sniffer", s_err[0] ? "ERR" : nullptr, SNF, NK_BACK);
    d.fillRect(0, y, Wp, Hp - y, BG);
    char cv[16]; if (s_chan == 0) snprintf(cv, sizeof cv, "%s", TR("Hop banda", "Band hop")); else snprintf(cv, sizeof cv, TR("Canale %d", "Channel %d"), s_chan);
    lg_cfg_row(y + 14, Wp - 32, TR("Modo", "Mode"), MODE_NAME[s_mode], s_row == R_MODE, R_MODE);
    lg_cfg_row(y + 58, Wp - 32, TR("Canale", "Channel"), cv, s_row == R_CHAN, R_CHAN);
    const bool go = (s_row == R_GO);
    d.fillRoundRect(16, y + 108, Wp - 32, 48, 11, go ? GRN : lg::card());
    lg::font(2); d.setTextColor(go ? INK : GRN, go ? GRN : lg::card()); d.setTextDatum(middle_center);
    d.drawString(TR("AVVIA", "START"), Wp / 2, y + 132); d.setTextDatum(top_left);
    nucleo_touch_add(16, y + 108, Wp - 32, 48, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + R_GO));
    lg::font(0); d.setTextColor(s_err[0] ? ERR : DIM, BG);
    d.setTextDatum(bottom_left); d.drawString(s_err[0] ? s_err : TR("salva in /sd/sniffer/*.pcap", "saves to /sd/sniffer/*.pcap"), 16, Hp - 6); d.setTextDatum(top_left);
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("SNIFFER", SNF);
    if (s_state == ST_RUNNING) {
        d.setFont(&fonts::FreeSansBold18pt7b); d.setTextColor(FG, BG);
        char ps[12]; snprintf(ps, sizeof ps, "%u", nucleo_wifiatk_sniffer_pkts()); d.drawString(ps, 8, y + 2);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(TR("pacchetti", "packets"), 8, y + 34);
        unsigned ea = nucleo_wifiatk_sniffer_eapol();
        char bd[44]; snprintf(bd, sizeof bd, "B%u P%u D%u X%u", nucleo_wifiatk_sniffer_beacons(), nucleo_wifiatk_sniffer_probes(),
                              nucleo_wifiatk_sniffer_data(), nucleo_wifiatk_sniffer_deauth());
        d.setTextColor(SNF, BG); d.drawString(bd, 8, y + 48);
        if (ea) { char es[16]; snprintf(es, sizeof es, "HS %u", ea); d.setTextColor(GRN, BG); d.drawString(es, 180, y + 48); }
        lg::comp_keys(TR("INVIO ferma e salva  ESC esci", "ENTER stop & save  ESC back")); return;
    }
    if (s_state == ST_CONSENT || s_state == ST_ARMING || s_state == ST_STOPPING) {
        d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(SNF, BG);
        d.drawString(s_state == ST_CONSENT ? TR("Autorizza", "Authorize") : s_state == ST_ARMING ? TR("Avvio...", "Starting...") : TR("Salvo...", "Saving..."), 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        lg::wrap(TR("Accetta sul pannello per configurare.", "Accept on the panel to configure."), 8, y + 34, 224, 12, 2);
        lg::comp_keys(TR("INVIO accetto  ESC esci", "ENTER accept  ESC back")); return;
    }
    // CONFIG
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(SNF, BG); d.drawString(TR("Configura", "Configure"), 8, y + 4);
    d.setFont(&fonts::Font0); d.setTextColor(FG, BG);
    char ln[40]; char cv[16]; if (s_chan == 0) snprintf(cv, sizeof cv, TR("hop", "hop")); else snprintf(cv, sizeof cv, "ch%d", s_chan);
    snprintf(ln, sizeof ln, "%s  -  %s", MODE_NAME[s_mode], cv);
    d.drawString(ln, 8, y + 34);
    lg::comp_keys(TR(";/. riga  > cambia  INVIO avvia", ";/. row  > change  ENTER start"));
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    // RUNNING is on the DIRECT path (the canvas was freed for the sniffer queues) and repaints ~1 Hz
    // (pulse dot + live counters). A full-content fillRect on that cadence blinks the whole panel
    // (ANTI-FLICKER.md technique 2). Clear the band ONCE on entry (or when an overlay bumps the
    // repaint gen); draw_running() then repaints only its own fields — the title band self-clears, the
    // counters are opaque + monotonic, and the .pcap path is fixed for the session. All other states
    // are buffered / event-driven, so a per-draw full clear is free there.
    bool running = (s_state == ST_RUNNING);
    static bool s_run_painted = false;
    static unsigned s_gen = 0;
    unsigned gen = nucleo_app_repaint_gen();
    if (!running || nucleo_app_is_buffered() || !s_run_painted || gen != s_gen)
        d.fillRect(0, top, 240, h, BG);
    s_gen = gen; s_run_painted = running;
    if (s_state == ST_CONSENT)       draw_consent();
    else if (s_state == ST_ARMING)   { draw_busy(TR("Avvio...","Starting..."), TR("Apro la cattura.","Opening the capture.")); s_arm_armed = true; }
    else if (s_state == ST_STOPPING) { draw_busy(TR("Salvo...","Saving..."), TR("Chiudo il .pcap e ripristino rete.","Closing the .pcap and restoring network.")); s_stop_armed = true; }
    else if (s_state == ST_RUNNING)  draw_running();
    else                             draw_config();
}

static void leave(void) { }   // sniffer keeps running in the background if you leave; reopen to stop

extern "C" void nucleo_register_sniffer(void)
{
    static const nucleo_app_def_t app = {
        "sniffer", "WiFi Sniffer", "Security", "Promiscuous 802.11 capture to .pcap for authorized testing",
        'P', SNF, enter, on_key, tick, draw, leave, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
