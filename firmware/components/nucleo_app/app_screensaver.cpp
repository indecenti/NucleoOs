// app_screensaver.cpp — NucleoOS Salvaschermo
//
// Tre effetti animati + modalità schermo-off. Si attiva automaticamente dal
// launcher dopo inattività configurabile (5-600 s). Qualsiasi tasto lo chiude.
//
// ANTI-FLICKER: Tecnica 1 esclusivamente.
//   poll_fn() restituisce true alla frequenza target → il framework chiama
//   on_draw() → l'app disegna il frame completo sul CANVAS (sprite SRAM) →
//   il framework blitta il canvas sul TFT in un'unica transazione DMA.
//   MAI direct_draw=true con fillScreen: quella combo scrive direttamente al
//   TFT in due passate e produce sfarfallio evidente.
//
// Effetti (tutto static .bss, zero heap runtime):
//   MODE_CLOCK — orologio rimbalzante stile DVD (canvas: fillScreen + testo)
//   MODE_STARS — starfield warp 3D   (canvas: fillScreen + drawPixel)
//   MODE_FIRE  — fuoco procedurale   (canvas: fillRect per cella, batch write)

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_timer.h"

#define SETTINGS_PATH "/sd/system/config/screensaver.json"
#define W 240
#define H 135

// ---- Modalità ---------------------------------------------------------------
#define MODE_OFF   0
#define MODE_CLOCK 1
#define MODE_STARS 2
#define MODE_FIRE  3

// ---- Stato condiviso (letto da nucleo_app.cpp) ------------------------------
static int32_t g_threshold_ms = 60000;
static int     g_mode         = MODE_CLOCK;
static bool    g_trigger      = false;

// ---- Stato app --------------------------------------------------------------
static bool    s_running    = false;
static bool    s_auto       = false;
static int     s_tab        = 0;
static int     s_sel        = 0;
static bool    s_dirty      = true;
static int64_t s_frame_us   = 0;    // timestamp ultimo frame (µs)
static int     s_saved_bright = 80;

// ---- Frequenze target (µs/frame) -------------------------------------------
#define CLOCK_US  100000   // 10 fps — orologio secondi, abbastanza fluido
#define STARS_US   33333   // ~30 fps
#define FIRE_US    16666   // ~60 fps — fuoco ultrafluidissimo

// ==================== OROLOGIO RIMBALZANTE ==================================
// Tecnica 1: ogni frame → fillScreen(0) su canvas → disegna testo → blit unico.
// Nessun delta tracking: il canvas è lo stato intermedio, il TFT vede un frame
// completo per ogni blit.

#define CL_TW 144   // "HH:MM:SS" @ size-3: 8 char × 18 px
#define CL_TH  38   // 24 (ora) + 2 + 12 (data)

static int16_t cl_x, cl_y;
static int8_t  cl_vx, cl_vy;
static uint8_t cl_ci;

static const uint16_t CL_COLS[] = {
    0xFFFF, 0xFE8C, 0x8FF3, 0xFBB6, 0xC5F5, 0x07FF, 0xF96B
};
#define NCL 7

static void clock_init(void)
{
    srand((unsigned)((esp_timer_get_time() >> 8) & 0xFFFFFFFF));
    cl_x  = 30 + (int)(rand() % 36u);   // 30..66
    cl_y  = 20 + (int)(rand() % 57u);   // 20..77
    cl_vx = (rand() & 1) ? 2 : -2;
    cl_vy = (rand() & 1) ? 1 : -1;
    cl_ci = (uint8_t)(rand() % NCL);
}

static void clock_frame(void)
{
    // Aggiorna posizione con rimbalzo e cambio colore ai bordi
    cl_x += cl_vx; cl_y += cl_vy;
    if (cl_x <= 0)         { cl_x = 0;        cl_vx =  1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_x + CL_TW >= W) { cl_x = W - CL_TW; cl_vx = -1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_y <= 0)         { cl_y = 0;        cl_vy =  1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_y + CL_TH >= H) { cl_y = H - CL_TH; cl_vy = -1; cl_ci = (cl_ci + 1) % NCL; }

    // Frame completo sul canvas: sfondo nero + orologio
    d.fillScreen(0x0000);

    uint16_t col = CL_COLS[cl_ci];

    // Alone sottile (rettangolo scuro dietro il testo)
    d.fillRoundRect(cl_x - 4, cl_y - 3, CL_TW + 8, CL_TH + 4, 4,
                    (uint16_t)(col >> 3 & 0x1CE3));

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char hms[12];
    snprintf(hms, sizeof(hms), "%02d:%02d:%02d",
             tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0);
    char dat[20];
    if (tm) strftime(dat, sizeof(dat), "%a %d %b", tm);
    else    snprintf(dat, sizeof(dat), "---");

    d.setTextColor(col, 0x0000);
    d.setTextSize(3);
    d.setCursor(cl_x, cl_y);
    d.print(hms);

    int dw = (int)strlen(dat) * 6;
    d.setTextSize(1);
    d.setTextColor(0x8C71, 0x0000);
    d.setCursor(cl_x + (CL_TW - dw) / 2, cl_y + 26);
    d.print(dat);
}

// ==================== STARFIELD WARP 3D =====================================
// Tecnica 1: fillScreen(0) su canvas ogni frame, poi drawPixel delle stelle.
// Il canvas viene blittato una volta: il TFT vede un frame atomico senza strappi.

#define NSTARS 64
#define ZMAX   24

static struct { int16_t ox, oy; uint8_t z; } st[NSTARS];

static void stars_reset(int i)
{
    st[i].z  = ZMAX;
    st[i].ox = (int16_t)((rand() % (W + 40)) - (W / 2 + 20));
    st[i].oy = (int16_t)((rand() % (H + 40)) - (H / 2 + 20));
}

static void stars_init(void)
{
    srand((unsigned)(esp_timer_get_time() & 0xFFFFFFFF));
    for (int i = 0; i < NSTARS; i++) {
        stars_reset(i);
        st[i].z = (uint8_t)(1 + rand() % ZMAX);
    }
}

static void stars_frame(void)
{
    // Frame completo: cielo nero + stelle proiettate
    d.fillScreen(0x0000);

    int cx = W / 2, cy = H / 2;
    for (int i = 0; i < NSTARS; i++) {
        if (--st[i].z == 0) { stars_reset(i); continue; }

        int sx = cx + (int)st[i].ox * 20 / (int)st[i].z;
        int sy = cy + (int)st[i].oy * 20 / (int)st[i].z;
        if (sx < 0 || sx >= W || sy < 0 || sy >= H) { stars_reset(i); continue; }

        int bright = ((ZMAX - (int)st[i].z) * 255) / ZMAX;
        uint16_t col = (st[i].z <= 6)
            ? d.color565(bright, bright, 255)
            : d.color565(bright, bright, bright);

        d.drawPixel(sx, sy, col);
        if (st[i].z <= 4 && sx + 1 < W && sy + 1 < H) {
            d.drawPixel(sx + 1, sy,     col);
            d.drawPixel(sx,     sy + 1, col);
            d.drawPixel(sx + 1, sy + 1, col);
        }
    }
}

// ==================== FUOCO PROCEDURALE (Doom-fire) =========================
// Griglia 60×27 celle da 4×5 px = esattamente 240×135 (copertura totale).
// +1 riga sorgente nascosta sotto lo schermo: fire_buf[FH-1] è sempre 255,
// non viene renderizzata ma alimenta le fiamme che arrivano al bordo inferiore.
//
// Algoritmo classico Doom-fire (Fabien Sanglard):
//   ogni cella guarda la cella sotto, con shift laterale casuale e decadimento
//   0-1: produce lingua di fiamma con deriva naturale senza arte artefattuale.
//
// Palette 4 fasi: nero → rosso scuro → arancione → giallo → bianco alle punte.
// Tecnica 1: tutto sul canvas (d = canvas nell'on_draw), blit unico dal framework.

#define FW   60          // celle larghezza (60*4=240)
#define FHV  27          // celle visibili in altezza (27*5=135)
#define FH   28          // +1 riga sorgente nascosta
#define FCS  4           // px per cella orizzontale
#define FCH  5           // px per cella verticale

static uint8_t  fire_buf[FH][FW];
static uint16_t fire_pal[256];
static bool     fire_pal_ok = false;
static int      fire_breath = 0;     // oscillazione globale 0..8 per respiro fiamme

static void fire_init_pal(void)
{
    if (fire_pal_ok) return;
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        // Palette fuoco MASSIMO: nero → rosso scuro vivace → rosso fuoco → arancione bruciante → giallo+arancione → bianco incandescente
        if      (i < 15)  { r = 0;   g = 0;   b = 0; }                                      // nero profondo
        else if (i < 40)  { r = (i - 15) * 9;  g = 0;  b = 0; }                            // rosso scuro bruciante (0..225)
        else if (i < 80)  { r = 255; g = (i - 40) * 4; b = 0; }                            // rosso-arancione fuoco vivace
        else if (i < 120) { r = 255; g = 160 + (i - 80); b = (i - 80) / 4; }              // arancione intensissimo
        else if (i < 160) { r = 255; g = 200 + (i - 120) / 2; b = 30 + (i - 120) / 2; }   // giallo-arancione bruciante
        else if (i < 200) { r = 255; g = 220 + (i - 160) / 4; b = 60 + (i - 160) / 2; }   // giallo brillante con azzurro
        else              { r = 255; g = 230 + (i - 200) / 5; b = 100 + (i - 200) / 3; }  // bianco incandescente azzurrino
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        fire_pal[i] = d.color565(r, g, b);
    }
    fire_pal_ok = true;
}

static void fire_seed(void)
{
    // Sorgente caotica con breath globale (oscillazione 0..8) + picchi esplosivi
    fire_breath = (fire_breath + 1) % 16;  // 0..15, ciclo per respiro sinusoidale
    int breath_mod = 4 + (fire_breath < 8 ? fire_breath : 16 - fire_breath);  // 4..12

    for (int x = 0; x < FW; x++) {
        int base = 255;
        // Variazione intensità con modulazione breath
        if (rand() % 3 == 0)  base = 200 + (rand() % 55);
        if (rand() % 5 == 0)  base = 180 + (rand() % 75);    // più picchi brucianti
        if (rand() % 10 == 0) base = 100 + (rand() % 155);   // buchi per vortici
        if (rand() % 20 == 0) base = 255;                     // bolidi esplosivi rare

        // Applica breath globale come modulatore
        base = (base * breath_mod) / 8;
        fire_buf[FH - 1][x] = (uint8_t)(base > 255 ? 255 : base);
    }
}

static void fire_step(void)
{
    // Doom-fire MASSIMO: deriva esplosiva, decadimento dinamico, vortici + scintille
    for (int y = 0; y < FH - 1; y++) {
        for (int x = 0; x < FW; x++) {
            int r  = rand() % 16;  // [0..15] ultra-varianza
            int sx = x - ((r >> 2) - 1);  // shift ampio [-1..2]
            if (sx < 0)   sx = 0;
            if (sx >= FW) sx = FW - 1;

            // Decadimento variabile 0-4 per vortici più intensi
            int decay = (r & 7) >> 1;  // [0..3], ma spesso meno
            int v = (int)fire_buf[y + 1][sx] - decay;

            // Effetti boosters:
            if (rand() % 100 < 15) {  // scintille ascendenti rare
                v = (int)fire_buf[y + 1][sx] - (decay >> 1);
            }
            if (rand() % 100 < 8) {   // bolidi esplosivi ultra-rari
                v = (int)fire_buf[y + 1][sx];
            }

            fire_buf[y][x] = (uint8_t)(v < 0 ? 0 : v);
        }
    }
}

static void fire_init(void)
{
    fire_init_pal();
    memset(fire_buf, 0, sizeof(fire_buf));
    fire_breath = 0;  // reset respiro
    fire_seed();
}

static void fire_frame(void)
{
    fire_seed();   // sorgente sempre piena → fiamma continua al bordo inferiore
    fire_step();
    for (int y = 0; y < FHV; y++)
        for (int x = 0; x < FW; x++)
            d.fillRect(x * FCS, y * FCH, FCS, FCH, fire_pal[fire_buf[y][x]]);
}

// ==================== AVVIO / STOP SAVER ====================================
static void saver_start(void)
{
    s_running = true;
    // fullscreen: il canvas copre tutto 240×135, hint bar esclusa
    nucleo_app_set_fullscreen(true);
    // NON usare direct_draw: si disegna sul canvas, il framework blitta una volta
    // sola → zero sfarfallio (Tecnica 1 anti-flicker).
    if (g_mode == MODE_OFF) {
        s_saved_bright = nucleo_app_brightness();
        nucleo_app_set_brightness(0);
    } else if (g_mode == MODE_CLOCK) {
        clock_init();
    } else if (g_mode == MODE_STARS) {
        stars_init();
    } else {
        fire_init();
    }
    s_frame_us = esp_timer_get_time();
    nucleo_app_request_draw();  // forza il primo frame subito
}

static void saver_stop(void)
{
    s_running = false;
    nucleo_app_set_fullscreen(false);
    if (g_mode == MODE_OFF)
        nucleo_app_set_brightness(s_saved_bright);
    s_dirty = true;
}

// ==================== PERSISTENZA ===========================================
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    char buf[64];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int mode = MODE_CLOCK, sec = 60;
    sscanf(buf, "{\"mode\":%d,\"sec\":%d}", &mode, &sec);
    if (mode < 0 || mode > 3) mode = MODE_CLOCK;
    if (sec < 5)   sec = 5;
    if (sec > 600) sec = 600;
    g_mode         = mode;
    g_threshold_ms = (int32_t)sec * 1000;
}

static void save_settings(void)
{
    mkdir("/sd/system", 0775);
    mkdir("/sd/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    fprintf(f, "{\"mode\":%d,\"sec\":%d}\n", g_mode, (int)(g_threshold_ms / 1000));
    fclose(f);
}

// ---- app_ui_list callbacks (tab EFFETTO) ------------------------------------
static const char *mode_label(int i, void *)
{
    static const char *const N[] = { "Spento","Orologio","Stelle","Fuoco","► Prova" };
    return (unsigned)i < 5 ? N[i] : "";
}
static const char *mode_right(int i, void *)
{
    static const char *const D[] = { "schermo spento","rimbalzante","campo stellare","fuoco", nullptr };
    return (unsigned)i < 4 ? D[i] : nullptr;
}
static unsigned short mode_color(int i, void *)
{
    if (i == 4)      return C_YELLOW;
    if (i == g_mode) return C_GREEN;
    return C_PURPLE;
}

// ==================== UI IMPOSTAZIONI =======================================
static void draw_settings(void)
{
    int ch = nucleo_app_content_height();

    if (s_tab == 0) {
        int y0 = app_ui_title("EFFETTO", C_PURPLE, "TAB ►");
        app_ui_list(y0, ch - 24, 5, s_sel, mode_label, mode_right, mode_color, nullptr);
    } else {
        int y0 = app_ui_title("TIMER", C_PURPLE, "◄ TAB");
        int rem = ch - 24;
        d.fillRect(0, y0, W, rem, BG);

        int sec = (int)(g_threshold_ms / 1000);
        char sv[20];
        if (sec < 60)           snprintf(sv, sizeof sv, "%d sec", sec);
        else if (sec == 60)     snprintf(sv, sizeof sv, "1 min");
        else if (sec % 60 == 0) snprintf(sv, sizeof sv, "%d min", sec / 60);
        else                    snprintf(sv, sizeof sv, "%dm %ds", sec / 60, sec % 60);

        const char *sub = "Attivazione automatica dopo";
        d.setTextSize(1); d.setTextColor(C_GREY, BG);
        d.setCursor((W - (int)strlen(sub) * 6) / 2, y0 + 6);
        d.print(sub);

        int vw  = (int)strlen(sv) * 12;
        int vx  = (W - vw) / 2;
        int vy  = y0 + 28;
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
        d.setCursor(vx, vy); d.print(sv);
        d.setTextColor(C_GREY, BG);
        d.setCursor(vx - 14, vy); d.print("<");
        d.setCursor(vx + vw + 2, vy); d.print(">");

        const char *hlp = "</> 5s   UP/DOWN 30s   (5-600 s)";
        d.setTextSize(1); d.setTextColor(C_GREY, BG);
        d.setCursor((W - (int)strlen(hlp) * 6) / 2, vy + 22);
        d.print(hlp);

        const char *sts = g_threshold_ms > 0 ? "Attivo" : "Disabilitato";
        d.setTextColor(g_threshold_ms > 0 ? C_GREEN : C_RED, BG);
        d.setCursor((W - (int)strlen(sts) * 6) / 2, vy + 36);
        d.print(sts);
    }
}

// ==================== CALLBACK APP =========================================
static bool on_back(int key)
{
    if (s_running) {
        saver_stop();
        if (s_auto) { s_auto = false; nucleo_app_exit(); }
        else        { nucleo_app_set_hint("TAB tab  INVIO seleziona  ESC esci"); nucleo_app_request_draw(); }
        return true;
    }
    return false;
}

static void on_tab(void)
{
    if (s_running) {
        // Se preview da selezione (s_auto), TAB non cicla — solo BACK chiude
        if (s_auto) return;
        // Altrimenti cicla effetti (solo da impostazioni manuali)
        saver_stop();
        g_mode = (g_mode + 1) % 4;
        s_sel  = g_mode;
        save_settings();
        saver_start();
        return;
    }
    s_tab ^= 1; s_dirty = true;
    save_settings();
    nucleo_app_request_draw();
}

// poll_fn: chiamata ~50 Hz, restituisce true alla frequenza target dell'effetto.
// Il framework chiama on_draw solo quando true → blit al TFT alla data rate,
// non alla loop rate (anti-flicker, evita frame duplicati).
static bool poll_fn(void)
{
    if (!s_running || g_mode == MODE_OFF) return false;
    int64_t now = esp_timer_get_time();
    int64_t interval = (g_mode == MODE_CLOCK) ? CLOCK_US :
                       (g_mode == MODE_STARS) ? STARS_US : FIRE_US;
    if (now - s_frame_us < interval) return false;
    s_frame_us = now;
    return true;
}

static void on_enter(void)
{
    load_settings();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_poll_handler(poll_fn);

    if (g_trigger) {
        g_trigger = false; s_auto = true;
        saver_start();
    } else {
        s_running = false; s_auto = false;
        s_tab = 0; s_sel = g_mode; s_dirty = true;
        nucleo_app_set_hint("TAB tab  INVIO seleziona  ESC esci");
        nucleo_app_request_draw();
    }
}

static void on_key(int key, char ch)
{
    if (s_running) {
        saver_stop();
        if (s_auto) { s_auto = false; nucleo_app_exit(); }
        else        { nucleo_app_set_hint("TAB tab  INVIO seleziona  ESC esci"); nucleo_app_request_draw(); }
        return;
    }
    bool changed = false;
    if (s_tab == 0) {
        if (key == NK_DOWN && s_sel < 4) { s_sel++; if (s_sel < 4) g_mode = s_sel; changed = true; }
        if (key == NK_UP   && s_sel > 0) { s_sel--; if (s_sel < 4) g_mode = s_sel; changed = true; }
        if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_sel < 4) { g_mode = s_sel; save_settings(); changed = true; }
            else           { s_auto = false; saver_start(); return; }
        }
    } else {
        int sec = (int)(g_threshold_ms / 1000);
        if (key == NK_RIGHT && sec < 600) { sec += 5;  if (sec > 600) sec = 600; changed = true; }
        if (key == NK_LEFT  && sec > 5)   { sec -= 5;  if (sec < 5)   sec = 5;   changed = true; }
        if (key == NK_DOWN  && sec < 600) { sec += 30; if (sec > 600) sec = 600; changed = true; }
        if (key == NK_UP    && sec > 5)   { sec -= 30; if (sec < 5)   sec = 5;   changed = true; }
        if (changed) { g_threshold_ms = (int32_t)sec * 1000; save_settings(); }
    }
    if (changed) { s_dirty = true; nucleo_app_request_draw(); }
}

static void on_tick(void) {}

// on_draw: per il saver disegna il frame completo sul canvas (Tecnica 1).
// Per le impostazioni ridisegna solo quando s_dirty.
static void on_draw(void)
{
    if (s_running) {
        switch (g_mode) {
            case MODE_CLOCK: clock_frame(); break;
            case MODE_STARS: stars_frame(); break;
            case MODE_FIRE:  fire_frame();  break;
            // MODE_OFF: schermo spento, backlight 0, nulla da disegnare
        }
        return;
    }
    if (!s_dirty) return;
    s_dirty = false;
    draw_settings();
}

static void on_exit(void) { if (s_running) saver_stop(); save_settings(); }

// ==================== HOOK DI SISTEMA =======================================
extern "C" bool nucleo_screensaver_should_activate(int64_t idle_ms)
{
    return g_threshold_ms > 0 && idle_ms >= g_threshold_ms;
}
extern "C" void nucleo_screensaver_set_trigger(void) { g_trigger = true; }

// ==================== REGISTRAZIONE =========================================
extern "C" void nucleo_register_screensaver(void)
{
    static const nucleo_app_def_t app = {
        "screensaver", "Salvaschermo", "Tools",
        "Schermo spento o animato dopo inattività",
        'S', C_PURPLE,
        on_enter, on_key, on_tick, on_draw, on_exit
    };
    nucleo_app_register(&app);
}
