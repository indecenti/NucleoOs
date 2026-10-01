// Host test for the SD-content decision core (firmware/components/nucleo_sdcontent/content_policy.c).
// Proves: path hygiene, the writable allow-list (every ownership row in docs/sd-content-install.md §4),
// manifest header/line parsing (incl. hostile input), tag matching and the per-file plan. Pure C.
#include "content_policy.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define OK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static void test_hygiene(void)
{
    OK(sdc_path_hygienic("www/shell/app.js"), "ok relative path");
    OK(!sdc_path_hygienic("/www/shell/x"), "leading slash refused");
    OK(!sdc_path_hygienic("../etc/passwd"), "parent traversal refused");
    OK(!sdc_path_hygienic("a/../b"), "embedded traversal refused");
    OK(!sdc_path_hygienic("a\\b"), "backslash refused");
    OK(!sdc_path_hygienic("a//b"), "empty segment refused");
    OK(!sdc_path_hygienic(""), "empty path refused");
    char longp[260]; memset(longp, 'a', sizeof longp); longp[259] = 0;
    OK(!sdc_path_hygienic(longp), "overlong path refused");
    OK(!sdc_path_hygienic("a\x01b"), "control char refused");
}

static void test_allow(void)
{
    // allowed
    OK(sdc_path_writable("www/shell/index.html"), "shell allowed");
    OK(sdc_path_writable("apps/calc/www/app.js"), "bundled app allowed");
    OK(sdc_path_writable("system/registry/apps.json"), "registry allowed");
    OK(sdc_path_writable("system/ir/presets.bin"), "ir pack allowed");
    OK(sdc_path_writable("data/anima/anima-it-akb5.bin"), "anima brain allowed");
    OK(sdc_path_writable("data/anima/akb5/shard-00.bin"), "akb5 shard allowed");
    OK(sdc_path_writable("data/anima/dict-it.bin"), "dict allowed");
    OK(sdc_path_writable("data/anima/commands.bin"), "commands allowed");
    OK(sdc_path_writable("data/anima/learned/facets.it.jsonl"), "facet seed it allowed");
    OK(sdc_path_writable("data/anima/learned/facets.en.jsonl"), "facet seed en allowed");
    OK(sdc_path_writable("wallpapers/default.jpg"), "wallpaper allowed");
    OK(sdc_path_writable("evilportal/google.html"), "evilportal allowed");
    // refused — user/device state
    OK(!sdc_path_writable("apps/theme.cfg"), "theme.cfg refused");
    OK(!sdc_path_writable("apps/calc/data/save.json"), "app user-data refused");
    OK(!sdc_path_writable("system/config/setup.json"), "config refused");
    OK(!sdc_path_writable("system/keys/host.pem"), "keys refused");
    OK(!sdc_path_writable("system/sessions/s.json"), "sessions refused");
    OK(!sdc_path_writable("data/anima/teacher.json"), "api-key vault refused");
    OK(!sdc_path_writable("data/anima/learned/cache.jsonl"), "learned cache refused");
    OK(!sdc_path_writable("data/anima/sessions.json"), "anima sessions refused");
    OK(sdc_path_writable("data/tts/it/clips.pcm"), "tts bank it allowed (pipeline parity)");
    OK(sdc_path_writable("data/tts/en/index.bin"), "tts bank en allowed (pipeline parity)");
    OK(!sdc_path_writable("data/tts/speak.cfg"), "tts runtime cfg refused");
    OK(!sdc_path_writable("data/tts/fr/x.bin"), "tts other lang refused");
    OK(!sdc_path_writable("data/Documents/note.txt"), "user documents refused");
    OK(!sdc_path_writable("backups/b.bin"), "backups refused");
    OK(!sdc_path_writable("auth.json"), "auth refused");
}

static void test_header(void)
{
    sdc_header_t h;
    OK(sdc_parse_header("#nucleoos-sd 1 0.5.0 1261 53400000", &h) && h.files == 1261 && h.bytes == 53400000ULL
       && !strcmp(h.tag, "0.5.0"), "header parsed");
    OK(!sdc_parse_header("#nucleoos-sd 2 0.5.0 1 1", &h), "wrong version refused");
    OK(!sdc_parse_header("#pack core 100 200", &h), "pack line is not a header");
    OK(!sdc_parse_header("garbage", &h), "garbage header refused");
}

static void test_file(void)
{
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char line[300]; sdc_file_t f; bool cmt;
    snprintf(line, sizeof line, "%s 1234 core w www/shell/app.js", sha);
    OK(sdc_parse_file(line, &f, &cmt) && f.size == 1234 && f.mode == SDC_MODE_WRITE
       && !strcmp(f.path, "www/shell/app.js") && !strcmp(f.pack, "core"), "file line parsed");

    snprintf(line, sizeof line, "%s 10 core m system/registry/apps.json", sha);
    OK(sdc_parse_file(line, &f, &cmt) && f.mode == SDC_MODE_MERGE, "merge mode parsed");

    snprintf(line, sizeof line, "%s 10 core c data/anima/learned/facets.it.jsonl", sha);
    OK(sdc_parse_file(line, &f, &cmt) && f.mode == SDC_MODE_CREATE, "create mode parsed");

    // hostile: a path outside the allow-list must make the LINE fail (the run then fails loudly).
    snprintf(line, sizeof line, "%s 10 core w ../../system/keys/evil", sha);
    OK(!sdc_parse_file(line, &f, &cmt), "hostile path line refused");
    snprintf(line, sizeof line, "%s 10 core w system/config/setup.json", sha);
    OK(!sdc_parse_file(line, &f, &cmt), "config path line refused");

    // comments / blanks
    OK(!sdc_parse_file("#pack core 10 20", &f, &cmt) && cmt, "pack line flagged comment");
    OK(!sdc_parse_file("   ", &f, &cmt) && cmt, "blank flagged comment");

    // malformed
    snprintf(line, sizeof line, "nothex 10 core w www/shell/x");
    OK(!sdc_parse_file(line, &f, &cmt) && !cmt, "bad sha refused");
    snprintf(line, sizeof line, "%s 10 core z www/shell/x", sha);
    OK(!sdc_parse_file(line, &f, &cmt), "bad mode refused");
}

static void test_tag(void)
{
    OK(sdc_tag_matches("0.5.0+7.gabc", "0.5.0"), "tag matches ignoring +build");
    OK(sdc_tag_matches("0.5.0", "v0.5.0"), "tag matches with v prefix");
    OK(!sdc_tag_matches("0.4.0", "0.5.0"), "different minor does not match");
}

static void test_plan(void)
{
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    sdc_file_t f; memset(&f, 0, sizeof f); strcpy(f.sha, sha); f.size = 100; f.mode = SDC_MODE_WRITE;
    OK(sdc_plan(&f, false, 0, NULL) == SDC_PLAN_WRITE, "absent -> write");
    OK(sdc_plan(&f, true, 100, sha) == SDC_PLAN_SKIP, "identical -> skip");
    OK(sdc_plan(&f, true, 100, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff") == SDC_PLAN_WRITE, "same size diff sha -> write");
    OK(sdc_plan(&f, true, 50, NULL) == SDC_PLAN_WRITE, "diff size -> write");
    f.mode = SDC_MODE_CREATE;
    OK(sdc_plan(&f, true, 100, sha) == SDC_PLAN_CREATE_SKIP, "create + exists -> skip");
    OK(sdc_plan(&f, false, 0, NULL) == SDC_PLAN_WRITE, "create + absent -> write");
    f.mode = SDC_MODE_MERGE;
    OK(sdc_plan(&f, true, 100, sha) == SDC_PLAN_MERGE, "merge -> merge");
}

int main(void)
{
    test_hygiene();
    test_allow();
    test_header();
    test_file();
    test_tag();
    test_plan();
    printf("sdcontent-policy: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
