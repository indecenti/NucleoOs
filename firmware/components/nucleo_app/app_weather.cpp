// app_weather.cpp — Tools > Meteo: geolocated weather (Open-Meteo, free, no key). Tabs Adesso /
// Previsioni / Impostazioni. Big readable fonts, hand-drawn vector icons, up to 10 favourite cities
// (typed in on-device, geocoded), °C/°F. Location from the public IP (cached) — no GPS on the Cardputer.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nucleo_weather.h"
#include "nucleo_kbd.h"
}
#include "nucleo_exclusive.h"
#include "app_ui.h"             // shared tab strip + selectable row
#include "nucleo_theme.h"       // themed chrome palette (replaces the local classic-theme copy)
#include "nucleo_i18n.h"        // TR(it,en): hints follow the system language
#include "nucleo_touch.h"       // two screens: tap the tabs and the rows on the panel

// Free a big CONTIGUOUS block for the TLS handshake (httpd+L1+voice down, Wi-Fi stays up) around an
// online fetch, then restore. Without this the largest free block is ~7KB and HTTPS always OOMs ->
// "non disponibile". Guarded so we never exit an exclusive we didn't enter.
static bool s_excl_was = false;
static void net_up(void)   { s_excl_was = nucleo_exclusive_active(); nucleo_exclusive_enter(NX_NET_APP, nullptr); }
static void net_down(void) { if (!s_excl_was) nucleo_exclusive_exit(); }

// Chrome follows the active theme; ACCENT is the app's identity accent (its registered launcher
// color); the rest are semantic weather-icon / metric tints (named, never bare hex in draw calls).
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define INK   THEME_INK
#define LINE  THEME_LINE
static const unsigned short ACCENT = 0x5D9F, ACC = 0x5D9F,
                            GRN = 0x8FF3, WARN = 0xFE8C, SUN = 0xFE60, SUNHI = 0xFFF2,
                            CLD = 0xC618, CLDHI = 0xE73C, RAIN = 0x4D9F, SNOW = 0xFFFF;

enum Tab { T_NOW, T_FCAST, T_SET };
static Tab  s_tab = T_NOW;
static nucleo_weather_t s_w;
static bool s_ok = false;
static bool s_units_f = false;
static int  s_set_sel = 0;
static bool s_fav_screen = false;
static int  s_fav_sel = 0;

struct Fav { char place[40]; double lat, lon; };
#define MAX_FAV 10
static Fav s_fav[MAX_FAV];
static int s_fav_n = 0;
#define FAV_PATH "/sd/data/weather_fav.txt"

static int  Tc(double c) { return (int)lroundf(s_units_f ? (float)(c * 9.0 / 5.0 + 32.0) : (float)c); }
static char Tu(void)     { return s_units_f ? 'F' : 'C'; }

// ---- favourites (flat "lat|lon|name" lines on SD) -------------------------------------------------
static void fav_load(void)
{
    s_fav_n = 0; FILE *f = fopen(FAV_PATH, "r"); if (!f) return;
    char line[96];
    while (s_fav_n < MAX_FAV && fgets(line, sizeof line, f)) {
        double la, lo; char nm[40];
        if (sscanf(line, "%lf|%lf|%39[^\n]", &la, &lo, nm) == 3) {
            s_fav[s_fav_n].lat = la; s_fav[s_fav_n].lon = lo; snprintf(s_fav[s_fav_n].place, 40, "%s", nm); s_fav_n++;
        }
    }
    fclose(f);
}
static void fav_save(void)
{
    FILE *f = fopen(FAV_PATH, "w"); if (!f) return;
    for (int i = 0; i < s_fav_n; i++) fprintf(f, "%.4f|%.4f|%s\n", s_fav[i].lat, s_fav[i].lon, s_fav[i].place);
    fclose(f);
}
static void fav_add(const char *place, double la, double lo)
{
    if (s_fav_n >= MAX_FAV) return;
    for (int i = 0; i < s_fav_n; i++) if (!strcasecmp(s_fav[i].place, place)) return;   // dedup
    snprintf(s_fav[s_fav_n].place, 40, "%s", place); s_fav[s_fav_n].lat = la; s_fav[s_fav_n].lon = lo; s_fav_n++;
    fav_save();
}
static void fav_del(int idx)
{
    if (idx < 0 || idx >= s_fav_n) return;
    for (int i = idx; i < s_fav_n - 1; i++) s_fav[i] = s_fav[i + 1];
    s_fav_n--; fav_save();
}

// Where a transient screen (loading, typing a city) draws: the whole external panel below its tab bar when the
// app runs on two screens (d IS the panel then), the app's content area otherwise.
static int area_top(void) { return nucleo_app_two_screens() ? 34 : nucleo_app_content_top(); }
static int area_h(void)   { return nucleo_app_two_screens() ? d.height() - 34 : nucleo_app_content_height(); }
static int area_w(void)   { return nucleo_app_two_screens() ? d.width() : 240; }

// ---- vector weather icons (curated sun + clouds) --------------------------------------------------
static void draw_sun(int cx, int cy, float s, bool rays)
{
    if (rays) for (int i = 0; i < 8; i++) {
        float a = i * 0.7854f, da = 0.13f;
        int x1 = cx + cosf(a) * s * 0.98f, y1 = cy + sinf(a) * s * 0.98f;
        d.fillTriangle(cx + cosf(a - da) * s * 0.6f, cy + sinf(a - da) * s * 0.6f,
                       cx + cosf(a + da) * s * 0.6f, cy + sinf(a + da) * s * 0.6f, x1, y1, SUN);
    }
    d.fillCircle(cx, cy, (int)(s * 0.52f), SUN);
    d.fillCircle(cx - (int)(s * 0.16f), cy - (int)(s * 0.16f), (int)(s * 0.18f), SUNHI);   // highlight
}

static void draw_cloud(int cx, int cy, float s, unsigned short col)
{
    d.fillCircle(cx - (int)(s * 0.55f), cy + (int)(s * 0.06f), (int)(s * 0.42f), col);
    d.fillCircle(cx + (int)(s * 0.55f), cy + (int)(s * 0.06f), (int)(s * 0.48f), col);
    d.fillCircle(cx - (int)(s * 0.05f), cy - (int)(s * 0.36f), (int)(s * 0.52f), col);
    d.fillCircle(cx + (int)(s * 0.32f), cy - (int)(s * 0.14f), (int)(s * 0.40f), col);
    d.fillRoundRect(cx - (int)(s * 0.95f), cy, (int)(s * 1.9f), (int)(s * 0.55f), (int)(s * 0.27f), col);
    d.fillCircle(cx - (int)(s * 0.12f), cy - (int)(s * 0.42f), (int)(s * 0.18f), CLDHI);    // soft top highlight
}

static void rain_streaks(int cx, int cy, float s, int n, unsigned short col)
{
    for (int i = 0; i < n; i++) {
        int x = cx - (int)(s * 0.55f) + i * (int)(s * (1.1f / n));
        d.drawLine(x, cy + (int)(s * 0.35f), x - (int)(s * 0.12f), cy + (int)(s * 0.72f), col);
    }
}

static void draw_wx(int cx, int cy, float s, wx_icon_t ic)
{
    switch (ic) {
        case WX_SUN:    draw_sun(cx, cy, s, true); break;
        case WX_PARTLY: draw_sun(cx - (int)(s * 0.4f), cy - (int)(s * 0.4f), s * 0.55f, true);
                        draw_cloud(cx + (int)(s * 0.18f), cy + (int)(s * 0.18f), s * 0.78f, CLD); break;
        case WX_CLOUD:  draw_cloud(cx, cy, s, CLD); break;
        case WX_FOG:    draw_cloud(cx, cy - (int)(s * 0.2f), s * 0.85f, CLD);
                        for (int i = 0; i < 3; i++) { int yy = cy + (int)(s*0.5f) + i*(int)(s*0.22f); d.drawLine(cx - (int)(s*0.7f), yy, cx + (int)(s*0.7f), yy, MUTED); }
                        break;
        case WX_DRIZZLE:draw_cloud(cx, cy - (int)(s * 0.2f), s * 0.85f, CLD); rain_streaks(cx, cy, s * 0.85f, 3, RAIN); break;
        case WX_RAIN:   draw_cloud(cx, cy - (int)(s * 0.2f), s * 0.85f, CLD); rain_streaks(cx, cy, s * 0.85f, 4, RAIN); break;
        case WX_SNOW:   draw_cloud(cx, cy - (int)(s * 0.2f), s * 0.85f, CLD);
                        for (int i = 0; i < 3; i++) { int xx = cx - (int)(s*0.4f) + i*(int)(s*0.4f), yy = cy + (int)(s*0.55f); d.drawLine(xx-2, yy, xx+2, yy, SNOW); d.drawLine(xx, yy-2, xx, yy+2, SNOW); } break;
        case WX_STORM:  draw_cloud(cx, cy - (int)(s * 0.2f), s * 0.85f, CLD);
                        d.fillTriangle(cx + (int)(s*0.05f), cy + (int)(s*0.26f), cx - (int)(s*0.22f), cy + (int)(s*0.6f), cx + (int)(s*0.02f), cy + (int)(s*0.5f), SUN);
                        d.fillTriangle(cx + (int)(s*0.02f), cy + (int)(s*0.45f), cx + (int)(s*0.24f), cy + (int)(s*0.5f), cx + (int)(s*0.02f), cy + (int)(s*0.85f), SUN); break;
        default:        d.setTextSize(3); d.setTextColor(MUTED, BG); d.setCursor(cx - 9, cy - 11); d.print("?"); break;
    }
}

// ---- fetch ----------------------------------------------------------------------------------------
static void do_refresh(bool relocate)
{
    int top = area_top(), h = area_h();
    d.fillRect(0, top, area_w(), h, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 8); d.print(TR("Meteo", "Weather"));
    d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 44); d.print(TR("Carico...", "Loading..."));

    esp_task_wdt_reset();
    net_up();                                       // free contiguous heap for the TLS handshake(s)
    bool have_loc = (!relocate && s_w.place[0]) || (!relocate && nucleo_weather_cache_load(&s_w));
    if (!have_loc) {
        d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 72); d.print(TR("Cerco posizione...", "Locating..."));
        esp_task_wdt_reset();
        have_loc = nucleo_weather_locate_ip(&s_w);
        if (have_loc) nucleo_weather_cache_save(&s_w);
    }
    esp_task_wdt_reset();
    s_ok = have_loc && nucleo_weather_fetch(&s_w);
    esp_task_wdt_reset();
    net_down();
    nucleo_app_request_draw();
}

// ---- text input modal (type a city) --------------------------------------------------------------
static bool text_input(const char *title, char *buf, int maxlen)
{
    buf[0] = 0; int len = 0; bool dirty = true;
    for (;;) {
        if (dirty) {
            int top = area_top(), h = area_h();
            d.fillRect(0, top, area_w(), h, BG);
            d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 8); d.print(title);
            d.fillRoundRect(8, top + 40, area_w() - 16, 26, 4, LINE);
            d.setTextSize(2); d.setTextColor(FG, LINE); d.setCursor(14, top + 46); d.print(len ? buf : "");
            if (len) { int cw = 12; d.fillRect(14 + len * cw, top + 44, 2, 18, ACC); }   // caret
            d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 80); d.print(TR("digita il nome, ENTER conferma", "type the name, ENTER confirms"));
            d.setTextColor(DIM, BG); d.setCursor(8, top + 94); d.print(TR("`  annulla", "`  cancel"));
            dirty = false;
        }
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_BACK) return false;
        if (k.key == NK_ENTER) return len > 0;
        if (k.key == NK_DEL) { if (len > 0) { buf[--len] = 0; dirty = true; } }
        else if (k.ch >= 32 && k.ch < 127 && len < maxlen - 1) { buf[len++] = k.ch; buf[len] = 0; dirty = true; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void add_city_flow(void)
{
    char name[40];
    if (!text_input(TR("Aggiungi citta", "Add city"), name, sizeof name)) return;
    int top = area_top();
    d.fillRect(0, top, area_w(), area_h(), BG);
    d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 40); d.print(TR("Cerco citta...", "Finding city..."));
    esp_task_wdt_reset();
    nucleo_weather_t tmp; memset(&tmp, 0, sizeof tmp);
    net_up();
    bool found = nucleo_weather_locate_city(name, &tmp);
    net_down();
    if (found) fav_add(tmp.place, tmp.lat, tmp.lon);
    else {
        d.fillRect(0, top, area_w(), area_h(), BG);
        d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 40); d.print(TR("Citta non trovata", "City not found"));
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 66); d.print(name);
        for (int i = 0; i < 80; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(20)); nucleo_key_t k = nucleo_kbd_read(); if (k.key) break; }
    }
}

// ---- lifecycle ------------------------------------------------------------------------------------
static bool on_back(int key)
{
    if (s_fav_screen) { s_fav_screen = false; nucleo_app_set_hint(TR5("1-3 tab  </> cambia  r aggiorna  esc",
                                                    "1-3 tab  </> switch  r refresh  esc", "1-3 tab  </> cambia  r actual  esc", "1-3 tab  </> change  r actual  esc", "1-3 tab  </> aendern  r aktual  esc")); nucleo_app_request_draw(); return true; }
    if (key == NK_LEFT) { s_tab = (Tab)((s_tab + T_SET) % (T_SET + 1)); nucleo_app_request_draw(); return true; }
    return false;
}

static void on_enter(void)
{
    s_tab = T_NOW; s_set_sel = 0; s_fav_screen = false; s_fav_sel = 0;
    fav_load();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("1-3 tab  </> cambia  r aggiorna  esc",
                                                    "1-3 tab  </> switch  r refresh  esc", "1-3 tab  </> cambia  r actual  esc", "1-3 tab  </> change  r actual  esc", "1-3 tab  </> aendern  r aktual  esc"));
    do_refresh(false);
}

static void on_key(int key, char ch)
{
    // A tap on a row of the panel (two screens) arrives as ch 1..15 = that row: select it, then open it.
    if (key == NK_CHAR && ch >= 1 && ch <= 15) {
        if (s_fav_screen) s_fav_sel = ch - 1; else if (s_tab == T_SET) s_set_sel = ch - 1;
        key = NK_ENTER; ch = 0;
    }
    if (s_fav_screen) {
        int total = s_fav_n + 1;                      // +1 = "add"
        if (key == NK_UP)   { s_fav_sel = (s_fav_sel - 1 + total) % total; nucleo_app_request_draw(); }
        if (key == NK_DOWN) { s_fav_sel = (s_fav_sel + 1) % total; nucleo_app_request_draw(); }
        if ((ch == 'd' || key == NK_DEL) && s_fav_sel < s_fav_n) { fav_del(s_fav_sel); if (s_fav_sel >= s_fav_n && s_fav_sel) s_fav_sel--; nucleo_app_request_draw(); }
        if (key == NK_ENTER) {
            if (s_fav_sel == s_fav_n) { add_city_flow(); nucleo_app_request_draw(); }
            else {
                Fav *fv = &s_fav[s_fav_sel];
                snprintf(s_w.place, sizeof s_w.place, "%s", fv->place); s_w.lat = fv->lat; s_w.lon = fv->lon;
                nucleo_weather_cache_save(&s_w);
                s_fav_screen = false; s_tab = T_NOW; do_refresh(false);
            }
        }
        return;
    }
    if (ch >= '1' && ch <= '3') { s_tab = (Tab)(ch - '1'); nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT) { s_tab = (Tab)((s_tab + 1) % (T_SET + 1)); nucleo_app_request_draw(); return; }
    if (ch == 'r' || ch == 'R') { do_refresh(false); return; }
    if (s_tab == T_SET) {
        if (key == NK_UP)   { s_set_sel = (s_set_sel - 1 + 4) % 4; nucleo_app_request_draw(); }
        if (key == NK_DOWN) { s_set_sel = (s_set_sel + 1) % 4; nucleo_app_request_draw(); }
        if (key == NK_ENTER) {
            if (s_set_sel == 0) do_refresh(false);
            else if (s_set_sel == 1) do_refresh(true);
            else if (s_set_sel == 2) { s_fav_screen = true; s_fav_sel = 0; nucleo_app_set_hint(TR5("invio apri   d elimina   esc indietro", "enter open   d delete   esc back", "intro abre   d elimina   esc atras", "entrer ouvre   d supprime   esc ret", "enter oeffne   d loeschen   esc zur")); nucleo_app_request_draw(); }
            else { s_units_f = !s_units_f; nucleo_app_request_draw(); }
        }
    }
}

// ---- drawing --------------------------------------------------------------------------------------
static void draw_now(int top, int h)
{
    char ln[40];
    if (!s_ok) {
        d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 22); d.print(TR("Non disponibile", "Unavailable"));
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 48); d.print(s_w.place[0] ? s_w.place : TR("Serve il Wi-Fi.", "Wi-Fi needed."));
        d.setCursor(8, top + 64); d.print(TR("Premi  r  per riprovare.", "Press  r  to retry."));
        return;
    }
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(6, top + 2); d.print(s_w.place);
    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(192, top + 8); d.print(s_w.updated);

    draw_wx(44, top + 54, 27, weather_wmo_icon(s_w.code));                       // big icon

    d.setTextSize(4); d.setTextColor(FG, BG); d.setCursor(92, top + 26);
    snprintf(ln, sizeof ln, "%d", Tc(s_w.temp)); d.print(ln);
    int tx = 92 + (int)strlen(ln) * 24; d.setTextSize(2); d.setTextColor(SUN, BG);
    d.drawCircle(tx + 5, top + 30, 2, SUN);                                       // degree ring
    d.setCursor(tx + 11, top + 26); { char u[2] = { Tu(), 0 }; d.print(u); }

    d.setTextSize(2); d.setTextColor(SUN, BG); d.setCursor(92, top + 56); d.print(TR(weather_wmo_label(s_w.code), weather_wmo_label_en(s_w.code)));
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(92, top + 76);
    snprintf(ln, sizeof ln, TR("percepiti %d%c", "feels %d%c"), Tc(s_w.feels), Tu()); d.print(ln);

    // metric row — bigger values
    int y = top + 94, pp = s_w.n_days ? s_w.day[0].precip_prob : 0;
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(8, y);   d.print(TR("Umidita", "Humidity"));  d.setCursor(92, y);  d.print(TR("Vento", "Wind"));  d.setCursor(168, y); d.print(TR("Pioggia", "Rain"));
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, y + 11);   snprintf(ln, sizeof ln, "%d%%", s_w.humidity); d.print(ln);
    d.setTextColor(GRN, BG); d.setCursor(92, y + 11);  snprintf(ln, sizeof ln, "%d", (int)lroundf(s_w.wind)); d.print(ln); d.setTextSize(1); d.print(" km/h");
    d.setTextSize(2); d.setTextColor(RAIN, BG); d.setCursor(168, y + 11); snprintf(ln, sizeof ln, "%d%%", pp); d.print(ln);
}

static void draw_fcast(int top, int h)
{
    if (!s_ok || s_w.n_days == 0) { d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 26); d.print(TR("Non disponibile", "Unavailable")); return; }
    int rows = s_w.n_days; if (rows > 6) rows = 6;
    int rowh = h / rows;
    for (int i = 0; i < rows; i++) {
        int y0 = top + i * rowh, cy = y0 + rowh / 2; char ln[12];
        if (i) d.drawLine(6, y0, 234, y0, LINE);                            // subtle separator
        d.setTextSize(2); d.setTextColor(i == 0 ? SUN : FG, BG); d.setCursor(8, cy - 8);
        d.print(i == 0 ? TR("Oggi", "Today") : TR(weather_dow_short(s_w.day[i].dow), weather_dow_short_en(s_w.day[i].dow)));
        draw_wx(86, cy, rowh * 0.27f, weather_wmo_icon(s_w.day[i].code));
        int hi = Tc(s_w.day[i].tmax), lo = Tc(s_w.day[i].tmin);
        unsigned short hc = (hi >= 28) ? WARN : (hi <= 4 ? ACC : FG);       // warm=orange, cold=blue
        d.setTextSize(2); d.setTextColor(hc, BG);   d.setCursor(112, cy - 8); snprintf(ln, sizeof ln, "%2d", hi); d.print(ln);
        d.setTextColor(MUTED, BG); d.setCursor(148, cy - 8); snprintf(ln, sizeof ln, "%2d", lo); d.print(ln);
        int bw = 50, bx = 184; d.drawRoundRect(bx, cy - 5, bw, 10, 2, DIM);
        int fw = (bw - 2) * s_w.day[i].precip_prob / 100; if (fw > 0) d.fillRoundRect(bx + 1, cy - 4, fw, 8, 2, RAIN);
    }
}

static void draw_favs(int top, int h)
{
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 4); d.print(TR("Citta preferite", "Favourite cities"));
    int total = s_fav_n + 1, rowh = 22, maxrows = (h - 30) / rowh;
    int start = 0; if (s_fav_sel >= maxrows) start = s_fav_sel - maxrows + 1;
    int y = top + 30;
    for (int i = start; i < total && i < start + maxrows; i++) {
        bool on = (i == s_fav_sel), add = (i == s_fav_n);
        const char *lab = add ? TR("+ Aggiungi citta", "+ Add city") : s_fav[i].place;
        if (add && !on) {                                   // green "add" affordance when not focused
            d.setTextSize(2); d.setTextColor(GRN, BG); d.setCursor(14, y + (rowh - 16) / 2); d.print(lab);
        } else {
            app_ui_row(y, rowh, lab, on, ACCENT);           // shared selectable row (accent pill + INK)
        }
        y += rowh;
    }
}

static void draw_set(int top, int h)
{
    if (s_fav_screen) { draw_favs(top, h); return; }
    // Labels localized (5 languages) AND kept short enough to fit a 232px row at size 2 — the German
    // "Einheit: Fahrenheit" overflowed the right edge, so the unit word is trimmed per language.
    char favln[40], unit[28];
    snprintf(favln, sizeof favln, TR5("Preferite (%d)", "Favourites (%d)", "Favoritas (%d)",
                                      "Favoris (%d)", "Favoriten (%d)"), s_fav_n);
    snprintf(unit, sizeof unit, TR5("Unita: %s", "Units: %s", "Unidad: %s", "Unite: %s", "Grad: %s"),
             s_units_f ? "Fahrenheit" : "Celsius");
    const char *items[4] = {
        TR5("Aggiorna", "Refresh", "Actualizar", "Actualiser", "Aktualis."),
        TR5("Localizza (IP)", "Locate (IP)", "Localizar (IP)", "Localiser (IP)", "Orten (IP)"),
        favln, unit
    };
    // Fit all four rows ABOVE the coordinates footer: derive the row pitch from the space actually
    // available instead of a fixed 24 px (which ran the last row into the footer line). foot reserves
    // the bottom status line; the pitch is clamped so rows never look stretched or cramped.
    const int foot = 13;
    int step = (h - 4 - foot) / 4;
    if (step > 24) step = 24;
    if (step < 18) step = 18;
    const int rowh = step - 2;
    int y = top + 4;
    for (int i = 0; i < 4; i++) { app_ui_row(y, rowh, items[i], i == s_set_sel, ACCENT); y += step; }
    char ln[48]; d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, top + h - 10);
    snprintf(ln, sizeof ln, "%s  %.2f,%.2f", s_w.place[0] ? s_w.place : "-", s_w.lat, s_w.lon); d.print(ln);
}

static void draw_summary(void);   // two screens: the Cardputer's glanceable summary (defined below)
static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_summary(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_fav_screen) { draw_favs(top, h); return; }
    const char *TABS[] = { TR5("Adesso", "Now", "Ahora", "Maint.", "Jetzt"),
                           TR5("Previsioni", "Forecast", "Prevision", "Previsions", "Vorhersage"),
                           TR5("Imp", "Set", "Ajus", "Regl", "Einst") };
    int ctop = app_ui_tabs(top, TABS, T_SET + 1, s_tab, ACCENT), ch = h - (ctop - top);
    if (s_tab == T_NOW) draw_now(ctop, ch);
    else if (s_tab == T_FCAST) draw_fcast(ctop, ch);
    else draw_set(ctop, ch);
}

// ---- two screens: the whole app on the external 320x240 panel -----------------------------------------
// Everything the tabs hold, at a size read from across the desk: a tab bar you can tap, Now (place, big
// icon + temperature, condition, feels-like, three metrics), Forecast (a full-width row per day), Settings and
// Favourite cities (big rows, tap = open). The Cardputer shows a glanceable summary meanwhile (on_draw).
// No buffer: each region is painted over itself, and only what changed is repainted — moving through a menu
// repaints the rows, nothing else; a key that changes nothing on the panel costs nothing.
#define LG_TOP 34
static const char *tab_name(int t)
{
    return t == T_NOW ? TR5("Adesso", "Now", "Ahora", "Maint.", "Jetzt")
         : t == T_FCAST ? TR5("Previsioni", "Forecast", "Prevision", "Previsions", "Vorhersage")
         : TR5("Impostazioni", "Settings", "Ajustes", "Reglages", "Einstell.");
}

static void lg_tabs(int W2)
{
    d.fillRect(0, 0, W2, LG_TOP, BG);
    const int tw = W2 / 3;
    d.setFont(&fonts::Font2);
    for (int t = 0; t <= T_SET; t++) {
        bool on = (t == s_tab && !s_fav_screen);
        int x = t * tw + 3, w = tw - 6;
        if (on) d.fillRoundRect(x, 4, w, 26, 8, ACCENT); else d.drawRoundRect(x, 4, w, 26, 8, LINE);
        const char *n = tab_name(t);
        d.setTextColor(on ? INK : MUTED, on ? ACCENT : BG);
        d.setCursor(x + (w - (int)d.textWidth(n)) / 2, 9); d.print(n);
        nucleo_touch_add(x, 0, w, LG_TOP, NK_CHAR, (char)('1' + t));
    }
}

// One big selectable row over its own band: accent pill when focused, a quiet outline otherwise.
static void lg_row(int W2, int y, int h, const char *label, const char *val, bool on, unsigned short tint)
{
    d.fillRect(0, y, W2, h, BG);
    if (on) d.fillRoundRect(6, y + 2, W2 - 12, h - 4, 10, ACCENT);
    else    d.drawRoundRect(6, y + 2, W2 - 12, h - 4, 10, LINE);
    d.setFont(&fonts::Font4);
    d.setTextColor(on ? INK : tint, on ? ACCENT : BG);
    d.setCursor(18, y + (h - 26) / 2 + 1); d.print(label);
    if (val && val[0]) {
        d.setFont(&fonts::Font2); d.setTextColor(on ? INK : MUTED, on ? ACCENT : BG);
        d.setCursor(W2 - 18 - (int)d.textWidth(val), y + (h - 16) / 2); d.print(val);
    }
}

static void lg_now(int W2, int H2)
{
    char ln[40];
    if (!s_ok) {
        d.setFont(&fonts::Font4); d.setTextColor(WARN, BG);
        const char *m = TR("Non disponibile", "Unavailable");
        d.setCursor((W2 - (int)d.textWidth(m)) / 2, 100); d.print(m);
        d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG);
        m = TR("Serve il Wi-Fi: r per riprovare", "Wi-Fi needed: r to retry");
        d.setCursor((W2 - (int)d.textWidth(m)) / 2, 134); d.print(m);
        return;
    }
    d.setFont(&fonts::Font4); d.setTextColor(FG, BG); d.setCursor(10, LG_TOP + 4); d.print(s_w.place);
    if (s_w.updated[0]) { d.setFont(&fonts::Font2); d.setTextColor(DIM, BG); d.setCursor(W2 - 10 - (int)d.textWidth(s_w.updated), LG_TOP + 8); d.print(s_w.updated); }
    draw_wx(58, 112, 40, weather_wmo_icon(s_w.code));
    snprintf(ln, sizeof ln, "%d", Tc(s_w.temp));
    d.setFont(&fonts::Font7); d.setTextColor(FG, BG); d.setCursor(118, 70); d.print(ln);
    int tx = 118 + (int)d.textWidth(ln) + 5;
    d.drawCircle(tx + 5, 77, 5, SUN); d.drawCircle(tx + 5, 77, 4, SUN);
    d.setFont(&fonts::Font4); d.setTextColor(SUN, BG); d.setCursor(tx + 13, 70); { char u[2] = { Tu(), 0 }; d.print(u); }
    char cond[40]; snprintf(cond, sizeof cond, "%s", TR(weather_wmo_label(s_w.code), weather_wmo_label_en(s_w.code)));
    d.setFont(&fonts::Font4);
    while (cond[0] && (int)d.textWidth(cond) > W2 - 126) cond[strlen(cond) - 1] = 0;
    d.setTextColor(SUN, BG); d.setCursor(118, 124); d.print(cond);
    d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG); d.setCursor(118, 154);
    snprintf(ln, sizeof ln, TR("percepiti %d%c", "feels like %d%c"), Tc(s_w.feels), Tu()); d.print(ln);
    // The three metrics across the bottom.
    int pp = s_w.n_days ? s_w.day[0].precip_prob : 0, cw = W2 / 3;
    d.drawFastHLine(8, 178, W2 - 16, LINE);
    const char *lab[3] = { TR("Umidita", "Humidity"), TR("Vento km/h", "Wind km/h"), TR("Pioggia", "Rain") };
    char val[3][12]; unsigned short col[3] = { ACC, GRN, RAIN };
    snprintf(val[0], 12, "%d%%", s_w.humidity); snprintf(val[1], 12, "%d", (int)lroundf(s_w.wind)); snprintf(val[2], 12, "%d%%", pp);
    for (int i = 0; i < 3; i++) {
        int cx = i * cw + cw / 2;
        d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG); d.setCursor(cx - (int)d.textWidth(lab[i]) / 2, 184); d.print(lab[i]);
        d.setFont(&fonts::Font4); d.setTextColor(col[i], BG); d.setCursor(cx - (int)d.textWidth(val[i]) / 2, 204); d.print(val[i]);
    }
}

static void lg_fcast(int W2, int H2)
{
    if (!s_ok || s_w.n_days == 0) { d.setFont(&fonts::Font4); d.setTextColor(WARN, BG); d.setCursor(16, 100); d.print(TR("Non disponibile", "Unavailable")); return; }
    int n = s_w.n_days; if (n > WX_MAX_DAYS) n = WX_MAX_DAYS;
    int rh = (H2 - LG_TOP) / n;
    char ln[12];
    for (int i = 0; i < n; i++) {
        int y = LG_TOP + i * rh, cy = y + rh / 2;
        if (i) d.drawFastHLine(8, y, W2 - 16, LINE);
        d.setFont(&fonts::Font4); d.setTextColor(i == 0 ? SUN : FG, BG); d.setCursor(10, cy - 12);
        d.print(i == 0 ? TR("Oggi", "Today") : TR(weather_dow_short(s_w.day[i].dow), weather_dow_short_en(s_w.day[i].dow)));
        draw_wx(118, cy, rh * 0.30f, weather_wmo_icon(s_w.day[i].code));
        int hi = Tc(s_w.day[i].tmax), lo = Tc(s_w.day[i].tmin);
        snprintf(ln, sizeof ln, "%d", hi);
        d.setTextColor(hi >= 28 ? WARN : (hi <= 4 ? ACC : FG), BG); d.setCursor(152, cy - 12); d.print(ln);
        snprintf(ln, sizeof ln, "%d", lo);
        d.setTextColor(MUTED, BG); d.setCursor(192, cy - 12); d.print(ln);
        int bx = 232, bw = 46, pp = s_w.day[i].precip_prob;
        d.drawRoundRect(bx, cy - 6, bw, 12, 3, DIM);
        int fw = (bw - 2) * pp / 100; if (fw > 0) d.fillRoundRect(bx + 1, cy - 5, fw, 10, 3, RAIN);
        snprintf(ln, sizeof ln, "%d%%", pp);
        d.setFont(&fonts::Font2); d.setTextColor(RAIN, BG); d.setCursor(W2 - 6 - (int)d.textWidth(ln), cy - 8); d.print(ln);
    }
}

static void lg_set_rows(int W2, bool register_targets)
{
    char favv[8], unit[12];
    snprintf(favv, sizeof favv, "%d", s_fav_n);
    snprintf(unit, sizeof unit, "%s", s_units_f ? "Fahrenheit" : "Celsius");
    const char *lab[4] = { TR5("Aggiorna", "Refresh", "Actualizar", "Actualiser", "Aktualisieren"),
                           TR5("Localizza (IP)", "Locate (IP)", "Localizar (IP)", "Localiser (IP)", "Orten (IP)"),
                           TR5("Citta preferite", "Favourite cities", "Ciudades favoritas", "Villes favorites", "Lieblingsorte"),
                           TR5("Unita", "Units", "Unidad", "Unite", "Einheit") };
    const char *val[4] = { s_w.updated, "", favv, unit };
    for (int i = 0; i < 4; i++) {
        int y = LG_TOP + 4 + i * 44;
        lg_row(W2, y, 42, lab[i], val[i], i == s_set_sel, FG);
        if (register_targets) nucleo_touch_add(0, y, W2, 42, NK_CHAR, (char)(1 + i));
    }
}

static void lg_fav_rows(int W2, int H2, bool register_targets)
{
    const int y0 = LG_TOP + 30, rh = 34, vis = (H2 - y0) / rh, total = s_fav_n + 1;
    int start = s_fav_sel >= vis ? s_fav_sel - vis + 1 : 0;
    for (int r = 0; r < vis; r++) {
        int i = start + r, y = y0 + r * rh;
        if (i >= total) { d.fillRect(0, y, W2, rh, BG); continue; }
        bool add = (i == s_fav_n);
        lg_row(W2, y, rh, add ? TR("+ Aggiungi citta", "+ Add city") : s_fav[i].place, "", i == s_fav_sel, add ? GRN : FG);
        if (register_targets) nucleo_touch_add(0, y, W2, rh, NK_CHAR, (char)(1 + i));
    }
}

static void draw_large(void)
{
    const int W2 = d.width(), H2 = d.height();
    // What the panel shows, in two parts: the screen (tab, data, the lists' size) and the focus inside it.
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(nucleo_app_repaint_gen()); mix((uint32_t)s_tab); mix(s_fav_screen); mix(s_ok); mix(s_units_f); mix((uint32_t)s_fav_n);
    mix((uint32_t)lroundf((float)s_w.temp * 10)); mix((uint32_t)s_w.code); mix((uint32_t)s_w.humidity); mix((uint32_t)s_w.n_days);
    for (const char *p = s_w.place; *p; p++) mix((uint8_t)*p);
    for (const char *p = s_w.updated; *p; p++) mix((uint8_t)*p);
    static uint32_t s_sig = 0;
    static int s_sel_shown = -1, s_fsel_shown = -1;
    const bool full = (sig != s_sig);
    if (!full) {                                     // same screen: only the focus may have moved
        if (s_fav_screen && s_fav_sel != s_fsel_shown) { s_fsel_shown = s_fav_sel; lg_fav_rows(W2, H2, false); }
        else if (!s_fav_screen && s_tab == T_SET && s_set_sel != s_sel_shown) { s_sel_shown = s_set_sel; lg_set_rows(W2, false); }
        return;
    }
    s_sig = sig; s_sel_shown = s_set_sel; s_fsel_shown = s_fav_sel;
    nucleo_touch_clear();
    lg_tabs(W2);
    d.fillRect(0, LG_TOP, W2, H2 - LG_TOP, BG);
    if (s_fav_screen) {
        d.setFont(&fonts::Font4); d.setTextColor(ACC, BG); d.setCursor(10, LG_TOP + 2); d.print(TR("Citta preferite", "Favourite cities"));
        lg_fav_rows(W2, H2, true);
    }
    else if (s_tab == T_NOW) lg_now(W2, H2);
    else if (s_tab == T_FCAST) lg_fcast(W2, H2);
    else {
        lg_set_rows(W2, true);
        char ln[48]; d.setFont(&fonts::Font2); d.setTextColor(DIM, BG);
        snprintf(ln, sizeof ln, "%s  %.2f, %.2f", s_w.place[0] ? s_w.place : "-", s_w.lat, s_w.lon);
        d.setCursor(10, H2 - 20); d.print(ln);
    }
    nucleo_touch_commit();
    d.setFont(&fonts::Font0); d.setTextSize(1);
}

// The Cardputer's own screen while the panel has the app: a glanceable summary (place, temperature, sky).
static void draw_summary(void)
{
    char ln[24];
    d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG); d.setCursor(6, 4);
    d.print(s_w.place[0] ? s_w.place : TR("Meteo", "Weather"));
    if (!s_ok) { d.setFont(&fonts::Font4); d.setTextColor(WARN, BG); d.setCursor(6, 50); d.print(TR("Non disponibile", "Unavailable")); d.setFont(&fonts::Font0); return; }
    snprintf(ln, sizeof ln, "%d", Tc(s_w.temp));
    d.setFont(&fonts::Font7); d.setTextColor(FG, BG); d.setCursor(8, 30); d.print(ln);
    int tx = 8 + (int)d.textWidth(ln) + 4;
    d.setFont(&fonts::Font4); d.setTextColor(SUN, BG); d.setCursor(tx, 30); { char u[2] = { Tu(), 0 }; d.print(u); }
    draw_wx(188, 58, 28, weather_wmo_icon(s_w.code));
    char cond[32]; snprintf(cond, sizeof cond, "%s", TR(weather_wmo_label(s_w.code), weather_wmo_label_en(s_w.code)));
    d.setFont(&fonts::Font2); d.setTextColor(SUN, BG); d.setCursor(8, 92); d.print(cond);
    d.setFont(&fonts::Font0); d.setTextSize(1);
}

extern "C" void nucleo_register_weather(void)
{
    static const nucleo_app_def_t app = {
        "weather", "Meteo", "Tools", "Meteo geolocalizzato, citta preferite, previsioni (Open-Meteo)",
        'M', 0x5D9F, on_enter, on_key, nullptr, on_draw, nullptr, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
