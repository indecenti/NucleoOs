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

// Free a big CONTIGUOUS block for the TLS handshake (httpd+L1+voice down, Wi-Fi stays up) around an
// online fetch, then restore. Without this the largest free block is ~7KB and HTTPS always OOMs ->
// "non disponibile". Guarded so we never exit an exclusive we didn't enter.
static bool s_excl_was = false;
static void net_up(void)   { s_excl_was = nucleo_exclusive_active(); nucleo_exclusive_enter(NX_NET_APP, nullptr); }
static void net_down(void) { if (!s_excl_was) nucleo_exclusive_exit(); }

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x9CD3, DIM = 0x6B4D, ACC = 0x4DDF,
                            GRN = 0x8FF3, WARN = 0xFE8C, HL = 0x12B2, SUN = 0xFE60, SUNHI = 0xFFF2,
                            CLD = 0xC618, CLDHI = 0xE73C, RAIN = 0x4D9F, SNOW = 0xFFFF, INK = 0x0000;

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
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 8); d.print("Meteo");
    d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 44); d.print("Carico...");

    esp_task_wdt_reset();
    net_up();                                       // free contiguous heap for the TLS handshake(s)
    bool have_loc = (!relocate && s_w.place[0]) || (!relocate && nucleo_weather_cache_load(&s_w));
    if (!have_loc) {
        d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 72); d.print("Cerco posizione...");
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
            int top = nucleo_app_content_top(), h = nucleo_app_content_height();
            d.fillRect(0, top, 240, h, BG);
            d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 8); d.print(title);
            d.fillRoundRect(8, top + 40, 224, 26, 4, 0x10A2);
            d.setTextSize(2); d.setTextColor(FG, 0x10A2); d.setCursor(14, top + 46); d.print(len ? buf : "");
            if (len) { int cw = 12; d.fillRect(14 + len * cw, top + 44, 2, 18, ACC); }   // caret
            d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 80); d.print("digita il nome, ENTER conferma");
            d.setTextColor(DIM, BG); d.setCursor(8, top + 94); d.print("`  annulla");
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
    if (!text_input("Aggiungi citta", name, sizeof name)) return;
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), BG);
    d.setTextSize(2); d.setTextColor(MUTED, BG); d.setCursor(8, top + 40); d.print("Cerco citta...");
    esp_task_wdt_reset();
    nucleo_weather_t tmp; memset(&tmp, 0, sizeof tmp);
    net_up();
    bool found = nucleo_weather_locate_city(name, &tmp);
    net_down();
    if (found) fav_add(tmp.place, tmp.lat, tmp.lon);
    else {
        d.fillRect(0, top, 240, nucleo_app_content_height(), BG);
        d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 40); d.print("Citta non trovata");
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 66); d.print(name);
        for (int i = 0; i < 80; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(20)); nucleo_key_t k = nucleo_kbd_read(); if (k.key) break; }
    }
}

// ---- lifecycle ------------------------------------------------------------------------------------
static bool on_back(int key)
{
    if (s_fav_screen) { s_fav_screen = false; nucleo_app_set_hint("1-3 tab  </>  r aggiorna  ` esci"); nucleo_app_request_draw(); return true; }
    if (key == NK_LEFT) { s_tab = (Tab)((s_tab + T_SET) % (T_SET + 1)); nucleo_app_request_draw(); return true; }
    return false;
}

static void on_enter(void)
{
    s_tab = T_NOW; s_set_sel = 0; s_fav_screen = false; s_fav_sel = 0;
    fav_load();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint("1-3 tab  </>  r aggiorna  ` esci");
    do_refresh(false);
}

static void on_key(int key, char ch)
{
    if (s_fav_screen) {
        int total = s_fav_n + 1;                      // +1 = "aggiungi"
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
            else if (s_set_sel == 2) { s_fav_screen = true; s_fav_sel = 0; nucleo_app_set_hint("enter apri/aggiungi  d elimina  ` indietro"); nucleo_app_request_draw(); }
            else { s_units_f = !s_units_f; nucleo_app_request_draw(); }
        }
    }
}

// ---- drawing --------------------------------------------------------------------------------------
static void tabbar(int top)
{
    static const char *N[] = { "Adesso", "Previsioni", "Imp" };
    d.setTextSize(1);                                    // MUST set: otherwise a leaked size 4 (temp) blows up the tabs
    int x = 6;
    for (int i = 0; i <= T_SET; i++) {
        bool on = (i == s_tab); int w = (int)strlen(N[i]) * 6 + 10;
        d.fillRoundRect(x, top + 2, w, 14, 3, on ? ACC : 0x10A2);
        d.setTextColor(on ? INK : MUTED, on ? ACC : 0x10A2); d.setCursor(x + 5, top + 5); d.print(N[i]);
        x += w + 4;
    }
}

static void draw_now(int top, int h)
{
    char ln[40];
    if (!s_ok) {
        d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 22); d.print("Non disponibile");
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, top + 48); d.print(s_w.place[0] ? s_w.place : "Serve il Wi-Fi.");
        d.setCursor(8, top + 64); d.print("Premi  r  per riprovare.");
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

    d.setTextSize(2); d.setTextColor(SUN, BG); d.setCursor(92, top + 56); d.print(weather_wmo_label(s_w.code));
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(92, top + 76);
    snprintf(ln, sizeof ln, "percepiti %d%c", Tc(s_w.feels), Tu()); d.print(ln);

    // metric row — bigger values
    int y = top + 94, pp = s_w.n_days ? s_w.day[0].precip_prob : 0;
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(8, y);   d.print("Umidita");  d.setCursor(92, y);  d.print("Vento");  d.setCursor(168, y); d.print("Pioggia");
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, y + 11);   snprintf(ln, sizeof ln, "%d%%", s_w.humidity); d.print(ln);
    d.setTextColor(GRN, BG); d.setCursor(92, y + 11);  snprintf(ln, sizeof ln, "%d", (int)lroundf(s_w.wind)); d.print(ln); d.setTextSize(1); d.print(" km/h");
    d.setTextSize(2); d.setTextColor(RAIN, BG); d.setCursor(168, y + 11); snprintf(ln, sizeof ln, "%d%%", pp); d.print(ln);
}

static void draw_fcast(int top, int h)
{
    if (!s_ok || s_w.n_days == 0) { d.setTextSize(2); d.setTextColor(WARN, BG); d.setCursor(8, top + 26); d.print("Non disponibile"); return; }
    int rows = s_w.n_days; if (rows > 6) rows = 6;
    int rowh = h / rows;
    for (int i = 0; i < rows; i++) {
        int y0 = top + i * rowh, cy = y0 + rowh / 2; char ln[12];
        if (i) d.drawLine(6, y0, 234, y0, 0x18C3);                          // subtle separator
        d.setTextSize(2); d.setTextColor(i == 0 ? SUN : FG, BG); d.setCursor(8, cy - 8);
        d.print(i == 0 ? "Oggi" : weather_dow_short(s_w.day[i].dow));
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
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 4); d.print("Citta preferite");
    int total = s_fav_n + 1, rowh = 22, maxrows = (h - 30) / rowh;
    int start = 0; if (s_fav_sel >= maxrows) start = s_fav_sel - maxrows + 1;
    int y = top + 30;
    for (int i = start; i < total && i < start + maxrows; i++) {
        bool on = (i == s_fav_sel), add = (i == s_fav_n);
        if (on) d.fillRoundRect(4, y, 232, rowh - 2, 4, HL);
        d.setTextSize(2); d.setTextColor(add ? GRN : (on ? FG : MUTED), on ? HL : BG); d.setCursor(14, y + 4);
        d.print(add ? "+ Aggiungi citta" : s_fav[i].place);
        y += rowh;
    }
}

static void draw_set(int top, int h)
{
    if (s_fav_screen) { draw_favs(top, h); return; }
    char favln[40], unit[24];
    snprintf(favln, sizeof favln, "Citta preferite (%d)", s_fav_n);
    snprintf(unit, sizeof unit, "Unita: %s", s_units_f ? "Fahrenheit" : "Celsius");
    const char *items[4] = { "Aggiorna meteo", "Localizza (IP)", favln, unit };
    int y = top + 4;
    for (int i = 0; i < 4; i++) {
        bool on = (i == s_set_sel);
        if (on) d.fillRoundRect(4, y, 232, 22, 4, HL);
        d.setTextSize(2); d.setTextColor(on ? FG : MUTED, on ? HL : BG); d.setCursor(14, y + 4); d.print(items[i]);
        y += 24;
    }
    char ln[48]; d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, top + h - 11);
    snprintf(ln, sizeof ln, "%s  %.2f,%.2f", s_w.place[0] ? s_w.place : "-", s_w.lat, s_w.lon); d.print(ln);
}

static void on_draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_fav_screen) { draw_favs(top, h); return; }
    tabbar(top);
    int ctop = top + 20, ch = h - 20;
    if (s_tab == T_NOW) draw_now(ctop, ch);
    else if (s_tab == T_FCAST) draw_fcast(ctop, ch);
    else draw_set(ctop, ch);
}

extern "C" void nucleo_register_weather(void)
{
    static const nucleo_app_def_t app = {
        "weather", "Meteo", "Tools", "Meteo geolocalizzato, citta preferite, previsioni (Open-Meteo)",
        'M', 0x5D9F, on_enter, on_key, nullptr, on_draw, nullptr, 0
    };
    nucleo_app_register(&app);
}
