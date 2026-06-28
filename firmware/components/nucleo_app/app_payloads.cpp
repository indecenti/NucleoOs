// app_payloads.cpp — Security > Payloads: a DuckyScript runner that injects keystrokes over USB HID
// (cable) OR BLE HID (wireless). FOR AUTHORIZED TESTING ONLY. Beyond Bruce's runner: a DRY-RUN PREVIEW
// (validate actions + estimated time + syntax errors before anything fires) and a US/IT keyboard LAYOUT
// selector. Payloads come from /sd/data/ducky/*.txt plus a few built-in demos.
//
// The run is a self-contained modal loop (like app_usbkbd): it owns the keyboard for live progress +
// abort (backtick), and feeds the task watchdog across DELAYs so a long payload can't reboot the device.
#include "nucleo_app.h"
#include "app_gfx.h"
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

static const char *BI_NAMES[]   = { "Demo: scrivi riga", "Win: apri Notepad", "Win: blocca schermo" };
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
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 6); d.print("Payload");
    d.setTextSize(1); d.setTextColor(col, BG); d.setCursor(8, top + 34); d.print(status);
    // progress bar
    int bw = 220, bx = 8, by = top + 56;
    d.drawRect(bx, by, bw, 12, MUTED);
    if (total > 0) { int fillw = (bw - 2) * (line < total ? line : total) / total; d.fillRect(bx + 1, by + 1, fillw, 10, GRN); }
    char ln[40]; snprintf(ln, sizeof ln, "%d / %d righe", line, total);
    d.setTextColor(FG, BG); d.setCursor(8, by + 18); d.print(ln);
    d.setTextColor(MUTED, BG); d.setCursor(8, by + 34); d.print("` (ESC) per annullare");
}

static void be_prog(void *c, int line, int total) { (void)c; draw_run(line, total, "esecuzione...", GRN); }

static void run_payload(void)
{
    if (!s_script) return;
    s_abort = false;
    bool ble = (s_transport == 1);
    nucleo_exclusive_info_t inf;
    bool was_excl = false, ble_ok = true;

    draw_run(0, s_stat.lines, ble ? "BLE: avvio radio..." : "USB: installo HID...", WARN);
    if (ble) {
        was_excl = nucleo_exclusive_active();           // don't exit an exclusive we didn't enter
        nucleo_exclusive_enter(NX_DEEP_OFFLINE, &inf);
        ble_ok = nucleo_ble_up();                        // false if the controller won't fit the heap
        if (ble_ok) nucleo_ble_hid_start();
    } else {
        nucleo_usbhid_start();
    }

    // wait for the target to be ready (USB host attach / BLE pair+subscribe), abortable, ~20s
    const char *wait = ble ? "accoppia il target: NucleoOS" : "collega il cavo USB-C";
    bool ready = false;
    for (int i = 0; ble_ok && i < 1400 && !s_abort; i++) {
        ready = ble ? nucleo_ble_hid_ready() : nucleo_usbhid_ready();
        if (ready) break;
        if (poll_abort()) break;
        if ((i & 7) == 0) draw_run(0, s_stat.lines, wait, WARN);
        vTaskDelay(pdMS_TO_TICKS(15)); esp_task_wdt_reset();
    }

    char result[48];
    if (!ble_ok)                snprintf(result, sizeof result, "errore: radio BLE non avviata");
    else if (!ready || s_abort) snprintf(result, sizeof result, s_abort ? "annullato" : "target non pronto");
    else {
        nucleo_ducky_backend_t be = {
            ble ? be_ready_ble : be_ready_usb, ble ? be_key_ble : be_key_usb,
            be_delay, be_abort, be_prog, NULL,
        };
        int n = nucleo_ducky_run(s_script, s_script_len, s_layout, &be);
        snprintf(result, sizeof result, s_abort ? "annullato a %d righe" : "fatto: %d righe", n);
    }

    bool ok = ble_ok && ready && !s_abort;
    if (ble) {
        if (ble_ok) nucleo_ble_hid_stop();
        bool down_ok = nucleo_ble_down();               // false = NimBLE stuck, controller RAM not reclaimed
        if (down_ok) { if (!was_excl) nucleo_exclusive_exit(); }   // don't exit an exclusive we didn't enter
        else { snprintf(result, sizeof result, "BLE bloccato: riavvia"); ok = false; }  // do NOT restart Wi-Fi onto a broken heap
    }
    // USB HID stays resident for the session (PHY taken; OTA over Wi-Fi still works) — same as app_usbkbd.

    draw_run(s_stat.lines, s_stat.lines, result, ok ? GRN : WARN);
    d.setTextColor(MUTED, BG); d.setCursor(8, nucleo_app_content_top() + nucleo_app_content_height() - 14); d.print("un tasto per tornare");
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
    nucleo_app_set_hint("frecce scegli  enter  esc indietro");
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    free(s_script); s_script = nullptr;     // give the ~6KB back to the heap (free(NULL) is safe)
    free(s_items);  s_items  = nullptr;
    s_count = 0;
}

static void on_key(int key, char ch)
{
    if (s_scr == LIST) {
        if (s_count == 0) return;
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
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 6); d.print("Payloads");
    d.setTextSize(1); d.setTextColor(WARN, BG); d.setCursor(110, top + 12); d.print("solo test autorizzati");
    if (s_count == 0) { d.setTextColor(MUTED, BG); d.setCursor(8, top + 34); d.print("nessun payload"); return; }
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
    snprintf(ln, sizeof ln, "%d righe  %d tasti  ~%lu ms", s_stat.lines, s_stat.keystrokes, (unsigned long)s_stat.est_ms);
    d.print(ln);
    if (s_stat.errors) { d.setTextColor(WARN, BG); d.setCursor(8, top + 44); snprintf(ln, sizeof ln, "%d errori (%s)", s_stat.errors, s_stat.first_error); d.print(ln); }
    else { d.setTextColor(MUTED, BG); d.setCursor(8, top + 44); d.print("sintassi ok"); }

    d.setTextColor(FG, BG); d.setCursor(8, top + 64);
    snprintf(ln, sizeof ln, "Transport: %s", s_transport ? "BLE (wireless)" : "USB (cavo)"); d.print(ln);
    d.setCursor(8, top + 78); snprintf(ln, sizeof ln, "Layout: %s", s_layout == DUCKY_LAYOUT_IT ? "IT" : "US"); d.print(ln);
    d.setTextColor(MUTED, BG); d.setCursor(8, top + 96); d.print("T transport  L layout");
    d.setTextColor(GRN, BG);   d.setCursor(8, top + 110); d.print("ENTER esegui");
    d.setTextColor(MUTED, BG); d.setCursor(96, top + 110); d.print("esc indietro");
}

static void on_draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_scr == LIST) draw_list(top, h); else draw_preview(top, h);
}

extern "C" void nucleo_register_payloads(void)
{
    static const nucleo_app_def_t app = {
        "payloads", "Payloads", "Security", "DuckyScript runner USB/BLE + preview (test autorizzati)",
        'D', 0xFD20, on_enter, on_key, nullptr, on_draw, on_exit, 0
    };
    nucleo_app_register(&app);
}
