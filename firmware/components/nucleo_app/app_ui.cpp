// Implementation of the shared focused-list widget (see app_ui.h).
// Static-only: rows snap to position instantly and long labels are truncated to fit (no scrolling
// text), so there is nothing to animate. Apps redraw only on key input — zero redraws when idle.
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include "esp_timer.h"

#include "app_gfx.h"
#include "nucleo_theme.h"
#include "nucleo_i18n.h"     // TR(it,en): the confirm card's Yes/No follow the system language
// Palette follows the active OS theme (was hardcoded literals -> the shared list widget ignored
// theme switches while the launcher recolored). ink/fg are constant across the current all-dark
// theme set, so contrast is preserved. Zero RAM cost: THEME_* are existing globals.
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
#define INK   THEME_INK
#define ACC   THEME_ACC

// ---- live-app repaint helpers (see app_ui.h) -------------------------------------------------------
int app_ui_step(int cur, float v)
{
    if (cur == INT32_MIN) return (int)lroundf(v);
    float dv = v - (float)cur;
    if (dv < 0) dv = -dv;
    return dv >= 1.5f ? (int)lroundf(v) : cur;      // deadband: hold the step the user is looking at
}

bool app_ui_frame_due(int64_t *next_us, int fps)
{
    int64_t now = esp_timer_get_time();
    if (*next_us && now < *next_us) return false;
    *next_us = now + 1000000 / (fps > 0 ? fps : 20);
    return true;
}

// Fold UTF-8 (accents, smart quotes, dashes) to ASCII and drop anything else, so the ASCII-only
// GFX fonts never render tofu boxes for web-authored text (calendar events), online answers or
// accented city names. Shared by ANIMA and Calendar (see app_ui.h).
void app_ui_ascii_fold(const char *src, char *dst, int cap)
{
    int o = 0; const unsigned char *s = (const unsigned char *)src;
    while (*s && o < cap - 1) {
        unsigned char c = *s;
        if (c < 0x80) { dst[o++] = (char)c; s++; continue; }
        if (c == 0xC3 && s[1]) {                              // Latin-1 supplement (accented letters)
            char r = 0; unsigned char d2 = s[1];
            if      (d2 >= 0x80 && d2 <= 0x85) r = 'A'; else if (d2 >= 0xA0 && d2 <= 0xA5) r = 'a';
            else if (d2 >= 0x88 && d2 <= 0x8B) r = 'E'; else if (d2 >= 0xA8 && d2 <= 0xAB) r = 'e';
            else if (d2 >= 0x8C && d2 <= 0x8F) r = 'I'; else if (d2 >= 0xAC && d2 <= 0xAF) r = 'i';
            else if (d2 >= 0x92 && d2 <= 0x96) r = 'O'; else if (d2 >= 0xB2 && d2 <= 0xB6) r = 'o';
            else if (d2 >= 0x99 && d2 <= 0x9C) r = 'U'; else if (d2 >= 0xB9 && d2 <= 0xBC) r = 'u';
            else if (d2 == 0x87) r = 'C'; else if (d2 == 0xA7) r = 'c';
            else if (d2 == 0x91) r = 'N'; else if (d2 == 0xB1) r = 'n';
            else if (d2 == 0x97) r = 'x';                     // multiplication sign
            if (r) dst[o++] = r;
            s += 2; continue;
        }
        if (c == 0xE2 && s[1] == 0x80 && s[2]) {              // general punctuation
            unsigned char d3 = s[2];
            if      (d3 == 0x98 || d3 == 0x99) dst[o++] = '\'';
            else if (d3 == 0x9C || d3 == 0x9D) dst[o++] = '"';
            else if (d3 == 0x93 || d3 == 0x94) dst[o++] = '-';
            else if (d3 == 0xA6 && o < cap - 3) { dst[o++] = '.'; dst[o++] = '.'; dst[o++] = '.'; }
            s += 3; continue;
        }
        s++; while ((*s & 0xC0) == 0x80) s++;                 // unknown: skip the whole codepoint
    }
    dst[o] = 0;
}

// Always false: no animations exist, so no app ever needs to keep ticking for redraws.
bool app_ui_list_animating(void) { return false; }

extern "C" {
#include "nucleo_app.h"
}

int app_ui_title(const char *text, unsigned short accent, const char *right)
{
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, 24, BG);
    d.setTextSize(2); d.setTextColor(accent, BG); d.setCursor(10, top + 2); d.print(text);
    if (right && right[0]) {
        d.setTextSize(1); d.setTextColor(MUTED, BG);
        d.setCursor(238 - (int)strlen(right) * 6, top + 7); d.print(right);
    }
    // Hairline rule with accent underline.
    d.drawFastHLine(10, top + 20, 220, LINE);
    int tw = (int)strlen(text) * 12; if (tw > 220) tw = 220;
    d.fillRect(10, top + 20, tw, 2, accent);
    return top + 24;
}

int app_ui_tabs(int top, const char *const *names, int n, int active, unsigned short accent)
{
    d.fillRect(0, top, 240, 20, BG);
    d.setTextSize(1);   // MUST pin: a leaked larger size from a previous draw would blow up the tabs
    int x = 6;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i] ? names[i] : "";
        int w = (int)strlen(nm) * 6 + 10;
        if (x + w > 236) break;                       // drop overflow rather than clip mid-glyph
        bool on = (i == active);
        d.fillRoundRect(x, top + 2, w, 15, 3, on ? accent : LINE);
        d.setTextColor(on ? INK : MUTED, on ? accent : LINE);
        d.setCursor(x + 5, top + 6); d.print(nm);
        x += w + 4;
    }
    return top + 20;
}

void app_ui_row(int y, int h, const char *label, bool focus, unsigned short accent)
{
    if (focus) d.fillRoundRect(4, y, 232, h - 2, 4, accent);
    d.setTextSize(2);
    d.setTextColor(focus ? INK : MUTED, focus ? accent : BG);
    int ty = y + (h - 16) / 2; if (ty < y) ty = y;    // vertically center size-2 (16px) text
    d.setCursor(14, ty); d.print(label ? label : "");
}

void app_ui_gauge(int y, const char *label, const char *val, int pct, unsigned short col)
{
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(10, y); d.print(label ? label : "");   // bg-backed text overwrites itself, no flicker

    d.fillRect(96, y - 6, 134, 16, BG);                // clear ONLY the value field (handles shrinking text)
    d.setTextSize(2); d.setTextColor(col, BG);
    int vw = (int)strlen(val ? val : "") * 12;
    d.setCursor(230 - vw, y - 6); d.print(val ? val : "");

    d.drawRoundRect(10, y + 12, 220, 5, 2, LINE);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    d.fillRect(11, y + 13, 218, 3, BG);                // clear bar interior so a shrinking bar leaves no trail
    d.fillRoundRect(10, y + 12, 220 * pct / 100, 5, 2, col);
}

void app_ui_list(int top, int h, int count, int sel,
                 app_ui_text_fn label, app_ui_text_fn right, app_ui_color_fn color, void *ud)
{
    d.fillRect(0, top, 240, h, BG);
    d.setClipRect(0, top, 240, h);
    if (count <= 0) { d.clearClipRect(); return; }

    const int STEP = 22;

    // Top-anchored: show 1 item above selection when possible, scroll down as sel moves.
    int vis = h / STEP;
    int scroll = sel - 1;
    if (scroll > count - vis) scroll = count - vis;
    if (scroll < 0) scroll = 0;

    for (int i = scroll; i < count; i++) {
        int y = top + (i - scroll) * STEP + STEP / 2;
        if (y > top + h) break;
        bool focus = (i == sel);
        unsigned short col = color ? color(i, ud) : ACC;
        const char *lab = label(i, ud);
        const char *rt = right ? right(i, ud) : nullptr;

        if (focus) {
            const int ph = STEP - 4;
            d.fillRoundRect(6, y - ph / 2, 240 - 12, ph, ph / 2, col);
            int rx = 240 - 12;
            if (rt && rt[0]) {
                d.setTextSize(1); d.setTextColor(INK, col);
                int w = (int)strlen(rt) * 6; d.setCursor(rx - w, y - 3); d.print(rt); rx -= w + 8;
            }
            int px = 12, avail = rx - px;
            // Truncate label to fit — no marquee, no animation.
            int maxc = avail / 12; if (maxc < 1) maxc = 1;
            char lb[26]; snprintf(lb, sizeof lb, "%.*s", maxc < 25 ? maxc : 25, lab);
            d.setTextSize(2); d.setTextColor(INK, col);
            d.setCursor(px, y - 7); d.print(lb);
        } else {
            // Wear OS hierarchy: the rows ADJACENT to the focus stay big + readable (size 2, white) so
            // you can read what's coming as you scroll; rows further away shrink + dim to give depth.
            // Readability win, zero RAM — all drawn direct to the panel, no buffer.
            bool near = (abs(i - sel) == 1);
            d.fillCircle(11, y, near ? 3 : 2, near ? col : DIM);
            int rx = 233;
            if (rt && rt[0]) {
                int w = (int)strlen(rt) * 6;
                d.setTextSize(1); d.setTextColor(near ? MUTED : DIM, BG); d.setCursor(232 - w, y - 3); d.print(rt);
                rx = 232 - w - 6;
            }
            int tsz = near ? 2 : 1, chw = tsz * 6, ty = near ? y - 7 : y - 3;
            int maxc = (rx - 20) / chw; if (maxc < 1) maxc = 1; if (maxc > 34) maxc = 34;
            char b[36]; snprintf(b, sizeof(b), "%.*s", maxc, lab);
            d.setTextSize(tsz); d.setTextColor(near ? FG : DIM, BG); d.setCursor(20, ty); d.print(b);
        }
    }

    // Scroll indicator knob.
    if (count > vis && vis > 0) {
        int track = h - 8, kh = track * vis / count; if (kh < 10) kh = 10;
        int ky = top + 4 + (track - kh) * sel / (count - 1);
        d.fillRoundRect(236, top + 4, 3, track, 1, LINE);
        d.fillRoundRect(236, ky, 3, kh, 1, color ? color(sel, ud) : ACC);
    }
    d.clearClipRect();
}

// First alphanumeric char of a label, lowercased (skips leading glyphs/spaces). 0 if none.
static char first_alnum(const char *lab)
{
    if (!lab) return 0;
    int p = 0;
    while (lab[p] && !isalnum((unsigned char)lab[p])) p++;
    return lab[p] ? (char)tolower((unsigned char)lab[p]) : 0;
}

// Case-insensitive prefix match of `pre` (len n) against the label's alnum-anchored text.
static bool prefix_match(const char *lab, const char *pre, int n)
{
    if (!lab) return false;
    int p = 0;
    while (lab[p] && !isalnum((unsigned char)lab[p])) p++;
    for (int j = 0; j < n; j++) {
        char c = lab[p + j];
        if (!c || tolower((unsigned char)c) != (unsigned char)pre[j]) return false;
    }
    return true;
}

void app_ui_confirm(const char *title, const char *msg, bool yes_focus)
{
    static const unsigned short DANGER = 0xF9A6;   // warm red for the destructive choice
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    const int cw = 212, chh = 90;
    int cx = (240 - cw) / 2;
    int cy = top + (h - chh) / 2; if (cy < top + 2) cy = top + 2;

    d.fillRoundRect(cx, cy, cw, chh, 9, BG);
    d.drawRoundRect(cx, cy, cw, chh, 9, DANGER);

    d.setTextSize(2); d.setTextColor(DANGER, BG);
    char t[19]; snprintf(t, sizeof t, "%.18s", title ? title : "");
    d.setCursor(cx + 12, cy + 10); d.print(t);

    d.setTextSize(1); d.setTextColor(MUTED, BG);
    char m[35]; snprintf(m, sizeof m, "%.34s", msg ? msg : "");
    d.setCursor(cx + 12, cy + 34); d.print(m);

    const int bw = 90, bh = 24, by = cy + chh - bh - 10;
    int yx = cx + 10, nx = cx + cw - 10 - bw;
    // Yes = destructive (red); No = safe (accent). Focused button is filled.
    d.fillRoundRect(yx, by, bw, bh, 7, yes_focus ? DANGER : BG);
    d.drawRoundRect(yx, by, bw, bh, 7, DANGER);
    const char *yes = TR("Si", "Yes"), *no = "No";
    d.setTextSize(2); d.setTextColor(yes_focus ? INK : DANGER, yes_focus ? DANGER : BG);
    d.setCursor(yx + bw / 2 - (int)strlen(yes) * 6, by + 4); d.print(yes);
    d.fillRoundRect(nx, by, bw, bh, 7, yes_focus ? BG : ACC);
    d.drawRoundRect(nx, by, bw, bh, 7, ACC);
    d.setTextColor(yes_focus ? FG : INK, yes_focus ? BG : ACC);
    d.setCursor(nx + bw / 2 - (int)strlen(no) * 6, by + 4); d.print(no);
}

int app_ui_confirm_key(int key, char ch, bool *yes_focus)
{
    if (!yes_focus) return -1;
    switch (key) {
        case NK_UP: case NK_DOWN: case NK_LEFT: case NK_RIGHT:
            *yes_focus = !*yes_focus; return -1;
        case NK_ENTER:
            return *yes_focus ? 1 : 0;
        default: break;
    }
    if (ch == 'y' || ch == 'Y') return 1;
    if (ch == 'n' || ch == 'N') return 0;
    if (ch == ',' || ch == '/') { *yes_focus = !*yes_focus; return -1; }
    return -1;
}

bool app_ui_list_key(int key, char ch, int *sel, int count, app_ui_text_fn label, void *ud)
{
    if (count <= 0 || !sel || !label) return false;

    // Type-ahead state: a short prefix buffer that decays after a pause, plus same-key cycling.
    static char s_pre[24];
    static int s_len = 0;
    static int64_t s_last = 0;

    if (key == NK_UP) {
        *sel = (*sel + count - 1) % count;
        s_len = 0;
        return true;
    }
    if (key == NK_DOWN) {
        *sel = (*sel + 1) % count;
        s_len = 0;
        return true;
    }
    if (key == NK_CHAR && ch > ' ' && ch < 127) {
        // 1-9: direct jump to the n-th row (launcher-consistent smartwatch shortcut).
        if (ch >= '1' && ch <= '9') {
            int t = ch - '1';
            s_len = 0;
            if (t < count) { *sel = t; return true; }
            return false;
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last > 800000) s_len = 0;   // >0.8s idle -> start a fresh search
        s_last = now;
        char lc = (char)tolower((unsigned char)ch);

        // Same single key tapped again -> cycle to the NEXT item starting with it.
        int start;
        if (s_len == 1 && s_pre[0] == lc) {
            start = 1;                            // skip current, find the next match
        } else {
            if (s_len < (int)sizeof(s_pre) - 1) { s_pre[s_len++] = lc; s_pre[s_len] = 0; }
            start = 0;                            // extend prefix, keep current if it still matches
        }

        for (int i = 0; i < count; i++) {
            int idx = (*sel + start + i) % count;
            const char *lab = label(idx, ud);
            if (s_len == 1 ? first_alnum(lab) == lc : prefix_match(lab, s_pre, s_len)) {
                *sel = idx;
                return true;
            }
        }
        return false;
    }
    return false;
}

// ---- system glyph set (see ui_glyph.h) ------------------------------------------------------------
#include "ui_glyph.h"

#define UGI(v)  ((int)lroundf((float)(v)))
#define UGW(v)  (UGI(v) > 0 ? UGI(v) : 1)        // a stroke is never thinner than one pixel

// Thick line as a filled quad (exact width) — the few diagonals (mute cross).
static void ug_line(LovyanGFX *g, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { g->fillCircle(UGI(x0), UGI(y0), UGI(w / 2), c); return; }
    float px = -dy / len * (w / 2), py = dx / len * (w / 2);
    g->fillTriangle(UGI(x0 + px), UGI(y0 + py), UGI(x0 - px), UGI(y0 - py), UGI(x1 + px), UGI(y1 + py), c);
    g->fillTriangle(UGI(x1 + px), UGI(y1 + py), UGI(x1 - px), UGI(y1 - py), UGI(x0 - px), UGI(y0 - py), c);
}

// Speaker body + cone, shared by UG_SPEAKER and UG_MUTE.
static void ug_speaker(LovyanGFX *g, float cx, float cy, float r, uint16_t col)
{
    g->fillRect(UGI(cx - r * 0.85f), UGI(cy - r * 0.28f), UGI(r * 0.38f), UGI(r * 0.56f) + 1, col);
    g->fillTriangle(UGI(cx - r * 0.55f), UGI(cy - r * 0.28f), UGI(cx), UGI(cy - r * 0.78f), UGI(cx), UGI(cy + r * 0.78f), col);
    g->fillTriangle(UGI(cx - r * 0.55f), UGI(cy - r * 0.28f), UGI(cx), UGI(cy + r * 0.78f), UGI(cx - r * 0.55f), UGI(cy + r * 0.28f), col);
}

void ui_glyph(LovyanGFX *g, int glyph, int cx, int cy, int ri, uint16_t col, uint16_t bg)
{
    if (!g || ri < 3) return;
    const float r = (float)ri, t = (r * 0.26f > 2.0f) ? r * 0.26f : 2.0f;
    switch (glyph) {
    case UG_WIFI: {                                             // dot + two rising arcs (a Wi-Fi fan)
        float by = cy + r * 0.55f;
        g->fillCircle(cx, UGI(by), UGI(t * 0.75f), col);
        g->fillArc(cx, UGI(by), UGI(r * 0.55f), UGI(r * 0.55f + t), 218.0f, 322.0f, col);
        g->fillArc(cx, UGI(by), UGI(r * 1.05f), UGI(r * 1.05f + t), 218.0f, 322.0f, col);
    } break;
    case UG_HOTSPOT:                                            // broadcast: dot + arcs to both sides
        g->fillCircle(cx, cy, UGI(t * 0.85f), col);
        g->fillArc(cx, cy, UGI(r * 0.45f), UGI(r * 0.45f + t * 0.8f), 140.0f, 220.0f, col);
        g->fillArc(cx, cy, UGI(r * 0.45f), UGI(r * 0.45f + t * 0.8f), 320.0f, 400.0f, col);
        g->fillArc(cx, cy, UGI(r * 0.82f), UGI(r * 0.82f + t * 0.8f), 145.0f, 215.0f, col);
        g->fillArc(cx, cy, UGI(r * 0.82f), UGI(r * 0.82f + t * 0.8f), 325.0f, 395.0f, col);
        break;
    case UG_SUN:                                                // disc + 8 ray dots
        g->fillCircle(cx, cy, UGI(r * 0.42f), col);
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785398f;
            g->fillCircle(UGI(cx + cosf(a) * r * 0.84f), UGI(cy + sinf(a) * r * 0.84f), UGW(t * 0.5f), col);
        }
        break;
    case UG_SPEAKER:                                            // speaker + two sound waves
        ug_speaker(g, cx, cy, r, col);
        g->fillArc(cx, cy, UGI(r * 0.35f), UGI(r * 0.35f + t * 0.75f), 305.0f, 415.0f, col);
        g->fillArc(cx, cy, UGI(r * 0.72f), UGI(r * 0.72f + t * 0.75f), 310.0f, 410.0f, col);
        break;
    case UG_MUTE:                                               // speaker + a cross
        ug_speaker(g, cx - r * 0.1f, cy, r, col);
        ug_line(g, cx + r * 0.3f, cy - r * 0.38f, cx + r * 0.95f, cy + r * 0.38f, t * 0.8f, col);
        ug_line(g, cx + r * 0.3f, cy + r * 0.38f, cx + r * 0.95f, cy - r * 0.38f, t * 0.8f, col);
        break;
    case UG_GLOBE:                                              // ring + meridian ellipse + equator
        g->fillCircle(cx, cy, ri, col); g->fillCircle(cx, cy, UGI(r - t * 0.8f), bg);
        g->drawEllipse(cx, cy, UGI(r * 0.42f), ri - 1, col); g->drawEllipse(cx, cy, UGI(r * 0.42f) - 1, ri - 1, col);
        g->fillRect(cx - ri + 1, cy - UGI(t * 0.35f), 2 * ri - 2, UGW(t * 0.7f), col);
        break;
    case UG_CLOCK:                                              // ring + two hands + hub
        g->fillCircle(cx, cy, ri, col); g->fillCircle(cx, cy, UGI(r - t), bg);
        g->fillRect(UGI(cx - t * 0.4f), UGI(cy - r * 0.62f), UGW(t * 0.8f), UGI(r * 0.62f), col);
        g->fillRect(cx, UGI(cy - t * 0.4f), UGI(r * 0.48f), UGW(t * 0.8f), col);
        g->fillCircle(cx, cy, UGW(t * 0.6f), col);
        break;
    case UG_CHIP:                                               // package + die + 3 pins per side
        g->fillRoundRect(UGI(cx - r * 0.62f), UGI(cy - r * 0.62f), UGI(r * 1.24f), UGI(r * 1.24f), 2, col);
        g->fillRect(UGI(cx - r * 0.26f), UGI(cy - r * 0.26f), UGI(r * 0.52f), UGI(r * 0.52f), bg);
        for (int i = -1; i <= 1; i++) {
            int o = UGI(i * r * 0.4f), pw = UGW(t * 0.6f), pl = UGI(r * 0.3f);
            g->fillRect(cx + o - pw / 2, cy - ri, pw, pl, col); g->fillRect(cx + o - pw / 2, cy + ri - pl, pw, pl, col);
            g->fillRect(cx - ri, cy + o - pw / 2, pl, pw, col); g->fillRect(cx + ri - pl, cy + o - pw / 2, pl, pw, col);
        }
        break;
    case UG_RESET: {                                            // open ring + arrow head (restore / restart)
        float R = r * 0.84f, a = 300.0f * 0.0174533f;
        g->fillArc(cx, cy, UGI(R - t), UGI(R), 20.0f, 300.0f, col);
        float rm = R - t / 2, px = cx + cosf(a) * rm, py = cy + sinf(a) * rm;
        float tx = -sinf(a), ty = cosf(a), nx = cosf(a), ny = sinf(a), h = t * 1.5f;
        g->fillTriangle(UGI(px + tx * h * 1.2f), UGI(py + ty * h * 1.2f),
                        UGI(px + nx * h - tx * h * 0.3f), UGI(py + ny * h - ty * h * 0.3f),
                        UGI(px - nx * h - tx * h * 0.3f), UGI(py - ny * h - ty * h * 0.3f), col);
    } break;
    case UG_POWER: {                                            // ring open at the top + a bar
        float R = r * 0.86f;
        g->fillArc(cx, cy, UGI(R - t), UGI(R), 305.0f, 595.0f, col);
        g->fillRect(UGI(cx - t * 0.45f), cy - ri, UGW(t * 0.9f), UGI(r * 0.95f), col);
    } break;
    case UG_MOON:                                               // crescent
        g->fillCircle(UGI(cx - r * 0.08f), cy, UGI(r * 0.86f), col);
        g->fillCircle(UGI(cx + r * 0.34f), UGI(cy - r * 0.3f), UGI(r * 0.68f), bg);
        break;
    case UG_TORCH:                                              // flared head + body + switch
        g->fillTriangle(UGI(cx - r * 0.66f), UGI(cy - r * 0.86f), UGI(cx + r * 0.66f), UGI(cy - r * 0.86f), UGI(cx + r * 0.3f), UGI(cy - r * 0.22f), col);
        g->fillTriangle(UGI(cx - r * 0.66f), UGI(cy - r * 0.86f), UGI(cx + r * 0.3f), UGI(cy - r * 0.22f), UGI(cx - r * 0.3f), UGI(cy - r * 0.22f), col);
        g->fillRoundRect(UGI(cx - r * 0.3f), UGI(cy - r * 0.3f), UGI(r * 0.6f), UGI(r * 1.2f), 2, col);
        g->fillRect(UGI(cx - t * 0.35f), UGI(cy + r * 0.05f), UGW(t * 0.7f), UGI(r * 0.32f), bg);
        break;
    case UG_GEAR:                                               // body + 8 teeth + hub hole
        g->fillCircle(cx, cy, UGI(r * 0.66f), col);
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785398f; int s = UGI(t * 1.3f);
            g->fillRect(UGI(cx + cosf(a) * r * 0.78f) - s / 2, UGI(cy + sinf(a) * r * 0.78f) - s / 2, s, s, col);
        }
        g->fillCircle(cx, cy, UGI(r * 0.27f), bg);
        break;
    case UG_KEYBOARD: {                                         // board + 2 key rows + space bar
        g->fillRoundRect(cx - ri, UGI(cy - r * 0.62f), 2 * ri, UGI(r * 1.24f), 2, col);
        int k = UGW(t * 0.62f);
        for (int ry = 0; ry < 2; ry++) for (int c = 0; c < 4; c++)
            g->fillRect(UGI(cx - r * 0.7f + c * r * 0.46f), UGI(cy - r * 0.34f + ry * r * 0.4f), k, k, bg);
        g->fillRect(UGI(cx - r * 0.42f), UGI(cy + r * 0.3f), UGI(r * 0.84f), k, bg);
    } break;
    case UG_USB:                                                // plug head + body with a slot
        g->fillRect(UGI(cx - t), cy - ri, UGI(2 * t), UGI(r * 0.4f), col);
        g->fillRoundRect(UGI(cx - r * 0.5f), UGI(cy - r * 0.62f), ri, UGI(r * 1.62f), 2, col);
        g->fillRect(UGI(cx - r * 0.5f), UGI(cy + r * 0.12f), ri, UGW(t * 0.55f), bg);
        break;
    case UG_MONITOR:                                            // screen + stand (web client)
        g->fillRoundRect(cx - ri, UGI(cy - r * 0.78f), 2 * ri, UGI(r * 1.26f), 2, col);
        g->fillRect(UGI(cx - r + t * 0.75f), UGI(cy - r * 0.78f + t * 0.75f), UGI(2 * r - t * 1.5f), UGI(r * 1.26f - t * 1.5f), bg);
        g->fillRect(UGI(cx - t * 0.4f), UGI(cy + r * 0.48f), UGW(t * 0.8f), UGI(r * 0.3f), col);
        g->fillRect(UGI(cx - r * 0.5f), UGI(cy + r * 0.76f), ri, UGW(t * 0.6f), col);
        break;
    case UG_PALETTE:                                            // half-filled disc (theme)
        g->fillCircle(cx, cy, UGI(r * 0.9f), col);
        g->fillRect(cx + 1, cy - ri, ri, 2 * ri + 1, bg);
        g->drawCircle(cx, cy, UGI(r * 0.9f), col); g->drawCircle(cx, cy, UGI(r * 0.9f) - 1, col);
        break;
    case UG_MIC:                                                // capsule + holder + stand
        g->fillRoundRect(UGI(cx - r * 0.3f), cy - ri, UGI(r * 0.6f), UGI(r * 1.2f), UGI(r * 0.3f), col);
        g->fillArc(cx, UGI(cy - r * 0.05f), UGI(r * 0.55f), UGI(r * 0.55f + t * 0.7f), 15.0f, 165.0f, col);
        g->fillRect(UGI(cx - t * 0.35f), UGI(cy + r * 0.5f), UGW(t * 0.7f), UGI(r * 0.35f), col);
        g->fillRect(UGI(cx - r * 0.42f), UGI(cy + r * 0.82f), UGI(r * 0.84f), UGW(t * 0.55f), col);
        break;
    case UG_STAR: {                                             // five-point star (preferred)
        float ox[5], oy[5], ix[5], iy[5];
        for (int k = 0; k < 5; k++) {
            float a = (-90.0f + 72.0f * k) * 0.0174533f, b = a + 36.0f * 0.0174533f;
            ox[k] = cx + cosf(a) * r; oy[k] = cy + sinf(a) * r;
            ix[k] = cx + cosf(b) * r * 0.42f; iy[k] = cy + sinf(b) * r * 0.42f;
        }
        for (int k = 0; k < 5; k++) {
            int p = (k + 4) % 5;
            g->fillTriangle(UGI(ox[k]), UGI(oy[k]), UGI(ix[p]), UGI(iy[p]), UGI(ix[k]), UGI(iy[k]), col);
            g->fillTriangle(cx, cy, UGI(ix[p]), UGI(iy[p]), UGI(ix[k]), UGI(iy[k]), col);
        }
    } break;
    case UG_LOCK:                                               // shackle + body + keyhole
        g->fillArc(cx, UGI(cy - r * 0.12f), UGI(r * 0.36f), UGI(r * 0.36f + t * 0.75f), 180.0f, 360.0f, col);
        g->fillRect(UGI(cx - r * 0.36f - t * 0.75f), UGI(cy - r * 0.12f), UGW(t * 0.75f), UGI(r * 0.2f), col);
        g->fillRect(UGI(cx + r * 0.36f), UGI(cy - r * 0.12f), UGW(t * 0.75f), UGI(r * 0.2f), col);
        g->fillRoundRect(UGI(cx - r * 0.62f), UGI(cy - r * 0.05f), UGI(r * 1.24f), UGI(r * 0.95f), 2, col);
        g->fillCircle(cx, UGI(cy + r * 0.38f), UGW(t * 0.45f), bg);
        break;
    case UG_UPDATE: {                                           // down arrow into a tray
        int w = UGW(t * 0.7f);
        g->fillRect(UGI(cx - t * 0.45f), cy - ri, UGW(t * 0.9f), UGI(r * 0.95f), col);
        g->fillTriangle(UGI(cx - r * 0.55f), UGI(cy - r * 0.1f), UGI(cx + r * 0.55f), UGI(cy - r * 0.1f), cx, UGI(cy + r * 0.5f), col);
        g->fillRect(cx - ri, UGI(cy + r * 0.72f), 2 * ri, w, col);
        g->fillRect(cx - ri, UGI(cy + r * 0.3f), w, UGI(r * 0.5f), col);
        g->fillRect(cx + ri - w, UGI(cy + r * 0.3f), w, UGI(r * 0.5f), col);
    } break;
    case UG_KEY:                                                // bow ring + shaft + two bits (pairing PIN)
        g->fillCircle(UGI(cx - r * 0.45f), cy, UGI(r * 0.48f), col);
        g->fillCircle(UGI(cx - r * 0.45f), cy, UGW(r * 0.2f), bg);
        g->fillRect(UGI(cx - r * 0.05f), UGI(cy - t * 0.4f), ri, UGW(t * 0.8f), col);
        g->fillRect(UGI(cx + r * 0.52f), cy, UGW(t * 0.7f), UGI(r * 0.38f), col);
        g->fillRect(UGI(cx + r * 0.8f), cy, UGW(t * 0.7f), UGI(r * 0.28f), col);
        break;
    case UG_CHECK:                                              // tick (confirmation)
        ug_line(g, cx - r * 0.72f, cy + r * 0.02f, cx - r * 0.2f, cy + r * 0.55f, t * 1.1f, col);
        ug_line(g, cx - r * 0.2f, cy + r * 0.55f, cx + r * 0.8f, cy - r * 0.55f, t * 1.1f, col);
        break;
    case UG_SEARCH:                                             // magnifier: ring + handle
        g->fillCircle(UGI(cx - r * 0.2f), UGI(cy - r * 0.2f), UGI(r * 0.6f), col);
        g->fillCircle(UGI(cx - r * 0.2f), UGI(cy - r * 0.2f), UGI(r * 0.6f - t), bg);
        ug_line(g, cx + r * 0.22f, cy + r * 0.22f, cx + r * 0.85f, cy + r * 0.85f, t * 1.1f, col);
        break;
    case UG_BELL:                                               // bell: dome + rim + clapper
        g->fillRoundRect(UGI(cx - r * 0.62f), UGI(cy - r * 0.7f), UGI(r * 1.24f), UGI(r * 1.2f), UGI(r * 0.6f), col);
        g->fillRect(UGI(cx - r * 0.85f), UGI(cy + r * 0.34f), UGI(r * 1.7f), UGW(t * 0.7f), col);
        g->fillCircle(cx, UGI(cy + r * 0.72f), UGW(t * 0.6f), col);
        break;
    case UG_BATTERY: {                                          // cell body + terminal, two-thirds full
        int bw = UGI(r * 1.6f), bh = UGI(r * 0.95f), bx = cx - bw / 2 - 1, by = cy - bh / 2;
        g->fillRoundRect(bx, by, bw, bh, 2, col);
        g->fillRect(bx + bw, cy - bh / 4, UGW(t * 0.6f), bh / 2, col);
        g->fillRect(bx + UGW(t * 0.6f) + UGI(bw * 0.6f), by + UGW(t * 0.6f), bw - 2 * UGW(t * 0.6f) - UGI(bw * 0.6f), bh - 2 * UGW(t * 0.6f), bg);
    } break;
    case UG_BLUETOOTH:                                          // the rune: spine + two chevrons
        ug_line(g, cx, cy - r, cx, cy + r, t * 0.8f, col);
        ug_line(g, cx, cy - r, cx + r * 0.55f, cy - r * 0.45f, t * 0.8f, col);
        ug_line(g, cx + r * 0.55f, cy - r * 0.45f, cx - r * 0.55f, cy + r * 0.45f, t * 0.8f, col);
        ug_line(g, cx, cy + r, cx + r * 0.55f, cy + r * 0.45f, t * 0.8f, col);
        ug_line(g, cx + r * 0.55f, cy + r * 0.45f, cx - r * 0.55f, cy - r * 0.45f, t * 0.8f, col);
        break;
    case UG_INFO:                                               // (i)
        g->fillCircle(cx, cy, ri, col);
        g->fillRect(UGI(cx - t * 0.45f), UGI(cy - r * 0.15f), UGW(t * 0.9f), UGI(r * 0.65f), bg);
        g->fillCircle(cx, UGI(cy - r * 0.48f), UGW(t * 0.55f), bg);
        break;
    case UG_NEXT:                                               // chevron right
        g->fillTriangle(UGI(cx - r * 0.35f), UGI(cy - r * 0.6f), UGI(cx - r * 0.35f), UGI(cy + r * 0.6f), UGI(cx + r * 0.4f), cy, col);
        break;
    case UG_PREV:                                               // chevron left
        g->fillTriangle(UGI(cx + r * 0.35f), UGI(cy - r * 0.6f), UGI(cx + r * 0.35f), UGI(cy + r * 0.6f), UGI(cx - r * 0.4f), cy, col);
        break;
    default:
        g->fillCircle(cx, cy, UGI(r * 0.4f), col);
        break;
    }
}
#undef UGW
#undef UGI
