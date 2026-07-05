// KeyDeck — use the Cardputer as a Wi-Fi keyboard for NucleoOS Anima (the ESP32-P4 board),
// with a live system monitor (free PSRAM + per-core CPU load) streamed back on this screen.
//
// Companion of app_usbkbd (same "Digita" passthrough UX), but over the network: a plain TCP
// line protocol to the board's nv_keydeck service (port 5588), discovered via mDNS
// (_keydeck._tcp) or a manually entered IP (persisted in NVS). Protocol contract:
// docs/keydeck.md (v1: HELLO/TXT/KEY/PING up, WELCOME/STAT/PONG/ERR down, STAT every ~1s).
//
// RAM posture: no TLS, no exclusive mode — one socket, static buffers, mDNS only for the
// one-shot async discovery. Networking is pumped from on_tick (5 Hz) and inside the modal
// typing loop (50 Hz), all on the app task; no extra FreeRTOS task.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"
extern "C" {
#include "nucleo_kbd.h"
#include "nucleo_i18n.h"
esp_err_t nucleo_discovery_resume(void);   // restart mDNS if a security app stopped it
const char *nucleo_setup_ip(void);         // our own STA IP ("" when offline)
}

#define TR(it_, en_) nucleo_tr((it_), (en_))

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x4DDF, GRN = 0x8FF3, WARN = 0xFE8C, INK = 0x0000;

enum { KD_PORT = 5588, KD_STALE_MS = 5000, KD_PING_MS = 2000, KD_CONN_TO_MS = 4000, KD_RETRY_MS = 2500 };

enum Tab { T_MON, T_SRV, T_INFO };
static Tab s_tab = T_MON;

// ---- connection state ------------------------------------------------------------------
enum ConnSt { CS_IDLE, CS_SEARCH, CS_CONNECT, CS_ONLINE };
static ConnSt s_cs = CS_IDLE;
static int  s_sock = -1;
static char s_ip[16] = "";                  // current target
static char s_saved_ip[16] = "";            // NVS-persisted fallback / manual entry
static char s_server[24] = "";              // WELCOME host name
static char s_status[44] = "";              // one-line state/error message for the UI
static mdns_search_once_t *s_search = NULL;
static int64_t s_conn_t0 = 0, s_last_tx = 0, s_retry_at = 0;

// ---- telemetry -------------------------------------------------------------------------
static unsigned s_ps_free = 0, s_ps_total = 0, s_sram_free = 0, s_uptime = 0;
static int s_cpu0 = -1, s_cpu1 = -1;
static int64_t s_stat_at = 0;               // ms timestamp of the last STAT (0 = never)
static bool s_nofocus = false;              // server said ERR nofocus (no field focused)

// ---- rx line assembly ------------------------------------------------------------------
static char   s_rx[192];
static size_t s_rxlen = 0;

// ---- optional PIN pairing (server-side "keydeck_pin"; empty = open, the default) --------
static char s_pin[12] = "";                 // NVS-persisted; sent as HELLO ... PIN=<pin>
static bool s_need_pin = false;             // server replied ERR badpin: stop auto-retry

// ---- manual IP / PIN editor (Server tab) -------------------------------------------------
enum EditMode { E_NONE = 0, E_IP, E_PIN };
static EditMode s_edit = E_NONE;
static char s_ebuf[16] = "";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_status(const char *msg) { snprintf(s_status, sizeof s_status, "%s", msg); }

// ---- NVS: remember the last good server IP ----------------------------------------------
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("keydeck", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_saved_ip;
    nvs_get_str(h, "ip", s_saved_ip, &n);
    n = sizeof s_pin;
    nvs_get_str(h, "pin", s_pin, &n);
    nvs_close(h);
}

static void pin_save(const char *pin)
{
    snprintf(s_pin, sizeof s_pin, "%s", pin);
    nvs_handle_t h;
    if (nvs_open("keydeck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "pin", s_pin);
    nvs_commit(h);
    nvs_close(h);
}

static void ip_save(const char *ip)
{
    if (!ip[0] || strcmp(ip, s_saved_ip) == 0) return;
    snprintf(s_saved_ip, sizeof s_saved_ip, "%s", ip);
    nvs_handle_t h;
    if (nvs_open("keydeck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ip", s_saved_ip);
    nvs_commit(h);
    nvs_close(h);
}

// ---- socket lifecycle --------------------------------------------------------------------
static void sock_close(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

static void search_cancel(void)
{
    if (s_search) { mdns_query_async_delete(s_search); s_search = NULL; }
}

static void go_offline(const char *msg, bool retry)
{
    sock_close();
    s_cs = CS_IDLE;
    s_stat_at = 0;
    if (msg) set_status(msg);
    s_retry_at = retry && s_ip[0] ? now_ms() + KD_RETRY_MS : 0;
    nucleo_app_request_draw();
}

// Begin a non-blocking connect to s_ip. Progress is polled in pump().
static void start_connect(void)
{
    sock_close();
    s_rxlen = 0;
    s_need_pin = false;
    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock < 0) { go_offline(TR("errore socket", "socket error"), false); return; }
    fcntl(s_sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(KD_PORT);
    if (!inet_aton(s_ip, &a.sin_addr)) { go_offline(TR("IP non valido", "bad IP"), false); return; }
    connect(s_sock, (struct sockaddr *)&a, sizeof a);   // EINPROGRESS expected
    s_cs = CS_CONNECT;
    s_conn_t0 = now_ms();
    set_status(TR("connetto...", "connecting..."));
    nucleo_app_request_draw();
}

// Kick an async mDNS browse for _keydeck._tcp; falls back to the saved IP on failure.
static void start_discover(void)
{
    search_cancel();
    nucleo_discovery_resume();   // no-op if mDNS is already up
    s_search = mdns_query_async_new(NULL, "_keydeck", "_tcp", MDNS_TYPE_PTR, 2500, 4, NULL);
    if (!s_search) {
        if (s_saved_ip[0]) { snprintf(s_ip, sizeof s_ip, "%s", s_saved_ip); start_connect(); }
        else go_offline(TR("mDNS ko e nessun IP", "mDNS down, no IP"), false);
        return;
    }
    s_cs = CS_SEARCH;
    set_status(TR("cerco NucleoV2...", "searching NucleoV2..."));
    nucleo_app_request_draw();
}

// ---- tx ----------------------------------------------------------------------------------
static bool tx_line(const char *fmt, ...)
{
    if (s_cs != CS_ONLINE || s_sock < 0) return false;
    char out[80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof out, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof out) return false;  // truncated = lost '\n' = merged lines: never send
    if (send(s_sock, out, (size_t)n, 0) != n) {
        go_offline(TR("connessione persa", "link lost"), true);
        return false;
    }
    s_last_tx = now_ms();
    return true;
}

// ---- rx parsing --------------------------------------------------------------------------
static void handle_stat(char *args)
{
    unsigned v;
    int iv;
    for (char *tok = strtok(args, " "); tok; tok = strtok(NULL, " ")) {
        if      (sscanf(tok, "ps_free=%u",   &v) == 1) s_ps_free   = v;
        else if (sscanf(tok, "ps_total=%u",  &v) == 1) s_ps_total  = v;
        else if (sscanf(tok, "sram_free=%u", &v) == 1) s_sram_free = v;
        else if (sscanf(tok, "cpu0=%d",     &iv) == 1) s_cpu0      = iv;
        else if (sscanf(tok, "cpu1=%d",     &iv) == 1) s_cpu1      = iv;
        else if (sscanf(tok, "up=%u",        &v) == 1) s_uptime    = v;
    }
    s_stat_at = now_ms();
}

static void handle_line(char *line)
{
    if (strncmp(line, "STAT ", 5) == 0) { handle_stat(line + 5); return; }
    if (strncmp(line, "WELCOME ", 8) == 0) {
        // "WELCOME v1 <os> <host>" — keep the host label for the UI
        char *last = strrchr(line, ' ');
        if (last && last[1]) snprintf(s_server, sizeof s_server, "%s", last + 1);
        set_status(TR("collegato", "connected"));
        ip_save(s_ip);                         // remember the last good server
        return;
    }
    if (strcmp(line, "ERR nofocus") == 0) { s_nofocus = true; return; }
    if (strcmp(line, "ERR badpin") == 0) {
        // Server requires (a different) PIN: stop the retry loop, point the user at the editor.
        s_need_pin = true;
        go_offline(TR("PIN richiesto: Server, tasto P", "PIN required: Server tab, P"), false);
        return;
    }
    // PONG / other ERR: nothing to do — any traffic already proves the link is alive.
}

// Drive discovery/connect/receive. Called at 5 Hz from on_tick and ~50 Hz inside the typing
// modal. Returns true when something displayable changed.
static bool pump(void)
{
    bool changed = false;
    const int64_t now = now_ms();

    if (s_cs == CS_IDLE && s_retry_at && now >= s_retry_at) { s_retry_at = 0; start_connect(); }

    if (s_cs == CS_SEARCH && s_search) {
        mdns_result_t *res = NULL;
        uint8_t n = 0;
        if (mdns_query_async_get_results(s_search, 0, &res, &n)) {
            char found[16] = "";
            for (mdns_result_t *r = res; r && !found[0]; r = r->next)
                for (mdns_ip_addr_t *ad = r->addr; ad; ad = ad->next)
                    if (ad->addr.type == ESP_IPADDR_TYPE_V4) {
                        esp_ip4_addr_t ip4 = ad->addr.u_addr.ip4;
                        snprintf(found, sizeof found, IPSTR, IP2STR(&ip4));
                        break;
                    }
            if (res) mdns_query_results_free(res);
            search_cancel();
            if (found[0]) {
                snprintf(s_ip, sizeof s_ip, "%s", found);
                start_connect();
            } else if (s_saved_ip[0]) {
                snprintf(s_ip, sizeof s_ip, "%s", s_saved_ip);
                set_status(TR("mDNS muto, provo IP noto", "mDNS silent, trying saved IP"));
                start_connect();
            } else {
                go_offline(TR("nessun NucleoV2 in rete", "no NucleoV2 found"), false);
            }
            changed = true;
        }
    }

    if (s_cs == CS_CONNECT) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s_sock, &wf);
        struct timeval tv = { 0, 0 };
        if (select(s_sock + 1, NULL, &wf, NULL, &tv) > 0 && FD_ISSET(s_sock, &wf)) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(s_sock, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) {
                // Connected: back to blocking sends (bounded) + MSG_DONTWAIT receives.
                fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) & ~O_NONBLOCK);
                const struct timeval sto = { 0, 300 * 1000 };
                setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof sto);
                s_cs = CS_ONLINE;
                s_nofocus = false;
                if (s_pin[0]) tx_line("HELLO v1 cardputer PIN=%s\n", s_pin);
                else          tx_line("HELLO v1 cardputer\n");
                set_status(TR("collegato", "connected"));
            } else {
                go_offline(TR("P4 non risponde", "no answer from P4"), true);
            }
            changed = true;
        } else if (now - s_conn_t0 > KD_CONN_TO_MS) {
            go_offline(TR("timeout connessione", "connect timeout"), true);
            changed = true;
        }
    }

    if (s_cs == CS_ONLINE) {
        char buf[96];
        int n;
        while ((n = recv(s_sock, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
            for (int i = 0; i < n; i++) {
                const char c = buf[i];
                if (c == '\n') {
                    s_rx[s_rxlen] = '\0';
                    if (s_rxlen && s_rx[s_rxlen - 1] == '\r') s_rx[s_rxlen - 1] = '\0';
                    handle_line(s_rx);
                    s_rxlen = 0;
                    if (s_cs != CS_ONLINE) return true;   // handler dropped the link (ERR badpin)
                    changed = true;
                } else if (s_rxlen < sizeof s_rx - 1) {
                    s_rx[s_rxlen++] = c;
                }
            }
        }
        if (n == 0 || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
            go_offline(TR("connessione persa", "link lost"), true);
            changed = true;
        } else if (now - s_last_tx > KD_PING_MS) {
            tx_line("PING\n");
        }
    }
    return changed;
}

// ---- key forwarding ------------------------------------------------------------------------
// Map one Cardputer key event to a protocol line. Returns a short echo label, or NULL if the
// key isn't forwardable. `fn` reuses the USB Keyboard conventions: Fn+Canc = forward Delete.
static const char *forward_key(int key, char ch, bool fn)
{
    switch (key) {
        case NK_UP:    return tx_line("KEY UP\n")    ? "Up"  : NULL;
        case NK_DOWN:  return tx_line("KEY DOWN\n")  ? "Dn"  : NULL;
        case NK_LEFT:  return tx_line("KEY LEFT\n")  ? "Lt"  : NULL;
        case NK_RIGHT: return tx_line("KEY RIGHT\n") ? "Rt"  : NULL;
        case NK_ENTER: return tx_line("KEY ENTER\n") ? "\xC2\xB6" : NULL;   // ¶
        case NK_TAB:   return tx_line("KEY TAB\n")   ? "Tab" : NULL;
        case NK_DEL:
            if (fn)   return tx_line("KEY DELETE\n")    ? "Del" : NULL;
            return       tx_line("KEY BACKSPACE\n")     ? "<-"  : NULL;
        default: break;
    }
    if (ch >= 32 && ch < 127) {
        static char lbl[2];
        if (!tx_line("TXT %c\n", ch)) return NULL;
        lbl[0] = ch ? ch : ' ';
        lbl[1] = 0;
        return ch == ' ' ? "spc" : lbl;
    }
    return NULL;
}

// ---- drawing helpers -------------------------------------------------------------------------
static void chip(int x, int y, const char *label, bool on, unsigned short oncol)
{
    int w = (int)strlen(label) * 6 + 8;
    d.fillRoundRect(x, y, w, 13, 3, on ? oncol : 0x10A2);
    d.setTextColor(on ? INK : MUTED, on ? oncol : 0x10A2);
    d.setCursor(x + 4, y + 3);
    d.print(label);
}

static void bar(int x, int y, int w, int h, int pct, unsigned short col)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d.drawRoundRect(x, y, w, h, 2, DIM);
    const int fw = (w - 2) * pct / 100;
    if (fw > 0) d.fillRoundRect(x + 1, y + 1, fw, h - 2, 1, col);
}

// "27.3" from bytes, integer math (one decimal).
static void fmt_mb(char *out, size_t n, unsigned bytes)
{
    const unsigned x10 = (unsigned)(((uint64_t)bytes * 10) / (1024 * 1024));
    snprintf(out, n, "%u.%u", x10 / 10, x10 % 10);
}

static bool stats_fresh(void) { return s_stat_at && now_ms() - s_stat_at < KD_STALE_MS; }

// Shared status row: colored dot + server/state text. Returns the y below it.
static int status_row(int y)
{
    const bool on = (s_cs == CS_ONLINE);
    d.fillCircle(12, y + 4, 4, on ? GRN : (s_cs == CS_IDLE ? WARN : ACC));
    d.setTextSize(1);
    d.setTextColor(on ? GRN : MUTED, BG);
    d.setCursor(22, y);
    char ln[44];
    if (on) snprintf(ln, sizeof ln, "%s  %s", s_server[0] ? s_server : "NucleoV2", s_ip);
    else    snprintf(ln, sizeof ln, "%s", s_status[0] ? s_status : TR("non collegato", "offline"));
    d.print(ln);
    return y + 12;
}

// ---- live typing modal (mirrors app_usbkbd's type_loop) ---------------------------------------
static void type_screen(unsigned char m, const char *echo)
{
    d.fillScreen(BG);
    d.setTextSize(2);
    d.setTextColor(ACC, BG);
    d.setCursor(8, 4);
    d.print(TR("Digita su P4", "Type on P4"));
    d.setTextSize(1);
    const bool on = (s_cs == CS_ONLINE);
    d.fillCircle(160, 12, 4, on ? GRN : WARN);
    d.setTextColor(on ? GRN : WARN, BG);
    d.setCursor(170, 8);
    d.print(on ? "online" : TR("perso", "lost"));

    int x = 8, y = 28;
    chip(x, y, "CTRL",  m & NK_MOD_CTRL, ACC);  x += 38;
    chip(x, y, "ALT",   m & NK_MOD_ALT, ACC);   x += 32;
    chip(x, y, "SHIFT", m & NK_MOD_SHIFT, ACC); x += 44;
    chip(x, y, "FN",    m & NK_MOD_FN, ACC);

    d.setTextColor(MUTED, BG); d.setCursor(8, 50); d.print(TR("Inviato:", "Sent:"));
    d.setTextColor(FG, BG);    d.setCursor(60, 50); d.print(echo && echo[0] ? echo : "-");

    if (s_nofocus) {
        d.setTextColor(WARN, BG);
        d.setCursor(8, 64);
        d.print(TR("Tocca un campo di testo sul P4!", "Tap a text field on the P4!"));
    }

    // Mini system monitor — keeps the telemetry on screen while typing.
    char mb[8], ln[44];
    fmt_mb(mb, sizeof mb, s_ps_free);
    const bool fresh = stats_fresh();
    d.setTextColor(fresh ? GRN : DIM, BG);
    d.setCursor(8, 82);
    if (s_cpu0 >= 0) snprintf(ln, sizeof ln, "PSRAM %s MB   CPU %d%% / %d%%", mb, s_cpu0, s_cpu1);
    else             snprintf(ln, sizeof ln, "PSRAM %s MB", mb);
    d.print(s_stat_at ? ln : TR("attendo dati...", "waiting for data..."));

    d.setTextColor(DIM, BG);
    d.setCursor(8, 104);  d.print(TR("Frecce/Tab/Invio inoltrati", "Arrows/Tab/Enter forwarded"));
    d.setCursor(8, 116);  d.print(TR("Fn+Canc=Del  Fn+`=Esc", "Fn+Del=Del  Fn+`=Esc"));
    d.setTextColor(WARN, BG); d.setCursor(178, 116); d.print(TR("` esci", "` exit"));
}

static void type_loop(void)
{
    char echo[26] = "";
    unsigned char last_m = 0xFF;
    bool redraw = true, back = false;
    while (!back) {
        esp_task_wdt_reset();
        if (pump()) redraw = true;
        nucleo_key_t k = nucleo_kbd_read();
        unsigned char m = nucleo_kbd_mods();

        if (k.key != NK_NONE) {
            const bool fn = (m & NK_MOD_FN);
            if (k.key == NK_BACK) {
                if (fn) {
                    if (tx_line("KEY ESC\n")) { snprintf(echo, sizeof echo, "Esc"); redraw = true; }
                } else {
                    back = true;
                    break;
                }
            } else {
                s_nofocus = false;   // reset the hint; the server re-raises it if still true
                const char *lbl = forward_key(k.key, k.ch, fn);
                if (lbl) {
                    size_t l = strlen(echo);
                    if (l + strlen(lbl) + 1 >= sizeof echo) { echo[0] = 0; l = 0; }   // wrap
                    snprintf(echo + l, sizeof echo - l, "%s", lbl);
                    redraw = true;
                }
            }
        }
        if (m != last_m) { last_m = m; redraw = true; }
        if (redraw) { type_screen(m, echo); redraw = false; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    nucleo_app_request_draw();
}

// ---- framework callbacks ------------------------------------------------------------------------
static bool on_back(int key)
{
    if (key == NK_LEFT) {   // cycle tabs backward, same convention as USB Keyboard
        s_tab = (Tab)((s_tab + T_INFO) % (T_INFO + 1));
        nucleo_app_request_draw();
        return true;
    }
    if (s_edit != E_NONE) { s_edit = E_NONE; nucleo_app_request_draw(); return true; }  // ` cancels the editor
    return false;           // ` closes the app
}

static void on_enter(void)
{
    s_tab = T_MON;
    s_edit = E_NONE;
    s_server[0] = 0;
    s_stat_at = 0;
    s_nofocus = false;
    s_need_pin = false;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR("ENTER digita  </> tab  ` esci", "ENTER type  </> tab  ` exit"));
    cfg_load();
    if (s_saved_ip[0]) { snprintf(s_ip, sizeof s_ip, "%s", s_saved_ip); start_connect(); }
    else start_discover();
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    search_cancel();
    sock_close();
    s_cs = CS_IDLE;
    s_retry_at = 0;
}

static void on_key(int key, char ch)
{
    if (s_edit != E_NONE) {   // manual IP / PIN editor (Server tab)
        if ((ch >= '0' && ch <= '9') || (ch == '.' && s_edit == E_IP)) {
            size_t l = strlen(s_ebuf);
            const size_t cap = (s_edit == E_PIN) ? sizeof s_pin - 1 : sizeof s_ebuf - 1;
            if (l < cap) { s_ebuf[l] = ch; s_ebuf[l + 1] = 0; }
        } else if (key == NK_DEL) {
            size_t l = strlen(s_ebuf);
            if (l) s_ebuf[l - 1] = 0;
        } else if (key == NK_ENTER) {
            if (s_edit == E_IP && s_ebuf[0]) {
                snprintf(s_ip, sizeof s_ip, "%s", s_ebuf);
                ip_save(s_ebuf);
                s_edit = E_NONE;
                start_connect();
            } else if (s_edit == E_PIN) {       // empty = clear the PIN (back to open mode)
                pin_save(s_ebuf);
                s_edit = E_NONE;
                if (s_ip[0]) start_connect();
            }
        }
        nucleo_app_request_draw();
        return;
    }

    if (ch >= '1' && ch <= '3') { s_tab = (Tab)(ch - '1'); nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT) { s_tab = (Tab)((s_tab + 1) % (T_INFO + 1)); nucleo_app_request_draw(); return; }

    switch (s_tab) {
        case T_MON:
            if (key == NK_ENTER) {
                if (s_cs == CS_ONLINE) type_loop();
                else if (s_cs == CS_IDLE) { if (s_ip[0]) start_connect(); else start_discover(); }
            }
            break;
        case T_SRV:
            if (key == NK_ENTER) start_discover();                       // re-browse mDNS
            if (ch == 'm') { s_edit = E_IP;  snprintf(s_ebuf, sizeof s_ebuf, "%s", s_saved_ip); }
            if (ch == 'p') { s_edit = E_PIN; snprintf(s_ebuf, sizeof s_ebuf, "%s", s_pin); }
            nucleo_app_request_draw();
            break;
        default:
            break;
    }
}

static void on_tick(void)
{
    if (pump()) nucleo_app_request_draw();
    // Refresh Monitor/Info at the tick rate while online so uptime/staleness stay live.
    if (s_cs == CS_ONLINE && s_tab != T_SRV) nucleo_app_request_draw();
}

// ---- tab drawing ----------------------------------------------------------------------------------
static void tabbar(int top)
{
    const char *N[] = { "Monitor", "Server", "Info" };
    int x = 6;
    for (int i = 0; i <= T_INFO; i++) {
        const bool on = (i == (int)s_tab);
        const int w = (int)strlen(N[i]) * 6 + 10;
        d.fillRoundRect(x, top + 2, w, 14, 3, on ? ACC : 0x10A2);
        d.setTextColor(on ? INK : MUTED, on ? ACC : 0x10A2);
        d.setCursor(x + 5, top + 5);
        d.print(N[i]);
        x += w + 4;
    }
}

static void draw_monitor(int top)
{
    int y = status_row(top + 22);
    char mb[8], tot[8], ln[44];
    const bool fresh = stats_fresh();
    const unsigned short val = fresh ? FG : DIM;

    // Headline: free PSRAM, big.
    fmt_mb(mb, sizeof mb, s_ps_free);
    fmt_mb(tot, sizeof tot, s_ps_total);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(8, y + 4); d.print(TR("PSRAM libera", "Free PSRAM"));
    d.setTextSize(2); d.setTextColor(s_stat_at ? GRN : DIM, BG);
    d.setCursor(8, y + 14);
    if (s_stat_at) { snprintf(ln, sizeof ln, "%s MB", mb); d.print(ln); }
    else d.print("--");
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(120, y + 22);
    if (s_ps_total) { snprintf(ln, sizeof ln, "%s %s MB", TR("di", "of"), tot); d.print(ln); }
    bar(8, y + 32, 224, 6, s_ps_total ? (int)((uint64_t)s_ps_free * 100 / s_ps_total) : 0, GRN);

    // Per-core CPU load.
    for (int c = 0; c < 2; c++) {
        const int yy = y + 44 + c * 14;
        const int pct = c == 0 ? s_cpu0 : s_cpu1;
        d.setTextColor(MUTED, BG); d.setCursor(8, yy); d.print(c == 0 ? "CPU0" : "CPU1");
        d.setTextColor(val, BG);   d.setCursor(40, yy);
        if (pct >= 0) { snprintf(ln, sizeof ln, "%3d%%", pct); d.print(ln); }
        else d.print(" n/d");
        bar(76, yy, 156, 9, pct < 0 ? 0 : pct, pct >= 85 ? WARN : ACC);
    }

    // Footer: free SRAM + server uptime.
    d.setTextColor(DIM, BG);
    d.setCursor(8, y + 74);
    if (s_stat_at) {
        snprintf(ln, sizeof ln, "SRAM %u KB   %s %uh %02um", s_sram_free / 1024,
                 TR("acceso da", "up"), s_uptime / 3600, (s_uptime % 3600) / 60);
        d.print(ln);
    } else if (s_cs == CS_ONLINE) {
        d.print(TR("attendo telemetria...", "waiting for telemetry..."));
    } else {
        d.print(TR("ENTER riconnette", "ENTER reconnects"));
    }
}

static void draw_server(int top)
{
    int y = status_row(top + 22);
    char ln[44];
    d.setTextSize(1);
    d.setTextColor(MUTED, BG); d.setCursor(8, y + 6);  d.print(TR("Scoperta: mDNS _keydeck._tcp", "Discovery: mDNS _keydeck._tcp"));
    d.setTextColor(FG, BG);    d.setCursor(8, y + 20);
    snprintf(ln, sizeof ln, "%s: %-15s  PIN: %s", TR("IP salvato", "Saved IP"),
             s_saved_ip[0] ? s_saved_ip : "-", s_pin[0] ? s_pin : TR("no (aperto)", "off (open)"));
    d.print(ln);

    if (s_edit != E_NONE) {
        d.setTextColor(ACC, BG); d.setCursor(8, y + 38);
        d.print(s_edit == E_IP ? TR("Nuovo IP:", "New IP:")
                               : TR("PIN del P4 (vuoto=nessuno):", "P4 PIN (empty=none):"));
        d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(8, y + 50);
        snprintf(ln, sizeof ln, "%s_", s_ebuf);
        d.print(ln);
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(8, y + 72); d.print(TR("ENTER salva  Canc corregge  ` annulla", "ENTER save  Del edit  ` cancel"));
    } else {
        d.setTextColor(GRN, BG);   d.setCursor(8, y + 40);  d.print("ENTER");
        d.setTextColor(MUTED, BG); d.setCursor(56, y + 40); d.print(TR("cerca di nuovo (mDNS)", "search again (mDNS)"));
        d.setTextColor(GRN, BG);   d.setCursor(8, y + 54);  d.print("M");
        d.setTextColor(MUTED, BG); d.setCursor(56, y + 54); d.print(TR("inserisci IP a mano", "enter IP manually"));
        d.setTextColor(GRN, BG);   d.setCursor(8, y + 68);  d.print("P");
        d.setTextColor(s_need_pin ? WARN : MUTED, BG); d.setCursor(56, y + 68);
        d.print(s_need_pin ? TR("PIN richiesto dal P4!", "P4 requires a PIN!")
                           : TR("imposta PIN (se il P4 lo chiede)", "set PIN (if the P4 asks)"));
    }
}

static void draw_info(int top)
{
    int y = status_row(top + 22);
    char ln[44];
    d.setTextSize(1);
    d.setTextColor(FG, BG);
    d.setCursor(8, y + 6);
    snprintf(ln, sizeof ln, "%s: %s:%d", TR("Server", "Server"), s_ip[0] ? s_ip : "-", KD_PORT);
    d.print(ln);
    d.setCursor(8, y + 20);
    snprintf(ln, sizeof ln, "%s: %s", TR("Questo Cardputer", "This Cardputer"), nucleo_setup_ip());
    d.print(ln);
    d.setTextColor(MUTED, BG);
    d.setCursor(8, y + 38); d.print(TR("Protocollo KeyDeck v1 (TCP)", "KeyDeck protocol v1 (TCP)"));
    d.setCursor(8, y + 52); d.print(TR("Tasti -> campo attivo sul P4", "Keys -> focused field on P4"));
    d.setCursor(8, y + 66); d.print(TR("Telemetria: PSRAM+CPU ogni 1s", "Telemetry: PSRAM+CPU every 1s"));
}

static void draw(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    tabbar(top);
    switch (s_tab) {
        case T_MON:  draw_monitor(top); break;
        case T_SRV:  draw_server(top);  break;
        case T_INFO: draw_info(top);    break;
    }
}

extern "C" void nucleo_register_keydeck(void)
{
    static const nucleo_app_def_t app = {
        "keydeck", "KeyDeck", "Connect", "Tastiera Wi-Fi + monitor per NucleoV2 (P4)",
        'D', 0x4DDF, on_enter, on_key, on_tick, draw, on_exit, 0
    };
    nucleo_app_register(&app);
}
