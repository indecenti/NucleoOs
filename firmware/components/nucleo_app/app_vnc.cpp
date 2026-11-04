// app_vnc.cpp — VNC: the Cardputer as a remote desktop for a PC on the same network (RFB 3.3 / 3.8, RFC 6143).
//
// Written from the RFC, no code taken from other VNC clients. A store app only (permission "net"): the firmware
// carries none of it, just the nucleo_tcp it connects through.
//
//   • Network: only through nucleo_tcp (the system's TCP client: no listening, no scans, loopback and the device
//     refused). VNC is not encrypted: the UI says so; it is meant for the home network.
//   • Every byte from the server is untrusted: sizes, rectangles and lengths are checked against the framebuffer
//     and the buffers before anything is read into them, every read has a deadline and so does a whole update.
//   • Password (VNC authentication): DES (vnc_des.c). It is kept with nucleo_secret (internal flash, this app
//     only), never on the SD card; wiped from RAM after use.
//   • Saved PCs: /sd/data/vnc/connections.json (name, address, port, zoom, connect on open) - nothing secret.
//     Made with the form on the panel. /sd/data/vnc/vnc.json from the prototype is imported once (its "bench"
//     > 0 walks the desktop for that many seconds, writes /sd/data/vnc/metrics.json and closes).
//   • Pixels: the server sends RGB565 big-endian (SetPixelFormat), the panel's own byte order, in strips of 16
//     rows. No framebuffer. Hextile, Raw, DesktopSize. The panel's bus stays open across an update.
//   • Two screens: the external 320x240 panel IS the desktop; the Cardputer is the control panel (a map of the
//     desktop, speed, modifiers held, the menu with Fn+Space). Without the panel the Cardputer is the window.
//   • Touch (raw pointer): tap = left click, hold = right click, hold then drag = drag with the button down,
//     drag = move the window over the desktop, a drag along the right edge = the wheel.
//   • Keyboard: the Cardputer has no F-keys, arrows, Esc or Delete of its own: they live on Fn (the legend is in
//     the menu). Ctrl / Alt / Opt held make chords. Esc alone is the PC's Esc; Fn+Esc leaves the app.
#include "sdkconfig.h"
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)   // store-only: compiled as a module, never into the firmware

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "nucleo_board.h"
#include "nucleo_surface.h"
#include "nucleo_extdisp.h"
#include "nucleo_touch.h"
#include "nucleo_tcp.h"
#include "nucleo_secret.h"
#include "nucleo_i18n.h"
#include "nucleo_theme.h"
#include "app_gfx.h"
#include "app_large.h"
#include "app_ui.h"
#include "vnc_des.h"
#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <new>

[[maybe_unused]] static const char *TAG = "app.vnc";

#define VNC_DIR      NUCLEO_SD_MOUNT "/data/vnc"
#define VNC_CFG      VNC_DIR "/vnc.json"
#define VNC_METRICS  VNC_DIR "/metrics.json"
#define VNC_CONNS    VNC_DIR "/connections.json"
#define VNC_SHOTS    VNC_DIR "/shots"
static const unsigned short ACCENT = 0x05FF;
#define C_BG   THEME_BG
#define C_INK  THEME_FG
#define C_DIM  THEME_MUTED
#define C_WARN 0xFD20
#define C_GOOD 0x07E8
#define C_BAD  ((unsigned short)0xC104)

// Every text in five languages (ASCII: the device fonts have no accents).
#define L(it, en, es, fr, de) TR5(it, en, es, fr, de)

// Limits on what a server may announce (a desktop is never bigger than this; anything else is an attack or a bug).
#define FB_MAX     8192
#define NAME_MAX_B 2048
#define CUT_MAX    65536
#define READ_MS    8000          // one read
#define UPDATE_MS  20000         // one whole update

static char s_host[64] = "";
static int  s_port = 5900;
static int  s_bench = 0;           // seconds; 0 = interactive
static int  s_want_scale = 2;      // asked of a NucleoOS server at connect (1, 2, 4)
static bool s_auto = false, s_auto_done = false;
static bool s_test_setup = false;  // a /sd/data/vnc/vnc.json is there: the bench card (automatic screenshots)
static int  s_retries = 0;         // bench card: sessions to run back to back (the hostile-server test), each logged
static char s_status[96] = "";

// ── metrics (one session; no address in them) ──
struct Metrics {
    int64_t t_start, t_connected, t_first_frame;
    uint32_t bytes, updates, rects, raw_rects, hex_rects, tiles, raw_tiles, keys, clicks, pans, pushes;
    int64_t net_us, draw_us, upd_us_sum, upd_us_max, push_us_max;
    int64_t panel_fill_us, panel_push_us, panel_push_held_us;     // the panel self-test (one full 320x240 each)
    size_t free_before, free_min, largest_min, free_now, largest_now;
    uint32_t stack_min;
    uint32_t sec_bytes_max;
    int fb_w, fb_h, view_w, view_h;
    char err[64];
};
static Metrics M;

static void heap_sample()
{
    M.free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    M.largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (!M.free_min || M.free_now < M.free_min) M.free_min = M.free_now;
    if (!M.largest_min || M.largest_now < M.largest_min) M.largest_min = M.largest_now;
#ifndef NAP_MODULE                                      // a task probe the store's ABI does not give a module
    const uint32_t st = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (!M.stack_min || st < M.stack_min) M.stack_min = st;
#endif
}

// Integers only: a store module gets no double-precision helpers. ms values in tenths where they are short.
static long long ms10(int64_t us) { return (long long)(us / 100); }
static void metrics_write(const char *state)
{
    mkdir(VNC_DIR, 0777);
    FILE *f = fopen(VNC_METRICS, "w");
    if (!f) return;
    const int64_t us = esp_timer_get_time() - (M.t_connected ? M.t_connected : M.t_start);
    const long long secs10 = us / 100000 > 0 ? us / 100000 : 1;
    fprintf(f, "{\"state\":\"%s\",\"fb\":\"%dx%d\",\"view\":\"%dx%d\",\n", state, M.fb_w, M.fb_h, M.view_w, M.view_h);
    fprintf(f, " \"connect_ms\":%lld,\"first_frame_ms\":%lld,\"seconds_x10\":%lld,\n",
            M.t_connected ? (long long)((M.t_connected - M.t_start) / 1000) : -1LL,
            M.t_first_frame ? (long long)((M.t_first_frame - M.t_start) / 1000) : -1LL, secs10);
    fprintf(f, " \"bytes\":%lu,\"kb_per_s\":%lld,\"kb_per_s_peak\":%lu,\"updates\":%lu,\"updates_per_s_x10\":%lld,\n",
            (unsigned long)M.bytes, (long long)M.bytes * 10 / 1024 / secs10, (unsigned long)(M.sec_bytes_max / 1024),
            (unsigned long)M.updates, (long long)M.updates * 100 / secs10);
    fprintf(f, " \"update_ms_x10_avg\":%lld,\"update_ms_x10_max\":%lld,\"net_ms\":%lld,\"draw_ms\":%lld,\"pushes\":%lu,\"push_ms_x10_max\":%lld,\n",
            M.updates ? ms10(M.upd_us_sum) / (long long)M.updates : 0LL, ms10(M.upd_us_max),
            (long long)(M.net_us / 1000), (long long)(M.draw_us / 1000), (unsigned long)M.pushes, ms10(M.push_us_max));
    fprintf(f, " \"panel_fill_ms_x10\":%lld,\"panel_push_ms_x10\":%lld,\"panel_push_held_ms_x10\":%lld,\n",
            ms10(M.panel_fill_us), ms10(M.panel_push_us), ms10(M.panel_push_held_us));
    fprintf(f, " \"rects\":%lu,\"raw_rects\":%lu,\"hextile_rects\":%lu,\"tiles\":%lu,\"raw_tiles\":%lu,\"keys\":%lu,\"clicks\":%lu,\"pans\":%lu,\n",
            (unsigned long)M.rects, (unsigned long)M.raw_rects, (unsigned long)M.hex_rects, (unsigned long)M.tiles,
            (unsigned long)M.raw_tiles, (unsigned long)M.keys, (unsigned long)M.clicks, (unsigned long)M.pans);
    fprintf(f, " \"heap_free_before\":%u,\"heap_free_min\":%u,\"heap_largest_min\":%u,\"heap_free_now\":%u,\"heap_largest_now\":%u,\"stack_left_min\":%lu,\n",
            (unsigned)M.free_before, (unsigned)M.free_min, (unsigned)M.largest_min, (unsigned)M.free_now, (unsigned)M.largest_now,
            (unsigned long)M.stack_min);
    fprintf(f, " \"error\":\"%s\"}\n", M.err);
    fclose(f);
}

// The prototype's single /sd/data/vnc/vnc.json: imported once as the first saved PC; "bench" stays a test switch.
static void load_config()
{
    FILE *f = fopen(VNC_CFG, "r");
    if (!f) return;
    char buf[512]; const size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) return;
    s_test_setup = true;
    const cJSON *h = cJSON_GetObjectItem(j, "host");
    if (cJSON_IsString(h)) snprintf(s_host, sizeof s_host, "%s", h->valuestring);
    const cJSON *p = cJSON_GetObjectItem(j, "port");      if (cJSON_IsNumber(p)) s_port = p->valueint;
    const cJSON *b = cJSON_GetObjectItem(j, "bench");     s_bench = cJSON_IsNumber(b) ? b->valueint : 0;
    const cJSON *z = cJSON_GetObjectItem(j, "scale");     if (cJSON_IsNumber(z)) s_want_scale = z->valueint;
    const cJSON *a = cJSON_GetObjectItem(j, "auto");      s_auto = cJSON_IsTrue(a);
    const cJSON *r = cJSON_GetObjectItem(j, "retries");   s_retries = cJSON_IsNumber(r) ? r->valueint : 0;
    cJSON_Delete(j);
}

// ── the connection: one nucleo_tcp handle, a small read buffer, every read bounded in time ──
#define RBUF 4096
#define STRIP_ROWS 16
struct Conn { int h; uint8_t *rb; int rpos, rlen; bool fail; int64_t update_until; };
static Conn C;

static void fail(const char *why) { if (!C.fail) { C.fail = true; snprintf(M.err, sizeof M.err, "%s", why); } }

// While an update is drawn the panel's bus stays open (s_holding): opening it costs up to 40 ms each time, since it
// is shared with the card and the touch. It is handed back whenever the data has to be waited for, so the touch
// keeps sampling and the card keeps working.
static bool s_holding = false;
static void hold_bus(bool on);
static bool rd_fill(int timeout_ms)
{
    const int64_t t0 = esp_timer_get_time();
    bool gave_back = false;
    for (;;) {
        const int n = nucleo_tcp_recv(C.h, C.rb, RBUF, 100);
        if (n > 0) {
            C.rpos = 0; C.rlen = n; M.bytes += n; M.net_us += esp_timer_get_time() - t0;
            if (gave_back) hold_bus(true);
            return true;
        }
        if (n == NUCLEO_TCP_CLOSED) { fail(L("il PC ha chiuso", "the PC closed it", "el PC cerro", "le PC a ferme", "der PC hat beendet")); return false; }
        if (n != NUCLEO_TCP_TIMEOUT) { fail(L("connessione persa", "connection lost", "conexion perdida", "connexion perdue", "Verbindung weg")); return false; }
        if (s_holding && !gave_back) { hold_bus(false); gave_back = true; }
        const int64_t now = esp_timer_get_time();
        if ((now - t0) / 1000 > timeout_ms || (C.update_until && now > C.update_until)) {
            fail(L("il PC non risponde", "the PC does not answer", "el PC no responde", "le PC ne repond pas", "der PC antwortet nicht"));
            return false;
        }
    }
}
static bool rd(void *dst, int n)
{
    uint8_t *o = (uint8_t *)dst;
    while (n > 0 && !C.fail) {
        if (C.rpos >= C.rlen && !rd_fill(READ_MS)) return false;
        int k = C.rlen - C.rpos; if (k > n) k = n;
        if (o) { memcpy(o, C.rb + C.rpos, k); o += k; }
        C.rpos += k; n -= k;
    }
    return !C.fail;
}
static bool skip(uint32_t n)
{
    while (n > 0 && !C.fail) { const int k = n > 0x10000 ? 0x10000 : (int)n; if (!rd(nullptr, k)) return false; n -= (uint32_t)k; }
    return !C.fail;
}
static uint8_t  u8()  { uint8_t v = 0; rd(&v, 1); return v; }
static uint16_t u16() { uint8_t b[2] = {0, 0}; rd(b, 2); return (uint16_t)(b[0] << 8 | b[1]); }
static uint32_t u32() { uint8_t b[4] = {0, 0, 0, 0}; rd(b, 4); return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static bool wr(const void *p, int n)
{
    if (C.fail) return false;
    if (nucleo_tcp_send(C.h, p, n, 5000) != n) { fail(L("invio non riuscito", "send failed", "envio fallido", "envoi impossible", "Senden fehlgeschlagen")); return false; }
    return true;
}
static bool data_waiting(int ms) { return C.rpos < C.rlen || nucleo_tcp_readable(C.h, ms); }

// ── RFB ──
static int s_fbw = 0, s_fbh = 0;
static int s_vx = 0, s_vy = 0, s_vw = 240, s_vh = 135;   // the window on the desktop, in desktop pixels
// One screen only (an original Cardputer, or an ADV without the panel): the desktop and the controls share the
// 240x135 glass. The zoom is then the Cardputer's own (1 screen pixel = s_cz desktop pixels), the menu and the
// legend cover the desktop while they are open, and the keyboard can drive the pointer.
static bool s_single = false;
static int  s_cz = 1;
static int  s_scr_w = 240, s_scr_h = 135;   // the glass the session draws on
static int  s_strip_w = 240;                 // pixels per strip row (allocated once per session)
static bool s_overlay = false;               // single screen: a control page covers the desktop, updates are not drawn
static int s_scale = 1;                  // what the server reported last (1 = unknown or 1:1)
static bool s_nucleo_srv = false;        // the NucleoOS test server: it takes the zoom from the client
static bool s_hold_draw = false;         // the panel self-test said: keep the bus open across an update
static uint16_t *s_strip = nullptr;      // STRIP_ROWS rows of s_strip_w pixels, panel byte order
static uint16_t s_tile[16 * 16];         // one Hextile tile when a rectangle is wider than the strip
static bool s_full = true;               // the next request asks for the whole view (not incremental)
static int  s_pending = 0;               // requests the server still owes us
static char s_pw[NUCLEO_SECRET_MAX];     // the password, only between the form / the secret store and the login
static bool s_auth_vnc = false;          // this session answered a VNC password challenge
static bool s_need_pw = false;           // it ended because the PC wants a (different) password: ask for it

static bool des_ok()
{
    static const uint8_t k[8] = { 0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1 }, p[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    static const uint8_t want[8] = { 0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05 };   // FIPS 46 worked example
    uint8_t o[8]; vnc_des_block(k, p, o);
    return !memcmp(o, want, 8);
}

static bool vnc_auth()
{
    if (!s_pw[0]) { s_need_pw = true; fail(L("il PC chiede la password", "the PC wants a password", "el PC pide contrasena", "le PC veut un mot de passe", "der PC will ein Passwort")); return false; }
    if (!des_ok()) { fail("DES self-test"); return false; }
    uint8_t ch[16];
    if (!rd(ch, 16)) return false;
    vnc_des_response(s_pw, ch);
    memset(s_pw, 0, sizeof s_pw);
    s_auth_vnc = true;
    return wr(ch, 16);
}

static bool security_result(bool v38)
{
    const uint32_t r = u32();
    if (C.fail) return false;
    if (r == 0) return true;
    if (v38) {                                          // 3.8: a reason follows
        char why[48] = {0};
        const uint32_t n = u32();
        if (n > 1024) { fail(L("risposta non valida", "invalid answer", "respuesta no valida", "reponse invalide", "ungueltige Antwort")); return false; }
        const int keep = n < sizeof why - 1 ? (int)n : (int)sizeof why - 1;
        rd(why, keep); skip(n - (uint32_t)keep);
    }
    if (s_auth_vnc) s_need_pw = true;
    fail(L("password sbagliata", "wrong password", "contrasena incorrecta", "mot de passe faux", "falsches Passwort"));
    return false;
}

static bool rfb_handshake()
{
    char ver[13] = {0};
    if (!rd(ver, 12) || memcmp(ver, "RFB 003.", 8) != 0 || ver[11] != '\n') { fail(L("non e un server VNC", "not a VNC server", "no es un servidor VNC", "pas un serveur VNC", "kein VNC-Server")); return false; }
    const int minor = atoi(ver + 8);
    const bool v38 = minor >= 7;
    wr(v38 ? "RFB 003.008\n" : "RFB 003.003\n", 12);
    if (v38) {
        const int n = u8();
        if (C.fail) return false;
        if (n == 0) {
            const uint32_t len = u32();
            if (len <= 1024) skip(len);
            fail(L("il PC ha rifiutato", "the PC refused", "el PC rechazo", "le PC a refuse", "der PC lehnt ab"));
            return false;
        }
        bool none = false, vnc = false;
        for (int i = 0; i < n; i++) { const uint8_t t = u8(); none |= t == 1; vnc |= t == 2; }
        if (C.fail) return false;
        if (none && !s_pw[0]) {
            const uint8_t one = 1; wr(&one, 1);
            if (minor >= 8 && !security_result(true)) return false;
        } else if (vnc) {
            const uint8_t two = 2; wr(&two, 1);
            if (!vnc_auth() || !security_result(true)) return false;
        } else if (none) {
            const uint8_t one = 1; wr(&one, 1);
            if (minor >= 8 && !security_result(true)) return false;
        } else { fail(L("sicurezza non supportata", "security not supported", "seguridad no soportada", "securite non geree", "Sicherheit nicht unterstuetzt")); return false; }
    } else {
        const uint32_t t = u32();
        if (t == 2) { if (!vnc_auth() || !security_result(false)) return false; }
        else if (t != 1) { fail(L("sicurezza non supportata", "security not supported", "seguridad no soportada", "securite non geree", "Sicherheit nicht unterstuetzt")); return false; }
    }
    memset(s_pw, 0, sizeof s_pw);
    const uint8_t shared = 1; wr(&shared, 1);
    s_fbw = u16(); s_fbh = u16();
    skip(16);                                           // the server's pixel format: replaced below
    const uint32_t nl = u32();
    if (C.fail) return false;
    if (s_fbw <= 0 || s_fbh <= 0 || s_fbw > FB_MAX || s_fbh > FB_MAX || nl > NAME_MAX_B) {
        fail(L("risposta non valida", "invalid answer", "respuesta no valida", "reponse invalide", "ungueltige Antwort")); return false;
    }
    char name[24] = {0};
    const int keep = nl < sizeof name - 1 ? (int)nl : (int)sizeof name - 1;
    rd(name, keep); skip(nl - (uint32_t)keep);
    if (C.fail) return false;
    s_nucleo_srv = !strncmp(name, "NucleoOS", 8);
    // RGB565, big endian: the panel's own byte order, pushed as received
    const uint8_t pf[20] = { 0, 0, 0, 0,  16, 16, 1, 1,  0, 31, 0, 63, 0, 31,  11, 5, 0,  0, 0, 0 };
    wr(pf, sizeof pf);
    const uint8_t enc[4 + 12] = { 2, 0, 0, 3,  0, 0, 0, 5,  0, 0, 0, 0,  0xff, 0xff, 0xff, 0x21 };   // Hextile, Raw, DesktopSize
    wr(enc, sizeof enc);
    M.fb_w = s_fbw; M.fb_h = s_fbh;
    return !C.fail;
}

static void clamp_view()
{
    if (s_vx > s_fbw - s_vw) s_vx = s_fbw - s_vw;
    if (s_vy > s_fbh - s_vh) s_vy = s_fbh - s_vh;
    if (s_vx < 0) s_vx = 0;
    if (s_vy < 0) s_vy = 0;
}

static void request(bool incremental)
{
    const int w = s_vw < s_fbw ? s_vw : s_fbw, h = s_vh < s_fbh ? s_vh : s_fbh;
    const uint8_t m[10] = { 3, (uint8_t)(incremental ? 1 : 0), (uint8_t)(s_vx >> 8), (uint8_t)s_vx, (uint8_t)(s_vy >> 8), (uint8_t)s_vy,
                            (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h };
    wr(m, sizeof m);
}

// One area again, whole (not incremental): what a toast or the drawn pointer covered on the single screen.
static void request_area(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_fbw) w = s_fbw - x;
    if (y + h > s_fbh) h = s_fbh - y;
    if (w <= 0 || h <= 0) return;
    const uint8_t m[10] = { 3, 0, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h };
    wr(m, sizeof m);
}

// The zoom is the server's: ask the NucleoOS test server for 1:n of the real screen (a real VNC server keeps its size).
static void ask_scale(int n)
{
    if (!s_nucleo_srv) return;
    char t[20]; const int len = snprintf(t, sizeof t, "nucleo-scale:%d", n);
    const uint8_t h[8] = { 6, 0, 0, 0, 0, 0, 0, (uint8_t)len };
    wr(h, sizeof h); wr(t, len);
    s_want_scale = n;
}

static bool s_bus_open = false;
static void hold_bus(bool on)
{
    if (on && !s_bus_open) { d.startWrite(); s_bus_open = true; }
    else if (!on && s_bus_open) { d.endWrite(); s_bus_open = false; }
}
static void push(int x, int y, int w, int h, uint16_t *px)
{
    if (s_overlay) return;                               // the menu covers the desktop: a full view follows when it closes
    const int64_t t0 = esp_timer_get_time();
    if (s_holding) hold_bus(true);
    if (s_cz > 1) {
        // Nearest pixel on the view's grid, packed in place (a written pixel never lies after one still to read).
        const int k = s_cz;
        const int c0 = ((s_vx - x) % k + k) % k, r0 = ((s_vy - y) % k + k) % k;
        if (c0 >= w || r0 >= h) return;
        const int ow = (w - c0 + k - 1) / k, oh = (h - r0 + k - 1) / k;
        uint16_t *o = px;
        for (int r = r0; r < h; r += k) { const uint16_t *src = px + r * w; for (int c = c0; c < w; c += k) *o++ = src[c]; }
        d.pushImage((x + c0 - s_vx) / k, (y + r0 - s_vy) / k, ow, oh, px);
    } else d.pushImage(x - s_vx, y - s_vy, w, h, px);
    const int64_t dt = esp_timer_get_time() - t0;
    M.draw_us += dt; M.pushes++; if (dt > M.push_us_max) M.push_us_max = dt;
}

static bool rect_raw(int x, int y, int w, int h)
{
    M.raw_rects++;
    const int cap = s_strip_w * STRIP_ROWS;             // pixels the strip holds
    if (w > cap) return skip((uint32_t)w * (uint32_t)h * 2u);   // wider than the strip (never for a viewport request)
    int rows = cap / w; if (rows > h) rows = h;
    for (int r = 0; r < h; r += rows) {
        const int n = r + rows > h ? h - r : rows;
        if (!rd(s_strip, w * n * 2)) return false;
        push(x, y + r, w, n, s_strip);
    }
    return true;
}

static void fill(uint16_t *base, int stride, int x, int y, int w, int h, uint16_t c)
{
    for (int j = 0; j < h; j++) { uint16_t *p = base + (y + j) * stride + x; for (int i = 0; i < w; i++) p[i] = c; }
}

static bool rect_hextile(int x, int y, int w, int h)
{
    M.hex_rects++;
    uint16_t bg = 0, fg = 0;
    const bool strip_ok = w <= s_strip_w;               // a whole tile row fits the strip: one push per 16 rows
    for (int ty = 0; ty < h; ty += 16) {
        const int th = ty + 16 > h ? h - ty : 16;
        for (int tx = 0; tx < w; tx += 16) {
            const int tw = tx + 16 > w ? w - tx : 16;
            uint16_t *dst = strip_ok ? s_strip : s_tile;
            const int stride = strip_ok ? w : tw, ox = strip_ok ? tx : 0;
            M.tiles++;
            const uint8_t se = u8();
            if (C.fail) return false;
            if (se & 1) {                                // raw tile
                M.raw_tiles++;
                for (int j = 0; j < th; j++) if (!rd(dst + j * stride + ox, tw * 2)) return false;
            } else {
                if (se & 2) rd(&bg, 2);                  // colours stay in wire (panel) byte order
                fill(dst, stride, ox, 0, tw, th, bg);
                if (se & 4) rd(&fg, 2);
                if (se & 8) {
                    const int n = u8();
                    for (int k = 0; k < n; k++) {
                        uint16_t c = fg;
                        if (se & 16) rd(&c, 2);
                        const uint8_t xy = u8(), wh = u8();
                        // A subrectangle that pokes out of its tile is cut to it: never a write past the tile.
                        int sx = xy >> 4, sy = xy & 15, sw = (wh >> 4) + 1, sh = (wh & 15) + 1;
                        if (sx >= tw || sy >= th) continue;
                        if (sx + sw > tw) sw = tw - sx;
                        if (sy + sh > th) sh = th - sy;
                        fill(dst, stride, ox + sx, sy, sw, sh, c);
                    }
                }
                if (C.fail) return false;
            }
            if (!strip_ok) push(x + tx, y + ty, tw, th, s_tile);
        }
        if (strip_ok) push(x, y + ty, w, th, s_strip);
    }
    return true;
}

static void on_new_size(int w, int h)
{
    // Keep the point under the middle of the window where it was (the zoom changed around it).
    const int cx = s_vx + s_vw / 2, cy = s_vy + s_vh / 2;
    const int ow = s_fbw > 0 ? s_fbw : w, oh = s_fbh > 0 ? s_fbh : h;
    s_fbw = w; s_fbh = h; M.fb_w = w; M.fb_h = h;
    s_vx = (int)((int64_t)cx * w / ow) - s_vw / 2;
    s_vy = (int)((int64_t)cy * h / oh) - s_vh / 2;
    clamp_view();
    s_scale = s_want_scale;
    d.fillScreen(0);
    s_full = true;
}

static bool handle_message(int64_t *t_req)
{
    const uint8_t t = u8();
    if (C.fail) return false;
    if (t == 0) {                                        // FramebufferUpdate
        C.update_until = esp_timer_get_time() + (int64_t)UPDATE_MS * 1000;   // a server trickling bytes cannot hold the UI
        skip(1);
        const int n = u16();
        // Pipelining: ask for the next one now, so the server prepares it while this one is drawn.
        if (s_pending > 0) s_pending--;
        if (s_pending == 0 && !s_full) { request(true); s_pending = 1; }
        s_holding = s_hold_draw;
        for (int i = 0; i < n && !C.fail; i++) {
            const int x = u16(), y = u16(), w = u16(), h = u16();
            const int32_t enc = (int32_t)u32();
            if (C.fail) break;
            M.rects++;
            if (enc == -223) {                           // DesktopSize: the whole desktop changed size
                if (w <= 0 || h <= 0 || w > FB_MAX || h > FB_MAX) { fail(L("risposta non valida", "invalid answer", "respuesta no valida", "reponse invalide", "ungueltige Antwort")); break; }
                on_new_size(w, h);
                continue;
            }
            // Every rectangle must lie inside the desktop the server announced.
            if (w <= 0 || h <= 0 || (int64_t)x + w > s_fbw || (int64_t)y + h > s_fbh) {
                if (w == 0 || h == 0) continue;          // empty: nothing follows it
                fail(L("risposta non valida", "invalid answer", "respuesta no valida", "reponse invalide", "ungueltige Antwort")); break;
            }
            if (enc == 0) rect_raw(x, y, w, h);
            else if (enc == 5) rect_hextile(x, y, w, h);
            else { fail(L("codifica sconosciuta", "unknown encoding", "codificacion desconocida", "encodage inconnu", "unbekannte Kodierung")); break; }
        }
        s_holding = false; hold_bus(false);
        C.update_until = 0;
        const int64_t dt = esp_timer_get_time() - *t_req;
        M.updates++; M.upd_us_sum += dt; if (dt > M.upd_us_max) M.upd_us_max = dt;
        if (!M.t_first_frame && n) M.t_first_frame = esp_timer_get_time();
        *t_req = esp_timer_get_time();
        return !C.fail;
    }
    if (t == 1) { skip(1); u16(); const int n = u16(); return skip((uint32_t)n * 6u); }   // colour map: unused at 16 bpp
    if (t == 2) return true;                             // bell
    if (t == 3) {                                        // server cut text: not used, bounded
        skip(3); const uint32_t n = u32();
        if (n > CUT_MAX) { fail(L("risposta non valida", "invalid answer", "respuesta no valida", "reponse invalide", "ungueltige Antwort")); return false; }
        return skip(n);
    }
    fail(L("messaggio sconosciuto", "unknown message", "mensaje desconocido", "message inconnu", "unbekannte Nachricht"));
    return false;
}

// ── input to the PC ──
static void key_event(uint32_t sym, bool down)
{
    const uint8_t m[8] = { 4, (uint8_t)(down ? 1 : 0), 0, 0, (uint8_t)(sym >> 24), (uint8_t)(sym >> 16), (uint8_t)(sym >> 8), (uint8_t)sym };
    wr(m, sizeof m);
}
static void key_tap(uint32_t sym, unsigned char mods)
{
    static const struct { unsigned char bit; uint32_t sym; } MOD[] = {
        { NK_MOD_CTRL, 0xffe3 }, { NK_MOD_ALT, 0xffe9 }, { NK_MOD_GUI, 0xffeb }, { NK_MOD_SHIFT, 0xffe1 } };
    const int n = (int)(sizeof MOD / sizeof MOD[0]);
    for (int i = 0; i < n; i++) if (mods & MOD[i].bit) key_event(MOD[i].sym, true);
    key_event(sym, true); key_event(sym, false);
    for (int i = n - 1; i >= 0; i--) if (mods & MOD[i].bit) key_event(MOD[i].sym, false);
    M.keys++;
}
static int s_px = -1, s_py = -1;                        // where the PC's pointer is (desktop coordinates)
static void pointer(int x, int y, uint8_t mask)
{
    if (x > s_fbw - 1) x = s_fbw - 1;
    if (y > s_fbh - 1) y = s_fbh - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    s_px = x; s_py = y;
    const uint8_t m[6] = { 5, mask, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8), (uint8_t)y };
    wr(m, sizeof m);
}
// A shortcut on the PC: the modifiers down, the key, all up.
static void combo(uint32_t a, uint32_t b, uint32_t c)
{
    key_event(a, true);
    if (b) key_event(b, true);
    if (c) { key_event(c, true); key_event(c, false); }
    if (b) key_event(b, false);
    key_event(a, false);
}

// The Fn layer: the keys the Cardputer does not have. Fn + the key -> keysym. Shown in the menu's legend.
struct FnKey { char ch; uint32_t sym; };
static const FnKey FN_KEYS[] = {
    { '1', 0xffbe }, { '2', 0xffbf }, { '3', 0xffc0 }, { '4', 0xffc1 }, { '5', 0xffc2 }, { '6', 0xffc3 },
    { '7', 0xffc4 }, { '8', 0xffc5 }, { '9', 0xffc6 }, { '0', 0xffc7 }, { '-', 0xffc8 }, { '=', 0xffc9 },   // F1..F12
    { '[', 0xff55 }, { ']', 0xff56 },                                                                       // Page Up / Down
    { 'h', 0xff50 }, { 'e', 0xff57 },                                                                       // Home / End
    { 'i', 0xff63 }, { 'p', 0xff61 }, { 'm', 0xff67 },                                                      // Insert, Print, Menu
    { 'w', 0xffeb },                                                                                        // the Windows key
};

// ── touch: the app's own gestures on the raw pointer ──
enum { G_IDLE, G_PRESS, G_HOLD, G_DRAG, G_PAN, G_WHEEL };
struct Gesture { int mode; int x0, y0, x, y, wacc; int64_t t0; };
static Gesture G;
#define TAP_SLOP   10          // px a tap may wander
#define HOLD_US    450000      // still this long = a hold (right click, or a drag with the button down)
#define EDGE_W     22          // the wheel strip along the right edge
#define WHEEL_STEP 18          // px of finger travel per wheel notch

static void touch_event(const nucleo_touch_raw_t &e, int64_t now)
{
    const int dx = e.x - G.x0, dy = e.y - G.y0;
    const bool moved = dx > TAP_SLOP || dx < -TAP_SLOP || dy > TAP_SLOP || dy < -TAP_SLOP;
    if (e.down && G.mode == G_IDLE) {
        G = Gesture{ e.x >= s_vw - EDGE_W ? G_WHEEL : G_PRESS, e.x, e.y, e.x, e.y, 0, now };
        if (G.mode == G_PRESS) pointer(s_vx + e.x, s_vy + e.y, 0);   // the pointer follows the finger first
        return;
    }
    if (e.down) {
        const int stepy = e.y - G.y;
        G.x = e.x; G.y = e.y;
        switch (G.mode) {
            case G_PRESS: if (moved) G.mode = G_PAN; break;
            case G_HOLD:
                if (moved) { G.mode = G_DRAG; pointer(s_vx + G.x0, s_vy + G.y0, 1); pointer(s_vx + e.x, s_vy + e.y, 1); }
                break;
            case G_DRAG: pointer(s_vx + e.x, s_vy + e.y, 1); break;
            case G_WHEEL:
                G.wacc += stepy;
                while (G.wacc <= -WHEEL_STEP) { G.wacc += WHEEL_STEP; pointer(s_px, s_py, 16); pointer(s_px, s_py, 0); }   // finger up = content up
                while (G.wacc >=  WHEEL_STEP) { G.wacc -= WHEEL_STEP; pointer(s_px, s_py, 8);  pointer(s_px, s_py, 0); }
                break;
            default: break;
        }
        return;
    }
    switch (G.mode) {                                     // the lift
        case G_PRESS: pointer(s_vx + G.x0, s_vy + G.y0, 1); pointer(s_vx + G.x0, s_vy + G.y0, 0); M.clicks++; break;
        case G_HOLD:  pointer(s_vx + G.x0, s_vy + G.y0, 4); pointer(s_vx + G.x0, s_vy + G.y0, 0); M.clicks++; break;
        case G_DRAG:  pointer(s_vx + e.x, s_vy + e.y, 0); break;
        case G_PAN:
            s_vx -= dx; s_vy -= dy;                       // the desktop follows the finger
            clamp_view(); s_full = true; M.pans++;
            break;
        default: break;
    }
    G.mode = G_IDLE;
}

// ── saved connections: /sd/data/vnc/connections.json (on the card only; passwords are NOT in it) ──
#define MAX_SAVED  8
struct Saved { char name[24]; char host[64]; int port; int scale; bool autoc; uint32_t uid; };
static Saved *s_saved = nullptr;          // MAX_SAVED, APP_RAM
static int s_nsaved = 0, s_sel = 0, s_scroll = 0;

static void pw_key(uint32_t uid, char out[16]) { snprintf(out, 16, "pw%08lx", (unsigned long)uid); }

// A JSON string: quotes and backslashes escaped, control characters dropped (names are typed on the keyboard).
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c >= 0x20 && c < 0x7f) fputc(c, f);
    }
    fputc('"', f);
}

// Written by hand, integers only (cJSON's builders would pull double arithmetic into a module); read with cJSON.
static void conns_save()
{
    mkdir(VNC_DIR, 0777);
    // Temp file then rename: a power cut mid-write leaves the old list, never half a file.
    FILE *f = fopen(VNC_CONNS ".tmp", "w");
    if (!f) return;
    fputs("{\"list\":[", f);
    for (int i = 0; i < s_nsaved; i++) {
        const Saved &s = s_saved[i];
        fputs(i ? ",{\"name\":" : "{\"name\":", f); json_str(f, s.name);
        fputs(",\"host\":", f); json_str(f, s.host);
        fprintf(f, ",\"port\":%d,\"scale\":%d,\"auto\":%s,\"uid\":\"%08lx\"}", s.port, s.scale, s.autoc ? "true" : "false", (unsigned long)s.uid);
    }
    fprintf(f, "],\"last\":%d}\n", s_sel);
    const bool ok = fclose(f) == 0;                       // a failed write shows up when the file is flushed
    if (ok) { remove(VNC_CONNS); rename(VNC_CONNS ".tmp", VNC_CONNS); }
}

static uint32_t new_uid()
{
    for (;;) {
        const uint32_t u = esp_random();
        bool used = !u;
        for (int i = 0; i < s_nsaved; i++) used |= s_saved[i].uid == u;
        if (!used) return u;
    }
}

static void conns_load()
{
    s_nsaved = 0; s_sel = 0;
    bool fix = false;
    FILE *f = fopen(VNC_CONNS, "r");
    if (f) {
        char *buf = (char *)malloc(2048);
        const size_t n = buf ? fread(buf, 1, 2047, f) : 0;
        fclose(f);
        if (buf) {
            buf[n] = 0;
            cJSON *j = cJSON_Parse(buf);
            free(buf);
            const cJSON *arr = j ? cJSON_GetObjectItem(j, "list") : nullptr;
            const cJSON *o;
            cJSON_ArrayForEach(o, arr) {
                if (s_nsaved >= MAX_SAVED) break;
                Saved &s = s_saved[s_nsaved];
                memset(&s, 0, sizeof s);
                const cJSON *v;
                if (cJSON_IsString(v = cJSON_GetObjectItem(o, "name"))) snprintf(s.name, sizeof s.name, "%s", v->valuestring);
                if (cJSON_IsString(v = cJSON_GetObjectItem(o, "host"))) snprintf(s.host, sizeof s.host, "%s", v->valuestring);
                s.port = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "port")) ? v->valueint : 5900;
                if (s.port <= 0 || s.port > 65535) s.port = 5900;
                s.scale = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "scale")) ? v->valueint : 1;
                if (s.scale != 1 && s.scale != 2 && s.scale != 4) s.scale = 1;
                s.autoc = cJSON_IsTrue(cJSON_GetObjectItem(o, "auto"));
                s.uid = cJSON_IsString(v = cJSON_GetObjectItem(o, "uid")) ? (uint32_t)strtoul(v->valuestring, nullptr, 16) : 0;
                if (s.host[0]) s_nsaved++;
            }
            const cJSON *last = j ? cJSON_GetObjectItem(j, "last") : nullptr;
            if (cJSON_IsNumber(last) && last->valueint >= 0 && last->valueint < s_nsaved) s_sel = last->valueint;
            cJSON_Delete(j);
        }
    }
    for (int i = 0; i < s_nsaved; i++) if (!s_saved[i].uid) { s_saved[i].uid = new_uid(); fix = true; }
    // The prototype's single vnc.json becomes the first saved PC (its bench switch stays there).
    if (!s_nsaved && s_host[0]) {
        Saved &s = s_saved[0];
        memset(&s, 0, sizeof s);
        snprintf(s.name, sizeof s.name, "PC");
        snprintf(s.host, sizeof s.host, "%s", s_host);
        s.port = s_port; s.scale = s_want_scale; s.autoc = s_auto;
        s_nsaved = 1;
        s.uid = new_uid();
        fix = true;
    }
    if (fix) conns_save();
}

// ── the Cardputer's screen without flicker: every page is painted into a 240x45 band in RAM, three bands
// pushed one after the other. The page draws at its own coordinates through Y(); `d` is the band meanwhile. ──
#define BAND_H 45
static M5Canvas *s_band = nullptr;
static int s_oy = 0;
static inline int Y(int y) { return y - s_oy; }

static void band_alloc()
{
    if (s_band) return;
    s_band = new (std::nothrow) M5Canvas();
    if (s_band) {
        s_band->setColorDepth(16);
        if (!s_band->createSprite(240, BAND_H)) { delete s_band; s_band = nullptr; }
    }
}
static void band_free() { if (s_band) { s_band->deleteSprite(); delete s_band; s_band = nullptr; } }
static void band_render(LovyanGFX *dst, int top, int h, void (*paint)(void))
{
    if (!dst) return;
    if (!s_band) { s_oy = -top; paint(); s_oy = 0; return; }   // no RAM for the band: straight to the glass
    LovyanGFX *prev = nucleo_app_gfx();
    for (int y = 0; y < h; y += BAND_H) {
        nucleo_app_set_gfx(s_band);
        s_oy = y;
        s_band->fillScreen(C_BG);
        paint();
        nucleo_app_set_gfx(prev);
        s_band->pushSprite(dst, 0, top + y);
    }
    s_oy = 0;
}

// A BMP of the Cardputer's own screen on the card (the web server is off while the app runs).
static bool save_shot(const char *name)
{
    int w = 0, h = 0;
    if (!nucleo_extdisp_screen_size(0, &w, &h) || w <= 0 || w > 320 || h <= 0) return false;
    mkdir(VNC_DIR, 0777); mkdir(VNC_SHOTS, 0777);
    char path[96]; snprintf(path, sizeof path, VNC_SHOTS "/%s.bmp", name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int rs = (w * 3 + 3) & ~3;
    const uint32_t sz = 54 + (uint32_t)rs * h;
    uint8_t hd[54] = { 'B', 'M' };
    auto p32 = [&](int o, uint32_t v) { hd[o] = v; hd[o + 1] = v >> 8; hd[o + 2] = v >> 16; hd[o + 3] = v >> 24; };
    p32(2, sz); p32(10, 54); p32(14, 40); p32(18, w); p32(22, h); hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    static uint16_t row[320]; static uint8_t out[320 * 3 + 4];
    for (int y = h - 1; y >= 0; y--) {
        if (!nucleo_extdisp_screen_row(0, y, w, row)) memset(row, 0, sizeof row);
        memset(out, 0, rs);
        for (int x = 0; x < w; x++) {
            const uint16_t p = (uint16_t)((row[x] >> 8) | (row[x] << 8));
            out[x * 3 + 0] = (uint8_t)((p & 0x1F) * 255 / 31);
            out[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            out[x * 3 + 2] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        }
        fwrite(out, 1, rs, f);
    }
    fclose(f);
    return true;
}
static unsigned s_shots_done = 0;      // one automatic shot per page and app open (checking the design on the bench)
static void auto_shot(int page, const char *name)
{
    if (!s_test_setup) return;                           // only on the bench card (vnc.json), never on a user's card
    if (!(s_shots_done & (1u << page))) { s_shots_done |= 1u << page; save_shot(name); }
}

// ── small drawing helpers (the page's own coordinates, through Y()) ──
static void chip(int x, int y, const char *t, bool on)
{
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    const int w = (int)d.textWidth(t) + 10;
    const unsigned short bg = on ? ACCENT : lg::card();
    d.fillRoundRect(x, Y(y), w, 13, 6, bg);
    d.setTextColor(on ? C_BG : C_DIM, bg);
    d.drawString(t, x + 5, Y(y + 3));
}
static int chip_w(const char *t) { d.setFont(&fonts::Font0); d.setTextSize(1); return (int)d.textWidth(t) + 10; }
static void text(int x, int y, const char *t, unsigned short col, const lgfx::IFont *font = &fonts::Font0, unsigned short bg = C_BG)
{
    d.setFont(font); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(col, bg);
    d.drawString(t, x, Y(y));
}
static void head_band(const char *title, const char *right, unsigned short dot)
{
    d.fillRect(0, Y(0), 240, 18, lg::card());
    if (dot) d.fillCircle(9, Y(9), 3, dot);
    d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(C_INK, lg::card());
    char t[40]; lg::fit(t, sizeof t, title, right ? 140 : 220);
    d.drawString(t, dot ? 17 : 6, Y(1));
    if (right) {
        d.setFont(&fonts::Font0); d.setTextColor(C_DIM, lg::card()); d.setTextDatum(top_right);
        d.drawString(right, 234, Y(5)); d.setTextDatum(top_left);
    }
}
static void foot(const char *keys)
{
    d.fillRect(0, Y(123), 240, 12, C_BG);
    char t[64]; d.setFont(&fonts::Font0); lg::fit(t, sizeof t, keys, 228);
    text(6, 125, t, C_DIM);
}

// ── the session's control panel on the Cardputer ──
enum { CP_STATUS, CP_MENU, CP_KEYS };
static int s_cpage = CP_STATUS, s_msel = 0;
static uint32_t s_kbs = 0;
static int s_fps = 0;                     // updates in the last second
static char s_toast[48] = "";
static int64_t s_toast_until = 0;
static const char *s_cur_name = "";
static bool s_exit_app = false;           // Fn+Esc in a session: leave the whole app, not just the session

static const char *gesture_name()
{
    switch (G.mode) {
        case G_PRESS: return L("tocco", "tap", "toque", "appui", "Tippen");
        case G_HOLD:  return L("tieni: destro o trascina", "hold: right or drag", "manten: derecho o arrastra", "tenir: droit ou glisser", "halten: rechts oder ziehen");
        case G_DRAG:  return L("trascino", "dragging", "arrastrando", "glisse", "ziehe");
        case G_PAN:   return L("sposto la vista", "moving the view", "muevo la vista", "deplace la vue", "verschiebe Ansicht");
        case G_WHEEL: return L("rotella", "wheel", "rueda", "molette", "Rad");
        default:      return "";
    }
}

static void paint_status()
{
    char ln[64];
    snprintf(ln, sizeof ln, "1:%d", s_scale);
    head_band(s_cur_name, ln, C_GOOD);                   // connected (errors end the session)
    // the map: the whole desktop, the panel's window on it, the pointer
    const int mx = 6, my = 23, mw = 132, mh = 76;
    d.fillRoundRect(mx, Y(my), mw, mh, 6, lg::card());
    if (s_fbw > 0 && s_fbh > 0) {
        // the scale, in thousandths: whichever side fills the box first
        const int kx = (mw - 8) * 1000 / s_fbw, ky = (mh - 8) * 1000 / s_fbh, k = kx < ky ? kx : ky;
        const int dw = s_fbw * k / 1000, dh = s_fbh * k / 1000, ox = mx + (mw - dw) / 2, oy = my + (mh - dh) / 2;
        d.fillRect(ox, Y(oy), dw, dh, C_BG);
        d.drawRect(ox, Y(oy), dw, dh, C_DIM);
        int vx = s_vx, vy = s_vy;
        if (G.mode == G_PAN) { vx -= G.x - G.x0; vy -= G.y - G.y0; }   // live while the finger moves the view
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        int rw = s_vw * k / 1000, rh = s_vh * k / 1000;
        if (rw < 4) rw = 4;
        if (rh < 4) rh = 4;
        d.fillRect(ox + vx * k / 1000, Y(oy + vy * k / 1000), rw, rh, ACCENT);
        if (s_px >= 0) d.fillCircle(ox + s_px * k / 1000, Y(oy + s_py * k / 1000), 2, C_WARN);
    }
    // the numbers
    snprintf(ln, sizeof ln, "%d", s_fps);
    text(148, 22, ln, C_INK, &fonts::FreeSansBold12pt7b);
    text(148 + (int)d.textWidth(ln) + 4, 31, "fps", C_DIM);
    snprintf(ln, sizeof ln, "%lu KB/s", (unsigned long)s_kbs); text(148, 50, ln, C_INK, &fonts::Font2);
    snprintf(ln, sizeof ln, "%dx%d", s_fbw, s_fbh); text(148, 70, ln, C_DIM);
    snprintf(ln, sizeof ln, "RAM %uK", (unsigned)(M.free_now / 1024)); text(148, 82, ln, C_DIM);
    // held modifiers, then what the finger does
    const unsigned char mods = nucleo_kbd_mods();
    const char *names[] = { "Fn", "Ctrl", "Opt", "Alt" };                 // as on the keyboard
    const unsigned char bits[] = { NK_MOD_FN, NK_MOD_CTRL, NK_MOD_GUI, NK_MOD_ALT };
    int x = 6;
    for (int i = 0; i < 4; i++) { chip(x, 105, names[i], (mods & bits[i]) != 0); x += chip_w(names[i]) + 4; }
    if (s_toast[0] && esp_timer_get_time() < s_toast_until) text(x + 4, 108, s_toast, C_GOOD);
    else text(x + 4, 108, gesture_name(), C_GOOD);
    foot(L("Fn+Spazio menu   Fn+Esc esci dall'app", "Fn+Space menu   Fn+Esc quit the app", "Fn+Espacio menu   Fn+Esc salir",
           "Fn+Espace menu   Fn+Esc quitter", "Fn+Leer Menue   Fn+Esc beenden"));
}

enum { MA_ZOOM, MA_MOUSE, MA_KEYS, MA_DESKTOP, MA_ALTTAB, MA_CLOSE, MA_TASKS, MA_CAD, MA_SHOT, MA_QUIT, MA_N };
static bool s_mouse = false;              // the keyboard drives the pointer (Fn+Tab): the way to click without a touch screen
static const char *menu_label(int i)
{
    static char z[32];
    switch (i) {
        case MA_ZOOM:    snprintf(z, sizeof z, "Zoom  1:%d", s_single ? s_cz : s_want_scale); return z;
        case MA_MOUSE:   return s_mouse ? L("Mouse da tastiera: attivo", "Keyboard mouse: on", "Raton con teclado: si", "Souris au clavier: oui", "Tastaturmaus: an")
                                        : L("Mouse da tastiera (Fn+Tab)", "Keyboard mouse (Fn+Tab)", "Raton con teclado (Fn+Tab)", "Souris au clavier (Fn+Tab)", "Tastaturmaus (Fn+Tab)");
        case MA_KEYS:    return L("Tasti del PC (legenda)", "PC keys (legend)", "Teclas del PC (leyenda)", "Touches du PC (legende)", "PC-Tasten (Legende)");
        case MA_DESKTOP: return L("Mostra desktop", "Show desktop", "Mostrar escritorio", "Afficher le bureau", "Desktop zeigen");
        case MA_ALTTAB:  return L("Cambia finestra", "Switch window", "Cambiar ventana", "Changer de fenetre", "Fenster wechseln");
        case MA_CLOSE:   return L("Chiudi finestra", "Close window", "Cerrar ventana", "Fermer la fenetre", "Fenster schliessen");
        case MA_TASKS:   return L("Gestione attivita", "Task manager", "Administrador de tareas", "Gestionnaire de taches", "Task-Manager");
        case MA_CAD:     return "Ctrl+Alt+Canc";
        case MA_SHOT:    return L("Screenshot del Cardputer", "Cardputer screenshot", "Captura del Cardputer", "Capture du Cardputer", "Cardputer-Screenshot");
        default:         return L("Disconnetti (all'elenco)", "Disconnect (to the list)", "Desconectar (a la lista)", "Deconnecter (vers la liste)", "Trennen (zur Liste)");
    }
}
static void paint_menu()
{
    head_band(L("Comandi", "Commands", "Comandos", "Commandes", "Befehle"), s_cur_name, 0);
    const int rows = 6, rh = 17;
    int top = s_msel - rows / 2;
    if (top > MA_N - rows) top = MA_N - rows;
    if (top < 0) top = 0;
    for (int r = 0; r < rows && top + r < MA_N; r++) {
        const int i = top + r, y = 21 + r * rh;
        const bool f = i == s_msel;
        const unsigned short pill = i == MA_QUIT ? C_BAD : ACCENT;
        if (f) d.fillRoundRect(4, Y(y), 232, rh - 1, 6, pill);
        d.setFont(&fonts::Font2); d.setTextDatum(top_left);
        d.setTextColor(f ? C_BG : (i == MA_QUIT ? C_WARN : C_INK), f ? pill : C_BG);
        d.drawString(menu_label(i), 12, Y(y));
    }
    foot(L("; . scegli  Invio esegui  Esc chiudi  Fn+Esc esci", "; . pick  Enter run  Esc close  Fn+Esc quit", "; . elige  Intro  Esc cierra  Fn+Esc sale",
           "; . choisir  Entree  Esc ferme  Fn+Esc quitte", "; . waehlen  Enter  Esc zu  Fn+Esc Ende"));
}

static const char *legend(int i, int col)
{
    static const char *const KEYS[] = { "Fn + ; . , /", "Fn + 1 ... 0 - =", "Fn + Del", "Fn + [  ]", "Fn + H  E", "Fn + I  P  M", "Fn + W", "Esc",
                                        "Ctrl Alt Opt + ...", "Fn + Ctrl + ; . , /" };
    if (col == 0) return KEYS[i];
    switch (i) {
        case 0: return L("frecce", "arrows", "flechas", "fleches", "Pfeile");
        case 1: return "F1 ... F12";
        case 2: return L("Canc (avanti)", "Delete (forward)", "Suprimir", "Suppr", "Entf");
        case 3: return L("Pag Su  Pag Giu", "Page Up  Page Down", "Re Pag  Av Pag", "Page haut  Page bas", "Bild auf  Bild ab");
        case 4: return L("Inizio  Fine", "Home  End", "Inicio  Fin", "Debut  Fin", "Pos1  Ende");
        case 5: return L("Ins  Stamp  Menu", "Ins  PrtSc  Menu", "Ins  ImpPant  Menu", "Ins  ImprEc  Menu", "Einf  Druck  Menue");
        case 6: return L("tasto Windows", "Windows key", "tecla Windows", "touche Windows", "Windows-Taste");
        case 7: return L("Esc sul PC", "Esc on the PC", "Esc en el PC", "Esc sur le PC", "Esc am PC");
        case 8: return L("combinazioni", "shortcuts", "atajos", "raccourcis", "Kurzbefehle");
        default: return L("sposta la vista", "move the view", "mueve la vista", "deplace la vue", "Ansicht schieben");
    }
}
static void paint_keys()
{
    head_band(L("Tasti del PC", "PC keys", "Teclas del PC", "Touches du PC", "PC-Tasten"), "Fn", 0);
    for (int i = 0; i < 10; i++) {
        text(6, 21 + i * 9, legend(i, 0), C_INK);
        text(126, 21 + i * 9, legend(i, 1), C_DIM);
    }
    if (s_single) text(6, 112, L("Fn+Tab mouse: ; . , / muovi, Invio clic, Del destro", "Fn+Tab mouse: ; . , / move, Enter click, Del right",
                                 "Fn+Tab raton: ; . , / mueve, Intro clic, Del derecho", "Fn+Tab souris: ; . , / bouge, Entree clic, Del droit",
                                 "Fn+Tab Maus: ; . , / bewegt, Enter Klick, Del rechts"), C_GOOD);
    else text(6, 112, L("tocco clic, tieni destro, tieni+trascina seleziona", "tap click, hold right, hold+drag selects", "toque clic, manten derecho, manten+arrastra",
                   "appui clic, tenir droit, tenir+glisser", "tippen Klick, halten rechts, halten+ziehen"), C_GOOD);
    if (s_single) foot(L("mouse: - = rotella, D trascina  Esc torna", "mouse: - = wheel, D drag  Esc back", "raton: - = rueda, D arrastra  Esc",
                         "souris: - = molette, D glisser  Esc", "Maus: - = Rad, D ziehen  Esc"));
    else foot(L("trascina sposta, bordo destro rotella  Esc", "drag moves, right edge = wheel  Esc", "arrastra mueve, borde derecho rueda  Esc",
           "glisser deplace, bord droit molette  Esc", "ziehen schiebt, rechter Rand Rad  Esc"));
}

static void companion()
{
    LovyanGFX *ig = nucleo_surface_large() ? (LovyanGFX *)nucleo_extdisp_internal_gfx() : nullptr;
    if (!ig) return;
    band_render(ig, 0, 135, s_cpage == CP_MENU ? paint_menu : s_cpage == CP_KEYS ? paint_keys : paint_status);
}
static void toast(const char *t) { snprintf(s_toast, sizeof s_toast, "%s", t); s_toast_until = esp_timer_get_time() + 2500000; }

// The single screen's zoom, around the middle of the window.
static void set_zoom(int k)
{
    const int cx = s_vx + s_vw / 2, cy = s_vy + s_vh / 2;
    s_cz = k; s_vw = s_scr_w * k; s_vh = s_scr_h * k;
    s_vx = cx - s_vw / 2; s_vy = cy - s_vh / 2;
    clamp_view();
    if (!s_overlay) d.fillScreen(0);
    s_full = true;
    char t[24]; snprintf(t, sizeof t, "zoom 1:%d", k); toast(t);
}
static int next_zoom(int k) { return k == 1 ? 2 : k == 2 ? 4 : 1; }

// The pointer, drawn by the app over the desktop (a server does not draw it in the picture): where, and drawn yet.
static int s_cur_x = -1, s_cur_y = -1;     // screen position of the drawn one, -1 = none
static void cursor_forget()
{
    if (s_cur_x < 0) return;
    request_area(s_vx + (s_cur_x - 1) * s_cz, s_vy + (s_cur_y - 1) * s_cz, 12 * s_cz, 14 * s_cz);   // the server paints it away
    s_cur_x = s_cur_y = -1;
}
static void cursor_draw()
{
    if (!s_mouse || s_overlay || s_px < 0) return;
    const int x = (s_px - s_vx) / s_cz, y = (s_py - s_vy) / s_cz;
    if (x < 0 || y < 0 || x >= s_scr_w || y >= s_scr_h) return;
    hold_bus(false);
    d.fillTriangle(x, y, x, y + 11, x + 8, y + 8, TFT_WHITE);
    d.drawTriangle(x, y, x, y + 11, x + 8, y + 8, TFT_BLACK);
    s_cur_x = x; s_cur_y = y;
}
// The pointer moved: the window follows it near an edge (a quarter of the window at a time).
static void mouse_move(int dx, int dy, uint8_t mask)
{
    if (s_px < 0) { s_px = s_vx + s_vw / 2; s_py = s_vy + s_vh / 2; }
    cursor_forget();
    pointer(s_px + dx, s_py + dy, mask);
    const int m = 10 * s_cz;
    bool moved = false;
    if (s_px < s_vx + m && s_vx > 0)                 { s_vx -= s_vw / 4; moved = true; }
    if (s_px > s_vx + s_vw - m && s_vx + s_vw < s_fbw) { s_vx += s_vw / 4; moved = true; }
    if (s_py < s_vy + m && s_vy > 0)                 { s_vy -= s_vh / 4; moved = true; }
    if (s_py > s_vy + s_vh - m && s_vy + s_vh < s_fbh) { s_vy += s_vh / 4; moved = true; }
    if (moved) { clamp_view(); s_full = true; M.pans++; s_cur_x = s_cur_y = -1; }
    else cursor_draw();
}
static bool s_drag = false;               // keyboard mouse: the left button held (D)
// Keyboard mouse: ; . , / move (faster while held), Enter or Space click, Del right click, - = wheel, D drag, Esc off.
static bool mouse_key(const nucleo_key_t &k, int64_t now)
{
    static int64_t t_last = 0; static int run = 0;
    const bool arrow = k.key == NK_UP || k.key == NK_DOWN || k.key == NK_LEFT || k.key == NK_RIGHT;
    const uint8_t held = s_drag ? 1 : 0;
    if (arrow) {
        run = now - t_last < 250000 ? run + 1 : 0; t_last = now;
        const int step = (run < 3 ? 3 : run < 8 ? 8 : 20) * s_cz;
        mouse_move(k.key == NK_LEFT ? -step : k.key == NK_RIGHT ? step : 0, k.key == NK_UP ? -step : k.key == NK_DOWN ? step : 0, held);
        return true;
    }
    if (s_px < 0) mouse_move(0, 0, held);
    if (k.key == NK_ENTER || (k.key == NK_CHAR && k.ch == ' ')) { pointer(s_px, s_py, 1); pointer(s_px, s_py, 0); s_drag = false; M.clicks++; return true; }
    if (k.key == NK_DEL) { pointer(s_px, s_py, 4); pointer(s_px, s_py, held); M.clicks++; return true; }
    if (k.key == NK_CHAR && k.ch == '-') { pointer(s_px, s_py, 8 | held);  pointer(s_px, s_py, held); return true; }   // wheel up
    if (k.key == NK_CHAR && k.ch == '=') { pointer(s_px, s_py, 16 | held); pointer(s_px, s_py, held); return true; }   // wheel down
    if (k.key == NK_CHAR && (k.ch == 'd' || k.ch == 'D')) {
        s_drag = !s_drag; pointer(s_px, s_py, s_drag ? 1 : 0);
        toast(s_drag ? L("trascino: muovi e premi D", "dragging: move, then D", "arrastro: mueve y D", "glisse: bouge puis D", "ziehe: bewegen, dann D")
                     : L("rilasciato", "released", "soltado", "relache", "losgelassen"));
        return true;
    }
    if (k.key == NK_BACK) { s_mouse = false; s_drag = false; pointer(s_px, s_py, 0); cursor_forget(); toast(L("mouse spento", "mouse off", "raton apagado", "souris eteinte", "Maus aus")); return true; }
    return false;                                        // any other key still types
}
static void mouse_toggle()
{
    s_mouse = !s_mouse; s_drag = false;
    if (s_mouse) { mouse_move(0, 0, 0); toast(L("mouse: ; . , / Invio Del  Esc", "mouse: ; . , / Enter Del  Esc", "raton: ; . , / Intro Del  Esc",
                                              "souris: ; . , / Entree Del  Esc", "Maus: ; . , / Enter Del  Esc")); }
    else { cursor_forget(); toast(L("mouse spento", "mouse off", "raton apagado", "souris eteinte", "Maus aus")); }
}
static int s_shot_n = 0;
static bool shot_now() { char n[24]; snprintf(n, sizeof n, "cardputer-%d", ++s_shot_n); return save_shot(n); }
static const char *shot_word(bool ok)
{
    return ok ? L("screenshot salvato", "screenshot saved", "captura guardada", "capture enregistree", "Screenshot gespeichert")
              : L("screenshot fallito", "screenshot failed", "captura fallida", "capture ratee", "Screenshot fehlgeschlagen");
}

// ── the panel outside a session (connecting, errors): the kit's header and message ──
static void panel_message(const char *head, const char *sub, unsigned short col)
{
    d.startWrite();
    nucleo_touch_clear();
    const int y = lg::header(L("Desktop remoto", "Remote desktop", "Escritorio remoto", "Bureau a distance", "Fernzugriff"), s_cur_name, ACCENT, 0);
    d.fillRect(0, y, d.width(), d.height() - y, C_BG);
    lg::message(16, y + 16, d.width() - 32, d.height() - y - 32, head, sub, col);
    nucleo_touch_commit();
    d.endWrite();
}

// One full view three ways, once per session: is the time in the panel, the bus or the pixel copy?
static void panel_self_test()
{
    int64_t t0 = esp_timer_get_time();
    d.fillRect(0, 0, s_vw, s_vh, 0);
    M.panel_fill_us = esp_timer_get_time() - t0;
    memset(s_strip, 0, (size_t)s_strip_w * STRIP_ROWS * 2);
    t0 = esp_timer_get_time();
    for (int y = 0; y < s_vh; y += STRIP_ROWS) d.pushImage(0, y, s_vw, STRIP_ROWS, s_strip);
    M.panel_push_us = esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    d.startWrite();
    for (int y = 0; y < s_vh; y += STRIP_ROWS) d.pushImage(0, y, s_vw, STRIP_ROWS, s_strip);
    d.endWrite();
    M.panel_push_held_us = esp_timer_get_time() - t0;
    s_hold_draw = M.panel_push_held_us * 10 < M.panel_push_us * 8;   // keep the bus open only if it clearly pays
}

static const char *tcp_error(int e)
{
    switch (e) {
        case NUCLEO_TCP_NONET:  return L("nessun Wi-Fi", "no Wi-Fi", "sin Wi-Fi", "pas de Wi-Fi", "kein WLAN");
        case NUCLEO_TCP_NOHOST: return L("nome del PC sconosciuto", "unknown PC name", "nombre de PC desconocido", "nom de PC inconnu", "PC-Name unbekannt");
        case NUCLEO_TCP_DENIED: return L("indirizzo non permesso", "address not allowed", "direccion no permitida", "adresse interdite", "Adresse nicht erlaubt");
        default:                return L("PC non raggiungibile", "PC not reachable", "PC no alcanzable", "PC injoignable", "PC nicht erreichbar");
    }
}

static void run_session(const char *name)
{
    memset(&M, 0, sizeof M);
    M.t_start = esp_timer_get_time();
    s_auth_vnc = s_need_pw = false;
    nucleo_app_set_direct_draw(true);                    // the session draws straight to the screens: the canvas's RAM is its
    band_alloc();
    s_status[0] = 0;
    s_cur_name = name;
    bool switched = false;
    if (nucleo_surface_large_ok() && !nucleo_surface_large()) switched = nucleo_surface_set_large(true);
    s_scr_w = s_vw = d.width(); s_scr_h = s_vh = d.height(); s_strip_w = s_scr_w;
    s_single = !nucleo_surface_large(); s_cz = 1; s_overlay = false; s_mouse = false; s_drag = false; s_cur_x = s_cur_y = -1;
    M.view_w = s_vw; M.view_h = s_vh;
    const bool swap = d.getSwapBytes();
    d.setSwapBytes(false);                               // the strips are already in panel order
    char sub[96];
    snprintf(sub, sizeof sub, "%s:%d", s_host, s_port);
    if (!nucleo_tcp_network_up()) panel_message(L("Attendo il Wi-Fi...", "Waiting for Wi-Fi...", "Esperando Wi-Fi...", "Attente du Wi-Fi...", "Warte auf WLAN..."), sub, C_DIM);
    for (int i = 0; i < 200 && !nucleo_tcp_network_up(); i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(100)); }
    heap_sample(); M.free_before = M.free_now; M.free_min = M.largest_min = 0;
    C = {}; C.h = 0;
    G = Gesture{};
    s_cpage = CP_STATUS; s_msel = 0; s_toast[0] = 0;
    s_px = s_py = -1; s_scale = 1; s_fbw = s_fbh = 0; s_vx = s_vy = 0; s_fps = 0; s_kbs = 0;
    C.rb = (uint8_t *)malloc(RBUF);
    s_strip = (uint16_t *)heap_caps_malloc((size_t)s_strip_w * STRIP_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!C.rb || !s_strip) fail(L("RAM insufficiente", "not enough RAM", "RAM insuficiente", "RAM insuffisante", "zu wenig RAM"));
    else {
        panel_message(L("Connessione...", "Connecting...", "Conectando...", "Connexion...", "Verbinde..."), sub, C_DIM);
        const int h = nucleo_tcp_connect(s_host, s_port, 5000);
        if (h > 0) C.h = h; else fail(tcp_error(h));
    }
    if (!C.fail && rfb_handshake()) {
        M.t_connected = esp_timer_get_time();
        nucleo_touch_clear(); nucleo_touch_commit();
        nucleo_touch_raw(true);
        panel_self_test();
        d.fillScreen(0);
        clamp_view();
        if (s_nucleo_srv) ask_scale(s_want_scale);
        s_full = true; s_pending = 0;
        int64_t t_req = esp_timer_get_time(), t_sec = t_req, t_file = t_req, t_pan = t_req, t_comp = 0;
        uint32_t sec_bytes0 = M.bytes, sec_upd0 = 0;
        static const int PAN[][2] = { {0, 0}, {320, 0}, {640, 0}, {640, 240}, {320, 240}, {0, 240}, {0, 480}, {320, 480}, {640, 480} };
        int pan_i = 0;
        unsigned char mods_seen = 0;
        uint32_t upd_seen = 0;
        bool toast_shown = false;
        if (s_single) toast(L("Fn+Spazio menu   Fn+Tab mouse", "Fn+Space menu   Fn+Tab mouse", "Fn+Espacio menu   Fn+Tab raton",
                              "Fn+Espace menu   Fn+Tab souris", "Fn+Leer Menue   Fn+Tab Maus"));
        int gmode_seen = G_IDLE;
        bool quit = false, comp_dirty = true;
        while (!quit && !C.fail) {
            esp_task_wdt_reset();                         // the session runs inside one app callback: keep the dog fed
            int64_t now = esp_timer_get_time();
            // A full request starts the count again: a server may never answer the incremental one it still holds
            // for the old view (nothing changes there), and waiting for it would stall the new view.
            if (s_full) { request(false); s_full = false; s_pending = 1; t_req = now; }
            else if (s_pending == 0) { request(true); s_pending = 1; t_req = now; }
            if (data_waiting(5) && !handle_message(&t_req)) break;
            now = esp_timer_get_time();
            // touch
            nucleo_touch_raw_t te;
            for (int guard = 0; guard < 16 && nucleo_touch_raw_read(&te); guard++) touch_event(te, now);
            if (G.mode == G_PRESS && now - G.t0 >= HOLD_US) G.mode = G_HOLD;
            if (G.mode != gmode_seen || G.mode == G_PAN) { gmode_seen = G.mode; comp_dirty = true; }
            // keys
            for (int guard = 0; guard < 8; guard++) {
                const nucleo_key_t k = nucleo_kbd_read();
                if (k.key == NK_NONE) break;
                const unsigned char mods = nucleo_kbd_mods();
                const bool fn = mods & NK_MOD_FN;
                const unsigned char chord = mods & (NK_MOD_CTRL | NK_MOD_ALT | NK_MOD_GUI);
                const bool arrow = k.key == NK_UP || k.key == NK_DOWN || k.key == NK_LEFT || k.key == NK_RIGHT;
                comp_dirty = true;
                if (k.key == NK_BACK && fn) { quit = true; s_exit_app = true; break; }   // Fn+Esc: out of the app, from any page
                if (fn && k.key == NK_CHAR && k.ch == ' ') { s_cpage = s_cpage == CP_STATUS ? CP_MENU : CP_STATUS; continue; }
                if (s_cpage == CP_KEYS) {
                    if (k.key == NK_BACK || k.key == NK_ENTER) s_cpage = CP_MENU;
                    continue;
                }
                if (s_cpage == CP_MENU) {                     // the menu on the Cardputer owns the keys
                    if (k.key == NK_UP)   s_msel = (s_msel + MA_N - 1) % MA_N;
                    else if (k.key == NK_DOWN) s_msel = (s_msel + 1) % MA_N;
                    else if (k.key == NK_BACK) s_cpage = CP_STATUS;
                    else if (k.key == NK_ENTER) {
                        switch (s_msel) {
                            case MA_ZOOM:
                                if (s_single) { s_cpage = CP_STATUS; s_overlay = false; set_zoom(next_zoom(s_cz)); break; }
                                ask_scale(s_want_scale == 1 ? 2 : s_want_scale == 2 ? 4 : 1);
                                if (!s_nucleo_srv) toast(L("zoom: lo decide il PC", "zoom: set on the PC", "zoom: lo decide el PC", "zoom: regle sur le PC", "Zoom: am PC"));
                                break;
                            case MA_KEYS:    s_cpage = CP_KEYS; break;
                            case MA_MOUSE:   s_cpage = CP_STATUS; if (s_overlay) { s_overlay = false; d.fillScreen(0); s_full = true; } mouse_toggle(); break;
                            case MA_DESKTOP: combo(0xffeb, 0, 'd'); s_cpage = CP_STATUS; break;
                            case MA_ALTTAB:  combo(0xffe9, 0, 0xff09); s_cpage = CP_STATUS; break;
                            case MA_CLOSE:   combo(0xffe9, 0, 0xffc1); s_cpage = CP_STATUS; break;
                            case MA_TASKS:   combo(0xffe3, 0xffe1, 0xff1b); s_cpage = CP_STATUS; break;
                            case MA_CAD:     combo(0xffe3, 0xffe9, 0xffff); s_cpage = CP_STATUS; break;
                            case MA_SHOT:    toast(shot_word(shot_now())); s_cpage = CP_STATUS; break;
                            default:         quit = true; break;
                        }
                    }
                    continue;
                }
                if (s_mouse && !fn && !chord && mouse_key(k, now)) continue;                            // the keyboard mouse
                if (fn && k.key == NK_TAB) { mouse_toggle(); continue; }                                 // Fn+Tab: keyboard mouse on/off
                if (k.key == NK_BACK) key_tap(0xff1b, chord);                                          // Esc on the PC
                else if (fn && arrow && (mods & NK_MOD_CTRL)) {                                       // Fn+Ctrl+arrow: move the view
                    if (k.key == NK_LEFT)  s_vx -= s_vw / 2;
                    if (k.key == NK_RIGHT) s_vx += s_vw / 2;
                    if (k.key == NK_UP)    s_vy -= s_vh / 2;
                    if (k.key == NK_DOWN)  s_vy += s_vh / 2;
                    clamp_view(); s_full = true; M.pans++;
                }
                else if (fn && arrow) key_tap(k.key == NK_UP ? 0xff52 : k.key == NK_DOWN ? 0xff54 : k.key == NK_LEFT ? 0xff51 : 0xff53, chord);
                else if (fn && k.key == NK_DEL) key_tap(0xffff, chord);
                else if (fn && k.key == NK_CHAR && (k.ch == 'z' || k.ch == 'Z')) { if (s_single) set_zoom(next_zoom(s_cz)); else ask_scale(s_want_scale == 1 ? 2 : s_want_scale == 2 ? 4 : 1); }
                else if (fn && k.key == NK_CHAR && (k.ch == 's' || k.ch == 'S')) toast(shot_word(shot_now()));
                else if (fn && k.key == NK_CHAR) {
                    const char c = (k.ch >= 'A' && k.ch <= 'Z') ? (char)(k.ch + 32) : k.ch;
                    for (const FnKey &f : FN_KEYS) if (f.ch == c) { key_tap(f.sym, chord); break; }
                }
                else if (arrow) key_tap((unsigned char)k.ch, chord);                                 // ; . , / are text without Fn
                else if (k.key == NK_ENTER) key_tap(0xff0d, chord);
                else if (k.key == NK_DEL)   key_tap(0xff08, chord);
                else if (k.key == NK_TAB)   key_tap(0xff09, chord);
                else if (k.key == NK_CHAR && (unsigned char)k.ch >= 0x20 && (unsigned char)k.ch < 0x7f) {
                    // A chord sends the key itself (Ctrl+Shift+T = Ctrl, Shift and 't'); plain text goes as the character.
                    const bool upper = k.ch >= 'A' && k.ch <= 'Z';
                    const char c = (chord && upper) ? (char)(k.ch + 32) : k.ch;
                    key_tap((unsigned char)c, chord | (chord && upper ? NK_MOD_SHIFT : 0));
                }
            }
            const unsigned char mods_now = nucleo_kbd_mods();
            if (mods_now != mods_seen) { mods_seen = mods_now; comp_dirty = true; }
            if (s_bench > 0 && now - t_pan > 4000000) {   // bench: walk the desktop, a full view each time
                t_pan = now; pan_i = (pan_i + 1) % (int)(sizeof PAN / sizeof PAN[0]);
                s_vx = PAN[pan_i][0]; s_vy = PAN[pan_i][1]; clamp_view(); s_full = true; M.pans++;
            }
            if (now - t_sec >= 1000000) {
                const uint32_t b = M.bytes - sec_bytes0, u = M.updates - sec_upd0;
                if (b > M.sec_bytes_max) M.sec_bytes_max = b;
                sec_bytes0 = M.bytes; sec_upd0 = M.updates;
                s_kbs = b / 1024; s_fps = (int)((int64_t)u * 1000000 / (now - t_sec));
                heap_sample();
                t_sec = now; comp_dirty = true;
            }
            if (M.updates != upd_seen) {                      // an update may have painted over the pointer and the toast
                upd_seen = M.updates; cursor_draw();
                if (toast_shown && now < s_toast_until) toast_shown = false;
            }
            if (s_single) {
                if (s_cpage != CP_STATUS) {                       // the menu or the legend covers the desktop
                    if (!s_overlay) { s_overlay = true; s_cur_x = s_cur_y = -1; comp_dirty = true; }
                    if (comp_dirty && now - t_comp > 120000) {
                        hold_bus(false); d.setSwapBytes(swap);
                        band_render(nucleo_app_gfx(), 0, s_scr_h, s_cpage == CP_MENU ? paint_menu : paint_keys);
                        d.setSwapBytes(false);
                        comp_dirty = false; t_comp = now;
                        if (M.t_connected && now - M.t_connected > 8000000) auto_shot(s_cpage + 2, s_cpage == CP_MENU ? "session-menu" : "session-keys");
                    }
                } else {
                    if (s_overlay) { s_overlay = false; d.fillScreen(0); s_full = true; }
                    const bool tv = s_toast[0] && now < s_toast_until;
                    if (tv && !toast_shown) {                     // one line at the bottom, then that strip is asked again
                        hold_bus(false);
                        d.fillRect(0, s_scr_h - 14, s_scr_w, 14, lg::card());
                        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(middle_left); d.setTextColor(C_INK, lg::card());
                        char t[64]; lg::fit(t, sizeof t, s_toast, s_scr_w - 12);
                        d.drawString(t, 6, s_scr_h - 7); d.setTextDatum(top_left);
                        toast_shown = true;
                    } else if (!tv && toast_shown) {
                        toast_shown = false; s_toast[0] = 0;
                        request_area(s_vx, s_vy + (s_scr_h - 14) * s_cz, s_vw, 14 * s_cz);
                    }
                    if (comp_dirty && now - t_comp > 120000) {
                        comp_dirty = false; t_comp = now;
                        if (M.t_connected && now - M.t_connected > 8000000) auto_shot(2, "session");
                    }
                }
            } else if (comp_dirty && now - t_comp > 120000) {
                companion(); comp_dirty = false; t_comp = now;
                if (M.t_connected && now - M.t_connected > 8000000) auto_shot(s_cpage + 2, s_cpage == CP_MENU ? "session-menu" : s_cpage == CP_KEYS ? "session-keys" : "session");
            }
            if (s_bench > 0 && now - t_file > 10000000) { t_file = now; metrics_write("running"); }
            if (s_bench > 0 && M.t_connected && now - M.t_connected > (int64_t)s_bench * 1000000) quit = true;
        }
    }
    memset(s_pw, 0, sizeof s_pw);
    s_holding = false; hold_bus(false);                  // an update cut short by an error left it open
    nucleo_touch_raw(false);
    heap_sample();
    if (s_bench > 0) metrics_write(C.fail ? "failed" : "done");
    ESP_LOGW(TAG, "session end: %s, %lu B, %lu updates, heap min %u, largest min %u", C.fail ? M.err : "ok",
             (unsigned long)M.bytes, (unsigned long)M.updates, (unsigned)M.free_min, (unsigned)M.largest_min);
    if (C.h > 0) nucleo_tcp_close(C.h);
    C.h = 0;
    free(C.rb); C.rb = nullptr;
    free(s_strip); s_strip = nullptr;
    d.setSwapBytes(swap);
    if (switched) nucleo_surface_set_large(false);
    band_free();
    nucleo_app_set_direct_draw(false);                   // the list and the form: back to the system's canvas
    snprintf(s_status, sizeof s_status, "%s", C.fail ? M.err : "");
    nucleo_app_force_repaint();
    nucleo_app_request_draw();
}

// ── outside a session: the saved PCs and the form, on the panel; the Cardputer shows the detail ──
enum { P_LIST, P_FORM };
static int s_page = P_LIST, s_drawn = -1;
// the form
enum { F_NAME, F_HOST, F_PORT, F_PASS, F_ZOOM, F_AUTO, F_SAVE, F_DEL, F_N };
static Saved s_edit;
static int s_edit_i = -1;                 // -1 = a new one
static int s_fsel = 0, s_fscroll = 0;
static bool s_del_armed = false, s_pw_touched = false, s_pw_known = false;
static bool s_pw_prompt = false;          // opened because the PC asked: Enter on the password saves and connects
static char s_port_txt[8];

static int form_rows() { return s_edit_i >= 0 ? F_N : F_DEL; }
static bool form_text(int f) { return f == F_NAME || f == F_HOST || f == F_PORT || f == F_PASS; }
static char *form_buf(int f, int *cap)
{
    if (f == F_NAME) { *cap = sizeof s_edit.name; return s_edit.name; }
    if (f == F_HOST) { *cap = sizeof s_edit.host; return s_edit.host; }
    if (f == F_PASS) { *cap = 9; return s_pw; }        // VNC uses the first 8 characters
    *cap = sizeof s_port_txt; return s_port_txt;
}
static const char *field_label(int f)
{
    switch (f) {
        case F_NAME: return L("Nome", "Name", "Nombre", "Nom", "Name");
        case F_HOST: return L("Indirizzo del PC", "PC address", "Direccion del PC", "Adresse du PC", "PC-Adresse");
        case F_PORT: return L("Porta", "Port", "Puerto", "Port", "Port");
        case F_PASS: return L("Password VNC", "VNC password", "Contrasena VNC", "Mot de passe VNC", "VNC-Passwort");
        case F_ZOOM: return "Zoom";
        case F_AUTO: return L("Collega all'apertura", "Connect on open", "Conectar al abrir", "Connecter a l'ouverture", "Beim Start verbinden");
        case F_SAVE: return L("Salva", "Save", "Guardar", "Enregistrer", "Speichern");
        default:     return s_del_armed ? L("Invio di nuovo: elimina", "Enter again: delete", "Intro otra vez: borrar", "Entree encore: supprimer", "Nochmal Enter: loeschen")
                                        : L("Elimina", "Delete", "Borrar", "Supprimer", "Loeschen");
    }
}

static void open_form(int i)
{
    s_edit_i = i;
    if (i >= 0) s_edit = s_saved[i];
    else { memset(&s_edit, 0, sizeof s_edit); s_edit.port = 5900; s_edit.scale = 2; }
    snprintf(s_port_txt, sizeof s_port_txt, "%d", s_edit.port);
    memset(s_pw, 0, sizeof s_pw);
    s_pw_touched = false;
    s_pw_known = false;
    if (i >= 0) { char k[16]; pw_key(s_edit.uid, k); char tmp[NUCLEO_SECRET_MAX]; s_pw_known = nucleo_secret_get(k, tmp, sizeof tmp) && tmp[0]; memset(tmp, 0, sizeof tmp); }
    s_fsel = 0; s_fscroll = 0; s_del_armed = false; s_status[0] = 0; s_pw_prompt = false;
    s_page = P_FORM;
    nucleo_app_request_draw();
}
static void form_leave() { memset(s_pw, 0, sizeof s_pw); s_page = P_LIST; s_status[0] = 0; s_pw_prompt = false; }
static void connect_sel();
static void form_save()
{
    s_edit.port = atoi(s_port_txt);
    if (!s_edit.host[0]) { snprintf(s_status, sizeof s_status, "%s", L("manca l'indirizzo del PC", "the PC address is missing", "falta la direccion", "adresse manquante", "Adresse fehlt")); s_fsel = F_HOST; return; }
    if (s_edit.port <= 0 || s_edit.port > 65535) { snprintf(s_status, sizeof s_status, "%s", L("porta non valida", "invalid port", "puerto no valido", "port invalide", "ungueltiger Port")); s_fsel = F_PORT; return; }
    if (!s_edit.name[0]) snprintf(s_edit.name, sizeof s_edit.name, "%s", s_edit.host);
    if (s_edit_i < 0 && s_nsaved >= MAX_SAVED) { snprintf(s_status, sizeof s_status, "%s", L("elenco pieno (8 PC)", "list full (8 PCs)", "lista llena (8 PC)", "liste pleine (8 PC)", "Liste voll (8 PCs)")); return; }
    if (!s_edit.uid) s_edit.uid = new_uid();
    if (s_pw_touched) {                                  // only a password typed now replaces the stored one
        char k[16]; pw_key(s_edit.uid, k);
        if (!nucleo_secret_set(k, s_pw)) { snprintf(s_status, sizeof s_status, "%s", L("password non salvata", "password not saved", "contrasena no guardada", "mot de passe non garde", "Passwort nicht gespeichert")); return; }
    }
    if (s_edit.autoc) for (int i = 0; i < s_nsaved; i++) s_saved[i].autoc = false;   // one PC opens by itself, at most
    if (s_edit_i >= 0) { s_saved[s_edit_i] = s_edit; s_sel = s_edit_i; }
    else { s_saved[s_nsaved] = s_edit; s_sel = s_nsaved++; }
    conns_save();
    const bool again = s_pw_prompt;
    form_leave();
    if (again) connect_sel();                            // the PC asked for it: try the new password straight away
}
static void form_delete()
{
    if (s_edit_i < 0) return;
    char k[16]; pw_key(s_saved[s_edit_i].uid, k);
    nucleo_secret_set(k, nullptr);                       // its password goes with it
    for (int i = s_edit_i; i + 1 < s_nsaved; i++) s_saved[i] = s_saved[i + 1];
    s_nsaved--;
    if (s_sel >= s_nsaved) s_sel = s_nsaved > 0 ? s_nsaved - 1 : 0;
    conns_save();
    form_leave();
}
static void form_activate()
{
    switch (s_fsel) {
        case F_ZOOM: s_edit.scale = s_edit.scale == 1 ? 2 : s_edit.scale == 2 ? 4 : 1; break;
        case F_AUTO: s_edit.autoc = !s_edit.autoc; break;
        case F_SAVE: form_save(); break;
        case F_DEL:  if (s_del_armed) form_delete(); else s_del_armed = true; break;
        case F_PASS: if (s_pw_prompt) { form_save(); break; } if (s_fsel + 1 < form_rows()) s_fsel++; break;
        default:     if (s_fsel + 1 < form_rows()) s_fsel++; break;     // a text field: Enter goes on
    }
}

static void connect_sel()
{
    if (s_sel < 0 || s_sel >= s_nsaved) return;
    const Saved &s = s_saved[s_sel];
    snprintf(s_host, sizeof s_host, "%s", s.host);
    s_port = s.port; s_want_scale = s.scale;
    char k[16]; pw_key(s.uid, k);
    nucleo_secret_get(k, s_pw, sizeof s_pw);             // wiped by the login (or at the end of the session)
    conns_save();                                         // remembers the last one used
    static char name[24];
    snprintf(name, sizeof name, "%s", s.name);
    s_exit_app = false;
    const int sel = s_sel;
    run_session(name);
    s_drawn = -1;
    if (s_exit_app) { nucleo_app_exit(); return; }
    if (s_need_pw && s_test_setup == false) {            // the PC wants a password: ask for it right here
        char why[sizeof s_status]; snprintf(why, sizeof why, "%s", s_status);
        open_form(sel);
        s_fsel = F_PASS; s_pw_touched = true; s_pw_prompt = true;
        snprintf(s_status, sizeof s_status, "%s", why);
    }
}

// list rows: the saved PCs, then "new"
static void list_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    if (i < s_nsaved) {
        char r[80]; snprintf(r, sizeof r, "%s%s", s_saved[i].host, s_saved[i].autoc ? "  *" : "");
        lg::row_text(x, y, w, h, focus, bg, s_saved[i].name, r, C_INK);
    } else lg::row_text(x, y, w, h, focus, bg, L("+  Nuovo collegamento", "+  New connection", "+  Nueva conexion", "+  Nouvelle connexion", "+  Neue Verbindung"), nullptr, ACCENT);
}
static void form_row(int i, lg::Setting *s, void *)
{
    s->label = field_label(i);
    static char v[4][72];
    switch (i) {
        case F_NAME: case F_HOST: case F_PORT: {
            int cap; const char *b = form_buf(i, &cap);
            snprintf(v[i], sizeof v[i], "%s%s", b, i == s_fsel ? "_" : "");
            s->value = v[i][0] ? v[i] : "-";
            break;
        }
        case F_PASS: {                                   // never shown: dots while typing, a word for the stored one
            const int n = (int)strlen(s_pw);
            if (s_pw_touched) { memset(v[3], '*', n); v[3][n] = 0; if (i == s_fsel) strcat(v[3], "_"); s->value = v[3][0] ? v[3] : "-"; }
            else s->value = s_pw_known ? L("salvata", "saved", "guardada", "enregistre", "gespeichert") : L("nessuna", "none", "ninguna", "aucun", "keins");
            break;
        }
        case F_ZOOM: { static char z[8]; snprintf(z, sizeof z, "1:%d", s_edit.scale); s->kind = lg::ROW_CYCLE; s->value = z; break; }
        case F_AUTO: s->kind = lg::ROW_TOGGLE; s->on = s_edit.autoc; break;
        case F_SAVE: s->kind = lg::ROW_NAV; s->lcol = ACCENT; break;
        default:     s->kind = lg::ROW_DANGER; break;
    }
}
static void paint_comp_list();
static void paint_comp_form();
#define LIST_ROWH 38
#define FORM_ROWH 32
static void on_draw_large()
{
    const int W = d.width(), H = d.height();
    const bool full = s_drawn != s_page;
    nucleo_touch_clear();
    if (full) d.fillScreen(C_BG);
    if (s_page == P_LIST) {
        char right[40];
        if (s_status[0]) snprintf(right, sizeof right, "%s", s_status);
        else snprintf(right, sizeof right, "%d PC", s_nsaved);
        const int y = lg::header(L("Desktop remoto", "Remote desktop", "Escritorio remoto", "Bureau a distance", "Fernzugriff"), right, ACCENT);
        lg::list(0, y + 4, W, H - y - 8, LIST_ROWH, s_nsaved + 1, s_sel, &s_scroll, list_row, nullptr, ACCENT);
    } else {
        const int y = lg::header(s_edit_i >= 0 ? L("Modifica collegamento", "Edit connection", "Editar conexion", "Modifier la connexion", "Verbindung bearbeiten")
                                               : L("Nuovo collegamento", "New connection", "Nueva conexion", "Nouvelle connexion", "Neue Verbindung"),
                                 s_status[0] ? s_status : nullptr, ACCENT);
        lg::settings_list(8, y + 4, W - 16, H - y - 8, FORM_ROWH, form_rows(), s_fsel, &s_fscroll, form_row, nullptr, ACCENT);
    }
    nucleo_touch_commit();
    s_drawn = s_page;
}

// ── the Cardputer's screen outside a session: the system's canvas (buffered, no flicker) and the system's kit ──
// Two screens: the companion of the panel (lg::comp_head / comp_keys), the detail of what the panel shows.
// One screen: the pages themselves, as every Cardputer list app (app_ui), the keys in the system's bar.
static const char *KEYS_LIST_PC()  { return L("Invio collega  E modifica  N nuovo", "Enter connect  E edit  N new", "Intro conecta  E edita  N nueva",
                                             "Entree connecte  E modifie  N nouveau", "Enter verbindet  E aendern  N neu"); }
static const char *KEYS_LIST_NEW() { return L("Invio crea  Fn+Esc esci", "Enter create  Fn+Esc quit", "Intro crea  Fn+Esc sale", "Entree cree  Fn+Esc quitte", "Enter neu  Fn+Esc Ende"); }
static const char *KEYS_FORM_TXT() { return L("Invio avanti  Fn+; . campo  Esc annulla", "Enter next  Fn+; . field  Esc cancel", "Intro sigue  Fn+; . campo  Esc anula",
                                             "Entree suite  Fn+; . champ  Esc annule", "Enter weiter  Fn+; . Feld  Esc ab"); }
static const char *KEYS_FORM_OPT() { return L("; . campo  Invio cambia  Esc annulla", "; . field  Enter change  Esc cancel", "; . campo  Intro cambia  Esc anula",
                                             "; . champ  Entree change  Esc annule", "; . Feld  Enter aendert  Esc ab"); }
static const char *form_hint_text()
{
    switch (s_fsel) {
        case F_NAME: return L("Come lo vuoi chiamare", "What to call it", "Como llamarlo", "Comment l'appeler", "Wie er heissen soll");
        case F_HOST: return L("IP o nome del PC, es. 192.168.1.20", "The PC's IP or name, e.g. 192.168.1.20", "IP o nombre del PC, ej. 192.168.1.20",
                              "IP ou nom du PC, ex. 192.168.1.20", "IP oder Name des PCs, z.B. 192.168.1.20");
        case F_PORT: return L("Solo cifre, di solito 5900", "Digits only, usually 5900", "Solo cifras, normalmente 5900", "Chiffres, souvent 5900", "Nur Ziffern, meist 5900");
        case F_PASS: return s_pw_prompt ? L("Scrivila e premi Invio: mi collego", "Type it and press Enter to connect", "Escribela y pulsa Intro",
                                            "Tapez-le puis Entree", "Eingeben, dann Enter")
                                        : L("Max 8 caratteri, resta nel Cardputer", "8 characters max, kept in the Cardputer", "Max 8, queda en el Cardputer",
                                            "8 max, reste dans le Cardputer", "Max 8, bleibt im Cardputer");
        case F_ZOOM: return L("Quanto rimpicciolire il desktop", "How much to shrink the desktop", "Cuanto reducir el escritorio", "Reduction du bureau", "Wie stark verkleinern");
        case F_AUTO: return L("Si collega appena apri l'app", "Connects as soon as the app opens", "Conecta al abrir la app", "Connecte des l'ouverture", "Verbindet beim Start");
        case F_SAVE: return L("Salva e torna all'elenco", "Save and back to the list", "Guardar y volver", "Enregistrer et revenir", "Speichern, zurueck");
        default:     return L("Toglie il PC e la sua password", "Removes the PC and its password", "Quita el PC y su contrasena", "Retire le PC et son mot de passe", "Entfernt PC und Passwort");
    }
}
// What a form field holds, as shown: passwords as stars (or whether one is stored), a cursor on the field being typed.
static const char *form_value(int f, char *v, int cap)
{
    switch (f) {
        case F_NAME: case F_HOST: case F_PORT: { int c; snprintf(v, cap, "%s%s", form_buf(f, &c), f == s_fsel ? "_" : ""); break; }
        case F_PASS:
            if (s_pw_touched) { const int n = (int)strlen(s_pw); int k = 0; for (; k < n && k < cap - 2; k++) v[k] = '*'; if (f == s_fsel) v[k++] = '_'; v[k] = 0; }
            else snprintf(v, cap, "%s", s_pw_known ? L("salvata", "saved", "guardada", "enregistre", "gespeichert") : L("nessuna", "none", "ninguna", "aucun", "keins"));
            break;
        case F_ZOOM: snprintf(v, cap, "< 1:%d >", s_edit.scale); break;
        case F_AUTO: snprintf(v, cap, "%s", s_edit.autoc ? L("si", "on", "si", "oui", "an") : L("no", "off", "no", "non", "aus")); break;
        case F_DEL:  snprintf(v, cap, "%s", s_del_armed ? L("Invio: conferma", "Enter: confirm", "Intro: confirma", "Entree: confirme", "Enter: bestaetigen") : ""); break;
        default: v[0] = 0; break;
    }
    return v;
}
static const char *short_label(int f)
{
    switch (f) {
        case F_NAME: return L("Nome", "Name", "Nombre", "Nom", "Name");
        case F_HOST: return L("Indirizzo", "Address", "Direccion", "Adresse", "Adresse");
        case F_PORT: return L("Porta", "Port", "Puerto", "Port", "Port");
        case F_PASS: return "Password";
        case F_ZOOM: return "Zoom";
        case F_AUTO: return L("All'avvio", "On open", "Al abrir", "Au demarrage", "Beim Start");
        case F_SAVE: return L("Salva", "Save", "Guardar", "Enregistrer", "Speichern");
        default:     return L("Elimina", "Delete", "Borrar", "Supprimer", "Loeschen");
    }
}
static const char *NEW_ROW() { return L("+ Nuovo PC", "+ New PC", "+ Nuevo PC", "+ Nouveau PC", "+ Neuer PC"); }

// one screen: the list
static const char *list_label(int i, void *) { return i < s_nsaved ? s_saved[i].name : NEW_ROW(); }
static const char *list_right(int i, void *) { return i < s_nsaved ? s_saved[i].host : nullptr; }
static unsigned short list_color(int i, void *) { return i < s_nsaved ? ACCENT : C_GOOD; }
// one screen: the form, one row per field (label, value)
static char s_fval[F_N][40];
static const char *frow_label(int i, void *) { return short_label(i); }
static const char *frow_right(int i, void *) { return form_value(i, s_fval[i], sizeof s_fval[i]); }
static unsigned short frow_color(int i, void *) { return i == F_DEL ? C_BAD : i == F_SAVE ? C_GOOD : ACCENT; }

// A line under the title: what went wrong, or the field's help.
static int note_line(int y, const char *t, unsigned short col)
{
    d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(col, C_BG);
    char f[64]; lg::fit(f, sizeof f, t, 228);
    d.drawString(f, 8, y);
    d.setFont(&fonts::Font0);
    return y + 17;
}

static void single_list()
{
    const int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    char right[12]; snprintf(right, sizeof right, "%d PC", s_nsaved);
    int y = app_ui_title(L("Desktop remoto", "Remote desktop", "Escritorio remoto", "Bureau a distance", "Fernzugriff"), ACCENT, right);
    if (s_status[0]) y = note_line(y, s_status, C_WARN);
    else if (!s_nsaved) y = note_line(y, L("Serve un server VNC sul PC", "Needs a VNC server on the PC", "Hace falta un servidor VNC", "Il faut un serveur VNC", "Braucht einen VNC-Server"), C_INK);
    app_ui_list(y, bottom - y, s_nsaved + 1, s_sel, list_label, list_right, list_color, nullptr);
    nucleo_app_set_hint(s_sel < s_nsaved ? KEYS_LIST_PC() : KEYS_LIST_NEW());
}
static void single_form()
{
    const int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    char right[8]; snprintf(right, sizeof right, "%d/%d", s_fsel + 1, form_rows());
    int y = app_ui_title(s_edit_i >= 0 ? L("Modifica", "Edit", "Editar", "Modifier", "Bearbeiten") : L("Nuovo PC", "New PC", "Nuevo PC", "Nouveau PC", "Neuer PC"), ACCENT, right);
    y = s_status[0] ? note_line(y, s_status, C_WARN) : note_line(y, form_hint_text(), C_DIM);
    app_ui_list(y, bottom - y, form_rows(), s_fsel, frow_label, frow_right, frow_color, nullptr);
    nucleo_app_set_hint(form_text(s_fsel) ? KEYS_FORM_TXT() : KEYS_FORM_OPT());
}

// two screens: the companion of the panel
static void paint_comp_list()
{
    int y = lg::comp_head(L("Desktop remoto", "Remote desktop", "Escritorio remoto", "Bureau a distance", "Fernzugriff"), ACCENT);
    if (s_sel < s_nsaved) {
        const Saved &s = s_saved[s_sel];
        char ln[96]; lg::fit(ln, sizeof ln, s.name, 224);
        text(8, y + 4, ln, C_INK, &fonts::FreeSansBold12pt7b);
        snprintf(ln, sizeof ln, "%s : %d", s.host, s.port);
        char f[96]; d.setFont(&fonts::Font2); lg::fit(f, sizeof f, ln, 224);
        text(8, y + 34, f, ACCENT, &fonts::Font2);
        if (s_status[0]) { d.setFont(&fonts::Font2); d.setTextColor(C_WARN, C_BG); lg::wrap(s_status, 8, y + 56, 224, 17, 2); }
        else {
            snprintf(ln, sizeof ln, "zoom 1:%d%s", s.scale, s.autoc ? L("  -  si apre da solo", "  -  opens by itself", "  -  se abre solo", "  -  s'ouvre seul", "  -  startet selbst") : "");
            text(8, y + 56, ln, C_INK, &fonts::Font2);
            d.setFont(&fonts::Font2); d.setTextColor(C_DIM, C_BG);
            lg::wrap(L("VNC non e cifrato: usalo solo nella rete di casa", "VNC is not encrypted: home network only", "VNC no cifra: solo en la red de casa",
                       "VNC n'est pas chiffre: reseau maison seulement", "VNC ist unverschluesselt: nur im Heimnetz"), 8, y + 76, 224, 16, 2);
        }
        lg::comp_keys(KEYS_LIST_PC());
    } else {
        text(8, y + 4, L("Nuovo PC", "New PC", "Nuevo PC", "Nouveau PC", "Neuer PC"), C_INK, &fonts::FreeSansBold12pt7b);
        d.setFont(&fonts::Font2); d.setTextColor(C_INK, C_BG);
        lg::wrap(L("Sul PC serve un server VNC (TightVNC, RealVNC...), di solito sulla porta 5900.",
                   "The PC needs a VNC server (TightVNC, RealVNC...), usually on port 5900.",
                   "El PC necesita un servidor VNC (TightVNC, RealVNC...), puerto 5900.",
                   "Le PC doit avoir un serveur VNC (TightVNC, RealVNC...), port 5900.",
                   "Der PC braucht einen VNC-Server (TightVNC, RealVNC...), Port 5900."), 8, y + 34, 224, 17, 4);
        lg::comp_keys(KEYS_LIST_NEW());
    }
}
static void paint_comp_form()
{
    int y = lg::comp_head(s_edit_i >= 0 ? L("Modifica", "Edit", "Editar", "Modifier", "Bearbeiten") : L("Nuovo PC", "New PC", "Nuevo PC", "Nouveau PC", "Neuer PC"), ACCENT);
    text(8, y + 2, field_label(s_fsel), C_INK, &fonts::Font2);
    char v[48]; form_value(s_fsel, v, sizeof v);
    // the value, big: the end of a long address stays in sight
    d.fillRoundRect(6, y + 20, 228, 32, 8, lg::card());
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextSize(1); d.setTextDatum(middle_left); d.setTextColor(C_INK, lg::card());
    const char *show = v;
    while ((int)d.textWidth(show) > 212 && *show) show++;
    d.drawString(show[0] ? show : "-", 14, y + 36); d.setTextDatum(top_left);
    if (s_status[0]) { d.setFont(&fonts::Font2); d.setTextColor(C_WARN, C_BG); lg::wrap(s_status, 8, y + 58, 224, 17, 2); }
    else { d.setFont(&fonts::Font2); d.setTextColor(C_DIM, C_BG); lg::wrap(form_hint_text(), 8, y + 58, 224, 17, 2); }
    lg::comp_keys(form_text(s_fsel) ? KEYS_FORM_TXT() : KEYS_FORM_OPT());
}
static void on_draw()
{
    if (nucleo_app_two_screens()) { if (s_page == P_FORM) paint_comp_form(); else paint_comp_list(); }
    else {
        if (s_page == P_FORM) single_form(); else single_list();
        s_drawn = s_page;                                // one screen: this is the page (the bench's screenshots wait for it)
    }
}

static void form_key(int key, char ch)
{
    const unsigned char mods = nucleo_kbd_mods();
    const bool fn = mods & NK_MOD_FN;
    const int n = form_rows();
    if (s_fsel != F_DEL) s_del_armed = false;
    if (key == NK_BACK) { form_leave(); return; }
    if (nucleo_touch_is_row(key, ch)) {
        const int r = lg::row_tap(key, ch, s_fscroll, n, &s_fsel);
        if (r == 2) form_activate();
        return;
    }
    const bool text_row = form_text(s_fsel);
    // On a text field ; . , / are characters (an address has dots); Fn+; . move between fields.
    if ((key == NK_UP || key == NK_DOWN) && (fn || !text_row)) { s_fsel = (s_fsel + (key == NK_UP ? n - 1 : 1)) % n; return; }
    if ((key == NK_LEFT || key == NK_RIGHT) && !text_row) {
        if (s_fsel == F_ZOOM) s_edit.scale = key == NK_RIGHT ? (s_edit.scale == 1 ? 2 : s_edit.scale == 2 ? 4 : 1) : (s_edit.scale == 4 ? 2 : s_edit.scale == 2 ? 1 : 4);
        else if (s_fsel == F_AUTO) s_edit.autoc = key == NK_RIGHT;
        return;
    }
    if (key == NK_ENTER || key == NK_TAB) { if (key == NK_TAB && text_row) s_fsel = (s_fsel + 1) % n; else form_activate(); return; }
    if (!text_row) return;
    if (s_fsel == F_PASS && !s_pw_touched) { memset(s_pw, 0, sizeof s_pw); s_pw_touched = true; }   // typing replaces it
    int cap; char *b = form_buf(s_fsel, &cap);
    const int len = (int)strlen(b);
    if (key == NK_DEL) { if (len) b[len - 1] = 0; return; }
    const char c = ch;
    if ((key == NK_CHAR || key == NK_UP || key == NK_DOWN || key == NK_LEFT || key == NK_RIGHT) && c >= 0x20 && c < 0x7f && len < cap - 1) {
        if (s_fsel == F_PORT && (c < '0' || c > '9')) return;
        if (s_fsel == F_HOST && (c == ' ' || c == '/' || c == '\\' || c == ';' || c == ',')) return;
        b[len] = c; b[len + 1] = 0;
    }
}

static void list_key(int key, char ch)
{
    const int n = s_nsaved + 1;
    if (key == NK_BACK) { nucleo_app_exit(); return; }
    if (nucleo_touch_is_hold(key, ch)) {                  // a finger held on a PC: edit it
        int tx, ty;
        if (nucleo_touch_last(&tx, &ty)) {
            const int i = s_scroll + (ty - lg::HEAD - 4) / LIST_ROWH;
            if (i >= 0 && i < s_nsaved) { s_sel = i; open_form(i); }
        }
        return;
    }
    if (nucleo_touch_is_row(key, ch)) {
        if (lg::row_tap(key, ch, s_scroll, n, &s_sel) == 2) { if (s_sel < s_nsaved) connect_sel(); else open_form(-1); }
        return;
    }
    if (key == NK_UP)   { s_sel = (s_sel + n - 1) % n; return; }
    if (key == NK_DOWN) { s_sel = (s_sel + 1) % n; return; }
    if (key == NK_ENTER) { if (s_sel < s_nsaved) connect_sel(); else open_form(-1); return; }
    if (key == NK_CHAR && (ch == 'n' || ch == 'N')) { open_form(-1); return; }
    if (key == NK_CHAR && (ch == 'e' || ch == 'E') && s_sel < s_nsaved) { open_form(s_sel); return; }
    if (key == NK_TAB && s_sel < s_nsaved) open_form(s_sel);
}

static void on_key(int key, char ch)
{
    if (key == NK_BACK && (nucleo_kbd_mods() & NK_MOD_FN)) { memset(s_pw, 0, sizeof s_pw); nucleo_app_exit(); return; }   // Fn+Esc: out of the app
    if (key == NK_CHAR && (ch == 's' || ch == 'S') && (nucleo_kbd_mods() & NK_MOD_FN)) { save_shot(s_page == P_FORM ? "form" : "list"); return; }
    if (s_page == P_FORM) form_key(key, ch); else list_key(key, ch);
    nucleo_app_request_draw();
}
static void on_tick()
{
    if (s_page == P_LIST && s_drawn == P_LIST) auto_shot(0, "list");
    if (s_page == P_FORM && s_drawn == P_FORM) auto_shot(1, "form");
    if (!s_auto_done) {
        s_auto_done = true;
        int a = -1;
        for (int i = 0; i < s_nsaved; i++) if (s_saved[i].autoc) a = i;
        if (s_bench > 0 && s_nsaved) a = s_sel;
        if (a >= 0) {
            s_sel = a;
            connect_sel();
            // Bench card: run the sessions back to back and write how each one ended (a crash leaves the log short).
            for (int run = 1; run <= s_retries + 1; run++) {
                FILE *lf = s_test_setup ? fopen(VNC_DIR "/sessions.log", run == 1 ? "w" : "a") : nullptr;
                if (lf) { fprintf(lf, "%d %s %s heapmin=%u\n", run, C.fail ? "error" : "ok", M.err, (unsigned)M.free_min); fclose(lf); }
                if (run > s_retries) break;
                esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1500));
                connect_sel();
            }
            if (s_bench > 0 || s_retries > 0) { nucleo_app_exit(); return; }   // a bench run goes back to the full OS by itself
        }
    }
}
static bool back_handler(int key)
{
    (void)key;
    if (nucleo_kbd_mods() & NK_MOD_FN) return false;     // Fn+Esc: the framework closes the app
    if (s_page == P_FORM) { form_leave(); nucleo_app_request_draw(); return true; }
    return false;
}
static void on_enter()
{
    s_status[0] = 0;
    s_host[0] = 0;
    s_test_setup = false;
    memset(s_pw, 0, sizeof s_pw);
    load_config();                                       // the prototype's vnc.json (bench, import)
    conns_load();
    s_page = P_LIST; s_drawn = -1; s_scroll = 0; s_shots_done = 0; s_auto_done = false;
    nucleo_app_set_keep_awake(true);                     // the list and the form draw buffered: the system's canvas
    nucleo_app_set_back_handler(back_handler);
    nucleo_app_set_hint(L("Invio collega  E modifica  N nuovo", "Enter connect  E edit  N new", "Intro conecta  E edita  N nueva",
                          "Entree connecte  E modifie  N nouveau", "Enter verbindet  E aendern  N neu"));
    nucleo_app_request_draw();
}
static void on_exit()
{
    memset(s_pw, 0, sizeof s_pw);
    nucleo_touch_raw(false);
    if (C.h > 0) { nucleo_tcp_close(C.h); C.h = 0; }
    band_free();
}

static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_saved, sizeof(Saved) * MAX_SAVED },
    { nullptr, 0 } };

extern "C" void nucleo_register_vnc(void)
{
    static const nucleo_app_def_t app = {
        "vnc", "VNC", "Connect", "Desktop remoto VNC",
        'v', ACCENT, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_SOLO,          // a fresh heap with Wi-Fi up: httpd, mDNS and ANIMA stay off for the session
        APP_RAM, on_draw_large
    };
    nucleo_app_register(&app);
}

#endif  // NAP_MODULE
