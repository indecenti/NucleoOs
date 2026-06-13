// Implementation of the shared focused-list widget (see app_ui.h).
// Static-only: rows snap to position instantly and long labels are truncated to fit (no scrolling
// text), so there is nothing to animate. Apps redraw only on key input — zero redraws when idle.
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, LINE = 0x2945, INK = 0x0000, ACC = 0x4D1F;

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

    const int STEP = 26;
    int center = top + h / 2;

    for (int i = 0; i < count; i++) {
        int y = center + (i - sel) * STEP;
        if (y < top - STEP || y > top + h + STEP) continue;
        bool focus = (i == sel);
        unsigned short col = color ? color(i, ud) : ACC;
        const char *lab = label(i, ud);
        const char *rt = right ? right(i, ud) : nullptr;

        if (focus) {
            const int ph = 22;
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
            bool far = abs(i - sel) > 1;
            unsigned short dim = far ? DIM : MUTED;
            d.fillCircle(11, y, 2, far ? DIM : col);
            int rx = 233;
            if (rt && rt[0]) {
                int w = (int)strlen(rt) * 6;
                d.setTextSize(1); d.setTextColor(dim, BG); d.setCursor(232 - w, y - 3); d.print(rt);
                rx = 232 - w - 6;
            }
            int maxc = (rx - 18) / 6; if (maxc < 1) maxc = 1; if (maxc > 34) maxc = 34;
            char b[36]; snprintf(b, sizeof(b), "%.*s", maxc, lab);
            d.setTextSize(1); d.setTextColor(dim, BG); d.setCursor(18, y - 3); d.print(b);
        }
    }

    // Scroll indicator knob.
    int vis = h / STEP;
    if (count > vis && vis > 0) {
        int track = h - 8, kh = track * vis / count; if (kh < 10) kh = 10;
        int ky = top + 4 + (track - kh) * sel / (count - 1);
        d.fillRoundRect(236, top + 4, 3, track, 1, LINE);
        d.fillRoundRect(236, ky, 3, kh, 1, color ? color(sel, ud) : ACC);
    }
    d.clearClipRect();
}

bool app_ui_list_key(int key, char ch, int *sel, int count, app_ui_text_fn label, void *ud)
{
    if (count <= 0 || !sel || !label) return false;

    if (key == NK_UP) {
        *sel = (*sel + count - 1) % count;
        return true;
    }
    if (key == NK_DOWN) {
        *sel = (*sel + 1) % count;
        return true;
    }
    if (key == NK_CHAR && ch > ' ' && ch < 127) {
        char lower_ch = (char)tolower((unsigned char)ch);
        for (int i = 1; i <= count; i++) {
            int idx = (*sel + i) % count;
            const char *lab = label(idx, ud);
            if (!lab) continue;
            int p = 0;
            while (lab[p] && !isalnum((unsigned char)lab[p])) p++;
            if (lab[p] && tolower((unsigned char)lab[p]) == lower_ch) {
                *sel = idx;
                return true;
            }
        }
        return false;
    }
    return false;
}
