// App framework for the Cardputer: a registry of single-foreground native apps + the run
// loop that drives the Wear OS-style launcher. Keyboard-only.
//
// This file is intentionally thin — it orchestrates, it doesn't draw. The pieces live in:
//   launcher_theme.h    shared palette + screen geometry
//   launcher_menu.*     the menu tree + navigation (mirror of web/device/nav.js)
//   launcher_render.*   chrome, the animated list band (the tearing fix), overlays
//   app_info.cpp        built-in Connection app      app_remote.cpp  built-in Remote app
//   app_*.cpp           one file per native app (clock, files, player, ...)
//
// NOTE: M5GFX UI — verify on hardware. Assumes nucleo_ui_init() already began the display.
#include "nucleo_app.h"
#include "ota_listen_fsm.h"    // pure OTA listening-handshake decision core (host-tested)
#include "nucleo_exclusive.h"  // declarative exclusive_flags: framework enters on open, restores on close
#include "launcher_theme.h"
#include "launcher_menu.h"
#include "launcher_render.h"
#include "gamefront.h"
#include <M5GFX.h>
#include "nucleo_kbd.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"     // remote smartwatch monitor: live internal-SRAM free/used
#include <time.h>              // header clock on the watch faces
#include "driver/gpio.h"
extern "C" {
#include "nucleo_storage.h"    // SD free/used for the Storage face (REQUIRES already lists it)
#include "nucleo_ui.h"         // nucleo_ui_panel_size / read_row for the /api/screen readback
#include "nucleo_voice.h"      // Voice engine state for the PTT overlay
#include "nucleo_anima.h"      // anima_action_t for the voice result toast (include dir via CMakeLists)
}
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"         // esp_restart for the ANIMA Solo reboot
#include "esp_attr.h"           // RTC_NOINIT_ATTR for the Solo reboot flag
#include "nucleo_auth.h"        // NUCLEO_AUTH_GUARD (include dir pulled in via CMakeLists)

#include "app_gfx.h"

// Network info (resolved at link; declared here for the stub screen, no component dep).
extern "C" const char *nucleo_setup_mode(void);
extern "C" const char *nucleo_setup_ip(void);

// Panel backlight (implemented in nucleo_ui). Wrapped below so brightness has one owner.
extern "C" void nucleo_ui_set_brightness(unsigned char b);

// ─── ANIMA Solo personality (see nucleo_app.h) ───────────────────────────────
// Warm-reset flag in RTC no-init RAM: survives esp_restart(), cleared by a cold power-on so a
// power-cycle can never strand the device in Solo. Mirrors nucleo_usbmsc's reboot-flag pattern.
// Solo (dedicated-boot) personality: a heavy-TLS native app reboots into a stripped boot that NEVER
// starts httpd/mDNS/recorder/auth/IR (+ TTS/voice for the recorder profile) so the heap comes up large
// and UNFRAGMENTED — the only way this no-PSRAM chip fits a cloud-TLS handshake (mbedTLS+AES need a big
// CONTIGUOUS block that the runtime NX_NET_APP reclaim frees but can't DEFRAGMENT). The low byte of the
// magic carries WHICH app to open: 1=ANIMA, 2=Recorder. Legacy ANIMA_SOLO_MAGIC stays for back-compat.
#define ANIMA_SOLO_MAGIC 0xA11A5010u
#define SOLO_MAGIC_BASE  0x5010A100u           // generic: (BASE | app-code) in the low byte
enum { SOLO_NONE = 0, SOLO_ANIMA = 1, SOLO_RECORDER = 2, SOLO_APPID = 3 };  // 3 = open the app named in s_solo_id
RTC_NOINIT_ATTR static uint32_t s_solo_req;
RTC_NOINIT_ATTR static char     s_solo_id[24];   // SOLO_APPID target app id (e.g. "slots") — survives the warm reboot
RTC_NOINIT_ATTR static char     s_solo_open_file[256];   // "open with" path for a Solo target (Files -> .nfv): rides the warm reboot too
static char s_open_file[256] = "";               // "open with" handoff: Files passes the exact file; the viewer's on_enter consumes it
static bool s_solo_active = false;
static int  s_solo_app = SOLO_NONE;
extern "C" bool nucleo_recorder_is_busy(void);   // mic lifecycle: true until the WAV is fully closed + flushed

// Bounded, WDT-fed wait for the mic to FULLY release (WAV finalized) before a reboot — a Solo reboot must
// NEVER outrace record_task's fflush/fseek/write_header/fclose or it corrupts the FAT + the take.
static void wait_recorder_idle_then_restart(void)
{
    for (int i = 0; i < 100 && nucleo_recorder_is_busy(); i++) {
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));               // <= 2 s; record_task drains + finalizes in ~200 ms
    }
    esp_restart();                                    // NEVER returns
}
extern "C" void nucleo_anima_solo_request(void) { s_solo_req = ANIMA_SOLO_MAGIC; wait_recorder_idle_then_restart(); }   // legacy = ANIMA
extern "C" void nucleo_app_solo_request(int app) { s_solo_req = SOLO_MAGIC_BASE | (app & 0xFF); wait_recorder_idle_then_restart(); }
// Reboot into Solo with an ARBITRARY native app as the target (heavy games flagged NX_SOLO). The id rides
// in RTC_NOINIT across the warm reboot; the fresh boot brings up ONLY display+SD+power and opens it on an
// unfragmented heap. Esc closes the app -> close_app() warm-reboots back to the full OS (same as ANIMA Solo).
extern "C" void nucleo_app_solo_request_id(const char *id)
{
    snprintf(s_solo_id, sizeof s_solo_id, "%s", (id && id[0]) ? id : "");
    s_solo_req = SOLO_MAGIC_BASE | SOLO_APPID;
    wait_recorder_idle_then_restart();   // never returns
}
extern "C" bool nucleo_anima_solo_pending(void)
{
    // Honor the Solo latch ONLY on a clean software reboot (our esp_restart). A COLD boot / brownout leaves
    // RTC_NOINIT as garbage (could match a magic -> spurious Solo), and a PANIC/WDT during Solo would else
    // re-enter Solo -> reboot loop. ESP_RST_SW gates both: a crash lands in the FULL OS, deterministically.
    if (esp_reset_reason() != ESP_RST_SW) { s_solo_req = 0; return false; }
    if (s_solo_req == ANIMA_SOLO_MAGIC) { s_solo_req = 0; s_solo_active = true; s_solo_app = SOLO_ANIMA; }   // consume once, latch
    else if ((s_solo_req & 0xFFFFFF00u) == SOLO_MAGIC_BASE) { s_solo_app = (int)(s_solo_req & 0xFF); s_solo_req = 0; s_solo_active = true; }
    return s_solo_active;
}
extern "C" bool nucleo_anima_solo_active(void) { return s_solo_active; }
extern "C" int  nucleo_app_solo_app(void) { return s_solo_app; }   // SOLO_ANIMA / SOLO_RECORDER while active
// Per-profile policy (owned here, not hardcoded in main.c): only ANIMA Solo needs speech (TTS + voice/PTT).
// The Recorder Solo (and any future non-speech profile) skips them to free heap for the cloud TLS.
extern "C" bool nucleo_app_solo_needs_speech(void) { return s_solo_app == SOLO_ANIMA; }
// Generic Solo target (a heavy game via NX_SOLO, not ANIMA/Recorder): main.c registers ALL builtins in this
// case so the run loop's launch_id(s_solo_id) finds the game (registration is just pointers — cheap; the
// RAM win is from skipping httpd/mDNS, not from registering fewer apps). No speech/TTS brought up.
extern "C" bool nucleo_app_solo_is_generic(void) { return s_solo_app == SOLO_APPID; }

// Reopen-after-Solo: the Recorder Solo job warm-reboots back to the FULL OS; this RTC flag (set right
// before that reboot) tells the full-OS launcher to open the Recorder straight away so the user lands on
// the freshly-saved transcript/summary instead of a bare launcher. Consumed once, warm-reboot only.
#define REOPEN_RECORDER_MAGIC 0x5EC0DE02u
RTC_NOINIT_ATTR static uint32_t s_reopen_req;
extern "C" void nucleo_app_request_reopen_recorder(void) { s_reopen_req = REOPEN_RECORDER_MAGIC; }
static bool reopen_recorder_pending(void) { return esp_reset_reason() == ESP_RST_SW && s_reopen_req == REOPEN_RECORDER_MAGIC; }

// ---- shared display brightness ----------------------------------------------
static int s_brightness = 100;                 // percent; the Control Center + video share this
void nucleo_app_set_brightness(int pct)
{
    if (pct < 10)  pct = 10;                    // never let the screen go fully dark
    if (pct > 100) pct = 100;
    s_brightness = pct;
    nucleo_ui_set_brightness((unsigned char)(pct * 255 / 100));
}
int nucleo_app_brightness(void) { return s_brightness; }

// ---- HTTP: remote display power --------------------------------------------
// POST /api/display?on=0|1 — blank the Cardputer screen (backlight 0) or re-light it (restore
// the user's brightness). nucleo_app owns the brightness state, so it hosts this endpoint;
// nucleo_httpd registers it at boot. "off" drives the panel to 0 directly (bypassing the 10%
// floor in nucleo_app_set_brightness) WITHOUT changing s_brightness, so "on" can restore it.
// Lets a web app (the live-dictation app) keep the screen dark during use and guarantee it
// comes back on exit. The async mic stream keeps the server responsive, so this works mid-stream.
//
// DEADMAN SAFETY NET: turning the screen off arms a one-shot timer. The web app heartbeats
// (re-sends on=0) every ~45 s while it wants the screen dark; if those stop — the tab was
// closed, crashed, or lost the network — the timer fires and restores the screen on its own.
// So the display is GUARANTEED to come back even if the client never sends the on=1 on exit.
#define DISP_DEADMAN_US  (90LL * 1000 * 1000)   // 90 s without a heartbeat -> auto-restore
static esp_timer_handle_t s_disp_deadman = NULL;
static bool s_disp_off = false;
static bool s_disp_sleep = false;            // idle screen-off active (backlight 0 + 32 KB framebuffer freed). Declared
                                             // here (not with the other web-focus flags below) because display_post and
                                             // the deadman — which run on the httpd / esp_timer tasks — must yield to it.
static volatile bool s_disp_wake_req = false; // httpd-task -> launcher-task wake request: display_post(on=1) sets it when
                                             // idle screen-off had frozen the panel; the run loop consumes it and
                                             // re-acquires the 32 KB canvas (canvas/GFX ops stay on their single owner).

static void disp_deadman_cb(void *arg)
{
    (void)arg;
    if (s_disp_off) {                            // client went silent while the screen was dark
        s_disp_off = false;                      // release the web client's hold either way
        if (!s_disp_sleep)                       // if idle screen-off owns the panel, let display_wake restore it
            nucleo_app_set_brightness(s_brightness);  // (don't light the backlight over a freed/blank canvas)
        ESP_LOGW("display", "deadman: client silent, screen restored");
    }
}

static esp_err_t display_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char q[48] = {0}, on[8] = {0};
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK)
        httpd_query_key_value(q, "on", on, sizeof on);

    bool turn_on = (on[0] != '0');                          // default ON unless explicitly "0"
    if (turn_on) {
        s_disp_off = false;
        if (s_disp_deadman) esp_timer_stop(s_disp_deadman); // ESP_ERR_INVALID_STATE if idle: ignore
        // If the device had already drifted into idle screen-off (backlight 0 AND the 32 KB canvas freed),
        // re-lighting the backlight here alone would only show a frozen/stale frame. Hand the wake to the
        // launcher task (sole owner of the canvas) via a one-shot flag; it re-acquires the buffer + repaints.
        if (s_disp_sleep) s_disp_wake_req = true;
        else nucleo_app_set_brightness(s_brightness);       // reapply the user's stored level
    } else {
        nucleo_ui_set_brightness(0);                        // backlight fully off (no UI visible)
        s_disp_off = true;
        if (!s_disp_deadman) {
            esp_timer_create_args_t a = { .callback = disp_deadman_cb, .name = "disp_dead" };
            esp_timer_create(&a, &s_disp_deadman);
        }
        if (s_disp_deadman) { esp_timer_stop(s_disp_deadman); esp_timer_start_once(s_disp_deadman, DISP_DEADMAN_US); }
    }

    httpd_resp_set_type(req, "application/json");
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":true,\"on\":%s,\"brightness\":%d}",
             turn_on ? "true" : "false", s_brightness);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// GET /api/screen — stream the current shared UI canvas as a 24-bit BMP, so a paired client can grab
// the EXACT native screen over Wi-Fi. The canvas (RAM) is the trustworthy source — the ST7789 panel's
// own readback is byte-swapped (handled below). Reads the PHYSICAL PANEL, not the off-screen canvas,
// so it works whatever the device is doing: on this no-PSRAM chip the 32 KB canvas is usually NOT
// allocated (apps draw direct-to-panel to save RAM), so a canvas read would almost always be empty.
// Streamed row-by-row (one scanline buffer) → ~no heap.
static inline void scr_put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static inline void scr_put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static esp_err_t screen_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    int w = 0, h = 0;
    nucleo_ui_panel_size(&w, &h);
    if (w <= 0 || h <= 0 || w > 320) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"panel size unavailable\"}");
        return ESP_OK;
    }
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t imgSize = (uint32_t)rowSize * h;
    uint8_t hdr[54]; memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M'; scr_put32(hdr + 2, 54 + imgSize); scr_put32(hdr + 10, 54);
    scr_put32(hdr + 14, 40); scr_put32(hdr + 18, w); scr_put32(hdr + 22, h);
    scr_put16(hdr + 26, 1); scr_put16(hdr + 28, 24); scr_put32(hdr + 38, 2835); scr_put32(hdr + 42, 2835);
    httpd_resp_set_type(req, "image/bmp");
    if (httpd_resp_send_chunk(req, (const char *)hdr, 54) != ESP_OK) return ESP_FAIL;
    static uint16_t prow[320];                // one panel scanline, byte-swapped RGB565
    static uint8_t orow[320 * 3 + 4];
    for (int y = h - 1; y >= 0; y--) {        // BMP rows are bottom-up
        if (!nucleo_ui_read_row(y, w, prow)) memset(prow, 0, (size_t)w * 2);
        memset(orow, 0, rowSize);
        for (int x = 0; x < w; x++) {
            uint16_t raw = prow[x];
            uint16_t p = (uint16_t)((raw >> 8) | (raw << 8));   // un-swap the panel's readback byte order
            uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)(( p        & 0x1F) * 255 / 31);
            orow[x * 3 + 0] = b; orow[x * 3 + 1] = g; orow[x * 3 + 2] = r;   // BMP is BGR
        }
        if (httpd_resp_send_chunk(req, (const char *)orow, rowSize) != ESP_OK) return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

extern "C" esp_err_t nucleo_app_register_display(httpd_handle_t server)
{
    // Single-line designated init (fields in declaration order) so the api-spec generator
    // (tools/gen-api-spec.mjs) discovers the route from this source line.
    httpd_uri_t display = { .uri = "/api/display", .method = HTTP_POST, .handler = display_post };
    esp_err_t e = httpd_register_uri_handler(server, &display);
    httpd_uri_t screen = { .uri = "/api/screen", .method = HTTP_GET, .handler = screen_get };
    httpd_register_uri_handler(server, &screen);
    return e;
}

// ---- registered foreground apps --------------------------------------------
// Cap dimensionato sul numero reale di app (35) + margine; static array (no heap), deve combaciare
// con launcher_menu.cpp. Era 64: spreco di .bss residente su HW senza PSRAM.
#define MAX_APPS 64
static nucleo_app_def_t s_apps[MAX_APPS];
static int s_app_count;

void nucleo_app_register(const nucleo_app_def_t *app)
{
    if (s_app_count >= MAX_APPS) return;
    // Discipline guard: native games are heavy (fx3d + WAV SFX) and must dedicate RAM for their whole
    // foreground life. A game registered without exclusive_flags starves the audio player task under
    // fragmentation — the "game is mute" class. Flag it at boot so a new game can't regress silently.
    if (app && app->category && app->exclusive_flags == 0 && !strcmp(app->category, "Games"))
        ESP_LOGW("applaunch", "game '%s' registered without exclusive_flags (RAM/audio may starve; use NX_NET_APP)", app->id ? app->id : "?");
    s_apps[s_app_count++] = *app;
}

// Read access for the menu builder (plain C++ linkage; see launcher_menu.cpp).
int                     nucleo_app_count(void) { return s_app_count; }
const nucleo_app_def_t *nucleo_app_at(int i)   { return &s_apps[i]; }

// Built-in apps (each registers itself).
extern "C" void nucleo_register_info(void);
extern "C" void nucleo_register_clock(void);
extern "C" void nucleo_register_chrono(void);         // app_chrono.cpp — Office: stopwatch + countdown timer
extern "C" void nucleo_register_sysmon(void);
extern "C" void nucleo_register_calc(void);
extern "C" void nucleo_register_recorder(void);
extern "C" void nucleo_register_files(void);
extern "C" void nucleo_register_photos(void);
extern "C" void nucleo_register_player(void);
extern "C" void nucleo_register_video(void);
extern "C" void nucleo_register_remote(void);
extern "C" void nucleo_register_notepad(void);
extern "C" void nucleo_register_usb(void);
extern "C" void nucleo_register_calendar(void);
extern "C" void nucleo_register_notify(void);
extern "C" void nucleo_register_radio(void);
extern "C" void nucleo_register_anima(void);
extern "C" void nucleo_register_usbkbd(void);
extern "C" void nucleo_register_keydeck(void);
extern "C" void nucleo_register_torch(void);
extern "C" void nucleo_register_evilportal(void);
extern "C" void nucleo_register_wifiatk(void);
extern "C" void nucleo_register_sniffer(void);
extern "C" void nucleo_register_beacon(void);
extern "C" void nucleo_register_ethernet(void);
extern "C" void nucleo_register_ble(void);
extern "C" void nucleo_register_payloads(void);
extern "C" void nucleo_register_weather(void);

extern "C" void nucleo_register_voice(void);
extern "C" void nucleo_register_voicelab(void);
extern "C" void nucleo_register_ssh(void);
extern "C" void nucleo_register_wifi(void);
extern "C" void nucleo_register_mail(void);   // SMTP mail client (Gmail/Outlook/Yahoo/iCloud)
extern "C" void nucleo_register_ir(void);
extern "C" void nucleo_register_link(void);   // "Vicino" — device-to-device file/command transfer
extern "C" void nucleo_register_swarm(void);  // "Sciame" — ESP-NOW swarm presence + ping (nucleo_mesh)
extern "C" void nucleo_register_micspec(void);
extern "C" void nucleo_register_qr(void);
extern "C" void nucleo_register_theme(void);   // app_theme.cpp — replaces the empty nucleo_setup.c stub
extern "C" void nucleo_register_reactor(void); // app_reactor.cpp — Games: reactor-management game
extern "C" void nucleo_register_constellations(void); // app_constellations.cpp — Games: space-trader
extern "C" void nucleo_register_sandgarden(void);     // app_sandgarden.cpp — Games: falling-sand garden
extern "C" void nucleo_register_slots(void);          // app_slots.cpp — Games: slot machine
extern "C" void nucleo_register_poker(void);          // app_poker.cpp — Games: 5-card draw video poker
extern "C" void nucleo_register_pinball(void);        // app_pinball.cpp — Games: portrait pinball
extern "C" void nucleo_register_pong(void);           // app_pong.cpp — Games: 1v1 networked Pong (ESP-NOW) + vs CPU
extern "C" void nucleo_register_tanks(void);          // app_tanks.cpp — Games: turn-based artillery (destructible terrain) vs CPU
extern "C" void nucleo_register_tankduel(void);       // app_tankduel.cpp — Games: top-down 1v1 arena (40×40 map, shop zones, ESP-NOW)
extern "C" void nucleo_register_brawler(void);        // app_brawler.cpp — Games: SCORRIBANDA noir belt-scroll beat'em up (+ ESP-NOW co-op)
extern "C" void nucleo_register_dice(void);           // app_dice.cpp — Games: 3D dice roll (shake / ENTER); IMU shake is ADV-only
extern "C" void nucleo_register_yahtzee(void);        // app_yahtzee.cpp — Games: Yahtzee a turni (hot-seat 1-4 + CPU), dadi 3D fx3d
extern "C" void nucleo_register_snake(void);          // app_snake.cpp — Games: Snake Duel 1v1 in rete (ESP-NOW) o vs AI, power-up
extern "C" void nucleo_register_vs(void);             // app_vs.cpp — Games: Orde, mini vampire-survivors (Solo boot)
extern "C" void nucleo_register_cardler(void);        // app_cardler.cpp — Games: Cardler, mini RPG (Solo boot)
extern "C" void nucleo_register_level(void);          // app_level.cpp — Hardware (ADV-only): BMI270 bubble level
extern "C" void nucleo_register_goniometer(void);     // app_goniometer.cpp — Hardware (ADV-only): angle finder
extern "C" void nucleo_register_pedometer(void);      // app_pedometer.cpp — Hardware (ADV-only): step counter
extern "C" void nucleo_register_alarm(void);          // app_alarm.cpp — Hardware (ADV-only): motion alarm / antifurto
extern "C" void nucleo_register_pixelfix(void);       // app_pixelfix.cpp — Tools: LCD pixel rehabilitation (6 full-screen patterns)
extern "C" void nucleo_register_screensaver(void);    // app_screensaver.cpp — Tools: salvaschermo animato + screen-off
extern "C" bool nucleo_screensaver_should_activate(int64_t idle_ms);  // app_screensaver.cpp hook per main loop
extern "C" void nucleo_screensaver_set_trigger(void);                 // app_screensaver.cpp hook per main loop
extern "C" bool nucleo_ui_is_adv(void);               // Cardputer ADV? (M5GFX board detect — robust, independent of IMU init)

// Registration order defines BOTH the category order in the root menu (by first app seen)
// and the app order within each category. Ordered so everyday apps lead and System/Connect
// trail — matching the web emulator (Tools, Media, System, Connect).
void nucleo_app_register_builtins(void)
{
    nucleo_register_anima();                                                    // hoisted to Home top-level (launcher_build_menu); its "Tools" category is ignored
    nucleo_register_clock(); nucleo_register_chrono(); nucleo_register_weather(); nucleo_register_torch(); nucleo_register_calc(); nucleo_register_qr(); nucleo_register_pixelfix(); nucleo_register_files();
    nucleo_register_calendar(); nucleo_register_notify(); nucleo_register_notepad(); nucleo_register_mail(); nucleo_register_usb(); nucleo_register_usbkbd(); nucleo_register_ir(); nucleo_register_alarm();  // alarm on BOTH boards (mic-only on non-ADV)
    nucleo_register_radio(); nucleo_register_player(); nucleo_register_video(); nucleo_register_photos(); nucleo_register_recorder(); nucleo_register_micspec();  // Media
    nucleo_register_reactor(); nucleo_register_constellations(); nucleo_register_sandgarden(); nucleo_register_slots(); nucleo_register_poker(); nucleo_register_pinball(); nucleo_register_pong(); nucleo_register_tanks(); nucleo_register_tankduel(); nucleo_register_brawler(); nucleo_register_dice(); nucleo_register_yahtzee(); nucleo_register_snake(); nucleo_register_vs(); nucleo_register_cardler();   // Games
    if (nucleo_ui_is_adv()) { nucleo_register_level(); nucleo_register_goniometer(); nucleo_register_pedometer(); }  // Hardware (ADV-only): BMI270 measuring tools
    nucleo_register_screensaver();                                                  // Tools: salvaschermo
    nucleo_register_info(); nucleo_register_sysmon(); nucleo_register_theme();    // System
    nucleo_register_wifi(); nucleo_register_remote(); nucleo_register_ssh(); nucleo_register_link(); nucleo_register_swarm(); nucleo_register_keydeck();  // Connect
    nucleo_register_voice(); nucleo_register_voicelab();                          // Voice Control + live console
    nucleo_register_evilportal(); nucleo_register_wifiatk(); nucleo_register_beacon(); nucleo_register_sniffer(); nucleo_register_ethernet(); nucleo_register_ble(); nucleo_register_payloads();  // Security (authorized testing)
}

// ---- app lifecycle ----------------------------------------------------------
static int s_active = -1;                  // index into s_apps, -2 = stub, -1 = launcher
static const MenuNode *s_stub = nullptr;   // app launched without a native impl
static bool s_dirty = true;                // animated list band needs a redraw
static bool s_chrome_dirty = true;         // status/hint chrome needs a redraw
static bool s_hint_dirty   = true;         // bottom hint bar needs a redraw (set on hint/color change)
static bool s_control_center = false;      // global Control Center overlay active
static bool s_wake_pending  = false;       // screen was blanked by CC "screen off" — first key restores
static int  s_wake_saved_bright = 80;      // brightness to restore on wake
static bool s_gamefront     = false;       // GameFront (HyperSpin-style game launcher) state active
static bool s_gf_return     = false;       // re-open GameFront when the game it launched closes
static bool s_shot_req       = false;      // Fn+P -> a real full-frame screenshot on the next composite
static bool s_cover_req      = false;      // in-game 'C' -> refresh this game's carousel cover on the next composite
static int64_t s_shot_toast_until = 0;     // ms deadline to keep the confirm toast overlaid (NON-blocking)
static bool s_shot_toast_ok  = false;      // did the last capture write succeed?
static bool s_torch = false;               // global flashlight overlay active (G0 quick-tap)
static int  s_torch_prev_bright = 100;     // brightness to restore when the torch overlay closes
static void (*s_app_tab)(void) = nullptr;  // foreground app's TAB handler (else Control Center)
static bool (*s_app_back)(int key) = nullptr; // foreground app's Back/Esc/Left interceptor (gets the key; else close_app)
static bool s_app_direct = false;          // app freed the shared canvas -> draw direct, don't re-acquire
static bool s_cc_canvas_borrowed = false;  // CC re-acquired the canvas from a direct-draw app; release on close
static bool s_app_fullscreen = false;      // app reclaims the hint-bar rows: blit clipped to H, footer not drawn
static void (*s_app_ptt)(bool) = nullptr;  // foreground app owns the GO-hold (recorder PTT); else voice PTT
static bool (*s_app_poll)(void) = nullptr; // live app's per-loop poll: returns true to request a blit (gates the redraw to the app's data rate, not the loop rate)
static bool s_app_excl   = false;          // framework entered exclusive for this app (declarative) -> it restores it

// Idle-reblit guard (ANTI-FLICKER, system-wide). The ST7789 has no vsync, so pushing a frame that is
// byte-identical to the one already on screen still costs an SPI transaction and can tear/shimmer — the
// "idle flicker" of any app whose poll() keeps returning true on a static screen (menus, game-over,
// paused, a held poker hand). We composite as usual, then SKIP the blit when the freshly drawn canvas
// hashes identical to what we last pushed. Genuine animation differs every frame, so it always blits;
// a still screen pushes once and then rests. s_fg_was_fg forces a blit the frame the app regains the
// screen from an overlay/launcher (the canvas may match a stale pre-overlay frame). One owner of the rule.
static uint32_t s_fg_last_hash = 0;
static bool     s_fg_was_fg    = false;
static uint32_t fg_canvas_hash(M5Canvas *cv)
{
    const uint8_t *b = (const uint8_t *)cv->getBuffer();
    if (!b) return 0;
    size_t n = (size_t)cv->width() * cv->height();          // shared canvas is 8bpp -> 1 byte/px, rows word-aligned
    const uint32_t *w = (const uint32_t *)b;
    uint32_t h = 2166136261u;                               // FNV-1a over 32-bit words (≈8 k iters for 240x135)
    for (size_t i = 0; i < n / 4; i++) { h ^= w[i]; h *= 16777619u; }
    for (size_t i = (n / 4) * 4; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// ANTI-FLICKER (technique 1, refined): the ST7789 has no vsync, so a full-frame blit during animation
// (recorder waveform @20 Hz, launcher smooth-scroll) shimmers/tears the WHOLE screen even where nothing
// changed (the big timer, the title, static rows). Push only the horizontal BANDS whose pixels actually
// changed since the last frame: the unchanged regions are never re-sent, so they can't tear. Same
// 8bpp/no-PSRAM canvas — just a smarter blit. `force` repushes everything (app just regained the screen,
// or a stale-panel frame the per-band hash can't see). Returns true if any band was pushed.
#define BLIT_BANDS 9                              // 135 rows / 9 = 15-px bands (whole-row word hashing is cheap)
static uint32_t s_band_hash[BLIT_BANDS];
static bool blit_dirty_bands(M5Canvas *cv, int clip_bottom, bool force)
{
    const uint8_t *base = (const uint8_t *)cv->getBuffer();
    if (!base) return false;
    const int wpx = cv->width(), hpx = cv->height();
    const int band = (hpx + BLIT_BANDS - 1) / BLIT_BANDS;
    bool pushed = false;
    for (int bi = 0; bi < BLIT_BANDS; bi++) {
        int y0 = bi * band, y1 = y0 + band;
        if (y1 > hpx) y1 = hpx;
        if (y0 >= y1 || y0 >= clip_bottom) break;
        int yb = (y1 < clip_bottom) ? y1 : clip_bottom;          // don't hash/push past the hint bar
        uint32_t h = 2166136261u;                                // FNV-1a over this band's rows
        const uint32_t *w = (const uint32_t *)(base + (size_t)y0 * wpx);
        for (size_t i = 0; i < (size_t)wpx * (yb - y0) / 4; i++) { h ^= w[i]; h *= 16777619u; }
        if (!h) h = 1;
        if (force || h != s_band_hash[bi]) {
            s_band_hash[bi] = h;
            d.setClipRect(0, y0, wpx, yb - y0);                  // clip restricts the push to THIS band's rows
            cv->pushSprite(0, 0);
            pushed = true;
        }
    }
    d.clearClipRect();
    return pushed;
}

static int find_app(const char *id) { for (int i = 0; i < s_app_count; i++) if (!strcmp(s_apps[i].id, id)) return i; return -1; }

// "Open this app in your browser" placeholder for apps with no native screen yet.
static void stub_draw(void)
{
    int top_y = nucleo_app_content_top();
    d.fillRect(0, top_y, W, nucleo_app_content_height(), BG);
    d.setTextSize(2); d.setTextColor(s_stub ? s_stub->color : C_BLUE, BG);
    d.setCursor(10, top_y + 8); d.print(s_stub ? s_stub->label : "");
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(10, top_y + 26); d.print(s_stub ? s_stub->desc : "");

    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];
    char url[48];
    if (sta) snprintf(url, sizeof(url), "%s", nucleo_setup_ip());
    else     snprintf(url, sizeof(url), "192.168.4.1");

    d.setTextColor(DIM, BG); d.setCursor(10, top_y + 48); d.print("Open this IP in your browser:");
    d.setTextSize(2); d.setTextColor(C_GREEN, BG); d.setCursor(10, top_y + 60); d.print(url);
}
static void stub_enter(void) { nucleo_app_set_hint("esc back"); }
static const nucleo_app_def_t STUB = { "stub", "", "System", "", ' ', C_GREY, stub_enter, nullptr, nullptr, stub_draw, nullptr };

// Free ANIMA L1's ~24 KB RAM working set (hot-row cache + directory; centroids stream from SD) when a
// foreground app opens — the assistant lives at the launcher, so while you're in any app that memory
// belongs to the app/audio decoder. The next
// assistant query transparently reloads the index from SD. Guarded (_if_idle): freeing it under the
// native worker's running query is a use-after-free; if busy we just skip the reclaim.
// (extern: no REQUIRES cycle on nucleo_anima.)
extern "C" bool nucleo_anima_l1_unload_if_idle(void);

static void open_app_def(const nucleo_app_def_t *def)
{
    // DIAG breadcrumbs (WARN -> visible in /api/logs): bracket on_enter with heap stats so a hang/OOM
    // during an app's entry (e.g. the 2nd launch-from-ANIMA freeze) shows up as an "enter" with no
    // matching "done" in the live log. Cheap; one line each side.
    ESP_LOGW("applaunch", "enter '%s' free=%u largest=%u", def && def->id ? def->id : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    nucleo_anima_l1_unload_if_idle();
    // Declarative exclusive mode: heavy apps that need dedicated RAM for their whole foreground life
    // (e.g. SSH) just set exclusive_flags; the framework suspends the requested subsystems here and
    // restores them in close_app(). Apps that want reclaim only during a heavy action keep flags=0 and
    // call nucleo_exclusive_enter/exit themselves (e.g. Video play, the Recorder's background AI worker
    // which OUTLIVES the app on purpose) — the framework must NOT tear those down, so we only restore
    // what WE entered, tracked by s_app_excl.
    // App-to-app launch() bypasses close_app, so first restore any exclusive WE hold for the outgoing
    // app — never strand suspended services on a direct switch.
    if (s_app_excl) { s_app_excl = false; nucleo_exclusive_exit(); }
    if (def && def->exclusive_flags) {
        bool was_active = nucleo_exclusive_active();   // a background worker (Recorder AI) may already hold it
        nucleo_exclusive_enter(def->exclusive_flags, NULL);   // idempotent: adds nothing if already active
        s_app_excl = !was_active;                      // own the restore ONLY if WE activated it (no false latch)
    }
    s_app_tab = nullptr;                        // each app starts without a TAB claim; on_enter may set one
    s_app_back = nullptr; s_app_direct = false; s_app_fullscreen = false; // ...nor a Back claim / direct-draw / fullscreen pin; on_enter may set them
    s_app_ptt = nullptr;                        // ...nor a GO-hold (PTT) claim; the recorder sets it on_enter
    s_app_poll = nullptr;                       // ...nor a per-loop data poll; a live app (mic spectrum) sets it on_enter
    launcher_render_reset_hint_colors();        // default dark footer; on_enter may override (e.g. Torch)
    d.fillRect(0, 0, W, H - HINT, BG);
    if (def->on_enter) def->on_enter();
    ESP_LOGW("applaunch", "enter '%s' DONE free=%u", def && def->id ? def->id : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    // Universal anti-flicker guarantee. Any app that draws BUFFERED (s_app_direct still false after on_enter)
    // MUST have the shared 32 KB canvas, or the run loop falls back to DIRECT draw and a full-frame/animated
    // repaint (tanks/pong/pinball/constellations/brawler... the whole-screen games) flickers. A preceding
    // media/voice app may have released the canvas, and the fragmented heap returns it only after the
    // subsystems exclusive_enter just suspended coalesce — so retry briefly (≤150 ms), exactly like close_app().
    // This is the single owner of the invariant: individual apps no longer need their own acquire, and it also
    // covers the app->app path (launch_by_id, e.g. ANIMA "apri tanks") which bypasses close_app's re-acquire.
    // Direct-draw apps (ANIMA/radio/video/SSH/recorder) opt OUT by setting s_app_direct in on_enter — they
    // released the RAM on purpose, so we must not fight them for it.
    if (!s_app_direct) for (int i = 0; i < 6 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(25));
    s_dirty = true; s_hint_dirty = true;        // paint the footer once on entry
    s_fg_was_fg = false;                        // force the first frame's blit (don't trust a stale hash)
}
// A game flagged NX_SOLO wants a FRESH unfragmented heap: reboot into Solo with it as the target instead of
// opening it inline on the running (fragmented) heap (the OOM-32KB-canvas / Task-WDT case). Skipped when
// ALREADY in Solo — the Solo boot's own open of the target must run for real, not reboot again (else loop).
static bool maybe_solo_launch(int idx)
{
    if (s_solo_active || idx < 0) return false;
    const nucleo_app_def_t *a = &s_apps[idx];
    // Every GAME launches in Solo (fresh, unfragmented heap) — the user wants games to behave like ANIMA/
    // Recorder: reboot in, full heap, Esc reboots back to the OS. The 32KB canvas + game buffers couldn't be
    // carved inline on the fragmented running heap (OOM / Task-WDT). Category covers all games at once (and
    // future ones); the explicit NX_SOLO flag lets a non-game opt in too.
    bool wants_solo = (a->exclusive_flags & NX_SOLO) || (a->category && !strcmp(a->category, "Games"));
    if (!wants_solo) return false;
    // Carry a pending "open with" path (e.g. Files -> a .nfv for the Video player) across the warm reboot;
    // s_open_file is plain RAM and would be lost. Empty for a normal menu launch -> self-clears, no stale file.
    snprintf(s_solo_open_file, sizeof s_solo_open_file, "%s", s_open_file);
    nucleo_app_solo_request_id(a->id);   // never returns (warm reboot into Solo)
    return true;
}
static void launch(const MenuNode *app)
{
    int idx = find_app(app->id);
    if (maybe_solo_launch(idx)) return;
    if (idx >= 0) { s_active = idx; s_stub = nullptr; open_app_def(&s_apps[idx]); }
    else          { s_active = -2; s_stub = app;     open_app_def(&STUB); }
}
static void launch_by_id(const char *id)
{
    int idx = find_app(id);
    if (idx < 0) return;
    if (maybe_solo_launch(idx)) return;
    // App-to-app switch (Files "open with", ANIMA "apri musica"): give the OUTGOING app its on_exit
    // so it frees its buffers (e.g. Files' ~6 KB listing, ANIMA's worker) BEFORE the incoming app
    // allocates — more RAM for the viewer, and no leaked buffer. (close_app covers the app->launcher
    // path; this is the missing app->app path.) Guarded to a real app: not the launcher (-1) or stub (-2).
    if (s_active >= 0 && s_apps[s_active].on_exit) s_apps[s_active].on_exit();
    s_active = idx; s_stub = nullptr;
    open_app_def(&s_apps[idx]);
}

// Public: open a registered native app by id (used by the ANIMA shell to act on a
// LAUNCH intent like "apri musica"). Returns false if no app with that id exists, so
// the caller can tell the user it isn't available on the device.
bool nucleo_app_launch_id(const char *id)
{
    if (!id || !id[0] || find_app(id) < 0) return false;
    launch_by_id(id);
    return true;
}

// ANIMA returns WEB registry ids (app-aliases.json); the native launcher uses short ids. Single source
// for the ANIMA app AND the voice path so "apri musica" -> "music" opens the right app. Web-only apps
// (paint/spreadsheet/...) have no native id: they pass through and nucleo_app_launch_id() then returns
// false (the caller shows "only in the web app" / "not on the device").
const char *nucleo_app_native_id(const char *anima_id)
{
    if (!anima_id) return anima_id;
    static const struct { const char *web, *nat; } M[] = {
        {"photo-viewer","photos"}, {"media-player","music"}, {"video-player","video"},
        {"file-commander","files"}, {"calculator","calc"},  {"system-monitor","sysmon"},
        {"ir-remote","remote"},     {"notepad","notepad"},   {"clock","clock"},
        {"calendar","calendar"},    {"recorder","recorder"}, {"radio","radio"},
    };
    for (int i = 0; i < (int)(sizeof(M) / sizeof(M[0])); i++)
        if (!strcmp(M[i].web, anima_id)) return M[i].nat;
    return anima_id;   // already a native id (torch/usb/...) or unknown
}

// ---- "open with" file handoff ------------------------------------------------------------
// Files passes the EXACT file it wants opened; the target viewer (Notes/Music/Photos) reads it
// once in its on_enter via nucleo_app_take_open_file() and opens THAT file instead of its own
// default folder. One small static buffer, consumed once (cleared on read) so it never leaks into
// the next launch. (Declared up top so maybe_solo_launch can stash it into RTC before a Solo reboot.)
const char *nucleo_app_take_open_file(void)
{
    if (!s_open_file[0]) return nullptr;
    static char p[256]; snprintf(p, sizeof p, "%s", s_open_file); s_open_file[0] = 0;
    return p;
}

// Open the app associated with a file's extension (file manager "open-with"). The exact path is
// stashed for the viewer's on_enter (nucleo_app_take_open_file), so an .mp3 from ANY folder
// actually plays and a .txt opens in Notes — not just the app's default directory.
bool nucleo_app_launch_file(const char *path)
{
    if (!path) return false;
    const char *ext = strrchr(path, '.');
    const char *id = nullptr;
    if (ext) {
        if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") ||
            !strcasecmp(ext, ".bmp") || !strcasecmp(ext, ".gif")) id = "photos";
        else if (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".wav")) id = "music";
        else if (!strcasecmp(ext, ".nfv")) id = "video";       // native clip format only (others: web app)
        else if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".md") || !strcasecmp(ext, ".json") ||
                 !strcasecmp(ext, ".log") || !strcasecmp(ext, ".csv") || !strcasecmp(ext, ".ini")) id = "notepad";
    }
    if (!id) return false;                                    // no native viewer for this type
    snprintf(s_open_file, sizeof s_open_file, "%s", path);     // hand the exact file to the viewer's on_enter
    launch_by_id(id);
    return true;
}

static const nucleo_app_def_t *active_def()
{
    if (s_active == -2) return &STUB;
    if (s_active >= 0)  return &s_apps[s_active];
    return nullptr;
}
static void close_app(void)
{
    const nucleo_app_def_t *def = active_def();
    // ANIMA Solo: there is no launcher to return to — let the app save its state (anima's leave() writes
    // the chat), then reboot into the full OS. esp_restart() runs the registered storage-sync shutdown
    // hook (FAT flush), and the cold path on the next boot brings the normal OS back up.
    if (s_solo_active) {
        // Solo non-game apps (ANIMA, Voice Recorder) reboot on exit with httpd down, so grab their last
        // frame off the panel here into /data/Screenshots. Games are NOT captured on exit — that would
        // grab the exit/menu screen; their carousel cover is refreshed from live play in the run loop.
        if (def && def->id && def->id[0] && !(def->category && !strcmp(def->category, "Games")))
            gamefront_save_panel_screenshot(def->id);
        if (def && def->on_exit) def->on_exit();
        esp_restart();   // NEVER returns
    }
    // Covers are captured ONLY on explicit request in-game ('C' / Ctrl+P), never automatically on exit.
    if (def && def->on_exit) def->on_exit();
    // Restore ONLY exclusive we entered declaratively for this app (back/crash/early exit all land here).
    // We deliberately do NOT touch exclusive entered imperatively by an app or a background worker (the
    // Recorder's AI task self-restores when it finishes) — tearing it down here would restart httpd/voice
    // while that worker is still mid-TLS. Runs before the canvas re-acquire: network back first, canvas after.
    if (s_app_excl) { s_app_excl = false; nucleo_exclusive_exit(); }
    // An audio app may have freed the shared canvas for its decoder. The decoder frees on stop but
    // the Wi-Fi RX buffers that grew during streaming take a beat to coalesce, so a single acquire
    // here often still fails and the launcher would render direct (flickery scroll). Retry briefly
    // (≤150 ms) so the common exit-to-launcher path comes back buffered at once; the timed self-heal
    // in nucleo_screen() covers the slower cases.
    for (int i = 0; i < 6 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(25));
    s_app_tab = nullptr;                        // app gone -> TAB returns to the Control Center
    s_app_back = nullptr; s_app_direct = false; // ...Back returns to close, drawing back to buffered
    if (s_app_fullscreen) { s_app_fullscreen = false; s_hint_dirty = true; } // restore the footer on app close
    s_app_ptt = nullptr;                        // ...GO-hold returns to the voice recognizer
    s_app_poll = nullptr;                       // ...no live app polling once back at the launcher
    launcher_render_reset_hint_colors();        // restore the default dark footer for the launcher
    s_active = -1; s_stub = nullptr; s_dirty = true; s_chrome_dirty = true;
    // A game launched FROM GameFront returns to it, not to the home carousel — the front-end keeps
    // its selection so you land back on the game you just played.
    if (s_gf_return) { s_gf_return = false; gamefront_open(); s_gamefront = true; }
}
void nucleo_app_exit(void) { close_app(); }

// Global flashlight overlay (G0 quick-tap). Unlike opening the Torch app, this paints a pure-white
// panel over WHATEVER is on screen and, when dismissed, restores it exactly — your place in any app,
// the launcher, or the Control Center is never disturbed. On a Cardputer the LCD is the only light
// emitter, so "max light" = every subpixel on at 100% backlight.
static void torch_off(void)
{
    if (!s_torch) return;
    s_torch = false;
    nucleo_app_set_brightness(s_torch_prev_bright);                       // give the user's level back
    d.fillScreen(BG);                                                    // wipe white before the layer below repaints
    s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;          // force whatever was underneath back on screen
}
static void torch_on(void)
{
    s_torch_prev_bright = nucleo_app_brightness();                        // remember to restore on dismiss
    s_torch = true;
    nucleo_app_set_brightness(100);
    s_dirty = true;                                                      // the run loop paints the white overlay
}
static void toggle_torch(void) { if (s_torch) torch_off(); else torch_on(); }

// ---- helpers apps draw with --------------------------------------------------
void nucleo_app_set_hint(const char *h) { launcher_render_set_hint(h); s_hint_dirty = true; }
void nucleo_app_set_hint_colors(unsigned short bg, unsigned short fg) { launcher_render_set_hint_colors(bg, fg); s_hint_dirty = true; }
void nucleo_app_set_tab_handler(void (*fn)(void)) { s_app_tab = fn; }
void nucleo_app_set_back_handler(bool (*fn)(int key)) { s_app_back = fn; }
void nucleo_app_set_ptt_handler(void (*fn)(bool on)) { s_app_ptt = fn; }
void nucleo_app_set_poll_handler(bool (*fn)(void)) { s_app_poll = fn; }
void nucleo_app_set_direct_draw(bool on)
{
    s_app_direct = on;
    // A direct-draw app does NOT composite through the shared 32 KB back-buffer, so while it is
    // foreground the launcher's menu canvas is dead weight: free it and hand those 32 KB to the app
    // (SSH terminal, the recorder's capture, photo view...). Re-acquire when the app goes back to
    // buffered drawing. Idempotent — release()/acquire() no-op if already in that state.
    if (on) nucleo_screen_release();
    else    nucleo_screen_acquire();
}
int  nucleo_app_content_top(void)    { return 0; }
int  nucleo_app_content_height(void) { return s_app_fullscreen ? H : (H - HINT); }
void nucleo_app_set_fullscreen(bool on)
{
    if (s_app_fullscreen == on) return;
    s_app_fullscreen = on;
    s_dirty = true;                  // recomposite at the new height
    if (!on) s_hint_dirty = true;    // leaving fullscreen -> repaint the footer over the reclaimed rows
}
void nucleo_app_request_draw(void)   { s_dirty = true; }

// Audio apps (radio, music) call this before starting playback: free the 32 KB shared canvas so
// the Helix MP3 decoder can grab the contiguous block it needs (else MP3InitDecoder fails OOM and
// playback is silent on this PSRAM-less chip). The launcher re-acquires the canvas lazily on
// return. NOTE: video does NOT call this — it BORROWS the canvas as its frame buffer instead.
void nucleo_app_release_buffers(void) { nucleo_screen_release(); }

// ---- launcher input ----------------------------------------------------------
// Keyboard-first carousel: the apps are a single horizontal rail, so all four arrows step the focus
// by one — Left/Up = previous, Right/Down = next (both wrap). Enter opens; backtick (Esc) goes back /
// clears the filter; any printable character type-to-filters (digits included — no numeric quick-select).
// Open the GameFront overlay (HyperSpin-style game launcher) instead of descending into the plain
// Games category carousel. It's a launcher state, so s_active stays -1 the whole time.
static void open_gamefront(void)
{
    gamefront_open();
    s_gamefront = true;
    d.fillScreen(BG);
    s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
}

static void handle_launcher(int k, char ch)
{
    int s = launcher_sel();
    if      (k == NK_LEFT  || k == NK_UP)   launcher_set_sel(s - 1);   // previous (wraps to the last)
    else if (k == NK_RIGHT || k == NK_DOWN) launcher_set_sel(s + 1);   // next (wraps to the first)
    else if (k == NK_ENTER) {
        // The Games tile opens the dedicated game front-end; everything else behaves as before.
        const MenuNode *f = launcher_focused();
        if (f && f->kind == N_MENU && !strcmp(f->id, "Games")) { open_gamefront(); return; }
        const MenuNode *a = launcher_enter(); if (a) launch(a);
    }
    else if (k == NK_BACK)  launcher_back();
    else if (k == NK_DEL)   launcher_filter_backspace();
    else if (k == NK_CHAR && ch == '*') {                        // '*' = pin/unpin the focused app on Home
        const MenuNode *f = launcher_focused();
        if (f && f->kind == N_APP) launcher_toggle_pin(f->id);
    }
    else if (k == NK_CHAR && ch > ' ') launcher_filter_push(ch);
    s_dirty = true;
}

// ---- remote handoff: a connected web client takes priority. While remote we suspend ALL
// local input + rendering and free the UI buffers, concentrating CPU+RAM on the server. ----
extern "C" int  nucleo_ws_client_count(void);
extern "C" int  nucleo_ws_shell_count(void);   // web clients that identified as the OS shell (/ws?shell=1) — drives handoff
extern "C" bool nucleo_remote_enabled(void);
extern "C" unsigned int nucleo_event_publish(const char *topic, const char *payload);
// Per-core CPU load from the idle-hook probe in nucleo_httpd (resolved at final link).
extern "C" int  nucleo_cpu_core_count(void);
extern "C" int  nucleo_cpu_load_pct(int core);   // 0..100, or -1 if the core index is out of range
// Connection info for the watch INFO/NETWORK faces (same sources as the native Connection app).
extern "C" const char *nucleo_auth_pin(void);
extern "C" const char *nucleo_setup_ssid(void);
extern "C" bool        nucleo_setup_time_synced(void);
extern "C" int         nucleo_setup_rssi(void);      // dBm (negative), 0 = not associated
extern "C" int         nucleo_setup_channel(void);   // 2.4 GHz channel
#define REMOTE_GRACE_MS 10000            // tolerate this long at 0 shell clients before reclaiming. 4000 was too
                                         // short: on weak Wi-Fi the /ws drops + reconnects (backoff starts 3s),
                                         // and each gap >grace woke the panel — the screen wouldn't STAY off
                                         // while a client was driving. 10s absorbs the reconnect flaps.
static bool s_remote = false;
static int  s_remote_clients = 0;

// OTA handshake (the definitive fix for "OTA only works after I open Remote Control by hand"). The moment
// an OTA request lands, httpd raises s_force_listen; the run loop then LAUNCHES the Remote Control app,
// whose enter frees the 32 KB launcher canvas + the ~24 KB L1 index — the proven server-listening posture.
// Only once that posture is actually live does the loop raise s_listen_ready, and ota_post BLOCKS on it
// before streaming the image in — so the flash never starts RAM-starved. All cross-task state is plain
// volatile bools written by one side (no shared draw, no lock). Cleared when the OTA ends; on success the
// device reboots anyway. nucleo_app_request_remote_listen(false) also drops the ready latch.
static volatile bool s_force_listen = false;
static volatile bool s_listen_ready = false;
static bool          s_listen_owned = false;   // WE auto-launched Remote Control for an OTA -> WE close it after
extern "C" void nucleo_app_request_remote_listen(bool on) { s_force_listen = on; if (!on) s_listen_ready = false; }
extern "C" bool nucleo_app_remote_listen_ready(void) { return s_listen_ready; }

// ---- web focus + idle screen-off ------------------------------------------------------------
// Extra symbols, resolved at final link (main pulls these components in) — same escape hatch
// nucleo_exclusive.c uses, so nucleo_app needs no REQUIRES cycle. nucleo_voice_enable comes from
// the already-included nucleo_voice.h; nucleo_anima_l1_unload is declared above (line ~212).
extern "C" bool      nucleo_anima_teacher_configured(void);   // an online model key is set (Grok/Claude/Groq/OpenAI)
extern "C" void      nucleo_anima_l1_set_external_brain(bool); // force the offline ANIMA brain to stand down
extern "C" void      nucleo_discovery_stop(void);             // stop mDNS advertising (client already connected)
extern "C" esp_err_t nucleo_discovery_resume(void);           // resume mDNS advertising
extern "C" bool      nucleo_webfs_take_heap_request(void);    // webfs: un asset pesante chiede di liberare i 32 KB canvas

static bool s_web_focus = false;            // deep RAM teardown active (online key + signal while a client drives)
static bool s_mdns_off  = false;            // mDNS stopped while the OS shell is connected (s_remote; frees ~10KB, unconditional re: online key / AP-vs-STA)
// s_disp_sleep is declared up with s_disp_off (top of file) so the httpd/esp_timer display paths can read it.
static bool s_voice_dark = false;           // PTT low-power capture: backlight 0 + canvas freed for the voice session
#define SCREEN_OFF_MS  4000                  // idle (no on-device key) before the panel sleeps ("a few seconds")
#define IDLE_SERVE_MS  30000                 // NO web client: idle at the launcher this long -> free the 32 KB canvas
                                             // anyway so httpd ALWAYS has heap to serve (web OS / OTA / API). On this
                                             // PSRAM-less chip the held canvas was starving httpd (OTA /api/pair timed
                                             // out). Longer than SCREEN_OFF_MS so a glance isn't cut short; any key wakes.
#define G0_HOLD_MS     350                   // GO held past this = Push-to-Talk; below = a torch tap

// While the OS web shell is connected the device's offline L1 brain stands down — ALWAYS, regardless of
// an online key. On this no-PSRAM chip the L1 cascade can't serve a web client (its ~30 KB worker OOMs),
// so keeping L1 unloaded just hands the web server the contiguous RAM it needs. The web OS does offline
// ANIMA in its OWN browser (WASM), and proposes downloading that brain when it isn't installed — it never
// asks the Cardputer for offline answers. The HTTP server is KEPT up (unlike nucleo_exclusive). Fully
// restored (L1 reloads lazily on the next on-device query) the moment the client leaves.
static void web_focus_enter(void)
{
    if (s_web_focus) return;
    // The stand-down frees the L1 index: only under the spine gate (a cascade may be mid-query on the
    // worker/httpd task). If busy, DON'T latch s_web_focus — web_focus_update() retries next tick.
    if (!nucleo_anima_try_lock()) return;
    s_web_focus = true;
    nucleo_anima_l1_set_external_brain(true);   // offline L1 stands down (never reloads) while the web shell is connected
    nucleo_anima_unlock();
    // Voice stays ENABLED: on-device PTT now streams templates from SD (~7 KB, cheap),
    // and disabling it would break the Voice Manager web app and the voice→web command
    // routing — both need PTT live WHILE a web client is connected.
    // (mDNS is stopped UNCONDITIONALLY for the OS shell client in web_focus_update, not here.)
    nucleo_event_publish("system.focus", "{\"active\":true}");
}
static void web_focus_exit(void)
{
    if (!s_web_focus) return;
    s_web_focus = false;
    // No spine gate needed on the way OUT: external_brain(false) only flips the policy flag back to
    // serving (frees nothing — the index reloads lazily inside the next gated query), so there is no
    // use-after-free to guard and exit_remote can never get stuck behind a long online query.
    nucleo_anima_l1_set_external_brain(false);
    // mDNS resume is handled in web_focus_update when the last client leaves (not tied to the key).
    nucleo_event_publish("system.focus", "{\"active\":false}");
}
// Engage/release from the live predicate. Called on connect and periodically while remote, so adding a
// key mid-session takes effect. rssi()<0 means STA is associated WITH signal (0 in AP mode / unassociated)
// — without upstream Wi-Fi the cloud models are unreachable, so we KEEP the offline brain in that case.
static void web_focus_update(void)
{
    // mDNS off whenever the OS shell is connected (s_remote; it already found us): frees ~10KB UNCONDITIONALLY,
    // independent of an online key or AP-vs-STA. Resumed only when the last client leaves.
    if (s_remote && !s_mdns_off)      { nucleo_discovery_stop();   s_mdns_off = true; }
    else if (!s_remote && s_mdns_off) { nucleo_discovery_resume(); s_mdns_off = false; }
    // L1 stands down WHENEVER the OS web shell is connected — unconditionally, regardless of an online
    // key or Wi-Fi signal. The Cardputer (no-PSRAM) can't run the offline L1 cascade for a web client
    // anyway (the ~30 KB worker OOMs -> 503), so loading it only fragments the heap the web server needs.
    // The web OS uses its IN-BROWSER WASM offline brain instead (and proposes its download when absent);
    // the device's L1 is for the NATIVE on-device app + PTT, restored the moment the client leaves.
    bool eligible = s_remote;
    if (eligible) web_focus_enter(); else web_focus_exit();
}

// ---- Remote smartwatch monitor ----------------------------------------------
// While a web client drives the device, its own apps are suspended — so the panel becomes a
// glanceable, multi-face watch instead of a dead "remote session" card. TAB pages through:
//   CONNECTION (SSID / IP / pairing PIN) · MEMORY (SRAM) · CPU (per-core) · STORAGE (SD).
// Every face is composited into the shared canvas and blitted ONCE (ANTI-FLICKER technique 1),
// and the blit is gated by a per-face value signature so the panel only repaints when a number
// it shows actually changes — a single ~1 ms canvas blit, leaving CPU+RAM for the HTTP task.
enum { FACE_INFO = 0, FACE_RAM, FACE_CPU, FACE_STO, FACE_NET, NFACES };
static int      s_remote_face  = FACE_INFO; // CONNECTION is the default: shows IP+PIN to pair more devices
static bool     s_remote_force = true;      // force a repaint (entry / face switch / value change)
static uint32_t s_remote_sig   = 0;         // last painted value signature

// Tiny trend ring-buffers for the MEMORY/CPU sparklines. 60 bytes each — negligible, and they
// advance once per second on ANY face so a trend is already there when you page to it.
#define SPN 60
static uint8_t s_h_cpu[SPN], s_h_ram[SPN];
static int     s_h_head = 0, s_h_n = 0, s_h_sec = -1;

static unsigned short watch_level_col(int pct) { return pct >= 85 ? C_RED : pct >= 60 ? C_YELLOW : C_GREEN; }
static unsigned short watch_accent(int face) {
    switch (face) {
        case FACE_RAM: return C_GREEN;  case FACE_CPU: return C_PURPLE;
        case FACE_STO: return C_YELLOW; case FACE_NET: return C_PINK;
        default:       return C_BLUE;   // CONNECTION
    }
}
// RSSI (dBm) -> 0..100% bars, the usual Wi-Fi mapping (-90 dBm ≈ 0, -40 dBm ≈ full).
static int watch_rssi_pct(int rssi) { if (!rssi) return 0; int p = 2 * (rssi + 90); return p < 0 ? 0 : p > 100 ? 100 : p; }
// FNV-1a over a C string, for change-detection of the textual INFO face.
static uint32_t watch_shash(const char *s) { uint32_t h = 2166136261u; while (s && *s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

template <typename T> static void watch_ring(T *g, int cx, int cy, int rad, int pct, unsigned short col)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r0 = rad - 8, r1 = rad;
    g->fillArc(cx, cy, r0, r1, 0, 360, 0x2104);                       // dim full track
    if (pct > 0) g->fillArc(cx, cy, r0, r1, 0, 360 * pct / 100, col); // value arc
    char b[6]; snprintf(b, sizeof b, "%d%%", pct);
    g->setTextSize(2); g->setTextColor(col, BG);
    g->setCursor(cx - (int)strlen(b) * 6, cy - 7); g->print(b);
}

template <typename T> static void watch_bar(T *g, int x, int y, int w, const char *lbl, int pct, unsigned short col)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(x, y); g->print(lbl);
    char b[6]; snprintf(b, sizeof b, "%d%%", pct);
    g->setTextColor(col, BG); g->setCursor(x + w - (int)strlen(b) * 6, y); g->print(b);
    g->drawRoundRect(x, y + 11, w, 6, 2, 0x2945);
    g->fillRoundRect(x, y + 11, w * pct / 100, 6, 2, col);
}

// Trend line into a small strip (the watch "history" trick). Reads the ring buffer oldest->newest.
template <typename T> static void watch_spark(T *g, int x, int y, int w, int h, const uint8_t *hist, unsigned short col)
{
    g->drawFastHLine(x, y + h, w, 0x2104);
    if (s_h_n < 2) return;
    int show = s_h_n < SPN ? s_h_n : SPN;
    int px = -1, py = 0;
    for (int i = 0; i < show; i++) {
        int idx = (s_h_head - show + i + SPN * 2) % SPN;
        int v = hist[idx]; if (v > 100) v = 100;
        int cx = x + (w - 1) * i / (show - 1);
        int cy = y + h - 1 - v * (h - 1) / 100;
        if (px >= 0) g->drawLine(px, py, cx, cy, col);
        px = cx; py = cy;
    }
}

// Top status strip: live dot + client count (left), centred face name (accent), clock (right).
template <typename T> static void watch_chrome(T *g, int face)
{
    static const char *NM[NFACES] = { "CONNECTION", "MEMORY", "CPU", "STORAGE", "NETWORK" };
    g->setTextSize(1);
    g->fillCircle(10, 8, 3, C_GREEN);
    char cc[8]; snprintf(cc, sizeof cc, "%dc", s_remote_clients);
    g->setTextColor(MUTED, BG); g->setCursor(18, 5); g->print(cc);
    const char *nm = NM[face];
    g->setTextColor(watch_accent(face), BG); g->setCursor((W - (int)strlen(nm) * 6) / 2, 5); g->print(nm);
    char clk[6] = "--:--";
    if (nucleo_setup_time_synced()) { time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv); snprintf(clk, sizeof clk, "%02d:%02d", tmv.tm_hour, tmv.tm_min); }
    g->setTextColor(MUTED, BG); g->setCursor(W - (int)strlen(clk) * 6 - 8, 5); g->print(clk);
    g->drawFastHLine(8, 16, W - 16, 0x2945);
}

// Bottom pager dots — the active face is a larger, accent-colored dot (smartwatch pagination).
template <typename T> static void watch_footer(T *g, int face)
{
    int gap = 12, x0 = (W - (NFACES - 1) * gap) / 2, y = 128;
    for (int i = 0; i < NFACES; i++) {
        if (i == face) g->fillCircle(x0 + i * gap, y, 3, watch_accent(i));
        else           g->fillCircle(x0 + i * gap, y, 2, 0x4208);
    }
}

struct WatchData {
    int ramFree, ramUsed, ramTotal, ramPct;
    int n, l0, l1, avg, tasks;
    int stoPct, up, rssi, ch;
    uint64_t stoFree, stoTotal;
    const char *fs, *ssid, *ip, *pin; bool sta;
};

template <typename T> static void watch_face_info(T *g, const WatchData *w)
{
    watch_chrome(g, FACE_INFO);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(10, 23); g->print(w->sta ? "WI-FI" : "MODE");
    char net[22]; snprintf(net, sizeof net, "%.18s", (w->sta && w->ssid[0]) ? w->ssid : "Setup AP");
    g->setTextSize(2); g->setTextColor(C_BLUE, BG); g->setCursor(10, 32); g->print(net);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(10, 56); g->print("ADDRESS");
    char url[40]; if (w->sta && w->ip[0]) snprintf(url, sizeof url, "%s", w->ip); else snprintf(url, sizeof url, "192.168.4.1");
    g->setTextSize(2); g->setTextColor(C_GREEN, BG); g->setCursor(10, 65); g->print(url);
    int by = 88, bh = 24; g->drawRoundRect(8, by, W - 16, bh, 6, C_YELLOW);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(16, by + 9); g->print("PIN");
    if (w->sta && w->ip[0]) { g->setTextSize(2); g->setTextColor(C_YELLOW, BG); g->setCursor(54, by + 5); g->print(w->pin); }
    else                    { g->setTextSize(1); g->setTextColor(MUTED, BG);   g->setCursor(54, by + 9); g->print("join Wi-Fi to pair"); }
    watch_footer(g, FACE_INFO);
}

template <typename T> static void watch_face_ram(T *g, const WatchData *w)
{
    watch_chrome(g, FACE_RAM);
    watch_ring(g, 48, 52, 30, w->ramPct, watch_level_col(w->ramPct));
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(48 - 9, 84); g->print("RAM");
    struct { const char *k; int kb; unsigned short c; } r[] = {
        { "FREE", w->ramFree, C_GREEN }, { "USED", w->ramUsed, C_BLUE }, { "TOTAL", w->ramTotal, MUTED } };
    int x = 100;
    for (int i = 0; i < 3; i++) {
        int y = 22 + i * 25;
        g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(x, y); g->print(r[i].k);
        char v[16]; snprintf(v, sizeof v, "%d KB", r[i].kb);
        g->setTextSize(2); g->setTextColor(r[i].c, BG); g->setCursor(x, y + 9); g->print(v);
    }
    watch_spark(g, 8, 100, W - 16, 12, s_h_ram, C_GREEN);
    watch_footer(g, FACE_RAM);
}

template <typename T> static void watch_face_cpu(T *g, const WatchData *w)
{
    watch_chrome(g, FACE_CPU);
    watch_ring(g, 48, 52, 30, w->avg, watch_level_col(w->avg));
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(48 - 9, 84); g->print("CPU");
    int x = 100, bw = W - x - 8;
    watch_bar(g, x, 24, bw, "Core 0", w->l0, watch_level_col(w->l0));
    if (w->n > 1) watch_bar(g, x, 50, bw, "Core 1", w->l1, watch_level_col(w->l1));
    char ln[28]; snprintf(ln, sizeof ln, "%d MHz / %d tasks", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, w->tasks);
    g->setTextSize(1); g->setTextColor(DIM, BG); g->setCursor(x, 76); g->print(ln);
    watch_spark(g, 8, 100, W - 16, 12, s_h_cpu, C_PURPLE);
    watch_footer(g, FACE_CPU);
}

template <typename T> static void watch_face_sto(T *g, const WatchData *w)
{
    watch_chrome(g, FACE_STO);
    watch_ring(g, 48, 52, 30, w->stoPct, C_YELLOW);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(48 - 6, 84); g->print("SD");
    int x = 100;
    int ftn = (int)(w->stoFree / 100000000ULL), ttn = (int)(w->stoTotal / 100000000ULL);  // tenths of GB (no float)
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(x, 24); g->print("FREE");
    char v[20]; snprintf(v, sizeof v, "%d.%d GB", ftn / 10, ftn % 10);
    g->setTextSize(2); g->setTextColor(C_GREEN, BG); g->setCursor(x, 33); g->print(v);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(x, 58); g->print("TOTAL");
    snprintf(v, sizeof v, "%d.%d GB", ttn / 10, ttn % 10);
    g->setTextSize(2); g->setTextColor(MUTED, BG); g->setCursor(x, 67); g->print(v);
    char up[30]; snprintf(up, sizeof up, "up %dh %dm  %s", w->up / 3600, (w->up % 3600) / 60, w->fs);
    g->setTextSize(1); g->setTextColor(DIM, BG); g->setCursor(8, 100); g->print(up);
    watch_footer(g, FACE_STO);
}

// Smartwatch signal-bars glyph: `bars`/5 lit, ascending heights.
template <typename T> static void watch_signal(T *g, int x, int y, int bars, unsigned short col)
{
    for (int i = 0; i < 5; i++) {
        int bh = 4 + i * 4, bx = x + i * 7, by = y - bh;
        if (i < bars) g->fillRoundRect(bx, by, 5, bh, 1, col);
        else          g->drawRoundRect(bx, by, 5, bh, 1, 0x4208);
    }
}

template <typename T> static void watch_face_net(T *g, const WatchData *w)
{
    watch_chrome(g, FACE_NET);
    if (!w->sta) {   // AP/setup mode: no upstream link to rate
        g->setTextSize(2); g->setTextColor(C_PINK, BG); g->setCursor(10, 50); g->print("Access Point");
        g->setTextSize(1); g->setTextColor(DIM, BG);  g->setCursor(10, 74); g->print("no upstream Wi-Fi link");
        watch_footer(g, FACE_NET); return;
    }
    int pct = watch_rssi_pct(w->rssi), bars = (pct + 19) / 20;   // 0..5
    unsigned short col = watch_level_col(pct);
    watch_ring(g, 48, 52, 30, pct, col);                         // % in the ring centre
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(48 - 12, 84); g->print("WIFI");
    int x = 104;
    watch_signal(g, x, 40, bars, col);                           // bars top-right (baseline y40)
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(x, 46); g->print("SIGNAL");
    char v[16]; snprintf(v, sizeof v, "%d dBm", w->rssi);
    g->setTextSize(2); g->setTextColor(col, BG); g->setCursor(x, 55); g->print(v);
    snprintf(v, sizeof v, "ch %d", w->ch);
    g->setTextSize(1); g->setTextColor(DIM, BG); g->setCursor(x, 80); g->print(v);
    g->setTextSize(1); g->setTextColor(DIM, BG); g->setCursor(8, 104); g->print(w->ssid[0] ? w->ssid : "-");
    watch_footer(g, FACE_NET);
}

template <typename T> static void watch_draw_face(T *g, const WatchData *w)
{
    switch (s_remote_face) {
        case FACE_RAM: watch_face_ram(g, w); break;
        case FACE_CPU: watch_face_cpu(g, w); break;
        case FACE_STO: watch_face_sto(g, w); break;
        case FACE_NET: watch_face_net(g, w); break;
        default:       watch_face_info(g, w); break;
    }
}

// Gather live values, advance the trend buffers, gate on a per-face signature, blit once on change.
static void remote_monitor_render(void)
{
    int up = (int)(esp_timer_get_time() / 1000000);
    bool tick1s = (up != s_h_sec);
    int face = s_remote_face;

    // CPU loads are just reads of the probe's smoothed floats — no lock, ~free.
    int n  = nucleo_cpu_core_count(); if (n < 1) n = 1;
    int l0 = nucleo_cpu_load_pct(0); if (l0 < 0) l0 = 0;
    int l1 = n > 1 ? nucleo_cpu_load_pct(1) : 0; if (l1 < 0) l1 = 0;
    int avg = n > 1 ? (l0 + l1) / 2 : l0;
    int tasks = (int)uxTaskGetNumberOfTasks();

    // RAM costs a heap walk (briefly takes the heap lock the client's HTTP/TLS allocs also want),
    // so only do it when a value actually needs it: the MEMORY face, or the 1 Hz trend sample.
    int totalKB = 0, freeKB = 0, usedKB = 0, ramPct = 0;
    if (face == FACE_RAM || tick1s) {
        multi_heap_info_t hi; heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
        totalKB = (int)((hi.total_free_bytes + hi.total_allocated_bytes) / 1024);
        freeKB  = (int)(hi.total_free_bytes / 1024);
        usedKB  = totalKB - freeKB;
        ramPct  = totalKB ? usedKB * 100 / totalKB : 0;
    }
    if (tick1s) {   // advance the sparkline trend once per second, on any face
        s_h_sec = up;
        s_h_cpu[s_h_head] = (uint8_t)avg; s_h_ram[s_h_head] = (uint8_t)ramPct;
        s_h_head = (s_h_head + 1) % SPN; if (s_h_n < SPN) s_h_n++;
    }

    // Storage (cached; refreshed only on landing the face) and Wi-Fi RSSI (driver call) are each
    // computed only for the face that shows them — nothing the client's session pays for otherwise.
    const nucleo_storage_info_t *st = nucleo_storage_info();
    int stoPct = (face == FACE_STO && st->total_bytes) ? (int)(100 - st->free_bytes * 100 / st->total_bytes) : 0;
    int rssi = 0, ch = 0;
    if (face == FACE_NET) { rssi = nucleo_setup_rssi(); ch = nucleo_setup_channel(); }

    int clkMin = 0;
    if (nucleo_setup_time_synced()) { time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv); clkMin = tmv.tm_hour * 60 + tmv.tm_min; }

    // Per-face signature: the panel only repaints when a number THIS face shows changed. The live
    // faces fold in `up` so their sparkline scrolls (and the clock ticks) at 1 Hz.
    uint32_t sig = ((uint32_t)face << 28) ^ ((uint32_t)s_remote_clients << 24) ^ ((uint32_t)clkMin << 6);
    switch (face) {
        case FACE_RAM: sig ^= (uint32_t)freeKB ^ ((uint32_t)usedKB << 10) ^ ((uint32_t)up << 1); break;
        case FACE_CPU: sig ^= (uint32_t)l0 ^ ((uint32_t)l1 << 7) ^ ((uint32_t)tasks << 14) ^ ((uint32_t)up << 1); break;
        case FACE_STO: sig ^= (uint32_t)(st->free_bytes >> 20) ^ ((uint32_t)stoPct << 12); break;
        case FACE_NET: sig ^= (uint32_t)(rssi & 0xff) ^ ((uint32_t)ch << 8) ^ ((uint32_t)up << 12); break;
        default:       sig ^= watch_shash(nucleo_setup_ssid()) ^ watch_shash(nucleo_setup_ip()) ^ watch_shash(nucleo_auth_pin()); break;
    }
    if (!s_remote_force && sig == s_remote_sig) return;
    s_remote_force = false; s_remote_sig = sig;

    WatchData w = {
        freeKB, usedKB, totalKB, ramPct,
        n, l0, l1, avg, tasks,
        stoPct, up, rssi, ch, st->free_bytes, st->total_bytes,
        st->mounted ? st->fs_type : "no SD", nucleo_setup_ssid(), nucleo_setup_ip(), nucleo_auth_pin(),
        !strcmp(nucleo_setup_mode(), "sta"),
    };

    // Server mode draws the faces DIRECT to the panel and NEVER holds the 32 KB shared canvas — that block
    // is released in enter_remote and kept free for the HTTP/TLS path the whole session (the single biggest
    // RAM win on this PSRAM-less chip; the canvas couldn't even allocate under load before). FACE_INFO is
    // static so the direct redraw is flicker-free; the live faces (RAM/CPU) only repaint on a value change.
    d.startWrite(); d.fillScreen(BG); watch_draw_face(&d, &w); d.endWrite();   // direct (no 32 KB canvas)
}

// Page to a face (TAB / arrows). Storage needs a fresh statvfs; refresh only on landing there.
static void remote_set_face(int face)
{
    s_remote_face = (face + NFACES) % NFACES;
    s_remote_force = true;
    if (s_remote_face == FACE_STO) nucleo_storage_refresh();
}

static void enter_remote(int clients)
{
    s_remote = true; s_remote_clients = clients;
    s_remote_face = FACE_INFO; s_remote_force = true;    // open on CONNECTION (IP + pairing PIN)
    // Hand the client every spare byte: the assistant's ~24 KB L1 working set isn't needed while the device
    // is just a monitor, and freeing it gives the HTTP/TLS path contiguous RAM. It reloads from SD on
    // the next query once the client disconnects. Skipped if a query is running (no use-after-free).
    nucleo_anima_l1_unload_if_idle();
    web_focus_update();                                   // deep teardown if an online key + Wi-Fi signal
    if (!s_app_direct) nucleo_screen_release();           // hand the 32 KB canvas to the server NOW (faces draw direct)
    if (!s_disp_sleep && !s_disp_off) remote_monitor_render();   // first face, drawn DIRECT (no 32 KB canvas)
    char p[40]; snprintf(p, sizeof(p), "{\"active\":true,\"clients\":%d}", clients);
    nucleo_event_publish("system.remote", p);
}
static void exit_remote(void)
{
    s_remote = false; s_remote_clients = 0;
    web_focus_update();                                  // s_remote now false -> restores L1 policy AND resumes mDNS
    if (!s_disp_sleep) d.fillScreen(BG);
    s_dirty = true; s_chrome_dirty = true;                // force a full launcher repaint
    nucleo_event_publish("system.remote", "{\"active\":false}");
}

// ---- idle screen-off --------------------------------------------------------------------------
// ONLY while a web client is driving the device (s_remote): after SCREEN_OFF_MS with no on-device key,
// blank the backlight AND free the 32 KB framebuffer (the single biggest RAM win on this PSRAM-less
// chip) — handing that block to the web OS that actually needs it while nobody looks at the panel.
// With NO client connected the panel never sleeps. Any key wakes it; so does the client going away
// (handled in the run loop). Independent of the web /api/display path (s_disp_off): we never fight a
// client that is deliberately holding the screen dark.
static void display_sleep(void)
{
    if (s_disp_sleep) return;
    s_disp_sleep = true;
    s_disp_wake_req = false;                      // drop any /api/display wake racing in: honor only requests posted while THIS sleep holds the panel
    nucleo_ui_set_brightness(0);                 // backlight off; the stored level (s_brightness) is untouched
    if (!s_app_direct) nucleo_screen_release();  // give the 32 KB canvas back (unless an app owns that RAM)
    nucleo_event_publish("system.display", "{\"on\":false,\"reason\":\"idle\"}");
}
static void display_wake(void)
{
    s_disp_wake_req = false;                      // any wake satisfies a pending /api/display wake request
    if (!s_disp_sleep) return;
    s_disp_sleep = false;
    s_disp_off = false;                           // a physical wake also ends a web client's dark-hold, else it strands until the <=90s deadman (blocking idle screen-off + 32 KB reclaim)
    // Don't reclaim the 32 KB canvas on a mid-session remote wake: the watch faces draw DIRECT (cv is forced
    // NULL in remote_monitor_render), so re-acquiring it would only take the block back from the server for
    // one ~SCREEN_OFF_MS window until the next idle re-sleep. Keep it freed for the whole remote session.
    if (!s_app_direct && !s_remote) for (int i = 0; i < 6 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_set_brightness(s_brightness);     // restore the user's stored brightness
    d.fillScreen(BG);
    s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true; s_remote_force = true;  // force a full repaint
    nucleo_event_publish("system.display", "{\"on\":true}");
}

// ---- reminder banner: a global overlay fed by the background calendar service -----------
// The service (calendar_svc.cpp) runs on its own task and can't draw; it posts here and the
// UI task renders. While remote the device UI is suspended, so the service routes the alert
// to the web client instead and never posts a banner.
extern "C" void nucleo_calendar_svc_start(void);
extern "C" bool nucleo_ui_is_remote(void) { return s_remote; }

struct notify_t { char title[64]; char body[96]; };
static QueueHandle_t s_notify_q = nullptr;
static bool s_notify_show = false;
static int64_t s_notify_at = 0;            // ms when the banner was raised (for auto-dismiss)
static char s_notify_title[64] = "", s_notify_body[96] = "";

extern "C" void nucleo_notify_post(const char *title, const char *body)
{
    if (!s_notify_q) return;
    notify_t n;
    snprintf(n.title, sizeof n.title, "%s", title ? title : "");
    snprintf(n.body, sizeof n.body, "%s", body ? body : "");
    xQueueSend(s_notify_q, &n, 0);
}

// Wrap `src` into `lines` of at most `maxc` chars (break on spaces), drawing each centered at
// `cw` px/char from `y0`, stepping `dy`. Returns the next free y. Tiny helper so title + body
// share one word-wrap path. Mutates a local copy only.
static int notify_wrap(const char *src, int lines, int maxc, int cw, int y0, int dy)
{
    char buf[96]; snprintf(buf, sizeof buf, "%s", src);
    char *p = buf; int y = y0;
    for (int line = 0; line < lines && *p; line++) {
        int len = (int)strlen(p);
        if (len > maxc) { int cut = maxc; while (cut > 6 && p[cut] != ' ') cut--; if (p[cut] == ' ') { p[cut] = 0; len = cut; } }
        d.setCursor((W - len * cw) / 2, y); d.print(p);
        p += len; while (*p == ' ') p++; y += dy;
    }
    return y;
}

static void draw_notify_banner(void)
{
    d.fillRect(0, 0, W, H - HINT, C_GREEN);
    d.setTextColor(INK, C_GREEN);
    d.setTextSize(1); d.setCursor(10, 8); d.print("NOTIFICA");
    d.drawFastHLine(10, 18, 220, INK);

    d.setTextSize(2); notify_wrap(s_notify_title, 2, 19, 12, 28, 18);     // title: prominent, up to 2 lines
    d.setTextSize(1); notify_wrap(s_notify_body, 2, 38, 6, 72, 12);       // body: secondary

    const char *hint = "press any key";
    d.setCursor((W - (int)strlen(hint) * 6) / 2, 104); d.print(hint);
}

// ---- voice overlay: real-time dictation preview -------------------------------
static void draw_voice_overlay(void)
{
    d.fillRect(0, 0, W, H, BG);
    
    // Status header
    d.setTextSize(2); d.setTextColor(C_GREEN, BG); d.setCursor(16, 16); 
    d.print("IN ASCOLTO");
    
    // Mic pulse animation
    int64_t t = esp_timer_get_time() / 150000;
    int radius = 10 + (t % 4);
    unsigned short pulse = (t & 1) ? C_RED : C_YELLOW;
    d.fillCircle(W - 24, 24, radius, pulse);
    d.fillCircle(W - 24, 24, 6, 0xFFFF);
    
    // Live text
    char live[128];
    nucleo_voice_get_live_sentence(live, sizeof(live));
    
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(16, 48);
    d.print("Trascrizione in tempo reale:");
    
    d.setTextSize(2); d.setCursor(16, 68);
    if (live[0]) {
        d.setTextColor(0xFFFF, BG); d.print(live);
    } else {
        d.setTextColor(DIM, BG); d.print("...");
    }
    
    // Footer instruction
    d.setTextSize(1); d.setTextColor(C_BLUE, BG); d.setCursor(16, H - 20);
    d.print("Rilascia GO per elaborare, tieni per parlare");
}

// Toast pill over a just-captured frame: green check + "Screenshot salvato", or red X + error.
static void draw_shot_toast(M5Canvas *c, bool ok)
{
    const char *msg = ok ? "Screenshot salvato" : "Errore salvataggio";
    uint16_t col = ok ? C_GREEN : C_RED;
    c->setFont(&fonts::Font2); c->setTextSize(1);
    int tw = (int)c->textWidth(msg);
    int ph = 24, pw = tw + 34, px = (W - pw) / 2, py = H - ph - 8;
    c->fillRoundRect(px + 2, py + 3, pw, ph, 7, (uint16_t)0x0000);         // shadow
    c->fillRoundRect(px, py, pw, ph, 7, INK);
    c->drawRoundRect(px, py, pw, ph, 7, col);
    int ix = px + 15, iy = py + ph / 2;                                    // status glyph
    c->fillCircle(ix, iy, 7, col);
    if (ok) { c->drawLine(ix - 3, iy, ix - 1, iy + 3, INK); c->drawLine(ix - 1, iy + 3, ix + 4, iy - 3, INK); }
    else    { c->drawLine(ix - 3, iy - 3, ix + 3, iy + 3, INK); c->drawLine(ix + 3, iy - 3, ix - 3, iy + 3, INK); }
    c->setTextColor(FG, INK); c->setCursor(px + 28, py + (ph - c->fontHeight()) / 2);
    c->print(msg);
}

// ---- run loop ----------------------------------------------------------------
void nucleo_app_run(void)
{
    launcher_build_menu();
    d.setRotation(1);
    d.fillScreen(BG);
    launcher_reset();
    int64_t last_tick = 0, last_clock = 0, remote_gone_ms = 0, last_remote = 0, last_remote_input = 0;
    int64_t last_act = esp_timer_get_time() / 1000, focus_chk = 0;   // idle screen-off + web-focus recheck timers

    // Watchdog the loop: if an iteration wedges >8 s the chip resets instead of freezing.
    // Tolerate ESP_ERR_INVALID_STATE if the TWDT is disabled in this build.
    esp_task_wdt_add(NULL);

    // Background reminder service + the queue it posts on-screen banners through.
    // ANIMA Solo runs EXCLUSIVE (like USB-MSC): do NOT start the calendar service — it is a second task
    // doing periodic SD reads (fopen calendar.json on a 4 KB stack) that competes with the ANIMA worker's
    // SD access. Reminders belong to the full OS; in Solo only the assistant runs.
    s_notify_q = xQueueCreate(4, sizeof(notify_t));
    if (!s_solo_active) nucleo_calendar_svc_start();

    // G0 side button (GPIO0), active-low with an internal pull-up. We are its ONLY reader: a quick
    // tap toggles the torch, a hold is Push-to-Talk. It is NOT on the keyboard matrix, so polling
    // it here is conflict-free.
    gpio_config_t g0cfg = { .pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&g0cfg);
    int64_t g0_down_ms = 0; int64_t g0_cooldown_ms = 0;
    bool    g0_ptt = false;                    // this press already crossed into a talk-hold (PTT engaged)
    void  (*g0_app_ptt)(bool) = nullptr;       // latched app PTT handler for THIS hold (recorder); else voice PTT
    int64_t g0_dark_since = 0;                 // when the PTT low-power blank started (safety-timeout anchor)

    // Solo: we booted as a dedicated personality — open the target app straight away and never show the
    // launcher. close_app() reboots back to the full OS instead of returning here.
    if (s_solo_active) {
        const char *sid = (s_solo_app == SOLO_RECORDER)               ? "recorder"
                        : (s_solo_app == SOLO_APPID && s_solo_id[0])  ? s_solo_id   // heavy game in its fresh-heap Solo
                        :                                               "anima";
        if (s_solo_app == SOLO_APPID && s_solo_open_file[0]) {        // restore a Files "open with" handoff (e.g. a .nfv)
            snprintf(s_open_file, sizeof s_open_file, "%s", s_solo_open_file);
            s_solo_open_file[0] = 0;                                  // consume once
        }
        nucleo_app_launch_id(sid);
    }
    // Full OS after a Recorder Solo job: re-open the Recorder so the user lands on the saved result.
    else if (reopen_recorder_pending()) { s_reopen_req = 0; nucleo_app_launch_id("recorder"); }

    for (;;) {
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time() / 1000;

        // OTA listening-handshake (ota_listen_fsm.h is the tested decision core). An OTA request raised
        // s_force_listen from the httpd task; bring the device into the server-listening posture by LAUNCHING
        // Remote Control — its enter frees the 32 KB canvas + the L1 index, exactly what made a hand-opened
        // Remote Control let the flash through. ota_post() blocks on s_listen_ready (set only once Remote
        // Control is truly foreground). When the OTA ends we close the Remote Control WE opened so everything
        // returns as before (a Remote Control the user opened by hand is left alone). Runs before everything
        // else so the posture comes up regardless of screen-off / a foreground app / GameFront.
        {
            const nucleo_app_def_t *cur = active_def();
            ota_listen_state_t ls = {
                .force = s_force_listen,
                .active_is_remote = (cur && cur->id && !strcmp(cur->id, "remote")),
                .owned = s_listen_owned,
                .ready = s_listen_ready,
            };
            ota_listen_action_t act = ota_listen_step(&ls);
            s_listen_owned = ls.owned;
            s_listen_ready = ls.ready;
            if (act == OTA_LISTEN_LAUNCH) {
                if (s_disp_sleep) display_wake();          // asleep -> wake so the app can enter cleanly
                nucleo_app_launch_id("remote");            // closes whatever's open, runs remote_enter() (frees canvas+L1)
                last_act = now;
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;                                  // re-tick; next pass sees "remote" foreground -> READY
            } else if (act == OTA_LISTEN_RESTORE) {
                close_app();                               // OTA done -> close the Remote Control we opened, back to launcher
                last_act = now;
                continue;
            }
        }

        // Idle screen-off: while the panel sleeps we draw NOTHING (the 32 KB canvas is freed and given to
        // the web OS), but still track the web client so connect/disconnect + web-focus stay correct. Any
        // on-device key — or the G0 side button — wakes it.
        if (s_disp_sleep) {
            // A web app asked to re-light the screen via /api/display while idle screen-off had frozen the panel:
            // honor it here on the launcher task (display_wake re-acquires the 32 KB canvas + forces a full repaint).
            if (s_disp_wake_req) { s_disp_wake_req = false; display_wake(); last_act = now; continue; }
            int clients = nucleo_remote_enabled() ? nucleo_ws_shell_count() : 0;
            if (clients > 0 || s_force_listen) { remote_gone_ms = 0; if (!s_remote) enter_remote(clients); else s_remote_clients = clients; }
            else if (s_remote) { if (remote_gone_ms == 0) remote_gone_ms = now;
                                 // Client gone for good → wake the panel: with no client it must stay on.
                                 if (now - remote_gone_ms >= REMOTE_GRACE_MS) { exit_remote(); display_wake(); last_act = now; } }
            nucleo_key_t wk = nucleo_kbd_read();
            if (wk.key != NK_NONE || gpio_get_level(GPIO_NUM_0) == 0) { display_wake(); last_act = now; last_remote_input = now; }
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        // Awake: blank the panel once the device has been untouched for SCREEN_OFF_MS — but ONLY while a
        // web client is connected (s_remote), since the whole point is to free that RAM for it. With
        // nobody connected the panel stays on indefinitely. Never while the torch overlay or PTT voice is
        // up, nor while a web app holds the screen dark via /api/display.
        // ...also blank immediately (not after the SCREEN_OFF_MS idle) when the webfs raises a heap request: a
        // heavy asset (Vosk lib/model) is being pulled and needs the 32 KB canvas back NOW. take_*()
        // consumes the one-shot flag; all the same guards apply (only while remote, no torch/PTT/etc.).
        // Free the 32 KB canvas at idle so httpd ALWAYS has the contiguous heap to serve. Two triggers:
        //   • a web client is connected (s_remote): blank after SCREEN_OFF_MS — the canvas goes to the web OS;
        //   • NO client, sitting idle at the launcher (no app open): blank after the longer IDLE_SERVE_MS so a
        //     headless/server device keeps serving heap free. This is the fix for "OTA /api/pair times out": with
        //     the canvas held the heap-starved ADV had no block to serve the pairing handshake, so the flash never
        //     started. Freeing it gives httpd ~+32 KB. Any key/G0 wakes (display_wake re-acquires the canvas).
        bool blank_for_client = s_remote && (nucleo_webfs_take_heap_request() || now - last_act >= SCREEN_OFF_MS);
        bool blank_for_serve  = !s_remote && s_active == -1 && !s_gamefront && (now - last_act >= IDLE_SERVE_MS);
        if ((blank_for_client || blank_for_serve) &&
            !s_disp_off && !s_torch && !s_voice_dark && !nucleo_voice_is_listening()) {
            display_sleep();
            continue;
        }
        // Salvaschermo locale: si attiva dal launcher (nessuna app aperta), senza client remoto.
        // Funziona anche da GameFront: s_gamefront viene azzerato prima del lancio (altrimenti il
        // branch render gamefront>foreground-app impedisce a on_draw di girare) e s_gf_return=true
        // assicura il rientro nel carosello dopo la chiusura del saver.
        // Qualsiasi tasto nell'app screensaver resetta last_act (via on_key) e chiude il saver.
        if (s_active == -1 && !s_remote && !s_torch && !s_voice_dark && !s_disp_sleep &&
            nucleo_screensaver_should_activate(now - last_act)) {
            nucleo_screensaver_set_trigger();
            if (s_gamefront) { s_gamefront = false; s_gf_return = true; }
            nucleo_app_launch_id("screensaver");
            last_act = now;  // reset per non ri-triggerare subito dopo
        }

        // PTT low-power capture finished: the voice engine has captured, recognized, and played the
        // end beep, then released VH_PTT. Bring the panel back and force a full repaint. The 32 KB
        // back-buffer is re-acquired lazily by the render path (once the voice task's buffers free),
        // so we don't force a createSprite here that could briefly race the recognizer's RAM.
        // Safety net: also restore if the blank has lasted > 15 s WITHOUT the mic being open — covers
        // the (rare) case where the engine never came up (e.g. OOM creating the task) and so never
        // releases VH_PTT, which would otherwise strand the panel dark. A genuine long hold keeps the
        // mic open (is_listening true), so it never trips this.
        if (s_voice_dark && (!nucleo_voice_ptt_engaged() ||
                             (!nucleo_voice_is_listening() && now - g0_dark_since > 15000))) {
            s_voice_dark = false;
            nucleo_app_set_brightness(s_brightness);           // restore the user's stored backlight level
            d.fillScreen(BG);
            s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true; s_remote_force = true;
        }

        // G0 ("GO" side button) is dual-action, split by how long it is held:
        //   • quick tap  (< G0_HOLD_MS) → toggle the flashlight overlay
        //   • press-hold (≥ G0_HOLD_MS) → Push-to-Talk: bring the voice engine up, sound a "ready"
        //                                  chime, capture while held, recognize on release.
        // We own the GPIO0 timing (the single reader) and drive the engine via nucleo_voice_ptt();
        // the engine no longer polls the pin, so a tap and a talk-hold can never both fire on one
        // press. (The old long-press jump to the ANIMA app is gone — hold now talks instead.)
        if (gpio_get_level(GPIO_NUM_0) == 0) {                 // pressed (active-low)
            last_act = now;
            if (g0_down_ms == 0) g0_down_ms = now;
            else if (!g0_ptt && now - g0_down_ms >= G0_HOLD_MS) {
                g0_ptt = true;                                 // crossed the threshold → start talking
                if (s_torch) torch_off();                      // a hold past a torch tap dismisses the light first
                if (s_app_ptt) {
                    // A foreground app (Voice Recorder) owns the GO-hold: it records while held. We do NOT
                    // blank the panel or free the canvas — the user watches the live waveform/timer, and the
                    // app provides its own audio start/end cues. Latch the handler so THIS hold's release is
                    // delivered even if the app exits mid-hold (s_app_ptt would clear).
                    g0_app_ptt = s_app_ptt;
                    g0_app_ptt(true);
                } else {
                    // Enter low-power capture for the WHOLE voice session: blank the panel (backlight 0)
                    // AND free the 32 KB shared back-buffer. Two wins: (1) the recognizer needs ~38 KB of
                    // buffers — vdsp_acc alone is one ~24 KB contiguous block — and the idle heap frees only
                    // ~49 KB with a ~21 KB largest block, so without this the PTT heap gate refuses it
                    // SILENTLY (no chime); freeing the canvas opens the contiguous room. (2) the screen is
                    // off anyway during recognition, so this also saves power. No overlay is drawn — the
                    // ready chime and the end beep are the feedback. Skipped when an app owns the buffer as
                    // its framebuffer (video), mirroring display_sleep(). Restored once the engine releases
                    // VH_PTT (i.e. AFTER the end beep), polled below. Same canvas-free pattern as radio/music.
                    s_voice_dark = true; g0_dark_since = now;
                    nucleo_ui_set_brightness(0);                   // backlight off; the stored level (s_brightness) is untouched
                    if (!s_app_direct) nucleo_app_release_buffers();
                    nucleo_voice_ptt(true);                        // engine up + "ready" chime, then it captures
                }
            }
        } else {                                               // released
            if (g0_ptt) {
                if (g0_app_ptt) { g0_app_ptt(false); g0_app_ptt = nullptr; }  // app PTT (recorder): stop + end cue
                else            { nucleo_voice_ptt(false); }                  // voice PTT → recognize (screen dark until end beep)
                g0_ptt = false;
            } else if (g0_down_ms != 0 && now - g0_down_ms >= 30 && now >= g0_cooldown_ms) {
                toggle_torch(); g0_cooldown_ms = now + 350;    // quick tap -> flashlight
            }
            g0_down_ms = 0;
        }

        // Drain pending reminders. While remote the device UI is suspended and the web client
        // owns the alert, so we discard rather than show; otherwise raise the banner.
        if (s_notify_q) {
            notify_t nq;
            while (xQueueReceive(s_notify_q, &nq, 0) == pdTRUE) {
                if (!s_remote && !s_notify_show) {
                    snprintf(s_notify_title, sizeof s_notify_title, "%s", nq.title);
                    snprintf(s_notify_body, sizeof s_notify_body, "%s", nq.body);
                    s_notify_show = true; s_dirty = true; s_notify_at = now;
                }
            }
        }
        // Auto-dismiss the banner after a few seconds: a glanceable peek, not a modal that blocks
        // the device until a keypress. (Any key still dismisses it sooner.)
        if (s_notify_show && now - s_notify_at >= 6000) {
            s_notify_show = false; d.fillScreen(BG); s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
        }

        // Capture confirm toast: a small pill, overlaid for ~1.2 s WITHOUT freezing the frame.
        // The game keeps running underneath; we just force a redraw each frame so the pill stays
        // composited on top until it expires (then one more redraw clears it).
        if (s_shot_toast_until && now < s_shot_toast_until + 120) s_dirty = true;

        int clients = nucleo_remote_enabled() ? nucleo_ws_shell_count() : 0;
        // An in-flight OTA forces listening mode too (frees the 32 KB canvas), even with no shell client and
        // even if the auto-handoff toggle is off — a flash is the heaviest download and needs that contiguous block.
        bool listen = clients > 0 || s_force_listen;
        if (listen) {
            remote_gone_ms = 0;
            if (!s_remote) { enter_remote(clients); last_act = now; }   // give the first face its full SCREEN_OFF_MS window (else a connect >SCREEN_OFF_MS after the last local key blanks it instantly)
            else if (clients != s_remote_clients) { s_remote_clients = clients; s_remote_force = true; }
        } else if (s_remote) {
            if (remote_gone_ms == 0) remote_gone_ms = now;
            if (now - remote_gone_ms >= REMOTE_GRACE_MS) exit_remote();
        }
        if (s_remote) {
            // Smartwatch monitor: TAB (or Left/Right) flips RAM <-> CPU face; otherwise just refresh
            // the live values at ~3 Hz (the signature gate blits only when a number changed).
            // Zero-cost smart nav: TAB / arrows page, number keys 1..N jump straight to a face, and
            // after 30 s of no input the watch drifts back to its CONNECTION home (ambient behaviour).
            nucleo_key_t rk = nucleo_kbd_read();
            if (rk.key != NK_NONE) { last_remote_input = now; last_act = now; }
            if (now - focus_chk >= 3000) { focus_chk = now; web_focus_update(); }  // key/signal added mid-session?
            if      (rk.key == NK_TAB || rk.key == NK_RIGHT) remote_set_face(s_remote_face + 1);
            else if (rk.key == NK_LEFT)                      remote_set_face(s_remote_face - 1);
            else if (rk.key == NK_CHAR && rk.ch >= '1' && rk.ch <= '0' + NFACES) remote_set_face(rk.ch - '1');
            if (s_remote_face != FACE_INFO && now - last_remote_input >= 30000) remote_set_face(FACE_INFO);
            // !s_voice_dark: a PTT on the device blanks the watch and frees its canvas for the recognizer —
            // don't re-acquire/redraw it here mid-session (it would starve the recognizer's contiguous block).
            if (!s_voice_dark && !s_disp_off && (s_remote_force || now - last_remote >= 300)) { last_remote = now; remote_monitor_render(); }
            vTaskDelay(pdMS_TO_TICKS(80));   // poll TAB responsively; render stays gated to ~3 Hz + signature
            continue;
        }

        nucleo_key_t nk = nucleo_kbd_read();
        if (nk.key != NK_NONE) {
            last_act = now;                      // any key counts as activity (resets the idle screen-off timer)
            // Screen-off wake: CC "screen off" action blanked the backlight; first key restores and is swallowed.
            if (s_wake_pending) { s_wake_pending = false; nucleo_app_set_brightness(s_wake_saved_bright); continue; }
            // Capture: Fn+P is an OS-wide screenshot -> a real full-frame image in /data/Screenshots
            // (EVERYWHERE, games included — it used to save only a tiny cover for games, so "screenshot"
            // seemed broken in a game). While a game is foreground, Ctrl+P (or a plain 'C') instead
            // refreshes THAT game's carousel cover -> /data/GameShots/<id>.bmp, overwriting the old one.
            // Ctrl+P is the primary binding: games routinely use letter keys for play, so a bare 'C'
            // gets eaten by gameplay — the Ctrl modifier keeps the cover gesture unambiguous. Saved after
            // the next composite (the canvas holds the clean frame). Key consumed; a toast confirms.
            const nucleo_app_def_t *cap_def = active_def();
            bool cap_in_game = (cap_def && cap_def->category && !strcmp(cap_def->category, "Games"));
            unsigned cap_mods = nucleo_kbd_mods();
            bool cap_is_p    = (nk.ch == 'p' || nk.ch == 'P' || nk.ch == 0x10);   // Ctrl+P may arrive as DLE (0x10)
            bool cap_fnp     = (cap_mods & NK_MOD_FN)   && cap_is_p;              // Fn+P   -> OS-wide screenshot
            bool cap_ctrlp   = (cap_mods & NK_MOD_CTRL) && cap_is_p;              // Ctrl+P -> game carousel cover
            bool cap_cover   = cap_in_game && (cap_ctrlp || nk.ch == 'c' || nk.ch == 'C');
            if (nk.key == NK_CHAR && (cap_cover || cap_fnp)) {
                if (cap_cover) s_cover_req = true; else s_shot_req = true;   // in-game cover (Ctrl+P/'C') vs Fn+P screenshot
                s_dirty = true;
            } else if (s_torch) {                // flashlight overlay is up — any key turns it off
                torch_off();
            } else if (s_notify_show) {           // a reminder banner is up — any key dismisses it
                s_notify_show = false; d.fillScreen(BG); s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
            } else if (nk.key == NK_TAB) {
                // A foreground app may claim TAB for its own overlay (e.g. ANIMA settings);
                // otherwise TAB toggles the global Control Center.
                if (!s_control_center && s_app_tab) { s_app_tab(); s_dirty = true; }
                else {
                    s_control_center = !s_control_center; s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
                    if (s_control_center) {
                        launcher_render_control_center_open();
                        // Direct-draw apps release the 32 KB canvas; the CC needs it for flicker-free
                        // composite. The app won't draw while the CC is up, so borrow the canvas now
                        // and give it back when the CC closes (s_cc_canvas_borrowed tracks the loan).
                        if (s_app_direct) s_cc_canvas_borrowed = nucleo_screen_acquire();
                    } else {
                        launcher_render_control_center_close();
                        if (s_cc_canvas_borrowed) { nucleo_screen_release(); s_cc_canvas_borrowed = false; }
                        d.fillScreen(BG);
                    }
                }
            } else if (s_control_center) {
                // The sheet now owns Back/Left and pops hierarchically (edit -> row -> header);
                // it returns CC_CLOSE (2) when Back is pressed past the top level, CC_SCREEN_OFF (3) for the screen-off action.
                int r = launcher_render_control_center_key(nk.key, nk.ch);
                if (r == 2) { s_control_center = false; s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true; launcher_render_control_center_close(); if (s_cc_canvas_borrowed) { nucleo_screen_release(); s_cc_canvas_borrowed = false; } d.fillScreen(BG); }
                else if (r == 3) { s_control_center = false; launcher_render_control_center_close(); if (s_cc_canvas_borrowed) { nucleo_screen_release(); s_cc_canvas_borrowed = false; } d.fillScreen(BG); s_wake_saved_bright = nucleo_app_brightness(); nucleo_app_set_brightness(0); s_wake_pending = true; }
                else if (r == 4) { const char *lid = launcher_render_control_center_launch_id(); s_control_center = false; launcher_render_control_center_close(); if (s_cc_canvas_borrowed) { nucleo_screen_release(); s_cc_canvas_borrowed = false; } d.fillScreen(BG); if (lid) nucleo_app_launch_id(lid); }
                else if (r == 1) s_dirty = true;
            } else if (s_gamefront) {
                // GameFront owns input while up (s_active is still -1). It returns an id to launch.
                char gid[24];
                int r = gamefront_key(nk.key, nk.ch, gid, sizeof gid);
                if (r == GF_CLOSE)       { s_gamefront = false; d.fillScreen(BG); s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true; }
                else if (r == GF_LAUNCH) { s_gamefront = false; s_gf_return = true; launch_by_id(gid); }
                else if (r == GF_REDRAW) s_dirty = true;
            } else if (s_active == -1) {
                // The LIST always animates on a key (s_dirty inside handle_launcher) — it's buffered, one
                // blit, no flicker. The CHROME (top/bottom bars) is drawn DIRECT, so mark it dirty ONLY when
                // its content really changed (category/filter/Wi-Fi/hint) — not on every scroll key, which
                // re-wiped the static bars and flickered. ANTI-FLICKER.md rule 69.
                handle_launcher(nk.key, nk.ch);
                if (launcher_render_chrome_changed()) s_chrome_dirty = true;
            } else if (nk.key == NK_BACK || nk.key == NK_LEFT) {
                // The app may intercept Back/Left (e.g. ANIMA's sheet returns to its chat); only
                // close to the launcher when it doesn't consume the key.
                if (!(s_app_back && s_app_back(nk.key))) close_app();
            } else {
                const nucleo_app_def_t *def = active_def(); if (def && def->on_key) def->on_key(nk.key, nk.ch);
            }
        }
        
        bool voice_active = nucleo_voice_is_listening();
        static bool s_voice_was_active = false;
        if (voice_active != s_voice_was_active) {
            s_voice_was_active = voice_active;
            s_dirty = true;
            if (!voice_active) { d.fillScreen(BG); s_chrome_dirty = true; s_hint_dirty = true; }
        }
        if (voice_active && now - last_tick >= 100) s_dirty = true; // force animation update

        // Voice result toast: a PTT recognized locally otherwise ONLY speaks its answer (silent
        // without the TTS pack) and shows nothing. Surface it on-screen. Polled because the engine
        // can't push to a native consumer. Skipped while remote (the web client owns the UI), in
        // test mode (the Voce console renders results itself), and for a successful app launch
        // (the app opening is the feedback). One toast per dispatch, via the seq counter.
        static uint32_t s_voice_res_seen = 0;
        if (!s_remote && !voice_active && !nucleo_voice_test_mode()) {
            nucleo_voice_result_t vr;
            if (nucleo_voice_last_result(&vr) && vr.seq != s_voice_res_seen) {
                s_voice_res_seen = vr.seq;
                bool launched_ok = (vr.action == ANIMA_ACT_LAUNCH) && vr.launched;
                if (!launched_ok) {
                    char body[96];
                    if (!vr.matched)
                        snprintf(body, sizeof body, "Non ho capito: %.40s", vr.sentence);
                    else if (vr.action == ANIMA_ACT_LAUNCH)
                        snprintf(body, sizeof body, "App non trovata: %.40s", vr.reply);
                    else
                        snprintf(body, sizeof body, "%.80s", vr.reply[0] ? vr.reply : vr.sentence);
                    nucleo_notify_post("Voce", body);
                }
            }
        }

        if (now - last_tick >= 200) {
            last_tick = now;
            // !s_voice_dark: across the whole PTT session the back-buffer is freed, so an app's on_tick
            // must not draw (it would re-acquire the 32 KB canvas and starve the recognizer). This spans
            // the async dispatch tail AFTER button release too, not just while GO is held.
            if (!s_control_center && !s_notify_show && !s_torch && !s_voice_dark) {
                const nucleo_app_def_t *def = active_def();
                if (def && def->on_tick) def->on_tick();      // foreground apps tick ~5x/s
            }
        }
        // Once-a-second refresh: the launcher chrome (clock digits) or, when the SISTEMA tab
        // is up, the Control Center info strip (it embeds a clock + free RAM). RAPIDE and RETE
        // tabs have no time-varying content, so skip the 1 Hz tick there. Suppressed during PTT.
        if (!s_torch && !s_voice_dark && now - last_clock >= 1000) {
            last_clock = now;
            if (s_control_center && launcher_render_control_center_tab() == 2) s_dirty = true;
            // Launcher home: tick only the clock digits in place — repainting the whole chrome
            // every second flashed all three bars black. Skip while the list is mid-scroll so
            // the direct write can't race the band blit.
            else if (s_active == -1 && !s_gamefront && !s_chrome_dirty && !s_dirty) launcher_render_clock_tick();
        }

        // Smooth-scroll toward the focused row (the only animation). Suppressed during the PTT session
        // (launcher_render_step_scroll composites into the back-buffer = re-acquires the freed canvas).
        if (s_active == -1 && !s_gamefront && !s_control_center && !s_torch && !s_voice_dark && launcher_render_step_scroll()) s_dirty = true;
        if (s_gamefront && !s_control_center && !s_torch && !s_voice_dark && gamefront_step()) s_dirty = true;

        bool fg_taken = false;   // did the foreground-app branch own the screen this iteration? (idle-reblit guard)
        if (s_voice_dark) {
            // PTT low-power capture: the panel is off and the 32 KB back-buffer is freed for the
            // recognizer (held across the async dispatch tail too). Draw NOTHING — never fall through
            // to a branch that would re-acquire the canvas and starve the engine of its contiguous
            // block. The ready chime + end beep are the feedback; the panel returns after the beep.
            s_dirty = false;
        } else if (s_torch) {
            // Flashlight overlay: a single full-white blit, then idle until a key/tap dismisses it.
            if (s_dirty) { s_dirty = false; d.fillScreen(0xFFFF); }
        } else if (voice_active) {
            if (s_dirty) { s_dirty = false; draw_voice_overlay(); }   // overlay paints DIRECT to d (no back-buffer)
        } else if (s_notify_show) {
            if (s_dirty) { s_dirty = false; draw_notify_banner(); }
        } else if (s_control_center) {
            if (s_dirty) { s_dirty = false; launcher_render_control_center(); }
        } else if (s_gamefront) {
            // GameFront composites its whole screen into the shared canvas and blits once, like the
            // Control Center. Drawn only on s_dirty (a key), so image decode happens ~once per press.
            if (s_dirty) { s_dirty = false; gamefront_render(); }
        } else if (s_active == -1) {
            // Chrome is static during a scroll -> draw it directly, only when it changes. The
            // scrolling list band composites into the shared back-buffer and blits once.
            if (s_chrome_dirty) { s_chrome_dirty = false; launcher_render_update_chrome(); launcher_render_chrome(); }
            if (s_dirty) { s_dirty = false; launcher_render_list(); }
        } else {
            // A foreground app owns the content area. Composite its whole frame into the shared
            // back-buffer and blit once (no cleared-then-redrawn flicker). The buffer persists
            // across launcher<->app transitions, so nothing re-allocates here. Only the blocking
            // media modals (video/music) release it for the decoder, then draw direct.
            fg_taken = true;
            const nucleo_app_def_t *def = active_def();
            // Live apps (mic spectrum) drive their own data source: poll it every loop (~50 Hz) and
            // ask for a blit ONLY when a new frame actually arrived, so the full-frame composite+push
            // runs at the data rate (~31 Hz), not the loop rate. Blitting duplicate frames at 50 Hz is
            // exactly the cadence redraw ANTI-FLICKER.md #4 forbids. Null for ordinary apps (no change).
            if (s_app_poll && s_app_poll()) s_dirty = true;
            if (s_dirty) {
                s_dirty = false;
                // Apps that freed the canvas (ANIMA) pin direct draw: don't lazily re-acquire the
                // 32 KB buffer here — that would reclaim the RAM the app gave to its index/worker.
                M5Canvas *cv = s_app_direct ? nullptr : nucleo_screen();
                if (cv) {
                    nucleo_app_set_gfx(cv);                     // route on_draw into the canvas
                    cv->fillSprite(BG);
                    if (def && def->on_draw) def->on_draw();
                    nucleo_app_set_gfx(nullptr);                // back to the real display

                    // Pending capture (Fn+P / 'C'): the canvas now holds exactly the CLEAN frame on
                    // screen (no overlay yet). Save it FIRST so the toast pill never leaks into the
                    // saved image, then arm a non-blocking confirm toast (no frozen preview).
                    if (s_shot_req) {                              // Fn+P -> real full-frame screenshot, always
                        s_shot_req = false;
                        static int seq = 0; char nm[24]; snprintf(nm, sizeof nm, "shot_%d", ++seq);
                        s_shot_toast_ok = gamefront_save_screenshot(nm);
                        s_shot_toast_until = now + 1200;
                    }
                    if (s_cover_req) {                             // 'C' in a game -> refresh its carousel cover
                        s_cover_req = false;
                        s_shot_toast_ok = (def && def->id && def->id[0]) ? gamefront_save_canvas_cover(def->id) : false;
                        s_shot_toast_until = now + 1200;
                    }
                    // Keep each game's carousel cover fresh from LIVE gameplay — but capture ONCE per session
                    // (~8 s in, past the opening menu), NOT every few seconds. The old 6 s cadence did a
                    // synchronous SD write mid-fight, a periodic hitch that read as "it keeps taking
                    // screenshots". Statics reset on each game's Solo-boot relaunch, so covers still refresh.
                    if (def && def->category && !strcmp(def->category, "Games") && def->id && def->id[0]) {
                        static int64_t s_cover_at = 0; static bool s_cover_done = false;
                        if (!s_cover_done) {
                            if (s_cover_at == 0)          s_cover_at = now + 8000;   // arm once, let the menu pass
                            else if (now >= s_cover_at) { s_cover_done = true; gamefront_save_canvas_cover(def->id); }
                        }
                    }

                    // Non-blocking confirm toast: draw the pill OVER the live frame (after the save,
                    // so it isn't captured). The game keeps running underneath; this just overlays it.
                    bool toast = s_shot_toast_until && now < s_shot_toast_until;
                    if (toast) {
                        draw_shot_toast(cv, s_shot_toast_ok);
                        // draw_shot_toast leaves Font2 on the canvas; fillSprite does NOT reset GFX
                        // state, so the next game on_draw that print()s without setFont() would render
                        // oversized. Reset to the default font/size after the overlay.
                        cv->setFont(&fonts::Font0); cv->setTextSize(1);
                    }

                    // Idle-reblit guard: skip the SPI push when this frame is byte-identical to the last
                    // one we pushed (a static menu / paused / game-over screen whose poll keeps firing).
                    // Force it the frame the app regains the screen (was_fg false) or while the toast is
                    // overlaid (the pill must reach the panel even on an otherwise-static frame).
                    uint32_t h = fg_canvas_hash(cv);
                    bool force = !s_fg_was_fg || toast;
                    if (force || h != s_fg_last_hash) {
                        s_fg_last_hash = h;
                        // Push only the bands that changed (fullscreen apps reclaim the hint rows). Unchanged
                        // regions are never re-sent, so the static parts of an animating screen can't tear.
                        blit_dirty_bands(cv, s_app_fullscreen ? H : (H - HINT), force);
                    }
                } else if (def && def->on_draw) {
                    // Canvas unavailable (a background decoder holds the RAM): draw direct. App
                    // draws already clear their own region (app_ui_title/app_ui_list fillRect), so
                    // no extra clear here — that double-clear was visible flicker. Batch the whole
                    // repaint into ONE SPI transaction so the clear→text gap is as short as possible.
                    d.startWrite();
                    def->on_draw();
                    d.endWrite();
                    // Direct-draw screen (ANIMA, or any app that freed the canvas): the frame is on the
                    // PANEL, so grab it from there — works even in Solo boot (no httpd, no canvas), as a
                    // local SD write. Read it later over Wi-Fi via /api/fs once back at the launcher.
                    if (s_shot_req) {                              // Fn+P -> real full-frame screenshot from the panel
                        s_shot_req = false;
                        static int seq = 0; char nm[24]; snprintf(nm, sizeof nm, "shot_%d", ++seq);
                        bool ok = gamefront_save_panel_screenshot(nm);
                        nucleo_notify_post("Screenshot", ok ? "Saved" : "Capture failed");
                    }
                    if (s_cover_req) {                             // Ctrl+P/'C' cover — canvas is freed here, so read the PANEL
                        s_cover_req = false;                       // (panel_cover un-swaps the readback bytes -> correct colours)
                        bool ok = (def && def->id && def->id[0]) ? gamefront_save_panel_cover(def->id) : false;
                        nucleo_notify_post("Screenshot", ok ? "Cover salvata" : "Cover non disponibile");
                    }
                }
                // The hint bar sits below the app's clipped blit, so the app frame never
                // touches it. Repaint it only when the hint text/colors actually changed —
                // not on every animation frame (e.g. ANIMA's "pensa…" spinner) which made the
                // footer flicker by clearing+redrawing it ~50x/s.
                if (s_hint_dirty && !s_app_fullscreen) { s_hint_dirty = false; launcher_render_hint_bar(); }
            }
        }
        s_fg_was_fg = fg_taken;   // remember for next iteration's idle-reblit force decision
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
