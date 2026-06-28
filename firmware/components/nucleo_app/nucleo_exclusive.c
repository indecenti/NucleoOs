// nucleo_exclusive — see nucleo_exclusive.h. Reclaims RAM by suspending heavy subsystems for a
// dedicated native app, restoring them on exit. Wi-Fi STA is left untouched (SSH/network needs it).
#include "nucleo_exclusive.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// Forward-declares: the symbols live in nucleo_httpd / nucleo_discovery / nucleo_voice /
// nucleo_recorder / nucleo_anima, all already pulled into the image by main's REQUIRES. We avoid a
// REQUIRES here (would cycle nucleo_app<->those) — the same link-time escape hatch nucleo_app already
// uses for nucleo_anima_l1_unload(). Resolved at final link.
extern esp_err_t nucleo_httpd_stop(void);
extern esp_err_t nucleo_httpd_start(void);
extern bool      nucleo_anima_l1_unload_if_idle(void);
extern void      nucleo_discovery_stop(void);
extern esp_err_t nucleo_discovery_resume(void);
extern void      nucleo_voice_enable(bool en);
extern void      nucleo_voice_suspend(bool suspend);
extern void      nucleo_recorder_stop(void);
extern bool      nucleo_recorder_is_recording(void);
// Deep-offline Wi-Fi teardown/restore (link-time, like the rest): stop frees the driver+lwip RAM;
// apply_network re-joins the saved STA (or falls back to AP) on exit. suspend() halts auto-reconnect.
extern esp_err_t esp_wifi_stop(void);
extern esp_err_t esp_wifi_start(void);
extern void      nucleo_setup_suspend(void);
extern esp_err_t nucleo_setup_apply_network(void);

static const char *TAG = "exclusive";
static uint32_t s_active = 0;

static inline size_t hfree(void)    { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static inline size_t hlargest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }

bool nucleo_exclusive_active(void) { return s_active != 0; }

bool nucleo_exclusive_enter(uint32_t flags, nucleo_exclusive_info_t *out)
{
    size_t fb = hfree(), lb = hlargest();
    bool acted = false;                       // did THIS call perform the suspend? (owns the matching exit)
    if (!s_active && flags) {
        // Order: cheapest/least-disruptive first; servers + index last so the big blocks free together.
        if (flags & NX_DISCOVERY) { nucleo_discovery_stop();  s_active |= NX_DISCOVERY; }
        if (flags & NX_VOICE)     { nucleo_voice_suspend(true); s_active |= NX_VOICE; }
        if (flags & NX_RECORDER)  { if (nucleo_recorder_is_recording()) nucleo_recorder_stop(); s_active |= NX_RECORDER; }
        if (flags & NX_HTTPD)     { nucleo_httpd_stop();      s_active |= NX_HTTPD; }
        if (flags & NX_ANIMA_L1)  {                          // lazy reload next query; skipped (less reclaim,
            if (!nucleo_anima_l1_unload_if_idle())           // no use-after-free) if a query is mid-cascade
                ESP_LOGW(TAG, "ANIMA busy: L1 not reclaimed");
            s_active |= NX_ANIMA_L1;
        }
        if (flags & NX_WIFI)      { nucleo_setup_suspend(); esp_wifi_stop(); s_active |= NX_WIFI; }  // network down LAST
        acted = true;
        ESP_LOGI(TAG, "enter 0x%02x: free %u->%u, largest %u->%u",
                 (unsigned)s_active, (unsigned)fb, (unsigned)hfree(), (unsigned)lb, (unsigned)hlargest());
    }
    if (out) { out->free_before = fb; out->largest_before = lb; out->free_after = hfree(); out->largest_after = hlargest(); out->stopped = s_active; }
    // Return OWNERSHIP, not global active-ness: true only when this call actually stopped something, so a
    // caller that pairs enter->exit tears down ONLY what it stopped. A no-op (already active, or flags==0)
    // returns false — its exit() would otherwise restore a window another owner (e.g. the session window or
    // the Recorder's background AI task) still needs. Callers' own !active guards become belt-and-suspenders.
    return acted;
}

void nucleo_exclusive_exit(void)
{
    if (!s_active) return;
    uint32_t a = s_active; s_active = 0;
    // Restore in a safe order. ANIMA L1 reloads itself on the next query; the recorder was transient.
    // Wi-Fi comes back FIRST — mDNS and the HTTP server both need the network up before they restart.
    if (a & NX_WIFI)      { esp_wifi_start(); nucleo_setup_apply_network(); }
    if (a & NX_VOICE)     nucleo_voice_suspend(false);
    if (a & NX_DISCOVERY) nucleo_discovery_resume();
    if (a & NX_HTTPD)     nucleo_httpd_start();
    ESP_LOGI(TAG, "exit: restored 0x%02x, free=%u", (unsigned)a, (unsigned)hfree());
}
