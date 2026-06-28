// Host test for the Evil Portal page-cloner asset rewriter (firmware/components/nucleo_evilportal/
// nucleo_evilportal_clone.c). Compiled + run by portalclone-check.mjs with MinGW gcc — no ESP-IDF, no
// networking. Proves the same-origin detection, relative/root-relative/absolute URL resolution, the
// extension filter, the in-place shrink-only rewrite, and the cross-origin / data: / unknown-ext
// rejections BEFORE any of it touches a live clone. FOR AUTHORIZED TESTING ONLY — string logic only.
#include "nucleo_evilportal_clone.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

int main(void)
{
    printf("== nucleo_evilportal_clone host test ==\n");
    ep_asset_t a[EP_ASSET_MAX];

    // 1) Absolute same-origin stylesheet -> rewritten to a local path; URL recorded verbatim.
    {
        char h[256] = "<link rel=stylesheet href=\"http://portal.test/css/main.css\">";
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 1, "abs css: one asset");
        CHECK(strcmp(a[0].url, "http://portal.test/css/main.css") == 0, "abs css: url recorded");
        CHECK(strcmp(a[0].local, "/a/0.css") == 0, "abs css: local path");
        CHECK(has(h, "href=\"/a/0.css\""), "abs css: html rewritten");
        CHECK(!has(h, "main.css"), "abs css: original gone");
    }
    // 2) Root-relative image -> resolved against host.
    {
        char h[256] = "<img src=\"/img/logo.png\">";
        int n = ep_collect_assets(h, "http://portal.test/dir/login", a, EP_ASSET_MAX);
        CHECK(n == 1, "root-rel img: one asset");
        CHECK(strcmp(a[0].url, "http://portal.test/img/logo.png") == 0, "root-rel img: url");
        CHECK(strcmp(a[0].local, "/a/0.png") == 0, "root-rel img: local");
        CHECK(has(h, "src=\"/a/0.png\""), "root-rel img: rewritten");
    }
    // 3) Document-relative ref -> resolved against the page's directory.
    {
        char h[256] = "<link href=\"styles_main.css\">";
        int n = ep_collect_assets(h, "http://portal.test/dir/login", a, EP_ASSET_MAX);
        CHECK(n == 1, "doc-rel: one asset");
        CHECK(strcmp(a[0].url, "http://portal.test/dir/styles_main.css") == 0, "doc-rel: url resolved");
    }
    // 4) Cross-origin asset -> left untouched.
    {
        char h[256] = "<script src=\"http://cdn.other.test/app.js\"></script>";
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 0, "cross-origin: skipped");
        CHECK(has(h, "cdn.other.test/app.js"), "cross-origin: untouched");
    }
    // 5) data: URI -> untouched.
    {
        char h[256] = "<img src=\"data:image/png;base64,AAAA\">";
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 0 && has(h, "data:image/png"), "data uri untouched");
    }
    // 6) Unknown extension (.html) -> not an asset, untouched.
    {
        char h[256] = "<a href=\"/next_page.html\">go</a>";
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 0 && has(h, "next_page.html"), "unknown ext untouched");
    }
    // 7) Shrink-only rule: a value shorter than the local path is left as-is (no buffer growth).
    {
        char h[256] = "<script src=\"a.js\"></script>";   // value "a.js" (4) < "/a/0.js" (7)
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 0 && has(h, "src=\"a.js\""), "short value not grown");
    }
    // 8) Mixed page: 2 same-origin assets rewritten, cross-origin + anchor left alone.
    {
        char h[512] =
            "<link href=\"http://portal.test/s/site.css\">"
            "<script src=\"http://cdn.x.test/v.js\"></script>"
            "<img src=\"/media/hero_banner.png\">"
            "<a href=\"/login.html\">x</a>";
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == 2, "mixed: two assets");
        CHECK(has(h, "/a/0.css") && has(h, "/a/1.png"), "mixed: both rewritten");
        CHECK(has(h, "cdn.x.test/v.js") && has(h, "/login.html"), "mixed: non-assets kept");
    }
    // 9) Cap: more than EP_ASSET_MAX assets -> only EP_ASSET_MAX recorded.
    {
        char h[4096]; int o = 0;
        for (int i = 0; i < EP_ASSET_MAX + 6; i++)
            o += sprintf(h + o, "<link href=\"http://portal.test/style_number_%02d.css\">", i);
        int n = ep_collect_assets(h, "http://portal.test/login", a, EP_ASSET_MAX);
        CHECK(n == EP_ASSET_MAX, "cap at EP_ASSET_MAX");
    }
    // 10) Bad base URL -> 0, no crash.
    {
        char h[64] = "<link href=\"/x/style_long.css\">";
        CHECK(ep_collect_assets(h, "not-a-url", a, EP_ASSET_MAX) == 0, "bad base -> 0");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
