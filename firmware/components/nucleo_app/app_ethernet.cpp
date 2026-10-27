// Ethernet (W5500) wired-attack app — on-device UI for the nucleo_eth Layer-2/3 engine.
//
// AUTHORIZED USE ONLY. This drives wired ARP/DHCP/MAC attacks of the kind Bruce's "Ethernet" menu
// ships — but it runs where Bruce can't (no-PSRAM Cardputer) because the engine uses the W5500 in
// MACRAW with no lwIP, and it does more: a stack-free DHCP self-config, OUI fingerprinting, a live
// network map, a rogue-DHCP MITM, on-SD PCAP, and REVERSIBLE ARP attacks that heal the caches on stop.
// The app opens on a consent screen and refuses to act until the operator acknowledges it.
//
// Hardware: needs a W5500 SPI module wired to the shared microSD bus + a CS GPIO (see nucleo_eth/
// nucleo_w5500). With no module the app shows a connect screen instead of pretending.
//
// Controls (the launcher routes Left/Back to our handler; Up/Down/Enter + a few letter keys drive it):
//   Consent : Enter accept, Esc leave, TAB guide
//   Menu    : Up/Down pick, Enter run, TAB guide
//   Hosts   : Up/Down pick, Enter = MITM host<->gateway, m = map/list, Left = back
//   Running : Enter stop (heals + frees), Esc = leave running in background
#include "nucleo_app.h"
#include "app_ui.h"
#include "app_large.h"          // two screens: the engine's UI on the 320x240 panel, a status companion
#include "nucleo_touch.h"
#include "nucleo_eth.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"

#include "app_gfx.h"
#include "nucleo_i18n.h"        // TR(it,en): hints follow the system language

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, LINE = 0x2945,
                            INK = 0x0000, ATK = 0xFD20 /*orange*/, GRN = 0x8FF3, YEL = 0xFE8C,
                            CYN = 0x5C9F /*blue*/, COL_RED = 0xF9A6;

enum { ST_CONSENT, ST_NOMODULE, ST_MENU, ST_SCANNING, ST_HOSTS, ST_RUNNING, ST_STOPPING };
static int  s_state;
static bool s_consented;
static bool s_begun;
static int  s_sel;          // menu row or host index
static int  s_msel;         // selected host in the map/list
static bool s_map;          // hosts view: map vs list
static bool s_panel;        // TAB guide overlay
static int  s_page;
static bool s_stop_armed;
static char s_err[44];
static bool s_pulse;
static uint32_t s_last_s;
static int  s_menu_top;     // panel list scroll (top row) for the menu / host list on the 320x240 screen
static int  s_host_top;

#define GUIDE_PAGES 5

// ---- menu model -------------------------------------------------------------
struct MenuItem { const char *label; const char *label_en; char glyph; };
static const MenuItem MENU[] = {
    { "Scansiona rete",     "Scan network",      'S' },   // 0
    { "Mappa & host",       "Map & hosts",       'M' },   // 1
    { "ARP poison gateway", "ARP poison gateway",'P' },   // 2
    { "DHCP starvation",    "DHCP starvation",   'D' },   // 3
    { "MAC flooding",       "MAC flooding",      'F' },   // 4
    { "Rogue DHCP (MITM)",  "Rogue DHCP (MITM)", 'R' },   // 5
    { "Cattura PCAP",       "PCAP capture",      'C' },   // 6
};
#define MENU_N ((int)(sizeof(MENU)/sizeof(MENU[0])))

static void set_hint(void)
{
    if (s_panel)                       nucleo_app_set_hint(TR5("su/giu pagina   invio chiudi", "up/dn page   enter close", "arr/abj pag.   enter cerr.", "haut/bas page   entree f.", "auf/ab seite   enter sch."));
    else switch (s_state) {
        case ST_CONSENT:   nucleo_app_set_hint(TR5("invio accetto   esc esci   tab guida", "enter accept   esc back   tab guide", "enter acepto   esc volver   tab guia", "entree accepte   esc ret.   tab guide", "enter akzept.   esc zur.   tab anleit.")); break;
        case ST_NOMODULE:  nucleo_app_set_hint(TR5("collega W5500  premi un tasto", "connect W5500   press a key to detect", "conecta W5500   pulsa tecla detectar", "branche W5500   appuyez touche detecter", "verbinde W5500   drucke taste erkennen")); break;
        case ST_SCANNING:  nucleo_app_set_hint(TR5("scansione in corso...", "scanning...", "escaneando...", "balayage...", "scanning...")); break;
        case ST_STOPPING:  nucleo_app_set_hint(TR5("arresto + ripristino...", "stopping + restoring...", "deteniendo + restaur...", "stop + restore...", "stop + restore...")); break;
        case ST_HOSTS:     nucleo_app_set_hint(TR5("su/giu  invio MITM  m mappa  esc", "up/dn   enter MITM   m map   esc back", "arr/abj   enter MITM   m map   esc vol.", "h/b   entree MITM   m carte   esc ret.", "auf/ab   enter MITM   m karte   esc z.")); break;
        case ST_RUNNING:   nucleo_app_set_hint(TR5("invio: stop+ripr  esc: lascia on", "enter: stop+restore   esc: leave on", "enter: detn.+rest.   esc: dejar act.", "entree: arret+r.   esc: laisser act.", "enter: stop+restore   esc: stay on")); break;
        default:           nucleo_app_set_hint(TR5("su/giu   invio avvia   tab guida", "up/dn   enter start   tab guide", "arr/abj   enter inicia   tab guia", "h/b   entree demarrer   tab guide", "auf/ab   enter start   tab anleit.")); break;
    }
}

static const char *op_name(nucleo_eth_op_t op)
{
    switch (op) {
        case ETH_OP_MITM:        return "MITM";
        case ETH_OP_POISON:      return "ARP POISON";
        case ETH_OP_DHCP_STARVE: return "DHCP STARVE";
        case ETH_OP_MACFLOOD:    return "MAC FLOOD";
        case ETH_OP_ROGUE_DHCP:  return "ROGUE DHCP";
        case ETH_OP_PCAP:        return "PCAP";
        default:                 return "—";
    }
}

static void eth_tab(void);
static bool eth_back(int key);

static void enter(void)
{
    s_err[0] = 0; s_panel = false;
    if (nucleo_eth_current() != ETH_OP_NONE) {
        s_state = ST_RUNNING;                       // resume the live screen of a background attack
    } else {
        if (!s_begun || !nucleo_eth_present()) { s_begun = true; nucleo_eth_begin(); }  // detect / hot-plug retry
        if (!nucleo_eth_present())      s_state = ST_NOMODULE;
        else if (!s_consented)          s_state = ST_CONSENT;
        else                            s_state = ST_MENU;
    }
    nucleo_app_set_tab_handler(eth_tab);
    nucleo_app_set_back_handler(eth_back);
    set_hint();
    nucleo_app_request_draw();
}

static void eth_tab(void)
{
    if (s_state == ST_SCANNING || s_state == ST_STOPPING) return;
    s_panel = !s_panel;
    set_hint();
    nucleo_app_request_draw();
}

// Left/Back: unwind one nesting level instead of closing the app where it makes sense.
static bool eth_back(int key)
{
    (void)key;
    if (s_panel)            { s_panel = false; set_hint(); nucleo_app_request_draw(); return true; }
    if (s_state == ST_HOSTS){ s_state = ST_MENU; set_hint(); nucleo_app_request_draw(); return true; }
    return false;   // consent/menu/nomodule/running -> let the framework close the app
}

static void fail(const char *msg) { snprintf(s_err, sizeof s_err, "%s", msg); nucleo_app_request_draw(); }

static void start_running(int rc) { if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; } else fail(TR("Avvio fallito","Start failed")); set_hint(); nucleo_app_request_draw(); }

static void activate_menu(void)
{
    s_err[0] = 0;
    switch (s_sel) {
        case 0: if (nucleo_eth_scan_start() == 0) { s_state = ST_SCANNING; set_hint(); } else fail(TR("Scan fallito","Scan failed")); break;
        case 1: if (nucleo_eth_host_count() > 0) { s_state = ST_HOSTS; s_map = false; s_msel = 0; set_hint(); }
                else fail(TR("Scansiona prima la rete","Scan the network first"));
                break;
        case 2: { uint32_t gw = nucleo_eth_gateway_ip();
                  if (!gw) fail(TR("Gateway ignoto: scansiona","Gateway unknown: scan")); else start_running(nucleo_eth_poison_start(gw)); } break;
        case 3: start_running(nucleo_eth_dhcp_starve_start()); break;
        case 4: start_running(nucleo_eth_macflood_start()); break;
        case 5:
            if (!nucleo_eth_has_identity()) fail(TR("Serve identita': scansiona","Identity needed: scan"));
            else start_running(nucleo_eth_rogue_dhcp_start());
            break;
        case 6: start_running(nucleo_eth_pcap_start(nullptr)); break;
    }
    nucleo_app_request_draw();
}

static void start_mitm_on_selected(void)
{
    uint32_t victim = nucleo_eth_host_ip_raw(s_msel);
    uint32_t gw = nucleo_eth_gateway_ip();
    if (!gw)            { fail(TR("Gateway ignoto","Gateway unknown")); return; }
    if (victim == gw)   { fail(TR("Scegli un host != gateway","Pick a host != gateway")); return; }
    if (!victim)        { fail(TR("Host non valido","Invalid host")); return; }
    start_running(nucleo_eth_mitm_start(victim, gw));
}

static void tick(void)
{
    if (s_state == ST_SCANNING) {
        if (nucleo_eth_current() == ETH_OP_NONE || nucleo_eth_scan_progress() >= 100) {
            s_state = (nucleo_eth_host_count() > 0) ? ST_HOSTS : ST_MENU;
            s_map = false; s_msel = 0;
            if (nucleo_eth_host_count() == 0) fail(TR("Nessun host (DHCP? cavo?)","No host (DHCP? cable?)"));
            set_hint();
        }
        nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;
        nucleo_eth_stop();
        s_state = ST_MENU; set_hint();
        nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_RUNNING || s_state == ST_SCANNING) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
        if (t != s_last_s) { s_last_s = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
    }
}

static void on_key(int key, char ch)
{
    if (s_panel) {
        if (key == NK_UP)        s_page = (s_page + GUIDE_PAGES - 1) % GUIDE_PAGES;
        else if (key == NK_DOWN) s_page = (s_page + 1) % GUIDE_PAGES;
        else if (key == NK_ENTER){ s_panel = false; set_hint(); }
        nucleo_app_request_draw();
        return;
    }
    switch (s_state) {
        case ST_CONSENT:
            if (key == NK_ENTER) { s_consented = true; s_state = nucleo_eth_present() ? ST_MENU : ST_NOMODULE; set_hint(); nucleo_app_request_draw(); }
            return;
        case ST_NOMODULE:
            nucleo_eth_begin();
            s_state = nucleo_eth_present() ? (s_consented ? ST_MENU : ST_CONSENT) : ST_NOMODULE;
            set_hint(); nucleo_app_request_draw();
            return;
        case ST_SCANNING:
        case ST_STOPPING:
            return;   // busy
        case ST_RUNNING:
            if (key == NK_ENTER) { s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw(); }
            return;
        case ST_HOSTS: {
            int n = nucleo_eth_host_count();
            if (nucleo_touch_is_row(key, ch)) {                      // tap a host row: pick it, tap again = MITM
                int i = ch - NUCLEO_TOUCH_ROW + s_host_top;
                if (i >= 0 && i < n) { if (i == s_msel) start_mitm_on_selected(); else s_msel = i; }
            }
            else if (key == NK_UP)        s_msel = (s_msel - 1 + (n?n:1)) % (n?n:1);
            else if (key == NK_DOWN) s_msel = (s_msel + 1) % (n?n:1);
            else if (key == NK_ENTER) start_mitm_on_selected();
            else if (key == NK_CHAR && (ch == 'm' || ch == 'M')) s_map = !s_map;
            nucleo_app_request_draw();
            return;
        }
        default:  // ST_MENU
            if (nucleo_touch_is_row(key, ch)) {                      // tap a menu row: select, tap again = run
                int i = ch - NUCLEO_TOUCH_ROW + s_menu_top;
                if (i >= 0 && i < MENU_N) { if (i == s_sel) activate_menu(); else s_sel = i; }
            }
            else if (key == NK_UP)        s_sel = (s_sel - 1 + MENU_N) % MENU_N;
            else if (key == NK_DOWN) s_sel = (s_sel + 1) % MENU_N;
            else if (key == NK_ENTER) activate_menu();
            nucleo_app_request_draw();
            return;
    }
}

// ---- drawing ---------------------------------------------------------------
static void id_line(int y)
{
    char b[42];
    snprintf(b, sizeof b, "io %s  gw %s", nucleo_eth_my_ip_str(), nucleo_eth_gateway_str());
    d.setTextSize(1); d.setTextColor(nucleo_eth_has_identity() ? GRN : MUTED, BG); d.setCursor(10, y); d.print(b);
}

static void draw_consent(int h)
{
    app_ui_title("Ethernet W5500", ATK, "AUTH");
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print(TR("Test autorizzati","Authorized testing"));
    const char *L_it[] = { "Attacchi L2/L3 cablati:", "scan, ARP MITM/poison,", "DHCP starve, MAC flood,",
                        "rogue DHCP, PCAP.", "Solo reti tue o con", "permesso scritto." };
    const char *L_en[] = { "Wired L2/L3 attacks:", "scan, ARP MITM/poison,", "DHCP starve, MAC flood,",
                        "rogue DHCP, PCAP.", "Your own networks or with", "written permission." };
    const char *const *L = nucleo_i18n_is_it() ? L_it : L_en;
    unsigned short C[] = { FG, FG, FG, FG, MUTED, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) { d.setTextColor(C[i], BG); d.setCursor(10, 52 + i * 11); d.print(L[i]); }
    (void)h;
}

static void draw_nomodule(int h)
{
    app_ui_title("Ethernet W5500", ATK, "");
    d.setTextSize(2); d.setTextColor(COL_RED, BG); d.setCursor(10, 28); d.print("Modulo assente");
    const char *L[] = { "Collega un W5500 SPI:", "SCLK40 MOSI14 MISO39", "CS->G1 (Grove)  saldato", "sul bus microSD condiviso." };
    d.setTextSize(1);
    for (int i = 0; i < 4; i++) { d.setTextColor(i==0?FG:MUTED, BG); d.setCursor(10, 56 + i * 11); d.print(L[i]); }
    (void)h;
}

static void list_row(int y, bool focus, const char *label, const char *right, unsigned short dot)
{
    if (focus) {
        d.fillRoundRect(6, y, 228, 24, 6, ATK);
        d.setTextSize(2); d.setTextColor(INK, ATK); d.setCursor(14, y + 4);
        char b[18]; snprintf(b, sizeof b, "%.15s", label); d.print(b);
        if (right && right[0]) { int w = (int)strlen(right) * 6; d.setTextSize(1); d.setTextColor(INK, ATK); d.setCursor(226 - w, y + 14); d.print(right); }
    } else {
        if (dot) d.fillCircle(13, y + 12, 3, dot);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 6);
        char b[30]; snprintf(b, sizeof b, "%.26s", label); d.print(b);
        if (right && right[0]) { int w = (int)strlen(right) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 6); d.print(right); }
    }
}

static void draw_menu(int h)
{
    char rt[14]; snprintf(rt, sizeof rt, "%d host", nucleo_eth_host_count());
    app_ui_title("Ethernet W5500", ATK, rt);
    id_line(24);
    const int rowh = 24, y0 = 36;
    int vis = (h - y0 + 8) / rowh; if (vis < 1) vis = 1;
    int first = s_sel - vis / 2; if (first > MENU_N - vis) first = MENU_N - vis; if (first < 0) first = 0;
    for (int r = 0; r < vis && first + r < MENU_N; r++) {
        int idx = first + r, y = y0 + r * rowh;
        list_row(y, idx == s_sel, TR(MENU[idx].label, MENU[idx].label_en), "", CYN);
    }
    if (s_err[0]) { d.setTextSize(1); d.setTextColor(COL_RED, BG); d.setCursor(10, h - 2); d.print(s_err); }
}

static void draw_scanning(int h)
{
    app_ui_title(TR("Scansione ARP","ARP scan"), ATK, "");
    int p = nucleo_eth_scan_progress();
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, 34); char b[8]; snprintf(b, sizeof b, "%d%%", p); d.print(b);
    d.drawRoundRect(10, 60, 220, 14, 4, LINE);
    d.fillRoundRect(12, 62, (216 * p) / 100, 10, 3, GRN);
    char st[40]; snprintf(st, sizeof st, "host %d   tx %lu   rx %lu", nucleo_eth_host_count(), nucleo_eth_tx_frames(), nucleo_eth_rx_frames());
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 84); d.print(st);
    if (!nucleo_eth_has_identity()) { d.setTextColor(YEL, BG); d.setCursor(10, h - 10); d.print(TR("acquisisco lease DHCP...","acquiring DHCP lease...")); }
    (void)h;
}

static void draw_hosts_list(int h)
{
    int n = nucleo_eth_host_count();
    char rt[12]; snprintf(rt, sizeof rt, "%d host", n);
    app_ui_title(TR("Host trovati","Hosts found"), ATK, rt);
    const int rowh = 24, y0 = 26;
    int vis = (h - y0) / rowh; if (vis < 1) vis = 1;
    int first = s_msel - vis / 2; if (first > n - vis) first = n - vis; if (first < 0) first = 0;
    for (int r = 0; r < vis && first + r < n; r++) {
        int idx = first + r, y = y0 + r * rowh;
        char lbl[28]; snprintf(lbl, sizeof lbl, "%s", nucleo_eth_host_ip(idx));
        char info[24]; snprintf(info, sizeof info, "%s%s", nucleo_eth_host_vendor(idx), nucleo_eth_host_is_gateway(idx) ? " GW" : "");
        list_row(y, idx == s_msel, lbl, info, nucleo_eth_host_is_gateway(idx) ? YEL : GRN);
    }
    if (n == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, h - 10); d.print(TR("nessun host","no hosts")); }
    else if (s_err[0]) { d.setTextSize(1); d.setTextColor(COL_RED, BG); d.setCursor(10, h - 10); d.print(s_err); }
}

// Live network map: gateway hub at top, YOU at bottom, hosts as a row of dots; selected host links
// through YOU (the MITM picture). Smartwatch-style: one glance shows topology + who you'd intercept.
static void draw_hosts_map(int h)
{
    int n = nucleo_eth_host_count();
    app_ui_title(TR("Mappa rete","Network map"), ATK, "");
    int gx = 120, gy = 40, yx = 120, yy = h - 8;
    // gateway hub
    d.fillRoundRect(gx - 28, gy - 12, 56, 22, 5, YEL);
    d.setTextSize(1); d.setTextColor(INK, YEL); d.setCursor(gx - 24, gy - 5); d.print("GATEWAY");
    // you node
    d.fillRoundRect(yx - 18, yy - 18, 36, 16, 5, CYN);
    d.setTextColor(INK, CYN); d.setCursor(yx - 8, yy - 14); d.print("IO");
    d.drawLine(yx, yy - 18, gx, gy + 10, DIM);   // you<->gateway
    // host dots in an arc
    int cols = n < 1 ? 1 : n; if (cols > 8) cols = 8;
    for (int i = 0; i < n && i < 8; i++) {
        int hx = 24 + i * (192 / 8), hy = 78;
        bool sel = (i == s_msel);
        unsigned short c = nucleo_eth_host_is_gateway(i) ? YEL : (sel ? ATK : GRN);
        d.drawLine(hx, hy, gx, gy + 10, sel ? ATK : LINE);
        if (sel) d.drawLine(hx, hy, yx, yy - 18, ATK);    // selected host routed through YOU = MITM
        d.fillCircle(hx, hy, sel ? 6 : 4, c);
    }
    // selected host caption
    if (n > 0) {
        char b[40]; snprintf(b, sizeof b, "%s %s%s", nucleo_eth_host_ip(s_msel), nucleo_eth_host_vendor(s_msel),
                             nucleo_eth_host_is_gateway(s_msel) ? " (GW)" : "");
        d.setTextSize(1); d.setTextColor(FG, BG); d.setCursor(10, h - 2); d.print(b);
    }
    if (s_err[0]) { d.setTextColor(COL_RED, BG); d.setCursor(10, h - 12); d.print(s_err); }
    (void)cols;
}

static void big(int cx, const char *label, const char *val, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(cx - (int)strlen(label) * 3, 40); d.print(label);
    d.setTextSize(3); d.setTextColor(col, BG); d.setCursor(cx - (int)strlen(val) * 9, 52); d.print(val);
}

static void draw_running(int h)
{
    nucleo_eth_op_t op = nucleo_eth_current();
    app_ui_title(op_name(op), ATK, "");
    if (s_pulse) d.fillCircle(150, 9, 4, ATK);
    d.setTextSize(1); d.setTextColor(ATK, BG); d.setCursor(160, 7); d.print("LIVE");
    unsigned up = nucleo_eth_uptime_s();
    char el[10]; snprintf(el, sizeof el, "%02u:%02u", up / 60, up % 60);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(el) * 6, 7); d.print(el);

    char fr[12]; unsigned long f = nucleo_eth_tx_frames();
    if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);

    // op-specific hero numbers + a one-line honest verdict
    const char *verdict; unsigned short vc = GRN;
    char sub[44] = {0};
    switch (op) {
        case ETH_OP_PCAP: {
            char kb[12]; unsigned long by = nucleo_eth_pcap_bytes();
            if (by >= 1024) snprintf(kb, sizeof kb, "%luK", by / 1024); else snprintf(kb, sizeof kb, "%lu", by);
            big(62, "FRAME", fr, CYN); big(178, "RX", kb, GRN);
            verdict = TR("cattura -> SD /capture.pcap","capturing -> SD /capture.pcap"); vc = GRN;
            break;
        }
        case ETH_OP_ROGUE_DHCP: {
            char lz[8]; snprintf(lz, sizeof lz, "%d", nucleo_eth_rogue_leases());
            big(62, "OFFER", fr, CYN); big(178, "LEASE", lz, GRN);
            verdict = TR("router+DNS -> me (MITM)","router+DNS -> me (MITM)"); vc = nucleo_eth_rogue_leases() ? GRN : MUTED;
            break;
        }
        default: {
            char rx[12]; unsigned long r = nucleo_eth_rx_frames();
            if (r >= 100000) snprintf(rx, sizeof rx, "%luk", r / 1000); else snprintf(rx, sizeof rx, "%lu", r);
            big(62, "TX", fr, ATK); big(178, "RX", rx, CYN);
            if (op == ETH_OP_MITM)             { verdict = TR("intercetto host<->gw (reversibile)","intercepting host<->gw (reversible)"); vc = GRN; }
            else if (op == ETH_OP_POISON)      { verdict = TR("gateway dirottato (reversibile)","gateway hijacked (reversible)"); vc = ATK; }
            else if (op == ETH_OP_DHCP_STARVE) { verdict = TR("esaurisco il pool DHCP","draining the DHCP pool"); vc = ATK; }
            else if (op == ETH_OP_MACFLOOD)    { verdict = TR("CAM overflow -> hub mode","CAM overflow -> hub mode"); vc = ATK; }
            else                               { verdict = TR("in corso...","running..."); vc = YEL; }
            break;
        }
    }
    d.drawFastHLine(10, 84, 220, LINE);
    if (sub[0]) { d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 92); d.print(sub); }
    char st[52]; snprintf(st, sizeof st, TR("link %s   io %s","link %s   me %s"), nucleo_eth_link() ? "UP" : TR("giu","down"), nucleo_eth_my_ip_str());
    d.setTextSize(1); d.setTextColor(nucleo_eth_link() ? MUTED : COL_RED, BG); d.setCursor(10, 92); d.print(st);
    d.setTextColor(vc, BG); d.setCursor(10, h - 9); d.print(verdict);
}

static void draw_busy(const char *title, const char *msg, int h)
{
    app_ui_title("Ethernet W5500", ATK, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print(title);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6); d.print(msg);
}

// ---- guide ------------------------------------------------------------------
static const char *GH_IT[GUIDE_PAGES] = { "Cos'e'", "Hardware", "Scan & mappa", "Attacchi", "Comandi" };
static const char *GH_EN[GUIDE_PAGES] = { "What", "Hardware", "Scan & map", "Attacks", "Keys" };
#define GH (nucleo_i18n_is_it() ? GH_IT : GH_EN)
static const char *GB_IT[GUIDE_PAGES][6] = {
    { "Attacchi L2/L3 cablati via", "modulo W5500 in MACRAW.", "Niente lwIP: forgiamo i", "frame a mano -> gira sul",
      "Cardputer dove Bruce no.", "Una sola op alla volta." },
    { "W5500 SPI sul bus microSD:", "SCLK40 MOSI14 MISO39 +", "CS su G1 (Grove), saldato.", "Senza modulo l'app lo dice",
      "e non finge.", "" },
    { "Scan: DHCP self-config poi", "ARP sweep del /24.", "Vendor da OUI offline.", "Mappa: GATEWAY in alto, IO",
      "in basso, host a raggiera.", "Invio su host = MITM." },
    { "MITM/poison sono REVERSIBILI:", "all'uscita ripristino le", "cache ARP (Bruce no).", "Rogue DHCP: router+DNS->me.",
      "PCAP: cattura su SD.", "Solo reti autorizzate." },
    { "su/giu  scegli", "invio   avvia / MITM / ferma", "m       mappa/lista", "sx      indietro",
      "esc     lascia attivo (bg)", "tab     questa guida" },
};
static const char *GB_EN[GUIDE_PAGES][6] = {
    { "Wired L2/L3 attacks via", "the W5500 in MACRAW.", "No lwIP: we forge the", "frames by hand -> runs on",
      "the Cardputer where Bruce can't.", "One op at a time." },
    { "W5500 SPI on the microSD bus:", "SCLK40 MOSI14 MISO39 +", "CS on G1 (Grove), soldered.", "Without the module the app says so",
      "and does not fake it.", "" },
    { "Scan: DHCP self-config then", "ARP sweep of the /24.", "Vendor from offline OUI.", "Map: GATEWAY on top, ME",
      "below, hosts fanned out.", "Enter on a host = MITM." },
    { "MITM/poison are REVERSIBLE:", "on exit we restore the", "ARP caches (Bruce doesn't).", "Rogue DHCP: router+DNS->me.",
      "PCAP: capture to SD.", "Authorized networks only." },
    { "up/dn  pick", "enter   start / MITM / stop", "m       map/list", "left    back",
      "esc     leave on (bg)", "tab     this guide" },
};
#define GB (nucleo_i18n_is_it() ? GB_IT : GB_EN)
static void draw_guide(void)
{
    char rt[10]; snprintf(rt, sizeof rt, "%d/%d", s_page + 1, GUIDE_PAGES);
    app_ui_title("Guida", ATK, rt);
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 26); d.print(GH[s_page]);
    d.setTextSize(1);
    for (int i = 0; i < 6; i++) {
        const char *l = GB[s_page][i]; if (!l[0]) continue;
        bool legal = (s_page == GUIDE_PAGES - 1 && i >= 4) || (s_page == 3 && i >= 5);
        d.setTextColor(legal ? YEL : FG, BG); d.setCursor(10, 46 + i * 11); d.print(l);
    }
}

// ===== two screens ==============================================================================
// The engine's UI, laid out for the 320x240 touch panel: the consent gate, the attack menu (tap a row,
// tap again to run), the scan progress, the host list / live network map, and the running screen with big
// hero numbers and a STOP button. The header carries a back chevron and the guide glyph. The Cardputer,
// where the keyboard is, becomes a status companion: the state, what is selected, the live op + uptime.
static void eth_chevron(int x, int y, unsigned short c)
{
    d.drawLine(x + 6, y, x, y + 6, c); d.drawLine(x + 7, y, x + 1, y + 6, c);
    d.drawLine(x, y + 6, x + 6, y + 12, c); d.drawLine(x + 1, y + 6, x + 7, y + 12, c);
}
// A native header in the app's own palette (so the body's BG never two-tones against lg's THEME_BG).
// back != 0 adds a chevron (-> that key); menu != 0 adds the stacked guide glyph (-> that key).
static int eth_lg_header(const char *title, const char *right, int back, int menu)
{
    const int W = d.width(), HEAD = 30;
    d.fillRect(0, 0, W, HEAD, BG);
    int tx = 10;
    if (back) { eth_chevron(10, HEAD / 2 - 7, ATK); nucleo_touch_add(0, 0, 90, HEAD, back, 0); tx = 28; }
    int rx = W - 10;
    if (menu) { for (int k = 0; k < 3; k++) d.fillRoundRect(W - 26, HEAD / 2 - 6 + k * 5, 16, 2, 1, MUTED);
                nucleo_touch_add(W - 46, 0, 46, HEAD, menu, 0); rx = W - 36; }
    if (right && right[0]) {
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(middle_right); d.setTextColor(MUTED, BG);
        d.drawString(right, rx, HEAD / 2); d.setTextDatum(top_left);
    }
    lg::font(1); d.setTextColor(ATK, BG); d.setTextDatum(middle_left);
    char t[48]; lg::fit(t, sizeof t, title, rx - tx - 40); d.drawString(t, tx, HEAD / 2 + 1);
    d.setTextDatum(top_left);
    d.drawFastHLine(10, HEAD - 2, W - 20, LINE);
    return HEAD;
}

static void lg_consent(int W, int H)
{
    eth_lg_header("Ethernet W5500", "AUTH", NK_BACK, NK_TAB);
    int y = 36;
    d.setFont(&fonts::Font0);
    lg::font(2); d.setTextColor(YEL, BG); d.setTextDatum(top_left);
    d.drawString(TR("Test autorizzati", "Authorized testing"), 12, y); y += 26;
    const char *L_it[] = { "Attacchi L2/L3 cablati: scan, ARP", "MITM/poison, DHCP starve, MAC flood,",
                           "rogue DHCP, PCAP.", "Solo reti tue o con permesso scritto." };
    const char *L_en[] = { "Wired L2/L3 attacks: scan, ARP MITM/", "poison, DHCP starve, MAC flood,",
                           "rogue DHCP, PCAP.", "Your own networks or written consent." };
    const char *const *L = nucleo_i18n_is_it() ? L_it : L_en;
    lg::font(0);
    for (int i = 0; i < 4; i++) { d.setTextColor(i < 3 ? FG : MUTED, BG); d.drawString(L[i], 12, y); y += 18; }
    const int bw = (W - 36) / 2, by = H - 52;
    lg::button(12, by, bw, 40, TR("Esci", "Leave"), LINE, FG, NK_BACK, 0);
    lg::button(24 + bw, by, bw, 40, TR("Accetto", "Accept"), ATK, INK, NK_ENTER, 0);
}

static void lg_nomodule(int W, int H)
{
    eth_lg_header("Ethernet W5500", nullptr, NK_BACK, 0);
    int y = 40;
    lg::font(2); d.setTextColor(COL_RED, BG); d.setTextDatum(top_left);
    d.drawString(TR("Modulo assente", "Module absent"), 12, y); y += 28;
    const char *L[] = { TR("Collega un W5500 SPI sul bus microSD:", "Connect a W5500 SPI on the microSD bus:"),
                        "SCLK 40   MOSI 14   MISO 39", TR("CS -> G1 (Grove), saldato.", "CS -> G1 (Grove), soldered.") };
    lg::font(0);
    for (int i = 0; i < 3; i++) { d.setTextColor(i == 0 ? FG : MUTED, BG); d.drawString(L[i], 12, y); y += 18; }
    lg::button(12, H - 52, W - 24, 40, TR("Rileva ora", "Detect now"), ATK, INK, NK_ENTER, 0);
}

static void lg_menu(int W, int H)
{
    char rt[14]; snprintf(rt, sizeof rt, "%d host", nucleo_eth_host_count());
    int y = eth_lg_header("Ethernet W5500", rt, NK_BACK, NK_TAB);
    char idb[42]; snprintf(idb, sizeof idb, "io %s  gw %s", nucleo_eth_my_ip_str(), nucleo_eth_gateway_str());
    lg::font(0); d.setTextColor(nucleo_eth_has_identity() ? GRN : MUTED, BG); d.setTextDatum(top_left);
    char idf[48]; lg::fit(idf, sizeof idf, idb, W - 20); d.drawString(idf, 12, y + 4);
    const int y0 = y + 22, rowh = 26, avail = H - y0 - (s_err[0] ? 18 : 6);
    int vis = avail / rowh; if (vis > MENU_N) vis = MENU_N;
    if (s_sel < s_menu_top) s_menu_top = s_sel;
    if (s_sel >= s_menu_top + vis) s_menu_top = s_sel - vis + 1;
    if (s_menu_top > MENU_N - vis) s_menu_top = MENU_N - vis;
    if (s_menu_top < 0) s_menu_top = 0;
    for (int r = 0; r < vis; r++) {
        int idx = s_menu_top + r, ry = y0 + r * rowh; bool on = idx == s_sel;
        lg::panel(10, ry, W - 20, rowh - 4, on ? ATK : lg::card());
        lg::font(1); d.setTextDatum(middle_left); d.setTextColor(on ? INK : FG, on ? ATK : lg::card());
        char b[28]; lg::fit(b, sizeof b, TR(MENU[idx].label, MENU[idx].label_en), W - 48);
        d.drawString(b, 22, ry + (rowh - 4) / 2);
        nucleo_touch_add(10, ry, W - 20, rowh - 4, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + r));
    }
    d.setTextDatum(top_left);
    if (s_err[0]) { lg::font(0); d.setTextColor(COL_RED, BG); d.drawString(s_err, 12, H - 16); }
}

static void lg_scanning(int W, int H)
{
    eth_lg_header(TR("Scansione ARP", "ARP scan"), nullptr, NK_BACK, 0);
    int p = nucleo_eth_scan_progress();
    lg::font(3); d.setTextColor(FG, BG); d.setTextDatum(top_center);
    char b[8]; snprintf(b, sizeof b, "%d%%", p); d.drawString(b, W / 2, 56);
    d.setTextDatum(top_left);
    lg::bar(20, 112, W - 40, 14, p, GRN);
    char st[48]; snprintf(st, sizeof st, "host %d   tx %lu   rx %lu", nucleo_eth_host_count(), nucleo_eth_tx_frames(), nucleo_eth_rx_frames());
    lg::font(0); d.setTextColor(MUTED, BG); d.setTextDatum(top_center); d.drawString(st, W / 2, 140);
    if (!nucleo_eth_has_identity()) { d.setTextColor(YEL, BG); d.drawString(TR("acquisisco lease DHCP...", "acquiring DHCP lease..."), W / 2, 162); }
    d.setTextDatum(top_left);
    (void)H;
}

static void lg_hosts_list(int W, int H)
{
    int n = nucleo_eth_host_count();
    int y = eth_lg_header(TR("Host trovati", "Hosts found"), nullptr, NK_BACK, 0);   // count shown on the toggle side
    lg::button(W - 92, 2, 82, 26, s_map ? TR("Lista", "List") : TR("Mappa", "Map"), lg::card(), FG, NK_CHAR, 'm');
    const int y0 = y + 4, rowh = 30, avail = H - y0 - 4;
    int vis = avail / rowh; if (vis > n) vis = n; if (vis < 1) vis = 1;
    if (s_msel < s_host_top) s_host_top = s_msel;
    if (s_msel >= s_host_top + vis) s_host_top = s_msel - vis + 1;
    if (s_host_top > n - vis) s_host_top = n - vis;
    if (s_host_top < 0) s_host_top = 0;
    for (int r = 0; r < vis; r++) {
        int idx = s_host_top + r, ry = y0 + r * rowh; bool on = idx == s_msel;
        bool gw = nucleo_eth_host_is_gateway(idx);
        lg::panel(10, ry, W - 20, rowh - 4, on ? ATK : lg::card());
        if (!on) d.fillCircle(22, ry + (rowh - 4) / 2, 4, gw ? YEL : GRN);
        lg::font(1); d.setTextDatum(middle_left); d.setTextColor(on ? INK : FG, on ? ATK : lg::card());
        d.drawString(nucleo_eth_host_ip(idx), on ? 22 : 34, ry + (rowh - 4) / 2);
        char info[24]; snprintf(info, sizeof info, "%s%s", nucleo_eth_host_vendor(idx), gw ? " GW" : "");
        lg::font(0); d.setTextDatum(middle_right); d.setTextColor(on ? INK : MUTED, on ? ATK : lg::card());
        char inf[24]; lg::fit(inf, sizeof inf, info, (W - 20) / 2); d.drawString(inf, W - 18, ry + (rowh - 4) / 2);
        nucleo_touch_add(10, ry, W - 20, rowh - 4, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + r));
    }
    d.setTextDatum(top_left);
    if (n == 0) { lg::font(0); d.setTextColor(DIM, BG); d.drawString(TR("nessun host", "no hosts"), 12, y0 + 6); }
}

static void lg_hosts_map(int W, int H)
{
    int n = nucleo_eth_host_count();
    eth_lg_header(TR("Mappa rete", "Network map"), nullptr, NK_BACK, 0);
    lg::button(W - 92, 2, 82, 26, TR("Lista", "List"), lg::card(), FG, NK_CHAR, 'm');
    int gx = W / 2, gy = 64, yx = W / 2, yy = H - 48;
    d.fillRoundRect(gx - 44, gy - 16, 88, 30, 6, YEL);
    lg::font(1); d.setTextColor(INK, YEL); d.setTextDatum(middle_center); d.drawString("GATEWAY", gx, gy - 1);
    d.fillRoundRect(yx - 26, yy - 2, 52, 26, 6, CYN);
    d.setTextColor(INK, CYN); d.drawString("IO", yx, yy + 11);
    d.setTextDatum(top_left);
    d.drawLine(yx, yy - 2, gx, gy + 14, DIM);
    for (int i = 0; i < n && i < 8; i++) {
        int hx = 30 + i * ((W - 60) / 8), hy = 118; bool sel = (i == s_msel);
        unsigned short c = nucleo_eth_host_is_gateway(i) ? YEL : (sel ? ATK : GRN);
        d.drawLine(hx, hy, gx, gy + 14, sel ? ATK : LINE);
        if (sel) d.drawLine(hx, hy, yx, yy - 2, ATK);
        d.fillCircle(hx, hy, sel ? 8 : 5, c);
        nucleo_touch_add(hx - 14, hy - 14, 28, 28, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i)); s_host_top = 0;
    }
    if (n > 0) {
        char b[48]; snprintf(b, sizeof b, "%s %s%s", nucleo_eth_host_ip(s_msel), nucleo_eth_host_vendor(s_msel),
                             nucleo_eth_host_is_gateway(s_msel) ? " (GW)" : "");
        lg::font(0); d.setTextColor(FG, BG); d.setTextDatum(top_left);
        char bf[48]; lg::fit(bf, sizeof bf, b, W - 120); d.drawString(bf, 12, H - 40);
        lg::button(W - 100, H - 44, 88, 34, "MITM", ATK, INK, NK_ENTER, 0);
    }
}

static void lg_big(int cx, int top, const char *label, const char *val, unsigned short col)
{
    lg::font(0); d.setTextColor(MUTED, BG); d.setTextDatum(top_center); d.drawString(label, cx, top);
    lg::font(3); d.setTextColor(col, BG); d.drawString(val, cx, top + 16);
    d.setTextDatum(top_left);
}

static void lg_running(int W, int H)
{
    nucleo_eth_op_t op = nucleo_eth_current();
    char up[12]; unsigned u = nucleo_eth_uptime_s(); snprintf(up, sizeof up, "%02u:%02u", u / 60, u % 60);
    eth_lg_header(op_name(op), up, NK_BACK, 0);
    if (s_pulse) d.fillCircle(W - 120, 15, 4, ATK);
    lg::font(0); d.setTextColor(ATK, BG); d.setTextDatum(top_left); d.drawString("LIVE", W - 112, 11);

    char fr[12]; unsigned long f = nucleo_eth_tx_frames();
    if (f >= 100000) snprintf(fr, sizeof fr, "%luk", f / 1000); else snprintf(fr, sizeof fr, "%lu", f);
    const char *verdict; unsigned short vc = GRN;
    switch (op) {
        case ETH_OP_PCAP: {
            char kb[12]; unsigned long by = nucleo_eth_pcap_bytes();
            if (by >= 1024) snprintf(kb, sizeof kb, "%luK", by / 1024); else snprintf(kb, sizeof kb, "%lu", by);
            lg_big(W / 4, 52, "FRAME", fr, CYN); lg_big(W * 3 / 4, 52, "RX", kb, GRN);
            verdict = TR("cattura -> SD /capture.pcap", "capturing -> SD /capture.pcap"); break;
        }
        case ETH_OP_ROGUE_DHCP: {
            char lz[8]; snprintf(lz, sizeof lz, "%d", nucleo_eth_rogue_leases());
            lg_big(W / 4, 52, "OFFER", fr, CYN); lg_big(W * 3 / 4, 52, "LEASE", lz, GRN);
            verdict = TR("router+DNS -> me (MITM)", "router+DNS -> me (MITM)"); vc = nucleo_eth_rogue_leases() ? GRN : MUTED; break;
        }
        default: {
            char rx[12]; unsigned long r = nucleo_eth_rx_frames();
            if (r >= 100000) snprintf(rx, sizeof rx, "%luk", r / 1000); else snprintf(rx, sizeof rx, "%lu", r);
            lg_big(W / 4, 52, "TX", fr, ATK); lg_big(W * 3 / 4, 52, "RX", rx, CYN);
            if (op == ETH_OP_MITM)             { verdict = TR("intercetto host<->gw (reversibile)", "intercepting host<->gw (reversible)"); }
            else if (op == ETH_OP_POISON)      { verdict = TR("gateway dirottato (reversibile)", "gateway hijacked (reversible)"); vc = ATK; }
            else if (op == ETH_OP_DHCP_STARVE) { verdict = TR("esaurisco il pool DHCP", "draining the DHCP pool"); vc = ATK; }
            else if (op == ETH_OP_MACFLOOD)    { verdict = TR("CAM overflow -> hub mode", "CAM overflow -> hub mode"); vc = ATK; }
            else                               { verdict = TR("in corso...", "running..."); vc = YEL; }
            break;
        }
    }
    d.drawFastHLine(16, 120, W - 32, LINE);
    char st[56]; snprintf(st, sizeof st, TR("link %s   io %s", "link %s   me %s"), nucleo_eth_link() ? "UP" : TR("giu", "down"), nucleo_eth_my_ip_str());
    lg::font(0); d.setTextColor(nucleo_eth_link() ? MUTED : COL_RED, BG); d.setTextDatum(top_left); d.drawString(st, 16, 130);
    lg::font(1); d.setTextColor(vc, BG); char vf[52]; lg::fit(vf, sizeof vf, verdict, W - 32); d.drawString(vf, 16, 152);
    lg::button(16, H - 52, W - 32, 40, TR("STOP + ripristina", "STOP + restore"), COL_RED, 0xFFFF, NK_ENTER, 0);
}

static void lg_busy(const char *title, const char *msg, int W, int H)
{
    eth_lg_header("Ethernet W5500", nullptr, 0, 0);
    lg::message(0, 30, W, H - 30, title, msg, FG);
}

static void lg_guide(int W, int H)
{
    char rt[10]; snprintf(rt, sizeof rt, "%d/%d", s_page + 1, GUIDE_PAGES);
    eth_lg_header(TR("Guida", "Guide"), rt, NK_BACK, 0);
    lg::font(2); d.setTextColor(YEL, BG); d.setTextDatum(top_left); d.drawString(GH[s_page], 12, 38);
    lg::font(0); int y = 68;
    for (int i = 0; i < 6; i++) {
        const char *l = GB[s_page][i]; if (!l[0]) continue;
        bool legal = (s_page == GUIDE_PAGES - 1 && i >= 4) || (s_page == 3 && i >= 5);
        d.setTextColor(legal ? YEL : FG, BG); char lf[52]; lg::fit(lf, sizeof lf, l, W - 24); d.drawString(lf, 12, y); y += 20;
    }
    const int bw = (W - 36) / 2;
    lg::button(12, H - 46, bw, 34, TR("< Prec", "< Prev"), lg::card(), FG, NK_UP, 0);
    lg::button(24 + bw, H - 46, bw, 34, TR("Succ >", "Next >"), lg::card(), FG, NK_DOWN, 0);
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    d.fillRect(0, 0, W, H, BG);
    if (s_panel) { lg_guide(W, H); nucleo_touch_commit(); return; }
    switch (s_state) {
        case ST_CONSENT:  lg_consent(W, H); break;
        case ST_NOMODULE: lg_nomodule(W, H); break;
        case ST_SCANNING: lg_scanning(W, H); break;
        case ST_STOPPING: lg_busy(TR("Arresto...", "Stopping..."), TR("Ripristino cache ARP.", "Restoring ARP cache."), W, H); s_stop_armed = true; break;
        case ST_HOSTS:    if (s_map) lg_hosts_map(W, H); else lg_hosts_list(W, H); break;
        case ST_RUNNING:  lg_running(W, H); break;
        default:          lg_menu(W, H); break;
    }
    nucleo_touch_commit();
    d.setFont(&fonts::Font0); d.setTextSize(1);
}

// The Cardputer while the panel has the engine: the state, what is selected, and the live op.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("ETHERNET", ATK);
    d.setFont(&fonts::FreeSansBold9pt7b);
    if (s_panel) {
        d.setTextColor(YEL, BG); d.drawString(GH[s_page], 8, y + 2);
        lg::comp_keys(TR("su/giu pagina  invio chiudi", "up/dn page  enter close"));
        return;
    }
    switch (s_state) {
        case ST_CONSENT:
            d.setTextColor(FG, BG); lg::wrap(TR("Test autorizzati: accetta sul 2o schermo.", "Authorized testing: accept on the 2nd screen."), 8, y + 2, 224, 20, 3);
            lg::comp_keys(TR("invio accetto  esc esci  tab guida", "enter accept  esc back  tab guide")); break;
        case ST_NOMODULE:
            d.setTextColor(COL_RED, BG); d.drawString(TR("Modulo W5500 assente", "W5500 module absent"), 8, y + 2);
            lg::comp_keys(TR("un tasto per rilevare", "a key to detect")); break;
        case ST_SCANNING: {
            char b[8]; snprintf(b, sizeof b, "%d%%", nucleo_eth_scan_progress());
            d.setFont(&fonts::Font0); d.setTextSize(3); d.setTextColor(FG, BG); d.setCursor(8, y + 10); d.print(b); d.setTextSize(1);
            lg::comp_keys(TR("scansione...", "scanning...")); break;
        }
        case ST_HOSTS: {
            int n = nucleo_eth_host_count();
            char t[40]; snprintf(t, sizeof t, "%s", n ? nucleo_eth_host_ip(s_msel) : TR("nessun host", "no hosts"));
            d.setTextColor(FG, BG); d.drawString(t, 8, y + 2);
            if (n) { d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.setCursor(8, y + 26);
                     d.printf("%d/%d  %s%s", s_msel + 1, n, nucleo_eth_host_vendor(s_msel), nucleo_eth_host_is_gateway(s_msel) ? " GW" : ""); }
            lg::comp_keys(TR("su/giu  invio MITM  m mappa", "up/dn  enter MITM  m map")); break;
        }
        case ST_RUNNING: {
            nucleo_eth_op_t op = nucleo_eth_current();
            d.setTextColor(ATK, BG); d.drawString(op_name(op), 8, y + 2);
            unsigned u = nucleo_eth_uptime_s();
            d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.setCursor(8, y + 26);
            d.printf("%02u:%02u   tx %lu", u / 60, u % 60, nucleo_eth_tx_frames());
            lg::comp_keys(TR("invio stop  esc lascia on", "enter stop  esc leave on")); break;
        }
        default: {  // ST_MENU
            d.setTextColor(FG, BG); d.drawString(TR(MENU[s_sel].label, MENU[s_sel].label_en), 8, y + 2);
            d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.setCursor(8, y + 26);
            d.printf("%d/%d   %d host", s_sel + 1, MENU_N, nucleo_eth_host_count());
            lg::comp_keys(TR("su/giu  invio avvia  tab guida", "up/dn  enter start  tab guide")); break;
        }
    }
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }   // the panel owns the engine UI; here: status

    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_panel) { draw_guide(); return; }
    switch (s_state) {
        case ST_CONSENT:  draw_consent(h); break;
        case ST_NOMODULE: draw_nomodule(h); break;
        case ST_SCANNING: draw_scanning(h); break;
        case ST_STOPPING: draw_busy(TR("Arresto...","Stopping..."), TR("Ripristino cache ARP.","Restoring ARP cache."), h); s_stop_armed = true; break;
        case ST_HOSTS:    if (s_map) draw_hosts_map(h); else draw_hosts_list(h); break;
        case ST_RUNNING:  draw_running(h); break;
        default:          draw_menu(h); break;
    }
}

static void leave(void)
{
    // Loud continuous attacks keep running in the background (the launcher shows the alert), like the
    // Deauth Flood. Idle/scan state is torn down so we release the W5500 + exclusive reclaim.
    nucleo_eth_op_t op = nucleo_eth_current();
    if (op == ETH_OP_NONE || op == ETH_OP_SCAN) { nucleo_eth_stop(); nucleo_eth_end(); s_begun = false; }
    s_consented = false;   // re-ask authorization next session
}

extern "C" void nucleo_register_ethernet(void)
{
    static const nucleo_app_def_t app = {
        "ethernet", "Ethernet W5500", "Security", "Wired L2/L3 attacks (W5500 MACRAW) for authorized testing",
        'E', ATK, enter, on_key, tick, draw, leave, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
