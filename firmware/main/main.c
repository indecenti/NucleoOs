// NucleoOS boot (device-first): the Cardputer screen is the OS home.
//   NVS -> UI(display+keyboard) -> SD -> first-run wizard (AP or join Wi-Fi)
//   -> network -> services -> home screen. The web UI is the rich remote companion.
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task.h"          // ESP_TASK_MAIN_PRIO for the dedicated ANIMA Solo task
#include "esp_heap_caps.h"
#include "nucleo_storage.h"
#include "nucleo_i18n.h"
#include "nucleo_prefs.h"   // persistent brightness/volume/mute (settings.json power.*), applied at boot
#include "nucleo_registry.h"
#include "nucleo_eventbus.h"
#include "nucleo_ui.h"
#include "nucleo_setup.h"
#include "nucleo_httpd.h"
#include "nucleo_arb.h"
#include "nucleo_auth.h"
#include "nucleo_recorder.h"
#include "nucleo_discovery.h"
#include "nucleo_app.h"
#include "nucleo_anima.h"
#include "nucleo_tts.h"
#include "nucleo_audio.h"
#include "nucleo_kbd.h"     // system I2C bus owner (ADV) -> shared with the codec
#include "nucleo_codec.h"   // ES8311 (Cardputer ADV); no-op on the original board
#include "nucleo_imu.h"     // BMI270 tilt sensor (Cardputer ADV); no-op on the original board
#include "nucleo_ble.h"     // boot-time BLE controller memory reclaim (Bluetooth OFF by default)
#include "nucleo_log.h"
#include "nucleo_power.h"
#include "nucleo_usbmsc.h"
#include "nucleo_voice.h"
#include "nucleo_ir.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"   // esp_app_get_description(): real running-image version for the boot banner
#include "esp_system.h"     // esp_reset_reason(): why the PREVIOUS boot ended (panic vs WDT vs brownout)
#include "nucleo_board.h"   // HLOG(): compile-time-gated heap tracing (see NUCLEO_HEAPLOG)

static const char *TAG = "nucleoos";

// void-returning shim so nucleo_anima_l1_unload_if_idle (returns bool) matches the audio reclaim hook
// signature (void(*)(void)). Calling through a mismatched function-pointer type is UB, hence the wrap.
static void nucleo_anima_l1_unload_if_idle_void(void) { (void)nucleo_anima_l1_unload_if_idle(); }

// ANIMA Solo registers ONLY the assistant (builtin registrar; not in a public header — forward-declared
// here, mirroring how nucleo_app_register_builtins fans out to the per-app registrars internally).
extern void nucleo_register_anima(void);
extern void nucleo_register_recorder(void);   // Recorder Solo: dedicated boot for heavy cloud transcribe/summarize
extern int  nucleo_app_solo_app(void);        // which Solo profile (1=ANIMA, 2=Recorder) is active
extern bool nucleo_app_solo_needs_speech(void); // only ANIMA Solo brings up TTS+voice; others skip them
extern bool nucleo_app_solo_is_generic(void);   // generic Solo (heavy game via NX_SOLO): register all builtins
extern bool nucleo_app_solo_is_server(void);    // Web Client server-Solo: keep httpd + auth up, skip the rest
extern void nucleo_register_remote(void);        // server Solo registers ONLY the Remote Control app

// ANIMA Solo runs nucleo_app_run() on a DEDICATED large-stack task (the 8 KB main task overflows running
// the ANIMA UI). nucleo_app_run never returns (Esc reboots), so vTaskDelete is just defensive.
static void anima_solo_task(void *arg) { (void)arg; nucleo_app_run(); vTaskDelete(NULL); }

// Human label for the last reset cause. This is THE first question when a device reboots on its own:
// PANIC/abort = a crash or a failed assert (often heap OOM downstream); INT/TASK_WDT = a hang (a task
// blocked too long); BROWNOUT = the 3V3 rail sagged (power/battery, NOT software); POWERON/SW = clean.
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "POWERON (cold boot)";
        case ESP_RST_SW:       return "SW (esp_restart)";
        case ESP_RST_PANIC:    return "PANIC (crash/abort/assert)";
        case ESP_RST_INT_WDT:  return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT: return "TASK_WDT (task hung)";
        case ESP_RST_WDT:      return "WDT (other watchdog)";
        case ESP_RST_BROWNOUT: return "BROWNOUT (power rail sag)";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP wake";
        case ESP_RST_USB:      return "USB reset";
        case ESP_RST_JTAG:     return "JTAG reset";
        default:               return "UNKNOWN";
    }
}

// Log free/min/largest heap in one line — quick snapshot of RAM state at each boot stage.
// HLOG (not ESP_LOGI) so a production build (-DNUCLEO_HEAPLOG=0) strips these entirely.
#define HMEM(lbl) HLOG(TAG, "heap " lbl ": free=%u min=%u largest=%u frag=%d%%", \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT), \
    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), \
    (int)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) > 0 \
        ? 100 - (int)(100ULL * heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) \
                      / heap_caps_get_free_size(MALLOC_CAP_DEFAULT)) : 0))

// After an OTA the new image boots in PENDING_VERIFY; if we never confirm it, the
// bootloader rolls back to the previous image on the next reboot. We reach this point
// only once core services (storage attempt + network + HTTP server) are up, so the
// image is healthy: confirm it and cancel the pending rollback. (No-op on factory.)
static void ota_confirm_if_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "OTA image confirmed valid (rollback cancelled)");
        else
            ESP_LOGW(TAG, "failed to confirm OTA image");
    }
}

// Boot breadcrumb: log to the serial console AND append to /sd/boot_trace.txt, so the LAST stage
// reached before a hang is visible on the SD card (readable on a PC) even with no serial console.
// fopen before the SD mount just returns NULL (skipped); bootmark_begin() truncates a fresh trace.
// Steps taken BEFORE the SD mounts are buffered in RAM and flushed by bootmark_begin() (which starts
// a fresh file at SD-mount time); after that bootmark() appends straight to the card.
static char s_pre[480]; static int s_pre_n = 0;   // pre-mount breadcrumb buffer
static bool s_sd_up = false;
static void bootmark(const char *step)
{
    unsigned fr = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned lg = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "BOOTSTEP %s free=%u largest=%u", step, fr, lg);
    if (!s_sd_up) {                               // SD not mounted yet -> buffer it
        int rem = (int)sizeof(s_pre) - s_pre_n;
        if (rem > 1) { int w = snprintf(s_pre + s_pre_n, rem, "%s  free=%u largest=%u\n", step, fr, lg);
                       s_pre_n += (w > 0 && w < rem) ? w : (rem - 1); }
        return;
    }
    FILE *f = fopen(NUCLEO_SD_MOUNT "/boot_trace.txt", "a");
    if (f) { fprintf(f, "%s  free=%u largest=%u\n", step, fr, lg); fclose(f); }
}
static void bootmark_begin(void)                  // call right after the SD mounts: fresh file + flush pre-mount log
{
    s_sd_up = true;
    FILE *f = fopen(NUCLEO_SD_MOUNT "/boot_trace.txt", "w");
    if (f) { fputs("=== boot ===\n", f); if (s_pre_n > 0) fwrite(s_pre, 1, (size_t)s_pre_n, f); fclose(f); }
}

void app_main(void)
{
    ESP_LOGI(TAG, "NucleoOS booting — last reset: %s", reset_reason_str(esp_reset_reason()));
    // Version banner — first thing on the serial console (COM), so the running image is identifiable
    // without the network. Version == PROJECT_VER from firmware/version/* + git; matches /api/status.
    {
        const esp_app_desc_t *appd = esp_app_get_description();
        ESP_LOGI(TAG, "NucleoOS %s | proj %s | built %s %s | idf %s | %s",
                 appd ? appd->version : "?", appd ? appd->project_name : "?",
                 appd ? appd->date : "?", appd ? appd->time : "?",
                 appd ? appd->idf_ver : "?", CONFIG_IDF_TARGET);
    }
    HMEM("boot-start");
    bootmark("boot-start");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    bootmark("nvs");

    // Reclaim the BLE controller's idle DRAM (~tens of KB) for the OS/ANIMA on a PSRAM-less, heap-starved
    // chip. Bluetooth is OFF by default (a niche Security tool); enabling it in the BLE app + rebooting
    // keeps the radio. MUST run before any BLE init — BLE is on-demand, so nothing has touched it yet.
    nucleo_ble_boot_reclaim();
    bootmark("bt-reclaim");

    // Power-loss-safe config store on internal flash. Mount before networking, which
    // reads setup.json (load_config) on the very first call. Independent of the SD.
    if (nucleo_storage_mount_cfg() != ESP_OK)
        ESP_LOGW(TAG, "config store (littlefs) unavailable — falling back to NVS for setup/network persistence");
    bootmark("cfg");

    nucleo_event_init();
    nucleo_log_init();                      // start capturing logs early (RAM ring -> /api/logs)
    bootmark("event+log");
    {   // DIAG (WiFi-visible): the boot reset cause + the ANIMA tier active at the last reset. The INFO
        // line above predates the ring and is stripped at WARN; this WARN line lands in /api/logs so a
        // crash is diagnosable over WiFi (no serial). g_anima_stage survives a warm reboot (RTC mem).
        extern uint32_t g_anima_stage; extern uint8_t g_anima_phase;
        esp_reset_reason_t rr = esp_reset_reason();
        ESP_LOGW(TAG, "DIAG last-reset=%s anima_phase=0x%02X anima_stage=0x%02X",
                 reset_reason_str(rr), (unsigned)g_anima_phase, (unsigned)g_anima_stage);
        g_anima_stage = 0; g_anima_phase = 0;   // consume the breadcrumbs for the next boot cycle
    }
    nucleo_ui_init();                       // M5GFX display + Cardputer keyboard
    nucleo_codec_init(nucleo_kbd_i2c_bus()); // ADV: attach ES8311 to the keyboard's I2C bus (no-op on original)
    nucleo_imu_init(nucleo_kbd_i2c_bus());   // ADV: attach BMI270 tilt sensor to the same I2C bus (no-op on original)
    HMEM("after-ui");
    bootmark("ui-init");

    // USB Mass Storage mode: if the USB Drive app asked for it (flag set + reboot), run ONLY the
    // dedicated MSC loop — mount the SD to get the card handle, then hand its raw blocks to the
    // host. The normal OS never starts, so nothing else touches the card while the PC owns it.
    if (nucleo_usbmsc_pending()) {
        nucleo_storage_mount();
        nucleo_usbmsc_run();                // never returns (key/reset restarts into the normal OS)
    }

    // Animated boot identity: a glowing atomic nucleus with orbiting electrons (Nucleo = nucleus).
    // Blocks ~2.5 s on its own canvas while the heap is still clean; any key skips it. The final
    // frame stays on the panel through the SD/network bringup below until the launcher draws.
    nucleo_ui_boot_splash();
    bootmark("after-splash");

    bool sd_ok = false;
    if (nucleo_storage_mount() == ESP_OK) {
        sd_ok = true;
        bootmark_begin();                   // SD up: start a fresh trace on the card (/sd/boot_trace.txt)
        bootmark("sd-mounted");
        nucleo_storage_provision();         bootmark("sd-provision");
        nucleo_storage_refresh();           bootmark("sd-refresh");
        nucleo_i18n_load();                 // system UI language (settings.json ui.language) for native apps

        // Restore the user's saved display/audio prefs from the SAME settings.json the web Settings app
        // writes (power.display_brightness / volume / muted). Without this only the language survived a
        // reboot; brightness/volume/mute always came back at their compiled defaults. The setters just
        // store state (+ set the backlight, already up from the splash), so this is safe pre-httpd/audio.
        {
            int bri = nucleo_app_brightness(), vol = nucleo_audio_volume();
            bool mute = nucleo_audio_is_muted();
            if (nucleo_prefs_load(&bri, &vol, &mute)) {
                nucleo_app_set_brightness(bri);
                nucleo_audio_set_volume(vol);
                nucleo_audio_set_mute(mute);
            }
        }

        if (nucleo_registry_load() != ESP_OK)
            ESP_LOGW(TAG, "registry not loaded");
        bootmark("registry");
    } else {
        ESP_LOGE(TAG, "no usable SD card");
    }

    // Graceful-shutdown hook: ESP-IDF runs nucleo_storage_sync() on every clean esp_restart()
    // (OTA, /api/reboot, native SISTEMA ▸ Riavvia, Wi-Fi reset, USB-MSC) to flush + unmount the
    // filesystems — the OS `sync` that stops FAT from corrupting on reboot. Panics skip it by design.
    if (esp_register_shutdown_handler(nucleo_storage_sync) != ESP_OK)
        ESP_LOGW(TAG, "could not register storage shutdown hook");

    // ANIMA Solo personality (see nucleo_app.h): the user opened the assistant from the full OS, which
    // set an RTC flag + rebooted here. Bring up ONLY what the assistant needs — display + SD + Wi-Fi +
    // power + arbiter + ANIMA/TTS/voice — and SKIP httpd / mDNS / recorder / auth / IR / the launcher's
    // app registry. The heap then comes up large and UNFRAGMENTED (no 18 KB httpd block carving it), the
    // only way this PSRAM-less chip fits online TLS + L1 + voice at once. Consumed once (RTC -> latch).
    const bool solo = nucleo_anima_solo_pending();
    const bool solo_rec = solo && (nucleo_app_solo_app() == 2);   // Recorder profile: pure cloud transcribe, no voice/TTS
    const bool solo_srv = solo && nucleo_app_solo_is_server();     // Web Client: httpd + auth stay UP; everything else skipped for max heap
    if (solo_srv)  ESP_LOGW(TAG, "Web Client Solo boot: httpd + auth + mDNS UP; skipping recorder/IR/voice/TTS/L1/calendar for max web-OS heap");
    else if (solo) ESP_LOGW(TAG, "%s Solo boot: skipping httpd/mDNS/recorder/auth/IR%s",
                       solo_rec ? "Recorder" : "ANIMA", solo_rec ? " + TTS/voice" : "");

    // Bring up networking + services FIRST so the device is never bricked and the web UI
    // always works, even while the on-device keyboard is being verified/tuned.
    HMEM("after-sd");
    nucleo_setup_apply_network();           bootmark("network");   // AP on first boot (no saved config) — Wi-Fi stays up in Solo (cloud)
    nucleo_power_init();                     bootmark("power");      // DFS: scale CPU down when idle (battery)
    // mDNS (discovery) is started AFTER httpd_start (further down), NOT here: it costs ~12 KB at boot, and on
    // the ADV that 12 KB was the difference between httpd_start fitting its ~26 KB need and OOM-failing on the
    // ~30 KB boot heap. The failed attempt left port 80 bound, so the retry's listen() hit EADDRINUSE
    // (errno 112) -> ESP_ERROR_CHECK -> abort -> intermittent reboot loop ("0.104 si riavvia"). Starting httpd
    // FIRST (with mDNS's 12 KB still free) makes the boot deterministic; mDNS then comes up post-httpd
    // (~16 KB free, fits) and advertises a few hundred ms later — functionally irrelevant, and a rare mDNS
    // failure is non-fatal (the device stays reachable by IP), unlike the httpd abort.
    if ((!solo || solo_rec) && sd_ok && nucleo_recorder_init() != ESP_OK)  // PDM mic — NEEDED in Recorder Solo (record IN the dedicated boot); skipped only in ANIMA Solo
        ESP_LOGW(TAG, "recorder mic init failed");
    bootmark("recorder");
    if (!solo || solo_srv) nucleo_auth_init();  // pairing PIN + session tokens — needed whenever httpd runs (full OS + Web Client Solo)
    bootmark("auth");
    nucleo_arb_init();                       bootmark("arb");        // heavy-work arbiter (one job at a time) — KEPT: ANIMA online needs it
    if (!solo && nucleo_ir_init() != ESP_OK) // IR LED (GPIO44) via RMT; serves /api/ir/* + TV-B-Gone — not in Solo
        ESP_LOGW(TAG, "IR init failed (GPIO busy?)");
    bootmark("ir");
    HMEM("after-network");
    bootmark("pre-httpd");
    // httpd MUST come up — the web OS is not optional, so we do NOT limp on without it (a half-booted OS
    // is worse than an honest stop). The deep ANIMA cascade is now off-loaded to a transient worker, so
    // this task is lean (18KB) and httpd_start is deterministic. If it EVER still fails, log it to serial
    // AND to the SD boot trace (readable without a serial console — the silent-freeze case), then halt:
    // error logged, nothing half-starts, no fallback.
    if (!solo || solo_srv) { esp_err_t he = nucleo_httpd_start();   // SKIPPED in Solo EXCEPT Web Client server-Solo, which exists to serve the web OS on a fresh heap (this 18 KB task is what carves the heap on this no-PSRAM chip)
      if (he != ESP_OK) {
          // ADV boot is on a knife-edge: httpd needs an ~18 KB CONTIGUOUS block and a rebuild can
          // fragment the heap just below it (largest ~17 KB < 18432) -> start fails -> abort -> reboot
          // loop (no degraded boot). Before giving up, free the 32 KB off-screen canvas (the launcher
          // re-acquires it lazily; worst case a little launcher flicker) and retry: 32 KB >> 18 KB, so
          // httpd then starts cleanly. The original board, which has more margin, never hits this path.
          ESP_LOGW(TAG, "httpd start failed (%s) — freeing the 32 KB canvas + settle, then retrying", esp_err_to_name(he));
          nucleo_app_release_buffers();
          // The failed attempt can leave the port-80 listen socket bound -> the retry's listen() then hits
          // EADDRINUSE (errno 112) -> abort. Give LWIP a moment to release it before re-listening.
          vTaskDelay(pdMS_TO_TICKS(500));
          he = nucleo_httpd_start();
      }
      if (he != ESP_OK) {
          ESP_LOGE(TAG, "httpd start FAILED (%s) — halting (no degraded boot)", esp_err_to_name(he));
          bootmark("httpd-FAILED");
          ESP_ERROR_CHECK(he);   // halt: nothing else starts
      } }
    bootmark("httpd");
    // mDNS — AFTER httpd so its ~12 KB stayed free for the httpd_start carve (see note above). HEAP-GATED:
    // on a degraded/tight boot (seen on the ADV: largest block fell to ~7 KB after httpd) mDNS would alloc
    // its responder down to a few dozen FREE bytes — leaving httpd with no working heap to serve requests
    // (it then RESETS every connection: /api/status, /api/ota, the web OS, all dead) AND spamming
    // "mdns_send: Cannot allocate memory". Discovery is a convenience (the device is always reachable by IP;
    // ota.ps1 already prefers IP, and web-focus stops mDNS the moment a client connects), so when the
    // contiguous heap is too low to run it without starving the SERVER, SKIP it and keep those bytes for httpd.
    if (!solo || solo_srv) {   // Web Client Solo advertises too (heap-gated): it stops the moment a shell connects (web-focus), so it costs nothing while driving
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 14336) nucleo_discovery_start(nucleo_setup_device_name());   // ~14 KB headroom: mDNS fits without starving httpd
        else ESP_LOGW(TAG, "mDNS SKIPPED at boot: largest free block %u < 14336 — reachable by IP, httpd keeps the heap", (unsigned)largest);
    }
    bootmark("discovery");
    ota_confirm_if_pending();               // healthy boot reached -> confirm OTA image, cancel rollback
    // Enrich the boot event so the journal's first line is a diagnosis seed: why the last boot ended,
    // the heap we came up with + the worst-ever watermark, and whether the SD mounted. Cheap (one
    // event at boot); the Log Viewer surfaces it and the "Diagnose" digest anchors uptime/resets on it.
    {
        char p[160];
        snprintf(p, sizeof p, "{\"ok\":true,\"rst\":\"%s\",\"free\":%u,\"min\":%u,\"sd\":%d}",
                 reset_reason_str(esp_reset_reason()),
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 sd_ok ? 1 : 0);
        nucleo_event_publish("system.boot", p);
    }

    // First-run wizard: choose Create AP or Join a Wi-Fi network (keyboard driven).
    // Setup lives on internal LittleFS (not the SD), so the wizard must run even with no SD
    // card inserted — otherwise a card-less device never gets asked how to connect.
    if (!solo && !nucleo_setup_is_complete())   // Solo is reached only from an already-configured OS -> wizard never needed
        nucleo_setup_run();
    bootmark("setup");

    ESP_LOGI(TAG, "boot complete");

    HMEM("after-httpd");
    // ANIMA: offline assistant. L0 orchestrator is ready; higher tiers plug in later. SKIPPED in Web Client
    // server-Solo: Remote Control has zero use for the on-device assistant, and nucleo_anima_l1_init() can
    // eagerly load the ~18 KB flat centroid slab (AKB5-absent SDs) right after httpd — exactly the contiguous
    // heap this mode exists to keep free for the browser's first, heaviest load. Skipping it also makes the
    // boot log honest (it already lists "L1" among the skipped subsystems). The web OS answers in its own
    // browser brain; /api/anima degrades to a graceful "not ready" (tier none) if the shell ever asks.
    if (!solo_srv) nucleo_anima_init("it");
    bootmark("anima");
    if (sd_ok && (!solo || nucleo_app_solo_needs_speech())) nucleo_tts_init("it");   // SKIP in non-speech Solo (e.g. Recorder), save heap
    bootmark("tts");
    // Voce a step su heap minuscolo (no PSRAM): se il task audio non trova lo stack contiguo, il player
    // libera l'indice L1 offline (se inattivo) e ritenta -> niente piu' "offline niente voce". main e'
    // l'unico punto che puo' dipendere sia da nucleo_audio sia da nucleo_anima, quindi cabliamo qui.
    nucleo_audio_set_reclaim_cb(nucleo_anima_l1_unload_if_idle_void);
#if CONFIG_NUCLEO_ANIMA_BENCH
    nucleo_anima_benchmark();                // Phase 0: log the three numbers (opt-in)
#endif
#if CONFIG_NUCLEO_ANIMA_HDC_SELFTEST
    nucleo_anima_hdc_selftest();             // HDC/KGE reasoning proven on-device (opt-in)
#endif
    // AVCEB voice engine: PTT (FN key) → DTW template match → nucleo_anima_query() → action.
    // Initialized after anima (needs anima_try_lock) and after SD (loads .tpl templates).
    if ((!solo || nucleo_app_solo_needs_speech()) && nucleo_voice_init() != ESP_OK)   // SKIP in non-speech Solo: frees mic+task heap
        ESP_LOGW(TAG, "voice engine init failed (mic busy?)");
    bootmark("voice");

    // Launch the Native OS App environment (Wear OS style) instead of the static setup loop.
    HMEM("after-anima");
    if (solo) {
        // Solo: register ONLY the target app. nucleo_app_run() auto-launches it and Esc reboots to the
        // full OS. (App .bss is linked-in regardless, so this is about isolation/clarity, not RAM.)
        if (solo_rec)                      { nucleo_register_recorder(); bootmark("app-recorder-solo"); }
        else if (solo_srv)                 { nucleo_register_remote();   bootmark("app-remote-solo"); }  // Web Client: only Remote Control (the server-listening watch)
        else if (nucleo_app_solo_is_generic()) { nucleo_app_register_builtins(); bootmark("app-game-solo"); }  // heavy game (NX_SOLO): register all so launch_id finds it
        else                               { nucleo_register_anima();    bootmark("app-anima-solo"); }
    } else {
        nucleo_app_register_builtins();      bootmark("app-builtins");
        nucleo_setup_register_apps();        bootmark("app-setup");
    }
    bootmark("pre-app-run");
    // Web Client server-Solo is the ONE Solo profile that keeps httpd UP, and httpd_start carves the heap:
    // afterwards the largest internal block is only ~7 KB on the ADV (~8.7 KB on the original). Creating a
    // NEW 8 KB Solo UI task then FAILS to allocate on the ADV -> app_main returns with no UI task -> the
    // screen freezes on the boot splash ("stuck at the intro", Web Client never appears). So server-Solo runs
    // nucleo_app_run() on the MAIN task, exactly like the full OS (whose entire launcher already runs there):
    // no extra stack to allocate, deterministic on both boards. The watch faces draw direct and there is no
    // inline TLS on this task (httpd owns its own), so the main stack is ample.
    if (solo && !solo_srv) {
        // ANIMA Solo runs like the USB-MSC personality: a DEDICATED task owns the assistant with a LARGE
        // stack. The 8 KB main task OVERFLOWS running ANIMA's UI (chat reflow + calendar/JSON parsing)
        // while its worker reads the SD — the real cause of the "still reboots" crash. app_main then
        // returns (its small task is deleted); only the Solo task, the on-demand query worker and the
        // audio task remain. Nothing else (no launcher chrome, no calendar service) competes for SD/display.
        // ANIMA runs the INLINE query+TLS on this task (needs 26 KB). Recorder + generic game Solo are
        // UI-only (no inline TLS) and SKIP httpd, so the heap stays large and 8 KB fits with margin. 26 KB is
        // ANIMA inline-TLS only (at pre-app-run the largest block is ~25 KB on the ADV, so it fits).
        uint32_t solo_stk = (solo_rec || nucleo_app_solo_is_generic()) ? 8192 : 26624;
        BaseType_t tcr = xTaskCreatePinnedToCore(anima_solo_task,
                                                 solo_rec ? "rec-solo" : "anima-solo",
                                                 solo_stk, NULL, ESP_TASK_MAIN_PRIO, NULL, 0);
        if (tcr != pdPASS) ESP_LOGE(TAG, "SOLO task create FAILED (stack %u, largest %u) — halting",
                                    (unsigned)solo_stk, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;   // main task exits; the Solo task is now the app's home
    }
    nucleo_app_run();   // full OS AND Web Client server-Solo: launcher/watch loop on the main task
}
