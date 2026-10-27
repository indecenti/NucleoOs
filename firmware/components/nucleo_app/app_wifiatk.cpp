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
#include "app_large.h"        // two screens: the deauth toolkit laid out on the 320x240 panel
#include "nucleo_touch.h"

// BG/FG/MUTED/DIM/LINE/INK come from launcher_theme.h (themed, shared with the launcher).
static const unsigned short ATK = 0xFD20 /*orange*/, GRN = C_GREEN,
                            YEL = C_YELLOW, PANEL = 0x18E3, PMF = 0x5C9F /*blue: PMF/immune*/;
static int s_lg_scroll = 0;   // panel target-list scroll

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
    if (s_panel)                     nucleo_app_set_hint(TR5("su/giu pagina   invio chiudi", "up/dn page   enter close", "ar/ab pag   intro cierra", "haut/bas pg   entrer ferme", "auf/ab pg   enter schliess"));
    else if (s_state == ST_CONSENT)  nucleo_app_set_hint(TR5("invio accetto   esc esci   tab guida", "enter accept   esc back   tab guide", "intro acepta   esc atras   tab guia", "entrer accepter   esc ret   tab guide", "enter akzept   esc zur   tab anleit"));
    else if (s_state == ST_SCAN)     nucleo_app_set_hint(TR5("scansione in corso...", "scanning...", "escaneo...", "balayage...", "abtast..."));
    else if (s_state == ST_STOPPING) nucleo_app_set_hint(TR5("arresto in corso...", "stopping...", "detencion...", "arret...", "beendigung..."));
    else if (s_state == ST_RUNNING)  nucleo_app_set_hint(TR5("invio ferma  esc lascia on  tab guida", "enter stop  esc leave on  tab guide", "intro detiene  esc deja on  tab guia", "entree arret  esc laisse  tab guide", "enter stopp  esc weiter  tab Hilfe"));
    else                             nucleo_app_set_hint(TR5("su/giu avvia   r riscan   tab guida", "up/dn start   r rescan   tab guide", "ar/ab inicia   r rescan   tab guia", "h/b demarre   r rescan   tab guide", "auf/ab start   r rescan   tab Hilfe"));
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
    else snprintf(s_err, sizeof s_err, TR("Avvio fallito (err %d)","Start failed (err %d)"), rc);
    set_hint();
    nucleo_app_request_draw();
}

static void tick(void)
{
    // Deferred scan: only after the "Scansione..." screen has been painted (it briefly blocks). The arm is set
    // on the first tick in the state (draw also sets it) so a redraw of the busy screen is guaranteed between
    // entering ST_SCAN and the blocking scan — same timing on the device, and reachable in the PC gallery.
    if (s_state == ST_SCAN) {
        if (!s_scan_armed) { s_scan_armed = true; return; }
        s_n = nucleo_wifiatk_scan();
        if (s_sel >= total_rows()) s_sel = 0;
        s_state = ST_LIST;
        set_hint();
        nucleo_app_request_draw();
        return;
    }
    // Deferred teardown: stop only after the "Arresto..." screen has been painted.
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) { s_stop_armed = true; return; }
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
    if (int t = lg::row_tap(key, ch, s_lg_scroll, tot, &s_sel)) {   // panel: tap a target to pick, again to arm
        if (t == 2) { start_attack(); return; }
        nucleo_app_request_draw(); return;
    }
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
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print(TR("Test autorizzati","Authorized testing"));
    const char *lines_it[] = {
        "Inonda di frame deauth/",
        "disassoc le reti scelte:",
        "scollega i client dall'AP.",
        "",
        "Solo su reti tue o con",
        "permesso scritto (CTF, audit).",
    };
    const char *lines_en[] = {
        "Floods deauth/disassoc",
        "frames at the chosen nets:",
        "knocks clients off the AP.",
        "",
        "Your own networks or with",
        "written permission (CTF, audit).",
    };
    const char *const *lines = nucleo_i18n_is_it() ? lines_it : lines_en;
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
            list_row(y, focus, TR("TUTTE (vuln.)","ALL (vuln.)"), s_n ? tv : "", ATK);   // flood-all hits only the deauthable APs
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
    if (s_n == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, h - 10); d.print(TR("nessuna rete - premi R","no networks - press R")); }
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
    if (nucleo_wifiatk_targets_active() > 1) snprintf(ss, sizeof ss, TR("TUTTE  ora: %.16s","ALL  now: %.16s"), nucleo_wifiatk_cur_ssid());
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
    if (hsok > 0 || hspm > 0)   { snprintf(vbuf, sizeof vbuf, TR("CATTURATI: %d HS, %d PMKID","CAPTURED: %d HS, %d PMKID"), hsok, hspm); vd = vbuf; vc = GRN; }
    else if (inj < 50)          { vd = "INIEZIONE DEBOLE"; vc = ATK; }
    else if (seen == 0)         { vd = (up < 6) ? TR("avvio...","starting...") : TR("nessun client","no clients"); vc = MUTED; }
    else if (kick > 0 || recon) { vd = "EFFICACE"; vc = GRN; }
    else if (weak)              { vd = "segnale debole: avvicinati"; vc = ATK; }
    else if (up >= 12)          { vd = s_armed_protected ? "PMF: resiste (CSA)" : TR("nessun effetto","no effect"); vc = ATK; }
    else                        { vd = s_armed_protected ? "CSA in corso..." : "in corso..."; vc = YEL; }
    char fb[44]; snprintf(fb, sizeof fb, "%s   inj %d%%", vd, inj);
    d.setTextColor(vc, BG); d.setCursor(10, h - 9); d.print(fb);
}

// ---- how-to-use guide (TAB overlay) ----------------------------------------
static const char *GUIDE_HEAD_IT[GUIDE_PAGES] = {
    "Come funziona", "Leggere i dati", "Usarla meglio", "Limiti reali", "Comandi"
};
static const char *GUIDE_HEAD_EN[GUIDE_PAGES] = {
    "How it works", "Reading the data", "Using it better", "Real limits", "Keys"
};
#define GUIDE_HEAD (nucleo_i18n_is_it() ? GUIDE_HEAD_IT : GUIDE_HEAD_EN)
static const char *GUIDE_BODY_IT[GUIDE_PAGES][6] = {
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
static const char *GUIDE_BODY_EN[GUIDE_PAGES][6] = {
    { "Knocks clients off an AP",
      "with 802.11 deauth frames.",
      "Sniffs the real clients and",
      "hits them ONE BY ONE (unicast)",
      "+ broadcast: more effective",
      "than broadcast alone." },
    { "FRAME sent  KICK kicked",
      "att on air  rec reconnected",
      "rec rising = the deauth landed",
      "inj% TX health (low = the",
      "chip drops: short range / KO)",
      "Good: KICK/rec up, att -> 0" },
    { "Pick 1 AP with clients:",
      "more targeted than ALL.",
      "Blue dot = WPA3/OWE immune,",
      "ALL skips them on its own.",
      "Stay close. Give it time: the",
      "clients surface as you sniff." },
    { "2.4GHz open/WPA2: disconnects",
      "and forces reconnects (see rec).",
      "WPA3/PMF: deauth ignored, uses",
      "CSA channel-switch (beacons not",
      "protected) - effect varies.",
      "Frames at 1Mbps for max range." },
    { "up/dn  pick target",
      "R       rescan",
      "enter   start / stop",
      "esc     leave on (bg)",
      "Your own networks or with written",
      "permission: CTF / audit." },
};
#define GUIDE_BODY (nucleo_i18n_is_it() ? GUIDE_BODY_IT : GUIDE_BODY_EN)

static void draw_guide(void)
{
    char rt[12]; snprintf(rt, sizeof rt, "%d/%d", s_page + 1, GUIDE_PAGES);
    app_ui_title(TR("Guida","Guide"), ATK, rt);
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

// ===== two screens ==============================================================================================
// The panel carries the toolkit: the consent card with Accept/Back buttons, the scrollable target list (tap a
// row to pick, again to arm) with the picked AP's details + a big ARM button beside it, the running flood as big
// FRAME/KICK counters with a live verdict and a STOP button, and the how-to guide as a paged panel. Touch is the
// content; the header menu glyph opens the guide. The Cardputer becomes the companion: the picked target or the
// live counters + verdict, and the key hints.
static void lg_atk_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *) {
    if (i == 0) {
        char rv[10]; snprintf(rv, sizeof rv, "%d", nucleo_wifiatk_targets_vulnerable());
        lg::row_text(x, y, w, h, focus, bg, TR("TUTTE (vuln.)", "ALL (vuln.)"), s_n ? rv : "", focus ? INK : ATK);
        return;
    }
    int t = i - 1;
    bool prot = nucleo_wifiatk_target_protected(t);
    char right[20]; snprintf(right, sizeof right, "c%d %ddB", nucleo_wifiatk_target_channel(t), nucleo_wifiatk_target_rssi(t));
    const char *ss = nucleo_wifiatk_target_ssid(t);
    lg::row_text(x, y, w, h, focus, bg, ss[0] ? ss : "(hidden)", right, focus ? INK : (prot ? PMF : FG));
}

static void lg_big(int cx, int cy, const char *label, const char *val, unsigned short col, unsigned short bg) {
    lg::font(3); d.setTextColor(col, bg); d.setTextDatum(middle_center); d.drawString(val, cx, cy);
    lg::font(0); d.setTextColor(MUTED, bg); d.drawString(label, cx, cy + 28); d.setTextDatum(top_left);
}

static void draw_large(void) {
    const int Wp = d.width(), Hp = d.height();
    nucleo_touch_clear();

    if (s_panel) {   // guide: paged, closes on the chevron (mapped to ENTER) or a tap
        char rt[12]; snprintf(rt, sizeof rt, "%d/%d", s_page + 1, GUIDE_PAGES);
        int y = lg::header(TR("Guida", "Guide"), rt, ATK, NK_ENTER);
        d.fillRect(0, y, Wp, Hp - y, BG);
        lg::panel(12, y + 6, Wp - 24, Hp - y - 58);
        const unsigned short cb = lg::card();
        lg::font(2); d.setTextColor(YEL, cb); d.drawString(GUIDE_HEAD[s_page], 24, y + 16);
        lg::font(0); d.setTextColor(FG, cb);
        int ty = y + 46;
        for (int i = 0; i < 6; i++) { const char *l = GUIDE_BODY[s_page][i]; if (!l[0]) { ty += 8; continue; }
            d.setTextColor(s_page == GUIDE_PAGES - 1 && i >= 4 ? YEL : FG, cb); d.drawString(l, 24, ty); ty += 20; }
        const int bw = (Wp - 36) / 2;
        lg::button(12, Hp - 44, bw, 34, TR("< Prec", "< Prev"), lg::card(), FG, NK_UP, 0);
        lg::button(24 + bw, Hp - 44, bw, 34, TR("Succ >", "Next >"), lg::card(), FG, NK_DOWN, 0);
        nucleo_touch_commit(); return;
    }

    if (s_state == ST_CONSENT) {
        int y = lg::header("Deauth Flood", "AUTH", ATK, NK_BACK, NK_TAB);
        d.fillRect(0, y, Wp, Hp - y, BG);
        lg::panel(12, y + 8, Wp - 24, Hp - y - 70);
        const unsigned short cb = lg::card();
        lg::font(1); d.setTextColor(YEL, cb); d.drawString(TR("Test autorizzati", "Authorized testing"), 24, y + 18);
        lg::font(0); d.setTextColor(FG, cb);
        lg::wrap(TR("Inonda di frame deauth/disassoc le reti scelte: scollega i client dall'AP.",
                    "Floods deauth/disassoc frames at the chosen networks: knocks clients off the AP."), 24, y + 44, Wp - 48, 19, 3);
        d.setTextColor(MUTED, cb);
        lg::wrap(TR("Solo su reti tue o con permesso scritto (CTF, audit).",
                    "Your own networks or with written permission (CTF, audit)."), 24, y + 108, Wp - 48, 19, 2);
        const int bw = (Wp - 36) / 2;
        lg::button(12, Hp - 54, bw, 42, TR("Indietro", "Back"), lg::card(), FG, NK_BACK, 0);
        lg::button(24 + bw, Hp - 54, bw, 42, TR("Accetto", "Accept"), ATK, INK, NK_ENTER, 0);
        nucleo_touch_commit(); return;
    }
    if (s_state == ST_SCAN || s_state == ST_STOPPING) {
        lg::header("Deauth Flood", nullptr, ATK, NK_BACK);
        d.fillRect(0, lg::HEAD, Wp, Hp - lg::HEAD, BG);
        lg::message(0, lg::HEAD, Wp, Hp - lg::HEAD,
                    s_state == ST_SCAN ? TR("Scansione...", "Scanning...") : TR("Arresto...", "Stopping..."),
                    s_state == ST_SCAN ? TR("Cerco le reti vicine.", "Looking for nearby networks.") : TR("Ripristino rete OS.", "Restoring OS network."), ATK);
        if (s_state == ST_SCAN) s_scan_armed = true; else s_stop_armed = true;
        nucleo_touch_commit(); return;
    }
    if (s_state == ST_RUNNING) {
        unsigned up = nucleo_wifiatk_uptime_s();
        char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
        int y = lg::header("Deauth Flood", el, ATK, NK_BACK, NK_TAB);
        d.fillRect(0, y, Wp, Hp - y, BG);
        char ss[36]; snprintf(ss, sizeof ss, "%.34s", nucleo_wifiatk_cur_ssid());
        lg::font(1); d.setTextColor(GRN, BG); d.drawString(ss, 14, y + 6);
        // two hero counters
        char fr[12]; unsigned long f = nucleo_wifiatk_frames();
        if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);
        int seen = nucleo_wifiatk_clients(), act = nucleo_wifiatk_clients_active(), kick = seen - act; if (kick < 0) kick = 0;
        char kk[8]; snprintf(kk, sizeof kk, "%d", kick);
        lg::panel(12, y + 28, (Wp - 30) / 2, 72);
        lg::panel(18 + (Wp - 30) / 2, y + 28, (Wp - 30) / 2, 72);
        lg_big(12 + (Wp - 30) / 4, y + 56, "FRAME", fr, ATK, lg::card());
        lg_big(18 + (Wp - 30) / 2 + (Wp - 30) / 4, y + 56, "KICK", kk, kick ? GRN : MUTED, lg::card());
        // status line
        int inj = nucleo_wifiatk_inject_health(); int rssi = nucleo_wifiatk_cur_rssi();
        char st[64]; snprintf(st, sizeof st, "att %d  rec %u  ch%d  %ddB  inj %d%%",
                              act, nucleo_wifiatk_reconnects(), nucleo_wifiatk_cur_channel(), rssi, inj);
        lg::font(0); d.setTextColor(MUTED, BG); d.drawString(st, 14, y + 108);
        // verdict
        int hsok = nucleo_wifiatk_handshake_count();
        const char *vd; unsigned short vc;
        char vbuf[40];
        if (hsok > 0) { snprintf(vbuf, sizeof vbuf, TR("CATTURATI: %d handshake", "CAPTURED: %d handshake"), hsok); vd = vbuf; vc = GRN; }
        else if (inj < 50) { vd = TR("INIEZIONE DEBOLE", "WEAK INJECTION"); vc = ATK; }
        else if (kick > 0) { vd = TR("EFFICACE", "EFFECTIVE"); vc = GRN; }
        else { vd = TR("in corso...", "in progress..."); vc = YEL; }
        d.fillRoundRect(12, y + 126, Wp - 24, 28, 7, vc);
        lg::font(1); d.setTextColor(INK, vc); d.setTextDatum(middle_left); d.drawString(vd, 26, y + 140); d.setTextDatum(top_left);
        lg::button(40, Hp - 50, Wp - 80, 40, TR("FERMA", "STOP"), C_RED, INK, NK_ENTER, 0);
        nucleo_touch_commit(); return;
    }
    // ST_LIST
    char rt[12]; snprintf(rt, sizeof rt, "%d AP", s_n);
    int y = lg::header("Deauth Flood", rt, ATK, NK_BACK, NK_TAB);
    const int LW = 182, tot = total_rows();
    lg::list(0, y + 2, LW, Hp - y - 6, lg::ROW, tot, s_sel, &s_lg_scroll, lg_atk_row, nullptr, ATK);
    d.fillRect(LW, y, Wp - LW, Hp - y, BG);
    const int dx = LW + 4, dw = Wp - LW - 10;
    lg::panel(dx, y + 6, dw, Hp - y - 58);
    const unsigned short cb = lg::card();
    if (s_sel == 0) {
        lg::font(1); d.setTextColor(ATK, cb); d.drawString(TR("Tutte le reti", "All networks"), dx + 10, y + 16);
        lg::font(0); d.setTextColor(FG, cb);
        char b[32]; snprintf(b, sizeof b, TR("%d attaccabili", "%d attackable"), nucleo_wifiatk_targets_vulnerable());
        d.drawString(b, dx + 10, y + 44);
        d.setTextColor(MUTED, cb);
        lg::wrap(TR("Salta da solo le reti WPA3/OWE.", "Skips WPA3/OWE on its own."), dx + 10, y + 66, dw - 20, 17, 2);
    } else {
        int t = s_sel - 1; bool prot = nucleo_wifiatk_target_protected(t);
        char nm[28]; lg::fit(nm, sizeof nm, nucleo_wifiatk_target_ssid(t), dw - 20);
        lg::font(1); d.setTextColor(prot ? PMF : ATK, cb); d.drawString(nm[0] ? nm : "(hidden)", dx + 10, y + 16);
        d.drawFastHLine(dx + 10, y + 38, dw - 20, LINE);
        int ky = y + 48; char v[16];
        snprintf(v, sizeof v, "%d", nucleo_wifiatk_target_channel(t)); lg::kv(dx + 10, ky, dw - 20, TR("Canale", "Channel"), v, FG, cb); ky += 24;
        lg::kv(dx + 10, ky, dw - 20, TR("Sicur.", "Auth"), nucleo_wifiatk_target_auth(t), FG, cb); ky += 24;
        snprintf(v, sizeof v, "%d dB", nucleo_wifiatk_target_rssi(t)); lg::kv(dx + 10, ky, dw - 20, "RSSI", v, FG, cb); ky += 26;
        if (prot) { lg::font(0); d.setTextColor(PMF, cb); d.drawString(TR("WPA3/PMF: immune", "WPA3/PMF: immune"), dx + 10, ky); }
    }
    lg::button(dx, Hp - 48, dw, 36, TR("ARMA", "ARM"), ATK, INK, NK_ENTER, 0);
    if (s_n == 0) { lg::font(0); d.setTextColor(DIM, BG); d.setTextDatum(bottom_left);
                    d.drawString(TR("nessuna rete - r riscan", "no networks - r rescan"), 8, Hp - 4); d.setTextDatum(top_left); }
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("DEAUTH", ATK);
    if (s_panel) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(YEL, BG); d.drawString(GUIDE_HEAD[s_page], 8, y + 6);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); char p[16]; snprintf(p, sizeof p, "%d/%d", s_page + 1, GUIDE_PAGES); d.drawString(p, 8, y + 30);
        lg::comp_keys(TR(";/. pagina  INVIO chiudi", ";/. page  ENTER close")); return;
    }
    if (s_state == ST_RUNNING) {
        char fr[12]; unsigned long f = nucleo_wifiatk_frames();
        if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);
        d.setFont(&fonts::FreeSansBold18pt7b); d.setTextColor(ATK, BG); d.drawString(fr, 8, y + 2);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(TR("frame TX", "frames TX"), 8, y + 34);
        int seen = nucleo_wifiatk_clients(), kick = seen - nucleo_wifiatk_clients_active(); if (kick < 0) kick = 0;
        char ln[40]; snprintf(ln, sizeof ln, TR("KICK %d  att %d  ch%d", "KICK %d  act %d  ch%d"), kick, nucleo_wifiatk_clients_active(), nucleo_wifiatk_cur_channel());
        d.setTextColor(GRN, BG); d.drawString(ln, 8, y + 48);
        char ss[40]; snprintf(ss, sizeof ss, "> %.30s", nucleo_wifiatk_cur_ssid());
        d.setTextColor(FG, BG); d.drawString(ss, 8, y + 62);
        lg::comp_keys(TR("INVIO ferma  TAB guida  ESC on", "ENTER stop  TAB guide  ESC on")); return;
    }
    if (s_state == ST_CONSENT || s_state == ST_SCAN || s_state == ST_STOPPING) {
        d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(ATK, BG);
        d.drawString(s_state == ST_CONSENT ? TR("Autorizza", "Authorize") : s_state == ST_SCAN ? TR("Scansione...", "Scanning...") : TR("Arresto...", "Stopping..."), 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        lg::wrap(s_state == ST_CONSENT ? TR("Accetta sul pannello per scansionare.", "Accept on the panel to scan.")
                                       : TR("Attendi...", "Please wait..."), 8, y + 34, 224, 12, 2);
        lg::comp_keys(TR("INVIO accetto  ESC esci  TAB guida", "ENTER accept  ESC back  TAB guide")); return;
    }
    // ST_LIST
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(ATK, BG);
    const char *sel = s_sel == 0 ? TR("TUTTE", "ALL") : nucleo_wifiatk_target_ssid(s_sel - 1);
    char t[32]; snprintf(t, sizeof t, "%.24s", sel && sel[0] ? sel : "(hidden)");
    d.drawString(t, 8, y + 4);
    d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
    char m[32]; snprintf(m, sizeof m, TR("%d reti trovate", "%d networks found"), s_n);
    d.drawString(m, 8, y + 34);
    if (s_sel > 0) { char info[40]; snprintf(info, sizeof info, "c%d  %s  %ddB", nucleo_wifiatk_target_channel(s_sel - 1),
                     nucleo_wifiatk_target_auth(s_sel - 1), nucleo_wifiatk_target_rssi(s_sel - 1));
                     d.setTextColor(FG, BG); d.drawString(info, 8, y + 50); }
    lg::comp_keys(TR(";/. scegli  INVIO arma  r riscan", ";/. pick  ENTER arm  r rescan"));
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_panel)                     draw_guide();
    else if (s_state == ST_CONSENT)  draw_consent(top, h);
    else if (s_state == ST_SCAN)     { draw_busy(TR("Scansione...","Scanning..."), TR("Cerco le reti vicine.","Looking for nearby networks."), h); s_scan_armed = true; }
    else if (s_state == ST_STOPPING) { draw_busy(TR("Arresto...","Stopping..."), TR("Ripristino rete OS.","Restoring OS network."), h); s_stop_armed = true; }
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
        'D', ATK, enter, on_key, tick, draw, leave, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
