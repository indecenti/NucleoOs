// Deauth Flood app — the on-device UI for the Wi-Fi offensive toolkit engine (nucleo_wifiatk).
//
// AUTHORIZED USE ONLY. This drives an 802.11 deauthentication flood of the same kind Bruce and
// Marauder ship. It is for security-awareness demos, auditing networks you own or have written
// permission to test, and CTF/lab work. The app opens on a consent screen and will not arm until
// the operator acknowledges it. Jamming/deauthing networks you don't own without authorization is
// illegal in most jurisdictions.
//
// Controls (the launcher eats Left/Back to leave the app, so the UI uses Up/Down/Enter + keys):
//   Consent : Enter = accept, Esc = leave
//   List    : Up/Down pick a target ("TUTTE" floods every AP), Enter arms, R rescans
//   Running : Enter stops the flood (Esc also stops, via on_exit)
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"

extern "C" {
// Engine API. Declared here (not via REQUIRES) so nucleo_app's link graph mirrors the Evil Portal
// arrangement — symbols resolve at final link because main pulls nucleo_wifiatk in.
int         nucleo_wifiatk_scan(void);
int         nucleo_wifiatk_target_count(void);
const char *nucleo_wifiatk_target_ssid(int i);
int         nucleo_wifiatk_target_channel(int i);
int         nucleo_wifiatk_target_rssi(int i);
const char *nucleo_wifiatk_target_bssid(int i);
const char *nucleo_wifiatk_target_auth(int i);
bool        nucleo_wifiatk_target_protected(int i);
int         nucleo_wifiatk_targets_vulnerable(void);
int         nucleo_wifiatk_deauth_start(int target_idx);   // esp_err_t (0 == ok)
void        nucleo_wifiatk_deauth_stop(void);
bool        nucleo_wifiatk_deauth_running(void);
unsigned long nucleo_wifiatk_frames(void);
int         nucleo_wifiatk_clients(void);
int         nucleo_wifiatk_clients_active(void);
int         nucleo_wifiatk_inject_health(void);
unsigned    nucleo_wifiatk_reconnects(void);
int         nucleo_wifiatk_cur_rssi(void);
int         nucleo_wifiatk_targets_active(void);
int         nucleo_wifiatk_cur_channel(void);
const char *nucleo_wifiatk_cur_ssid(void);
unsigned    nucleo_wifiatk_uptime_s(void);
int         nucleo_wifiatk_handshake_aps(void);       // WPA handshakes captured alongside the flood (multi-AP)
int         nucleo_wifiatk_handshake_count(void);
int         nucleo_wifiatk_handshake_pmkidn(void);
bool        nucleo_wifiatk_handshake_ready(void);
bool        nucleo_wifiatk_handshake_pmkid(void);
}

#include "launcher_theme.h"   // themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (launcher-consistent)
#include "app_gfx.h"
#include "nucleo_i18n.h"      // TR(it,en): hints follow the system language

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short ATK = 0xFD20 /*orange*/, GRN = C_GREEN,
                            YEL = C_YELLOW, PANEL = 0x18E3, PMF = 0x5C9F /*blue: PMF/immune*/;

enum { ST_CONSENT, ST_SCAN, ST_LIST, ST_RUNNING, ST_STOPPING };
static int  s_state;
static bool s_consented;          // skip the consent screen after the first accept this boot
static bool s_scan_armed;         // set once the "Scansione..." screen has been painted
static bool s_stop_armed;         // set once the "Arresto..." screen has been painted
static int  s_sel;                // 0 = "TUTTE (flood)", k>=1 = target k-1
static int  s_n;                  // cached target count
static char s_err[40];
static uint32_t s_last_refresh;
static bool s_pulse;
static bool s_panel;              // TAB overlay: paged how-to-use guide
static int  s_page;              // current guide page
static bool s_armed_protected;   // armed single target is PMF/WPA3 (deauth likely ineffective)

#define GUIDE_PAGES 5

static void wa_tab(void);         // TAB handler, registered in enter()

static int total_rows(void) { return s_n + 1; }   // +1 for the "TUTTE" row

static void set_hint(void)
{
    if (s_panel)                     nucleo_app_set_hint(TR("su/giu pagina   invio chiudi", "up/dn page   enter close"));
    else if (s_state == ST_CONSENT)  nucleo_app_set_hint(TR("invio accetto   esc esci   tab guida", "enter accept   esc back   tab guide"));
    else if (s_state == ST_SCAN)     nucleo_app_set_hint(TR("scansione in corso...", "scanning..."));
    else if (s_state == ST_STOPPING) nucleo_app_set_hint(TR("arresto in corso...", "stopping..."));
    else if (s_state == ST_RUNNING)  nucleo_app_set_hint(TR("invio: ferma   esc: lascia on   tab: guida", "enter: stop   esc: leave on   tab: guide"));
    else                             nucleo_app_set_hint(TR("su/giu avvia   r riscan   tab guida", "up/dn start   r rescan   tab guide"));
}

static void enter(void)
{
    s_err[0] = 0; s_panel = false;
    if (nucleo_wifiatk_deauth_running()) { s_state = ST_RUNNING; }
    else if (s_consented)               { s_state = ST_SCAN; s_scan_armed = false; }
    else                                { s_state = ST_CONSENT; }
    nucleo_app_set_tab_handler(wa_tab);    // claim TAB for the guide overlay
    set_hint();
    nucleo_app_request_draw();
}

// TAB toggles the how-to-use guide; usable any time except while busy (scan/stop).
static void wa_tab(void)
{
    if (s_state == ST_SCAN || s_state == ST_STOPPING) return;
    s_panel = !s_panel;
    set_hint();
    nucleo_app_request_draw();
}

static void start_attack(void)
{
    int idx = (s_sel == 0) ? -1 : s_sel - 1;
    s_armed_protected = (idx >= 0) && nucleo_wifiatk_target_protected(idx);   // warn: PMF target
    int rc = nucleo_wifiatk_deauth_start(idx);
    if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; }
    else snprintf(s_err, sizeof s_err, "Avvio fallito (err %d)", rc);
    set_hint();
    nucleo_app_request_draw();
}

static void tick(void)
{
    // Deferred scan: only after the "Scansione..." screen has been painted (it briefly blocks).
    if (s_state == ST_SCAN) {
        if (!s_scan_armed) return;
        s_n = nucleo_wifiatk_scan();
        if (s_sel >= total_rows()) s_sel = 0;
        s_state = ST_LIST;
        set_hint();
        nucleo_app_request_draw();
        return;
    }
    // Deferred teardown: stop only after the "Arresto..." screen has been painted.
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;
        nucleo_wifiatk_deauth_stop();
        nucleo_app_exit();
        return;
    }
    if (s_state != ST_RUNNING) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last_refresh) { s_last_refresh = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
}

static void on_key(int key, char ch)
{
    if (s_panel) {                                 // guide is modal: page with Up/Down, Enter closes
        if (key == NK_UP)        s_page = (s_page + GUIDE_PAGES - 1) % GUIDE_PAGES;
        else if (key == NK_DOWN) s_page = (s_page + 1) % GUIDE_PAGES;
        else if (key == NK_ENTER) { s_panel = false; set_hint(); }
        nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_CONSENT) {
        if (key == NK_ENTER) { s_consented = true; s_state = ST_SCAN; s_scan_armed = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    if (s_state == ST_SCAN || s_state == ST_STOPPING) return;   // busy: ignore keys
    if (s_state == ST_RUNNING) {
        if (key == NK_ENTER) { s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    // ST_LIST
    int tot = total_rows();
    switch (key) {
        case NK_UP:    s_sel = (s_sel - 1 + tot) % tot; break;
        case NK_DOWN:  s_sel = (s_sel + 1) % tot;       break;
        case NK_ENTER: start_attack(); return;
        case NK_CHAR:
            if (ch == 'r' || ch == 'R') { s_state = ST_SCAN; s_scan_armed = false; set_hint(); }
            break;
        default: return;
    }
    nucleo_app_request_draw();
}

// ---- drawing ----------------------------------------------------------------
static void draw_consent(int top, int h)
{
    (void)top; (void)h;
    app_ui_title("Deauth Flood", ATK, "AUTH");
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *lines[] = {
        "Inonda di frame deauth/",
        "disassoc le reti scelte:",
        "scollega i client dall'AP.",
        "",
        "Solo su reti tue o con",
        "permesso scritto (CTF, audit).",
    };
    unsigned short cols[] = { FG, FG, FG, BG, MUTED, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) { d.setTextColor(cols[i], BG); d.setCursor(10, 52 + i * 11); d.print(lines[i]); }
}

static void draw_busy(const char *title, const char *msg, int h)
{
    app_ui_title("Deauth Flood", ATK, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print(title);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6); d.print(msg);
}

// A list row: focused row is a filled accent pill (size 2 label); others are dim size-1 lines.
static void list_row(int y, bool focus, const char *label, const char *right, unsigned short dot)
{
    if (focus) {
        d.fillRoundRect(6, y, 228, 26, 6, ATK);
        d.setTextSize(2); d.setTextColor(INK, ATK); d.setCursor(14, y + 5);
        char b[18]; snprintf(b, sizeof b, "%.15s", label); d.print(b);
        if (right && right[0]) { int w = (int)strlen(right) * 6; d.setTextSize(1); d.setTextColor(INK, ATK); d.setCursor(226 - w, y + 16); d.print(right); }
    } else {
        if (dot) d.fillCircle(13, y + 13, 3, dot);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 6);
        char b[30]; snprintf(b, sizeof b, "%.26s", label); d.print(b);
        if (right && right[0]) { int w = (int)strlen(right) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 6); d.print(right); }
    }
}

static void draw_list(int top, int h)
{
    (void)top;
    char rt[16]; snprintf(rt, sizeof rt, "%d AP", s_n);
    app_ui_title("Deauth Flood", ATK, rt);

    int tot = total_rows();
    const int rowh = 28, y0 = 26;
    int vis = (h - y0) / rowh; if (vis < 1) vis = 1;
    int first = s_sel - vis / 2;
    if (first > tot - vis) first = tot - vis;
    if (first < 0) first = 0;

    for (int r = 0; r < vis && first + r < tot; r++) {
        int idx = first + r, y = y0 + r * rowh;
        bool focus = (idx == s_sel);
        if (idx == 0) {
            char tv[8]; snprintf(tv, sizeof tv, "%d", nucleo_wifiatk_targets_vulnerable());
            list_row(y, focus, "TUTTE (vuln.)", s_n ? tv : "", ATK);   // flood-all hits only the deauthable APs
        } else {
            int t = idx - 1;
            bool prot = nucleo_wifiatk_target_protected(t);
            char lbl[34]; snprintf(lbl, sizeof lbl, "%s", nucleo_wifiatk_target_ssid(t));
            char info[22];
            if (prot) snprintf(info, sizeof info, "c%d %s immune", nucleo_wifiatk_target_channel(t), nucleo_wifiatk_target_auth(t));
            else      snprintf(info, sizeof info, "c%d %s %ddB", nucleo_wifiatk_target_channel(t), nucleo_wifiatk_target_auth(t), nucleo_wifiatk_target_rssi(t));
            list_row(y, focus, lbl, info, prot ? PMF : GRN);
        }
    }
    if (s_n == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, h - 10); d.print("nessuna rete - premi R"); }
    else if (s_err[0]) { d.setTextSize(1); d.setTextColor(ATK, BG); d.setCursor(10, h - 10); d.print(s_err); }
}

static void tile(int cx, const char *label, const char *val, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(cx - (int)strlen(label) * 3, 40); d.print(label);
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(cx - (int)strlen(val) * 9, 52); d.print(val);
}

static void draw_running(int top, int h)
{
    (void)top;
    app_ui_title("Deauth Flood", ATK, "");
    if (s_pulse) d.fillCircle(150, 9, 4, ATK);
    d.setTextSize(1); d.setTextColor(ATK, BG); d.setCursor(160, 7); d.print("TX");
    unsigned up = nucleo_wifiatk_uptime_s();
    char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(el) * 6, 7); d.print(el);

    // What's being hit (the live target line).
    char ss[30];
    if (nucleo_wifiatk_targets_active() > 1) snprintf(ss, sizeof ss, "TUTTE  ora: %.16s", nucleo_wifiatk_cur_ssid());
    else                                     snprintf(ss, sizeof ss, "%.26s", nucleo_wifiatk_cur_ssid());
    d.setTextSize(1); d.setTextColor(GRN, BG); d.setCursor(10, 28); d.print(ss);

    // Two big hero counters: frames TX'd, and clients we've knocked silent (the effectiveness number
    // Bruce never shows). KICK = discovered stations that have gone quiet under the flood.
    char fr[12]; unsigned long f = nucleo_wifiatk_frames();
    if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);
    int seen = nucleo_wifiatk_clients(), act = nucleo_wifiatk_clients_active();
    int kick = seen - act; if (kick < 0) kick = 0;
    char kk[8]; snprintf(kk, sizeof kk, "%d", kick);
    tile(62,  "FRAME", fr, ATK);
    tile(178, "KICK",  kk, kick ? GRN : MUTED);

    int inj = nucleo_wifiatk_inject_health();
    unsigned recon = nucleo_wifiatk_reconnects();
    int rssi = nucleo_wifiatk_cur_rssi();

    // Status line: live effectiveness inputs. Signal turns amber when too weak (move closer). When a
    // WPA handshake is coming in, append which of the 4 EAPOL messages we've grabbed (e.g. "HS12").
    int hsaps = nucleo_wifiatk_handshake_aps();
    int hsok = nucleo_wifiatk_handshake_count();
    int hspm = nucleo_wifiatk_handshake_pmkidn();
    char hs[28] = "";
    if (hsaps && hspm) snprintf(hs, sizeof hs, " HS%d/%d PMKID%d", hsok, hsaps, hspm);
    else if (hsaps)    snprintf(hs, sizeof hs, " HS%d/%d", hsok, hsaps);
    d.drawFastHLine(10, 84, 220, LINE);
    char st[64]; snprintf(st, sizeof st, "att %d  rec %u  ch%d  %ddB%s",
                          act, recon, nucleo_wifiatk_cur_channel(), rssi, hs);
    bool weak = (rssi != 0 && rssi <= -80);
    d.setTextSize(1); d.setTextColor(weak ? ATK : MUTED, BG); d.setCursor(10, 92); d.print(st);

    // Honest live verdict (the never-blind part): derived from injection health + measured effect.
    const char *vd; unsigned short vc;
    static char vbuf[34];
    if (hsok > 0 || hspm > 0)   { snprintf(vbuf, sizeof vbuf, "CATTURATI: %d HS, %d PMKID", hsok, hspm); vd = vbuf; vc = GRN; }
    else if (inj < 50)          { vd = "INIEZIONE DEBOLE"; vc = ATK; }
    else if (seen == 0)         { vd = (up < 6) ? "avvio..." : "nessun client"; vc = MUTED; }
    else if (kick > 0 || recon) { vd = "EFFICACE"; vc = GRN; }
    else if (weak)              { vd = "segnale debole: avvicinati"; vc = ATK; }
    else if (up >= 12)          { vd = s_armed_protected ? "PMF: resiste (CSA)" : "nessun effetto"; vc = ATK; }
    else                        { vd = s_armed_protected ? "CSA in corso..." : "in corso..."; vc = YEL; }
    char fb[44]; snprintf(fb, sizeof fb, "%s   inj %d%%", vd, inj);
    d.setTextColor(vc, BG); d.setCursor(10, h - 9); d.print(fb);
}

// ---- how-to-use guide (TAB overlay) ----------------------------------------
static const char *GUIDE_HEAD[GUIDE_PAGES] = {
    "Come funziona", "Leggere i dati", "Usarla meglio", "Limiti reali", "Comandi"
};
static const char *GUIDE_BODY[GUIDE_PAGES][6] = {
    { "Sgancia i client da un AP",
      "con frame 802.11 deauth.",
      "Sniffa i client reali e li",
      "colpisce UNO PER UNO (unicast)",
      "+ broadcast: piu efficace",
      "del solo broadcast." },
    { "FRAME inviati  KICK cacciati",
      "att in onda  rec riconnessi",
      "rec sale = il deauth ha colpito",
      "inj% salute TX (basso = il",
      "chip scarta: poco raggio / KO)",
      "Bene: KICK/rec su, att -> 0" },
    { "Scegli 1 AP con client:",
      "piu mirato che TUTTE.",
      "Dot blu = WPA3/OWE immune,",
      "TUTTE li salta da solo.",
      "Stai vicino. Dai tempo: i",
      "client emergono sniffando." },
    { "2.4GHz aperte/WPA2: scollega",
      "e forza riconnessioni (vedi rec).",
      "WPA3/PMF: deauth ignorato, uso",
      "CSA channel-switch (beacon non",
      "protetti) - effetto variabile.",
      "Frame a 1Mbps per max raggio." },
    { "su/giu  scegli bersaglio",
      "R       riscansiona",
      "invio   avvia / ferma",
      "esc     lascia attivo (bg)",
      "Solo reti tue o con permesso",
      "scritto: CTF / audit." },
};

static void draw_guide(void)
{
    char rt[12]; snprintf(rt, sizeof rt, "%d/%d", s_page + 1, GUIDE_PAGES);
    app_ui_title("Guida", ATK, rt);
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 26); d.print(GUIDE_HEAD[s_page]);
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) {
        const char *l = GUIDE_BODY[s_page][i];
        if (!l[0]) continue;
        bool legal = (s_page == GUIDE_PAGES - 1 && i >= 4);   // authorization note stands out
        d.setTextColor(legal ? YEL : FG, BG);
        d.setCursor(10, 48 + i * 11); d.print(l);
    }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_panel)                     draw_guide();
    else if (s_state == ST_CONSENT)  draw_consent(top, h);
    else if (s_state == ST_SCAN)     { draw_busy("Scansione...", "Cerco le reti vicine.", h); s_scan_armed = true; }
    else if (s_state == ST_STOPPING) { draw_busy("Arresto...", "Ripristino rete OS.", h); s_stop_armed = true; }
    else if (s_state == ST_RUNNING)  draw_running(top, h);
    else                             draw_list(top, h);
}

static void leave(void)
{
    // The flood keeps running in the background after you leave the app (the launcher shows a red
    // alert bar). Reopen the app and press Enter to stop it. Reset consent so a new session always
    // re-asks for authorization (s_consented was a per-boot latch).
    s_consented = false;
}

extern "C" void nucleo_register_wifiatk(void)
{
    static const nucleo_app_def_t app = {
        "wifiatk", "Deauth Flood", "Security", "802.11 deauth flood for authorized Wi-Fi testing",
        'D', ATK, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
