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
#include "app_large.h"     // two screens: the terminal at 320x240 on the panel, typing on the Cardputer
#include "nucleo_touch.h"
#include "nucleo_i18n.h"   // s_en follows the OS language (ui.language)
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

[[maybe_unused]] static const char *TAG = "app.ssh";   // used by ESP_LOGI on-device; no-op (unused) on the host

// Two-screen touch: a drag inside the panel's scrollback delivers these (as NK_CHAR, control codes that are
// never typed) so a finger pages the output the way Fn+Up/Fn+Down do on the Cardputer. Form field taps use the
// standard NUCLEO_TOUCH_ROW codes; the Connect button taps one slot past the last field.
#define SSH_TSCROLL_UP   0x1e
#define SSH_TSCROLL_DOWN 0x1f

#define SSH_DIR   NUCLEO_SD_MOUNT "/data/ssh"
#define HIST_PATH SSH_DIR "/history.txt"
#define KH_PATH   SSH_DIR "/known_hosts"
#define KEYS_DIR  SSH_DIR "/keys"

static bool s_en = false;   // mirrors the OS language; seeded from nucleo_i18n in on_enter (bilingual strings throughout)

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
static char  *s_cur; static uint8_t *s_curcol; static int s_curlen = 0;   // SB_W each, APP_RAM (0 B when closed)
static int   s_aesc = 0, s_p[6], s_pn = 0;     // CSI param accumulator (for SGR)
static uint8_t s_fgbase = DEF_FG; static bool s_bold = false;
static int   s_scroll = 0;
static bool  s_mask = false;
static bool  s_cr = false;          // CR seen: erase the line only if TEXT follows (so "\r\n" keeps it)
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
static void sb_putc(char c) {
    if (s_cr) { s_cr = false; s_curlen = 0; s_cur[0] = 0; }   // bare CR + text = overwrite the line
    if (s_curlen < SB_W - 1) { if (s_sbcol) s_curcol[s_curlen] = cur_fg(); s_cur[s_curlen++] = c; s_cur[s_curlen] = 0; } }
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
            else if (c == '\n') { s_cr = false; sb_commit(); }
            else if (c == '\r') s_cr = true;   // deferred: servers end EVERY line with \r\n; erasing here blanked all output
            else if (c == '\t') { int t = (s_curlen / 8 + 1) * 8; if (t > SB_W-1) t = SB_W-1; while (s_curlen < t) sb_putc(' '); }  // clamp: sb_putc caps at SB_W-1, so an unclamped target near EOL spins forever holding s_lock
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
static char (*s_hist)[SB_W]; static int s_histN = 0, s_histPos = -1;   // HIST_MAX rows, APP_RAM; hist_load() refills it on_enter
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
static char *s_compPrefix; static int s_compIdx = -1;   // SB_W, APP_RAM

// ── input line ──
static char *s_line; static int s_linelen = 0, s_cur_x = 0;   // SB_W, APP_RAM

// ── connection / state ──
enum { ST_FORM, ST_CONNECTING, ST_SHELL, ST_CLOSED };
enum { AUTH_PW, AUTH_KEY };
static int s_state = ST_FORM;
static char s_host[80] = "", s_user[32] = "", s_pass[80] = "", s_passph[80] = "";
static int  s_auth = AUTH_PW;
static int  s_field = 0;
#define STATUS_W 96
static char *s_status;                 // STATUS_W, APP_RAM (ssh_task writes it, but on_exit stops/deletes the task first)
static volatile bool s_quit = false;
static TaskHandle_t s_task = nullptr;
static StreamBufferHandle_t s_in = nullptr;
static int  s_zoom = 1;
static bool s_manual = false;
// key files
#define MAX_KEYS 12
#define KEY_W 48
static char (*s_keys)[KEY_W]; static int s_keysN = 0, s_keyIdx = 0;   // MAX_KEYS rows, APP_RAM; scan_keys() refills it on_enter
#define CAND_MAX 170
static const char **s_cand;            // CAND_MAX Tab-completion candidates (do_complete scratch), APP_RAM

static void set_status(const char *s) { snprintf(s_status, STATUS_W, "%s", s); s_dirty = true; nucleo_app_request_draw(); }
static void send_bytes(const char *b, int n) { if (s_in) xStreamBufferSend(s_in, b, n, 0); }
static void scan_keys() { s_keysN = 0; ensure_dirs(); DIR *dp = opendir(KEYS_DIR); if (!dp) return;
    struct dirent *e; while ((e = readdir(dp)) && s_keysN < MAX_KEYS) { if (e->d_name[0] == '.') continue; if (strstr(e->d_name, ".pub")) continue; snprintf(s_keys[s_keysN++], KEY_W, "%s", e->d_name); } closedir(dp); }
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
    if (chk == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) { sb_info(s_en ? "!! HOST KEY CHANGED - possible MITM, aborting" : "!! HOST KEY CHANGED - possibile MITM, annullo"); set_status(s_en ? "host key changed - aborted" : "host key cambiata - annullato"); rc = -1; }
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
    s_head = s_count = s_curlen = s_scroll = 0; s_aesc = s_pn = 0; s_mask = false; s_cr = false; s_cur[0] = 0;
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
    const char **cand = s_cand; int nc = 0; bool whole = (ws == 0);
    for (int i = 0; CMDS[i] && nc < 160; i++) if (strncmp(CMDS[i], tok, tl) == 0) cand[nc++] = CMDS[i];
    if (whole) for (int i = s_histN - 1; i >= 0 && nc < 168; i--) if (strncmp(s_hist[i], tok, tl) == 0) cand[nc++] = s_hist[i];
    if (nc == 0) return;
    if (strcmp(tok, s_compPrefix) != 0 || s_compIdx < 0) { snprintf(s_compPrefix, SB_W, "%s", tok); s_compIdx = 0; }
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
    if (nucleo_touch_is_row(key, ch)) {                 // panel: tap a field to edit it; tap past the last = Connect
        int idx = ch - NUCLEO_TOUCH_ROW;
        if (idx >= nf) { start_connect(); return; }
        if (idx == 2 && s_field == 2) s_auth = (s_auth == AUTH_PW) ? AUTH_KEY : AUTH_PW;  // re-tap Auth toggles it
        else s_field = idx;
        s_dirty = true; nucleo_app_request_draw(); return;
    }
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
#ifdef __MINGW32__
// Host gallery ONLY (MinGW; the firmware target is xtensa and never defines __MINGW32__): seed a
// fictional connected shell so the panel terminal (draw_shell_large) is actually exercised on the PC.
// A live libssh2 session cannot be stood up in a render, so the ui-host scene injects a sentinel key
// (c:~) that drops us straight into ST_SHELL with canned output. Never compiled into the firmware.
static void ssh_demo_shell() {
    if (!s_sb) s_sb = (char *)calloc((size_t)SB_LINES * SB_W, 1);
    if (!s_sbcol) s_sbcol = (uint8_t *)calloc((size_t)SB_LINES * SB_W, 1);
    if (!s_sb) return;
    if (!s_lock) s_lock = (SemaphoreHandle_t)1;   // non-null so sb_feed() runs (host sem calls are no-ops)
    snprintf(s_host, sizeof s_host, "pi.local");
    snprintf(s_user, sizeof s_user, "nicola");
    s_head = s_count = s_curlen = s_scroll = 0; s_aesc = s_pn = 0; s_cur[0] = 0;
    s_fgbase = DEF_FG; s_bold = false;
    static const char *const L[] = {
        "Linux pi 6.1.0-rpi7 aarch64 GNU/Linux",
        "Last login: Mon Oct 27 10:14:02 2026 from 192.168.1.5",
        "nicola@pi:~ $ ls -la",
        "total 28",
        "drwxr-xr-x 4 nicola nicola 4096 Oct 27 10:02 .",
        "-rw-r--r-- 1 nicola nicola  220 Sep  1  2025 .bash_logout",
        "drwxr-xr-x 2 nicola nicola 4096 Oct 20 18:33 projects",
        "nicola@pi:~ $ uptime",
        " 10:14:31 up 6 days,  2:41,  load average: 0.08, 0.03, 0.01",
        "nicola@pi:~ $ ",
    };
    for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++) sb_info(L[i]);
    snprintf(s_line, SB_W, "sudo systemctl status nginx");
    s_linelen = (int)strlen(s_line); s_cur_x = s_linelen;
    s_state = ST_SHELL; s_dirty = true;
}
#endif

static void on_key(int key, char ch) {
#ifdef __MINGW32__
    if (key == NK_CHAR && ch == '~') { ssh_demo_shell(); nucleo_app_request_draw(); return; }
#endif
    if (s_manual) { s_manual = false; s_dirty = true; nucleo_app_request_draw(); return; }
    if (s_state == ST_FORM) { form_key(key, ch); return; }
    if (s_state == ST_CLOSED) { if (s_sb) { free(s_sb); s_sb = nullptr; } if (s_sbcol) { free(s_sbcol); s_sbcol = nullptr; } s_state = ST_FORM; s_dirty = true; nucleo_app_request_draw(); return; }
    if (s_state != ST_SHELL) return;
    unsigned m = nucleo_kbd_mods();
    int rows = (nucleo_app_content_height() - 23) / (8 * s_zoom); if (rows < 1) rows = 1;
    if (key == NK_CHAR && (ch == SSH_TSCROLL_UP || ch == SSH_TSCROLL_DOWN)) {   // panel: drag the scrollback
        if (ch == SSH_TSCROLL_UP) { s_scroll += 1; if (s_scroll > s_count - 1) s_scroll = s_count > 0 ? s_count - 1 : 0; }
        else                      { s_scroll -= 1; if (s_scroll < 0) s_scroll = 0; }
        s_dirty = true; nucleo_app_request_draw(); return;
    }
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
// s_big: the panel terminal's 8x16 cells (Font0 scaled 4/3 x 2: 39 columns = the PTY's width on 312 px).
static bool s_big = false;
static void draw_text(int x, int y, const char *s, unsigned short fg, unsigned short bg, int zoom) {
    d.setFont(&fonts::Font0);
    if (s_big) d.setTextSize(zoom * 1.34f, zoom * 2.0f);   // 6 x 1.34 -> an 8 px advance (4/3 rounds to 7) else d.setTextSize(zoom);
    d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}
// One connect-form field's label and shown value (passwords masked), for the compact form and the panel's.
static const char *form_field(int i, char *val, int cap) {
    const char *lab = "Passphrase", *pw = s_passph;
    if (i == 0) { snprintf(val, cap, "%s", s_host); return "Host"; }
    if (i == 1) { snprintf(val, cap, "%s", s_user); return s_en ? "User" : "Utente"; }
    if (i == 2) { snprintf(val, cap, "< %s >", s_auth == AUTH_PW ? "Password" : (s_en ? "Key" : "Chiave")); return "Auth"; }
    if (s_auth == AUTH_KEY && i == 3) { snprintf(val, cap, s_keysN ? "< %s >" : "%s", s_keysN ? s_keys[s_keyIdx] : (s_en ? "(none in keys/)" : "(nessuna in keys/)")); return s_en ? "Key" : "Chiave"; }
    if (i == 3) { lab = "Password"; pw = s_pass; }
    int n = strlen(pw), k = 0; for (; k < n && k < cap - 1; k++) val[k] = '*'; val[k] = 0;
    return lab;
}
static const char *const HELP_IT[] = { "Invio  esegui comando", "Su/Giu  cronologia", "Tab  completa (cmd+storia)",
    "Sin/Des  cursore", "Esc  cancella riga", "Fn+Su/Giu  scorri output", "Fn+Z  zoom font",
    "Fn+H  guida", "Fn+Esc  esci", "Auth: Password o Chiave", "Chiavi in /sd/data/ssh/keys",
    "host key fidata al 1o uso", "(no editor full-screen)", nullptr };
static const char *const HELP_EN[] = { "Enter  run command", "Up/Down  history", "Tab  complete (cmd+hist)",
    "Left/Right  cursor", "Esc  clear line", "Fn+Up/Down  scroll output", "Fn+Z  zoom font",
    "Fn+H  help", "Fn+Esc  quit", "Auth: Password or Key", "Keys in /sd/data/ssh/keys",
    "host key trusted on 1st use", "(no full-screen editors)", nullptr };
static void draw_form() {
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), C_BG);
    int nf = form_nfields(); char val[96];
    for (int i = 0; i < nf; i++) {
        int y = top + 4 + i * 21; bool sel = (i == s_field);
        const char *lab = form_field(i, val, sizeof val);
        draw_text(8, y, lab, sel ? ACCENT : C_DIM, C_BG, 1);
        d.fillRect(64, y - 1, 168, 11, sel ? C_INBG : C_BG);
        draw_text(67, y, val, C_INK, sel ? C_INBG : C_BG, 1);
    }
    draw_text(8, top + 4 + nf * 21, s_status, C_WARN, C_BG, 1);
}
// draw one text/colour segment, grouping equal-colour runs
static void draw_run(int x, int y, const char *txt, const uint8_t *col, int len, int zoom) {
    int charW = (s_big ? 8 : 6) * zoom;
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
    const char *const *m = s_en ? HELP_EN : HELP_IT;
    for (int i = 0; m[i]; i++) draw_text(8, top + 15 + i * 8, m[i], i >= 9 ? C_DIM : C_INK, C_BG, 1);
}
// ===== two screens =============================================================================================
// The panel is the terminal at 320x240 — more columns and rows than the Cardputer — while the typing, history,
// completion and scrolling stay on the Cardputer keyboard (the companion shows the line you are typing, big, and
// the key map). The connect form is laid out as big tappable field rows with a Connect button; a drag on the
// scrollback pages it. Never a mirror.
static void draw_form_large() {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    int y = lg::header("SSH", s_status[0] ? s_status : nullptr, ACCENT);
    d.fillRect(0, y, W, H - y, C_BG);
    const int nf = form_nfields(), cx = 16, cw = W - 32, rh = nf > 4 ? 24 : 30, gap = 4;
    int ry = y + 8;
    char val[96];
    for (int i = 0; i < nf; i++) {
        const char *lab = form_field(i, val, sizeof val);           // the kit's settings rows
        const bool cyc = val[0] == '<';                                // "< x >": the row's own < > draw it
        const char *v = val;
        if (cyc) { v = val + 2; const int l = (int)strlen(val); if (l > 3) val[l - 2] = 0; }
        const lg::Setting st = { lab, nullptr, v[0] ? v : "-", (unsigned char)(cyc ? lg::ROW_CYCLE : lg::ROW_VALUE), -1, -1, false, false, 0, 0, 0 };
        lg::setting(cx, ry, cw, rh + gap, st, i == s_field, ACCENT);
        nucleo_touch_add(cx, ry, cw, rh, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
        ry += rh + gap;
    }
    const int bh = 42, by = H - bh - 12;
    lg::button(cx, by, cw, bh, s_en ? "Connect" : "Connetti", ACCENT, C_BG, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + nf));
    nucleo_touch_commit();
}

// The terminal on the panel, big: 8x16 cells (s_big), 39 columns = the PTY's width, so remote lines wrap exactly
// where the host wraps them. Drawn straight to the panel WITHOUT a whole-screen clear: every row paints its text
// on its own background and clears only the rest of its line, so typing and output never flash the screen. A drag
// on the scrollback scrolls it one line per row-height of finger travel (SSH_TSCROLL_*, with the fling).
static void draw_shell_large() {
    const int W = 320, H = 240;
    nucleo_touch_clear();
    s_big = true;
    const int charW = 8 * s_zoom, rowH = 16 * s_zoom, cols = (W - 8) / charW;
    const int hdrH = 18, inH = 20, sbTop = hdrH, sbH = H - hdrH - inH, rows = sbH / rowH, sbBot = sbTop + rows * rowH;
    char st[100]; snprintf(st, sizeof st, "%s %s@%s%s", s_scroll ? "^" : "*", s_user, s_host, s_sbcol ? "" : " [mono]");
    st[(W - 8) / 6] = 0;
    s_big = false;                                       // the status line in Font0 x2 height (6x16): more fits
    d.setFont(&fonts::Font0); d.setTextSize(1, 2); d.setTextColor(s_scroll ? C_WARN : ACCENT, C_BG); d.setCursor(4, 1); d.print(st);
    d.fillRect(4 + (int)strlen(st) * 6, 0, W, hdrH, C_BG); d.fillRect(0, 0, 4, H, C_BG);
    s_big = true;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int li = s_count - 1 - s_scroll, yy = sbBot - rowH;
    while (li >= 0 && yy >= sbTop) {
        const char *ln = sb_line(li); const uint8_t *cl = sb_col(li);
        int len = strlen(ln), nseg = len <= 0 ? 1 : (len + cols - 1) / cols;
        for (int seg = nseg - 1; seg >= 0 && yy >= sbTop; seg--) {
            int off = seg * cols, pl = len - off; if (pl > cols) pl = cols; if (pl < 0) pl = 0;
            char piece[SB_W]; memcpy(piece, ln + off, pl); piece[pl] = 0;
            draw_run(4, yy, piece, cl ? cl + off : nullptr, pl, s_zoom);
            d.fillRect(4 + pl * charW, yy, W - 4 - pl * charW, rowH, C_BG);
            yy -= rowH;
        }
        li--;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    if (yy >= sbTop) d.fillRect(4, sbTop, W - 4, yy + rowH - sbTop, C_BG);     // above the oldest line
    if (sbBot < H - inH) d.fillRect(4, sbBot, W - 4, H - inH - sbBot, C_BG);
    const int iy = H - inH, icols = (W - 24) / 8, from = s_cur_x > icols ? s_cur_x - icols : 0;
    char shown[SB_W]; int k = 0; for (int i = from; i < s_linelen && k < icols; i++, k++) shown[k] = s_mask ? '*' : s_line[i]; shown[k] = 0;
    d.fillRect(0, iy, 4, inH, C_INBG);
    draw_text(4, iy + 2, ">", ACCENT, C_INBG, 1); draw_text(16, iy + 2, shown, C_INK, C_INBG, 1);
    d.fillRect(16 + k * 8, iy, W - 16 - k * 8, inH, C_INBG);
    d.fillRect(16 + (s_cur_x - from) * 8, iy + 2, 2, 16, ACCENT);
    s_big = false;
    nucleo_touch_add_scroll(0, sbTop, W, sbH, rowH, NK_CHAR, SSH_TSCROLL_UP, NK_CHAR, SSH_TSCROLL_DOWN);
    nucleo_touch_commit();
}

static void draw_status_large(const char *head, const char *sub, unsigned short col) {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    int y = lg::header("SSH", nullptr, ACCENT);
    d.fillRect(0, y, W, H - y, C_BG);
    lg::message(16, y + 12, W - 32, H - y - 24, head, sub, col);
    nucleo_touch_commit();
}

static void draw_manual_large() {
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    int y = lg::header(s_en ? "SSH - help" : "SSH - guida", nullptr, ACCENT);
    d.fillRect(0, y, W, H - y, C_BG);
    const char *const *m = s_en ? HELP_EN : HELP_IT;
    lg::font(0);
    const int colw = (W - 32) / 2;
    for (int i = 0; i < 12; i++) {                      // two columns of six (the "no editors" note is compact-only)
        const int col = i / 6, row = i % 6;
        d.setTextColor(i >= 9 ? C_DIM : C_INK, C_BG);
        d.drawString(m[i], 16 + col * colw, y + 12 + row * 24);
    }
    nucleo_touch_commit();
}

static void draw_large() {
    if (s_manual) { draw_manual_large(); return; }
    if (s_state == ST_FORM) { draw_form_large(); return; }
    if (s_state == ST_SHELL) { draw_shell_large(); return; }
    if (s_state == ST_CLOSED) {
        draw_status_large(s_status[0] ? s_status : (s_en ? "Disconnected" : "Disconnesso"),
                          s_en ? "press a key for the form" : "premi un tasto per il form", C_WARN);
        return;
    }
    char head[120]; snprintf(head, sizeof head, "SSH %s@%s", s_user, s_host);
    draw_status_large(head, s_status, C_DIM);
}

static void draw_companion() {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, C_BG);
    int y = lg::comp_head("SSH", ACCENT);
    if (s_manual) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(C_INK, C_BG);
        lg::wrap(s_en ? "Quick help on the panel" : "Guida rapida sul pannello", 8, y + 4, 224, 20, 2);
        lg::comp_keys(s_en ? "Fn+H closes it" : "Fn+H la chiude");
        return;
    }
    if (s_state == ST_FORM) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(C_INK, C_BG);
        char ln[64]; snprintf(ln, sizeof ln, "%s@%s", s_user[0] ? s_user : (s_en ? "user" : "utente"), s_host[0] ? s_host : "host");
        lg::wrap(ln, 8, y + 4, 224, 20, 2);
        d.setFont(&fonts::Font0); d.setTextColor(C_DIM, C_BG);
        static const char *FN_IT[] = { "campo: Host", "campo: Utente", "campo: Auth", "campo: Password/Chiave", "campo: Passphrase" };
        static const char *FN_EN[] = { "field: Host", "field: User", "field: Auth", "field: Password/Key", "field: Passphrase" };
        d.drawString((s_en ? FN_EN : FN_IT)[s_field < 5 ? s_field : 4], 8, y + 34);
        char ab[40]; snprintf(ab, sizeof ab, "Auth: %s", s_auth == AUTH_PW ? "Password" : (s_en ? "Key" : "Chiave")); d.drawString(ab, 8, y + 48);
        lg::comp_keys(s_en ? "type fields  Enter next  </>auth" : "scrivi campi  Invio avanti  </>auth");
        return;
    }
    if (s_state == ST_SHELL) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(s_scroll ? C_WARN : ACCENT, C_BG);
        char ln[64]; snprintf(ln, sizeof ln, "%s@%s %s", s_user, s_host, s_scroll ? "[scroll]" : "[live]");
        lg::wrap(ln, 8, y + 2, 224, 18, 2);
        d.setFont(&fonts::Font0); d.setTextColor(C_DIM, C_BG); d.drawString(s_en ? "typing:" : "digiti:", 8, y + 34);
        const int icw = 12, icols = 18, from = s_cur_x > icols ? s_cur_x - icols : 0;
        char shown[24]; int k = 0; for (int i = from; i < s_linelen && k < icols; i++, k++) shown[k] = s_mask ? '*' : s_line[i]; shown[k] = 0;
        d.setTextSize(2); d.setTextColor(C_INK, C_BG); d.setCursor(8, y + 46); d.print(shown[0] ? shown : " ");
        d.fillRect(8 + (int)strlen(shown) * icw, y + 44, 3, 18, ACCENT); d.setTextSize(1);
        lg::comp_keys(s_en ? "Enter send  Tab compl  Fn+Up/Dn scroll  Fn+Esc" : "Invio invia  Tab compl  Fn+Su/Giu scorri  Fn+Esc");
        return;
    }
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(C_WARN, C_BG);
    lg::wrap(s_status[0] ? s_status : "...", 8, y + 4, 224, 20, 2);
    lg::comp_keys(s_en ? "ESC cancels" : "ESC annulla");
}

static void on_draw() {
    s_dirty = false;
    if (nucleo_app_two_screens()) { draw_companion(); return; }
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
// The framework never hands TAB to on_key: unclaimed, it toggles the Control Center. Claim it
// (next field in the connect form, completion in the shell).
static void ssh_tab() { on_key(NK_TAB, 0); }
static void on_enter() {
    s_en = !nucleo_i18n_is_it();   // follow the OS language (English floor for es/fr/de)
    s_state = ST_FORM; s_field = 0; s_status[0] = 0; s_dirty = true; s_zoom = 1; s_manual = false;
    s_auth = AUTH_PW; s_keyIdx = 0;
    s_linelen = s_cur_x = 0; s_curlen = 0; s_compIdx = -1;   // their APP_RAM buffers come back zeroed
    // The terminal paints straight to the panel (every draw_* uses d.*), so the 32 KB shared canvas
    // is dead weight: release it AND pin direct draw so the run loop won't composite an empty canvas
    // over us — handing that contiguous block to libssh2/mbedTLS (the start_connect 34 KB gate now
    // sees it). close_app() re-acquires the canvas and clears the flag on exit. Mirrors app_anima.
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);
    scan_keys(); hist_load();
    nucleo_app_set_hint(s_en ? "arrows/Tab  </>auth/key  Enter go  Fn+H" : "frecce/Tab  </>auth  invio vai  Fn+H");
    nucleo_app_set_back_handler(back_handler);
    nucleo_app_set_tab_handler(ssh_tab);
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

// Per-app working RAM: allocated (zeroed) before on_enter, freed after on_exit. All of it is UI-only
// or touched by ssh_task, which on_exit stops/deletes before returning. s_host/s_user stay static on
// purpose: the form keeps the last target across sessions.
static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_hist,       HIST_MAX * SB_W },
    { (void **)&s_cand,       sizeof(const char *) * CAND_MAX },
    { (void **)&s_keys,       MAX_KEYS * KEY_W },
    { (void **)&s_cur,        SB_W },
    { (void **)&s_curcol,     SB_W },
    { (void **)&s_line,       SB_W },
    { (void **)&s_compPrefix, SB_W },
    { (void **)&s_status,     STATUS_W },
    { nullptr, 0 } };

extern "C" void nucleo_register_ssh(void) {
    static const nucleo_app_def_t app = {
        "ssh", "SSH", "Connect", "Terminale SSH nativo (modalita dedicata)",
        's', 0x2EE0, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP,  // declarative: dedicated RAM for the whole SSH session; framework restores on close
        APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
