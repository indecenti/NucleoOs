// Built-in "Connection" app: a scrollable info sheet — how to reach the device from a browser
// (Wi-Fi / IP / pairing PIN), the useful web URLs to open, and live device stats (board, firmware,
// RAM, battery, SD, MAC, uptime). Big readable rows + smooth scroll, in line with the other apps.
// Buffered rendering (re-acquires the shared canvas) so it never flickers.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"       // TR(it,en): hint follows the system language
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
static Row  *s_row = nullptr;   // heap-on-enter (was .bss ~2.2 KB); rebuilt each draw, freed on exit
static int   s_nrow;
static float s_scroll;        // eased top-row offset
static int   s_target;        // desired top row
static int   s_vis = 5;       // rows visible (recomputed in draw)

static void add_head(const char *l, uint16_t c)
{
    if (!s_row || s_nrow >= 40) return;
    Row &r = s_row[s_nrow++]; r.kind = RK_HEAD; r.col = c; r.val[0] = 0;
    snprintf(r.label, sizeof r.label, "%s", l);
}
static void add_kv(const char *l, const char *v, uint16_t c)
{
    if (!s_row || s_nrow >= 40) return;
    Row &r = s_row[s_nrow++]; r.kind = RK_KV; r.col = c;
    snprintf(r.label, sizeof r.label, "%s", l);
    snprintf(r.val, sizeof r.val, "%s", v ? v : "");
}

static void build_rows(void)
{
    s_nrow = 0;
    if (!s_row) return;
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];
    const char *ip = nucleo_setup_ip();
    char ipb[24]; snprintf(ipb, sizeof ipb, "%s", (sta && ip[0]) ? ip : "192.168.4.1");
    char tmp[34];

    // ---- Connection ---- (labels localized in all five languages; universal terms — PIN, MAC,
    // Firmware, ESP-IDF, Desktop, Monitor, Log — stay as-is, and so do the web-link paths.)
    add_head(TR5("CONNESSIONE", "CONNECTION", "CONEXION", "CONNEXION", "VERBINDUNG"), C_BLUE);
    add_kv(TR5("Rete", "Network", "Red", "Reseau", "Netz"), sta ? nucleo_setup_ssid() : "Setup AP", C_BLUE);
    add_kv(TR5("Indirizzo", "Address", "Direccion", "Adresse", "Adresse"), ipb, C_GREEN);
    add_kv("PIN", (sta && ip[0]) ? nucleo_auth_pin()
                : TR5("unisci il Wi-Fi", "join Wi-Fi", "unir Wi-Fi", "rejoins le Wi-Fi", "WLAN beitreten"), C_YELLOW);

    // ---- Web links (open in a browser; /apps/<id>/ serves that app's index.html) ----
    add_head(TR5("WEB - apri nel browser", "WEB - open in browser", "WEB - abrir en navegador",
                 "WEB - ouvrir navigateur", "WEB - im Browser oeffnen"), C_GREEN);
    add_kv("Desktop", "/", FG);
    add_kv(TR5("Configura", "Setup", "Configurar", "Config", "Einrichten"), "/apps/settings", FG);
    add_kv(TR5("File", "Files", "Archivos", "Fichiers", "Dateien"), "/apps/file-commander", FG);
    add_kv("Monitor", "/apps/system-monitor", FG);
    add_kv("Log", "/apps/log-viewer", FG);
    add_kv(TR5("Aggiorna", "Updates", "Novedades", "Mises a jour", "Updates"), "/apps/updates", FG);
    add_kv(TR5("Aiuto/API", "Help/API", "Ayuda/API", "Aide/API", "Hilfe/API"), "/apps/help", FG);

    // ---- Device ----
    add_head(TR5("DISPOSITIVO", "DEVICE", "DISPOSITIVO", "APPAREIL", "GERAET"), C_YELLOW);
    add_kv(TR5("Modello", "Model", "Modelo", "Modele", "Modell"), nucleo_ui_is_adv() ? "Cardputer ADV" : "Cardputer", FG);
    const esp_app_desc_t *ad = esp_app_get_description();
    if (ad) { snprintf(tmp, sizeof tmp, "v%s", ad->version); add_kv("Firmware", tmp, FG);
              add_kv("ESP-IDF", ad->idf_ver, MUTED); }
    snprintf(tmp, sizeof tmp, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    add_kv(TR5("RAM libera", "Free RAM", "RAM libre", "RAM libre", "Freier RAM"), tmp, FG);
    snprintf(tmp, sizeof tmp, "%u KB", (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024));
    add_kv(TR5("Blocco max", "Max block", "Bloque max", "Bloc max", "Max Block"), tmp, MUTED);
    int pct = nucleo_power_battery_pct();
    if (pct >= 0) { int mv = nucleo_power_battery_mv();
                    if (mv > 0) snprintf(tmp, sizeof tmp, "%d%%  %d.%02dV", pct, mv / 1000, (mv % 1000) / 10);
                    else        snprintf(tmp, sizeof tmp, "%d%%", pct);
                    add_kv(TR5("Batteria", "Battery", "Bateria", "Batterie", "Akku"), tmp, pct <= 15 ? C_RED : C_GREEN); }
    else add_kv(TR5("Batteria", "Battery", "Bateria", "Batterie", "Akku"), "USB", MUTED);
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(tmp, sizeof tmp, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        add_kv("MAC", tmp, MUTED);
    }
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(tmp, sizeof tmp, "%dh %dm", (int)(up / 3600), (int)((up % 3600) / 60));
    add_kv(TR5("Acceso da", "Uptime", "Encendido", "Allume depuis", "Laufzeit"), tmp, FG);

    // ---- Storage ----
    add_head(TR5("ARCHIVIO (SD)", "STORAGE (SD)", "ALMACEN (SD)", "STOCKAGE (SD)", "SPEICHER (SD)"), C_PINK);
    const nucleo_storage_info_t *si = nucleo_storage_info();
    if (si && si->mounted) {
        add_kv(TR5("Scheda", "Card", "Tarjeta", "Carte", "Karte"), si->fs_type[0] ? si->fs_type : "OK", C_GREEN);
        double fg = si->free_bytes / 1073741824.0, tg = si->total_bytes / 1073741824.0;
        snprintf(tmp, sizeof tmp, "%.1f / %.1f GB", fg, tg);
        add_kv(TR5("Spazio", "Space", "Espacio", "Espace", "Platz"), tmp, FG);
    } else {
        add_kv(TR5("Scheda", "Card", "Tarjeta", "Carte", "Karte"),
               TR5("assente", "absent", "ausente", "absente", "fehlt"), C_RED);
    }
}

static int max_top(void) { int m = s_nrow - s_vis; return m > 0 ? m : 0; }

// ---- repaint bookkeeping (ANTI-FLICKER.md) -------------------------------------------------------
// The shared back-buffer is NOT guaranteed: mid-session (a web client took Remote, a decoder ran) the
// 32 KB canvas is gone and the framework hands this app the PANEL directly. The old draw cleared the
// whole content area and redrew it on every frame — once per second for the live stats, plus one frame
// per easing step of the scroll — which on the direct path is exactly the clear-then-draw cadence that
// blinks. So: buffered -> draw everything (the sprite is wiped for us); direct -> repaint only the rows
// whose content or position actually changed, each in its own row box, and snap the scroll instead of
// easing it (one repaint per keypress, not ten frames of full-area clear).
#define MAX_SLOT 10
static uint32_t s_slot[MAX_SLOT];    // signature of what is currently ON the panel in each visible slot
static bool     s_buffered = true;   // last known draw path (set in info_draw, read by info_poll)
static uint32_t s_content_sig;       // signature of the whole row model - gates the 1 Hz repaint
static unsigned s_seen_gen;          // framework panel generation we last drew against

static uint32_t fnv_s(uint32_t h, const char *s) { while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static uint32_t rows_sig(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < s_nrow; i++) {
        h = fnv_s(h, s_row[i].label); h = fnv_s(h, s_row[i].val);
        h ^= s_row[i].col; h *= 16777619u;
    }
    return h ? h : 1;
}
static uint32_t slot_sig(const Row &R, int ry)
{
    uint32_t h = fnv_s(fnv_s(2166136261u, R.label), R.val);
    h ^= (uint32_t)R.col;        h *= 16777619u;
    h ^= (uint32_t)(ry + 4096);  h *= 16777619u;
    h ^= (uint32_t)R.kind;       h *= 16777619u;
    return h ? h : 1;                                   // 0 is reserved for "slot empty / cleared"
}
static void slots_invalidate(void) { for (int i = 0; i < MAX_SLOT; i++) s_slot[i] = 0xFFFFFFFFu; }

static bool info_poll(void)
{
    bool redraw = false;
    float tgt = (float)s_target;
    if (s_scroll != tgt) {
        if (s_buffered) { s_scroll += (tgt - s_scroll) * 0.35f; if (fabsf(s_scroll - tgt) < 0.02f) s_scroll = tgt; }
        else            s_scroll = tgt;                 // no buffer + no vsync -> snap, never animate
        redraw = true;
    }
    static int64_t last = 0; int64_t now = esp_timer_get_time();
    if (now - last > 1000000) {                         // refresh live stats (RAM/battery/uptime) ~1 Hz
        last = now;
        // If the enter-time calloc lost to fragmentation, the sheet would stay blank forever (every
        // build_rows() no-ops on a null s_row, sig never moves). Retry here — fragmentation is
        // transient on this device, so the sheet self-heals the moment heap frees up.
        if (!s_row) { s_row = (Row *)calloc(40, sizeof *s_row); if (s_row) slots_invalidate(); }
        build_rows();                                   // model is rebuilt HERE, not inside the draw
        uint32_t sig = rows_sig();
        if (sig != s_content_sig) { s_content_sig = sig; redraw = true; }   // nothing changed -> no frame
    }
    return redraw;
}

static void info_enter(void)
{
    nucleo_screen_acquire();                            // buffered -> no flicker (canvas may have been freed by a media app)
    if (!s_row) s_row = (Row *)calloc(40, sizeof *s_row);   // ~2.2 KB only while open, zero .bss at boot
    s_scroll = 0; s_target = 0;
    s_buffered = true; slots_invalidate();              // first draw settles the real path (and wipes if direct)
    s_seen_gen = nucleo_app_repaint_gen() - 1;          // != current -> the first draw is a full pass
    build_rows(); s_content_sig = rows_sig();
    nucleo_app_set_poll_handler(info_poll);
    nucleo_app_set_hint(TR5("su/giu scorri   esc indietro", "up/dn scroll   esc back",
                            "arr scroll   esc atras", "haut/bas defiler   esc retour",
                            "hoch/runter   esc zurueck"));
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
    s_vis = ch / rowH; if (s_vis < 1) s_vis = 1; if (s_vis > MAX_SLOT - 1) s_vis = MAX_SLOT - 1;
    if (!s_nrow) build_rows();
    if (s_target > max_top()) s_target = max_top();

    // Font0 is the framework default, but an app/overlay that drew before us could have left another
    // font on this target — and the label/value layout below is measured in Font0 cells. Pin it, so a
    // stale font can never turn this sheet into overlapping text.
    d.setFont(&fonts::Font0);

    const bool buf = nucleo_app_is_buffered();
    unsigned gen = nucleo_app_repaint_gen();
    bool full = (buf != s_buffered) || (gen != s_seen_gen);
    if (full) {                                         // path flipped, or the panel was owned by an overlay
        s_buffered = buf; s_seen_gen = gen;
        slots_invalidate();
        if (!buf) { d.fillRect(0, top, W, ch, BG); s_scroll = (float)s_target; }   // one wipe, then incremental
    }
    if (buf) d.fillRect(0, top, W, ch, BG);             // canvas frame: full repaint, composited off-screen

    // Clip: a partially scrolled row used to bleed its text into the hint bar (which this app never
    // clears), leaving a permanent smear there on the direct path. One clip covers both paths.
    d.setClipRect(0, top, W, ch);

    int start = (int)s_scroll; float frac = s_scroll - (float)start;
    bool touched = false;
    for (int r = 0; r <= s_vis; r++) {
        int idx = start + r;
        int ry  = top + (int)((float)r * rowH - frac * rowH);
        uint32_t sig = (idx >= 0 && idx < s_nrow) ? slot_sig(s_row[idx], ry) : 0;
        if (!buf) {
            if (sig == s_slot[r]) continue;             // this slot already shows exactly this
            s_slot[r] = sig; touched = true;
            d.fillRect(0, ry, W, rowH, BG);             // smallest box that owns the change
        }
        if (!sig) continue;                             // past the last row: cleared, nothing to draw
        Row &R = s_row[idx];
        if (R.kind == RK_HEAD) {
            d.fillRect(6, ry + rowH - 4, W - 14, 2, R.col);                       // section underline
            d.setTextSize(1); d.setTextColor(R.col, BG); d.setCursor(8, ry + 5); d.print(R.label);
            continue;
        }
        // Label as a SMALL muted caption on the left; the value is the hero on the right, in the row's
        // semantic colour, big when it fits beside the caption and small otherwise. The value's start
        // is clamped to always clear the caption, so a long label + long value can never overlap (the
        // old code sized the value down but still right-aligned it, letting it slide under the label).
        const int rightM = 8;
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, ry + 6); d.print(R.label);
        if (!R.val[0]) continue;
        const int labelEnd = 8 + (int)strlen(R.label) * 6;
        const int vw2 = (int)strlen(R.val) * 12, vw1 = (int)strlen(R.val) * 6;
        if (labelEnd + 8 + vw2 <= W - rightM) {                                   // value fits BIG beside the caption
            d.setTextSize(2); d.setTextColor(R.col, BG); d.setCursor(W - rightM - vw2, ry + 2); d.print(R.val);
        } else {                                                                  // value SMALL, but never under the caption
            int vx = W - rightM - vw1; if (vx < labelEnd + 6) vx = labelEnd + 6;
            d.setTextSize(1); d.setTextColor(R.col, BG); d.setCursor(vx, ry + 6); d.print(R.val);
        }
    }

    // Scrollbar. On the direct path the row boxes above wipe its column, so redraw it whenever a row
    // was touched; on an untouched frame it is still on the panel and must NOT be cleared+redrawn.
    if (s_nrow > s_vis && (buf || touched)) {
        int trackH = ch - 4, th = trackH * s_vis / s_nrow;
        int ty = top + 2 + (trackH - th) * start / (max_top() > 0 ? max_top() : 1);
        d.fillRect(W - 3, top + 2, 2, trackH, MUTED);
        d.fillRect(W - 3, ty, 2, th, C_BLUE);
    }
    d.clearClipRect();
}

static void info_exit(void) { free(s_row); s_row = nullptr; s_nrow = 0; }   // back to zero .bss until reopened

extern "C" void nucleo_register_info(void)
{
    static const nucleo_app_def_t app = {
        "info", "Connection", "System", "Wi-Fi, indirizzi web e info dispositivo",
        'i', C_BLUE, info_enter, info_key, nullptr, info_draw, info_exit
    };
    nucleo_app_register(&app);
}
