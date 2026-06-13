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
#include "nucleo_exclusive.h"  // declarative exclusive_flags: framework enters on open, restores on close
#include "launcher_theme.h"
#include "launcher_menu.h"
#include "launcher_render.h"
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
#include "nucleo_voice.h"      // Voice engine state for the PTT overlay
#include "nucleo_anima.h"      // anima_action_t for the voice result toast (include dir via CMakeLists)
}
#include "esp_http_server.h"
#include "esp_log.h"
#include "nucleo_auth.h"        // NUCLEO_AUTH_GUARD (include dir pulled in via CMakeLists)

#include "app_gfx.h"

// Network info (resolved at link; declared here for the stub screen, no component dep).
extern "C" const char *nucleo_setup_mode(void);
extern "C" const char *nucleo_setup_ip(void);

// Panel backlight (implemented in nucleo_ui). Wrapped below so brightness has one owner.
extern "C" void nucleo_ui_set_brightness(unsigned char b);

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

static void disp_deadman_cb(void *arg)
{
    (void)arg;
    if (s_disp_off) {                            // client went silent while the screen was dark
        nucleo_app_set_brightness(s_brightness);
        s_disp_off = false;
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
        nucleo_app_set_brightness(s_brightness);            // reapply the user's stored level
        s_disp_off = false;
        if (s_disp_deadman) esp_timer_stop(s_disp_deadman); // ESP_ERR_INVALID_STATE if idle: ignore
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

extern "C" esp_err_t nucleo_app_register_display(httpd_handle_t server)
{
    // Single-line designated init (fields in declaration order) so the api-spec generator
    // (tools/gen-api-spec.mjs) discovers the route from this source line.
    httpd_uri_t display = { .uri = "/api/display", .method = HTTP_POST, .handler = display_post };
    return httpd_register_uri_handler(server, &display);
}

// ---- registered foreground apps --------------------------------------------
#define MAX_APPS 32
static nucleo_app_def_t s_apps[MAX_APPS];
static int s_app_count;

void nucleo_app_register(const nucleo_app_def_t *app) { if (s_app_count < MAX_APPS) s_apps[s_app_count++] = *app; }

// Read access for the menu builder (plain C++ linkage; see launcher_menu.cpp).
int                     nucleo_app_count(void) { return s_app_count; }
const nucleo_app_def_t *nucleo_app_at(int i)   { return &s_apps[i]; }

// Built-in apps (each registers itself).
extern "C" void nucleo_register_info(void);
extern "C" void nucleo_register_clock(void);
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
extern "C" void nucleo_register_torch(void);
extern "C" void nucleo_register_evilportal(void);
extern "C" void nucleo_register_wifiatk(void);
extern "C" void nucleo_register_beacon(void);
extern "C" void nucleo_register_ethernet(void);

extern "C" void nucleo_register_voice(void);
extern "C" void nucleo_register_voicelab(void);
extern "C" void nucleo_register_ssh(void);
extern "C" void nucleo_register_wifi(void);
extern "C" void nucleo_register_ir(void);
extern "C" void nucleo_register_link(void);   // "Vicino" — device-to-device file/command transfer
extern "C" void nucleo_register_micspec(void);
extern "C" void nucleo_register_qr(void);

// Registration order defines BOTH the category order in the root menu (by first app seen)
// and the app order within each category. Ordered so everyday apps lead and System/Connect
// trail — matching the web emulator (Tools, Media, System, Connect).
void nucleo_app_register_builtins(void)
{
    nucleo_register_anima();                                                    // hoisted to Home top-level (launcher_build_menu); its "Tools" category is ignored
    nucleo_register_clock(); nucleo_register_torch(); nucleo_register_calc(); nucleo_register_qr(); nucleo_register_files();
    nucleo_register_calendar(); nucleo_register_notify(); nucleo_register_notepad(); nucleo_register_usb(); nucleo_register_usbkbd(); nucleo_register_ir();
    nucleo_register_radio(); nucleo_register_player(); nucleo_register_video(); nucleo_register_photos(); nucleo_register_recorder(); nucleo_register_micspec();  // Media
    nucleo_register_info(); nucleo_register_sysmon();                            // System
    nucleo_register_wifi(); nucleo_register_remote(); nucleo_register_ssh(); nucleo_register_link();  // Connect
    nucleo_register_voice(); nucleo_register_voicelab();                          // Voice Control + live console
    nucleo_register_evilportal(); nucleo_register_wifiatk(); nucleo_register_beacon(); nucleo_register_ethernet();  // Security (authorized testing)
}

// ---- app lifecycle ----------------------------------------------------------
static int s_active = -1;                  // index into s_apps, -2 = stub, -1 = launcher
static const MenuNode *s_stub = nullptr;   // app launched without a native impl
static bool s_dirty = true;                // animated list band needs a redraw
static bool s_chrome_dirty = true;         // status/hint chrome needs a redraw
static bool s_hint_dirty   = true;         // bottom hint bar needs a redraw (set on hint/color change)
static bool s_control_center = false;      // global Control Center overlay active
static bool s_torch = false;               // global flashlight overlay active (G0 quick-tap)
static int  s_torch_prev_bright = 100;     // brightness to restore when the torch overlay closes
static void (*s_app_tab)(void) = nullptr;  // foreground app's TAB handler (else Control Center)
static bool (*s_app_back)(int key) = nullptr; // foreground app's Back/Esc/Left interceptor (gets the key; else close_app)
static bool s_app_direct = false;          // app freed the shared canvas -> draw direct, don't re-acquire
static void (*s_app_ptt)(bool) = nullptr;  // foreground app owns the GO-hold (recorder PTT); else voice PTT
static bool s_app_excl   = false;          // framework entered exclusive for this app (declarative) -> it restores it

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

// Free ANIMA L1's ~18 KB RAM index when a foreground app opens — the assistant lives at the
// launcher, so while you're in any app that memory belongs to the app/audio decoder. The next
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
    s_app_back = nullptr; s_app_direct = false; // ...nor a Back claim / direct-draw pin; on_enter may set them
    s_app_ptt = nullptr;                        // ...nor a GO-hold (PTT) claim; the recorder sets it on_enter
    launcher_render_reset_hint_colors();        // default dark footer; on_enter may override (e.g. Torch)
    d.fillRect(0, 0, W, H - HINT, BG);
    if (def->on_enter) def->on_enter();
    ESP_LOGW("applaunch", "enter '%s' DONE free=%u", def && def->id ? def->id : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    s_dirty = true; s_hint_dirty = true;        // paint the footer once on entry
}
static void launch(const MenuNode *app)
{
    int idx = find_app(app->id);
    if (idx >= 0) { s_active = idx; s_stub = nullptr; open_app_def(&s_apps[idx]); }
    else          { s_active = -2; s_stub = app;     open_app_def(&STUB); }
}
static void launch_by_id(const char *id)
{
    int idx = find_app(id);
    if (idx >= 0) { s_active = idx; s_stub = nullptr; open_app_def(&s_apps[idx]); }
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

// Open the app associated with a file's extension (file manager "open-with"). Native
// viewers browse their own folder, so this opens the right app rather than the exact file.
void nucleo_app_launch_file(const char *path)
{
    if (!path) return;
    const char *ext = strrchr(path, '.');
    const char *id = nullptr;
    if (ext) {
        if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") ||
            !strcasecmp(ext, ".bmp") || !strcasecmp(ext, ".gif")) id = "photos";
        else if (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".wav")) id = "music";
        else if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".md") || !strcasecmp(ext, ".json") ||
                 !strcasecmp(ext, ".log") || !strcasecmp(ext, ".csv") || !strcasecmp(ext, ".ini")) id = "notepad";
    }
    if (id) launch_by_id(id);
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
    s_app_ptt = nullptr;                        // ...GO-hold returns to the voice recognizer
    launcher_render_reset_hint_colors();        // restore the default dark footer for the launcher
    s_active = -1; s_stub = nullptr; s_dirty = true; s_chrome_dirty = true;
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
void nucleo_app_set_direct_draw(bool on) { s_app_direct = on; }
int  nucleo_app_content_top(void)    { return 0; }
int  nucleo_app_content_height(void) { return H - HINT; }
void nucleo_app_request_draw(void)   { s_dirty = true; }

// Audio apps (radio, music) call this before starting playback: free the 32 KB shared canvas so
// the Helix MP3 decoder can grab the contiguous block it needs (else MP3InitDecoder fails OOM and
// playback is silent on this PSRAM-less chip). The launcher re-acquires the canvas lazily on
// return. NOTE: video does NOT call this — it BORROWS the canvas as its frame buffer instead.
void nucleo_app_release_buffers(void) { nucleo_screen_release(); }

// ---- launcher input ----------------------------------------------------------
// Keyboard-first, Wear OS style: arrows scroll, Enter opens, `/` opens the focused app's
// options, and any printable character type-to-filters (digits included — there is no
// numeric quick-select, which used to hijack digits the user meant for the filter).
static void handle_launcher(int k, char ch)
{
    if      (k == NK_UP)    launcher_set_sel(launcher_sel() - 1);
    else if (k == NK_DOWN)  launcher_set_sel(launcher_sel() + 1);
    else if (k == NK_ENTER) { const MenuNode *a = launcher_enter(); if (a) launch(a); }
    else if (k == NK_RIGHT) launcher_open_context();
    else if (k == NK_BACK || k == NK_LEFT) launcher_back();
    else if (k == NK_DEL)   launcher_filter_backspace();
    else if (k == NK_CHAR && ch > ' ') launcher_filter_push(ch);
    s_dirty = true;
}

// ---- remote handoff: a connected web client takes priority. While remote we suspend ALL
// local input + rendering and free the UI buffers, concentrating CPU+RAM on the server. ----
extern "C" int  nucleo_ws_client_count(void);
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
#define REMOTE_GRACE_MS 4000             // tolerate this long at 0 clients before reclaiming
#define REMOTE_IDLE_MS  200              // loop period while remote: yields CPU to the HTTP task
static bool s_remote = false;
static int  s_remote_clients = 0;

// ---- web focus + idle screen-off ------------------------------------------------------------
// Extra symbols, resolved at final link (main pulls these components in) — same escape hatch
// nucleo_exclusive.c uses, so nucleo_app needs no REQUIRES cycle. nucleo_voice_enable comes from
// the already-included nucleo_voice.h; nucleo_anima_l1_unload is declared above (line ~212).
extern "C" bool      nucleo_anima_teacher_configured(void);   // an online model key is set (Grok/Claude/Groq/OpenAI)
extern "C" void      nucleo_anima_l1_set_external_brain(bool); // force the offline ANIMA brain to stand down
extern "C" void      nucleo_discovery_stop(void);             // stop mDNS advertising (client already connected)
extern "C" esp_err_t nucleo_discovery_resume(void);           // resume mDNS advertising

static bool s_web_focus = false;            // deep RAM teardown active (online key + signal while a client drives)
static bool s_disp_sleep = false;           // idle screen-off active (backlight 0 + 32 KB framebuffer freed)
static bool s_voice_dark = false;           // PTT low-power capture: backlight 0 + canvas freed for the voice session
#define SCREEN_OFF_MS  10000                 // idle (no on-device key) before the panel sleeps
#define G0_HOLD_MS     350                   // GO held past this = Push-to-Talk; below = a torch tap

// When a web client is connected AND an online model key is set AND we have Wi-Fi signal, the offline
// ANIMA brain is redundant: stand it down (frees the L1 index, the cascade goes straight online) and
// pause the on-device-only services so every spare byte keeps the web OS + its apps stable. The HTTP
// server is KEPT up (the client lives on it) — unlike nucleo_exclusive, which suspends it for a heavy
// NATIVE app. Fully restored when the client leaves or the key/signal goes away.
static void web_focus_enter(void)
{
    if (s_web_focus) return;
    // The stand-down frees the L1 index: only under the spine gate (a cascade may be mid-query on the
    // worker/httpd task). If busy, DON'T latch s_web_focus — web_focus_update() retries next tick.
    if (!nucleo_anima_try_lock()) return;
    s_web_focus = true;
    nucleo_anima_l1_set_external_brain(true);   // offline brain off -> L1 index freed, queries go online
    nucleo_anima_unlock();
    // Voice stays ENABLED: on-device PTT now streams templates from SD (~7 KB, cheap),
    // and disabling it would break the Voice Manager web app and the voice→web command
    // routing — both need PTT live WHILE a web client is connected.
    nucleo_discovery_stop();                    // mDNS no longer needed (the client already found us)
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
    nucleo_discovery_resume();
    nucleo_event_publish("system.focus", "{\"active\":false}");
}
// Engage/release from the live predicate. Called on connect and periodically while remote, so adding a
// key mid-session takes effect. rssi()<0 means STA is associated WITH signal (0 in AP mode / unassociated)
// — without upstream Wi-Fi the cloud models are unreachable, so we KEEP the offline brain in that case.
static void web_focus_update(void)
{
    bool eligible = s_remote && nucleo_anima_teacher_configured() && nucleo_setup_rssi() < 0;
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

    M5Canvas *cv = nucleo_screen();
    if (cv) {
        cv->fillSprite(BG);
        watch_draw_face(cv, &w);
        d.setClipRect(0, 0, W, H); cv->pushSprite(0, 0); d.clearClipRect();   // ONE blit -> no flicker
    } else {
        d.startWrite(); d.fillScreen(BG); watch_draw_face(&d, &w); d.endWrite();   // OOM fallback: direct
    }
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
    // Hand the client every spare byte: the assistant's ~18 KB L1 index isn't needed while the device
    // is just a monitor, and freeing it gives the HTTP/TLS path contiguous RAM. It reloads from SD on
    // the next query once the client disconnects. Skipped if a query is running (no use-after-free).
    nucleo_anima_l1_unload_if_idle();
    web_focus_update();                                   // deep teardown if an online key + Wi-Fi signal
    if (!s_disp_sleep) remote_monitor_render();           // buffer is permanent; HTTP task runs in the rest of the heap
    char p[40]; snprintf(p, sizeof(p), "{\"active\":true,\"clients\":%d}", clients);
    nucleo_event_publish("system.remote", p);
}
static void exit_remote(void)
{
    s_remote = false; s_remote_clients = 0;
    web_focus_exit();                                     // restore offline ANIMA + voice + mDNS
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
    nucleo_ui_set_brightness(0);                 // backlight off; the stored level (s_brightness) is untouched
    if (!s_app_direct) nucleo_screen_release();  // give the 32 KB canvas back (unless an app owns that RAM)
    nucleo_event_publish("system.display", "{\"on\":false,\"reason\":\"idle\"}");
}
static void display_wake(void)
{
    if (!s_disp_sleep) return;
    s_disp_sleep = false;
    if (!s_app_direct) for (int i = 0; i < 6 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
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
    s_notify_q = xQueueCreate(4, sizeof(notify_t));
    nucleo_calendar_svc_start();

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

    for (;;) {
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time() / 1000;

        // Idle screen-off: while the panel sleeps we draw NOTHING (the 32 KB canvas is freed and given to
        // the web OS), but still track the web client so connect/disconnect + web-focus stay correct. Any
        // on-device key — or the G0 side button — wakes it.
        if (s_disp_sleep) {
            int clients = nucleo_remote_enabled() ? nucleo_ws_client_count() : 0;
            if (clients > 0) { remote_gone_ms = 0; if (!s_remote) enter_remote(clients); else s_remote_clients = clients; }
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
        if (s_remote && !s_disp_off && !s_torch && !s_voice_dark && !nucleo_voice_is_listening() && now - last_act >= SCREEN_OFF_MS) {
            display_sleep();
            continue;
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

        int clients = nucleo_remote_enabled() ? nucleo_ws_client_count() : 0;
        if (clients > 0) {
            remote_gone_ms = 0;
            if (!s_remote) enter_remote(clients);
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
            if (!s_voice_dark && (s_remote_force || now - last_remote >= 300)) { last_remote = now; remote_monitor_render(); }
            vTaskDelay(pdMS_TO_TICKS(80));   // poll TAB responsively; render stays gated to ~3 Hz + signature
            continue;
        }

        nucleo_key_t nk = nucleo_kbd_read();
        if (nk.key != NK_NONE) {
            last_act = now;                      // any key counts as activity (resets the idle screen-off timer)
            if (s_torch) {                       // flashlight overlay is up — any key turns it off
                torch_off();
            } else if (s_notify_show) {           // a reminder banner is up — any key dismisses it
                s_notify_show = false; d.fillScreen(BG); s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
            } else if (nk.key == NK_TAB) {
                // A foreground app may claim TAB for its own overlay (e.g. ANIMA settings);
                // otherwise TAB toggles the global Control Center.
                if (!s_control_center && s_app_tab) { s_app_tab(); s_dirty = true; }
                else {
                    s_control_center = !s_control_center; s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true;
                    if (s_control_center) launcher_render_control_center_open();
                    else { launcher_render_control_center_close(); d.fillScreen(BG); }
                }
            } else if (s_control_center) {
                // The sheet now owns Back/Left and pops hierarchically (edit -> row -> header);
                // it returns 2 only when Back is pressed past the top level, which closes it.
                int r = launcher_render_control_center_key(nk.key, nk.ch);
                if (r == 2) { s_control_center = false; s_dirty = true; s_chrome_dirty = true; s_hint_dirty = true; launcher_render_control_center_close(); d.fillScreen(BG); }
                else if (r == 1) s_dirty = true;
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
        // Once-a-second refresh: the launcher chrome (clock digits) or, when it is up, the
        // Control Center (its clock + Now Playing time). Suppressed during the PTT session (panel off).
        if (!s_torch && !s_voice_dark && now - last_clock >= 1000) {
            last_clock = now;
            if (s_control_center) s_dirty = true;
            // Launcher home: tick only the clock digits in place — repainting the whole chrome
            // every second flashed all three bars black. Skip while the list is mid-scroll so
            // the direct write can't race the band blit.
            else if (s_active == -1 && !s_chrome_dirty && !s_dirty) launcher_render_clock_tick();
        }

        // Smooth-scroll toward the focused row (the only animation). Suppressed during the PTT session
        // (launcher_render_step_scroll composites into the back-buffer = re-acquires the freed canvas).
        if (s_active == -1 && !s_control_center && !s_torch && !s_voice_dark && launcher_render_step_scroll()) s_dirty = true;

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
            const nucleo_app_def_t *def = active_def();
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
                    d.setClipRect(0, 0, W, H - HINT);           // canvas is taller (shared w/ video) -> clip
                    cv->pushSprite(0, 0);                       // one blit -> no flicker
                    d.clearClipRect();
                } else if (def && def->on_draw) {
                    // Canvas unavailable (a background decoder holds the RAM): draw direct. App
                    // draws already clear their own region (app_ui_title/app_ui_list fillRect), so
                    // no extra clear here — that double-clear was visible flicker. Batch the whole
                    // repaint into ONE SPI transaction so the clear→text gap is as short as possible.
                    d.startWrite();
                    def->on_draw();
                    d.endWrite();
                }
                // The hint bar sits below the app's clipped blit, so the app frame never
                // touches it. Repaint it only when the hint text/colors actually changed —
                // not on every animation frame (e.g. ANIMA's "pensa…" spinner) which made the
                // footer flicker by clearing+redrawing it ~50x/s.
                if (s_hint_dirty) { s_hint_dirty = false; launcher_render_hint_bar(); }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
