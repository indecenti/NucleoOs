// Pure HTML asset rewriter for the page cloner (F2). See nucleo_evilportal_clone.h. No ESP-IDF.
#include "nucleo_evilportal_clone.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Known static asset extensions we are willing to mirror + serve. Order longest-first so "woff2"
// is matched before "woff". Each maps to a short local extension (kept <= 5 chars for "/a/<k>.<ext>").
static const char *EXTS[] = { "woff2", "woff", "jpeg", "webp", "css", "js", "png", "jpg",
                              "gif", "svg", "ico", "ttf", "otf", NULL };

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

// Pull scheme ("http"/"https") and host out of an absolute base URL. dir gets the directory portion
// of the path (everything up to and including the last '/', or "/" if none). Returns 0 on failure.
static int parse_base(const char *base, char *scheme, int sc, char *host, int hc, char *dir, int dc)
{
    if (!base) return 0;
    const char *p = strstr(base, "://");
    if (!p) return 0;
    int slen = (int)(p - base);
    if (slen <= 0 || slen >= sc) return 0;
    memcpy(scheme, base, slen); scheme[slen] = 0;
    const char *h = p + 3;
    const char *hend = h;
    while (*hend && *hend != '/' && *hend != '?' && *hend != '#') hend++;
    int hlen = (int)(hend - h);
    if (hlen <= 0 || hlen >= hc) return 0;
    memcpy(host, h, hlen); host[hlen] = 0;
    // Directory = path up to the last '/'. If the path is empty or has no '/', dir = "/".
    const char *path = hend;
    const char *lastslash = path;
    for (const char *q = path; *q && *q != '?' && *q != '#'; q++) if (*q == '/') lastslash = q;
    if (lastslash == path) { if (dc < 2) return 0; dir[0] = '/'; dir[1] = 0; }
    else {
        int dlen = (int)(lastslash - path) + 1;     // include the trailing '/'
        if (dlen >= dc) return 0;
        memcpy(dir, path, dlen); dir[dlen] = 0;
    }
    return 1;
}

// Resolve a (possibly relative) ref to an absolute URL given the base parts. Returns 0 if the ref is
// one we should skip (data:/javascript:/mailto:/anchor/empty) or doesn't fit. Same-origin only:
// absolute refs to a different host are rejected here too.
static int resolve_url(const char *ref, int reflen, const char *scheme, const char *host,
                       const char *dir, char *out, int oc)
{
    if (reflen <= 0) return 0;
    // Reject schemes/anchors we never mirror.
    if (ref[0] == '#') return 0;
    if (reflen >= 5 && ci_eq(ref, "data:", 5)) return 0;
    if (reflen >= 11 && ci_eq(ref, "javascript:", 11)) return 0;
    if (reflen >= 7 && ci_eq(ref, "mailto:", 7)) return 0;

    char tmp[EP_ASSET_URL_MAX];
    int n = 0;
    if ((reflen >= 7 && ci_eq(ref, "http://", 7)) || (reflen >= 8 && ci_eq(ref, "https://", 8))) {
        if (reflen >= (int)sizeof tmp) return 0;
        memcpy(tmp, ref, reflen); tmp[reflen] = 0;
        // same-origin test: host must match ours
        const char *hp = strstr(tmp, "://"); if (!hp) return 0; hp += 3;
        int hl = (int)strlen(host);
        if (strncmp(hp, host, hl) != 0 || (hp[hl] != '/' && hp[hl] != 0 && hp[hl] != ':')) return 0;
        n = reflen;
    } else if (reflen >= 2 && ref[0] == '/' && ref[1] == '/') {     // scheme-relative //host/path
        n = snprintf(tmp, sizeof tmp, "%s:%.*s", scheme, reflen, ref);
        const char *hp = strstr(tmp, "://"); if (!hp) return 0; hp += 3;
        int hl = (int)strlen(host);
        if (strncmp(hp, host, hl) != 0 || (hp[hl] != '/' && hp[hl] != 0)) return 0;
    } else if (ref[0] == '/') {                                     // root-relative /path
        n = snprintf(tmp, sizeof tmp, "%s://%s%.*s", scheme, host, reflen, ref);
    } else {                                                        // document-relative path
        n = snprintf(tmp, sizeof tmp, "%s://%s%s%.*s", scheme, host, dir, reflen, ref);
    }
    if (n <= 0 || n >= (int)sizeof tmp || n >= oc) return 0;
    memcpy(out, tmp, n); out[n] = 0;
    return 1;
}

// If the URL's path ends in a known static extension (before any ?/#), return its index in EXTS,
// else -1. Also outputs the matched extension string.
static int url_ext(const char *url, const char **ext_out)
{
    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;
    for (int i = 0; EXTS[i]; i++) {
        int el = (int)strlen(EXTS[i]);
        if (end - url > el && end[-el - 1] == '.' && ci_eq(end - el, EXTS[i], el)) {
            *ext_out = EXTS[i];
            return i;
        }
    }
    return -1;
}

// Find the next href=/src= attribute value starting at *pp. On success sets *vstart to the first
// char of the value, *vlen to its length (quotes stripped), advances *pp past the value, and returns
// 1. Returns 0 at end of string. Handles single/double quotes; skips unquoted values (too ambiguous).
static int next_attr_value(char **pp, char **vstart, int *vlen)
{
    char *p = *pp;
    while (*p) {
        // attribute name must start at a boundary (preceding char is space/quote/'<' or start)
        int is_href = (tolower((unsigned char)p[0]) == 'h' && ci_eq(p, "href", 4));
        int is_src  = (tolower((unsigned char)p[0]) == 's' && ci_eq(p, "src", 3));
        if (is_href || is_src) {
            char before = (p == *pp) ? ' ' : p[-1];
            if (before == ' ' || before == '\t' || before == '\n' || before == '\r' ||
                before == '"' || before == '\'' || before == '<' || before == '/') {
                char *a = p + (is_href ? 4 : 3);
                while (*a == ' ' || *a == '\t') a++;
                if (*a == '=') {
                    a++;
                    while (*a == ' ' || *a == '\t') a++;
                    if (*a == '"' || *a == '\'') {
                        char q = *a++;
                        char *v = a;
                        while (*a && *a != q) a++;
                        if (*a == q) {
                            *vstart = v; *vlen = (int)(a - v);
                            *pp = a + 1;        // past closing quote
                            return 1;
                        }
                    }
                }
            }
        }
        p++;
    }
    *pp = p;
    return 0;
}

int ep_collect_assets(char *html, const char *base_url, ep_asset_t *assets, int max)
{
    if (!html || !assets || max <= 0) return 0;
    if (max > EP_ASSET_MAX) max = EP_ASSET_MAX;

    char scheme[8], host[80], dir[160];
    if (!parse_base(base_url, scheme, sizeof scheme, host, sizeof host, dir, sizeof dir)) return 0;

    int k = 0;
    char *p = html;
    char *vstart; int vlen;
    while (k < max && next_attr_value(&p, &vstart, &vlen)) {
        char abs[EP_ASSET_URL_MAX];
        if (!resolve_url(vstart, vlen, scheme, host, dir, abs, sizeof abs)) continue;
        const char *ext = NULL;
        if (url_ext(abs, &ext) < 0) continue;

        char local[16];
        int ln = snprintf(local, sizeof local, "/a/%d.%s", k, ext);
        if (ln <= 0 || ln >= (int)sizeof local) continue;
        if (ln > vlen) continue;                 // refuse to grow the buffer (in-place must shrink)

        // Record, then rewrite in place: overwrite the value with the local path and close the gap.
        memcpy(assets[k].url, abs, strlen(abs) + 1);
        memcpy(assets[k].local, local, ln + 1);
        memcpy(vstart, local, ln);
        if (ln < vlen) memmove(vstart + ln, vstart + vlen, strlen(vstart + vlen) + 1);
        p = vstart + ln;                         // continue scanning after the rewrite
        k++;
    }
    return k;
}
