// nucleo_ui_modal.cpp — the BLOCKING modals: message, home, menu, text input (the first-boot wizard and
// a few system prompts). Split out of nucleo_ui.cpp so the host renderer (tools/ui-host) can draw them with
// the real code: they only need the panel `d`, the shared back-buffer API (nucleo_screen) and the keyboard.
#include "nucleo_ui.h"
#include "nucleo_kbd.h"
#include "nucleo_theme.h"
#include <M5GFX.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

extern M5GFX d;                                 // nucleo_ui.cpp: the panel
M5Canvas *nucleo_screen(void);                  // nucleo_ui.cpp: the shared back-buffer, or NULL (re-acquires lazily)
bool nucleo_screen_acquire(void);

static const int W = 240, H = 135;
#define BG THEME_BG
#define ACC THEME_ACC
#define FG THEME_FG
#define MUT THEME_MUTED
#define LINE THEME_LINE

// The modals (first-boot wizard, messages, menus, text fields) wear the SAME chrome as the rest of the OS:
// an 18 px header band — accent title at size 2 + hairline — like Settings, and the launcher's hint bar —
// hairline + centred, muted, localized hint. They used to have their own: a 26 px header, list items at
// size 1 and "[;/.] move [enter] ok [`] back" in English on every language.
static const int MHDR = 18, MHINT = 14;
static void header(LovyanGFX *canvas, const char *title)
{
    nucleo_theme_draw_bg(canvas, W, H);
    canvas->setTextColor(ACC, BG);
    canvas->setTextSize(2);
    canvas->setCursor(6, 1);
    canvas->print(title);
    canvas->drawFastHLine(0, MHDR - 1, W, LINE);
    canvas->setTextSize(1);
}

static const char *s_hint = "";   // the last hint drawn: present() repaints it below a short back-buffer
static void hint(LovyanGFX *canvas, const char *h)
{
    s_hint = h ? h : "";
    int y = H - MHINT;
    canvas->fillRect(0, y, W, MHINT, BG);
    canvas->drawFastHLine(0, y, W, LINE);
    canvas->setTextColor(MUT, BG);
    canvas->setTextSize(1);
    int len = (int)strlen(h); if (len > 39) len = 39;
    int x = (W - len * 6) / 2; if (x < 4) x = 4;
    canvas->setCursor(x, y + 4);
    canvas->print(h);
}

// The UI language, when the i18n engine is linked in (nucleo_storage): a weak reference keeps this
// component free of that dependency. NULL -> English.
extern "C" const char *nucleo_i18n_lang(void) __attribute__((weak));
static const char *H5(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang ? nucleo_i18n_lang() : nullptr;
    if (!l) return en;
    if (l[0] == 'i' && l[1] == 't') return it;
    if (l[0] == 'e' && l[1] == 's') return es;
    if (l[0] == 'f' && l[1] == 'r') return fr;
    if (l[0] == 'd' && l[1] == 'e') return de;
    return en;
}

// Draw the body text of a message/home screen with READABLE, self-fitting fonts. With a handful of
// lines (<=4) each is drawn as large as it fits (size 2) so short prompts and confirmations are easy
// to read on the 240x135 panel; any line too wide for size 2 (URLs, long technical strings) auto-drops
// to size 1 so it is never clipped. Dense screens (>4 lines) stay compact at size 1 to avoid overflow.
// An empty string is a vertical spacer. Caller sets the text colour; header()/hint() own the chrome.
static void draw_body(LovyanGFX &canvas, const char *const *lines, int n)
{
    bool big = (n <= 4);
    int y = MHDR + 8;
    for (int i = 0; i < n; i++) {
        const char *ln = lines[i] ? lines[i] : "";
        if (!ln[0]) { y += big ? 10 : 8; continue; }        // blank line = spacer
        // Size 2 when it fits; else Font2 (16 px proportional, Settings' reading face) — a long line used to
        // fall straight to size 1 (6 px glyphs) and the welcome screen mixed two sizes.
        if (big) {
            canvas.setTextSize(2);
            if (canvas.textWidth(ln) <= W - 16) { canvas.setCursor(8, y); canvas.print(ln); y += 20; continue; }
            canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
            if (canvas.textWidth(ln) <= W - 16) { canvas.setCursor(8, y + 1); canvas.print(ln); canvas.setFont(&fonts::Font0); y += 20; continue; }
            canvas.setFont(&fonts::Font0);
        }
        canvas.setTextSize(1);
        canvas.setCursor(8, y);
        canvas.print(ln);
        y += 13;
    }
}

// Where a blocking modal (the first-boot wizard, a message, a menu, a text field) draws. In order: the
// shared back-buffer (re-acquired if it was dropped), else a private 8-bpp sprite (32 KB — the old 16-bpp
// one needed ~64 KB, never there once Wi-Fi + httpd are up), else the PANEL itself. The old code drew into
// a sprite whose createSprite() had FAILED unchecked: every draw was a no-op and the first-boot wizard sat
// on a BLACK screen waiting for keys (ADV under M5Launcher after a factory reset, 2026-10-01).
// When set, modals draw DIRECT to the panel and never touch a back-buffer. The boot-window SD-content
// installer sets this so drawing its progress bar can't re-allocate the 32 KB shared canvas it just freed
// for the TLS handshake (the re-acquire was eating the very RAM the download needs).
static bool s_modal_direct = false;
extern "C" void nucleo_ui_modal_direct(bool on) { s_modal_direct = on; }

struct ModalSurface {
    M5Canvas local;
    LovyanGFX *g;
    bool sprite;
    ModalSurface() : local(&d), g(&d), sprite(false)
    {
        if (s_modal_direct) return;                 // direct to the panel: keep every byte free for TLS
        M5Canvas *sh = nucleo_screen();
        if (sh || (nucleo_screen_acquire() && (sh = nucleo_screen()))) { g = sh; sprite = true; return; }
        local.setPsram(false);
        local.setColorDepth(8);
        if (local.createSprite(W, H)) { g = &local; sprite = true; return; }
        ESP_LOGW("ui", "modal: no RAM for a back-buffer, drawing direct to the panel");
    }
    ~ModalSurface() { if (g == &local) local.deleteSprite(); }
    // Show the frame. A sprite goes up in one push (and the DMA is waited for: the next frame refills it);
    // direct frames are already on the panel. The shared back-buffer can be SHORTER than the panel — on the
    // ADV it is fitted to the largest block (~130 of 135 rows) — so the rows it lacks, where the hint bar
    // lives, are painted straight onto the panel under a clip; without this the hint was cut in half.
    void present()
    {
        if (!sprite) return;
        M5Canvas *cv = static_cast<M5Canvas *>(g);
        cv->pushSprite(0, 0);
        d.waitDMA();
        int ch = cv->height();
        if (ch < H) {
            d.setClipRect(0, ch, W, H - ch);
            d.fillRect(0, ch, W, H - ch, BG);
            hint(&d, s_hint);
            d.clearClipRect();
        }
    }
};
// The modals redraw only when what they show changes (key, cursor blink, marquee step): the old loops
// re-rendered and pushed the whole 240x135 frame every 15 ms while waiting for a key — battery for nothing,
// and on the direct path a visible flicker.
static void modal_idle(void)
{
    esp_task_wdt_reset();   // a slow typist/idle dialog must not trip the 8s task WDT (no-op if unwatched)
    vTaskDelay(pdMS_TO_TICKS(15));
}

extern "C" void nucleo_ui_message(const char *title, const char *const *lines, int n)
{
    ModalSurface s;
    header(s.g, title);
    s.g->setTextColor(FG, BG);
    draw_body(*s.g, lines, n);
    hint(s.g, H5("invio continua", "enter continue", "enter continuar", "enter continuer", "enter weiter"));
    s.present();
    for (;;) {
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_ENTER) break;
        modal_idle();
    }
}

extern "C" void nucleo_ui_home(const char *title, const char *const *lines, int n)
{
    s_hint = "";                                   // no hint bar here: present() must not repaint a stale one
    ModalSurface s;
    header(s.g, title);
    s.g->setTextColor(FG, BG);
    draw_body(*s.g, lines, n);
    s.present();
}


// A one-shot progress screen (no key loop): header, one status line, a filled bar with the percent.
// Used by the boot-window SD-content installer, which repaints it per file. pct < 0 shows an indeterminate
// bar (a moving block). Safe to call from the pre-httpd boot window (same surfaces as every modal).
extern "C" void nucleo_ui_progress(const char *title, const char *line, int pct)
{
    s_hint = "";
    ModalSurface s;
    LovyanGFX &g = *s.g;
    header(&g, title);
    g.setTextColor(FG, BG);
    g.setFont(&fonts::Font0); g.setTextSize(1);
    if (line && line[0]) { g.setCursor(8, MHDR + 10); g.print(line); }
    const int bx = 8, bw = W - 16, by = 70, bh = 18;
    g.drawRoundRect(bx, by, bw, bh, 4, LINE);
    if (pct < 0) {                                   // indeterminate: a block that walks with the frame count
        static int walk = 0; walk = (walk + 12) % (bw - 40);
        g.fillRoundRect(bx + 2 + walk, by + 2, 36, bh - 4, 3, ACC);
    } else {
        int p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        int fw = (bw - 4) * p / 100;
        if (fw > 0) g.fillRoundRect(bx + 2, by + 2, fw, bh - 4, 3, ACC);
        char pc[8]; snprintf(pc, sizeof pc, "%d%%", p);
        g.setTextColor(FG, BG); g.setCursor(bx + bw / 2 - 10, by + bh + 6); g.print(pc);
    }
    s.present();
}

extern "C" int nucleo_ui_menu(const char *title, const char *const *items, int n)
{
    // Settings' list look: every item at size 2 (the old unfocused items were size 1, 6 px glyphs), the
    // focused one in the accent chip with the theme ink. The view keeps the focus in sight with a row
    // of context; a back-buffer glides, the direct path snaps (no in-place repaint per eased frame).
    const int ROW = 24, TOP = MHDR + 3, AREA = H - MHINT - TOP;
    int sel = 0;
    float smooth_y = 0.0f;
    uint32_t frame = 0;
    int last_y = -1, last_sel = -1, last_off = -1;

    ModalSurface s;
    LovyanGFX &canvas = *s.g;
    const bool ease = s.sprite;

    for (;;) {
        int span = n * ROW - AREA; if (span < 0) span = 0;
        float target_y = (float)(sel * ROW - ROW); if (target_y > span) target_y = (float)span; if (target_y < 0) target_y = 0;
        smooth_y = ease ? smooth_y + (target_y - smooth_y) * 0.3f : target_y;
        if (fabsf(target_y - smooth_y) < 0.5f) smooth_y = target_y;
        canvas.setTextSize(2);
        int tw_sel = (sel >= 0 && sel < n) ? (int)canvas.textWidth(items[sel]) : 0;
        int off = tw_sel > W - 28 ? (int)((frame / 2) % (uint32_t)(tw_sel + 40)) : 0;   // marquee step of a long item
        if ((int)smooth_y == last_y && sel == last_sel && off == last_off) {         // nothing visible changed
            nucleo_key_t k = nucleo_kbd_read();
            if (k.key == NK_UP) { sel = (sel + n - 1) % n; frame = 0; }
            else if (k.key == NK_DOWN) { sel = (sel + 1) % n; frame = 0; }
            else if (k.key == NK_ENTER) return sel;
            else if (k.key == NK_BACK) return -1;
            frame++;
            modal_idle();
            continue;
        }
        last_y = (int)smooth_y; last_sel = sel; last_off = off;
        header(&canvas, title);
        canvas.setClipRect(0, TOP, W, AREA);
        for (int i = 0; i < n; i++) {
            int y = TOP + i * ROW - (int)smooth_y;
            if (y + ROW <= TOP || y >= TOP + AREA) continue;
            canvas.setTextSize(2);
            if (i == sel) {
                canvas.fillRoundRect(3, y, W - 8, ROW - 2, 9, ACC);
                canvas.setTextColor(THEME_INK, ACC);
                int tw = (int)canvas.textWidth(items[i]);
                if (tw > W - 28) {                                          // too long: marquee inside the chip
                    canvas.setClipRect(8, y, W - 18, ROW - 2);
                    canvas.setCursor(10 - off, y + 4); canvas.print(items[i]);
                    canvas.setCursor(10 - off + tw + 40, y + 4); canvas.print(items[i]);
                    canvas.setClipRect(0, TOP, W, AREA);
                } else { canvas.setCursor(12, y + 4); canvas.print(items[i]); }
            } else {
                canvas.setTextColor(FG, BG);
                canvas.setCursor(12, y + 4);
                canvas.print(items[i]);
            }
        }
        canvas.clearClipRect();
        if (span > 0) {                                                     // scrollbar, like Settings
            int kh = AREA * AREA / (n * ROW); if (kh < 8) kh = 8;
            int ky = TOP + (AREA - kh) * (int)smooth_y / span;
            canvas.fillRect(W - 3, TOP, 2, AREA, LINE);
            canvas.fillRect(W - 3, ky, 2, kh, ACC);
        }
        hint(&canvas, H5("su/giu scegli   invio ok", "up/dn choose   enter ok", "arr/ab elegir   enter ok",
                         "haut/bas choisir   enter ok", "auf/ab waehlen   enter ok"));
        s.present();

        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_UP) { sel = (sel + n - 1) % n; frame = 0; }
        else if (k.key == NK_DOWN) { sel = (sel + 1) % n; frame = 0; }
        else if (k.key == NK_ENTER) return sel;
        else if (k.key == NK_BACK) return -1;

        frame++;
        modal_idle();
    }
}

extern "C" void nucleo_ui_input(const char *title, char *buf, int len, int masked)
{
    // Settings' text field: a rounded LINE box with an accent edge, the value at size 2 (the tail of a long
    // value scrolls in), a steady caret. A masked value shows the character just typed for 1.5 s and TAB
    // shows / hides all of it — nothing is typed blind.
    ModalSurface s;
    LovyanGFX &canvas = *s.g;
    int pos = (int)strlen(buf);
    int last_pos = -1, last_blink = -1, last_peek = -1, last_rev = -1, last_latch = -1; uint32_t last_sum = 0;
    bool reveal = false; int64_t key_us = 0;
    const int FY = MHDR + 18, FH = 28, MAXC = (W - 48) / 12 - 1;   // room for the Aa badge

    for (;;) {
        int64_t now = esp_timer_get_time();
        int blink = (int)((now / 500000) % 2);
        int peek = (masked && !reveal && key_us && now - key_us < 1500000) ? 1 : 0;
        uint32_t sum = 2166136261u; for (int i = 0; i < pos; i++) sum = (sum ^ (uint8_t)buf[i]) * 16777619u;
        if (pos == last_pos && blink == last_blink && sum == last_sum && peek == last_peek && (int)reveal == last_rev) {
            nucleo_key_t k = nucleo_kbd_read();
            if (k.key == NK_ENTER || k.key == NK_BACK) { buf[pos] = '\0'; break; }
            else if (k.key == NK_DEL) { if (pos > 0) buf[--pos] = '\0'; }
            else if (k.key == NK_TAB && masked) reveal = !reveal;
            else if (k.ch >= 32 && pos < len - 1) { buf[pos++] = nucleo_kbd_take_shift_tap() ? nucleo_kbd_shifted(k.ch) : k.ch; buf[pos] = '\0'; key_us = esp_timer_get_time(); }
            if (nucleo_kbd_shift_latched() != (last_latch == 1)) last_sum ^= 1u;   // repaint the Aa badge
            modal_idle();
            continue;
        }
        last_pos = pos; last_blink = blink; last_sum = sum; last_peek = peek; last_rev = reveal;
        last_latch = nucleo_kbd_shift_latched() ? 1 : 0;
        header(&canvas, title);
        canvas.fillRoundRect(4, FY, W - 8, FH, 7, LINE);
        canvas.drawRoundRect(4, FY, W - 8, FH, 7, ACC);
        int from = pos > MAXC ? pos - MAXC : 0, k = 0;
        char sh[24];
        for (int i = from; i < pos && k < MAXC && k < (int)sizeof sh - 1; i++, k++)
            sh[k] = (masked && !reveal && !(peek && i == pos - 1)) ? '*' : buf[i];
        sh[k] = 0;
        canvas.setTextSize(2);
        canvas.setTextColor(FG, LINE);
        canvas.setCursor(12, FY + 6);
        canvas.print(sh);
        if (blink == 0) canvas.fillRect(12 + k * 12 + 1, FY + 6, 2, 16, ACC);   // caret
        if (last_latch == 1) { canvas.fillRoundRect(W - 32, FY + 6, 22, 15, 4, ACC); canvas.setTextSize(1);
                               canvas.setTextColor(THEME_INK, ACC); canvas.setCursor(W - 27, FY + 10); canvas.print("Aa"); }
        if (masked)
            hint(&canvas, reveal ? H5("invio ok  tab nascondi  esc annulla", "enter ok  tab hide  esc cancel", "enter ok  tab ocultar  esc anular",
                                      "enter ok  tab cacher  esc annuler", "enter ok  tab verbergen  esc Abbr.")
                                 : H5("invio ok  tab mostra  esc annulla", "enter ok  tab show  esc cancel", "enter ok  tab mostrar  esc anular",
                                      "enter ok  tab voir  esc annuler", "enter ok  tab zeigen  esc Abbr."));
        else hint(&canvas, H5("invio ok   canc cancella", "enter ok   del erase", "enter ok   del borrar", "enter ok   del effacer", "enter ok   del loeschen"));
        s.present();

        nucleo_key_t kk = nucleo_kbd_read();
        if (kk.key == NK_ENTER || kk.key == NK_BACK) { buf[pos] = '\0'; break; }
        else if (kk.key == NK_DEL) { if (pos > 0) buf[--pos] = '\0'; }
        else if (kk.key == NK_TAB && masked) reveal = !reveal;
        else if (kk.ch >= 32 && pos < len - 1) { buf[pos++] = nucleo_kbd_take_shift_tap() ? nucleo_kbd_shifted(kk.ch) : kk.ch; buf[pos] = '\0'; key_us = esp_timer_get_time(); }

        modal_idle();
    }
}
