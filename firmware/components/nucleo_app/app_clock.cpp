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
    bool en = nucleo_i18n_is_en();
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
    d.setTextSize(size); d.setTextColor(col, bg);
    d.setCursor((W - (int)strlen(str) * 6 * size) / 2, y); d.print(str);
}

// ── digital face ──────────────────────────────────────────────────────────────
static void draw_digital(int top, int h, const struct tm *tm, bool synced)
{
    if (!synced) {
        centered("--:--", 6, top + h / 2 - 30, C_GREY, BG);
        centered(TR("attendo sincronizzazione NTP...", "waiting for NTP sync..."), 1, top + h / 2 + 20, C_YELLOW, BG);
        return;
    }
    int hh = s_h24 ? tm->tm_hour : (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12);
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", hh, tm->tm_min);

    const int big = 6, bigw = (int)strlen(hm) * 6 * big;          // "HH:MM" at size 6
    int side = (s_secs ? 4 + 2 * 6 * 3 : 0);                       // room for "SS" at size 3
    if (!s_h24 && side < 4 + 2 * 6 * 2) side = 4 + 2 * 6 * 2;      // or "AM"/"PM" at size 2
    int gx = (W - (bigw + side)) / 2;
    int ty = top + (h - 48 - 24) / 2;

    d.setTextSize(big); d.setTextColor(FG, BG); d.setCursor(gx, ty); d.print(hm);   // huge HH:MM in theme fg
    int sidex = gx + bigw + 4;
    if (s_secs) {
        char ss[4]; snprintf(ss, sizeof(ss), "%02d", tm->tm_sec);
        d.setTextSize(3); d.setTextColor(C_BLUE, BG); d.setCursor(sidex, ty + 24); d.print(ss);
    }
    if (!s_h24) {
        d.setTextSize(2); d.setTextColor(MUTED, BG);
        d.setCursor(sidex, ty + (s_secs ? 24 + 28 : 24)); d.print(tm->tm_hour < 12 ? "AM" : "PM");
    }
    char date[32]; fmt_date(tm, date, sizeof(date));
    centered(date, 2, ty + 48 + 12, MUTED, BG);
}

// A tapered watch hand (base quad + a short counter-balance tail) as two filled triangles.
static void hand(int cx, int cy, float ang, float len, float w, unsigned short col)
{
    float s = sinf(ang), c = cosf(ang), hw = w * 0.5f;
    int tx = (int)lroundf(cx + len * s),        ty = (int)lroundf(cy - len * c);
    int lx = (int)lroundf(cx - len * 0.16f * s), ly = (int)lroundf(cy + len * 0.16f * c);
    int b1x = (int)lroundf(cx + c * hw), b1y = (int)lroundf(cy + s * hw);
    int b2x = (int)lroundf(cx - c * hw), b2y = (int)lroundf(cy - s * hw);
    d.fillTriangle(b1x, b1y, b2x, b2y, tx, ty, col);
    d.fillTriangle(b1x, b1y, b2x, b2y, lx, ly, col);
}

// ── analog face ───────────────────────────────────────────────────────────────
static void draw_analog(int top, int h, const struct tm *tm, bool synced)
{
    int R = (h - 14) / 2; if (R > 46) R = 46;
    int cx = W / 2, cy = top + R + 2;

    d.fillCircle(cx, cy, R, C_GREY);        // rim...
    d.fillCircle(cx, cy, R - 3, BG);        // ...over the dark dial (3px ring)

    for (int i = 0; i < 12; i++) {          // hour ticks — fatter at 12/3/6/9
        float a = i * (TWO_PI / 12.0f); bool q = (i % 3 == 0);
        int tx = (int)lroundf(cx + (R - 6) * sinf(a)), tyk = (int)lroundf(cy - (R - 6) * cosf(a));
        d.fillCircle(tx, tyk, q ? 2 : 1, q ? FG : C_GREY);
    }

    if (!synced) {
        centered(TR("attendo NTP...", "waiting NTP..."), 1, cy - 4, C_YELLOW, BG);
        return;
    }

    float ah = ((tm->tm_hour % 12) + tm->tm_min / 60.0f) / 12.0f * TWO_PI;
    float am = (tm->tm_min + tm->tm_sec / 60.0f) / 60.0f * TWO_PI;
    float as = tm->tm_sec / 60.0f * TWO_PI;
    hand(cx, cy, ah, R * 0.50f, 5.0f, FG);      // hour
    hand(cx, cy, am, R * 0.78f, 3.0f, FG);      // minute
    hand(cx, cy, as, R * 0.86f, 1.5f, C_RED);   // second (accent)
    d.fillCircle(cx, cy, 3, C_RED);             // hub
    d.fillCircle(cx, cy, 1, BG);

    char date[32]; fmt_date(tm, date, sizeof(date));
    centered(date, 1, cy + R + 4, MUTED, BG);
}

// ── options card ──────────────────────────────────────────────────────────────
static void draw_settings(int top, int h)
{
    int bw = 204, bx = (W - bw) / 2, by = top + 3, bh = h - 6;
    d.fillRoundRect(bx, by, bw, bh, 8, INK);
    d.drawRoundRect(bx, by, bw, bh, 8, C_BLUE);
    d.setTextSize(2); d.setTextColor(C_BLUE, INK);
    d.setCursor(bx + 12, by + 6); d.print(TR("Opzioni", "Options"));

    char rows[3][32];
    snprintf(rows[0], 32, "%s %s", TR("Quadrante:", "Face:"), s_face == FACE_ANALOG ? TR("Analog.", "Analog") : TR("Digit.", "Digital"));
    snprintf(rows[1], 32, "%s %s", TR("Formato:", "Format:"), s_h24 ? "24h" : "12h");
    snprintf(rows[2], 32, "%s %s", TR("Secondi:", "Seconds:"), s_secs ? "On" : "Off");
    for (int i = 0; i < 3; i++) {
        bool sel = (i == s_set_sel); int ry = by + 28 + i * 20;
        if (sel) d.fillRoundRect(bx + 6, ry - 2, bw - 12, 18, 5, C_BLUE);
        d.setTextSize(2); d.setTextColor(sel ? INK : FG, sel ? C_BLUE : INK);
        d.setCursor(bx + 12, ry); d.print(rows[i]);
    }
    d.setTextSize(1); d.setTextColor(MUTED, INK);
    d.setCursor(bx + 12, by + bh - 10); d.print(TR("su/giu  </>  INVIO chiudi", "up/dn  </>  ENTER close"));
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);

    time_t now = time(NULL); struct tm *tm = localtime(&now);
    bool synced = (now >= EPOCH_2023) && tm;
    if (s_face == FACE_ANALOG) draw_analog(top, h, tm, synced);
    else                       draw_digital(top, h, tm, synced);

    if (s_settings) draw_settings(top, h);
    nucleo_app_set_hint(s_settings ? TR("su/giu | </> cambia | INVIO chiudi", "up/dn | </> change | ENTER close")
                                   : TR("TAB quadrante | INVIO opzioni", "TAB face | ENTER options"));
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
        'c', C_BLUE, enter, on_key, tick, draw, nullptr
    };
    nucleo_app_register(&app);
}
