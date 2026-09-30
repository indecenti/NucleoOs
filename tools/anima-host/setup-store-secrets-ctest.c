// Host gate for nucleo_setup's three-tier config store (firmware/components/nucleo_setup/setup_store.c),
// compiled with the REAL cJSON from ESP-IDF. Proves the removable SD card never receives a Wi-Fi or
// hotspot password while first-boot recovery keeps working:
//   S1  save: /cfg + NVS hold the full document; the SD mirror has no password member and no password value
//   S2  the redacted mirror keeps what recovery needs (SSIDs, priorities, recency, setup flag, names)
//   S3  load order /cfg -> NVS -> SD; a flash wipe (no /cfg, no NVS) recovers from the SD mirror
//   S4  a legacy SD mirror holding passwords is flagged dirty; one re-save scrubs it, and an SD-only
//       legacy recovery still hands the passwords back so the re-save migrates them into /cfg + NVS
//   S5  every tier stays best-effort: no card, no /cfg, NVS down, all down — and the mount is never created
//   S6  allocation failure at EVERY malloc of a save: the SD never gets the full text (a legacy dirty
//       copy is removed instead), no leak, no crash; a failed first print is -1 and touches nothing
//   S7  member detection: names only (an SSID *valued* "pass" is no secret), escapes, spacing, nesting, case
//   S8  card hygiene when the mirror can't be rewritten: a dirty copy is deleted, a clean stale one kept;
//       an older firmware's orphan .tmp with the secret is removed on load; the zero-heap file probe
//       finds a member straddling its 64-byte read chunks at every offset
//   A1-A6  hotspot credentials (firmware/components/nucleo_setup/ap_creds.c): per-device defaults, the
//       DELIBERATE open choice is never re-minted (the bug: every AP restart used to mint a password over
//       it), setter bounds, legacy-default upgrade, and the setup.json round-trip through this store —
//       an open hotspot stays open across reboot AND flash-wipe recovery, a secured one is re-minted
// Runs in a scratch cwd: ./cfg/config stands in for the LittleFS mount, ./sd for the card.
// Driven by tools/anima-host/setup-store-check.mjs (`npm run setupstore:test`), next to the reset
// contract in setup-store-ctest.c; NVS is the shared in-memory one (nvs_host.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "setup_store.h"
#include "ap_creds.h"
#include "nvs.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ---- NVS: the shared in-memory NVS of tools/anima-host/nvs_host.c --------------------------------
static void nvs_wipe(void) { nvs_host_reset(); }                       // flash wipe (NVS back up, empty)
static void nvs_down(bool down) { nvs_host_set_initialized(!down); }   // a corrupt / unavailable NVS
// The stored document under `key`, or NULL. One static buffer: the value is valid until the next call.
static const char *nvs_peek(const char *key)
{
    static char buf[4096];
    nvs_handle_t h; size_t sz = sizeof buf;
    if (nvs_open(SETUP_STORE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    esp_err_t e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    return e == ESP_OK ? buf : NULL;
}

// ---- cJSON allocation hooks: live count (leaks) + fail-the-Nth-malloc (OOM) ------------------------
static long g_live = 0, g_allocs = 0, g_fail_at = 0;   // g_fail_at: 1-based malloc index to fail; 0 = off
static void *t_malloc(size_t n)
{
    g_allocs++;
    if (g_fail_at && g_allocs == g_fail_at) return NULL;
    void *p = malloc(n);
    if (p) g_live++;
    return p;
}
static void t_free(void *p) { if (p) { g_live--; free(p); } }

// ---- fixture files ------------------------------------------------------------------------------
#define CFG_NETS  "cfg/config/networks.json"
#define CFG_SETUP "cfg/config/setup.json"
#define SD_NETS   "sd/system/config/networks.json"
#define SD_SETUP  "sd/system/config/setup.json"
// Mirrors NETS_DOC / SETUP_DOC in nucleo_setup.c (setup-store-check.mjs asserts the secret names match).
static const setup_doc_t NETS  = { CFG_NETS,  "networks", SD_NETS,  "pass" };
static const setup_doc_t SETUP = { CFG_SETUP, "setup",    SD_SETUP, "ap_pass" };

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static char *readf(const char *p) { return setup_store_slurp(p); }
static void writef(const char *p, const char *text) { FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); } }
static void rm_card(void)
{
    remove(SD_NETS); remove(SD_SETUP); remove(SD_NETS ".tmp"); remove(SD_SETUP ".tmp");
    rmdir("sd/system/config"); rmdir("sd/system"); rmdir("sd");
}
static void rm_cfg(void) { remove(CFG_NETS); remove(CFG_SETUP); rmdir("cfg/config"); rmdir("cfg"); }
// Fresh device: /cfg mounted (its config dir exists, as nucleo_storage_mount_cfg leaves it), a blank
// card inserted (just the mount — the store must create /system/config itself), NVS empty.
static void reset_env(void)
{
    rm_card(); rm_cfg(); nvs_wipe();
    mkdir("cfg", 0775); mkdir("cfg/config", 0775); mkdir("sd", 0775);
}

typedef struct { const char *ssid, *pass; int prio; unsigned seq; } tnet_t;
static const tnet_t NETS4[] = {
    { "HomeWiFi",      "s3cr3t-home-pw",      2, 7 },
    { "Cafe \"Open\"", "",                    0, 3 },   // open network (and a quote in the SSID)
    { "Office\\5G",    "p\"w\\with-escapes",  1, 9 },   // JSON escapes in the password
    { "pass",          "value-named-pass-pw", 0, 1 },   // an SSID literally named "pass" is not a secret
};
#define NNETS ((int)(sizeof NETS4 / sizeof NETS4[0]))

// Same shape save_networks() builds.
static cJSON *nets_doc(const tnet_t *n, int k, unsigned seq)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "seq", seq);
    cJSON *arr = cJSON_AddArrayToObject(r, "nets");
    for (int i = 0; i < k; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", n[i].ssid);
        cJSON_AddStringToObject(o, "pass", n[i].pass);
        cJSON_AddNumberToObject(o, "prio", n[i].prio);
        cJSON_AddNumberToObject(o, "seq",  n[i].seq);
        cJSON_AddItemToArray(arr, o);
    }
    return r;
}
#define AP_PASS "k7mq2xw9pz3a"
// Same shape save_config() builds. The device is named "ap_pass" on purpose: a VALUE, not the secret.
static cJSON *setup_doc(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r,   "complete",    1);
    cJSON_AddStringToObject(r, "mode",        "sta");
    cJSON_AddStringToObject(r, "ssid",        "HomeWiFi");
    cJSON_AddStringToObject(r, "device_name", "ap_pass");
    cJSON_AddStringToObject(r, "ap_ssid",     "NucleoOS-1A2B");
    cJSON_AddStringToObject(r, "ap_pass",     AP_PASS);
    return r;
}

// Any string value (or member name) equal to `s`, anywhere in the tree.
static int tree_has_string(const cJSON *n, const char *s)
{
    for (const cJSON *c = n ? n->child : NULL; c; c = c->next) {
        if (cJSON_IsString(c) && !strcmp(c->valuestring, s)) return 1;
        if (tree_has_string(c, s)) return 1;
    }
    return 0;
}
// The file holds no password of NETS4 + the hotspot password, by member name, by value, or as raw bytes.
static int file_is_clean(const char *path, const char *secret)
{
    char *t = readf(path);
    if (!t) return 1;                                   // absent = nothing leaked
    int clean = !setup_store_has_member(t, secret) && !strstr(t, AP_PASS);
    cJSON *j = cJSON_Parse(t);
    clean = clean && j != NULL;                         // a torn/garbage mirror is a failure too
    for (int i = 0; i < NNETS && clean; i++) {
        if (!NETS4[i].pass[0]) continue;
        if (tree_has_string(j, NETS4[i].pass) || strstr(t, NETS4[i].pass)) clean = 0;
    }
    if (j && tree_has_string(j, AP_PASS)) clean = 0;
    cJSON_Delete(j);
    free(t);
    return clean;
}
static const cJSON *net_at(const cJSON *doc, int i) { return cJSON_GetArrayItem(cJSON_GetObjectItem(doc, "nets"), i); }
static const char *str_of(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }
static double num_of(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(v) ? v->valuedouble : -1; }

// ---- S1 + S2 ------------------------------------------------------------------------------------
static void t_save_split(void)
{
    reset_env();
    int tiers = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_store_status_t st; setup_store_status(&st);
    CHECK(tiers == 3 && st.tiers_ok == 3 && st.cfg_ok && st.nvs_ok && st.sd_ok, "S1: all three tiers accept a save (got %d)", tiers);

    char *cfg = readf(CFG_NETS);
    CHECK(cfg != NULL, "S1: /cfg networks.json written");
    CHECK(cfg && nvs_peek("networks") && !strcmp(cfg, nvs_peek("networks")), "S1: NVS holds the same full document as /cfg");
    cJSON *c = cJSON_Parse(cfg);
    for (int i = 0; i < NNETS; i++) {
        const char *p = str_of(net_at(c, i), "pass");
        CHECK(p && !strcmp(p, NETS4[i].pass), "S1: /cfg keeps the password of net %d", i);
    }
    cJSON_Delete(c); free(cfg);

    CHECK(exists(SD_NETS), "S1: the store created /system/config on the card and wrote the mirror");
    CHECK(file_is_clean(SD_NETS, "pass"), "S1: SD networks.json holds no password (name, value or bytes)");
    CHECK(!exists(SD_NETS ".tmp"), "S1: no temp file left on the card");

    char *sd = readf(SD_NETS);
    cJSON *s = cJSON_Parse(sd);
    CHECK(num_of(s, "seq") == 9, "S2: mirror keeps the recency counter");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(s, "nets")) == NNETS, "S2: mirror keeps every saved network");
    for (int i = 0; i < NNETS; i++) {
        const cJSON *o = net_at(s, i);
        const char *ss = str_of(o, "ssid");
        CHECK(ss && !strcmp(ss, NETS4[i].ssid), "S2: mirror keeps SSID %d verbatim (%s)", i, NETS4[i].ssid);
        CHECK(num_of(o, "prio") == NETS4[i].prio && num_of(o, "seq") == NETS4[i].seq, "S2: mirror keeps prio/seq of net %d", i);
        CHECK(cJSON_GetObjectItem(o, "pass") == NULL, "S2: net %d has no pass member at all", i);
    }
    cJSON_Delete(s); free(sd);

    tiers = setup_store_save(&SETUP, setup_doc());
    CHECK(tiers == 3, "S1: setup.json reaches all three tiers");
    char *cs = readf(CFG_SETUP);
    cJSON *cj = cJSON_Parse(cs);
    CHECK(str_of(cj, "ap_pass") && !strcmp(str_of(cj, "ap_pass"), AP_PASS), "S1: /cfg keeps the hotspot password");
    cJSON_Delete(cj); free(cs);
    CHECK(file_is_clean(SD_SETUP, "ap_pass"), "S1: SD setup.json holds no hotspot password");
    char *ss = readf(SD_SETUP);
    cJSON *sj = cJSON_Parse(ss);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(sj, "complete")), "S2: mirror keeps complete:true (the wizard is not re-run after a flash wipe)");
    CHECK(str_of(sj, "mode") && !strcmp(str_of(sj, "mode"), "sta"), "S2: mirror keeps the mode");
    CHECK(str_of(sj, "ssid") && !strcmp(str_of(sj, "ssid"), "HomeWiFi"), "S2: mirror keeps the last SSID");
    CHECK(str_of(sj, "device_name") && !strcmp(str_of(sj, "device_name"), "ap_pass"), "S2: a VALUE equal to the secret's name survives");
    CHECK(str_of(sj, "ap_ssid") && !strcmp(str_of(sj, "ap_ssid"), "NucleoOS-1A2B"), "S2: mirror keeps the hotspot SSID");
    cJSON_Delete(sj); free(ss);
    CHECK(g_live == 0, "S1: save frees its document and both prints (live=%ld)", g_live);
}

// ---- S3 -----------------------------------------------------------------------------------------
static void t_load_order(void)
{
    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_tier_t tier = SETUP_TIER_NONE; bool dirty = true;

    char *t = setup_store_load(&NETS, &tier, &dirty);
    char *cfg = readf(CFG_NETS);
    CHECK(t && tier == SETUP_TIER_CFG && !dirty, "S3: healthy device loads /cfg (tier %d, dirty %d)", tier, dirty);
    CHECK(t && cfg && !strcmp(t, cfg), "S3: /cfg text returned verbatim");
    free(t); free(cfg);

    remove(CFG_NETS);                                            // /cfg lost (launcher install / LittleFS wipe)
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_NVS && !dirty, "S3: no /cfg -> NVS answers (tier %d)", tier);
    CHECK(t && setup_store_has_member(t, "pass") && strstr(t, "s3cr3t-home-pw"), "S3: NVS copy still carries the passwords");
    free(t);

    nvs_wipe();                                                  // full flash wipe: only the card is left
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_SD && !dirty, "S3: flash wipe -> recovered from the SD mirror (tier %d)", tier);
    cJSON *j = cJSON_Parse(t);
    CHECK(j && cJSON_GetArraySize(cJSON_GetObjectItem(j, "nets")) == NNETS, "S3: every saved SSID recovered");
    CHECK(j && !setup_store_has_member(t, "pass"), "S3: recovered copy carries no password (user re-enters them)");
    cJSON_Delete(j); free(t);

    rm_card();
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(!t && tier == SETUP_TIER_NONE && !dirty, "S3: nothing anywhere -> NULL / NONE (first boot)");
    CHECK(g_live == 0, "S3: no cJSON leak (live=%ld)", g_live);
}

// ---- S4 -----------------------------------------------------------------------------------------
static void t_legacy_scrub(void)
{
    setup_tier_t tier; bool dirty;

    // Device updated from a firmware that mirrored the FULL doc: /cfg + NVS fine, card holds passwords.
    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    char *full = readf(CFG_NETS);
    writef(SD_NETS, full);
    free(full);
    char *t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_CFG && dirty, "S4: legacy card copy with passwords is flagged dirty (tier %d, dirty %d)", tier, dirty);
    setup_store_save(&NETS, cJSON_Parse(t));                     // what load_networks() does: re-save from RAM
    free(t);
    CHECK(file_is_clean(SD_NETS, "pass"), "S4: one re-save scrubs the card");
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_CFG && !dirty, "S4: scrubbed card is no longer dirty (no rewrite every boot)");
    CHECK(t && strstr(t, "s3cr3t-home-pw"), "S4: /cfg kept the passwords through the scrub");
    free(t);

    // Legacy SD-ONLY device (pre-/cfg firmware, or a hand-written provisioning file with spacing and a
    // different key case), freshly flashed: the passwords must survive into the internal tiers.
    reset_env();
    mkdir("sd/system", 0775); mkdir("sd/system/config", 0775);
    writef(SD_NETS, "{ \"seq\": 4, \"nets\": [ { \"ssid\": \"HomeWiFi\", \"Pass\" : \"s3cr3t-home-pw\", \"prio\": 2, \"seq\": 4 } ] }");
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_SD && dirty, "S4: legacy SD-only doc loads from SD, flagged dirty");
    CHECK(t && strstr(t, "s3cr3t-home-pw"), "S4: the returned text keeps the password for the RAM copy");
    cJSON *j = cJSON_Parse(t); free(t);
    CHECK(j != NULL, "S4: hand-formatted legacy doc parses");
    setup_store_save(&NETS, j);
    char *cfg = readf(CFG_NETS);
    CHECK(cfg && strstr(cfg, "s3cr3t-home-pw"), "S4: SD-only recovery migrated the password into /cfg");
    CHECK(nvs_peek("networks") && strstr(nvs_peek("networks"), "s3cr3t-home-pw"), "S4: ...and into NVS");
    free(cfg);
    CHECK(file_is_clean(SD_NETS, "pass"), "S4: ...and the card is scrubbed (case-insensitive 'Pass' stripped)");

    // Same for setup.json's hotspot password.
    reset_env();
    mkdir("sd/system", 0775); mkdir("sd/system/config", 0775);
    writef(SD_SETUP, "{\"complete\":true,\"mode\":\"ap\",\"ssid\":\"\",\"device_name\":\"nucleo-01\",\"ap_ssid\":\"NucleoOS-1A2B\",\"ap_pass\":\"" AP_PASS "\"}");
    t = setup_store_load(&SETUP, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_SD && dirty, "S4: legacy setup.json with ap_pass flagged dirty");
    setup_store_save(&SETUP, cJSON_Parse(t)); free(t);
    CHECK(file_is_clean(SD_SETUP, "ap_pass"), "S4: setup.json on the card scrubbed of ap_pass");
    cfg = readf(CFG_SETUP);
    CHECK(cfg && strstr(cfg, AP_PASS), "S4: hotspot password migrated into /cfg");
    free(cfg);
    CHECK(g_live == 0, "S4: no cJSON leak (live=%ld)", g_live);
}

// ---- S5 -----------------------------------------------------------------------------------------
static void t_best_effort(void)
{
    setup_tier_t tier; bool dirty; setup_store_status_t st;

    reset_env(); rm_card();                                      // no card inserted
    int n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_store_status(&st);
    CHECK(n == 2 && st.cfg_ok && st.nvs_ok && !st.sd_ok, "S5: no card -> /cfg + NVS still persist (%d)", n);
    CHECK(!exists("sd"), "S5: the store never creates the mount point itself");
    char *t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_CFG && !dirty, "S5: no card -> loads /cfg, not dirty");
    free(t);

    reset_env(); rm_cfg();                                       // launcher install: no /cfg partition
    n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_store_status(&st);
    CHECK(n == 2 && !st.cfg_ok && st.nvs_ok && st.sd_ok, "S5: no /cfg -> NVS + SD persist (%d)", n);
    CHECK(file_is_clean(SD_NETS, "pass"), "S5: no /cfg does not push the passwords onto the card");
    t = setup_store_load(&NETS, &tier, &dirty);
    CHECK(t && tier == SETUP_TIER_NVS && strstr(t, "s3cr3t-home-pw") && !dirty, "S5: no /cfg -> NVS answers with the passwords");
    free(t);

    reset_env(); nvs_down(true);                                 // NVS unavailable
    n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    CHECK(n == 2, "S5: NVS down -> /cfg + SD persist (%d)", n);
    CHECK(file_is_clean(SD_NETS, "pass"), "S5: NVS down does not push the passwords onto the card");

    reset_env(); rm_cfg(); rm_card(); nvs_down(true);           // nothing at all
    n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_store_status(&st);
    CHECK(n == 0 && st.tiers_ok == 0, "S5: no tier at all -> 0 reported (the caller logs it)");
    nvs_down(false);
    CHECK(g_live == 0, "S5: no cJSON leak (live=%ld)", g_live);
}

// ---- S6 -----------------------------------------------------------------------------------------
static void t_oom(void)
{
    // Count the mallocs of one clean save, then fail each one in turn.
    reset_env();
    cJSON *d = nets_doc(NETS4, NNETS, 9);
    long base = g_allocs;
    setup_store_save(&NETS, d);
    long per_save = g_allocs - base;
    CHECK(per_save >= 2, "S6: a save allocates (two prints) — counted %ld", per_save);

    // Card starts clean (older mirror) or DIRTY (an older firmware's full copy). No failure point may
    // put a password on a clean card; a dirty one must end clean as soon as the save reached /cfg or NVS
    // (if nothing was printed at all, -1, the card is left exactly as it was for the next load to retry).
    for (int dirty_start = 0; dirty_start < 2; dirty_start++) {
        int ok_all = 1;
        for (long k = 1; k <= per_save + 1; k++) {
            reset_env();
            setup_store_save(&NETS, nets_doc(NETS4, 1, 1));
            char *legacy = NULL;
            if (dirty_start) { legacy = readf(CFG_NETS); writef(SD_NETS, legacy); }
            cJSON *doc = nets_doc(NETS4, NNETS, 9);
            g_fail_at = g_allocs + k;
            int r = setup_store_save(&NETS, doc);
            g_fail_at = 0;
            char *now = readf(SD_NETS);
            int good = (r < 0 && legacy) ? (now && !strcmp(now, legacy)) : file_is_clean(SD_NETS, "pass");
            free(now); free(legacy);
            int leak = g_live != 0;
            if (!good || leak) { ok_all = 0; printf("  OOM at save malloc #%ld (%s card, r=%d): sd_ok=%d live=%ld\n", k, dirty_start ? "dirty" : "clean", r, good, g_live); }
        }
        CHECK(ok_all, "S6: every allocation failure on a %s card: no new password on it, no leak (%ld points)", dirty_start ? "dirty" : "clean", per_save + 1);
    }

    // First print fails: -1, no tier touched, the previous status is kept (not a fake "0 tiers").
    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, 1, 1));
    char *before = readf(CFG_NETS);
    cJSON *doc = nets_doc(NETS4, NNETS, 9);
    g_fail_at = g_allocs + 1;
    int r = setup_store_save(&NETS, doc);
    g_fail_at = 0;
    setup_store_status_t st; setup_store_status(&st);
    char *after = readf(CFG_NETS);
    CHECK(r == -1, "S6: OOM on the first print returns -1 (got %d)", r);
    CHECK(st.tiers_ok == 3 && st.sd_ok, "S6: ...and leaves the last save's status alone (tiers_ok %d)", st.tiers_ok);
    CHECK(before && after && !strcmp(before, after), "S6: ...and /cfg untouched");
    free(before); free(after);
    CHECK(g_live == 0, "S6: no cJSON leak (live=%ld)", g_live);
}

// ---- S8 -----------------------------------------------------------------------------------------
static void t_card_hygiene(void)
{
    setup_tier_t tier; bool dirty; setup_store_status_t st;

    // The mirror can't be rewritten (simulated by a DIRECTORY squatting on the temp name, so the
    // store's fopen(tmp, "w") fails like on a full / write-protected card).
    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    char *full = readf(CFG_NETS); writef(SD_NETS, full); free(full);   // legacy dirty copy
    mkdir(SD_NETS ".tmp", 0775);
    int n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    setup_store_status(&st);
    CHECK(n == 2 && !st.sd_ok, "S8: mirror write blocked -> /cfg + NVS only (%d)", n);
    CHECK(!exists(SD_NETS), "S8: ...and the dirty legacy copy is DELETED rather than left with passwords");
    rmdir(SD_NETS ".tmp");

    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, 1, 1));                     // clean, older mirror
    mkdir(SD_NETS ".tmp", 0775);
    setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    CHECK(exists(SD_NETS) && file_is_clean(SD_NETS, "pass"), "S8: a CLEAN stale mirror is kept for recovery when the rewrite fails");
    rmdir(SD_NETS ".tmp");

    // ...but a dirty copy that is the LAST persistent one (no /cfg, NVS down) is never deleted.
    reset_env(); rm_cfg(); nvs_down(true);
    mkdir("sd/system", 0775); mkdir("sd/system/config", 0775);
    writef(SD_NETS, "{\"seq\":1,\"nets\":[{\"ssid\":\"HomeWiFi\",\"pass\":\"s3cr3t-home-pw\",\"prio\":0,\"seq\":1}]}");
    mkdir(SD_NETS ".tmp", 0775);
    n = setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    CHECK(n == 0 && exists(SD_NETS), "S8: nothing internal took the save -> the only copy on the card is kept (%d)", n);
    rmdir(SD_NETS ".tmp"); nvs_down(false);

    // Orphan temp file from an older firmware (power cut between its FATFS remove and rename).
    reset_env();
    setup_store_save(&NETS, nets_doc(NETS4, NNETS, 9));
    full = readf(CFG_NETS); writef(SD_NETS ".tmp", full); free(full);
    char *t = setup_store_load(&NETS, &tier, &dirty);
    free(t);
    CHECK(!exists(SD_NETS ".tmp"), "S8: an orphan .tmp holding passwords is removed on load");
    char *red = readf(SD_NETS); writef(SD_NETS ".tmp", red); free(red);
    t = setup_store_load(&NETS, &tier, &dirty);
    free(t);
    CHECK(exists(SD_NETS ".tmp"), "S8: a redacted .tmp (ours) is left alone");
    remove(SD_NETS ".tmp");
    reset_env();                                                        // nothing internal: the tmp may be the last copy
    mkdir("sd/system", 0775); mkdir("sd/system/config", 0775);
    writef(SD_NETS ".tmp", "{\"seq\":1,\"nets\":[{\"ssid\":\"HomeWiFi\",\"pass\":\"s3cr3t-home-pw\"}]}");
    t = setup_store_load(&NETS, &tier, &dirty);
    free(t);
    CHECK(exists(SD_NETS ".tmp"), "S8: a legacy .tmp is kept while no internal tier holds the document");
    remove(SD_NETS ".tmp");

    // Streaming probe vs its 64-byte read chunks: the key must be found wherever it lands, and a
    // key-equal VALUE never, at every offset across two chunk boundaries.
    int ok_member = 1, ok_value = 1;
    for (int pad = 0; pad < 140; pad++) {
        char buf[512], fill[160];
        memset(fill, 'a', (size_t)pad); fill[pad] = 0;
        reset_env();
        setup_store_save(&NETS, nets_doc(NETS4, 1, 1));                 // /cfg answers; the card is probed
        snprintf(buf, sizeof buf, "{\"x\":\"%s\",\"nets\":[{\"ssid\":\"a\",\"pass\" \t: \"z\"}]}", fill);
        writef(SD_NETS, buf);
        t = setup_store_load(&NETS, &tier, &dirty); free(t);
        if (tier != SETUP_TIER_CFG || !dirty) { ok_member = 0; printf("  probe missed the member at pad %d\n", pad); }
        snprintf(buf, sizeof buf, "{\"x\":\"%s\",\"nets\":[{\"ssid\":\"pass\",\"note\":\"\\\"pass\\\":\"}]}", fill);
        writef(SD_NETS, buf);
        t = setup_store_load(&NETS, &tier, &dirty); free(t);
        if (dirty) { ok_value = 0; printf("  probe mistook a value for a member at pad %d\n", pad); }
    }
    CHECK(ok_member, "S8: file probe finds a member straddling any 64-byte chunk boundary");
    CHECK(ok_value,  "S8: file probe never flags a key-equal value or an escaped look-alike");
    CHECK(g_live == 0, "S8: no cJSON leak (live=%ld)", g_live);
}

// ---- S7 -----------------------------------------------------------------------------------------
static void t_member_scan(void)
{
    CHECK( setup_store_has_member("{\"a\":1,\"pass\":\"x\"}", "pass"), "S7: plain member found");
    CHECK( setup_store_has_member("{\"nets\":[{\"ssid\":\"a\",\"pass\":\"b\"}]}", "pass"), "S7: nested member found");
    CHECK( setup_store_has_member("{ \"ap_pass\" \r\n : \"x\" }", "ap_pass"), "S7: whitespace before ':' tolerated");
    CHECK( setup_store_has_member("{\"AP_PASS\":\"x\"}", "ap_pass"), "S7: case-insensitive, like cJSON_GetObjectItem");
    CHECK(!setup_store_has_member("{\"ssid\":\"pass\"}", "pass"), "S7: a value equal to the name is not a member");
    CHECK(!setup_store_has_member("{\"ssid\":\"pass\",\"prio\":1}", "pass"), "S7: ...not even followed by ','");
    CHECK(!setup_store_has_member("{\"ssid\":\"x\\\"pass\\\":y\"}", "pass"), "S7: escaped quotes inside a value don't fake a member");
    CHECK(!setup_store_has_member("{\"ap_pass\":\"x\"}", "pass"), "S7: 'ap_pass' is not 'pass' (exact token)");
    CHECK(!setup_store_has_member("{\"passive\":true}", "pass"), "S7: 'passive' is not 'pass'");
    CHECK(!setup_store_has_member("{\"pass", "pass"), "S7: unterminated literal -> no match, no overrun");
    CHECK(!setup_store_has_member("{\"pa\\ss\":1}", "pass"), "S7: a name with an escape is not the key");
    CHECK( setup_store_has_member("{\"a\\\\\":1,\"pass\":2}", "pass"), "S7: an escaped backslash ends its literal correctly");
    CHECK( setup_store_has_member("{\"v\":\"\\\"\",\"pass\":\"x\"}", "pass"), "S7: one escaped quote in a value doesn't flip string parity (no missed member)");
    CHECK(!setup_store_has_member(NULL, "pass") && !setup_store_has_member("{}", ""), "S7: NULL / empty key -> false");
}

// ---- A1-A6: hotspot credentials -------------------------------------------------------------------
static uint32_t g_rng = 12345;
static uint32_t t_rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }   // deterministic stand-in for esp_random
static const uint8_t MAC[6] = { 0x24, 0x58, 0x7C, 0x11, 0x1A, 0x2B };
static int pass_is_minted(const char *p)
{
    if (strlen(p) != 12) return 0;
    for (const char *q = p; *q; q++) if (!strchr("abcdefghjkmnpqrstuvwxyz23456789", *q)) return 0;
    return 1;
}
// What save_config() persists for the hotspot part, through the real store; then what load_config()
// reads back into a FRESH struct (a reboot). Returns the reloaded creds.
static ap_creds_t ap_reboot(const ap_creds_t *c, int flash_wipe)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "complete", 1);
    ap_creds_save(c, doc);
    setup_store_save(&SETUP, doc);
    if (flash_wipe) { remove(CFG_SETUP); nvs_wipe(); }
    ap_creds_t r; memset(&r, 0, sizeof r);
    setup_tier_t tier; bool dirty;
    char *t = setup_store_load(&SETUP, &tier, &dirty);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    if (j) ap_creds_load(&r, j);
    cJSON_Delete(j); free(t);
    return r;
}

static void t_ap_creds(void)
{
    ap_creds_t c; memset(&c, 0, sizeof c);
    CHECK(ap_creds_ensure(&c, MAC, t_rnd), "A1: first use changes the creds (caller persists once)");
    CHECK(!strcmp(c.ssid, "NucleoOS-1A2B"), "A1: per-device SSID from the SoftAP MAC (%s)", c.ssid);
    CHECK(pass_is_minted(c.pass) && !c.open && ap_creds_secure(&c), "A1: random 12-char WPA2 password, not open (%s)", c.pass);
    char first[64]; strcpy(first, c.pass);
    CHECK(!ap_creds_ensure(&c, MAC, t_rnd) && !strcmp(c.pass, first), "A1: next AP start keeps the same password");

    // A2 — the bug: "" (open) must survive every AP (re)start instead of being re-minted.
    CHECK(ap_creds_set_pass(&c, "") && c.open && !c.pass[0], "A2: set \"\" = deliberate open hotspot");
    int stays = 1;
    for (int i = 0; i < 5; i++) if (ap_creds_ensure(&c, MAC, t_rnd) || c.pass[0] || !c.open) stays = 0;
    CHECK(stays && !ap_creds_secure(&c), "A2: open hotspot is NOT re-minted by later AP restarts");

    // A3 — setter bounds; a real password clears the open choice.
    ap_creds_t b = c;
    CHECK(!ap_creds_set_pass(&b, "1234567") && !memcmp(&b, &c, sizeof b), "A3: 7 chars rejected, creds unchanged");
    char p64[65]; memset(p64, 'x', 64); p64[64] = 0;
    CHECK(!ap_creds_set_pass(&b, p64) && !memcmp(&b, &c, sizeof b), "A3: 64 chars rejected (would be truncated)");
    CHECK(!ap_creds_set_pass(&b, NULL), "A3: NULL rejected");
    p64[63] = 0;
    CHECK(ap_creds_set_pass(&b, p64) && !b.open && ap_creds_secure(&b), "A3: 63 chars accepted");
    CHECK(ap_creds_set_pass(&b, "12345678") && !b.open && ap_creds_secure(&b), "A3: 8 chars accepted, open cleared");
    CHECK(!ap_creds_ensure(&b, MAC, t_rnd) && !strcmp(b.pass, "12345678"), "A3: a user password is never replaced");

    // A4 — the old shared defaults are upgraded to per-device ones.
    ap_creds_t l; memset(&l, 0, sizeof l);
    strcpy(l.ssid, AP_CREDS_SSID_LEGACY); strcpy(l.pass, AP_CREDS_PASS_LEGACY);
    CHECK(ap_creds_ensure(&l, MAC, t_rnd), "A4: legacy defaults -> changed");
    CHECK(!strcmp(l.ssid, "NucleoOS-1A2B") && pass_is_minted(l.pass) && strcmp(l.pass, AP_CREDS_PASS_LEGACY) && !l.open,
          "A4: legacy SSID + shared password replaced by per-device ones");

    // A5 — setup.json round-trip through the store (reboot, then flash-wipe recovery from the SD mirror).
    reset_env();
    ap_creds_t o; memset(&o, 0, sizeof o); ap_creds_ensure(&o, MAC, t_rnd); ap_creds_set_pass(&o, "");
    ap_creds_t r = ap_reboot(&o, 0);
    CHECK(r.open && !r.pass[0] && !strcmp(r.ssid, o.ssid), "A5: open hotspot reloads open after a reboot");
    CHECK(!ap_creds_ensure(&r, MAC, t_rnd) && r.open, "A5: ...and stays open when the AP starts");
    char *sd = readf(SD_SETUP);
    CHECK(sd && setup_store_has_member(sd, "ap_open") && !setup_store_has_member(sd, "ap_pass"),
          "A5: the SD mirror keeps ap_open (not a secret) and drops ap_pass");
    free(sd);
    r = ap_reboot(&o, 1);
    CHECK(r.open && !r.pass[0] && !ap_creds_ensure(&r, MAC, t_rnd) && r.open, "A5: flash wipe -> recovered from the card still OPEN, no password minted");

    reset_env();
    ap_creds_t s; memset(&s, 0, sizeof s); ap_creds_ensure(&s, MAC, t_rnd);
    r = ap_reboot(&s, 0);
    CHECK(!r.open && !strcmp(r.pass, s.pass), "A5: secured hotspot reloads its password after a reboot");
    r = ap_reboot(&s, 1);
    CHECK(!r.open && !r.pass[0], "A5: flash wipe -> the card has no hotspot password to give back");
    CHECK(ap_creds_ensure(&r, MAC, t_rnd) && pass_is_minted(r.pass) && strcmp(r.pass, s.pass), "A5: ...so a NEW one is minted");

    // A6 — load edge cases.
    ap_creds_t e; memset(&e, 0, sizeof e);
    cJSON *j = cJSON_Parse("{\"ap_ssid\":\"Mine\",\"ap_pass\":\"hunter2hunter2\",\"ap_open\":true}");
    ap_creds_load(&e, j); cJSON_Delete(j);
    CHECK(!e.open && !strcmp(e.pass, "hunter2hunter2") && !strcmp(e.ssid, "Mine"), "A6: a stored password wins over a stale open flag");
    memset(&e, 0, sizeof e);
    j = cJSON_Parse("{\"ap_ssid\":\"\",\"ap_pass\":\"\"}");                   // older firmware: no ap_open
    ap_creds_load(&e, j); cJSON_Delete(j);
    CHECK(!e.open && ap_creds_ensure(&e, MAC, t_rnd) && pass_is_minted(e.pass), "A6: no ap_open (older firmware) = not open -> minted as before");
    j = cJSON_CreateObject();
    g_fail_at = g_allocs + 1;
    int saved = ap_creds_save(&e, j);
    g_fail_at = 0;
    cJSON_Delete(j);
    CHECK(!saved, "A6: save reports OOM (the caller then persists nothing)");
    CHECK(g_live == 0, "A: no cJSON leak (live=%ld)", g_live);
}

int main(void)
{
    cJSON_Hooks hooks = { t_malloc, t_free };
    cJSON_InitHooks(&hooks);
    t_member_scan();
    t_save_split();
    t_load_order();
    t_legacy_scrub();
    t_best_effort();
    t_oom();
    t_card_hygiene();
    t_ap_creds();
    reset_env(); rm_card(); rm_cfg(); nvs_wipe();
    printf("setup-store: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
