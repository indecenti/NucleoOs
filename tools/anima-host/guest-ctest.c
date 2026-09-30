// Host gate for M5Launcher guest mode: compile the REAL decision core
// (firmware/components/nucleo_guest/guest_policy.c) and prove the invariants on the PC.
// Wired as `npm run guest:test` (tools/anima-host/guest-check.mjs), which first encodes
// firmware/partitions.csv into a binary ESP-IDF table and passes its path as argv[1].
//
// Invariants under test:
//   1. Our shipped table (partitions.csv) is NEVER classified as hosted -> stand-alone is untouched.
//   2. A table M5Launcher writes (its `test` slot + our app in an ota_N slot) IS hosted, from any slot.
//   3. Self-OTA is allowed exactly when not hosted.
//   4. "Back to Launcher" mirrors the Launcher bootloader: key > DDLB > deep sleep.
//   5. M5Launcher's merged-image installer can take our image: it finds the app at ota_0 and carries
//      the `cfg` LittleFS partition across WITH its label (nucleo_storage mounts it by label).
//   6. The table parser is strict: bad magic, erased flash and the MD5 row end it.
#include "guest_policy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0, g_total = 0;
#define CHECK(cond, name) do { g_total++; if (!(cond)) { g_fail++; printf("FAIL %-58s\n", name); } } while (0)

static void put_entry(uint8_t *e, uint8_t type, uint8_t sub, uint32_t off, uint32_t size, const char *label)
{
    memset(e, 0, GUEST_PT_ENTRY_SIZE);
    e[0] = 0xAA; e[1] = 0x50; e[2] = type; e[3] = sub;
    for (int i = 0; i < 4; i++) { e[4 + i] = (uint8_t)(off >> (8 * i)); e[8 + i] = (uint8_t)(size >> (8 * i)); }
    strncpy((char *)e + 12, label, 16);
}

static const guest_part_t *find(const guest_part_t *p, int n, const char *label)
{
    for (int i = 0; i < n; i++) if (!strcmp(p[i].label, label)) return &p[i];
    return NULL;
}

int main(int argc, char **argv)
{
    // ── 1 + 5: the table we actually ship ─────────────────────────────────────────────────────────
    uint8_t own[GUEST_PT_MAX_BYTES];
    memset(own, 0xFF, sizeof own);
    size_t own_len = 0;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (f) { own_len = fread(own, 1, sizeof own, f); fclose(f); }
    }
    CHECK(own_len >= GUEST_PT_ENTRY_SIZE, "shipped table loaded (partitions.csv encoded)");
    guest_part_t op[24];
    int on = guest_parse_table(own, own_len, op, 24);
    CHECK(on >= 4, "shipped table parses");
    const guest_part_t *ota0 = find(op, on, "ota_0");
    CHECK(ota0 && ota0->type == GUEST_PT_TYPE_APP && ota0->subtype == GUEST_PT_SUB_OTA_0, "shipped table has app ota_0");
    CHECK(!guest_is_hosted(op, on, ota0 ? ota0->offset : 0), "shipped table, running ota_0 -> NOT hosted");
    const guest_part_t *ota1 = find(op, on, "ota_1");
    CHECK(ota1 && !guest_is_hosted(op, on, ota1->offset), "shipped table, running ota_1 -> NOT hosted");
    CHECK(guest_self_ota_allowed(guest_is_hosted(op, on, ota0 ? ota0->offset : 0)), "shipped table keeps self-OTA");
    for (int i = 0; i < on; i++)
        CHECK(!(op[i].type == GUEST_PT_TYPE_APP && op[i].subtype == GUEST_PT_SUB_TEST), "shipped table has no `test` app slot");

    guest_install_plan_t plan;
    guest_launcher_plan(op, on, &plan);
    CHECK(plan.app_found, "Launcher installer finds our app");
    CHECK(ota0 && plan.app_offset == ota0->offset, "Launcher installer takes ota_0 (the image the merge writes)");
    bool cfg_kept = false;
    for (int i = 0; i < plan.n_data; i++)
        if (!strcmp(plan.data_label[i], "cfg") && plan.data_subtype[i] == GUEST_PT_SUB_SPIFFS) cfg_kept = true;
    CHECK(cfg_kept, "Launcher installer carries `cfg` across with its label");

    // ── 2: a table as M5Launcher writes it (support_files/custom_8Mb2.csv + two installed apps) ─────
    uint8_t lt[GUEST_PT_MAX_BYTES];
    memset(lt, 0xFF, sizeof lt);
    put_entry(lt + 0 * 32, 0x01, 0x02, 0x9000,   0x5000,   "nvs");
    put_entry(lt + 1 * 32, 0x01, 0x00, 0xE000,   0x2000,   "otadata");
    put_entry(lt + 2 * 32, 0x00, 0x20, 0x10000,  0x170000, "app0");        // the Launcher itself
    put_entry(lt + 3 * 32, 0x01, 0x03, 0x180000, 0x10000,  "coredump");
    put_entry(lt + 4 * 32, 0x00, 0x10, 0x190000, 0x280000, "app1");        // another firmware
    put_entry(lt + 5 * 32, 0x00, 0x11, 0x410000, 0x290000, "app2");        // NucleoOS
    put_entry(lt + 6 * 32, 0x01, 0x82, 0x6A0000, 0x70000,  "cfg");
    lt[7 * 32] = 0xEB; lt[7 * 32 + 1] = 0xEB;                              // MD5 row
    guest_part_t hp[24];
    int hn = guest_parse_table(lt, sizeof lt, hp, 24);
    CHECK(hn == 7, "Launcher table parses to 7 rows (MD5 row ends it)");
    CHECK(guest_is_hosted(hp, hn, 0x410000), "Launcher table, running app2 -> hosted");
    CHECK(guest_is_hosted(hp, hn, 0x190000), "Launcher table, running app1 -> hosted");
    CHECK(!guest_is_hosted(hp, hn, 0x10000), "the Launcher itself is not a guest");
    CHECK(!guest_self_ota_allowed(true), "hosted -> self-OTA refused");
    const guest_part_t *cfg = find(hp, hn, "cfg");
    CHECK(cfg && cfg->subtype == GUEST_PT_SUB_SPIFFS && cfg->size == 0x70000, "hosted cfg found by label");

    // ── 4: return-to-Launcher policy ─────────────────────────────────────────────────────────────
    CHECK(guest_return_mode(false, 0, false, -1) == GUEST_RET_DEEP_SLEEP, "Launcher defaults -> deep sleep");
    CHECK(guest_return_mode(true, 0, true, -1) == GUEST_RET_DEEP_SLEEP, "DDLB=0, key off -> deep sleep");
    CHECK(guest_return_mode(true, 1, false, -1) == GUEST_RET_POWER_CYCLE, "DDLB=1 -> power cycle");
    CHECK(guest_return_mode(true, 1, true, 0) == GUEST_RET_POWER_CYCLE, "key GPIO0 is 'not configured' (bootloader: > 0)");
    CHECK(guest_return_mode(false, 0, true, 5) == GUEST_RET_HOLD_KEY, "key configured -> hold key");
    CHECK(guest_return_mode(true, 1, true, 5) == GUEST_RET_HOLD_KEY, "key beats DDLB (bootloader order)");
    CHECK(guest_return_mode(false, 1, false, 5) == GUEST_RET_DEEP_SLEEP, "unread values are ignored");

    // ── 6: parser strictness ─────────────────────────────────────────────────────────────────────
    guest_part_t tmp[4];
    uint8_t bad[64]; memset(bad, 0, sizeof bad);
    CHECK(guest_parse_table(bad, sizeof bad, tmp, 4) == -1, "no 0xAA50 magic -> -1");
    CHECK(guest_parse_table(NULL, 0, tmp, 4) == -1, "NULL buffer -> -1");
    CHECK(guest_parse_table(lt, sizeof lt, tmp, 4) == 4, "cap is honoured");
    uint8_t erased[64]; memset(erased, 0xFF, sizeof erased); put_entry(erased, 0, 0x10, 0x10000, 0x1000, "a");
    CHECK(guest_parse_table(erased, sizeof erased, tmp, 4) == 1, "erased flash ends the table");
    CHECK(!guest_is_hosted(NULL, 3, 0), "NULL parts -> not hosted");
    guest_launcher_plan(NULL, 3, &plan);
    CHECK(!plan.app_found && plan.n_data == 0, "NULL parts -> empty plan");

    printf("guest-policy: %d/%d passed%s\n", g_total - g_fail, g_total, g_fail ? "  (FAILURES)" : "");
    return g_fail ? 1 : 0;
}
