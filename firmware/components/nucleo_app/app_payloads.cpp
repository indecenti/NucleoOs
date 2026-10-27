// app_payloads.cpp — Security > Payloads: a DuckyScript runner that injects keystrokes over USB HID
// (cable) OR BLE HID (wireless). FOR AUTHORIZED TESTING ONLY. Beyond Bruce's runner: a DRY-RUN PREVIEW
// (validate actions + estimated time + syntax errors before anything fires) and a US/IT keyboard LAYOUT
// selector. Payloads come from /sd/data/ducky/*.txt plus a few built-in demos.
//
// The run is a self-contained modal loop (like app_usbkbd): it owns the keyboard for live progress +
// abort (backtick), and feeds the task watchdog across DELAYs so a long payload can't reboot the device.
#include "nucleo_app.h"
#include "app_gfx.h"
#include "nucleo_i18n.h"       // TR(it,en): hint follows the system language
#include "app_large.h"         // two screens: payload list + preview details on the panel, Cardputer companion
#include "nucleo_touch.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
extern "C" {
#include "nucleo_ducky.h"
#include "nucleo_usbhid.h"
#include "nucleo_ble.h"
#include "nucleo_exclusive.h"
#include "nucleo_kbd.h"
}

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x4DDF, GRN = 0x8FF3, WARN = 0xFE8C, HL = 0x12B2;

#define DUCKY_DIR "/sd/data/ducky"
#define SCRIPT_MAX 4096

enum Scr { LIST, PREVIEW };
struct Item { char name[40]; bool builtin; int bi; };
#define MAX_ITEMS 48

static Scr s_scr = LIST;
static int s_sel = 0;
static nucleo_ducky_layout_t s_layout = DUCKY_LAYOUT_US;
static int s_transport = 0;                 // 0 = USB, 1 = BLE
// Heap-allocated in on_enter, freed in on_exit — ~6KB that must NOT sit permanently in .bss on a chip
// whose runtime heap is ~16KB (min ~2KB). NULL when the app is closed.
static char *s_script = nullptr;
static int   s_script_len = 0;
static nucleo_ducky_stat_t s_stat;
static Item *s_items = nullptr;
static int   s_count = 0;

static const char *BI_NAMES_IT[] = { "Demo: scrivi riga", "Win: apri Notepad", "Win: blocca schermo" };
static const char *BI_NAMES_EN[] = { "Demo: type a line", "Win: open Notepad", "Win: lock screen" };
#define BI_NAMES (nucleo_i18n_is_it() ? BI_NAMES_IT : BI_NAMES_EN)
static const char *BI_SCRIPTS[] = {
    "STRING Ciao da NucleoOS HID\nENTER\n",
    "REM apri Esegui e lancia Notepad\nGUI r\nDELAY 400\nSTRING notepad\nENTER\nDELAY 700\nSTRING Payload NucleoOS\nENTER\n",
    "REM blocca la sessione Windows\nGUI l\n",
};
static const int N_BI = 3;

// ---- payload list / load ---------------------------------------------------------------------------
static void scan_items(void)
{
    s_count = 0;
    if (!s_items) return;
    for (int i = 0; i < N_BI; i++) { snprintf(s_items[s_count].name, 40, "%s", BI_NAMES[i]); s_items[s_count].builtin = true; s_items[s_count].bi = i; s_count++; }
    DIR *dir = opendir(DUCKY_DIR);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && s_count < MAX_ITEMS) {
            const char *n = e->d_name; int l = (int)strlen(n);
            if (l > 4 && strcasecmp(n + l - 4, ".txt") == 0) {
                snprintf(s_items[s_count].name, 40, "%s", n); s_items[s_count].builtin = false; s_items[s_count].bi = -1; s_count++;
            }
        }
        closedir(dir);
    }
}

static bool load_script(int idx)
{
    if (!s_script || !s_items) return false;
    s_script_len = 0; s_script[0] = 0;
    Item *it = &s_items[idx];
    if (it->builtin) {
        s_script_len = snprintf(s_script, SCRIPT_MAX, "%s", BI_SCRIPTS[it->bi]);
    } else {
        char p[200]; snprintf(p, sizeof p, "%s/%s", DUCKY_DIR, it->name);
        FILE *f = fopen(p, "rb"); if (!f) return false;
        size_t got = fread(s_script, 1, SCRIPT_MAX - 1, f); fclose(f);
        s_script_len = (int)got;
        s_script[s_script_len] = 0;
    }
    nucleo_ducky_analyze(s_script, s_script_len, s_layout, &s_stat);
    return true;
}

// ---- run backend (USB / BLE), self-contained modal -------------------------------------------------
static volatile bool s_abort = false;

static bool poll_abort(void) { nucleo_key_t k = nucleo_kbd_read(); if (k.key == NK_BACK) s_abort = true; return s_abort; }

static bool be_ready_usb(void *c) { (void)c; return nucleo_usbhid_ready(); }
static bool be_ready_ble(void *c) { (void)c; return nucleo_ble_hid_ready(); }
// Feed the task watchdog per keystroke: a long STRING (esp. over BLE, where each tap waits ~16ms) would
// otherwise loop inside type_string for >8s with no yield to the watched launcher task -> WDT reboot.
static void be_key_usb(void *c, uint8_t m, uint8_t k) { (void)c; nucleo_usbhid_key(m, k); esp_task_wdt_reset(); }
static void be_key_ble(void *c, uint8_t m, uint8_t k) { (void)c; nucleo_ble_hid_key(m, k); esp_task_wdt_reset(); }
static bool be_abort(void *c) { (void)c; esp_task_wdt_reset(); return poll_abort(); }
static void be_delay(void *c, uint32_t ms)
{
    (void)c;
    uint32_t left = ms ? ms : 1;
    while (left) { uint32_t step = left > 150 ? 150 : left; vTaskDelay(pdMS_TO_TICKS(step)); esp_task_wdt_reset(); left -= step; }
}

static void draw_run(int line, int total, const char *status, unsigned short col)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 6); d.print(TR("Payload","Payload"));
    d.setTextSize(1); d.setTextColor(col, BG); d.setCursor(8, top + 34); d.print(status);
    // progress bar
    int bw = 220, bx = 8, by = top + 56;
    d.drawRect(bx, by, bw, 12, MUTED);
    if (total > 0) { int fillw = (bw - 2) * (line < total ? line : total) / total; d.fillRect(bx + 1, by + 1, fillw, 10, GRN); }
    char ln[40]; snprintf(ln, sizeof ln, TR("%d / %d righe","%d / %d lines"), line, total);
    d.setTextColor(FG, BG); d.setCursor(8, by + 18); d.print(ln);
    d.setTextColor(MUTED, BG); d.setCursor(8, by + 34); d.print(TR("` (ESC) per annullare","` (ESC) to cancel"));
}

static void be_prog(void *c, int line, int total) { (void)c; draw_run(line, total, "esecuzione...", GRN); }

static void run_payload(void)
{
    if (!s_script) return;
    s_abort = false;
    bool ble = (s_transport == 1);
    nucleo_exclusive_info_t inf;
    bool was_excl = false, ble_ok = true;

    draw_run(0, s_stat.lines, ble ? TR("BLE: avvio radio...","BLE: starting radio...") : TR("USB: installo HID...","USB: installing HID..."), WARN);
    if (ble) {
        was_excl = nucleo_exclusive_active();           // don't exit an exclusive we didn't enter
        nucleo_exclusive_enter(NX_DEEP_OFFLINE, &inf);
        ble_ok = nucleo_ble_up();                        // false if the controller won't fit the heap
        if (ble_ok) nucleo_ble_hid_start();
    } else {
        nucleo_usbhid_start();
    }

    // wait for the target to be ready (USB host attach / BLE pair+subscribe), abortable, ~20s
    const char *wait = ble ? TR("accoppia il target: NucleoOS","pair the target: NucleoOS") : TR("collega il cavo USB-C","plug the USB-C cable");
    bool ready = false;
    for (int i = 0; ble_ok && i < 1400 && !s_abort; i++) {
        ready = ble ? nucleo_ble_hid_ready() : nucleo_usbhid_ready();
        if (ready) break;
        if (poll_abort()) break;
        if ((i & 7) == 0) draw_run(0, s_stat.lines, wait, WARN);
        vTaskDelay(pdMS_TO_TICKS(15)); esp_task_wdt_reset();
    }

    char result[48];
    if (!ble_ok)                snprintf(result, sizeof result, "%s", TR("errore: radio BLE non avviata","error: BLE radio not started"));
    else if (!ready || s_abort) snprintf(result, sizeof result, "%s", s_abort ? TR("annullato","cancelled") : TR("target non pronto","target not ready"));
    else {
        nucleo_ducky_backend_t be = {
            ble ? be_ready_ble : be_ready_usb, ble ? be_key_ble : be_key_usb,
            be_delay, be_abort, be_prog, NULL,
        };
        int n = nucleo_ducky_run(s_script, s_script_len, s_layout, &be);
        snprintf(result, sizeof result, s_abort ? TR("annullato a %d righe","cancelled at %d lines") : TR("fatto: %d righe","done: %d lines"), n);
    }

    bool ok = ble_ok && ready && !s_abort;
    if (ble) {
        if (ble_ok) nucleo_ble_hid_stop();
        bool down_ok = nucleo_ble_down();               // false = NimBLE stuck, controller RAM not reclaimed
        if (down_ok) { if (!was_excl) nucleo_exclusive_exit(); }   // don't exit an exclusive we didn't enter
        else { snprintf(result, sizeof result, "%s", TR("BLE bloccato: riavvia","BLE stuck: reboot")); ok = false; }  // do NOT restart Wi-Fi onto a broken heap
    }
    // USB HID stays resident for the session (PHY taken; OTA over Wi-Fi still works) — same as app_usbkbd.

    draw_run(s_stat.lines, s_stat.lines, result, ok ? GRN : WARN);
    d.setTextColor(MUTED, BG); d.setCursor(8, nucleo_app_content_top() + nucleo_app_content_height() - 14); d.print(TR("un tasto per tornare","any key to go back"));
    for (;;) { nucleo_key_t k = nucleo_kbd_read(); if (k.key) break; vTaskDelay(pdMS_TO_TICKS(20)); esp_task_wdt_reset(); }
    nucleo_app_request_draw();
}

// ---- lifecycle / input -----------------------------------------------------------------------------
static bool on_back(int key)
{
    if (s_scr == PREVIEW) { s_scr = LIST; nucleo_app_request_draw(); return true; }
    return false;
}

static void on_enter(void)
{
    s_scr = LIST; s_sel = 0;
    if (!s_script) s_script = (char *)malloc(SCRIPT_MAX);          // ~6KB total, only while the app is open
    if (!s_items)  s_items  = (Item *)malloc(sizeof(Item) * MAX_ITEMS);
    scan_items();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("frecce scegli  invio apri  esc", "arrows pick   enter open   esc back", "flechas elige   intro abre   esc atr", "fleches choix   entree ouv   esc ret", "pfeile waehlen   enter oeff   esc zur"));
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    free(s_script); s_script = nullptr;     // give the ~6KB back to the heap (free(NULL) is safe)
    free(s_items);  s_items  = nullptr;
    s_count = 0;
}

static int s_lg_scroll = 0;        // payload list scroll on the panel (two screens)
static void on_key(int key, char ch)
{
    if (s_scr == LIST) {
        if (s_count == 0) return;
        if (int t = lg::row_tap(key, ch, s_lg_scroll, s_count, &s_sel)) {   // tap a row on the panel
            if (t == 2 && load_script(s_sel)) s_scr = PREVIEW;
            nucleo_app_request_draw(); return;
        }
        if (key == NK_UP)   { s_sel = (s_sel - 1 + s_count) % s_count; nucleo_app_request_draw(); }
        if (key == NK_DOWN) { s_sel = (s_sel + 1) % s_count; nucleo_app_request_draw(); }
        if (key == NK_ENTER) { if (load_script(s_sel)) { s_scr = PREVIEW; nucleo_app_request_draw(); } }
    } else { // PREVIEW
        if (ch == 't' || ch == 'T') { s_transport ^= 1; nucleo_app_request_draw(); }
        else if (ch == 'l' || ch == 'L') { s_layout = (s_layout == DUCKY_LAYOUT_US) ? DUCKY_LAYOUT_IT : DUCKY_LAYOUT_US;
                                           nucleo_ducky_analyze(s_script, s_script_len, s_layout, &s_stat); nucleo_app_request_draw(); }
        else if (key == NK_ENTER) { run_payload(); }
    }
}

// ---- drawing ---------------------------------------------------------------------------------------
static void draw_list(int top, int h)
{
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 6); d.print(TR("Payloads","Payloads"));
    d.setTextSize(1); d.setTextColor(WARN, BG); d.setCursor(110, top + 12); d.print("solo test autorizzati");
    if (s_count == 0) { d.setTextColor(MUTED, BG); d.setCursor(8, top + 34); d.print(TR("nessun payload","no payload")); return; }
    int y = top + 30, rowh = 16, maxrows = (h - 30) / rowh;
    int start = 0; if (s_sel >= maxrows) start = s_sel - maxrows + 1;
    for (int i = start; i < s_count && i < start + maxrows; i++) {
        bool on = (i == s_sel);
        if (on) d.fillRoundRect(4, y - 1, 232, rowh - 2, 3, HL);
        d.setTextColor(on ? ACC : (s_items[i].builtin ? MUTED : DIM), on ? HL : BG); d.setCursor(8, y + 2);
        d.print(s_items[i].builtin ? "*" : ".");
        d.setTextColor(on ? FG : MUTED, on ? HL : BG); d.setCursor(20, y + 2); d.print(s_items[i].name);
        y += rowh;
    }
}

static void draw_preview(int top, int h)
{
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 6); d.print("Anteprima");
    d.setTextSize(1);
    char ln[48];
    d.setTextColor(s_stat.errors ? WARN : GRN, BG); d.setCursor(8, top + 30);
    snprintf(ln, sizeof ln, TR("%d righe  %d tasti  ~%lu ms","%d lines  %d keys  ~%lu ms"), s_stat.lines, s_stat.keystrokes, (unsigned long)s_stat.est_ms);
    d.print(ln);
    if (s_stat.errors) { d.setTextColor(WARN, BG); d.setCursor(8, top + 44); snprintf(ln, sizeof ln, "%d errori (%s)", s_stat.errors, s_stat.first_error); d.print(ln); }
    else { d.setTextColor(MUTED, BG); d.setCursor(8, top + 44); d.print("sintassi ok"); }

    d.setTextColor(FG, BG); d.setCursor(8, top + 64);
    snprintf(ln, sizeof ln, "Transport: %s", s_transport ? TR("BLE (wireless)","BLE (wireless)") : TR("USB (cavo)","USB (cable)")); d.print(ln);
    d.setCursor(8, top + 78); snprintf(ln, sizeof ln, "Layout: %s", s_layout == DUCKY_LAYOUT_IT ? "IT" : "US"); d.print(ln);
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 96); d.print("T transport  L layout");
    d.setTextColor(GRN, BG);   d.setCursor(8, top + 110); d.print(TR("ENTER esegui","ENTER run"));
    d.setTextColor(MUTED, BG); d.setCursor(96, top + 110); d.print(TR("esc indietro","esc back"));
}

// ===== two screens =============================================================================================
// The panel carries the runner: the payload list on the left with the picked script's details on the right, then
// a full-width preview card (lines, keystrokes, time, syntax, transport, layout) with a big RUN button. Touch is
// the content — tap a payload to pick it, tap again (or its card) to open the preview; T/L buttons flip transport
// and layout. The Cardputer is the companion: what is picked / previewed and the keys, never a mirror.
static const int LG_LISTW = 148, LG_INK = 0x0000, LG_LINE = 0x2945;

static const char *pl_label(int i, void *) { return (i >= 0 && i < s_count) ? s_items[i].name : ""; }
static const char *pl_right(int i, void *) { return (i >= 0 && i < s_count) ? (s_items[i].builtin ? "demo" : "sd") : ""; }
static unsigned short pl_color(int i, void *) { return (i >= 0 && i < s_count && !s_items[i].builtin) ? FG : MUTED; }

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();

    if (s_scr == PREVIEW) {
        char ttl[40]; lg::fit(ttl, sizeof ttl, (s_sel >= 0 && s_sel < s_count) ? s_items[s_sel].name : "Payload", W - 60);
        int y = lg::header(ttl, nullptr, ACC, NK_BACK);
        d.fillRect(0, y, W, H - y, BG);
        const int cx = 16, cw = W - 32;
        int cy = y + 8;
        lg::panel(cx, cy, cw, 92);
        const unsigned short cb = lg::card();
        char v[32];
        snprintf(v, sizeof v, "%d", s_stat.lines);      lg::kv(cx + 14, cy + 12, cw - 28, TR("Righe", "Lines"), v, FG, cb);
        snprintf(v, sizeof v, "%d", s_stat.keystrokes); lg::kv(cx + 14, cy + 34, cw - 28, TR("Tasti", "Keystrokes"), v, FG, cb);
        snprintf(v, sizeof v, "~%lu ms", (unsigned long)s_stat.est_ms); lg::kv(cx + 14, cy + 56, cw - 28, TR("Durata", "Time"), v, MUTED, cb);
        lg::font(0); d.setTextColor(s_stat.errors ? WARN : GRN, cb);
        if (s_stat.errors) { char e[48]; snprintf(e, sizeof e, TR("%d errori: %s", "%d errors: %s"), s_stat.errors, s_stat.first_error); d.drawString(e, cx + 14, cy + 76); }
        else d.drawString(TR("sintassi ok", "syntax ok"), cx + 14, cy + 76);
        // transport + layout toggles
        cy += 100;
        const int bw = (cw - 10) / 2;
        char tl[28]; snprintf(tl, sizeof tl, "%s: %s", TR("Via", "Via"), s_transport ? "BLE" : "USB");
        lg::button(cx, cy, bw, 36, tl, LG_LINE, FG, NK_CHAR, 't');
        char ll[28]; snprintf(ll, sizeof ll, "%s: %s", TR("Layout", "Layout"), s_layout == DUCKY_LAYOUT_IT ? "IT" : "US");
        lg::button(cx + bw + 10, cy, bw, 36, ll, LG_LINE, FG, NK_CHAR, 'l');
        // run
        const int bh = 44, by = H - bh - 12;
        lg::button(cx, by, cw, bh, TR("Esegui (INVIO)", "Run (ENTER)"), GRN, LG_INK, NK_ENTER, 0);
        nucleo_touch_commit();
        return;
    }

    // LIST
    char c[16]; snprintf(c, sizeof c, "%d", s_count);
    int y = lg::header("Payloads", c, ACC, NK_BACK);
    d.fillRect(0, y, W, H - y, BG);
    if (s_count == 0) {
        lg::message(16, y + 10, W - 32, H - y - 20, TR("Nessun payload", "No payloads"),
                    TR("metti dei .txt in /sd/data/ducky", "drop .txt files in /sd/data/ducky"), MUTED);
        nucleo_touch_commit();
        return;
    }
    lg::text_list(0, y + 2, LG_LISTW, H - y - 4, s_count, s_sel, &s_lg_scroll, pl_label, pl_right, pl_color, nullptr, ACC);
    const int rx = LG_LISTW + 4, rw = W - LG_LISTW - 10, ry = y + 6, rh = H - y - 12;
    lg::panel(rx, ry, rw, rh);
    const unsigned short cb = lg::card();
    Item *it = &s_items[s_sel];
    lg::font(1); d.setTextColor(ACC, cb);
    char nm[40]; lg::fit(nm, sizeof nm, it->name, rw - 20); d.drawString(nm, rx + 10, ry + 8);
    d.drawFastHLine(rx + 10, ry + 30, rw - 20, LG_LINE);
    lg::font(0); d.setTextColor(it->builtin ? MUTED : FG, cb);
    d.drawString(it->builtin ? TR("Demo integrato", "Built-in demo") : TR("File su SD", "File on SD"), rx + 10, ry + 40);
    lg::font(4); d.setTextColor(WARN, cb);
    d.drawString(TR("solo test autorizzati", "authorized tests only"), rx + 10, ry + 58);
    const int bh = 34, by = ry + rh - bh - 8;
    lg::button(rx + 8, by, rw - 16, bh, TR("Apri", "Open"), ACC, LG_INK, NK_ENTER, 0);
    nucleo_touch_add(rx, ry, rw, by - ry, NK_ENTER, 0);   // the card itself opens too
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("PAYLOADS", ACC);
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
    const char *nm = (s_count && s_sel >= 0 && s_sel < s_count) ? s_items[s_sel].name : TR("nessun payload", "no payload");
    int nl = lg::wrap(nm, 8, y + 4, 224, 20, 2);
    d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
    char ln[48];
    if (s_scr == PREVIEW) {
        snprintf(ln, sizeof ln, TR("%d righe  %d tasti  ~%lu ms", "%d lines  %d keys  ~%lu ms"), s_stat.lines, s_stat.keystrokes, (unsigned long)s_stat.est_ms);
        d.drawString(ln, 8, y + 8 + nl * 20);
        snprintf(ln, sizeof ln, "%s  %s", s_transport ? "BLE" : "USB", s_layout == DUCKY_LAYOUT_IT ? "IT" : "US");
        d.setTextColor(s_stat.errors ? WARN : GRN, BG); d.drawString(ln, 8, y + 24 + nl * 20);
        lg::comp_keys(TR("T via  L layout  INVIO esegui  ESC", "T via  L layout  ENTER run  ESC"));
    } else {
        snprintf(ln, sizeof ln, TR("%d di %d", "%d of %d"), s_count ? s_sel + 1 : 0, s_count);
        d.drawString(ln, 8, y + 8 + nl * 20);
        lg::comp_keys(TR("frecce scegli  INVIO apri  ESC esci", "arrows pick  ENTER open  ESC back"));
    }
}

static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_scr == LIST) draw_list(top, h); else draw_preview(top, h);
}

extern "C" void nucleo_register_payloads(void)
{
    static const nucleo_app_def_t app = {
        "payloads", "Payloads", "Security", "DuckyScript runner USB/BLE + preview (test autorizzati)",
        'D', 0xFD20, on_enter, on_key, nullptr, on_draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
