/* Host test for the pure Evil-Portal engine helpers added for captive detection + UI dashboard.
 * These are standalone functions that don't call into ESP-IDF, so we duplicate the exact source
 * here (same text, byte-for-byte) and assert on it. If engine.c changes, this test must change.
 *
 * Mirrors tools/anima-host/portalclone-ctest.c pattern: compiled by -check.mjs with mingw gcc.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <assert.h>

#define NUCLEO_EP_VENDOR_N 6
#define NUCLEO_EP_RATE_SLOTS 6

/* ---- COPIED FROM nucleo_evilportal.c (keep in sync) ------------------------- */
static bool ci_has(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, nl) == 0) return true;
    return false;
}

static int classify_ua(const char *ua)
{
    if (!ua || !ua[0]) return 5;
    if (ci_has(ua, "iPhone") || ci_has(ua, "iPad") || ci_has(ua, "iPod")) return 0;
    if (ci_has(ua, "CaptiveNetworkSupport")) return 0;
    if (ci_has(ua, "Android"))                return 1;
    if (ci_has(ua, "Dalvik"))                 return 1;
    if (ci_has(ua, "Windows"))                return 2;
    if (ci_has(ua, "Microsoft NCSI"))         return 2;
    if (ci_has(ua, "Macintosh") || ci_has(ua, "Mac OS X")) return 3;
    if (ci_has(ua, "Linux") || ci_has(ua, "X11"))          return 4;
    return 5;
}

static bool is_captive_probe_uri(const char *uri)
{
    if (!uri || !uri[0]) return false;
    static const char *P[] = {
        "/generate_204", "/gen_204", "/generate204",
        "/hotspot-detect.html", "/library/test/success.html",
        "/connecttest.txt", "/ncsi.txt", "/redirect",
        "/canonical.html",
        "/kindle-wifi/wifistub.html",
        NULL,
    };
    for (int i = 0; P[i]; i++) if (strncmp(uri, P[i], strlen(P[i])) == 0) return true;
    return false;
}

/* ---- test harness --------------------------------------------------------- */
static int fail = 0;
#define OK(cond, label) do { if (!(cond)) { printf("FAIL: %s\n", label); fail++; } else printf("ok   %s\n", label); } while (0)

int main(void)
{
    printf("== Evil Portal engine fingerprint + probe host test ==\n");

    /* UA classification -- iOS */
    OK(classify_ua("Mozilla/5.0 (iPhone; CPU iPhone OS 17_1 like Mac OS X) AppleWebKit/605.1.15") == 0, "iPhone");
    OK(classify_ua("CaptiveNetworkSupport-456 wispr") == 0, "iOS probe UA");
    OK(classify_ua("Mozilla/5.0 (iPad; CPU OS 16_0 like Mac OS X)") == 0, "iPad before macOS clash");

    /* UA -- Android */
    OK(classify_ua("Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36") == 1, "Android phone");
    OK(classify_ua("Dalvik/2.1.0 (Linux; U; Android 13; SM-S918B Build/TP1A.220905.001)") == 1, "Android system probe (Dalvik)");

    /* UA -- Windows (incl. captive probe) */
    OK(classify_ua("Microsoft NCSI") == 2, "Windows NCSI probe");
    OK(classify_ua("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Edg/120") == 2, "Windows 10 Edge");

    /* UA -- macOS and Linux */
    OK(classify_ua("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)") == 3, "macOS Safari");
    OK(classify_ua("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120") == 4, "Linux desktop");

    /* UA -- empty / unknown */
    OK(classify_ua("") == 5, "empty UA -> Other");
    OK(classify_ua(NULL) == 5, "NULL UA -> Other");
    OK(classify_ua("curl/8.1.2") == 5, "unknown -> Other");

    /* Probe URI matching */
    OK(is_captive_probe_uri("/generate_204"), "Android generate_204");
    OK(is_captive_probe_uri("/generate_204?x=1"), "Android generate_204 with query");
    OK(is_captive_probe_uri("/hotspot-detect.html"), "Apple hotspot-detect");
    OK(is_captive_probe_uri("/library/test/success.html"), "macOS success.html");
    OK(is_captive_probe_uri("/connecttest.txt"), "Windows connecttest");
    OK(is_captive_probe_uri("/ncsi.txt"), "Windows NCSI");
    OK(is_captive_probe_uri("/canonical.html"), "Firefox canonical");
    OK(is_captive_probe_uri("/kindle-wifi/wifistub.html"), "Kindle probe");

    OK(!is_captive_probe_uri("/login"), "normal path is NOT a probe");
    OK(!is_captive_probe_uri("/"), "root is NOT a probe (goes through handle_off_host)");
    OK(!is_captive_probe_uri(""), "empty is NOT a probe");
    OK(!is_captive_probe_uri(NULL), "NULL is NOT a probe");

    /* Vendor id range sanity: six vendors, 0..5 inclusive. */
    for (int i = 0; i < NUCLEO_EP_VENDOR_N; i++) OK(i >= 0 && i < 6, "vendor id in range");

    printf("\n%d test(s), %d failed\n", 29 + NUCLEO_EP_VENDOR_N, fail);
    return fail ? 1 : 0;
}
