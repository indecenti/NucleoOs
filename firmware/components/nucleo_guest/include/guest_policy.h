// guest_policy — pure decision core for running NucleoOS as a GUEST of a foreign boot manager
// (bmorcelli/M5Launcher). No ESP-IDF dependency: host-compiled and gated by tools/anima-host/
// guest-check.mjs (`npm run guest:test`). The device glue (partition scan, NVS read, deep sleep)
// lives in nucleo_guest.c. Full design + the Launcher boot model: docs/m5launcher.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP-IDF partition-table constants (esp_partition.h values; duplicated so this file stays host-pure).
#define GUEST_PT_TYPE_APP        0x00
#define GUEST_PT_TYPE_DATA       0x01
#define GUEST_PT_SUB_FACTORY     0x00
#define GUEST_PT_SUB_OTA_0       0x10
#define GUEST_PT_SUB_OTA_MAX     0x1F
#define GUEST_PT_SUB_TEST        0x20
#define GUEST_PT_SUB_FAT         0x81
#define GUEST_PT_SUB_SPIFFS      0x82
#define GUEST_PT_SUB_LITTLEFS    0x83
#define GUEST_PT_ENTRY_SIZE      32
#define GUEST_PT_MAX_BYTES       0xC00   // the table lives in one 3 KB window at 0x8000

typedef struct {
    uint8_t  type, subtype;
    uint32_t offset, size;
    char     label[17];
} guest_part_t;

// Parse a binary ESP-IDF partition table (0xAA50 entries, stops at 0xEBEB MD5 row / 0xFFFF erased).
// Returns the number of entries written to `out` (at most `cap`), or -1 if the first row is not a
// partition entry. Labels are NUL-terminated copies of the 16-byte field.
int guest_parse_table(const uint8_t *buf, size_t len, guest_part_t *out, int cap);

// True when the LIVE partition table belongs to a foreign boot manager: it carries an app partition of
// subtype `test` (M5Launcher installs itself there, and its bootloader boots it on power-on) that is not
// the image we are running from. NucleoOS's own table (firmware/partitions.csv) never has one, so a
// stand-alone install is never classified as hosted.
bool guest_is_hosted(const guest_part_t *parts, int n, uint32_t running_offset);

// How "back to the Launcher" can be triggered, mirroring the Launcher bootloader's decision
// (myLibBuilder launcher_keyboot: bootloader_start.c, should_try_launcher_test_partition):
//   LauncherOnKey > 0  -> ONLY a held GPIO boots the Launcher (power-on and deep sleep do not)
//   DDLB != 0          -> power-on only (deep-sleep wake boots the app)
//   otherwise          -> power-on AND deep-sleep wake boot the Launcher
typedef enum {
    GUEST_RET_DEEP_SLEEP = 0,   // a timer deep sleep lands in the Launcher: we can do it for the user
    GUEST_RET_POWER_CYCLE,      // tell the user: switch off and on
    GUEST_RET_HOLD_KEY,         // tell the user: hold the configured key while restarting
} guest_return_t;

guest_return_t guest_return_mode(bool has_ddlb, uint8_t ddlb, bool has_on_key, int32_t on_key_gpio);

// Self-OTA writes esp_ota_get_next_update_partition(), which under a boot manager is ANOTHER installed
// app's slot (or our own running one). Only the stand-alone layout owns its A/B banks.
bool guest_self_ota_allowed(bool hosted);

// What M5Launcher's SD/merged-image installer (Launcher src/sd_functions.cpp, updateFromSD) extracts
// from an image's embedded table: the FIRST app partition of subtype factory/ota_0/test, plus every
// SPIFFS/LittleFS/FAT data partition (label kept). Lets the host gate prove our shipped table stays
// installable by Launcher.
typedef struct {
    bool     app_found;
    uint32_t app_offset, app_declared_size;
    int      n_data;
    char     data_label[8][17];
    uint8_t  data_subtype[8];
} guest_install_plan_t;

void guest_launcher_plan(const guest_part_t *parts, int n, guest_install_plan_t *plan);

#ifdef __cplusplus
}
#endif
