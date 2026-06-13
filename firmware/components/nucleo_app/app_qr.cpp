// QR Code ("Codice QR") — generate a big, high-contrast, easily-scannable QR on the Cardputer.
//
// Inspired by Bruce's QR menu, evolved for real use:
//  - encoder is M5GFX's built-in MIT QR (Richard Moore) — already linked, zero vendoring;
//  - the QR is auto-sized to the LARGEST square the panel allows (full content height), drawn on a
//    full-white panel with a guaranteed quiet zone (margin=true) so a phone locks on instantly;
//  - backlight is pushed to 100% while shown — a bright screen is far easier for a camera to read;
//  - four smart sources instead of plain text: PAIR this device (open the web OS), share WI-FI,
//    free TEXT/URL typed live, and SAVED — the cross-surface library written by the web "Codice QR"
//    app (/data/QR/qrcodes.json): one engine, two bodies. The footer says what you're scanning.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "launcher_theme.h"
#include "nucleo_board.h"   // NUCLEO_SD_MOUNT
#include "cJSON.h"          // read the web-created QR library off the SD

// Network/auth getters (resolved at final link; same extern hatch nucleo_app.cpp uses).
extern "C" const char *nucleo_setup_ip(void);
extern "C" const char *nucleo_setup_mode(void);
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_auth_pin(void);

enum { SRC_PAIR = 0, SRC_WIFI, SRC_TEXT, SRC_SAVED, N_SRC };

static int  s_src = SRC_PAIR;
static char s_text[160];          // free text (SRC_TEXT) / typed password (SRC_WIFI)
static int  s_len = 0;
static char s_payload[256];       // what actually gets encoded
static int  s_prev_bright = 100;

// SAVED: the library the web app writes to SD (read-only here; we re-encode each string on-device).
#define QR_SAVED_MAX  24
#define QR_SAVED_PATH NUCLEO_SD_MOUNT "/data/QR/qrcodes.json"
static char s_sv_label[QR_SAVED_MAX][40];
static char s_sv_data[QR_SAVED_MAX][256];
static int  s_sv_n = 0, s_sv_sel = 0;

static bool editing(void) { return s_src == SRC_TEXT || s_src == SRC_WIFI; }

static void load_saved(void)
{
    s_sv_n = 0; s_sv_sel = 0;
    FILE *f = fopen(QR_SAVED_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16384) { fclose(f); return; }
    char *buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, n, f); buf[got] = 0; fclose(f);
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) return;
    cJSON *items = cJSON_GetObjectItem(root, "items"), *it;
    cJSON_ArrayForEach(it, items) {
        if (s_sv_n >= QR_SAVED_MAX) break;
        cJSON *data = cJSON_GetObjectItem(it, "data"), *label = cJSON_GetObjectItem(it, "label");
        if (!cJSON_IsString(data) || !data->valuestring[0]) continue;
        snprintf(s_sv_data[s_sv_n], sizeof s_sv_data[0], "%s", data->valuestring);
        snprintf(s_sv_label[s_sv_n], sizeof s_sv_label[0], "%s",
                 (cJSON_IsString(label) && label->valuestring[0]) ? label->valuestring : data->valuestring);
        s_sv_n++;
    }
    cJSON_Delete(root);
}

// Build the encoded string + the footer caption for the current source.
static void rebuild(void)
{
    char cap[128];
    const char *ip   = nucleo_setup_ip();
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && ip[0];

    if (s_src == SRC_PAIR) {
        snprintf(s_payload, sizeof s_payload, "http://%s", sta ? ip : "192.168.4.1");
        snprintf(cap, sizeof cap, "Apri %s  PIN %s", sta ? ip : "192.168.4.1", nucleo_auth_pin());
    } else if (s_src == SRC_WIFI) {
        const char *ssid = nucleo_setup_ssid();
        snprintf(s_payload, sizeof s_payload, "WIFI:T:WPA;S:%s;P:%s;;", ssid[0] ? ssid : "?", s_text);
        snprintf(cap, sizeof cap, "Wi-Fi %s  pass: %s_", ssid[0] ? ssid : "?", s_text);
    } else if (s_src == SRC_SAVED) {
        if (s_sv_n == 0) { s_payload[0] = 0; snprintf(cap, sizeof cap, "Salvati: nessun QR dal web OS"); }
        else {
            if (s_sv_sel >= s_sv_n) s_sv_sel = 0;
            snprintf(s_payload, sizeof s_payload, "%s", s_sv_data[s_sv_sel]);
            snprintf(cap, sizeof cap, "Salvati %d/%d: %s", s_sv_sel + 1, s_sv_n, s_sv_label[s_sv_sel]);
        }
    } else {
        snprintf(s_payload, sizeof s_payload, "%s", s_text);
        snprintf(cap, sizeof cap, "Testo: %s_", s_text);
    }
    nucleo_app_set_hint(cap);
    nucleo_app_request_draw();
}

static void draw(void)
{
    int ch = nucleo_app_content_height();   // 121 — the QR square's height budget
    d.fillRect(0, 0, W, ch, 0xFFFF);        // full-white panel (extends the quiet zone)

    if (!s_payload[0]) {
        const char *m1 = (s_src == SRC_SAVED) ? "Nessun QR salvato" : "Scrivi un testo";
        const char *m2 = (s_src == SRC_SAVED) ? "creane dal web OS" : "o un URL, poi inquadra";
        d.setTextColor(0x0000, 0xFFFF); d.setTextSize(2);
        d.setCursor(28, 36); d.print(m1);
        d.setTextSize(1); d.setTextColor(0x8410, 0xFFFF);
        d.setCursor(28, 60); d.print(m2);
        return;
    }

    // Largest square the panel allows; margin=true guarantees the >=4-module quiet zone so the
    // code stays lockable even though the screen edges are tight. version=1 auto-grows to fit.
    int w = ch; if (w > W) w = W;
    d.qrcode(s_payload, (W - w) / 2, 0, w, 1, true);
}

static void set_src(int s)
{
    s_src = (s + N_SRC) % N_SRC;
    if (s_src == SRC_SAVED) load_saved();   // refresh from SD each time we land here
    rebuild();
}

static void on_key(int key, char ch)
{
    if (key == NK_RIGHT || key == NK_TAB) { set_src(s_src + 1); return; }
    if (s_src == SRC_SAVED) {
        if (key == NK_UP && s_sv_n)        { s_sv_sel = (s_sv_sel + s_sv_n - 1) % s_sv_n; rebuild(); }
        else if (key == NK_DOWN && s_sv_n) { s_sv_sel = (s_sv_sel + 1) % s_sv_n; rebuild(); }
        return;
    }
    if (editing()) {
        if (key == NK_DEL) { if (s_len > 0) s_text[--s_len] = 0; rebuild(); return; }
        if (ch >= ' ' && ch < 127 && s_len < (int)sizeof(s_text) - 1) { s_text[s_len++] = ch; s_text[s_len] = 0; rebuild(); return; }
    }
}

static bool on_back(int key)
{
    if (key == NK_LEFT) { set_src(s_src - 1); return true; }
    return false;   // Esc -> close
}

static void enter(void)
{
    s_src = SRC_PAIR; s_len = 0; s_text[0] = 0;
    s_prev_bright = nucleo_app_brightness();
    nucleo_app_set_brightness(100);             // bright screen = easy camera lock
    nucleo_app_set_back_handler(on_back);
    rebuild();
}

static void on_exit(void)
{
    nucleo_app_set_brightness(s_prev_bright);
}

extern "C" void nucleo_register_qr(void)
{
    static const nucleo_app_def_t app = {
        "qr", "Codice QR", "Tools", "Genera un QR grande e inquadrabile",
        'Q', C_GREEN, enter, on_key, nullptr, draw, on_exit,
    };
    nucleo_app_register(&app);
}
