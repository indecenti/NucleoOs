// update_policy — pure decision core for the native release-update flow.
//
// No ESP-IDF, no network, no NVS, no allocation: every input is a plain string/number, so
// tools/anima-host/update-check.mjs compiles this file with gcc and proves the invariants on the
// PC (mirror of wifi_policy.c / the JS twin web/shell/update-core.js, which shares the semantics:
// semver TRIPLET only — build metadata never triggers anything).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int maj, min, pat; } upd_semver_t;

// Parse the leading semver triplet of a firmware PROJECT_VER ("0.2.11+17.g1a2b3c4*"), a release
// tag ("v0.3.0", "v0.3.0.5") or a bare "1.2.3". False on anything without a full x.y.z prefix.
bool upd_parse_semver(const char *s, upd_semver_t *out);

// Compare two version strings by triplet: -1 / 0 / +1. Either side unparsable -> 0 ("equal"):
// garbage must never trigger an update prompt.
int upd_cmp(const char *a, const char *b);

// Extract the "tag" value from a version.json body ({"tag":"v0.3.0"}). Tolerates whitespace.
// False when absent, empty, non-string or longer than cap-1.
bool upd_extract_tag(const char *json, char *out, size_t cap);

// Find the SHA-256 for `name` in sha256sum output ("<64 hex>  <name>", optional '*' binary
// marker, CR tolerated). Writes 64 lowercase hex chars + NUL into out_hex65. False if absent.
bool upd_find_sha256(const char *sums, const char *name, char *out_hex65);

// Boot decision: surface the update dialog this boot?
//   current   running PROJECT_VER
//   latest    last tag learned from the network (may be "" = never checked)
//   dismissed tag the user chose to ignore (may be "")
// True only when latest parses, is strictly newer than current, and is not the dismissed tag.
bool upd_should_show(const char *current, const char *latest, const char *dismissed);

// Network-check throttle: at most one fetch per interval. last_s==0 = never checked.
// A clock that went backwards (now < last) counts as due — never let a bad RTC wedge the check.
bool upd_check_due(uint32_t now_s, uint32_t last_s, uint32_t interval_s);

#ifdef __cplusplus
}
#endif
