// Evil Portal app — the on-device UI for the authorized captive-portal engine (nucleo_evilportal).
//
// AUTHORIZED USE ONLY. This drives a rogue-AP credential-capture harness of the same kind Bruce
// and Marauder ship. It is for security-awareness demos, auditing networks you own or have
// written permission to test, and CTF/lab work. The app opens on a consent screen and will not
// arm until the operator acknowledges it. Capturing third parties' credentials without
// authorization is illegal in most jurisdictions.
//
// Controls (the launcher eats Left/Back to leave the app, so the UI uses Up/Down/Right/Enter):
//   Consent : Enter = accept, Esc = leave
//   Config  : Up/Down pick a field, Right changes it, type to set a custom SSID, Del backspaces,
//             Enter arms the portal
//   Running : Enter stops the portal (Esc also stops, via on_exit)
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_timer.h"

extern "C" {
// Engine API. Declared here (not via REQUIRES) so nucleo_app doesn't gain a dependency on the
// engine — the symbols resolve at final link because main pulls nucleo_evilportal in. Same
// no-cycle arrangement app_anima.cpp uses for the setup/anima getters.
int         nucleo_evilportal_template_count(void);
const char *nucleo_evilportal_template_name(int idx);
int         nucleo_evilportal_ssid_count(void);
const char *nucleo_evilportal_ssid_preset(int idx);
int         nucleo_evilportal_start(const char *ssid, int template_idx);   // esp_err_t (0 == ok)
void        nucleo_evilportal_stop(void);
bool        nucleo_evilportal_running(void);
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
// Evil-twin (clone a real AP + deauth it). Same no-cycle link arrangement as above.
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
}

#include "app_gfx.h"

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            LINE = 0x2945, INK = 0x0000, EP_RED = 0xF96B, GRN = 0x8FF3,
                            YEL = 0xFE8C, PANEL = 0x18E3, EP_CYAN = 0x5C9F;

enum { ST_CONSENT, ST_CONFIG, ST_SCAN, ST_RUNNING, ST_STOPPING };
#define CFG_ROWS 4                        // Modo, SSID/Rete, Pagina, AVVIA
static int  s_state;
static bool s_stop_armed;                // set once the "Arresto..." screen has been painted
static bool s_scan_armed;                // set once the "Scansione..." screen has been painted
static bool s_consented;                 // skip the consent screen after the first accept this boot
static int  s_mode;                      // 0 = Civetta (fake SSID), 1 = Gemello cattura (open), 2 = Gemello coerente (WPA2)
static int  s_ap_idx, s_ap_n;            // selected/scanned real AP (Gemello mode)
static int  s_ssid_idx, s_tpl_idx, s_row;
static int  s_ssid_n, s_tpl_n;           // cached catalog sizes (avoid per-frame SD scans)
static char s_custom[33];                // typed SSID; overrides the preset when non-empty
static char s_err[40];
static uint32_t s_last_refresh;
static bool s_pulse;                      // 1 Hz blink for the "live" REC dot
static bool s_panel;                      // TAB overlay: loot/details (running) or help (config)

static void ep_tab(void);                 // TAB handler, registered in enter()
static void twin_autopick_template(void); // defined below; used by tick()/on_key()

static const char *effective_ssid(void)
{
    if (s_custom[0]) return s_custom;
    return (s_ssid_n > 0) ? nucleo_evilportal_ssid_preset(s_ssid_idx) : "Free WiFi";
}

static void set_hint(void)
{
    if (s_panel)                     nucleo_app_set_hint("tab: chiudi pannello");
    else if (s_state == ST_CONSENT)  nucleo_app_set_hint("invio accetto   esc esci");
    else if (s_state == ST_SCAN)     nucleo_app_set_hint("scansione reti...");
    else if (s_state == ST_STOPPING) nucleo_app_set_hint("arresto in corso...");
    else if (s_state == ST_RUNNING)  nucleo_app_set_hint("invio: ferma  esc: lascia on  tab: loot");
    else                             nucleo_app_set_hint("su/giu destra cambia  invio avvia  r riscan");
}

static void enter(void)
{
    s_ssid_n = nucleo_evilportal_ssid_count();
    s_tpl_n  = nucleo_evilportal_template_count();
    if (s_tpl_idx >= s_tpl_n)  s_tpl_idx = 0;
    if (s_ssid_idx >= s_ssid_n) s_ssid_idx = 0;
    // Prefer a real SD clone over the firmware fallback (index 0) the first time we open.
    static bool tpl_inited = false;
    if (!tpl_inited) { tpl_inited = true; if (s_tpl_n > 1) s_tpl_idx = 1; }
    s_err[0] = 0; s_panel = false;
    s_state = nucleo_evilportal_running() ? ST_RUNNING : (s_consented ? ST_CONFIG : ST_CONSENT);
    nucleo_app_set_tab_handler(ep_tab);    // claim TAB for our overlay instead of the Control Center
    set_hint();
    nucleo_app_request_draw();
}

// TAB toggles a readable detail panel: live loot while running, controls guide while configuring.
static void ep_tab(void)
{
    if (s_state != ST_RUNNING && s_state != ST_CONFIG) return;
    s_panel = !s_panel;
    set_hint();
    nucleo_app_request_draw();
}

static void tick(void)
{
    // Deferred scan: only after the "Scansione..." screen has been painted (it briefly blocks).
    if (s_state == ST_SCAN) {
        if (!s_scan_armed) return;
        s_ap_n = nucleo_wifiatk_scan();
        if (s_ap_idx >= s_ap_n) s_ap_idx = 0;
        twin_autopick_template();                  // fit the portal to the cloned brand
        s_state = ST_CONFIG;
        set_hint();
        nucleo_app_request_draw();
        return;
    }
    // Deferred teardown: only after the "Arresto..." screen has actually been painted, so the
    // user sees feedback before the (briefly blocking) stop + network restore runs.
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;                 // wait for one draw of the stopping screen
        nucleo_evilportal_stop();                  // frees DNS task + web server + heap, restores net
        nucleo_app_exit();                         // close the app -> back to launcher
        return;
    }
    if (s_state != ST_RUNNING) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last_refresh) { s_last_refresh = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }  // 1 Hz live refresh
}

// True if the chosen real AP is encrypted (anything but "open"): cloning it as an OPEN twin is a
// credibility downgrade modern clients can flag — surfaced honestly in the UI.
static bool twin_encrypted(void)
{
    const char *a = nucleo_wifiatk_target_auth(s_ap_idx);
    return a[0] && strcmp(a, "open") != 0;
}

// Pick the SD/built-in template whose name is contained in the SSID (case-insensitive), so a clone
// of "TIM-1234" uses tim.html etc. — a context-fitting page, not a generic one. -1 if no match.
static int template_for_ssid(const char *ssid)
{
    int n = nucleo_evilportal_template_count();
    for (int i = 0; i < n; i++) {
        const char *nm = nucleo_evilportal_template_name(i);
        size_t ln = nm ? strlen(nm) : 0;
        if (ln < 3) continue;                     // skip vague names ("Default") to avoid false hits
        for (const char *p = ssid; *p; p++) {
            size_t k = 0;
            while (p[k] && k < ln && tolower((unsigned char)p[k]) == tolower((unsigned char)nm[k])) k++;
            if (k == ln) return i;
        }
    }
    return -1;
}

// In Gemello, fit the portal to the cloned brand automatically (user can still change it after).
static void twin_autopick_template(void)
{
    if (s_mode == 0 || s_ap_n <= 0) return;
    int t = template_for_ssid(nucleo_wifiatk_target_ssid(s_ap_idx));
    if (t >= 0) s_tpl_idx = t;
}

static void start_portal(void)
{
    int rc;
    if (s_mode == 0) {                          // Civetta: fake SSID, channel 1, no deauth
        rc = nucleo_evilportal_start(effective_ssid(), s_tpl_idx);
    } else {                                    // Gemello (1=cattura/open, 2=coerente/WPA2) + deauth
        if (s_ap_n <= 0) { snprintf(s_err, sizeof s_err, "Nessuna rete: premi R"); set_hint(); nucleo_app_request_draw(); return; }
        unsigned char b[6]; nucleo_wifiatk_target_bssid_bytes(s_ap_idx, b);
        rc = nucleo_evilportal_start_twin(nucleo_wifiatk_target_ssid(s_ap_idx), s_tpl_idx, b,
                                          nucleo_wifiatk_target_channel(s_ap_idx),
                                          nucleo_wifiatk_target_authmode(s_ap_idx),
                                          s_mode == 2);
    }
    if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; }
    else snprintf(s_err, sizeof s_err, "Avvio fallito (err %d)", rc);
    set_hint();
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_panel) {                                 // panel is modal: Enter closes it, rest ignored
        if (key == NK_ENTER) { s_panel = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    if (s_state == ST_CONSENT) {
        if (key == NK_ENTER) { s_consented = true; s_state = ST_CONFIG; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    if (s_state == ST_SCAN || s_state == ST_STOPPING) return;   // busy: ignore keys
    if (s_state == ST_RUNNING) {
        // Enter = ferma e chiudi: paint the "Arresto..." screen, the actual stop runs in tick().
        if (key == NK_ENTER) { s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    // ST_CONFIG. Rows: 0 Modo, 1 SSID(civetta)/Rete(gemello), 2 Pagina, 3 AVVIA.
    switch (key) {
        case NK_UP:    s_row = (s_row + CFG_ROWS - 1) % CFG_ROWS; break;
        case NK_DOWN:  s_row = (s_row + 1) % CFG_ROWS;           break;
        case NK_RIGHT:
            if (s_row == 0) {
                s_mode = (s_mode + 1) % 3;             // Civetta -> Gemello cattura -> Gemello coerente
                if (s_mode >= 1 && s_ap_n == 0) {      // entering a Gemello mode with no scan yet -> scan
                    s_state = ST_SCAN; s_scan_armed = false; set_hint(); nucleo_app_request_draw(); return;
                }
                if (s_mode >= 1) twin_autopick_template();   // entering Gemello with APs already scanned
            } else if (s_row == 1) {
                if (s_mode == 0) { if (s_ssid_n > 0) { s_ssid_idx = (s_ssid_idx + 1) % s_ssid_n; s_custom[0] = 0; } }
                else             { if (s_ap_n   > 0) { s_ap_idx = (s_ap_idx + 1) % s_ap_n; twin_autopick_template(); } }
            } else if (s_row == 2) {
                if (s_tpl_n > 0) s_tpl_idx = (s_tpl_idx + 1) % s_tpl_n;
            }
            break;
        case NK_ENTER: start_portal(); return;
        case NK_DEL:
            if (s_row == 1 && s_mode == 0) { int l = strlen(s_custom); if (l) s_custom[l - 1] = 0; }
            break;
        case NK_CHAR:
            if ((ch == 'r' || ch == 'R') && s_mode != 0) {   // rescan real APs (no custom typing in Gemello)
                s_state = ST_SCAN; s_scan_armed = false; set_hint(); nucleo_app_request_draw(); return;
            }
            if (s_row == 1 && s_mode == 0 && ch > ' ' && ch < 127) { int l = strlen(s_custom); if (l < 32) { s_custom[l] = ch; s_custom[l + 1] = 0; } }
            break;
        default: return;
    }
    nucleo_app_request_draw();
}

// ---- drawing ----------------------------------------------------------------
static void draw_consent(int top, int h)
{
    (void)top; (void)h;
    app_ui_title("Evil Portal", EP_RED, "AUTH");
    // Readable headline (size 2), then the explanation in size 1.
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *lines[] = {
        "AP civetta + pagina di login",
        "clonata: salva su SD le",
        "credenziali inviate.",
        "",
        "Solo su reti tue o con",
        "permesso scritto (CTF, audit).",
    };
    unsigned short cols[] = { FG, FG, FG, BG, MUTED, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) { d.setTextColor(cols[i], BG); d.setCursor(10, 52 + i * 11); d.print(lines[i]); }
}

// A 28px-tall setting row, smartwatch style: the focused row is a filled accent pill with a
// size-2 (readable) label; unfocused rows are a dim size-1 line with a colour dot. The value
// sits on the right at size 1, truncated to the space left of the label.
static void cfg_row(int y, bool focus, const char *label, const char *value, unsigned short vcol)
{
    const char *v = value && value[0] ? value : "-";
    if (focus) {
        d.fillRoundRect(6, y, 228, 28, 6, EP_RED);
        d.setTextSize(2); d.setTextColor(INK, EP_RED); d.setCursor(14, y + 6); d.print(label);
        char b[26]; snprintf(b, sizeof b, "%.22s", v);
        int w = (int)strlen(b) * 6; d.setTextSize(1); d.setTextColor(INK, EP_RED);
        d.setCursor(226 - w, y + 18); d.print(b);
    } else {
        d.fillCircle(13, y + 14, 3, vcol);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 5); d.print(label);
        char b[26]; snprintf(b, sizeof b, "%.22s", v);
        int w = (int)strlen(b) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 16); d.print(b);
    }
}

static void draw_config(int top, int h)
{
    (void)top; (void)h;
    const char *badge = s_mode == 0 ? "CIVETTA" : s_mode == 1 ? "GEMELLO" : "COERENTE";
    app_ui_title("Evil Portal", EP_RED, badge);

    const char *mv = s_mode == 0 ? "Civetta (finta)" : s_mode == 1 ? "Gemello (cattura)" : "Gemello (coerente)";
    cfg_row(22, s_row == 0, "Modo", mv, s_mode == 0 ? GRN : s_mode == 1 ? YEL : EP_CYAN);

    if (s_mode == 0) {
        char sl[20]; snprintf(sl, sizeof sl, "SSID%s", s_custom[0] ? "*" : "");
        cfg_row(49, s_row == 1, sl, effective_ssid(), GRN);
    } else {
        char rv[44];
        // Show the real AP's beacon fingerprint (WPA2 CCMP bgn WPS) — what a credible twin must match.
        if (s_ap_n > 0) snprintf(rv, sizeof rv, "%.10s c%d %s", nucleo_wifiatk_target_ssid(s_ap_idx),
                                 nucleo_wifiatk_target_channel(s_ap_idx), nucleo_wifiatk_target_fingerprint(s_ap_idx));
        else            snprintf(rv, sizeof rv, "(premi R)");
        // Amber dot = cattura/open twin of an encrypted AP is a visible security downgrade; cyan =
        // coerente (WPA2 cloned, identity matches); green = open target, nothing to downgrade.
        bool enc = (s_ap_n > 0) && twin_encrypted();
        unsigned short dot = (enc && s_mode == 1) ? YEL : (s_mode == 2 ? EP_CYAN : GRN);
        cfg_row(49, s_row == 1, "Rete", rv, dot);
    }
    cfg_row(76, s_row == 2, "Pagina", s_tpl_n ? nucleo_evilportal_template_name(s_tpl_idx) : "-", YEL);

    // Action button — or the error message (red) drawn in its place.
    bool go = (s_row == 3);
    d.fillRoundRect(6, 103, 228, 16, 4, (go && !s_err[0]) ? EP_RED : PANEL);
    if (s_err[0]) {
        d.setTextSize(1); d.setTextColor(EP_RED, PANEL);
        d.setCursor(120 - (int)strlen(s_err) * 3, 107); d.print(s_err);
    } else {
        const char *btn = "AVVIA";
        d.setTextSize(2); d.setTextColor(go ? INK : EP_RED, go ? EP_RED : PANEL);
        d.setCursor(120 - (int)strlen(btn) * 6, 104); d.print(btn);
    }
}

static void draw_scan(int h)
{
    app_ui_title("Evil Portal", EP_RED, "");
    d.setTextSize(2); d.setTextColor(FG, BG);    d.setCursor(10, h / 2 - 14); d.print("Scansione...");
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6);  d.print("Cerco le reti reali vicine.");
    s_scan_armed = true;
}

// Hero counter: tiny label + a big size-3 number, centred in a half-width column.
static void tile(int cx, const char *label, const char *val, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(cx - (int)strlen(label) * 3, 40); d.print(label);
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(cx - (int)strlen(val) * 9, 52); d.print(val);     // size-3 glyphs are ~18 px wide
}

static void draw_running(int top, int h)
{
    (void)top;
    // Header: title + pulsing REC dot + elapsed time (readable size-2 title from app_ui_title).
    app_ui_title("Evil Portal", EP_RED, "");
    if (s_pulse) d.fillCircle(150, 9, 4, EP_RED);
    d.setTextSize(1); d.setTextColor(EP_RED, BG); d.setCursor(160, 7); d.print("REC");
    if (nucleo_evilportal_twin()) {
        bool coh = nucleo_evilportal_twin_coherent();
        d.setTextColor(coh ? EP_CYAN : 0xF81F /*magenta*/, BG); d.setCursor(110, 7); d.print(coh ? "TWIN-C" : "TWIN");
    }
    unsigned up = nucleo_evilportal_uptime_s();
    char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(el) * 6, 7); d.print(el);

    // SSID being broadcast (green, the live "this is what they see").
    int caps = nucleo_evilportal_captures();
    char ss[26]; snprintf(ss, sizeof ss, "%.24s", nucleo_evilportal_ssid());
    d.setTextSize(1); d.setTextColor(GRN, BG); d.setCursor(10, 28); d.print(ss);

    // Two big hero counters; "conf N" badge flags how many captures were re-entered identically.
    char cl[8]; snprintf(cl, sizeof cl, "%d", nucleo_evilportal_clients());
    char cp[8]; snprintf(cp, sizeof cp, "%d", caps);
    tile(62,  "CLIENT",  cl, FG);
    tile(178, "CATTURE", cp, caps ? EP_RED : MUTED);
    int conf = nucleo_evilportal_confirmed();
    if (conf > 0) {
        char cf[12]; snprintf(cf, sizeof cf, "conf %d", conf);
        d.setTextSize(1); d.setTextColor(GRN, BG);
        d.setCursor(178 - (int)strlen(cf) * 3, 74); d.print(cf);
    }

    // Newest capture, full-width and readable; "..." prompt while empty.
    d.drawFastHLine(10, 84, 220, LINE);
    if (caps == 0) {
        // Coherent twin can't take logins (clients need the real PSK) — say so instead of "waiting".
        const char *empty = nucleo_evilportal_twin_coherent()
                          ? "coerente: identita clonata, no login"
                          : "in attesa di vittime...";
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, 92); d.print(empty);
    } else {
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 90); d.print("ultima:");
        char u[26]; snprintf(u, sizeof u, "%.24s", nucleo_evilportal_last_user());
        char p[26]; snprintf(p, sizeof p, "%.24s", nucleo_evilportal_last_pass());
        d.setTextColor(FG, BG);  d.setCursor(58, 90);  d.print(u);
        d.setTextColor(EP_RED, BG); d.setCursor(10, 101); d.print(p);
    }

    // Footer: how to close. TAB = full loot. In twin mode it also shows the live deauth stats.
    d.setTextSize(1); d.setTextColor(YEL, BG); d.setCursor(10, h - 9);
    if (nucleo_evilportal_twin()) {
        char tf[44]; snprintf(tf, sizeof tf, "%s ch%d deauth%lu  invio:ferma",
                              nucleo_evilportal_twin_coherent() ? "GEM-C" : "GEM",
                              nucleo_evilportal_twin_channel(), nucleo_evilportal_deauth_frames());
        d.print(tf);
    } else {
        d.print("invio: ferma   tab: loot");
    }
}

// TAB panel — readable detail view. Running: live loot list + stats. Config: controls guide.
static void draw_panel(int top, int h)
{
    (void)top;
    if (s_state == ST_RUNNING) {
        app_ui_title("Loot", EP_RED, "");
        unsigned up = nucleo_evilportal_uptime_s();
        char s[52]; snprintf(s, sizeof s, "%02u:%02u  cli %d  catt %d  conf %d",
                             up / 60, up % 60, nucleo_evilportal_clients(),
                             nucleo_evilportal_captures(), nucleo_evilportal_confirmed());
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 28); d.print(s);
        int n = nucleo_evilportal_recent_count();          // up to the ring depth (6)
        if (n == 0) { d.setTextColor(DIM, BG); d.setCursor(10, 44); d.print("nessuna cattura."); }
        for (int i = 0; i < n; i++) {
            char row[44];
            snprintf(row, sizeof row, "%.18s  %.14s", nucleo_evilportal_recent_user(i), nucleo_evilportal_recent_pass(i));
            d.setTextColor(i == 0 ? FG : MUTED, BG); d.setCursor(10, 42 + i * 11); d.print(row);
        }
        d.setTextColor(DIM, BG); d.setCursor(10, h - 9); d.print("salvato su /sd/evilportal/loot/");
    } else {
        app_ui_title("Guida", EP_RED, "");
        const char *l[] = {
            "Civetta: SSID finto (open+login)",
            "Gemello cattura: clona + open",
            " declassa WPA2->open: si rileva",
            "Gemello coerente: clona WPA2",
            " identita combacia, ma no login",
            "Solo test autorizzati.",
        };
        unsigned short c[] = { FG, YEL, MUTED, EP_CYAN, MUTED, YEL };
        d.setTextSize(1);
        for (int i = 0; i < 6; i++) { d.setTextColor(c[i], BG); d.setCursor(10, 30 + i * 13); d.print(l[i]); }
    }
}

static void draw_stopping(int top, int h)
{
    (void)top;
    app_ui_title("Evil Portal", EP_RED, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print("Arresto...");
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6);
    d.print("Chiudo AP, DNS e server; ripristino rete OS.");
    s_stop_armed = true;                           // we've painted it -> tick() may now run stop()
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_panel)                     draw_panel(top, h);
    else if (s_state == ST_CONSENT)  draw_consent(top, h);
    else if (s_state == ST_SCAN)     draw_scan(h);
    else if (s_state == ST_STOPPING) draw_stopping(top, h);
    else if (s_state == ST_RUNNING)  draw_running(top, h);
    else                             draw_config(top, h);
}

static void leave(void)
{
    // The portal keeps running in the background after you leave the app — the launcher status
    // bar turns into a red "EVIL PORTAL ATTIVO" alert so it can't be forgotten. Reopen the app
    // (or use that alert as the reminder) and press Enter to stop it.
}

extern "C" void nucleo_register_evilportal(void)
{
    static const nucleo_app_def_t app = {
        "evilportal", "Evil Portal", "Security", "Captive portal for authorized Wi-Fi testing",
        'E', EP_RED, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
