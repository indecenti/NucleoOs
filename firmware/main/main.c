// NucleoOS boot (device-first): the Cardputer screen is the OS home.
//   NVS -> UI(display+keyboard) -> SD -> first-run wizard (AP or join Wi-Fi)
//   -> network -> services -> home screen. The web UI is the rich remote companion.
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nucleo_storage.h"
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
#include "nucleo_log.h"
#include "nucleo_power.h"
#include "nucleo_usbmsc.h"
#include "nucleo_voice.h"
#include "nucleo_ir.h"
#include "esp_ota_ops.h"
#include "esp_system.h"     // esp_reset_reason(): why the PREVIOUS boot ended (panic vs WDT vs brownout)
#include "nucleo_board.h"   // HLOG(): compile-time-gated heap tracing (see NUCLEO_HEAPLOG)

static const char *TAG = "nucleoos";

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

void app_main(void)
{
    ESP_LOGI(TAG, "NucleoOS booting — last reset: %s", reset_reason_str(esp_reset_reason()));
    HMEM("boot-start");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Power-loss-safe config store on internal flash. Mount before networking, which
    // reads setup.json (load_config) on the very first call. Independent of the SD.
    if (nucleo_storage_mount_cfg() != ESP_OK)
        ESP_LOGW(TAG, "config store (littlefs) unavailable; setup may not persist safely");

    nucleo_event_init();
    nucleo_log_init();                      // start capturing logs early (RAM ring -> /api/logs)
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
    HMEM("after-ui");

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

    bool sd_ok = false;
    if (nucleo_storage_mount() == ESP_OK) {
        sd_ok = true;
        nucleo_storage_provision();
        nucleo_storage_refresh();
        if (nucleo_registry_load() != ESP_OK)
            ESP_LOGW(TAG, "registry not loaded");
    } else {
        ESP_LOGE(TAG, "no usable SD card");
    }

    // Bring up networking + services FIRST so the device is never bricked and the web UI
    // always works, even while the on-device keyboard is being verified/tuned.
    HMEM("after-sd");
    nucleo_setup_apply_network();           // AP on first boot (no saved config)
    nucleo_power_init();                     // DFS: scale CPU down when idle (battery), Wi-Fi up
    nucleo_discovery_start(nucleo_setup_device_name());
    if (sd_ok && nucleo_recorder_init() != ESP_OK)  // PDM mic -> WAV recorder
        ESP_LOGW(TAG, "recorder mic init failed");
    nucleo_auth_init();                     // generate pairing PIN + load session tokens (gates fs/ota/rec/ws)
    nucleo_arb_init();                       // heavy-work arbiter: one TLS/SD/heap job at a time (no OOM race)
    if (nucleo_ir_init() != ESP_OK)          // IR LED (GPIO44) via RMT; serves /api/ir/* + TV-B-Gone
        ESP_LOGW(TAG, "IR init failed (GPIO busy?)");
    HMEM("after-network");
    ESP_ERROR_CHECK(nucleo_httpd_start());
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
    if (!nucleo_setup_is_complete())
        nucleo_setup_run();

    ESP_LOGI(TAG, "boot complete");

    HMEM("after-httpd");
    // ANIMA: offline assistant. L0 orchestrator is ready; higher tiers plug in later.
    nucleo_anima_init("it");
    if (sd_ok) nucleo_tts_init("it");            // voce offline concatenativa (no-op se /data/tts/it manca)
#if CONFIG_NUCLEO_ANIMA_BENCH
    nucleo_anima_benchmark();                // Phase 0: log the three numbers (opt-in)
#endif
#if CONFIG_NUCLEO_ANIMA_HDC_SELFTEST
    nucleo_anima_hdc_selftest();             // HDC/KGE reasoning proven on-device (opt-in)
#endif
    // AVCEB voice engine: PTT (FN key) → DTW template match → nucleo_anima_query() → action.
    // Initialized after anima (needs anima_try_lock) and after SD (loads .tpl templates).
    if (nucleo_voice_init() != ESP_OK)
        ESP_LOGW(TAG, "voice engine init failed (mic busy?)");

    // Launch the Native OS App environment (Wear OS style) instead of the static setup loop.
    HMEM("after-anima");
    nucleo_app_register_builtins();
    nucleo_setup_register_apps();
    nucleo_app_run();
}
