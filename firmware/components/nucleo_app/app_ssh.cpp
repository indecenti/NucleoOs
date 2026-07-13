// app_ssh.cpp — native on-device SSH terminal for NucleoOS (Bruce-style: no web, no bridge).
//
// LINE-MODE client tuned for the 56-key Cardputer: compose a command in a local input bar with real
// editing, recall previous commands with up/down, Tab gives evolved autocompletion (commands + flags
// + your history). Enter sends the line; output streams into a colored scrollback you page with
// Fn+up/down (NOT plain arrows). Fn+Z zooms, Fn+H opens a bilingual manual, Fn+Esc quits.
//
//   • Auth: PASSWORD or KEY (private key dropped in /sd/data/ssh/keys/, pick + passphrase).
//   • Host-key pinning: /sd/data/ssh/known_hosts, TOFU on first connect, ABORT on a changed key (MITM).
//   • ANSI colors: 16-color foreground (RAM-gated: falls back to mono when memory is tight).
//   • History: persisted to /sd/data/ssh/history.txt across reboots.
//   • On connect enters nucleo_exclusive mode (~70 KB freed; Wi-Fi STA stays); restored on exit.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "nucleo_board.h"
#include "app_gfx.h"
#include "nucleo_theme.h"   // themed chrome palette (replaces the local classic-theme mirror)
#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <libssh2.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "app.ssh";

#define SSH_DIR   NUCLEO_SD_MOUNT "/data/ssh"
#define HIST_PATH SSH_DIR "/history.txt"
#define KH_PATH   SSH_DIR "/known_hosts"
#define KEYS_DIR  SSH_DIR "/keys"

static bool s_en = false;   // default IT (bilingual strings throughout)

// ── colors ── chrome follows the active OS theme (was a local classic-theme mirror that ignored
// theme switches); the C_* names stay as thin aliases so the many draw calls below are untouched.
// ACCENT = the app's registered launcher accent; WARN/ERR + the ANSI PAL are genuine content colors.
#define C_BG   THEME_BG
#define C_DIM  THEME_MUTED
#define C_INK  THEME_FG
#define C_INBG THEME_LINE
static const unsigned short ACCENT = 0x2EE0;   // registered app accent (was C_ACC 0x6C9F)
#define C_WARN 0xFD20   // status/warning text (semantic content color)
#define C_ERR  0xF9A6   // error text (semantic content color)
// 16-colour ANSI palette (RGB565): 0-7 normal, 8-15 bright
static const unsigned short PAL[16] = {
    0x4208, 0xC000, 0x0560, 0xC560, 0x021F, 0xC018, 0x0575, 0xC618,
    0x7BEF, 0xF904, 0x07E6, 0xFFE0, 0x451F, 0xF81F, 0x07FF, 0xFFFF };
#define DEF_FG 7

// ── scrollback (allocated on connect, freed on exit) ──
#define SB_LINES 110
#define SB_W     112
static char    *s_sb = nullptr;     // logical (unwrapped, ANSI-stripped) lines
static uint8_t *s_sbcol = nullptr;  // parallel per-char colour index (optional; NULL = monochrome)
static int   s_head = 0, s_count = 0;
static char  s_cur[SB_W]; static uint8_t s_curcol[SB_W]; static int s_curlen = 0;
static int   s_aesc = 0, s_p[6], s_pn = 0;     // CSI param accumulator (for SGR)
static uint8_t s_fgbase = DEF_FG; static bool s_bold = false;
static int   s_scroll = 0;
static bool  s_mask = false;
static SemaphoreHandle_t s_lock = nullptr;
static volatile bool s_dirty = true;

static int row_of(int logicalIdx) { return (s_head - s_count + logicalIdx + SB_LINES * 8) % SB_LINES; }
static char*    sb_line(int i) { return s_sb + (size_t)row_of(i) * SB_W; }
static uint8_t* sb_col(int i)  { return s_sbcol ? s_sbcol + (size_t)row_of(i) * SB_W : nullptr; }
static uint8_t cur_fg() { return s_bold ? (uint8_t)((s_fgbase & 7) | 8) : (uint8_t)(s_fgbase & 7); }

static void sb_commit() {
    char *d2 = s_sb + (size_t)(s_head % SB_LINES) * SB_W;
    memcpy(d2, s_cur, s_curlen); d2[s_curlen] = 0;
    if (s_sbcol) { uint8_t *c2 = s_sbcol + (size_t)(s_head % SB_LINES) * SB_W; memcpy(c2, s_curcol, s_curlen); }
    s_head++; if (s_count < SB_LINES) s_count++;
    // mask the next input if this line looks like "...password:"
    s_mask = false;
    char low[SB_W]; for (int i = 0; i < s_curlen; i++) low[i] = (char)tolower((unsigned char)s_cur[i]); low[s_curlen] = 0;
    if (strstr(low, "password") || strstr(low, "passphrase")) { int e = s_curlen; while (e > 0 && s_cur[e-1] == ' ') e--; if (e > 0 && s_cur[e-1] == ':') s_mask = true; }
    s_curlen = 0; s_cur[0] = 0;
}
static void sb_putc(char c) { if (s_curlen < SB_W - 1) { if (s_sbcol) s_curcol[s_curlen] = cur_fg(); s_cur[s_curlen++] = c; s_cur[s_curlen] = 0; } }
static void apply_sgr() {
    if (s_pn == 0) { s_fgbase = DEF_FG; s_bold = false; return; }
    for (int i = 0; i < s_pn; i++) { int n = s_p[i];
        if (n == 0) { s_fgbase = DEF_FG; s_bold = false; }
        else if (n == 1) s_bold = true;
        else if (n == 22) s_bold = false;
        else if (n >= 30 && n <= 37) s_fgbase = (uint8_t)(n - 30);
        else if (n >= 90 && n <= 97) { s_fgbase = (uint8_t)(n - 90); s_bold = true; }
        else if (n == 39) s_fgbase = DEF_FG;
    }
}
static void sb_feed(const char *b, int n) {
    if (!s_lock || !s_sb) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool following = (s_scroll == 0);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (s_aesc == 0) {
            if (c == 0x1b) s_aesc = 1;
            else if (c == '\n') sb_commit();
            else if (c == '\r') s_curlen = 0, s_cur[0] = 0;
            else if (c == '\t') { int t = (s_curlen / 8 + 1) * 8; while (s_curlen < t) sb_putc(' '); }
            else if (c == '\b') { if (s_curlen > 0) { s_curlen--; s_cur[s_curlen] = 0; } }
            else if (c >= 0x20 && c < 0x7f) sb_putc((char)c);
            else if (c >= 0x80) sb_putc('?');
        } else if (s_aesc == 1) { if (c == '[') { s_aesc = 2; s_pn = 0; s_p[0] = 0; } else if (c == ']') s_aesc = 3; else s_aesc = 0; }
        else if (s_aesc == 2) {
            if (c >= '0' && c <= '9') { if (s_pn == 0) s_pn = 1; if (s_pn <= 6) s_p[s_pn - 1] = s_p[s_pn - 1] * 10 + (c - '0'); }
            else if (c == ';') { if (s_pn < 6) { s_pn++; s_p[s_pn - 1] = 0; } }
            else { if (c == 'm') apply_sgr(); s_aesc = 0; }
        } else if (s_aesc == 3) { if (c == 0x07 || c == 0x1b) s_aesc = 0; }
    }
    if (following) s_scroll = 0;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}
static void sb_info(const char *s) { sb_feed(s, strlen(s)); sb_feed("\n", 1); }   // push an info line

// ── command history (in-RAM ring + persisted to SD) ──
#define HIST_MAX 30
static char s_hist[HIST_MAX][SB_W]; static int s_histN = 0, s_histPos = -1;
static void ensure_dirs() { mkdir(SSH_DIR, 0777); mkdir(KEYS_DIR, 0777); }
static void hist_add_mem(const char *cmd) {
    if (!cmd[0]) { s_histPos = -1; return; }
    if (s_histN > 0 && strncmp(s_hist[s_histN - 1], cmd, SB_W) == 0) { s_histPos = -1; return; }
    if (s_histN == HIST_MAX) { memmove(s_hist[0], s_hist[1], (HIST_MAX - 1) * SB_W); s_histN--; }
    snprintf(s_hist[s_histN++], SB_W, "%s", cmd); s_histPos = -1;
}
static void hist_load() {
    s_histN = 0; FILE *f = fopen(HIST_PATH, "r"); if (!f) return;
    char ln[SB_W];
    while (fgets(ln, sizeof ln, f)) { int L = strlen(ln); while (L > 0 && (ln[L-1] == '\n' || ln[L-1] == '\r')) ln[--L] = 0; if (L) hist_add_mem(ln); }
    fclose(f); s_histPos = -1;
}
static void hist_persist(const char *cmd) { FILE *f = fopen(HIST_PATH, "a"); if (f) { fprintf(f, "%s\n", cmd); fclose(f); } }

// ── Tab autocompletion: commands/flags + history ──
static const char *CMDS[] = {
    "ls", "ls -la", "ls -lah", "cd ", "cd ..", "cd /", "cat ", "less ", "tail -f ", "head ", "grep ",
    "grep -r ", "find . -name ", "pwd", "whoami", "clear", "echo ", "export ", "history", "man ",
    "sudo ", "su -", "systemctl status ", "systemctl restart ", "systemctl start ", "systemctl stop ",
    "journalctl -u ", "journalctl -f", "service ", "ps aux", "top", "htop", "kill ", "killall ",
    "df -h", "du -sh ", "free -h", "uptime", "uname -a", "dmesg", "mount", "lsblk",
    "ip a", "ifconfig", "ping ", "curl ", "wget ", "ss -tlnp", "netstat -tlnp", "nslookup ",
    "apt update", "apt upgrade", "apt install ", "apt remove ", "dpkg -l",
    "docker ps", "docker ps -a", "docker logs ", "docker exec -it ", "docker compose up -d", "docker restart ",
    "git status", "git pull", "git log --oneline", "git diff", "git add .", "git commit -m \"",
    "nano ", "vi ", "vim ", "mkdir ", "rm ", "rm -rf ", "cp ", "mv ", "chmod +x ", "chown ",
    "tar -xzf ", "tar -czf ", "unzip ", "scp ", "reboot", "shutdown -h now", "exit", nullptr };
static char s_compPrefix[SB_W] = ""; static int s_compIdx = -1;

// ── input line ──
static char s_line[SB_W]; static int s_linelen = 0, s_cur_x = 0;

// ── connection / state ──
enum { ST_FORM, ST_CONNECTING, ST_SHELL, ST_CLOSED };
enum { AUTH_PW, AUTH_KEY };
static int s_state = ST_FORM;
static char s_host[80] = "", s_user[32] = "", s_pass[80] = "", s_passph[80] = "";
static int  s_auth = AUTH_PW;
static int  s_field = 0;
static char s_status[96] = "";
static volatile bool s_quit = false;
static TaskHandle_t s_task = nullptr;
static StreamBufferHandle_t s_in = nullptr;
static int  s_zoom = 1;
static bool s_manual = false;
// key files
#define MAX_KEYS 12
static char s_keys[MAX_KEYS][48]; static int s_keysN = 0, s_keyIdx = 0;

static void set_status(const char *s) { snprintf(s_status, sizeof s_status, "%s", s); s_dirty = true; nucleo_app_request_draw(); }
static void send_bytes(const char *b, int n) { if (s_in) xStreamBufferSend(s_in, b, n, 0); }
static void scan_keys() { s_keysN = 0; ensure_dirs(); DIR *dp = opendir(KEYS_DIR); if (!dp) return;
    struct dirent *e; while ((e = readdir(dp)) && s_keysN < MAX_KEYS) { if (e->d_name[0] == '.') continue; if (strstr(e->d_name, ".pub")) continue; snprintf(s_keys[s_keysN++], 48, "%s", e->d_name); } closedir(dp); }
static int form_nfields() { return s_auth == AUTH_PW ? 4 : 5; }   // host,user,auth,(pass | key,passph)

// ── host-key pinning helpers ──
static int kh_keybit(int t) {
    switch (t) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}
// returns 0 ok, -1 abort (mismatch). Surfaces the fingerprint + TOFU note in the scrollback.
static int pin_hostkey(LIBSSH2_SESSION *sess) {
    size_t klen = 0; int ktype = 0;
    const char *hkey = libssh2_session_hostkey(sess, &klen, &ktype);
    const char *fp = libssh2_hostkey_hash(sess, LIBSSH2_HOSTKEY_HASH_SHA256);   // 32 raw bytes
    char fline[100]; int o = snprintf(fline, sizeof fline, "host key SHA256 ");
    if (fp) for (int i = 0; i < 32 && o < (int)sizeof(fline) - 3; i++) o += snprintf(fline + o, sizeof fline - o, "%02x", (unsigned char)fp[i]);
    sb_info(fline);
    if (!hkey) return 0;
    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(sess);
    if (!kh) return 0;
    ensure_dirs(); libssh2_knownhost_readfile(kh, KH_PATH, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    int chk = libssh2_knownhost_checkp(kh, s_host, 22, hkey, klen,
                                       LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, NULL);
    int rc = 0;
    if (chk == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) { sb_info("!! HOST KEY CHANGED - possibile MITM, annullo"); set_status(s_en ? "host key changed - aborted" : "host key cambiata - annullato"); rc = -1; }
    else if (chk == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        libssh2_knownhost_addc(kh, s_host, NULL, hkey, klen, "nucleoos", 8,
                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | kh_keybit(ktype), NULL);
        libssh2_knownhost_writefile(kh, KH_PATH, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        sb_info(s_en ? "new host - trusted (TOFU), saved" : "host nuovo - fidato (TOFU), salvato");
    } else sb_info(s_en ? "host key OK (known)" : "host key OK (conosciuta)");
    libssh2_knownhost_free(kh);
    return rc;
}

// ── SSH worker ──
static int tcp_connect(const char *host, int port) {
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); freeaddrinfo(res); return -1; }
    freeaddrinfo(res); return s;
}
static int term_cols() { return s_zoom == 2 ? 19 : 39; }

static void ssh_task(void *arg) {
    int sock = -1; LIBSSH2_SESSION *sess = nullptr; LIBSSH2_CHANNEL *chan = nullptr; int authok = 0;
    set_status(s_en ? "resolving host..." : "risolvo host...");
    sock = tcp_connect(s_host, 22);
    if (sock < 0) { set_status(s_en ? "host unreachable" : "host non raggiungibile"); goto done; }
    if (libssh2_init(0) != 0) { set_status("libssh2 init failed"); goto done; }
    sess = libssh2_session_init();
    if (!sess) { set_status("session failed"); goto done; }
    libssh2_session_set_blocking(sess, 1);
    set_status(s_en ? "handshake..." : "handshake...");
    if (libssh2_session_handshake(sess, sock) != 0) { set_status(s_en ? "handshake failed" : "handshake fallito"); goto done; }
    if (pin_hostkey(sess) < 0) goto done;                            // abort on a changed host key
    set_status(s_en ? "authenticating..." : "autenticazione...");
    if (s_auth == AUTH_KEY) {
        char priv[160]; snprintf(priv, sizeof priv, "%s/%s", KEYS_DIR, s_keysN ? s_keys[s_keyIdx] : "");
        authok = (libssh2_userauth_publickey_fromfile_ex(sess, s_user, strlen(s_user), NULL, priv, s_passph[0] ? s_passph : NULL) == 0);
    } else {
        authok = (libssh2_userauth_password(sess, s_user, s_pass) == 0);
    }
    if (!authok) { set_status(s_en ? "auth refused" : "autenticazione rifiutata"); goto done; }
    chan = libssh2_channel_open_session(sess);
    if (!chan) { set_status("channel failed"); goto done; }
    libssh2_channel_request_pty_ex(chan, "vt100", 5, NULL, 0, term_cols(), 14, 0, 0);
    if (libssh2_channel_shell(chan) != 0) { set_status("shell failed"); goto done; }
    memset(s_pass, 0, sizeof s_pass); memset(s_passph, 0, sizeof s_passph);
    s_state = ST_SHELL; set_status(s_en ? "connected" : "connesso");
    libssh2_session_set_blocking(sess, 0);
    while (!s_quit) {
        char buf[512];
        int n = libssh2_channel_read(chan, buf, sizeof buf);
        if (n > 0) { sb_feed(buf, n); nucleo_app_request_draw(); }
        else if (n == 0) { if (libssh2_channel_eof(chan)) { set_status(s_en ? "closed by server" : "chiuso dal server"); break; } }
        else if (n != LIBSSH2_ERROR_EAGAIN) { set_status(s_en ? "read error" : "errore lettura"); break; }
        char ib[256]; size_t got = s_in ? xStreamBufferReceive(s_in, ib, sizeof ib, 0) : 0;
        if (got > 0) { int off = 0; while (off < (int)got) { int w = libssh2_channel_write(chan, ib + off, got - off); if (w > 0) off += w; else break; } }
        if (n == LIBSSH2_ERROR_EAGAIN && got == 0) vTaskDelay(pdMS_TO_TICKS(12));
    }
done:
    s_state = ST_CLOSED;
    if (chan) { libssh2_channel_close(chan); libssh2_channel_free(chan); }
    if (sess) { libssh2_session_disconnect(sess, "bye"); libssh2_session_free(sess); }
    if (sock >= 0) close(sock);
    libssh2_exit();
    s_dirty = true; nucleo_app_request_draw();
    s_task = nullptr; vTaskDelete(nullptr);
}

static void start_connect() {
    if (!s_host[0] || !s_user[0]) { set_status(s_en ? "host and user required" : "host e utente obbligatori"); return; }
    if (s_auth == AUTH_KEY && !s_keysN) { set_status(s_en ? "no key in /sd/data/ssh/keys" : "nessuna chiave in /sd/data/ssh/keys"); return; }
    // Exclusive is already active (declarative, entered at app open). This idempotent re-enter just
    // re-reads the live heap into inf for the RAM gate — it adds no flags and never exits on failure
    // here (the framework owns the lifetime and restores on close).
    nucleo_exclusive_info_t inf; nucleo_exclusive_enter(NX_NET_APP, &inf);
    ESP_LOGI(TAG, "exclusive: free=%u largest=%u", (unsigned)inf.free_after, (unsigned)inf.largest_after);
    if (s_sb)    { free(s_sb);    s_sb = nullptr; }     // free stale buffers if a prior attempt failed after malloc
    if (s_sbcol) { free(s_sbcol); s_sbcol = nullptr; }
    s_sb = (char *)malloc((size_t)SB_LINES * SB_W);
    if (!s_sb || inf.largest_after < 34 * 1024) {
        free(s_sb); s_sb = nullptr;
        set_status(s_en ? "not enough RAM for SSH" : "RAM insufficiente per SSH"); s_state = ST_CLOSED; return;
    }
    // colour plane only when RAM is comfortable; otherwise run monochrome
    s_sbcol = (inf.free_after > 90 * 1024) ? (uint8_t *)malloc((size_t)SB_LINES * SB_W) : nullptr;
    s_head = s_count = s_curlen = s_scroll = 0; s_aesc = s_pn = 0; s_mask = false; s_cur[0] = 0;
    s_fgbase = DEF_FG; s_bold = false;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_in) s_in = xStreamBufferCreate(1024, 1);
    s_quit = false; s_line[0] = 0; s_linelen = s_cur_x = 0;
    s_state = ST_CONNECTING; set_status(s_en ? "starting..." : "avvio...");
    if (xTaskCreate(ssh_task, "ssh", 16384, nullptr, tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
        set_status("task failed"); s_state = ST_CLOSED;   // exclusive stays (declarative); freed on app close
    }
}

// ── input editing ──
static void line_insert(char c) { if (s_linelen >= SB_W - 1) return; memmove(&s_line[s_cur_x + 1], &s_line[s_cur_x], s_linelen - s_cur_x + 1); s_line[s_cur_x] = c; s_linelen++; s_cur_x++; s_compIdx = -1; }
static void line_backspace() { if (s_cur_x <= 0) return; memmove(&s_line[s_cur_x - 1], &s_line[s_cur_x], s_linelen - s_cur_x + 1); s_linelen--; s_cur_x--; s_compIdx = -1; }
static void line_clear() { s_line[0] = 0; s_linelen = s_cur_x = 0; s_compIdx = -1; }
static void line_set(const char *t) { snprintf(s_line, SB_W, "%s", t); s_linelen = strlen(s_line); s_cur_x = s_linelen; s_compIdx = -1; }
static void do_send_line() { hist_add_mem(s_line); hist_persist(s_line); char tmp[SB_W + 2]; int n = snprintf(tmp, sizeof tmp, "%s\n", s_line); send_bytes(tmp, n); line_clear(); s_scroll = 0; s_dirty = true; }
static void do_complete() {
    int ws = s_cur_x; while (ws > 0 && s_line[ws - 1] != ' ') ws--;
    char tok[SB_W]; int tl = s_cur_x - ws; if (tl < 0) tl = 0; if (tl > SB_W - 1) tl = SB_W - 1;
    memcpy(tok, &s_line[ws], tl); tok[tl] = 0;
    static const char *cand[170]; int nc = 0; bool whole = (ws == 0);
    for (int i = 0; CMDS[i] && nc < 160; i++) if (strncmp(CMDS[i], tok, tl) == 0) cand[nc++] = CMDS[i];
    if (whole) for (int i = s_histN - 1; i >= 0 && nc < 168; i--) if (strncmp(s_hist[i], tok, tl) == 0) cand[nc++] = s_hist[i];
    if (nc == 0) return;
    if (strcmp(tok, s_compPrefix) != 0 || s_compIdx < 0) { snprintf(s_compPrefix, sizeof s_compPrefix, "%s", tok); s_compIdx = 0; }
    else s_compIdx = (s_compIdx + 1) % nc;
    const char *pick = cand[s_compIdx]; char rest[SB_W]; snprintf(rest, sizeof rest, "%s", &s_line[s_cur_x]);
    int pl = strlen(pick); if (ws + pl >= SB_W) pl = SB_W - 1 - ws;
    memcpy(&s_line[ws], pick, pl); s_line[ws + pl] = 0; strncat(s_line, rest, SB_W - 1 - (ws + pl));
    s_cur_x = ws + pl; s_linelen = strlen(s_line);
    char st[48]; snprintf(st, sizeof st, "tab %d/%d", s_compIdx + 1, nc); set_status(st); s_dirty = true;
}

// ── keys ──
static void form_key(int key, char ch) {
    int nf = form_nfields();
    // resolve the editable target for the current field
    char *txt = nullptr; int cap = 0;
    if (s_field == 0) { txt = s_host; cap = sizeof s_host; }
    else if (s_field == 1) { txt = s_user; cap = sizeof s_user; }
    else if (s_field == 2) { /* auth toggle, no text */ }
    else if (s_auth == AUTH_PW && s_field == 3) { txt = s_pass; cap = sizeof s_pass; }
    else if (s_auth == AUTH_KEY && s_field == 4) { txt = s_passph; cap = sizeof s_passph; }
    if (key == NK_DOWN || key == NK_TAB) { s_field = (s_field + 1) % nf; }
    else if (key == NK_UP) { s_field = (s_field + nf - 1) % nf; }
    else if (key == NK_RIGHT) {
        if (s_field == 2) s_auth = (s_auth == AUTH_PW) ? AUTH_KEY : AUTH_PW;
        else if (s_auth == AUTH_KEY && s_field == 3 && s_keysN) s_keyIdx = (s_keyIdx + 1) % s_keysN;
    }
    else if (key == NK_ENTER) { if (s_field < nf - 1) s_field++; else start_connect(); }
    else if (key == NK_DEL) { if (txt) { int L = strlen(txt); if (L > 0) txt[L-1] = 0; } }
    else if (key == NK_CHAR && ch >= 32 && ch < 127) {
        if (s_field == 2) { if (ch == 'k' || ch == 'K') s_auth = AUTH_KEY; else if (ch == 'p' || ch == 'P') s_auth = AUTH_PW; }
        else if (txt) { int L = strlen(txt); if (L < cap - 1) { txt[L] = ch; txt[L+1] = 0; } }
    }
    s_dirty = true; nucleo_app_request_draw();
}
static void on_key(int key, char ch) {
    if (s_manual) { s_manual = false; s_dirty = true; nucleo_app_request_draw(); return; }
    if (s_state == ST_FORM) { form_key(key, ch); return; }
    if (s_state == ST_CLOSED) { if (s_sb) { free(s_sb); s_sb = nullptr; } if (s_sbcol) { free(s_sbcol); s_sbcol = nullptr; } s_state = ST_FORM; s_dirty = true; nucleo_app_request_draw(); return; }
    if (s_state != ST_SHELL) return;
    unsigned m = nucleo_kbd_mods();
    int rows = (nucleo_app_content_height() - 23) / (8 * s_zoom); if (rows < 1) rows = 1;
    if (m & NK_MOD_FN) {
        if (key == NK_UP)   { s_scroll += rows - 1 > 0 ? rows - 1 : 1; if (s_scroll > s_count - 1) s_scroll = s_count > 0 ? s_count - 1 : 0; s_dirty = true; nucleo_app_request_draw(); return; }
        if (key == NK_DOWN) { s_scroll -= rows - 1 > 0 ? rows - 1 : 1; if (s_scroll < 0) s_scroll = 0; s_dirty = true; nucleo_app_request_draw(); return; }
        if (key == NK_CHAR && (ch == 'z' || ch == 'Z')) { s_zoom = s_zoom == 1 ? 2 : 1; s_dirty = true; nucleo_app_request_draw(); return; }
        if (key == NK_CHAR && (ch == 'h' || ch == 'H' || ch == '?')) { s_manual = true; s_dirty = true; nucleo_app_request_draw(); return; }
    }
    if (key == NK_CHAR) { if (ch >= 32 && ch < 127) { line_insert(ch); s_dirty = true; nucleo_app_request_draw(); } return; }
    if (key == NK_ENTER) { do_send_line(); nucleo_app_request_draw(); return; }
    if (key == NK_DEL)   { line_backspace(); s_dirty = true; nucleo_app_request_draw(); return; }
    if (key == NK_TAB)   { do_complete(); nucleo_app_request_draw(); return; }
    if (key == NK_UP)    { if (s_histN) { if (s_histPos < 0) s_histPos = s_histN; if (s_histPos > 0) s_histPos--; line_set(s_hist[s_histPos]); s_dirty = true; nucleo_app_request_draw(); } return; }
    if (key == NK_DOWN)  { if (s_histPos >= 0 && s_histPos < s_histN - 1) { s_histPos++; line_set(s_hist[s_histPos]); } else { s_histPos = -1; line_clear(); } s_dirty = true; nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT) { if (s_cur_x < s_linelen) s_cur_x++; s_dirty = true; nucleo_app_request_draw(); return; }
}
static bool back_handler(int key) {
    if (s_manual) { s_manual = false; s_dirty = true; nucleo_app_request_draw(); return true; }
    if (s_state == ST_SHELL) {
        if (nucleo_kbd_mods() & NK_MOD_FN) return false;        // Fn+Esc / Fn+Left → close the app
        if (key == NK_LEFT) { if (s_cur_x > 0) s_cur_x--; s_dirty = true; nucleo_app_request_draw(); return true; }
        line_clear(); s_dirty = true; nucleo_app_request_draw(); return true;   // Esc clears the line
    }
    if (s_state == ST_FORM && key == NK_LEFT) {                 // Left in the form = toggle/cycle (mirror of Right)
        int nf = form_nfields();
        if (s_field == 2) { s_auth = (s_auth == AUTH_PW) ? AUTH_KEY : AUTH_PW; s_dirty = true; nucleo_app_request_draw(); return true; }
        if (s_auth == AUTH_KEY && s_field == 3 && s_keysN) { s_keyIdx = (s_keyIdx + s_keysN - 1) % s_keysN; s_dirty = true; nucleo_app_request_draw(); return true; }
        (void)nf;
    }
    return false;
}

// ── draw ──
static void draw_text(int x, int y, const char *s, unsigned short fg, unsigned short bg, int zoom) {
    d.setFont(&fonts::Font0); d.setTextSize(zoom); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}
static void draw_form() {
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), C_BG);
    int nf = form_nfields(); const char *lab; char val[96];
    for (int i = 0; i < nf; i++) {
        int y = top + 4 + i * 21; bool sel = (i == s_field);
        if (i == 0) { lab = "Host"; snprintf(val, sizeof val, "%s", s_host); }
        else if (i == 1) { lab = s_en ? "User" : "Utente"; snprintf(val, sizeof val, "%s", s_user); }
        else if (i == 2) { lab = "Auth"; snprintf(val, sizeof val, "< %s >", s_auth == AUTH_PW ? "Password" : (s_en ? "Key" : "Chiave")); }
        else if (s_auth == AUTH_PW && i == 3) { lab = "Password"; int n = strlen(s_pass); for (int k = 0; k < n && k < 90; k++) val[k] = '*'; val[n < 90 ? n : 90] = 0; }
        else if (s_auth == AUTH_KEY && i == 3) { lab = s_en ? "Key" : "Chiave"; snprintf(val, sizeof val, s_keysN ? "< %s >" : "%s", s_keysN ? s_keys[s_keyIdx] : (s_en ? "(none in keys/)" : "(nessuna in keys/)")); }
        else { lab = "Passphrase"; int n = strlen(s_passph); for (int k = 0; k < n && k < 90; k++) val[k] = '*'; val[n < 90 ? n : 90] = 0; }
        draw_text(8, y, lab, sel ? ACCENT : C_DIM, C_BG, 1);
        d.fillRect(64, y - 1, 168, 11, sel ? C_INBG : C_BG);
        draw_text(67, y, val, C_INK, sel ? C_INBG : C_BG, 1);
    }
    draw_text(8, top + 4 + nf * 21, s_status, C_WARN, C_BG, 1);
}
// draw one text/colour segment, grouping equal-colour runs
static void draw_run(int x, int y, const char *txt, const uint8_t *col, int len, int zoom) {
    int charW = 6 * zoom;
    if (!col) { draw_text(x, y, txt, PAL[DEF_FG], C_BG, zoom); return; }
    int i = 0; while (i < len) { int j = i + 1; while (j < len && col[j] == col[i]) j++;
        char piece[SB_W]; int pl = j - i; memcpy(piece, txt + i, pl); piece[pl] = 0;
        draw_text(x + i * charW, y, piece, PAL[col[i] & 15], C_BG, zoom); i = j; }
}
static void draw_shell() {
    int top = nucleo_app_content_top(), H = nucleo_app_content_height();
    int rowH = 8 * s_zoom, charW = 6 * s_zoom, cols = 236 / charW;
    int sbTop = top + 9, inH = 14, sbH = H - 9 - inH, rows = sbH / rowH;
    d.startWrite();
    d.fillRect(0, top, 240, H, C_BG);
    char st[80]; snprintf(st, sizeof st, "%s %s@%s%s", s_scroll ? "^scroll" : "*", s_user, s_host, s_sbcol ? "" : " [mono]");
    draw_text(2, top, st, s_scroll ? C_WARN : ACCENT, C_BG, 1);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int li = s_count - 1 - s_scroll; int y = sbTop + (rows - 1) * rowH;
    while (li >= 0 && y >= sbTop) {
        const char *ln = sb_line(li); const uint8_t *cl = sb_col(li);
        int len = strlen(ln); int nseg = len <= 0 ? 1 : (len + cols - 1) / cols;
        for (int seg = nseg - 1; seg >= 0 && y >= sbTop; seg--) {
            int off = seg * cols; int pl = len - off; if (pl > cols) pl = cols; if (pl < 0) pl = 0;
            char piece[SB_W]; memcpy(piece, ln + off, pl); piece[pl] = 0;
            draw_run(2, y, piece, cl ? cl + off : nullptr, pl, s_zoom);
            y -= rowH;
        }
        li--;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    int iy = top + H - inH; d.fillRect(0, iy, 240, inH, C_INBG);
    int icw = 6, icols = (228 - 12) / icw, from = s_cur_x > icols ? s_cur_x - icols : 0;
    char shown[SB_W]; int k = 0; for (int i = from; i < s_linelen && k < icols; i++, k++) shown[k] = s_mask ? '*' : s_line[i]; shown[k] = 0;
    draw_text(2, iy + 3, ">", ACCENT, C_INBG, 1); draw_text(12, iy + 3, shown, C_INK, C_INBG, 1);
    d.fillRect(12 + (s_cur_x - from) * icw, iy + 2, 2, 10, ACCENT);
    d.endWrite();
}
static void draw_connecting() {
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), C_BG);
    char l[120]; snprintf(l, sizeof l, "SSH %s@%s", s_user, s_host);
    draw_text(8, top + 18, l, C_INK, C_BG, 1); draw_text(8, top + 38, s_status, C_DIM, C_BG, 1);
}
static void draw_manual() {
    int top = nucleo_app_content_top(), H = nucleo_app_content_height();
    d.fillRect(0, top, 240, H, C_BG);
    draw_text(8, top + 3, s_en ? "SSH - quick help" : "SSH - guida rapida", ACCENT, C_BG, 1);
    const char *it[] = { "Invio  esegui comando", "Su/Giu  cronologia", "Tab  completa (cmd+storia)",
        "Sin/Des  cursore", "Esc  cancella riga", "Fn+Su/Giu  scorri output", "Fn+Z  zoom font",
        "Fn+H  guida", "Fn+Esc  esci", "Auth: Password o Chiave", "Chiavi in /sd/data/ssh/keys",
        "host key fidata al 1o uso", "(no editor full-screen)", nullptr };
    const char *en[] = { "Enter  run command", "Up/Down  history", "Tab  complete (cmd+hist)",
        "Left/Right  cursor", "Esc  clear line", "Fn+Up/Down  scroll output", "Fn+Z  zoom font",
        "Fn+H  help", "Fn+Esc  quit", "Auth: Password or Key", "Keys in /sd/data/ssh/keys",
        "host key trusted on 1st use", "(no full-screen editors)", nullptr };
    const char **m = s_en ? en : it;
    for (int i = 0; m[i]; i++) draw_text(8, top + 15 + i * 8, m[i], i >= 9 ? C_DIM : C_INK, C_BG, 1);
}
static void on_draw() {
    s_dirty = false;
    if (s_manual) draw_manual();
    else if (s_state == ST_FORM) draw_form();
    else if (s_state == ST_SHELL) draw_shell();
    else if (s_state == ST_CLOSED) {
        int top = nucleo_app_content_top();
        d.fillRect(0, top, 240, nucleo_app_content_height(), C_BG);
        draw_text(8, top + 18, s_status[0] ? s_status : (s_en ? "Disconnected" : "Disconnesso"), C_WARN, C_BG, 1);
        draw_text(8, top + 38, s_en ? "press a key for the form" : "premi un tasto per il form", C_DIM, C_BG, 1);
    } else draw_connecting();
}
static void on_tick() { if (s_dirty) nucleo_app_request_draw(); }
static void on_enter() {
    s_state = ST_FORM; s_field = 0; s_status[0] = 0; s_dirty = true; s_zoom = 1; s_manual = false;
    s_auth = AUTH_PW; s_keyIdx = 0;
    // The terminal paints straight to the panel (every draw_* uses d.*), so the 32 KB shared canvas
    // is dead weight: release it AND pin direct draw so the run loop won't composite an empty canvas
    // over us — handing that contiguous block to libssh2/mbedTLS (the start_connect 34 KB gate now
    // sees it). close_app() re-acquires the canvas and clears the flag on exit. Mirrors app_anima.
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);
    scan_keys(); hist_load();
    nucleo_app_set_hint(s_en ? "arrows/Tab field  </>auth/key  Enter go  Fn+H help" : "frecce/Tab campo  </>auth/chiave  Invio vai  Fn+H aiuto");
    nucleo_app_set_back_handler(back_handler);
    nucleo_app_request_draw();
}
static void on_exit() {
    s_quit = true;
    for (int i = 0; i < 220 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
    if (s_in) { vStreamBufferDelete(s_in); s_in = nullptr; }
    if (s_sb) { free(s_sb); s_sb = nullptr; }
    if (s_sbcol) { free(s_sbcol); s_sbcol = nullptr; }
    memset(s_pass, 0, sizeof s_pass); memset(s_passph, 0, sizeof s_passph);
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();
}

extern "C" void nucleo_register_ssh(void) {
    static const nucleo_app_def_t app = {
        "ssh", "SSH", "Connect", "Terminale SSH nativo (modalita dedicata)",
        's', 0x2EE0, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP   // declarative: dedicated RAM for the whole SSH session; framework restores on close
    };
    nucleo_app_register(&app);
}
