// fido_efuse_key — derive the 32-byte credential wrapping key.
//
// Best case: the S3 eFuse HMAC key block (esp_hmac). The key material lives in a
// read-protected eFuse and never appears in flash or RAM, so dumping the flash
// can't clone the authenticator — the single biggest weakness of a firmware that
// keeps its wrapping key in NVS. If no HMAC key is burned we fall back to a
// random NVS-stored key (with a warning) and NEVER burn an eFuse on our own.
#include "fido_port.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "fido_key";

int fido_derive_devkey(uint8_t out[32], bool *hardware) {
    // Domain-separated label so this key is distinct from any other HMAC use.
    static const char label[] = "NucleoOS-FIDO-devkey-v1";
    esp_err_t e = esp_hmac_calculate(HMAC_KEY0, label, sizeof(label) - 1, out);
    if (e == ESP_OK) {
        if (hardware) *hardware = true;
        ESP_LOGI(TAG, "wrapping key derived from eFuse HMAC (clone-resistant)");
        return 0;
    }

    if (hardware) *hardware = false;
    nvs_handle_t h;
    if (nvs_open("fido", NVS_READWRITE, &h) != ESP_OK) return -1;
    size_t len = 32;
    e = nvs_get_blob(h, "devkey", out, &len);
    if (e != ESP_OK || len != 32) {
        esp_fill_random(out, 32);
        nvs_set_blob(h, "devkey", out, 32);
        nvs_commit(h);
        ESP_LOGW(TAG, "no eFuse HMAC key burned; created a random NVS wrapping key");
    } else {
        ESP_LOGW(TAG, "wrapping key from NVS (burn an eFuse HMAC key for clone resistance)");
    }
    nvs_close(h);
    return 0;
}
