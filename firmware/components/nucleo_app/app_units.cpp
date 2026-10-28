// Unit converter — Tools, store only (native module, never built into the firmware).
//
// Pick a quantity (length, weight, temperature, volume, area, speed, data, time, pressure), type a number on the
// keyboard and every unit of that quantity shows its value at once. The selected row is the unit being typed;
// moving the selection keeps the quantity (the new row's value becomes the input), so "5 mi" -> down to "km" is
// "8.04672 km" ready to edit. Keys: 0-9, ',' decimal comma, '-' sign, DEL erase, c clear, ; . unit, / ] next
// quantity, [ previous. State (quantity + unit per quantity) is kept in /sd/data/units.cfg.
//
// Two screens: the panel is the primary view — the quantity strip on top (tap the arrows), every unit as a row
// (tap one to type in it) and a touch keypad at the right; the Cardputer shows the typed value and the closest
// conversions as a companion.
//
// Native-module constraints (ABI 3): no double arithmetic or printf of floats (the module imports no double
// helpers), so numbers are formatted here with integers; the OS language is read from settings.json -> ui.language
// (nucleo_tr5 is not in the ABI), all five languages are in this file.
// Store only: compiled for the native module (NAP_MODULE) and by the host harnesses, empty in the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "nucleo_app.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "nucleo_theme.h"      // THEME_*: the panel follows the OS theme
#include "nucleo_touch.h"
#include "app_large.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"

static const unsigned short ACC = 0x4D1F, ERR = 0xF96B;

// ---- language ---------------------------------------------------------------------------------------------------
static int s_lang = 1;                                      // 0 it, 1 en, 2 es, 3 fr, 4 de (default English, as the OS)
struct L5 { const char *t[5]; };
static const char *tr(const L5 &l) { return l.t[s_lang]; }

static void read_lang(void)
{
    s_lang = 1;
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/settings.json", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n <= 0 || n > 16384) { fclose(f); return; }
    char *buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, n, f); fclose(f); buf[got] = 0;
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) return;
    cJSON *ui = cJSON_GetObjectItem(root, "ui");
    cJSON *lg = ui ? cJSON_GetObjectItem(ui, "language") : nullptr;
    if (lg && lg->valuestring) {
        static const char *const CODES[5] = { "it", "en", "es", "fr", "de" };
        for (int i = 0; i < 5; i++) if (!strncmp(lg->valuestring, CODES[i], 2)) s_lang = i;
    }
    cJSON_Delete(root);
}

// ---- the units ------------------------------------------------------------------------------------------------
// Each unit is its size in the quantity's base unit (temperature is converted by formula: base = kelvin).
struct Unit { const char *sym; L5 name; float f; };
struct Qty  { L5 name; const Unit *u; int n; bool temp; };

#define U(s, f) { s, { { s, s, s, s, s } }, f }
static const Unit LEN[] = { U("mm", 0.001f), U("cm", 0.01f), U("m", 1.0f), U("km", 1000.0f),
                            U("in", 0.0254f), U("ft", 0.3048f), U("yd", 0.9144f), U("mi", 1609.344f) };
static const Unit MASS[] = { U("mg", 1e-6f), U("g", 0.001f), U("kg", 1.0f), U("t", 1000.0f),
                             U("oz", 0.028349523f), U("lb", 0.45359237f), U("st", 6.3502932f) };
static const Unit TEMP[] = { U("C", 0), U("F", 1), U("K", 2) };
static const Unit VOL[] = { U("ml", 0.001f), U("cl", 0.01f), U("l", 1.0f), U("m3", 1000.0f),
                            { "cup", { { "tazza US", "cup US", "taza US", "tasse US", "Tasse US" } }, 0.2365882f },
                            U("fl oz", 0.0295735f), U("gal US", 3.7854118f), U("gal UK", 4.54609f) };
static const Unit AREA[] = { U("cm2", 1e-4f), U("m2", 1.0f), U("ha", 1e4f), U("km2", 1e6f),
                             U("ft2", 0.09290304f), { "acre", { { "acri", "acres", "acres", "acres", "Acres" } }, 4046.8564f },
                             U("mi2", 2589988.1f) };
static const Unit SPEED[] = { U("m/s", 1.0f), U("km/h", 1.0f / 3.6f), U("mph", 0.44704f),
                              { "kn", { { "nodi", "knots", "nudos", "noeuds", "Knoten" } }, 0.5144444f }, U("ft/s", 0.3048f) };
static const Unit DATA[] = { U("B", 1.0f), U("KB", 1e3f), U("MB", 1e6f), U("GB", 1e9f), U("TB", 1e12f),
                             U("KiB", 1024.0f), U("MiB", 1048576.0f), U("GiB", 1073741824.0f) };
static const Unit TIME[] = { U("ms", 0.001f), U("s", 1.0f), U("min", 60.0f), U("h", 3600.0f),
                             { "d", { { "giorni", "days", "dias", "jours", "Tage" } }, 86400.0f },
                             { "wk", { { "settimane", "weeks", "semanas", "semaines", "Wochen" } }, 604800.0f },
                             { "y", { { "anni", "years", "anos", "ans", "Jahre" } }, 31557600.0f } };
static const Unit PRES[] = { U("Pa", 1.0f), U("kPa", 1000.0f), U("bar", 1e5f), U("atm", 101325.0f),
                             U("psi", 6894.757f), U("mmHg", 133.32237f) };
#undef U

#define N(a) (int)(sizeof(a) / sizeof(a[0]))
static const Qty QTY[] = {
    { { { "Lunghezza", "Length", "Longitud", "Longueur", "Laenge" } }, LEN, N(LEN), false },
    { { { "Peso", "Weight", "Peso", "Poids", "Gewicht" } }, MASS, N(MASS), false },
    { { { "Temperatura", "Temperature", "Temperatura", "Temperature", "Temperatur" } }, TEMP, N(TEMP), true },
    { { { "Volume", "Volume", "Volumen", "Volume", "Volumen" } }, VOL, N(VOL), false },
    { { { "Area", "Area", "Area", "Surface", "Flaeche" } }, AREA, N(AREA), false },
    { { { "Velocita", "Speed", "Velocidad", "Vitesse", "Geschw." } }, SPEED, N(SPEED), false },
    { { { "Dati", "Data", "Datos", "Donnees", "Daten" } }, DATA, N(DATA), false },
    { { { "Tempo", "Time", "Tiempo", "Temps", "Zeit" } }, TIME, N(TIME), false },
    { { { "Pressione", "Pressure", "Presion", "Pression", "Druck" } }, PRES, N(PRES), false },
};
enum { NQ = N(QTY), MAXU = 8 };
#undef N

static int  s_q;                       // the quantity shown
static int  s_sel[NQ];                 // the unit being typed, per quantity
static char s_in[20];                  // the typed number ('.' inside, shown with the language's separator)
static bool s_fresh;                   // the input came from a conversion: the next digit starts a new number
static float s_base;                   // ...and the quantity it shows, unrounded (so moving around never drifts)

static float to_base(const Qty &q, int u, float v)
{
    if (!q.temp) return v * q.u[u].f;
    return u == 0 ? v + 273.15f : u == 1 ? (v - 32.0f) * (5.0f / 9.0f) + 273.15f : v;
}
static float from_base(const Qty &q, int u, float b)
{
    if (!q.temp) return b / q.u[u].f;
    return u == 0 ? b - 273.15f : u == 1 ? (b - 273.15f) * (9.0f / 5.0f) + 32.0f : b;
}

// ---- numbers ----------------------------------------------------------------------------------------------------
static char sep(void) { return s_lang == 1 ? '.' : ','; }

static float p10(int e)                                     // 10^e, exact for the range a float shows
{
    float r = 1.0f;
    if (e >= 0) while (e--) r *= 10.0f; else while (e++) r /= 10.0f;
    return r;
}

// Six significant digits, trailing zeros dropped; scientific outside 1e-6 .. 1e10. `ds` = decimal separator.
static void fmt(float v, char *out, int n, char ds)
{
    if (v != v || v > 3e38f || v < -3e38f) { snprintf(out, n, "-"); return; }
    if (v == 0.0f) { snprintf(out, n, "0"); return; }
    char *o = out, *end = out + n - 1;
    if (v < 0) { *o++ = '-'; v = -v; }
    int e = (int)floorf(log10f(v));
    long long q = 0;
    for (int k = 0; k < 3; k++) {                           // log10f can land one off near a power of ten
        q = (long long)(v / p10(e - 5) + 0.5f);
        if (q >= 1000000) e++; else if (q < 100000) e--; else break;
    }
    char dg[7]; for (int i = 5; i >= 0; i--) { dg[i] = (char)('0' + (int)(q % 10)); q /= 10; } dg[6] = 0;
    int last = 5; while (last > 0 && dg[last] == '0') last--;           // last significant digit
    if (e >= 10 || e < -6) {                                            // d.ddddde<e>
        *o++ = dg[0];
        if (last > 0 && o < end) { *o++ = ds; for (int i = 1; i <= last && o < end; i++) *o++ = dg[i]; }
        snprintf(o, end - o + 1, "e%d", e);
        return;
    }
    if (e < 0) {                                                        // 0.000ddd
        if (o < end) *o++ = '0';
        if (o < end) *o++ = ds;
        for (int i = 0; i < -e - 1 && o < end; i++) *o++ = '0';
        for (int i = 0; i <= last && o < end; i++) *o++ = dg[i];
    } else {
        for (int i = 0; i <= e && o < end; i++) *o++ = i < 6 ? dg[i] : '0';
        if (last > e) { if (o < end) *o++ = ds; for (int i = e + 1; i <= last && o < end; i++) *o++ = dg[i]; }
    }
    *o = 0;
}

static float input_value(void) { return s_in[0] && strcmp(s_in, "-") ? strtof(s_in, nullptr) : 0.0f; }
static float base_value(void) { return s_fresh ? s_base : to_base(QTY[s_q], s_sel[s_q], input_value()); }

// The input as shown: the language's separator, "0" when empty.
static void shown_input(char *out, int n)
{
    snprintf(out, n, "%s", s_in[0] ? s_in : "0");
    for (char *p = out; *p; p++) if (*p == '.') *p = sep();
}

// The value of unit u for what is typed in the selected unit.
static void value_of(int u, char *out, int n)
{
    const Qty &q = QTY[s_q];
    if (u == s_sel[s_q]) { shown_input(out, n); return; }
    fmt(from_base(q, u, base_value()), out, n, sep());
}

// ---- state ------------------------------------------------------------------------------------------------------
#define CFG NUCLEO_SD_MOUNT "/data/units.cfg"
static void cfg_read(void)
{
    // first run: km, kg, C, l, m2, km/h, MB, min, bar
    unsigned char b[1 + NQ] = { 0, 3, 2, 0, 2, 1, 1, 2, 2, 2 };
    unsigned char r[sizeof b];
    FILE *f = fopen(CFG, "rb");
    if (f) { if (fread(r, 1, sizeof r, f) == sizeof r) memcpy(b, r, sizeof b); fclose(f); }
    s_q = b[0] < NQ ? b[0] : 0;
    for (int i = 0; i < NQ; i++) s_sel[i] = b[1 + i] < QTY[i].n ? b[1 + i] : 0;
}
static void cfg_write(void)
{
    unsigned char b[1 + NQ];
    b[0] = (unsigned char)s_q;
    for (int i = 0; i < NQ; i++) b[1 + i] = (unsigned char)s_sel[i];
    mkdir(NUCLEO_SD_MOUNT "/data", 0775);
    FILE *f = fopen(CFG, "wb");
    if (f) { fwrite(b, 1, sizeof b, f); fclose(f); }
}

// ---- input ------------------------------------------------------------------------------------------------------
static void set_quantity(int q) { s_q = (q + NQ) % NQ; s_in[0] = 0; s_fresh = false; }

static void select_unit(int u)                              // keep the quantity, type in the new unit
{
    const Qty &q = QTY[s_q];
    if (u < 0 || u >= q.n || u == s_sel[s_q]) return;
    if (s_in[0]) { const float b = base_value(); fmt(from_base(q, u, b), s_in, sizeof s_in, '.'); s_base = b; s_fresh = true; }
    if (!strcmp(s_in, "-")) s_in[0] = 0;
    s_sel[s_q] = u;
}

static void type_char(char c)
{
    if (s_fresh) { s_in[0] = 0; s_fresh = false; }
    int l = (int)strlen(s_in);
    if (c == '.') {
        if (strchr(s_in, '.') || strchr(s_in, 'e')) return;
        if (!l || !strcmp(s_in, "-")) { if (l + 2 >= (int)sizeof s_in) return; strcat(s_in, "0"); l++; }
    } else if (!strcmp(s_in, "0")) l = 0;                   // no leading zero
    else if (!strcmp(s_in, "-0")) l = 1;
    if (l + 1 >= (int)sizeof s_in) return;
    s_in[l] = c; s_in[l + 1] = 0;
}

static void toggle_sign(void)
{
    s_fresh = false;
    if (s_in[0] == '-') memmove(s_in, s_in + 1, strlen(s_in));
    else if ((int)strlen(s_in) + 1 < (int)sizeof s_in) { memmove(s_in + 1, s_in, strlen(s_in) + 1); s_in[0] = '-'; }
}

static void erase(void) { s_fresh = false; int l = (int)strlen(s_in); if (l) s_in[l - 1] = 0; }

static bool lg_tap(int key, char ch);
static void on_key(int key, char ch)
{
    if (lg_tap(key, ch)) { nucleo_app_request_draw(); return; }
    const int n = QTY[s_q].n;
    if (key == NK_UP) select_unit((s_sel[s_q] + n - 1) % n);
    else if (key == NK_DOWN) select_unit((s_sel[s_q] + 1) % n);
    else if (key == NK_LEFT) type_char('.');                // ',' : the decimal comma
    else if (key == NK_RIGHT || ch == ']') set_quantity(s_q + 1);
    else if (ch == '[') set_quantity(s_q - 1);
    else if (key == NK_DEL) erase();
    else if (ch == '-' || ch == '_') toggle_sign();
    else if (ch == 'c' || ch == 'C') { s_in[0] = 0; s_fresh = false; }
    else if (ch >= '0' && ch <= '9') type_char(ch);
    else return;
    nucleo_app_request_draw();
}

// ---- the Cardputer (240x135) ----------------------------------------------------------------------------------
// The quantity on top, the typed value large with its unit, then the other units two per line.
static void draw_compact(bool companion)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    const Qty &q = QTY[s_q];
    const int su = s_sel[s_q];
    d.fillRect(0, top, 240, h, THEME_BG);
    int y = top;
    if (companion) y = lg::comp_head(tr(q.name), ACC);
    else {
        d.setFont(&fonts::Font2); d.setTextDatum(top_left); d.setTextColor(ACC, THEME_BG);
        d.drawString(tr(q.name), 6, y + 1);
        char pg[8]; snprintf(pg, sizeof pg, "%d/%d", s_q + 1, NQ);
        d.setFont(&fonts::Font0); d.setTextDatum(top_right); d.setTextColor(THEME_MUTED, THEME_BG);
        d.drawString(pg, 234, y + 5);
        y += 18;
    }
    // the typed value: the biggest face it fits in, the tail when nothing does
    char v[24]; shown_input(v, sizeof v);
    const char *sym = q.u[su].sym;
    d.setFont(&fonts::Font0); const int sw = d.textWidth(sym) + 8;
    d.fillRoundRect(4, y, 232, 24, 6, lg::card());
    d.drawRoundRect(4, y, 232, 24, 6, ACC);
    const lgfx::IFont *faces[3] = { &fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::Font2 };
    int fi = 0; d.setFont(faces[0]);
    while (fi < 2 && d.textWidth(v) > 222 - sw) d.setFont(faces[++fi]);
    const char *t = v; while (*t && d.textWidth(t) > 222 - sw) t++;
    d.setTextDatum(middle_right); d.setTextColor(s_fresh ? THEME_MUTED : THEME_FG, lg::card());
    d.drawString(t, 232 - sw, y + 12);
    d.setFont(&fonts::Font0); d.setTextDatum(middle_right); d.setTextColor(ACC, lg::card());
    d.drawString(sym, 230, y + 12);
    y += 27;
    // the other units, two columns, as many lines as fit above the key hint
    const int lines = (top + h - 12 - y) / 16;
    int slot = 0;
    for (int u = 0; u < q.n && slot < lines * 2; u++) {
        if (u == su) continue;
        const int x = (slot % 2) * 118 + 6, ly = y + (slot / 2) * 16;
        char val[24]; value_of(u, val, sizeof val);
        char us[10]; snprintf(us, sizeof us, "%.9s", q.u[u].sym);
        d.setFont(&fonts::Font0); const int uw = d.textWidth(us) + 4;
        d.setFont(&fonts::Font2); d.setTextDatum(top_left); d.setTextColor(THEME_FG, THEME_BG);
        char fv[24]; lg::fit(fv, sizeof fv, val, 108 - uw);
        d.drawString(fv, x, ly);                                 // "value unit", the unit muted right after it
        const int vx = x + d.textWidth(fv) + 3;
        d.setFont(&fonts::Font0); d.setTextColor(ACC, THEME_BG);
        d.drawString(us, vx, ly + 5);
        slot++;
    }
    static const L5 HINTK = { { ";. unita  / tipo  , virgola  c azzera", ";. unit  / kind  , decimal  c clear",
                               ";. unidad  / tipo  , decimal  c borrar", ";. unite  / type  , virgule  c efface",
                               ";. Einheit  / Art  , Komma  c loeschen" } };
    if (companion) lg::comp_keys(tr(HINTK));               // alone, the system hint line says it
}

static void draw(void) { draw_compact(nucleo_app_two_screens()); }

// ---- the panel (320 x 210 under the system bar, 320 x 240 without it) -----------------------------------------
// Left: the quantity strip (tap < or > to change it) and one row per unit (tap one to type in it; the typed row is
// the accent pill). Right: the keypad. One field over everything, hit-tested in lg_tap — the system bar keeps Back.
static const int KX = 204, KCOLS = 3, KROWS = 5, STRIP = 30;
static const char KEYS[KROWS * KCOLS + 1] = "789456123,0<-C ";  // '<' erase, '-' sign, 'C' clear, ' ' = none
static int s_ph = 210;                                          // the panel surface height, read outside the frame

static int row_h(void) { return (s_ph - STRIP - 4) / MAXU; }

static void lg_paint(void *)
{
    const int H = s_ph;
    const Qty &q = QTY[s_q];
    const int su = s_sel[s_q];
    d.fillRect(0, 0, 320, H, THEME_BG);
    // the quantity strip
    lg::panel(4, 2, KX - 8, STRIP - 4);
    for (int s = -1; s <= 1; s += 2) {                           // the two chevrons
        const int cx = s < 0 ? 18 : KX - 18, cy = STRIP / 2;
        for (int o = 0; o < 2; o++) {
            d.drawLine(cx - 3 * s + o, cy - 6, cx + 3 * s + o, cy, ACC);
            d.drawLine(cx + 3 * s + o, cy, cx - 3 * s + o, cy + 6, ACC);
        }
    }
    lg::font(1); d.setTextDatum(middle_center); d.setTextColor(THEME_FG, lg::card());
    d.drawString(tr(q.name), KX / 2, STRIP / 2);
    // the units
    const int rh = row_h();
    for (int u = 0; u < q.n; u++) {
        const int y = STRIP + u * rh;
        const bool f = u == su;
        const unsigned short bg = f ? ACC : THEME_BG;
        if (f) d.fillRoundRect(4, y + 1, KX - 8, rh - 2, 8, ACC);
        char val[24]; value_of(u, val, sizeof val);
        lg::row_text(4, y, KX - 8, rh, f, bg, tr(q.u[u].name), val, f ? THEME_BG : THEME_FG);
        if (f && !s_fresh) {                                     // a caret after the number being typed
            const int cx = KX - 10;
            d.fillRect(cx, y + rh / 2 - 6, 2, 12, THEME_BG);
        }
    }
    // the keypad
    const int kw = (320 - KX) / KCOLS, kh = H / KROWS;
    for (int i = 0; i < KROWS * KCOLS; i++) {
        const char k = KEYS[i];
        if (k == ' ') continue;
        const int x = KX + (i % KCOLS) * kw, y = (i / KCOLS) * kh;
        d.fillRoundRect(x + 2, y + 2, kw - 4, kh - 4, 8, THEME_LINE);
        const int cx = x + kw / 2, cy = y + kh / 2;
        const unsigned short c = k == 'C' ? ERR : (k >= '0' && k <= '9') ? THEME_FG : ACC;
        if (k == '<') {                                          // erase: a tag with an x
            for (int o = 0; o < 2; o++) { d.drawLine(cx - 12, cy + o, cx - 6, cy - 6 + o, c); d.drawLine(cx - 12, cy - o, cx - 6, cy + 6 - o, c); }
            d.drawRect(cx - 6, cy - 6, 17, 13, c);
            d.drawLine(cx - 1, cy - 3, cx + 6, cy + 3, c); d.drawLine(cx - 1, cy + 3, cx + 6, cy - 3, c);
        } else if (k == '-') {                                   // the sign: +/-
            d.fillRect(cx - 12, cy - 5, 11, 2, c); d.fillRect(cx - 7, cy - 10, 2, 11, c);
            d.drawLine(cx + 7, cy - 10, cx - 3, cy + 10, c);
            d.fillRect(cx + 2, cy + 6, 11, 2, c);
        } else {
            char t[2] = { k == ',' ? sep() : k, 0 };
            lg::font(2); d.setTextDatum(middle_center); d.setTextColor(c, THEME_LINE);
            d.drawString(t, cx, cy + 1);
        }
    }
    d.setTextDatum(top_left);
    lg::field(0, 0, 320, H);
}

static void draw_large(void)
{
    s_ph = d.height() > 0 && d.height() <= 240 ? d.height() : 210;
    lg::frame(lg_paint, nullptr, 0, 0, true);                    // exact theme colours (RGB565 strips)
}

static bool lg_tap(int key, char ch)
{
    int x, y;
    if (!lg::tap(key, ch, &x, &y)) return false;
    if (x >= KX) {                                               // the keypad
        const int c = (x - KX) * KCOLS / (320 - KX), r = y * KROWS / s_ph;
        const char k = KEYS[(r < KROWS ? r : KROWS - 1) * KCOLS + (c < KCOLS ? c : KCOLS - 1)];
        if (k == '<') erase();
        else if (k == '-') toggle_sign();
        else if (k == 'C') { s_in[0] = 0; s_fresh = false; }
        else if (k == ',') type_char('.');
        else if (k != ' ') type_char(k);
    } else if (y < STRIP) set_quantity(x < KX / 2 ? s_q - 1 : s_q + 1);
    else select_unit((y - STRIP) / row_h());
    return true;
}

// ---- lifecycle --------------------------------------------------------------------------------------------------
static void on_enter(void)
{
    read_lang();
    cfg_read();
    s_in[0] = 0; s_fresh = false;
    static const L5 HINT = { { ";. unita  / tipo  , virgola  - segno", ";. unit  / kind  , decimal  - sign",
                               ";. unidad  / tipo  , decimal  - signo", ";. unite  / type  , virgule  - signe",
                               ";. Einheit  / Art  , Komma  - Vorz." } };
    nucleo_app_set_hint(tr(HINT));
}
static void on_exit(void) { cfg_write(); }

extern "C" void nucleo_register_units(void)
{
    static const nucleo_app_def_t app = {
        "units", "Converter", "Tools", "Convert length, weight, temperature and more",
        'U', ACC, on_enter, on_key, nullptr, draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
#endif  // NAP_MODULE || !ESP_PLATFORM
