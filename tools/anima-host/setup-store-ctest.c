// Host gate for the three-tier config store (firmware/components/nucleo_setup/setup_store.c) behind
// setup.json + networks.json, compiled against an in-memory NVS (nvs_host.c). Proves on the PC:
//   P1  a save fans out to /cfg, NVS and the SD mirror; each tier is independent (a dead one never
//       blocks the others) and the SD subtree is created on demand
//   P2  load order is /cfg -> NVS -> SD, and a fallback hit is flagged so the caller heals the rest
//   P3  why the old Settings ▸ Reset was a no-op: wiping the SD mirror leaves /cfg (or NVS) answering
//   R1  erase removes a document from EVERY tier (+ .tmp leftovers), per document, and leaves other
//       NVS namespaces (the pairing store "nucleoauth") alone — the soft reset keeps pairing
//   R2  erase is honest: absent tiers count as erased, an uninitialised NVS is reported as a failure
//   R3  once sealed, no save lands again (the Wi-Fi supervisor re-persisting after a join can't undo
//       a reset before the reboot), so the next boot finds nothing and the wizard re-runs
// Compiled by tools/anima-host/setup-store-check.mjs (`npm run setupstore:test`).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "setup_store.h"
#include "nvs.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define ROOT        "build/setupstore"
#define CFG_SETUP   ROOT "/cfg/config/setup.json"
#define SD_SETUP    ROOT "/sd/system/config/setup.json"
#define CFG_NETS    ROOT "/cfg/config/networks.json"
#define SD_NETS     ROOT "/sd/system/config/networks.json"
#define NOCFG_SETUP ROOT "/nocfg/config/setup.json"         // "nocfg" is a plain file: no /cfg partition
#define NOSD_SETUP  ROOT "/nosd/system/config/setup.json"   // "nosd" is a plain file: no SD card

static const char *DOC_A = "{\"complete\":true,\"mode\":\"sta\",\"ssid\":\"Home\",\"device_name\":\"desk\","
                           "\"ap_ssid\":\"NucleoOS-1A2B\",\"ap_pass\":\"k7m2p9q4r8s3\"}";
static const char *DOC_B = "{\"complete\":true,\"mode\":\"ap\",\"ssid\":\"\",\"device_name\":\"lab\"}";
static const char *NETS  = "{\"seq\":2,\"nets\":[{\"ssid\":\"Home\",\"pass\":\"hunter22\",\"prio\":0,\"seq\":2}]}";

static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void put(const char *p, const char *text) { FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); } }

static int nvs_has(const char *ns, const char *key)
{
    nvs_handle_t h; size_t sz = 0;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;
    int ok = nvs_get_str(h, key, NULL, &sz) == ESP_OK;
    nvs_close(h);
    return ok;
}
static void nvs_put(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, key, val); nvs_commit(h); nvs_close(h); }
}
static void nvs_del(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, key); nvs_close(h); }
}

// Load and compare. want == NULL: expect nothing loadable. fb: expected from_fallback (-1 = don't care).
static int load_is(const char *cfg, const char *sd, const char *key, const char *want, int fb)
{
    bool from_fb = false;
    char *got = setup_store_load(cfg, sd, key, &from_fb);
    int ok = want ? (got && !strcmp(got, want) && (fb < 0 || (int)from_fb == fb)) : (got == NULL);
    free(got);
    return ok;
}

static void fresh(void)
{
    nvs_host_reset();
    remove(CFG_SETUP); remove(CFG_SETUP ".tmp"); remove(SD_SETUP); remove(SD_SETUP ".tmp");
    remove(CFG_NETS);  remove(CFG_NETS ".tmp");  remove(SD_NETS);  remove(SD_NETS ".tmp");
}

static void t_fanout(void)
{
    fresh();
    CHECK(!is_dir(ROOT "/sd/system"), "precondition: SD subtree not provisioned yet");
    int n = setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    CHECK(n == 3, "P1: save lands on all three tiers (got %d)", n);
    CHECK(is_dir(ROOT "/sd/system/config"), "P1: SD mirror subtree created on demand");
    CHECK(exists(CFG_SETUP) && exists(SD_SETUP), "P1: /cfg file + SD mirror written");
    CHECK(nvs_has(SETUP_STORE_NVS_NS, "setup"), "P1: NVS copy under nucleocfg/setup");
    CHECK(!exists(CFG_SETUP ".tmp") && !exists(SD_SETUP ".tmp"), "P1: atomic write leaves no .tmp");
    bool c = false, v = false, s = false; int t = 0;
    setup_store_status(&c, &v, &s, &t);
    CHECK(c && v && s && t == 3, "P1: persist status reports 3/3 tiers");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 0), "P2: loads from /cfg, not flagged as fallback");
    n = setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_B);   // overwrite in place (SD: dest exists)
    CHECK(n == 3 && load_is(CFG_SETUP, SD_SETUP, "setup", DOC_B, 0), "P1: re-save overwrites every tier");
    remove(CFG_SETUP); nvs_del(SETUP_STORE_NVS_NS, "setup");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_B, 1), "P1: the SD mirror got the overwrite too");
    CHECK(nvs_host_open_handles() == 0, "no NVS handle leaked");
}

static void t_degraded(void)
{
    fresh();
    int n = setup_store_persist(CFG_SETUP, NOSD_SETUP, "setup", DOC_A);
    CHECK(n == 2, "P1: no SD card -> /cfg + NVS still persist (got %d)", n);
    n = setup_store_persist(NOCFG_SETUP, SD_SETUP, "setup", DOC_A);
    CHECK(n == 2, "P1: no /cfg partition (launcher install) -> NVS + SD still persist (got %d)", n);
    n = setup_store_persist(NOCFG_SETUP, NOSD_SETUP, "setup", DOC_A);
    CHECK(n == 1, "P1: neither -> NVS alone still persists (got %d)", n);
    bool c = true, v = false, s = true; int t = 0;
    setup_store_status(&c, &v, &s, &t);
    CHECK(!c && v && !s && t == 1, "P1: status names the surviving tier");
    CHECK(load_is(NOCFG_SETUP, NOSD_SETUP, "setup", DOC_A, 1), "P2: NVS-only config still loads");
    nvs_host_set_initialized(false);
    n = setup_store_persist(NOCFG_SETUP, NOSD_SETUP, "setup", DOC_A);
    CHECK(n == 0, "P1: nothing available -> 0 tiers (caller shouts)");
    nvs_host_set_initialized(true);
}

static void t_read_order(void)
{
    fresh();
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    put(CFG_SETUP, DOC_B);                               // tiers disagree: /cfg must win
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_B, 0), "P2: /cfg wins over NVS and SD");
    remove(CFG_SETUP);
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 1), "P2: /cfg gone -> NVS answers, flagged fallback");
    nvs_del(SETUP_STORE_NVS_NS, "setup");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 1), "P2: NVS gone too -> SD mirror answers");
    remove(SD_SETUP);
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", NULL, -1), "P2: no tier -> nothing (wizard runs)");
    // Heal: what load_config() does after a fallback hit — a re-save restores the primary tier.
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 0), "P2: re-persist heals /cfg");
}

static void t_old_reset_was_noop(void)
{
    fresh();
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    setup_store_persist(CFG_NETS, SD_NETS, "networks", NETS);
    remove(SD_SETUP); remove(SD_NETS);                   // exactly what rm_tree("/sd/system/config") did
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 0), "P3: SD-only wipe: /cfg still has complete:true -> wizard skipped");
    CHECK(load_is(CFG_NETS, SD_NETS, "networks", NETS, 0), "P3: SD-only wipe: saved networks survive");
    remove(CFG_SETUP);                                   // + deleting /cfg by hand still isn't enough...
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 1), "P3: /cfg + SD wiped: NVS still answers");
}

static void t_erase(void)
{
    fresh();
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    setup_store_persist(CFG_NETS, SD_NETS, "networks", NETS);
    put(CFG_SETUP ".tmp", "{half-written");            // a save interrupted by a power cut
    put(SD_SETUP ".tmp", "{half-written");
    nvs_put("nucleoauth", "auth", "{\"pin\":\"123456\",\"tokens\":[]}");   // pairing lives in its own namespace
    CHECK(setup_store_erase(CFG_SETUP, SD_SETUP, "setup"), "R1: erase reports success");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", NULL, -1), "R1: setup.json gone from every tier");
    CHECK(!exists(CFG_SETUP) && !exists(SD_SETUP), "R1: /cfg file and SD mirror removed");
    CHECK(!exists(CFG_SETUP ".tmp") && !exists(SD_SETUP ".tmp"), "R1: .tmp leftovers removed");
    CHECK(!nvs_has(SETUP_STORE_NVS_NS, "setup"), "R1: NVS key erased");
    CHECK(load_is(CFG_NETS, SD_NETS, "networks", NETS, 0), "R1: erase is per document (networks untouched)");
    CHECK(setup_store_erase(CFG_NETS, SD_NETS, "networks"), "R1: networks erase reports success");
    CHECK(load_is(CFG_NETS, SD_NETS, "networks", NULL, -1), "R1: networks.json gone from every tier");
    CHECK(nvs_host_key_count(SETUP_STORE_NVS_NS) == 0, "R1: nucleocfg namespace empty");
    CHECK(nvs_has("nucleoauth", "auth"), "R1: pairing store untouched (soft reset keeps pairing)");
    CHECK(setup_store_erase(CFG_SETUP, SD_SETUP, "setup"), "R2: erasing again is idempotent");
    CHECK(nvs_host_open_handles() == 0, "no NVS handle leaked");
}

static void t_erase_honest(void)
{
    nvs_host_reset();                                    // namespace never created
    CHECK(setup_store_erase(NOCFG_SETUP, NOSD_SETUP, "setup"), "R2: no /cfg, no SD, no namespace -> erased");
    CHECK(nvs_host_key_count(SETUP_STORE_NVS_NS) <= 0, "R2: nothing left in NVS");

    fresh();
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    nvs_host_set_initialized(false);
    CHECK(!setup_store_erase(CFG_SETUP, SD_SETUP, "setup"), "R2: NVS down -> erase must NOT claim success");
    nvs_host_set_initialized(true);
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", DOC_A, 1), "R2: ...because the NVS copy really survived");
    CHECK(setup_store_erase(CFG_SETUP, SD_SETUP, "setup"), "R2: retry with NVS up succeeds");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", NULL, -1), "R2: and nothing loads afterwards");
}

static void t_slurp(void)
{
    CHECK(setup_store_slurp(ROOT "/missing.json") == NULL, "slurp: missing file -> NULL");
    FILE *f = fopen(ROOT "/huge.json", "wb");
    if (f) { for (int i = 0; i < 33 * 1024; i++) fputc(' ', f); fclose(f); }
    CHECK(setup_store_slurp(ROOT "/huge.json") == NULL, "slurp: >32 KB is refused (no OOM on a corrupt file)");
    remove(ROOT "/huge.json");
}

// Must run LAST: the seal is irreversible for the life of the process, as on the device until reboot.
static void t_seal(void)
{
    fresh();
    setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    setup_store_persist(CFG_NETS, SD_NETS, "networks", NETS);
    CHECK(!setup_store_sealed(), "R3: store open before the reset");
    // The nucleo_setup_factory_reset() sequence: seal, then erase both documents.
    CHECK(setup_store_seal(), "R3: seal reports drained (no save in flight)");
    CHECK(setup_store_sealed(), "R3: sealed");
    CHECK(setup_store_erase(CFG_SETUP, SD_SETUP, "setup") && setup_store_erase(CFG_NETS, SD_NETS, "networks"),
          "R3: both documents erased");
    // Between the reset and esp_restart(): the supervisor joins and saves, the AP mints its password.
    int n1 = setup_store_persist(CFG_SETUP, SD_SETUP, "setup", DOC_A);
    int n2 = setup_store_persist(CFG_NETS, SD_NETS, "networks", NETS);
    CHECK(n1 == 0 && n2 == 0, "R3: sealed store refuses every save (got %d, %d)", n1, n2);
    CHECK(!exists(CFG_SETUP) && !exists(SD_SETUP) && !exists(CFG_NETS) && !exists(SD_NETS), "R3: no file came back");
    CHECK(nvs_host_key_count(SETUP_STORE_NVS_NS) == 0, "R3: no NVS key came back");
    CHECK(load_is(CFG_SETUP, SD_SETUP, "setup", NULL, -1) && load_is(CFG_NETS, SD_NETS, "networks", NULL, -1),
          "R3: next boot loads nothing -> first-run wizard re-runs");
    CHECK(setup_store_erase(CFG_SETUP, SD_SETUP, "setup"), "R3: erase still works while sealed");
}

int main(void)
{
    // Layout: build/setupstore/{cfg/config, sd} as the mounted stores; "nocfg" / "nosd" are plain
    // FILES so every path below them fails like an unmounted filesystem (mkdir can't create them).
    mkdir(ROOT, 0775); mkdir(ROOT "/cfg", 0775); mkdir(ROOT "/cfg/config", 0775); mkdir(ROOT "/sd", 0775);
    put(ROOT "/nocfg", "not a directory");
    put(ROOT "/nosd", "not a directory");

    t_fanout();
    t_degraded();
    t_read_order();
    t_old_reset_was_noop();
    t_erase();
    t_erase_honest();
    t_slurp();
    t_seal();

    printf("setup-store: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
