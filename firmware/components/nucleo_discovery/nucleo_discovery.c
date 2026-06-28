#define LOG_LOCAL_LEVEL ESP_LOG_NONE   // privacy: don't log the device's mDNS identity to the console
#include "nucleo_discovery.h"
#include "esp_log.h"
#include "esp_app_desc.h"   // esp_app_get_description(): advertise the real firmware version in mDNS
#include "mdns.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "discovery";

static char s_devid[64];          // remembered so security apps can stop/resume mDNS around an attack
static bool s_up;

esp_err_t nucleo_discovery_start(const char *device_id)
{
    if (device_id && device_id[0]) snprintf(s_devid, sizeof s_devid, "%s", device_id);
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err)); return err; }
    device_id = s_devid;

    mdns_hostname_set(device_id);
    mdns_instance_name_set("NucleoOS");

    // TXT records let a client identify the device before connecting. The version is the real
    // running-image version from the app descriptor (= PROJECT_VER), copied into a static buffer
    // so the TXT item points at storage that outlives this call.
    static char s_ver[40];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_ver, sizeof s_ver, "%s", app ? app->version : "?");
    mdns_txt_item_t txt[] = {
        { "os", "NucleoOS" },
        { "ver", s_ver },
        { "api", "/api/status" },
    };
    err = mdns_service_add("NucleoOS", "_nucleoos", "_tcp", 80, txt, 3);
    if (err != ESP_OK) { ESP_LOGW(TAG, "service_add: %s", esp_err_to_name(err)); return err; }

    ESP_LOGI(TAG, "mDNS up: %s.local, _nucleoos._tcp:80", device_id);
    s_up = true;
    return ESP_OK;
}

void nucleo_discovery_stop(void)
{
    if (!s_up) return;
    mdns_free();                  // kills the responder and all advertised services — no on-air mDNS
    s_up = false;
}

esp_err_t nucleo_discovery_resume(void)
{
    if (s_up || !s_devid[0]) return ESP_OK;
    return nucleo_discovery_start(s_devid);
}
