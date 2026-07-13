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

#define GUIDE_PAGES 5

// ---- menu model -------------------------------------------------------------
struct MenuItem { const char *label; char glyph; };
static const MenuItem MENU[] = {
    { "Scansiona rete",     'S' },   // 0
    { "Mappa & host",       'M' },   // 1
    { "ARP poison gateway", 'P' },   // 2
    { "DHCP starvation",    'D' },   // 3
    { "MAC flooding",       'F' },   // 4
    { "Rogue DHCP (MITM)",  'R' },   // 5
    { "Cattura PCAP",       'C' },   // 6
};
#define MENU_N ((int)(sizeof(MENU)/sizeof(MENU[0])))

static void set_hint(void)
{
    if (s_panel)                       nucleo_app_set_hint(TR("su/giu pagina   invio chiudi", "up/dn page   enter close"));
    else switch (s_state) {
        case ST_CONSENT:   nucleo_app_set_hint(TR("invio accetto   esc esci   tab guida", "enter accept   esc back   tab guide")); break;
        case ST_NOMODULE:  nucleo_app_set_hint(TR("collega W5500   premi un tasto per rilevare", "connect W5500   press a key to detect")); break;
        case ST_SCANNING:  nucleo_app_set_hint(TR("scansione in corso...", "scanning...")); break;
        case ST_STOPPING:  nucleo_app_set_hint(TR("arresto + ripristino...", "stopping + restoring...")); break;
        case ST_HOSTS:     nucleo_app_set_hint(TR("su/giu   invio MITM   m mappa   esc indietro", "up/dn   enter MITM   m map   esc back")); break;
        case ST_RUNNING:   nucleo_app_set_hint(TR("invio: ferma+ripristina   esc: lascia attivo", "enter: stop+restore   esc: leave on")); break;
        default:           nucleo_app_set_hint(TR("su/giu   invio avvia   tab guida", "up/dn   enter start   tab guide")); break;
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

static void start_running(int rc) { if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; } else fail("Avvio fallito"); set_hint(); nucleo_app_request_draw(); }

static void activate_menu(void)
{
    s_err[0] = 0;
    switch (s_sel) {
        case 0: if (nucleo_eth_scan_start() == 0) { s_state = ST_SCANNING; set_hint(); } else fail("Scan fallito"); break;
        case 1: if (nucleo_eth_host_count() > 0) { s_state = ST_HOSTS; s_map = false; s_msel = 0; set_hint(); }
                else fail("Scansiona prima la rete");
                break;
        case 2: { uint32_t gw = nucleo_eth_gateway_ip();
                  if (!gw) fail("Gateway ignoto: scansiona"); else start_running(nucleo_eth_poison_start(gw)); } break;
        case 3: start_running(nucleo_eth_dhcp_starve_start()); break;
        case 4: start_running(nucleo_eth_macflood_start()); break;
        case 5:
            if (!nucleo_eth_has_identity()) fail("Serve identita': scansiona");
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
    if (!gw)            { fail("Gateway ignoto"); return; }
    if (victim == gw)   { fail("Scegli un host != gateway"); return; }
    if (!victim)        { fail("Host non valido"); return; }
    start_running(nucleo_eth_mitm_start(victim, gw));
}

static void tick(void)
{
    if (s_state == ST_SCANNING) {
        if (nucleo_eth_current() == ETH_OP_NONE || nucleo_eth_scan_progress() >= 100) {
            s_state = (nucleo_eth_host_count() > 0) ? ST_HOSTS : ST_MENU;
            s_map = false; s_msel = 0;
            if (nucleo_eth_host_count() == 0) fail("Nessun host (DHCP? cavo?)");
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
            if (key == NK_UP)        s_msel = (s_msel - 1 + (n?n:1)) % (n?n:1);
            else if (key == NK_DOWN) s_msel = (s_msel + 1) % (n?n:1);
            else if (key == NK_ENTER) start_mitm_on_selected();
            else if (key == NK_CHAR && (ch == 'm' || ch == 'M')) s_map = !s_map;
            nucleo_app_request_draw();
            return;
        }
        default:  // ST_MENU
            if (key == NK_UP)        s_sel = (s_sel - 1 + MENU_N) % MENU_N;
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
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *L[] = { "Attacchi L2/L3 cablati:", "scan, ARP MITM/poison,", "DHCP starve, MAC flood,",
                        "rogue DHCP, PCAP.", "Solo reti tue o con", "permesso scritto." };
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
        list_row(y, idx == s_sel, MENU[idx].label, "", CYN);
    }
    if (s_err[0]) { d.setTextSize(1); d.setTextColor(COL_RED, BG); d.setCursor(10, h - 2); d.print(s_err); }
}

static void draw_scanning(int h)
{
    app_ui_title("Scansione ARP", ATK, "");
    int p = nucleo_eth_scan_progress();
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, 34); char b[8]; snprintf(b, sizeof b, "%d%%", p); d.print(b);
    d.drawRoundRect(10, 60, 220, 14, 4, LINE);
    d.fillRoundRect(12, 62, (216 * p) / 100, 10, 3, GRN);
    char st[40]; snprintf(st, sizeof st, "host %d   tx %lu   rx %lu", nucleo_eth_host_count(), nucleo_eth_tx_frames(), nucleo_eth_rx_frames());
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 84); d.print(st);
    if (!nucleo_eth_has_identity()) { d.setTextColor(YEL, BG); d.setCursor(10, h - 10); d.print("acquisisco lease DHCP..."); }
    (void)h;
}

static void draw_hosts_list(int h)
{
    int n = nucleo_eth_host_count();
    char rt[12]; snprintf(rt, sizeof rt, "%d host", n);
    app_ui_title("Host trovati", ATK, rt);
    const int rowh = 24, y0 = 26;
    int vis = (h - y0) / rowh; if (vis < 1) vis = 1;
    int first = s_msel - vis / 2; if (first > n - vis) first = n - vis; if (first < 0) first = 0;
    for (int r = 0; r < vis && first + r < n; r++) {
        int idx = first + r, y = y0 + r * rowh;
        char lbl[28]; snprintf(lbl, sizeof lbl, "%s", nucleo_eth_host_ip(idx));
        char info[24]; snprintf(info, sizeof info, "%s%s", nucleo_eth_host_vendor(idx), nucleo_eth_host_is_gateway(idx) ? " GW" : "");
        list_row(y, idx == s_msel, lbl, info, nucleo_eth_host_is_gateway(idx) ? YEL : GRN);
    }
    if (n == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(10, h - 10); d.print("nessun host"); }
    else if (s_err[0]) { d.setTextSize(1); d.setTextColor(COL_RED, BG); d.setCursor(10, h - 10); d.print(s_err); }
}

// Live network map: gateway hub at top, YOU at bottom, hosts as a row of dots; selected host links
// through YOU (the MITM picture). Smartwatch-style: one glance shows topology + who you'd intercept.
static void draw_hosts_map(int h)
{
    int n = nucleo_eth_host_count();
    app_ui_title("Mappa rete", ATK, "");
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
            verdict = "cattura -> SD /capture.pcap"; vc = GRN;
            break;
        }
        case ETH_OP_ROGUE_DHCP: {
            char lz[8]; snprintf(lz, sizeof lz, "%d", nucleo_eth_rogue_leases());
            big(62, "OFFER", fr, CYN); big(178, "LEASE", lz, GRN);
            verdict = "router+DNS -> me (MITM)"; vc = nucleo_eth_rogue_leases() ? GRN : MUTED;
            break;
        }
        default: {
            char rx[12]; unsigned long r = nucleo_eth_rx_frames();
            if (r >= 100000) snprintf(rx, sizeof rx, "%luk", r / 1000); else snprintf(rx, sizeof rx, "%lu", r);
            big(62, "TX", fr, ATK); big(178, "RX", rx, CYN);
            if (op == ETH_OP_MITM)             { verdict = "intercetto host<->gw (reversibile)"; vc = GRN; }
            else if (op == ETH_OP_POISON)      { verdict = "gateway dirottato (reversibile)"; vc = ATK; }
            else if (op == ETH_OP_DHCP_STARVE) { verdict = "esaurisco il pool DHCP"; vc = ATK; }
            else if (op == ETH_OP_MACFLOOD)    { verdict = "CAM overflow -> hub mode"; vc = ATK; }
            else                               { verdict = "in corso..."; vc = YEL; }
            break;
        }
    }
    d.drawFastHLine(10, 84, 220, LINE);
    if (sub[0]) { d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 92); d.print(sub); }
    char st[52]; snprintf(st, sizeof st, "link %s   io %s", nucleo_eth_link() ? "UP" : "giu", nucleo_eth_my_ip_str());
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
static const char *GH[GUIDE_PAGES] = { "Cos'e'", "Hardware", "Scan & mappa", "Attacchi", "Comandi" };
static const char *GB[GUIDE_PAGES][6] = {
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

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_panel) { draw_guide(); return; }
    switch (s_state) {
        case ST_CONSENT:  draw_consent(h); break;
        case ST_NOMODULE: draw_nomodule(h); break;
        case ST_SCANNING: draw_scanning(h); break;
        case ST_STOPPING: draw_busy("Arresto...", "Ripristino cache ARP.", h); s_stop_armed = true; break;
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
        'E', ATK, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
