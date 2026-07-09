// Implementation of the shared focused-list widget (see app_ui.h).
// Static-only: rows snap to position instantly and long labels are truncated to fit (no scrolling
// text), so there is nothing to animate. Apps redraw only on key input — zero redraws when idle.
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_timer.h"

#include "app_gfx.h"
#include "nucleo_theme.h"
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
    d.setTextSize(2); d.setTextColor(yes_focus ? INK : DANGER, yes_focus ? DANGER : BG);
    d.setCursor(yx + bw / 2 - 18, by + 4); d.print("Yes");
    d.fillRoundRect(nx, by, bw, bh, 7, yes_focus ? BG : ACC);
    d.drawRoundRect(nx, by, bw, bh, 7, ACC);
    d.setTextColor(yes_focus ? FG : INK, yes_focus ? BG : ACC);
    d.setCursor(nx + bw / 2 - 12, by + 4); d.print("No");
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
