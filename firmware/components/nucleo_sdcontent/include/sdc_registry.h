// SD content self-install — the app-registry MERGE (`system/registry/apps.json`, manifest mode 'm').
// Pure C + stdio, streamed: no JSON tree in RAM (the install boot has a ~31 KB largest block and the registry
// is ~12 KB). Compiled into the firmware and into the host gates (tools/anima-host/sdcontent-*.c).
//
// Rule (docs/sd-content-install.md §4): the device's file is the live list. Its entries are kept byte for
// byte — the user's enabled/permissions choices, the apps the Agent created, the order. Every entry of the
// release's `installed[]` whose `id` the device doesn't have is appended, so an app a release adds shows up.
// Nothing is removed or rewritten.
#pragma once
#include <stddef.h>

typedef enum {
    SDC_REG_MERGED = 1,          // new apps appended; *added says how many
    SDC_REG_UNCHANGED = 0,       // the device already lists every release app: file untouched
    SDC_REG_DEVICE_BAD = -1,     // the device file is unreadable / not a registry: untouched (keep the release copy)
    SDC_REG_RELEASE_BAD = -2,    // the release copy is not a registry: device file untouched
    SDC_REG_IO = -3,             // could not write the merged file: device file untouched
} sdc_reg_result_t;

// Merge `release_path` into `device_path` in place (atomic: a temp file, then remove + rename). `buf`/`cap`
// is scratch for one entry (an entry larger than cap counts as malformed); the device uses its download
// window. Temp files are `<device_path>.add` and `<device_path>.merge`, always removed.
sdc_reg_result_t sdc_registry_merge(const char *device_path, const char *release_path,
                                    char *buf, size_t cap, int *added);
