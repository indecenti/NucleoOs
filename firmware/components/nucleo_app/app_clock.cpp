// Orologio — the definitive "what time is it now" app: a polished digital face and a hand-drawn
// analog face, big and legible, with 12/24h + seconds options. It ONLY tells the current time; the
// stopwatch/countdown live in the separate Cronometro app (app_chrono.cpp) and the motion alarm in
// app_alarm.cpp, so nothing here duplicates them.
//
// Style: full-bleed faces on the theme background (no header chrome — reads like a real watch), an
// options card reached with ENTER, the footer hint spells the controls, day/month names localised
// IT/EN. Anti-flicker is the canvas+blit default; on_tick only asks for a redraw when the shown unit
// (second for the analog face / digital-with-seconds, otherwise minute) actually changes, so an idle
// clock repaints at most once a second and sips battery.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"       // TR("it","en") + nucleo_i18n_is_en()
#include "nucleo_audio.h"      // soft click when toggling an option
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "app_gfx.h"
#include "app_large.h"         // two screens: the watch face on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

// The UI strings, one row per string in its languages: a call site loads an index, not five literals.
static const char *const S5[][5] = {
    { "attendo sincronizzazione NTP...", "waiting for NTP sync...", "espera sincroniz. NTP...", "attente synchro NTP...", "warte auf NTP-Sync..." },
    { "attendo NTP...", "waiting NTP...", "esperando NTP...", "attente NTP...", "warte auf NTP..." },
    { "Quadrante", "Face", "Cuadr.", "Cadran", "Ziff." },
    { "Formato", "Format", "Formato", "Format", "Format" },
    { "Secondi", "Seconds", "Segundos", "Secondes", "Sekunden" },
    { "Analog.", "Analog", "Analog.", "Analog.", "Analog." },
    { "Digit.", "Digital", "Digit.", "Digit.", "Digit." },
    { "Opzioni", "Options", "Opciones", "Options", "Optionen" },
    { "su/giu  </>  INVIO chiudi", "up/dn  </>  ENTER close", "arr/abj  </>  ENTER cerr.", "haut/bas  <>  ENTREE f.", "auf/ab  <>  ENTER sch." },
    { "tocca una riga per cambiarla", "tap a row to change it", "toca una fila para cambiar", "touchez une ligne", "Zeile antippen zum Aendern" },
    { "OROLOGIO", "CLOCK", "RELOJ", "HORLOGE", "UHR" },
    { "su/giu | </> cambia | INVIO chiudi", "up/dn | </> change | ENTER close", "arr/abj | </> cambia | enter cerr.", "haut/bas | <> change | ENTREE ferm.", "auf/ab | <> aend. | ENTER schlie." },
    { "TAB quadrante | INVIO opzioni", "TAB face | ENTER options", "TAB cuadr. | ENTER opciones", "TAB cadran | ENTREE options", "TAB ziff. | ENTER optionen" },
};
static const char *s5(int i) { const char *const *v = S5[i]; return nucleo_tr5(v[0], v[1], v[2], v[3], v[4]); }
static const char *const S2[][2] = {
    { "attendo NTP...", "waiting NTP..." },
    { "TAB quadrante  INVIO opzioni", "TAB face  ENTER options" },
    { "Analogico", "Analog" },
    { "Digitale", "Digital" },
    { "sec", "sec" },
    { "su/giu  </> cambia  INVIO chiudi", "up/dn  </> change  ENTER close" },
};
static const char *s2(int i) { return nucleo_tr(S2[i][0], S2[i][1]); }

#define ACC C_BLUE             // the clock's accent, as in its def

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif
#define TWO_PI 6.2831853f

// NTP hasn't landed while the clock sits near the 1970 epoch; 1672531200 = 2023-01-01 UTC.
#define EPOCH_2023 1672531200

enum { FACE_DIGITAL, FACE_ANALOG, FACE_COUNT };
static int  s_face  = FACE_DIGITAL;
static bool s_h24   = true;    // 24-hour clock (Italian default) vs 12-hour AM/PM
static bool s_secs  = true;    // show seconds on the digital face
static bool s_settings;        // options card open
static int  s_set_sel;         // selected options row
static int  s_last_unit = -1;  // last drawn second/minute, for change-detection in tick()

#define CFG "/sd/system/config/clock.json"

// Localised day/month names — strftime() only yields English in newlib's C locale, so map by hand.
static const char *DOW_IT[7] = { "Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab" };
static const char *DOW_EN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MON_IT[12] = { "Gen", "Feb", "Mar", "Apr", "Mag", "Giu", "Lug", "Ago", "Set", "Ott", "Nov", "Dic" };
static const char *MON_EN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void fmt_date(const struct tm *tm, char *out, int n)
{
    if (!tm) { snprintf(out, n, "----"); return; }
    bool en = !nucleo_i18n_is_it();   // English floor for es/fr/de, as TR() does
    const char *dow = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? (en ? DOW_EN : DOW_IT)[tm->tm_wday] : "";
    const char *mon = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? (en ? MON_EN : MON_IT)[tm->tm_mon] : "";
    snprintf(out, n, "%s %d %s %d", dow, tm->tm_mday, mon, 1900 + tm->tm_year);
}

// ── persistence ───────────────────────────────────────────────────────────────
static void load_cfg(void)
{
    FILE *f = fopen(CFG, "rb"); if (!f) return;
    char buf[96]; int n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    int face = 0, h24 = 1, secs = 1;
    sscanf(buf, "{\"face\":%d,\"h24\":%d,\"secs\":%d", &face, &h24, &secs);
    s_face = (face >= 0 && face < FACE_COUNT) ? face : FACE_DIGITAL;
    s_h24 = h24 != 0; s_secs = secs != 0;
}
static void save_cfg(void)
{
    mkdir("/sd/system", 0775); mkdir("/sd/system/config", 0775);
    FILE *f = fopen(CFG, "wb"); if (!f) return;
    fprintf(f, "{\"face\":%d,\"h24\":%d,\"secs\":%d}\n", s_face, s_h24 ? 1 : 0, s_secs ? 1 : 0);
    fclose(f);
}

// ── centred text helper ───────────────────────────────────────────────────────
static void centered(const char *str, int size, int y, unsigned short col, unsigned short bg)
{
    LovyanGFX &G = d;
    G.setTextSize(size); G.setTextColor(col, bg);
    G.setCursor((W - (int)strlen(str) * 6 * size) / 2, y); G.print(str);
}

// ── digital face ──────────────────────────────────────────────────────────────
static void draw_digital(int top, int h, const struct tm *tm, bool synced)
{
    LovyanGFX &G = d;
    if (!synced) {
        centered("--:--", 6, top + h / 2 - 30, C_GREY, BG);
        centered(s5(0 /* waiting for NTP sync... */), 1, top + h / 2 + 20, C_YELLOW, BG);
        return;
    }
    int hh = s_h24 ? tm->tm_hour : (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12);
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", hh, tm->tm_min);

    const int big = 6, bigw = (int)strlen(hm) * 6 * big;          // "HH:MM" at size 6
    int side = (s_secs ? 4 + 2 * 6 * 3 : 0);                       // room for "SS" at size 3
    if (!s_h24 && side < 4 + 2 * 6 * 2) side = 4 + 2 * 6 * 2;      // or "AM"/"PM" at size 2
    int gx = (W - (bigw + side)) / 2;
    int ty = top + (h - 48 - 24) / 2;

    G.setTextSize(big); G.setTextColor(FG, BG); G.setCursor(gx, ty); G.print(hm);   // huge HH:MM in theme fg
    int sidex = gx + bigw + 4;
    if (s_secs) {
        char ss[4]; snprintf(ss, sizeof(ss), "%02d", tm->tm_sec);
        G.setTextSize(3); G.setTextColor(C_BLUE, BG); G.setCursor(sidex, ty + 24); G.print(ss);
    }
    if (!s_h24) {
        G.setTextSize(2); G.setTextColor(MUTED, BG);
        G.setCursor(sidex, ty + (s_secs ? 24 + 28 : 24)); G.print(tm->tm_hour < 12 ? "AM" : "PM");
    }
    char date[32]; fmt_date(tm, date, sizeof(date));
    centered(date, 2, ty + 48 + 12, MUTED, BG);
}

// A tapered watch hand (base quad + a short counter-balance tail) as two filled triangles.
static void hand(int cx, int cy, float ang, float len, float w, unsigned short col)
{
    LovyanGFX &G = d;
    float s = sinf(ang), c = cosf(ang), hw = w * 0.5f;
    int tx = (int)lroundf(cx + len * s),        ty = (int)lroundf(cy - len * c);
    int lx = (int)lroundf(cx - len * 0.16f * s), ly = (int)lroundf(cy + len * 0.16f * c);
    int b1x = (int)lroundf(cx + c * hw), b1y = (int)lroundf(cy + s * hw);
    int b2x = (int)lroundf(cx - c * hw), b2y = (int)lroundf(cy - s * hw);
    G.fillTriangle(b1x, b1y, b2x, b2y, tx, ty, col);
    G.fillTriangle(b1x, b1y, b2x, b2y, lx, ly, col);
}

// ── analog face ───────────────────────────────────────────────────────────────
// One dial for both screens: everything scales with R (46 px on the Cardputer, ~100 on the panel). Returns
// the y just below the dial (where the caller writes the date).
static int face_analog(int cx, int cy, int R, const struct tm *tm, bool synced)
{
    LovyanGFX &G = d;
    const int rim = R / 16 + 1;
    G.fillCircle(cx, cy, R, C_GREY);        // rim...
    G.fillCircle(cx, cy, R - rim, BG);      // ...over the dark dial

    for (int i = 0; i < 60; i++) {          // hour dots, fatter at 12/3/6/9; minute marks on a big dial
        if (i % 5 && R < 80) continue;
        float a = i * (TWO_PI / 60.0f); bool q = (i % 15 == 0);
        int rr = R - rim - R / 10;
        int tx = (int)lroundf(cx + rr * sinf(a)), tyk = (int)lroundf(cy - rr * cosf(a));
        if (i % 5) G.drawPixel(tx, tyk, C_GREY);
        else       G.fillCircle(tx, tyk, (q ? 2 : 1) * R / 46, q ? FG : C_GREY);
    }

    if (synced) {
        float ah = ((tm->tm_hour % 12) + tm->tm_min / 60.0f) / 12.0f * TWO_PI;
        float am = (tm->tm_min + tm->tm_sec / 60.0f) / 60.0f * TWO_PI;
        float as = tm->tm_sec / 60.0f * TWO_PI;
        const float k = R / 46.0f;
        hand(cx, cy, ah, R * 0.50f, 5.0f * k, FG);      // hour
        hand(cx, cy, am, R * 0.78f, 3.0f * k, FG);      // minute
        hand(cx, cy, as, R * 0.86f, 1.5f * k, C_RED);   // second (accent)
        G.fillCircle(cx, cy, R / 15 + 1, C_RED);        // hub
        G.fillCircle(cx, cy, R / 40 + 1, BG);
    }
    return cy + R + 4;
}
static void draw_analog(int top, int h, const struct tm *tm, bool synced)
{
    int R = (h - 14) / 2; if (R > 46) R = 46;
    int y = face_analog(W / 2, top + R + 2, R, tm, synced);
    if (!synced) { centered(s5(1 /* waiting NTP... */), 1, top + R - 2, C_YELLOW, BG); return; }
    char date[32]; fmt_date(tm, date, sizeof(date));
    centered(date, 1, y, MUTED, BG);
}

// ── options card ──────────────────────────────────────────────────────────────
static const char *opt_label(int i, void *)
{
    return i == 0 ? s5(2 /* Face: */)
         : i == 1 ? s5(3 /* Format: */)
                  : s5(4 /* Seconds: */);
}
static const char *opt_value(int i, void *)
{
    return i == 0 ? (s_face == FACE_ANALOG ? s5(5 /* Analog */) : s5(6 /* Digital */))
         : i == 1 ? (s_h24 ? "24h" : "12h") : (s_secs ? "On" : "Off");
}
static void draw_settings(int top, int h)
{
    LovyanGFX &G = d;
    int bw = 204, bx = (W - bw) / 2, by = top + 3, bh = h - 6;
    G.fillRoundRect(bx, by, bw, bh, 8, INK);
    G.drawRoundRect(bx, by, bw, bh, 8, C_BLUE);
    G.setTextSize(2); G.setTextColor(C_BLUE, INK);
    G.setCursor(bx + 12, by + 6); G.print(s5(7 /* Options */));

    for (int i = 0; i < 3; i++) {
        char row[32]; snprintf(row, sizeof row, "%s: %s", opt_label(i, nullptr), opt_value(i, nullptr));
        bool sel = (i == s_set_sel); int ry = by + 28 + i * 20;
        if (sel) G.fillRoundRect(bx + 6, ry - 2, bw - 12, 18, 5, C_BLUE);
        G.setTextSize(2); G.setTextColor(sel ? INK : FG, sel ? C_BLUE : INK);
        G.setCursor(bx + 12, ry); G.print(row);
    }
    G.setTextSize(1); G.setTextColor(MUTED, INK);
    G.setCursor(bx + 12, by + bh - 10); G.print(s5(8 /* up/dn  </>  ENTER close */));
}

// ===== two screens ==============================================================================================
// The external 320x240 panel is a real, big watch, composed in strips (lg::frame) so the second never flickers:
// the digital face in 7-segment digits 96 px tall (seconds and date under them) or a 98 px analog dial with
// minute marks. Touch lives in the face: tap a side = next dial, the centre = options (rows you tap to change),
// the chevron at the top left = leave. The Cardputer meanwhile is a glanceable companion.
#define LW 320
#define LH 240

// Centred text within an arbitrary width (the compact helper assumes W).
static void centered_w(const char *str, int size, int y, unsigned short col, unsigned short bg, int cw)
{
    LovyanGFX &G = d;
    G.setTextSize(size); G.setTextColor(col, bg);
    G.setCursor((cw - (int)strlen(str) * 6 * size) / 2, y); G.print(str);
}
static void lt(const char *s, int y, int f, unsigned short c)   // panel text centred on the panel
{
    LovyanGFX &G = d;
    lg::font(f); G.setTextDatum(top_center); G.setTextColor(c); G.drawString(s, LW / 2, y); G.setTextDatum(top_left);
}

// The options page, drawn straight to the panel (the kit's rows keep their card colour, which the 8-bit canvas of
// lg::frame would lose); every box repaints itself, the page is cleared once when it opens.
static void lg_options(void)
{
    int y = lg::header(s5(7 /* Options */), nullptr, ACC) + 8;
    for (int i = 0; i < 3; i++, y += 40) {                       // the kit's settings rows: two cycles + a switch
        const lg::Setting st = { opt_label(i, nullptr), nullptr, i < 2 ? opt_value(i, nullptr) : nullptr, (unsigned char)(i < 2 ? lg::ROW_CYCLE : lg::ROW_TOGGLE),
                                 -1, -1, s_secs, false, 0, 0, 0 };
        lg::setting(8, y, LW - 16, 40, st, i == s_set_sel, ACC);
        nucleo_touch_add(0, y, LW, 40, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
    lt(s5(9 /* tap a row to change it */), LH - 26, 0, MUTED);
}

static void lg_paint(void *)
{
    LovyanGFX &G = d;
    G.fillRect(0, 0, LW, LH, BG);
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    bool synced = (now >= EPOCH_2023) && tm;
    int y;
    if (s_face == FACE_ANALOG) {
        y = face_analog(LW / 2, 106, 98, tm, synced);
    } else {
        char t[8];
        if (synced) snprintf(t, sizeof t, "%02d:%02d", s_h24 ? tm->tm_hour : (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12), tm->tm_min);
        else        snprintf(t, sizeof t, "--:--");
        G.setFont(&fonts::Font7); G.setTextDatum(top_center);
        G.setTextSize(2); G.setTextColor(synced ? FG : C_GREY); G.drawString(t, LW / 2, 30);
        y = 136;
        if (s_secs && synced) {
            snprintf(t, sizeof t, "%02d", tm->tm_sec);
            G.setTextSize(1); G.setTextColor(ACC); G.drawString(t, LW / 2, y);
            y += 56;
        }
        G.setTextSize(1); G.setTextDatum(top_left);
    }
    if (!synced) lt(s5(0 /* waiting for NTP sync... */), LH - 30, 1, C_YELLOW);
    else {
        char date[40]; fmt_date(tm, date, sizeof date);
        if (!s_h24) { int l = strlen(date); snprintf(date + l, sizeof date - l, "  %s", tm->tm_hour < 12 ? "AM" : "PM"); }
        lt(date, y + 4, 2, MUTED);
    }
    for (int k = 0; k < 2; k++) { G.drawLine(16 + k, 8, 10 + k, 15, MUTED); G.drawLine(10 + k, 15, 16 + k, 22, MUTED); }
    nucleo_touch_add(0, 0, 90, 34, NK_BACK, 0);                          // top-left chevron: leave
    nucleo_touch_add(0, 34, LW * 2 / 5, LH - 34, NK_TAB, 0);             // sides: next dial
    nucleo_touch_add(LW - LW * 2 / 5, 34, LW * 2 / 5, LH - 34, NK_TAB, 0);
    nucleo_touch_add(LW * 2 / 5, 34, LW / 5, LH - 34, NK_ENTER, 0);      // centre: options
}

static bool s_lg_full = true;                                       // the options page must clear the panel first
static void draw_large(void)
{
    nucleo_touch_clear();
    if (s_settings) {
        if (s_lg_full) { d.fillRect(0, 0, LW, LH, BG); s_lg_full = false; }
        lg_options();
    } else { s_lg_full = true; lg::frame(lg_paint, nullptr, 0, 0, true); }
    nucleo_touch_commit();
}

// The Cardputer while the panel is the watch: the time big, the date, what dial/format is set, and the keys.
static void draw_companion(void)
{
    LovyanGFX &G = d;
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    G.fillRect(0, top, W, h, BG);
    int y = lg::comp_head(s5(10 /* CLOCK */), ACC);

    time_t now = time(NULL); struct tm *tm = localtime(&now);
    bool synced = (now >= EPOCH_2023) && tm;
    if (!synced) {
        G.setTextSize(4); G.setTextColor(C_GREY, BG); centered_w("--:--", 4, y + 20, C_GREY, BG, W);
        G.setTextSize(1); G.setTextColor(C_YELLOW, BG); centered_w(s2(0 /* waiting NTP... */), 1, y + 58, C_YELLOW, BG, W);
        lg::comp_keys(s2(1 /* TAB face  ENTER options */));
        return;
    }
    int hh = s_h24 ? tm->tm_hour : (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12);
    char hm[12];
    if (s_secs) snprintf(hm, sizeof(hm), "%02d:%02d:%02d", hh, tm->tm_min, tm->tm_sec);
    else        snprintf(hm, sizeof(hm), "%02d:%02d", hh, tm->tm_min);
    int size = s_secs ? 3 : 4;
    centered_w(hm, size, y + 16, FG, BG, W);
    if (!s_h24) { G.setTextSize(1); centered_w(tm->tm_hour < 12 ? "AM" : "PM", 1, y + 16 + size * 8 + 2, MUTED, BG, W); }

    char date[32]; fmt_date(tm, date, sizeof(date));
    centered_w(date, 1, y + 50, MUTED, BG, W);
    char info[40];
    snprintf(info, sizeof(info), "%s  %s  %s", s_face == FACE_ANALOG ? s2(2 /* Analog */) : s2(3 /* Digital */),
             s_h24 ? "24h" : "12h", s_secs ? s2(4 /* sec */) : "");
    centered_w(info, 1, y + 64, ACC, BG, W);
    lg::comp_keys(s_settings ? s2(5 /* up/dn  </> change  ENTER close */)
                             : s2(1 /* TAB face  ENTER options */));
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }

    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);

    time_t now = time(NULL); struct tm *tm = localtime(&now);
    bool synced = (now >= EPOCH_2023) && tm;
    if (s_face == FACE_ANALOG) draw_analog(top, h, tm, synced);
    else                       draw_digital(top, h, tm, synced);

    if (s_settings) draw_settings(top, h);
    nucleo_app_set_hint(s_settings ? s5(11 /* up/dn | </> change | ENTER close */)
                                   : s5(12 /* TAB face | ENTER options */));
}

// ── change detection: redraw only when the shown unit ticks over ──────────────
static void tick(void)
{
    if (s_settings) return;
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    if (!tm) return;
    int unit = (s_face == FACE_ANALOG || s_secs) ? tm->tm_sec : tm->tm_min;
    if (unit != s_last_unit) { s_last_unit = unit; nucleo_app_request_draw(); }
}

// ── input ──────────────────────────────────────────────────────────────────────
static void change_val(int row, int dir)
{
    if (row == 0)      s_face = (s_face + dir + FACE_COUNT) % FACE_COUNT;
    else if (row == 1) s_h24 = !s_h24;
    else               s_secs = !s_secs;
    s_last_unit = -1;   // force the next tick to repaint
    nucleo_audio_tone(1200, 28, 55);
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_settings) {
        if (nucleo_touch_is_row(key, ch)) {        // a tapped option row on the panel: select and toggle it
            int i = ch - NUCLEO_TOUCH_ROW; if (i >= 0 && i < 3) { s_set_sel = i; change_val(i, +1); }
            return;
        }
        if (key == NK_UP)         { s_set_sel = (s_set_sel + 2) % 3; nucleo_app_request_draw(); }
        else if (key == NK_DOWN)  { s_set_sel = (s_set_sel + 1) % 3; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT) { change_val(s_set_sel, +1); }
        else if (key == NK_ENTER) { s_settings = false; save_cfg(); nucleo_app_request_draw(); }
        return;
    }
    if (key == NK_ENTER) { s_settings = true; s_set_sel = 0; nucleo_app_request_draw(); }
}

// LEFT + BACK route here. In options LEFT decrements and BACK closes; on a face, BACK exits.
static bool on_back(int key)
{
    if (s_settings) {
        if (key == NK_LEFT) { change_val(s_set_sel, -1); return true; }
        s_settings = false; save_cfg(); nucleo_app_request_draw(); return true;
    }
    if (key == NK_LEFT) return true;   // swallow stray LEFT on a face
    return false;                       // BACK exits the app
}

static void on_tab(void)
{
    if (s_settings) return;
    s_face = (s_face + 1) % FACE_COUNT; s_last_unit = -1;
    save_cfg(); nucleo_audio_tone(1200, 30, 50); nucleo_app_request_draw();
}

static void enter(void)
{
    load_cfg();
    s_settings = false; s_set_sel = 0; s_last_unit = -1;
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_clock(void)
{
    static const nucleo_app_def_t app = {
        "clock", "Orologio", "Office", "Orologio digitale e analogico",
        'c', C_BLUE, enter, on_key, tick, draw, nullptr, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
