// nucleo_guest.c — device glue for guest mode under M5Launcher. Decisions: guest_policy.c (host-gated).
#include "nucleo_guest.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "nvs.h"

static const char *TAG = "guest";

// Launcher's settings, written by the Launcher app and read by its bootloader
// (myLibBuilder launcher_keyboot: bootloader_start.c). We only ever READ them.
#define LAUNCHER_NVS_NS       "launcher"
#define LAUNCHER_KEY_DDLB     "DDLB"            // u8:  disable "deep-sleep wake boots the Launcher"
#define LAUNCHER_KEY_ON_KEY   "LauncherOnKey"   // i32: GPIO that must be held to boot the Launcher (-1 = off)

static int8_t s_hosted = -1;   // -1 = not scanned yet
static bool   s_own_layout;    // our own two-bank table (guest_layout_is_own), valid once s_hosted >= 0

bool nucleo_guest_hosted(void)
{
    if (s_hosted >= 0) return s_hosted == 1;
    // App partitions only: that is where a boot manager's `test` slot lives. A table has at most a
    // handful; 16 covers Launcher's multi-app layouts with room to spare.
    guest_part_t parts[16];
    int n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it && n < (int)(sizeof parts / sizeof parts[0]); it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        parts[n].type = (uint8_t)p->type;
        parts[n].subtype = (uint8_t)p->subtype;
        parts[n].offset = p->address;
        parts[n].size = p->size;
        snprintf(parts[n].label, sizeof parts[n].label, "%s", p->label);
        n++;
    }
    esp_partition_iterator_release(it);   // NULL-safe
    const esp_partition_t *run = esp_ota_get_running_partition();
    bool hosted = guest_is_hosted(parts, n, run ? run->address : 0);
    s_own_layout = guest_layout_is_own(parts, n, run ? run->address : 0);
    s_hosted = hosted ? 1 : 0;
    if (hosted)
        ESP_LOGW(TAG, "installed by M5Launcher (running '%s' @0x%lx): self-OTA off, updates via Launcher",
                 run ? run->label : "?", run ? (unsigned long)run->address : 0UL);
    else if (!s_own_layout)
        ESP_LOGW(TAG, "partition layout is not NucleoOS's own two-bank table (running '%s'): self-OTA off",
                 run ? run->label : "?");
    return hosted;
}

// Both guards: not under M5Launcher (blacklist) AND our own A/B layout (whitelist) — the next OTA slot is
// then provably our other bank, never another installed app.
bool nucleo_guest_self_ota_allowed(void)
{
    bool hosted = nucleo_guest_hosted();          // scans once, fills s_own_layout
    return guest_self_ota_allowed(hosted) && s_own_layout;
}

bool nucleo_guest_layout_own(void) { (void)nucleo_guest_hosted(); return s_own_layout; }

guest_return_t nucleo_guest_return_mode(void)
{
    // Launcher's settings can only change inside the Launcher (i.e. after we left): read NVS once.
    static int8_t s_mode = -1;
    if (s_mode >= 0) return (guest_return_t)s_mode;
    bool has_ddlb = false, has_key = false;
    uint8_t ddlb = 0;
    int32_t key = -1;
    nvs_handle_t h;
    if (nvs_open(LAUNCHER_NVS_NS, NVS_READONLY, &h) == ESP_OK) {   // missing namespace = Launcher defaults
        has_ddlb = nvs_get_u8(h, LAUNCHER_KEY_DDLB, &ddlb) == ESP_OK;
        has_key = nvs_get_i32(h, LAUNCHER_KEY_ON_KEY, &key) == ESP_OK;
        nvs_close(h);
    }
    s_mode = (int8_t)guest_return_mode(has_ddlb, ddlb, has_key, key);
    return (guest_return_t)s_mode;
}

bool nucleo_guest_return_to_launcher(void)
{
    if (!nucleo_guest_hosted()) return false;
    if (nucleo_guest_return_mode() != GUEST_RET_DEEP_SLEEP) return false;
    ESP_LOGW(TAG, "returning to M5Launcher (timer deep sleep)");
    // The wake is a CORE_DEEP_SLEEP reset: Launcher's bootloader (SKIP_VALIDATE_IN_DEEP_SLEEP=n) re-runs
    // partition selection and boots its `test` slot. RTC_NOINIT one-shot flags are magic-guarded AND
    // gated on ESP_RST_SW, so nothing of ours fires on the way through.
    esp_sleep_enable_timer_wakeup(100 * 1000);
    esp_deep_sleep_start();   // never returns
    return false;
}
