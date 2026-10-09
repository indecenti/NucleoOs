// native-host core: the simulated Cardputer every native-game scenario runs on. Include it AFTER the game's
// own source (so the scenario sees the game's statics):
//
//     #include "../../../firmware/components/nucleo_app/app_snake.cpp"
//     #include "../core.h"
//     void nh_register(void) { nucleo_register_snake(); }
//     void nh_scenarios(const char *which, uint32_t seed) { ... }
//
// What it provides, mirroring the device (see firmware/components/nucleo_app/nucleo_app.cpp):
//   - the app framework slice the games use (handlers, hint, fullscreen, content height, APP_RAM alloc/free)
//   - key routing as the run loop does it (LEFT/BACK -> back handler, TAB -> tab handler, the rest -> on_key)
//   - the keyboard driver's auto-repeat (350 ms, then every 90 ms) for ; . , / and DEL, physically-held keys
//     (nucleo_kbd_char_down) and modifier state (nucleo_kbd_mods)
//   - a ~50 Hz loop with nh_jitter on a virtual clock; every frame composites into the shared 8bpp canvas
//   - the SD under a sandbox folder (the cwd): "/sd/..." -> "sd/..."
//   - board knobs: g_nh.adv (ADV vs original), g_nh.imu (tilt sensor present + tilt values), g_nh.lang
// The game's `d` draw-target macro (app_gfx.h) is live in this file: nothing here is named `d`.
#pragma once
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <string>
#include <M5GFX.h>
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
extern "C" {
#include "esp_err.h"
#include "nucleo_audio.h"
#include "nucleo_pnet.h"
#include "esp_timer.h"
#include "esp_random.h"
}

#ifndef NH_GAME_NAME
#define NH_GAME_NAME "game"
#endif

struct NhKnobs {
    bool adv = false;                 // Cardputer ADV (nucleo_ui_is_adv)
    bool imu = false;                 // BMI270 tilt sensor present (ADV only on real hardware)
    float tilt_x = 0, tilt_y = 0;     // nucleo_imu_tilt output
    const char *lang = "it";          // system language (nucleo_i18n)
    bool audio_playing = false;       // what nucleo_audio_is_playing reports
};
static NhKnobs g_nh;

// ---- virtual clock + deterministic RNG --------------------------------------------------------------
static int64_t nh_us = 5000000;
extern "C" int64_t esp_timer_get_time(void) { return nh_us; }
static uint32_t nh_rng = 0x9E3779B9u;
extern "C" uint32_t esp_random(void) { nh_rng ^= nh_rng << 13; nh_rng ^= nh_rng >> 17; nh_rng ^= nh_rng << 5; return nh_rng; }
static uint32_t nh_jit_rng = 12345;
static int nh_jitter(int n) { nh_jit_rng = nh_jit_rng * 1103515245u + 12345u; return n > 0 ? (int)((nh_jit_rng >> 8) % (unsigned)n) : 0; }

// ---- sandboxed file system --------------------------------------------------------------------------
#undef fopen
#undef stat
#undef mkdir
#undef remove
#undef rename
static void nh_path(const char *p, char *out, size_t n) { while (*p == '/') p++; snprintf(out, n, "%s", p); }
extern "C" FILE *host_fopen(const char *p, const char *m) { char q[256]; nh_path(p, q, sizeof q); return fopen(q, m); }
extern "C" int host_stat(const char *p, struct stat *st) { char q[256]; nh_path(p, q, sizeof q); return stat(q, st); }
extern "C" int host_mkdir(const char *p) { char q[256]; nh_path(p, q, sizeof q); return _mkdir(q); }
extern "C" int host_remove(const char *p) { char q[256]; nh_path(p, q, sizeof q); return remove(q); }
extern "C" int host_rename(const char *a, const char *b) { char q[256], r[256]; nh_path(a, q, sizeof q); nh_path(b, r, sizeof r); remove(r); return rename(q, r); }

// ---- display: a 16bpp "panel" + the 8bpp shared canvas every buffered frame composites into ------------
static M5GFX nh_panel;
static M5Canvas nh_canvas(&nh_panel);
static LovyanGFX *nh_target = &nh_panel;
LovyanGFX *nucleo_app_gfx(void) { return nh_target; }
bool nucleo_app_is_buffered(void) { return nh_target != &nh_panel; }
void nucleo_app_set_gfx(LovyanGFX *g) { nh_target = g ? g : &nh_panel; }
M5Canvas *nucleo_screen(void) { return &nh_canvas; }
bool nucleo_screen_acquire(void) { return true; }
void nucleo_screen_release(void) {}

// ---- app framework --------------------------------------------------------------------------------------
static const nucleo_app_def_t *nh_app;
static bool (*nh_back)(int);
static void (*nh_tab)(void);
static bool (*nh_poll)(void);
static void (*nh_ptt)(bool);
static bool nh_dirty, nh_full, nh_exit, nh_direct;
static char nh_hint[200];
void nucleo_app_register(const nucleo_app_def_t *a) { nh_app = a; }
void nucleo_app_set_back_handler(bool (*fn)(int)) { nh_back = fn; }
void nucleo_app_set_tab_handler(void (*fn)(void)) { nh_tab = fn; }
void nucleo_app_set_poll_handler(bool (*fn)(void)) { nh_poll = fn; }
void nucleo_app_set_ptt_handler(void (*fn)(bool)) { nh_ptt = fn; }
void nucleo_app_request_draw(void) { nh_dirty = true; }
void nucleo_app_set_fullscreen(bool on) { nh_full = on; }
void nucleo_app_set_hint(const char *h) { snprintf(nh_hint, sizeof nh_hint, "%s", h ? h : ""); }
void nucleo_app_set_hint_colors(unsigned short, unsigned short) {}
int  nucleo_app_content_height(void) { return nh_full ? H : (H - HINT); }
int  nucleo_app_content_top(void) { return 0; }
void nucleo_app_exit(void) { nh_exit = true; }
void nucleo_app_set_direct_draw(bool on) { nh_direct = on; }
void nucleo_app_set_keep_awake(bool) {}
void nucleo_app_request_full_redraw(void) { nh_dirty = true; }
bool nucleo_ui_is_adv(void) { return g_nh.adv; }

// ---- system language (nucleo_i18n) ------------------------------------------------------------------------
#include "nucleo_i18n.h"
static int nh_lang_ix(void) { const char *l = g_nh.lang; return !strcmp(l, "en") ? 1 : !strcmp(l, "es") ? 2 : !strcmp(l, "fr") ? 3 : !strcmp(l, "de") ? 4 : 0; }
bool nucleo_i18n_is_en(void) { return nh_lang_ix() != 0; }
const char *nucleo_i18n_lang(void) { return g_nh.lang; }
const char *nucleo_tr(const char *it, const char *en) { return nh_lang_ix() ? en : it; }
const char *nucleo_tr5(const char *it, const char *en, const char *es, const char *fr, const char *de) {
    switch (nh_lang_ix()) { case 1: return en; case 2: return es ? es : en; case 3: return fr ? fr : en; case 4: return de ? de : en; }
    return it;
}
uint32_t nucleo_i18n_gen(void) {                     // bumps when a scenario switches g_nh.lang, like a live OS change
    static const char *last; static uint32_t gen;
    if (g_nh.lang != last) { last = g_nh.lang; gen++; }
    return gen;
}

// ---- audio (counted, never played) ------------------------------------------------------------------------
static int nh_tones, nh_plays;
extern "C" {
esp_err_t nucleo_audio_play(const char *) { nh_plays++; return ESP_OK; }
void nucleo_audio_tone(int, int, int) { nh_tones++; }
void nucleo_audio_blip(int, int) { nh_tones++; }
void nucleo_audio_stop(void) {}
bool nucleo_audio_is_playing(void) { return g_nh.audio_playing; }
bool nucleo_audio_playing(void) { return g_nh.audio_playing; }
int  nucleo_audio_volume(void) { return 80; }
void nucleo_audio_set_volume(int) {}
}

// ---- tilt sensor ------------------------------------------------------------------------------------------
#include "nucleo_imu.h"
static nk_motion_t nh_motion;                         // what nucleo_imu_motion reports (a scenario can "shake")
extern "C" {
nk_motion_t nucleo_imu_motion(void) { nk_motion_t m = nh_motion; nh_motion = (nk_motion_t)0; return m; }
bool nucleo_imu_present(void) { return g_nh.imu; }
bool nucleo_imu_tilt(float *tx, float *ty) { if (!g_nh.imu) return false; if (tx) *tx = g_nh.tilt_x; if (ty) *ty = g_nh.tilt_y; return true; }
void nucleo_imu_recenter(void) {}
bool nucleo_imu_sample(void) { return g_nh.imu; }
bool nucleo_imu_gravity_screen(float *gx, float *gy, float *gz) { if (!g_nh.imu) return false; if (gx) *gx = g_nh.tilt_x; if (gy) *gy = g_nh.tilt_y; if (gz) *gz = 1; return true; }
float nucleo_imu_energy(void) { return 0; }
}

// ---- web endpoints a game registers (kept so a scenario can call them), auth always paired ---------------
#include "esp_http_server.h"
static httpd_uri_t nh_uris[8]; static int nh_nuris;
extern "C" {
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *u) { if (nh_nuris < 8) nh_uris[nh_nuris++] = *u; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *) { return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { snprintf(r->status, sizeof r->status, "%s", s); return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, long len) {
    if (!b) return ESP_OK; size_t n = len < 0 ? strlen(b) : (size_t)len;
    if (r->out_len + n >= sizeof r->out) n = sizeof r->out - 1 - r->out_len;
    memcpy(r->out + r->out_len, b, n); r->out_len += n; r->out[r->out_len] = 0; return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send_chunk(r, s, -1); }
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s) { return httpd_resp_send_chunk(r, s, -1); }
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len) {
    size_t left = r->content_len - r->body_off; if (!r->body || !left) return 0; if (len > left) len = left;
    memcpy(buf, r->body + r->body_off, len); r->body_off += len; return (int)len;
}
size_t httpd_req_get_url_query_len(httpd_req_t *r) { const char *q = r->uri ? strchr(r->uri, '?') : nullptr; return q ? strlen(q + 1) : 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len) { const char *q = r->uri ? strchr(r->uri, '?') : nullptr; if (!q) return ESP_FAIL; snprintf(buf, len, "%s", q + 1); return ESP_OK; }
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len) {
    size_t kl = strlen(key);
    for (const char *p = qry; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') { const char *v = p + kl + 1, *e = strchr(v, '&'); size_t n = e ? (size_t)(e - v) : strlen(v); if (n >= len) n = len - 1; memcpy(val, v, n); val[n] = 0; return ESP_OK; }
        p = strchr(p, '&'); if (p) p++;
    }
    return ESP_FAIL;
}
bool nucleo_auth_request_ok(httpd_req_t *) { return true; }
esp_err_t nucleo_auth_reject(httpd_req_t *) { return ESP_FAIL; }
void nucleo_anima_l1_unload(void) {}
}

// ---- radio: single device, nobody in range ----------------------------------------------------------------
extern "C" {
bool pnet_start(void) { return true; }
void pnet_stop(void) {}
const char *pnet_name(void) { return "host"; }
int  pnet_channel(void) { return 1; }
int  pnet_send(const uint8_t *, const void *, int) { return 0; }
bool pnet_recv(pnet_pkt_t *) { return false; }
}

// ---- keyboard ---------------------------------------------------------------------------------------------
static bool nh_down[128];
static unsigned char nh_mods;
bool nucleo_kbd_char_down(char c) { return (unsigned char)c < 128 && nh_down[(unsigned char)c]; }
unsigned char nucleo_kbd_mods(void) { return nh_mods; }
static char nh_held = 0; static int nh_held_nk = 0; static int64_t nh_rep_us = 0;
static bool nh_repeats(char c) { return c == ';' || c == '.' || c == ',' || c == '/' || c == '\b'; }
static int nh_nk(char ch) {
    switch (ch) { case ';': return NK_UP; case '.': return NK_DOWN; case ',': return NK_LEFT; case '/': return NK_RIGHT;
                  case '\n': return NK_ENTER; case '`': return NK_BACK; case '\t': return NK_TAB; case '\b': return NK_DEL; }
    return NK_CHAR;
}
static void nh_route(int nk, char ch) {                // nucleo_app.cpp's routing for a foreground app
    if (nk == NK_TAB) { if (nh_tab) nh_tab(); return; }
    if (nk == NK_LEFT || nk == NK_BACK) { if (nh_back && nh_back(nk)) return; if (nk == NK_BACK) { nh_exit = true; return; } }
    if (nh_app->on_key) nh_app->on_key(nk, ch);
}
static void nh_key_down(char ch) { nh_down[(unsigned char)ch] = true; nh_held = ch; nh_held_nk = nh_nk(ch); nh_rep_us = nh_us + 350000; nh_route(nh_held_nk, ch); }
static void nh_key_up(char ch) { nh_down[(unsigned char)ch] = false; if (nh_held == ch) nh_held = 0; }

// ---- reporting --------------------------------------------------------------------------------------------
static int nh_nfail, nh_nchecks;
static void nh_fail(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("FAIL "); vprintf(fmt, a); printf("\n"); va_end(a); nh_nfail++; nh_nchecks++; }
static void nh_check(bool ok, const char *fmt, ...) { nh_nchecks++; if (ok) return; va_list a; va_start(a, fmt); printf("FAIL "); vprintf(fmt, a); printf("\n"); va_end(a); nh_nfail++; }
static void nh_note(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  "); vprintf(fmt, a); printf("\n"); va_end(a); }

// ---- the run loop -----------------------------------------------------------------------------------------
static int nh_frames, nh_draws;
static int nh_draw_every = 5;                           // draw every Nth frame (1 = every frame): draw code runs on live state
static int nh_tick_ms = 18, nh_tick_jit = 10;            // the device loop: ~20 ms + nh_jitter
static void nh_hint_bar(void) {                        // launcher_render_hint_bar: centred, cut at 39 chars
    if (nh_full) return;
    int y = H - HINT, len = (int)strlen(nh_hint); if (len > 39) len = 39;
    nh_canvas.fillRect(0, y, W, HINT, 0x0000); nh_canvas.drawFastHLine(0, y, W, 0x4208);
    char b[40]; memcpy(b, nh_hint, len); b[len] = 0;
    int x = (W - len * 6) / 2; if (x < 4) x = 4;
    nh_canvas.setTextSize(1); nh_canvas.setTextColor(0xFFFF, 0x0000); nh_canvas.setCursor(x, y + 3); nh_canvas.print(b);
}
static void nh_draw_frame(void) {
    nucleo_app_set_gfx(&nh_canvas);
    if (nh_app->on_draw) nh_app->on_draw();
    nucleo_app_set_gfx(nullptr);
    nh_hint_bar();
    nh_draws++;
}
static int nh_tick_n;
static void nh_tick(int ms) {
    nh_us += (int64_t)ms * 1000;
    if (nh_held && nh_repeats(nh_held) && nh_us >= nh_rep_us) { nh_rep_us = nh_us + 90000; nh_route(nh_held_nk, nh_held); }
    bool want = nh_poll && nh_poll();
    if (nh_app->on_tick && (++nh_tick_n % 10) == 0) nh_app->on_tick();     // ~5 Hz like the run loop
    if (want || nh_dirty) { nh_dirty = false; nh_frames++; if ((nh_frames % nh_draw_every) == 0) nh_draw_frame(); }
}
static void nh_loop_ms(int ms) { int64_t end = nh_us + (int64_t)ms * 1000; while (nh_us < end && !nh_exit) nh_tick(nh_tick_ms + nh_jitter(nh_tick_jit)); }
static bool nh_run_until(bool (*cond)(void), int timeout_ms) {
    int64_t end = nh_us + (int64_t)timeout_ms * 1000;
    while (nh_us < end && !nh_exit) { if (cond()) return true; nh_tick(nh_tick_ms + nh_jitter(nh_tick_jit)); }
    return cond();
}
static void nh_tap(char ch, int hold_ms = 60, int after_ms = 60) { nh_key_down(ch); nh_loop_ms(hold_ms); nh_key_up(ch); nh_loop_ms(after_ms); }
static void nh_hold(char ch, int ms) { nh_key_down(ch); nh_loop_ms(ms); nh_key_up(ch); nh_loop_ms(40); }
static void nh_mod_pulse(unsigned char m, int ms = 80) { nh_mods = m; nh_loop_ms(ms); nh_mods = 0; nh_loop_ms(60); }

// Random key mashing: the keys a player can press, weighted toward the game controls. Never Esc (that
// leaves the app) unless allow_back; the scenario decides when to leave.
static void nh_fuzz(int ms, bool allow_back = false) {
    static const char keys[] = ";;;...,,,///\n\n   wasdqeWWzxcvbnm123456789\t\b";
    int64_t end = nh_us + (int64_t)ms * 1000;
    while (nh_us < end && !nh_exit) {
        char ch = keys[nh_jitter((int)sizeof keys - 1)];
        if (allow_back && nh_jitter(40) == 0) ch = '`';
        if (ch == '\t' && nh_jitter(3)) ch = ' ';
        nh_key_down(ch); nh_loop_ms(20 + nh_jitter(300)); nh_key_up(ch); nh_loop_ms(nh_jitter(200));
    }
}

static void nh_dump(const char *name) {
    nh_draw_frame();
    char p[256]; snprintf(p, sizeof p, "shots/%s_%s.rgb", NH_GAME_NAME, name);
    FILE *f = fopen(p, "wb"); if (!f) { nh_fail("cannot write %s", p); return; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t c = (uint16_t)nh_canvas.readPixel(x, y);
        unsigned char px[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
// Hint-bar text the footer would cut (it shows 39 chars).
static void nh_check_hint(const char *where) { nh_check(strlen(nh_hint) <= 39, "%s: hint is %d chars, the footer shows 39: '%s'", where, (int)strlen(nh_hint), nh_hint); }

// ---- app lifecycle ----------------------------------------------------------------------------------------
static void nh_open_app(uint32_t seed) {
    nh_rng = seed ? seed : 1; nh_jit_rng = seed * 7 + 1;
    for (const nucleo_app_ram_t *r = nh_app->ram; r && r->ptr; r++) *r->ptr = calloc(1, r->bytes);
    nh_back = nullptr; nh_tab = nullptr; nh_poll = nullptr; nh_ptt = nullptr; nh_exit = false; nh_full = false; nh_direct = false;
    memset(nh_down, 0, sizeof nh_down); nh_held = 0; nh_mods = 0; nh_hint[0] = 0;
    nh_canvas.fillScreen(0);
    if (nh_app->on_enter) nh_app->on_enter();
}
static void nh_close_app(void) {
    if (nh_app->on_exit) nh_app->on_exit();
    for (const nucleo_app_ram_t *r = nh_app->ram; r && r->ptr; r++) { free(*r->ptr); *r->ptr = nullptr; }
}

void nh_register(void);
void nh_scenarios(const char *which, uint32_t seed);

int main(int argc, char **argv) {
    nh_panel.setColorDepth(16); nh_panel.createSprite(W, H);
    nh_canvas.setColorDepth(8); nh_canvas.createSprite(W, H);
    _mkdir("shots"); _mkdir("sd"); _mkdir("sd/data");
    nh_register();
    if (!nh_app) { printf("FAIL %s: nothing registered\n", NH_GAME_NAME); return 1; }
    const char *which = argc > 1 ? argv[1] : "all";
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 7;
    nh_scenarios(which, seed);
    printf("%s: %d checks, %d failed (%d frames, %d drawn)\n", NH_GAME_NAME, nh_nchecks, nh_nfail, nh_frames, nh_draws);
    return nh_nfail ? 1 : 0;
}
