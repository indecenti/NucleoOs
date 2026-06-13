#include "nucleo_log.h"
#include "nucleo_board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   // failed-alloc hook: record OOM watermark for /api/diag (no PSRAM -> OOM is the #1 risk)
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define LOG_FILE NUCLEO_SD_MOUNT "/system/logs/system.log"
// The RAM ring is THE fixed runtime cost of the logging system: a static .bss buffer that is
// always resident in scarce internal SRAM (no PSRAM on this board). It holds the last ~N bytes of
// console output for /api/logs and is independent of how many log lines exist — a fixed wall.
// NUCLEO_LOG_RING_SZ is defined in nucleo_log.h (default 2048) so /api/logs sizes its reader to
// match. 2048 still captures a full boot + a query cycle; the 2 KB freed vs the old 4096 went to
// the ANIMA L1 hot-row cache. Override with -DNUCLEO_LOG_RING_SZ=4096 to restore deep boot history.
#define RING_SZ  NUCLEO_LOG_RING_SZ

static vprintf_like_t s_old_vprintf = NULL;

// In-RAM circular buffer of recent log text — survives an unmounted SD, so /api/logs
// can show why the card failed to mount, etc.
static char s_ring[RING_SZ];
static size_t s_pos = 0;
static bool s_wrapped = false;

static void ring_put(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        s_ring[s_pos++] = s[i];
        if (s_pos >= RING_SZ) { s_pos = 0; s_wrapped = true; }
    }
}

size_t nucleo_log_get(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    size_t len = 0;
    if (s_wrapped)
        for (size_t i = s_pos; i < RING_SZ && len < cap - 1; i++) out[len++] = s_ring[i];
    for (size_t i = 0; i < s_pos && len < cap - 1; i++) out[len++] = s_ring[i];
    out[len] = '\0';
    return len;
}

// ── OOM watermark ────────────────────────────────────────────────────────────────────────────────
// A heap allocation that fails is the single most useful crash precursor on this no-PSRAM board (a
// 32 KB TLS handshake that can't find a contiguous block, the L1 index reload mid-query, etc.). The
// hook below runs IN the context of the failing alloc, so it must not allocate: it only bumps static
// counters. We emit ONE breadcrumb on the very first failure (via the lock-free RAM ring above, no
// heap) so /api/logs shows it even if nobody polls /api/diag; after that it's silent counting.
static volatile unsigned s_oom_count, s_oom_last_size, s_oom_last_caps;

static void oom_hook(size_t size, uint32_t caps, const char *fn)
{
    s_oom_last_size = (unsigned)size;
    s_oom_last_caps = (unsigned)caps;
    if (++s_oom_count == 1)
        ESP_LOGE("oom", "alloc fail: %u B caps=0x%x in %s", (unsigned)size, (unsigned)caps, fn ? fn : "?");
}

void nucleo_log_oom(unsigned *count, unsigned *last_size, unsigned *last_caps)
{
    if (count)     *count     = s_oom_count;
    if (last_size) *last_size = s_oom_last_size;
    if (last_caps) *last_caps = s_oom_last_caps;
}

static int nucleo_log_vprintf(const char *fmt, va_list args)
{
    // Capture a copy into the RAM ring (for /api/logs), then pass the ORIGINAL args once
    // to the real console printer. A va_list must not be traversed twice — doing so was an
    // undefined-behaviour crash that reboot-looped the device once the SD log file was open.
    //
    // This scratch buffer sits on the STACK of whichever task is logging — including the 16 KB httpd
    // task while it is deep inside a recursive TLS cert-chain verify (the path that historically
    // overflowed the stack). So keep it lean: 160 B is enough for a heap/status line (timestamp+tag
    // prefix ~25 B + ~110 B message) and shaves peak stack on that critical path. A rare longer line
    // (e.g. a joined ANIMA reasoning trace) is only truncated in the /api/logs COPY — the real serial
    // console still receives the full text below via the original args.
    char line[160];
    va_list cp; va_copy(cp, args);
    int n = vsnprintf(line, sizeof(line), fmt, cp);
    va_end(cp);
    if (n > 0) ring_put(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    return s_old_vprintf ? s_old_vprintf(fmt, args) : vprintf(fmt, args);
}

void nucleo_log_init(void)
{
    if (s_old_vprintf) return;                 // already installed
    // RAM-ring capture only (read via /api/logs). No per-line SD write: it is slow, wears
    // the card, and isn't needed now that logs are reachable over HTTP.
    s_old_vprintf = esp_log_set_vprintf(nucleo_log_vprintf);
    heap_caps_register_failed_alloc_callback(oom_hook);   // record OOM watermark (read via /api/diag)
    ESP_LOGI("logger", "log capture started (RAM ring -> /api/logs)");
}
