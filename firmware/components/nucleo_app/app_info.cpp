// Built-in "Connection" app: a scrollable info sheet — how to reach the device from a browser
// (Wi-Fi / IP / pairing PIN), the useful web URLs to open, and live device stats (board, firmware,
// RAM, battery, SD, MAC, uptime). Big readable rows + smooth scroll, in line with the other apps.
// Buffered rendering (re-acquires the shared canvas) so it never flickers.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nucleo_power.h"
#include "nucleo_storage.h"
#include "nucleo_ui.h"

extern "C" const char *nucleo_auth_pin(void);
extern "C" const char *nucleo_setup_mode(void);
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_ip(void);
extern "C" const char *nucleo_setup_device_name(void);

// ---- row model ----------------------------------------------------------------------------------
enum { RK_HEAD, RK_KV };
struct Row { uint8_t kind; char label[18]; char val[34]; uint16_t col; };
static Row   s_row[40];
static int   s_nrow;
static float s_scroll;        // eased top-row offset
static int   s_target;        // desired top row
static int   s_vis = 5;       // rows visible (recomputed in draw)

static void add_head(const char *l, uint16_t c)
{
    if (s_nrow >= 40) return;
    Row &r = s_row[s_nrow++]; r.kind = RK_HEAD; r.col = c; r.val[0] = 0;
    snprintf(r.label, sizeof r.label, "%s", l);
}
static void add_kv(const char *l, const char *v, uint16_t c)
{
    if (s_nrow >= 40) return;
    Row &r = s_row[s_nrow++]; r.kind = RK_KV; r.col = c;
    snprintf(r.label, sizeof r.label, "%s", l);
    snprintf(r.val, sizeof r.val, "%s", v ? v : "");
}

static void build_rows(void)
{
    s_nrow = 0;
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];
    const char *ip = nucleo_setup_ip();
    char ipb[24]; snprintf(ipb, sizeof ipb, "%s", (sta && ip[0]) ? ip : "192.168.4.1");
    char tmp[34];

    // ---- Connection ----
    add_head("CONNESSIONE", C_BLUE);
    add_kv("Rete", sta ? nucleo_setup_ssid() : "Setup AP", C_BLUE);
    add_kv("Indirizzo", ipb, C_GREEN);
    add_kv("PIN", (sta && ip[0]) ? nucleo_auth_pin() : "unisci il Wi-Fi", C_YELLOW);

    // ---- Web links (open in a browser; /apps/<id>/ serves that app's index.html) ----
    add_head("WEB - apri nel browser", C_GREEN);
    add_kv("Desktop", "/", FG);
    add_kv("Setup", "/apps/settings", FG);
    add_kv("File", "/apps/file-commander", FG);
    add_kv("Monitor", "/apps/system-monitor", FG);
    add_kv("Log", "/apps/log-viewer", FG);
    add_kv("Aggiorna", "/apps/updates", FG);
    add_kv("Aiuto/API", "/apps/help", FG);

    // ---- Device ----
    add_head("DISPOSITIVO", C_YELLOW);
    add_kv("Modello", nucleo_ui_is_adv() ? "Cardputer ADV" : "Cardputer", FG);
    const esp_app_desc_t *ad = esp_app_get_description();
    if (ad) { snprintf(tmp, sizeof tmp, "v%s", ad->version); add_kv("Firmware", tmp, FG);
              add_kv("ESP-IDF", ad->idf_ver, MUTED); }
    snprintf(tmp, sizeof tmp, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024)); add_kv("RAM libera", tmp, FG);
    snprintf(tmp, sizeof tmp, "%u KB", (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024));
    add_kv("Blocco max", tmp, MUTED);
    int pct = nucleo_power_battery_pct();
    if (pct >= 0) { int mv = nucleo_power_battery_mv();
                    if (mv > 0) snprintf(tmp, sizeof tmp, "%d%%  %d.%02dV", pct, mv / 1000, (mv % 1000) / 10);
                    else        snprintf(tmp, sizeof tmp, "%d%%", pct);
                    add_kv("Batteria", tmp, pct <= 15 ? C_RED : C_GREEN); }
    else add_kv("Batteria", "USB", MUTED);
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(tmp, sizeof tmp, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        add_kv("MAC", tmp, MUTED);
    }
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(tmp, sizeof tmp, "%dh %dm", (int)(up / 3600), (int)((up % 3600) / 60)); add_kv("Acceso da", tmp, FG);

    // ---- Storage ----
    add_head("ARCHIVIO (SD)", C_PINK);
    const nucleo_storage_info_t *si = nucleo_storage_info();
    if (si && si->mounted) {
        add_kv("Scheda", si->fs_type[0] ? si->fs_type : "OK", C_GREEN);
        double fg = si->free_bytes / 1073741824.0, tg = si->total_bytes / 1073741824.0;
        snprintf(tmp, sizeof tmp, "%.1f / %.1f GB", fg, tg); add_kv("Spazio", tmp, FG);
    } else {
        add_kv("Scheda", "assente", C_RED);
    }
}

static int max_top(void) { int m = s_nrow - s_vis; return m > 0 ? m : 0; }

static bool info_poll(void)
{
    bool moving = false;
    float tgt = (float)s_target;
    if (s_scroll != tgt) { s_scroll += (tgt - s_scroll) * 0.35f; if (fabsf(s_scroll - tgt) < 0.02f) s_scroll = tgt; moving = true; }
    static int64_t last = 0; int64_t now = esp_timer_get_time();
    bool tick = (now - last > 1000000);                 // refresh live stats (RAM/battery/uptime) ~1 Hz
    if (tick) last = now;
    return moving || tick;
}

static void info_enter(void)
{
    nucleo_screen_acquire();                            // buffered -> no flicker (canvas may have been freed by a media app)
    s_scroll = 0; s_target = 0;
    nucleo_app_set_poll_handler(info_poll);
    nucleo_app_set_hint("su/giu scorri   esc indietro");
    nucleo_app_request_draw();
}

static void info_key(int key, char ch)
{
    (void)ch;
    if (key == NK_UP   && s_target > 0)          { s_target--; nucleo_app_request_draw(); }
    else if (key == NK_DOWN && s_target < max_top()) { s_target++; nucleo_app_request_draw(); }
}

static void info_draw(void)
{
    int top = nucleo_app_content_top(), ch = nucleo_app_content_height();
    const int rowH = 20;
    s_vis = ch / rowH; if (s_vis < 1) s_vis = 1;
    build_rows();
    if (s_target > max_top()) s_target = max_top();

    d.fillRect(0, top, W, ch, BG);

    int start = (int)s_scroll; float frac = s_scroll - (float)start;
    for (int r = 0; r <= s_vis; r++) {
        int idx = start + r; if (idx < 0 || idx >= s_nrow) continue;
        int ry = top + (int)((float)r * rowH - frac * rowH);
        if (ry + rowH <= top || ry >= top + ch) continue;
        Row &R = s_row[idx];
        if (R.kind == RK_HEAD) {
            d.fillRect(6, ry + rowH - 4, W - 14, 2, R.col);                       // section underline
            d.setTextSize(1); d.setTextColor(R.col, BG); d.setCursor(8, ry + 5); d.print(R.label);
            continue;
        }
        d.setTextSize(2); d.setTextColor(R.col, BG); d.setCursor(8, ry + 2); d.print(R.label);
        if (!R.val[0]) continue;
        int lw = (int)strlen(R.label) * 12, vw2 = (int)strlen(R.val) * 12;
        bool big = (8 + lw + 14 + vw2 <= W);                                      // big value only when it fits beside the label
        if (big) { d.setTextSize(2); d.setTextColor(0xFFFF, BG); d.setCursor(W - 8 - vw2, ry + 2); d.print(R.val); }
        else     { d.setTextSize(1); d.setTextColor(MUTED, BG); int vw = (int)strlen(R.val) * 6;
                   d.setCursor(W - 8 - vw, ry + 7); d.print(R.val); }
    }

    // scrollbar
    if (s_nrow > s_vis) {
        int trackH = ch - 4, th = trackH * s_vis / s_nrow;
        int ty = top + 2 + (trackH - th) * start / (max_top() > 0 ? max_top() : 1);
        d.fillRect(W - 3, top + 2, 2, trackH, MUTED);
        d.fillRect(W - 3, ty, 2, th, C_BLUE);
    }
}

extern "C" void nucleo_register_info(void)
{
    static const nucleo_app_def_t app = {
        "info", "Connection", "System", "Wi-Fi, indirizzi web e info dispositivo",
        'i', C_BLUE, info_enter, info_key, nullptr, info_draw, nullptr
    };
    nucleo_app_register(&app);
}
