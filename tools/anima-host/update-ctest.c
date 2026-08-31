// Host gate for the native release-update policy: compile the REAL decision core
// (firmware/components/nucleo_app/update_policy.c) and prove the invariants on the PC.
// Wired as `npm run update:test` (see tools/anima-host/update-check.mjs). Mirrors wifi-ctest.c.
//
// Invariants under test:
//   1. Only a full x.y.z prefix parses; build metadata (+N.g<hash>, .<build>, '*') never matters.
//   2. Comparison is numeric per component (0.2.9 < 0.2.11) and garbage compares "equal".
//   3. The dialog shows ONLY for a strictly newer, parsable, non-dismissed tag.
//   4. The throttle can never wedge: never-checked and clock-went-backwards are both "due".
//   5. version.json / SHA256SUMS parsing is strict about shape and refuses truncation.
#include "update_policy.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_total = 0;
#define CHECK(cond, name) do { g_total++; if (!(cond)) { g_fail++; printf("FAIL %-58s\n", name); } } while (0)

int main(void)
{
    upd_semver_t v;

    // ── parsing ────────────────────────────────────────────────────────────────
    CHECK(upd_parse_semver("v0.3.0", &v) && v.maj == 0 && v.min == 3 && v.pat == 0, "parse plain tag");
    CHECK(upd_parse_semver("0.2.11+17.g1a2b3c4*", &v) && v.pat == 11, "parse dirty PROJECT_VER");
    CHECK(upd_parse_semver("v0.3.0.5", &v) && v.pat == 0, "parse 4-part tag -> triplet");
    CHECK(upd_parse_semver("  V1.2.3", &v) && v.maj == 1, "parse leading space + capital V");
    CHECK(upd_parse_semver("1.0.0+3.gnogit", &v), "parse nogit build");
    CHECK(!upd_parse_semver("v1.2", NULL), "reject incomplete triplet");
    CHECK(!upd_parse_semver("", NULL), "reject empty");
    CHECK(!upd_parse_semver(NULL, NULL), "reject NULL");
    CHECK(!upd_parse_semver("?", NULL), "reject firmware '?' fallback");
    CHECK(!upd_parse_semver("abc.def.ghi", NULL), "reject non-numeric");
    CHECK(!upd_parse_semver("99999999.0.0", NULL), "reject absurd component");
    CHECK(!upd_parse_semver("1..3", NULL), "reject empty component");

    // ── comparison ─────────────────────────────────────────────────────────────
    CHECK(upd_cmp("0.2.11+17.gabc", "v0.2.11") == 0, "same release rebuilt == equal");
    CHECK(upd_cmp("0.2.11", "v0.3.0") < 0, "minor bump is newer");
    CHECK(upd_cmp("0.3.0", "v0.2.11") > 0, "reverse order");
    CHECK(upd_cmp("0.2.9", "v0.2.11") < 0, "numeric not lexicographic (9 < 11)");
    CHECK(upd_cmp("1.0.0", "v0.9.9") > 0, "major dominates");
    CHECK(upd_cmp("garbage", "v0.3.0") == 0, "garbage compares equal");
    CHECK(upd_cmp("v0.3.0", "garbage") == 0, "garbage on either side");

    // ── version.json tag extraction ────────────────────────────────────────────
    char tag[24];
    CHECK(upd_extract_tag("{\"tag\":\"v0.3.0\"}", tag, sizeof tag) && !strcmp(tag, "v0.3.0"), "extract clean tag");
    CHECK(upd_extract_tag("{ \"tag\" :  \"v1.2.3\" }", tag, sizeof tag) && !strcmp(tag, "v1.2.3"), "extract with whitespace");
    CHECK(upd_extract_tag("{\"other\":1,\"tag\":\"v0.4.0\"}", tag, sizeof tag) && !strcmp(tag, "v0.4.0"), "extract after other keys");
    CHECK(!upd_extract_tag("{\"tag\":\"\"}", tag, sizeof tag), "reject empty tag");
    CHECK(!upd_extract_tag("{\"tag\":null}", tag, sizeof tag), "reject non-string tag");
    CHECK(!upd_extract_tag("{\"nope\":\"v1.0.0\"}", tag, sizeof tag), "reject missing key");
    CHECK(!upd_extract_tag("<!doctype html>", tag, sizeof tag), "reject an HTML error page");
    CHECK(!upd_extract_tag("{\"tag\":\"vvvvvvvvvvvvvvvvvvvvvvvvvvvvvv0.1.0\"}", tag, 8), "refuse truncation");
    CHECK(!upd_extract_tag(NULL, tag, sizeof tag), "reject NULL json");

    // ── SHA256SUMS lookup ──────────────────────────────────────────────────────
    const char *sums =
        "1111111111111111111111111111111111111111111111111111111111111111  nucleoos-latest.bin\r\n"
        "ABCDEF2222222222222222222222222222222222222222222222222222222222 *nucleoos-latest-ota.bin\n"
        "not a real line\n"
        "3333333333333333333333333333333333333333333333333333333333333333  nucleoos-0.3.0.0-sd.zip";
    char hex[65];
    CHECK(upd_find_sha256(sums, "nucleoos-latest-ota.bin", hex) &&
          !strncmp(hex, "abcdef", 6) && (int)strlen(hex) == 64, "find + lowercase binary-marker line");
    CHECK(upd_find_sha256(sums, "nucleoos-latest.bin", hex) && hex[0] == '1', "find CRLF line, exact name");
    CHECK(upd_find_sha256(sums, "nucleoos-0.3.0.0-sd.zip", hex) && hex[0] == '3', "find last line (no newline)");
    CHECK(!upd_find_sha256(sums, "latest-ota.bin", hex), "suffix must not match (exact names only)");
    CHECK(!upd_find_sha256(sums, "missing.bin", hex), "absent asset");
    CHECK(!upd_find_sha256("", "x", hex), "empty sums");
    CHECK(!upd_find_sha256(sums, "", hex), "empty name");

    // ── dialog decision ────────────────────────────────────────────────────────
    CHECK(upd_should_show("0.2.11+17.gabc", "v0.3.0", ""),        "newer + no dismissal -> show");
    CHECK(upd_should_show("0.2.11+17.gabc", "v0.3.0", "v0.2.12"), "dismissal of an OLDER tag doesn't stick");
    CHECK(!upd_should_show("0.2.11", "v0.3.0", "v0.3.0"),         "dismissed tag stays hidden");
    CHECK(!upd_should_show("0.3.0+1.gdef", "v0.3.0", ""),          "same triplet -> quiet");
    CHECK(!upd_should_show("0.4.0+1.gdef", "v0.3.0", ""),          "dev build ahead -> quiet");
    CHECK(!upd_should_show("0.2.11", "", ""),                       "never checked -> quiet");
    CHECK(!upd_should_show("0.2.11", "garbage", ""),                "garbage latest -> quiet");
    CHECK(!upd_should_show("?", "v0.3.0", ""),                      "unknown current -> quiet");
    CHECK(!upd_should_show("0.2.11", "v0.3.0", "v0.3.0"),           "dismiss exact-match only");

    // ── throttle ───────────────────────────────────────────────────────────────
    CHECK(upd_check_due(1000, 0, 86400),          "never checked -> due");
    CHECK(!upd_check_due(1000, 900, 86400),       "fresh -> not due");
    CHECK(upd_check_due(90000, 100, 86400),       "stale -> due");
    CHECK(upd_check_due(50, 1000, 86400),         "clock went backwards -> due (never wedge)");
    CHECK(upd_check_due(86500, 100, 86400),       "exactly past interval -> due");
    CHECK(!upd_check_due(86499, 100, 86400),      "one second short -> not due");

    printf("update-policy: %d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
