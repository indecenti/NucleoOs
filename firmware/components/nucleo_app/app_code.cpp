// app_code.cpp: Nucleo Code (docs/nucleo-code.md). The agent (default view: ask it to write, run and fix programs
// or texts in work/main; code_agent.c, the firmware's LLM service) and, under /files, the runtime bench: the
// workspace's .js files, run in a MicroQuickJS arena (nucleo_code/code_rt.c), with the output, the error and
// file:line, the time, what the script really needed (live after a GC) and the system heap before/during/after.
// "b" runs every script of /work/bench and writes /work/bench/report.txt, the figures section 8 is decided on.
// Store-only: compiled as a module (NAP_MODULE), never into the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "nucleo_app.h"
#include "app_gfx.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"
#include "code_rt.h"
#include "code_agent.h"
#include "nucleo_tcp.h"        // nucleo_tcp_network_up
#include "nucleo_anima.h"      // nucleo_llm_*: the firmware streams the request (the key stays there)
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "nucleo_exclusive.h"  // NX_SOLO
#include "nucleo_theme.h"
#include "app_large.h"        // lg:: two-screen kit
#include "nucleo_touch.h"
#include "nucleo_kbd.h"
#include "nucleo_surface.h"
#include "nucleo_extdisp.h"    // the Cardputer's own panel while the external one runs an app
#include "esp_task_wdt.h"
#include <M5GFX.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const unsigned short ACCENT = 0xFD20;            // amber: a workshop
#define C_BG   THEME_BG
#define C_INK  THEME_FG
#define C_DIM  THEME_MUTED
#define L(it, en, es, fr, de) TR5(it, en, es, fr, de)

#define WS        NUCLEO_SD_MOUNT "/work"
#define BENCH     WS "/bench"
#define AWS       WS "/main"            // the workspace: the agent writes here, /files lists and runs it
#define MAX_FILES 24
#define OUT_CAP   3072
#define ARENA_MAX (40 * 1024)

struct State {
    char files[MAX_FILES][40];
    int  nfiles, sel;
    bool output;                // the output view is up (else the list)
    char out[OUT_CAP]; int olen;
    int  scroll;                // output view: first line shown
    code_result_t res;
    unsigned heap_before, block_before, heap_after, block_after, arena;
    char ran[40];
    // the agent
    bool agent_view, ask, have_key, thinking, was_running;
    agent_t *ag; char *toolbuf;
    char model[48], effort[8];
    char tr[6144]; int trlen, tscroll;
    char in[512]; int inlen;   // a task can be a paragraph
    char todo[512];
    bool auto_run, auto_sent, traced_draw, traced_net;
    // a job from ANIMA (work/main/.code/inbox.txt): a task to send, or a script to open; Esc goes back to ANIMA
    bool from_anima, inbox_task, outbox_done; char run_job[48];
    char last_js[48];           // the script the last task wrote: Enter on an empty line runs it
    char last_text[600]; int last_tlen;   // the model's last words, for ANIMA
    char phase[32]; int64_t phase_t0;     // what the header says it is doing, since when
    int64_t auto_t0;     // the maintainer's self-test (work/main/.code/selftest.txt)
};
static State *S;
static bool s_cleared;                // the agent view painted its background once since it came up

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
// Breadcrumbs on the SD card (work/main/.code/trace.txt): the Solo boot has no web server and a panic loses the log,
// so the last line written says where a crash happened. Cheap: one append per stage, the first time only.
static void trace(const char *what)
{
    FILE *f = fopen(NUCLEO_SD_MOUNT "/work/main/.code/trace.txt", "ab");
    if (f) { fprintf(f, "%lld %s free %u largest %u\n", (long long)(esp_timer_get_time() / 1000), what,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)); fclose(f); }
}
static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
static void out_fn(void *op, const char *s, size_t n)
{
    (void)op;
    size_t room = OUT_CAP - 1 - (size_t)S->olen;
    if (n > room) n = room;
    memcpy(S->out + S->olen, s, n); S->olen += (int)n; S->out[S->olen] = 0;
}

// ---- the scripts' screen (docs/nucleo-code.md 4.5) ------------------------------------------------------------
// A script that draws gets a 16-colour canvas: 240x135 on the Cardputer, 320x240 on the external panel when one is
// there (the Cardputer then shows what runs and how to stop it: never a mirror). Its rows arrive here in bands.
static lgfx::LovyanGFX *s_scr;          // where the frames go (NULL = nowhere)
static void scr_blit(void *, int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_scr) return;
    s_scr->startWrite();
    s_scr->pushImage(x, y, w, h, (const lgfx::rgb565_t *)px);
    s_scr->endWrite();
}
static lgfx::LovyanGFX *small_panel(void)   // the Cardputer's own panel, with or without the external one
{
    if (nucleo_surface_large()) return (lgfx::LovyanGFX *)nucleo_extdisp_internal_gfx();
    return &d;
}
// Keys while a script runs: the keyboard is read here (the run loop waits for the script); Esc stops it, the rest
// queue for key.get(). The engine polls the cancel often: that is also where the task lets the others run.
static struct { unsigned char ring[16]; int head, tail; bool stop; int64_t yield_at; } s_kb;   // a character, or 0x80 | NK_*
static const char *key_name(const nucleo_key_t &k, char *one)
{
    switch (k.key) {
    case NK_UP: return "up";      case NK_DOWN: return "down"; case NK_LEFT: return "left"; case NK_RIGHT: return "right";
    case NK_ENTER: return "enter"; case NK_DEL: return "del";  case NK_TAB: return "tab";
    case NK_CHAR: one[0] = k.ch; one[1] = 0; return one;
    default: return nullptr;
    }
}
static void kb_pump(void)
{
    for (nucleo_key_t k = nucleo_kbd_read(); k.key != NK_NONE; k = nucleo_kbd_read()) {
        if (k.key == NK_BACK) { s_kb.stop = true; continue; }
        if (k.key == NK_CHAR && (k.ch < 32 || k.ch > 126)) continue;
        if (k.key != NK_CHAR && k.key > NK_TAB) continue;
        if ((s_kb.head + 1) % 16 == s_kb.tail) continue;          // full: a script that never reads keys
        s_kb.ring[s_kb.head] = k.key == NK_CHAR ? (unsigned char)k.ch : (unsigned char)(0x80 | k.key);
        s_kb.head = (s_kb.head + 1) % 16;
    }
}
static const char *kb_key(void *)
{
    kb_pump();
    if (s_kb.tail == s_kb.head) return nullptr;
    static char one[2];
    unsigned char c = s_kb.ring[s_kb.tail];
    s_kb.tail = (s_kb.tail + 1) % 16;
    if (c & 0x80) { nucleo_key_t k = { c & 0x7F, 0 }; return key_name(k, one); }
    one[0] = (char)c; one[1] = 0;
    return one;
}
static bool kb_down(void *, const char *n)
{
    if (!strcmp(n, "up")) return nucleo_kbd_char_down(';');
    if (!strcmp(n, "down")) return nucleo_kbd_char_down('.');
    if (!strcmp(n, "left")) return nucleo_kbd_char_down(',');
    if (!strcmp(n, "right")) return nucleo_kbd_char_down('/');
    return n[0] && !n[1] && nucleo_kbd_char_down(n[0]);
}
static bool kb_cancel(void *)
{
    int64_t t = esp_timer_get_time() / 1000;
    if (t >= s_kb.yield_at) { s_kb.yield_at = t + 200; kb_pump(); esp_task_wdt_reset(); vTaskDelay(1); }
    return s_kb.stop;
}
static bool tp_read(void *, int *x, int *y, bool *down)
{
    nucleo_touch_raw_t ev;
    if (!nucleo_touch_raw_read(&ev)) return false;
    *x = ev.x; *y = ev.y; *down = ev.down;
    return true;
}
// what the Cardputer shows while an app runs on the external panel
static void companion_running(const char *name)
{
    lgfx::LovyanGFX *g = small_panel();
    if (!g || g == &d) return;
    g->startWrite();
    g->fillRect(0, 0, 240, 135, C_BG);
    g->setFont(&fonts::Font2); g->setTextSize(1); g->setTextDatum(top_left);
    g->setTextColor(ACCENT, C_BG); g->drawString(L("In esecuzione sul pannello", "Running on the panel", "En el panel", "Sur le panneau", "Auf dem Panel"), 8, 10);
    g->setTextColor(C_INK, C_BG); g->drawString(name, 8, 40);
    g->setTextColor(C_DIM, C_BG); g->drawString(L("esc ferma", "esc stops", "esc para", "esc arrete", "esc stoppt"), 8, 100);
    g->endWrite();
}

// The bench scripts: written once when the folder has none, so the first run has something to measure.
static const struct { const char *name, *src; } SAMPLES[] = {
    { "fizzbuzz.js", "for (var i = 1; i <= 30; i++) print(i % 15 == 0 ? 'FizzBuzz' : i % 3 == 0 ? 'Fizz' : i % 5 == 0 ? 'Buzz' : i);\n" },
    { "quicksort.js",
      "function qs(a){ if (a.length < 2) return a; var p = a[0], l = [], r = [];\n"
      "  for (var i = 1; i < a.length; i++) (a[i] < p ? l : r).push(a[i]); return qs(l).concat([p], qs(r)); }\n"
      "var a = []; for (var i = 0; i < 400; i++) a.push((i * 7919) % 1000);\n"
      "var t = Date.now(); var s = qs(a); print(s[0], s[399], s.length, (Date.now() - t) + ' ms');\n" },
    { "words.js",
      "var words = {}; var t = 'the quick brown fox jumps over the lazy dog the fox the end';\n"
      "for (var k = 0; k < 50; k++) { var w = t.split(' '); for (var i = 0; i < w.length; i++) words[w[i]] = (words[w[i]] || 0) + 1; }\n"
      "var keys = Object.keys(words).sort(function(a,b){ return words[b] - words[a]; });\n"
      "print(keys[0], words[keys[0]]); print(JSON.stringify({top: keys.slice(0,3)}));\n" },
    { "life.js",
      "var W = 30, H = 12, g = []; for (var y = 0; y < H; y++) { var row = []; for (var x = 0; x < W; x++) row.push(((x*3+y*5)%7)==0 ? 1 : 0); g.push(row); }\n"
      "function step(g){ var n = []; for (var y = 0; y < H; y++){ var r = []; for (var x = 0; x < W; x++){ var c = 0;\n"
      "  for (var dy = -1; dy <= 1; dy++) for (var dx = -1; dx <= 1; dx++) if (dx||dy) c += g[(y+dy+H)%H][(x+dx+W)%W];\n"
      "  r.push(c == 3 || (c == 2 && g[y][x]) ? 1 : 0); } n.push(r); } return n; }\n"
      "var t = Date.now(); for (var s = 0; s < 20; s++) g = step(g); var alive = 0;\n"
      "for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) alive += g[y][x]; print('alive', alive, (Date.now() - t) + ' ms');\n" },
    { "files.js", "fs.write('out.txt', 'riga 1\\n'); fs.append('out.txt', 'riga 2\\n'); print(fs.read('out.txt')); print(fs.list('.').join(' '));\n" },
    { "error.js", "var a = 1;\nvar b = { c: 2 };\nprint(b.d.e);\n" },
    { "loop.js", "var i = 0; while (true) i++;\n" },
    { "memory.js", "var a = []; for (var i = 0; ; i++) a.push('blocco ' + i + ' ' + Array(20).join('x')); \n" },
};

static void ensure_bench(void)
{
    mkdir(WS, 0775); mkdir(BENCH, 0775);
    for (size_t i = 0; i < sizeof SAMPLES / sizeof SAMPLES[0]; i++) {
        char p[96]; snprintf(p, sizeof p, BENCH "/%s", SAMPLES[i].name);
        struct stat st; if (stat(p, &st) == 0) continue;
        FILE *f = fopen(p, "wb"); if (f) { fputs(SAMPLES[i].src, f); fclose(f); }
    }
}

static void scan(const char *dir)
{
    S->nfiles = 0;
    DIR *dd = opendir(dir); if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd)) && S->nfiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n > 3 && n < sizeof S->files[0] && !strcmp(e->d_name + n - 3, ".js")) snprintf(S->files[S->nfiles++], sizeof S->files[0], "%s", e->d_name);
    }
    closedir(dd);
    if (S->sel >= S->nfiles) S->sel = 0;
}

// One run: the arena is the largest block minus a margin, capped; it exists only for the run.
// `interactive`: the user opened it - the screen, the keys and the touch are its, no time limit, Esc stops it.
static bool run_file(const char *dir, const char *name, uint32_t timeout_ms, bool interactive)
{
    S->olen = 0; S->out[0] = 0; S->scroll = 0;
    snprintf(S->ran, sizeof S->ran, "%s", name);
    char p[128]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(S->res.error, sizeof S->res.error, "cannot open %s", name); S->res.ok = false; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 32 * 1024) { fclose(f); snprintf(S->res.error, sizeof S->res.error, "%s: bigger than 32 KB", name); S->res.ok = false; return false; }
    char *src = (char *)malloc((size_t)n + 1);
    if (!src) { fclose(f); snprintf(S->res.error, sizeof S->res.error, "no RAM for the source"); S->res.ok = false; return false; }
    fread(src, 1, (size_t)n, f); fclose(f); src[n] = 0;

    const bool large = interactive && nucleo_surface_large();
    const bool draws = interactive && strstr(src, "gfx.") != nullptr;
    S->heap_before  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    S->block_before = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t arena = S->block_before > 4096 ? S->block_before - 2048 : 0;
    // a script that draws leaves the heap room for its canvas (strips: 20 KB at 240x135, 43 KB at 320x240)
    const size_t amax = !draws ? ARENA_MAX : large ? 14 * 1024 : 24 * 1024;
    if (arena > amax) arena = amax;
    arena &= ~(size_t)7;
    void *mem = arena ? heap_caps_malloc(arena, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
    S->arena = (unsigned)(mem ? arena : 0);
    if (!mem) { free(src); snprintf(S->res.error, sizeof S->res.error, "no block for the arena"); S->res.ok = false; return false; }
    code_env_t env = {};
    env.root = dir; env.out = out_fn; env.now_ms = now_ms; env.sleep_ms = sleep_ms; env.timeout_ms = timeout_ms;
    if (interactive) {
        env.timeout_ms = CODE_NO_LIMIT; env.cancel = kb_cancel;
        env.scr_w = large ? 320 : 240; env.scr_h = large ? 240 : 135;
        env.blit = scr_blit; env.key = kb_key; env.key_down = kb_down;
        s_kb.head = s_kb.tail = 0; s_kb.stop = false; s_kb.yield_at = 0;
        s_scr = large ? &d : small_panel();
        if (large) { nucleo_touch_raw(true); env.touch = tp_read; companion_running(name); }
    }
    bool ok = code_run(mem, arena, src, (size_t)n, name, &env, &S->res);
    if (interactive) {
        if (large) nucleo_touch_raw(false);
        s_scr = nullptr;
        if (S->res.cancelled) { S->res.ok = true; S->res.cancelled = false; S->res.error[0] = 0; ok = true; }   // Esc is how an app ends
        nucleo_app_force_repaint(); s_cleared = false;
    }
    free(mem); free(src);            // free() releases heap_caps_malloc memory too (heap_caps_free is not in the ABI)
    S->heap_after  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    S->block_after = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return ok;
}

static void bench_all(void)
{
    char *rep = (char *)malloc(4096);
    if (!rep) return;
    int rl = snprintf(rep, 4096, "Nucleo Code bench (MicroQuickJS, 32-bit)\nscript           ok  ms     live-max  arena   heap-before/block   heap-after/block  error\n");
    scan(BENCH);
    for (int i = 0; i < S->nfiles && rl < 3900; i++) {
        run_file(BENCH, S->files[i], 3000, false);
        rl += snprintf(rep + rl, 4096 - rl, "%-16s %-3s %-6u %-8u  %-6u  %6u/%-6u      %6u/%-6u  %s %s\n", S->files[i], S->res.ok ? "yes" : "no",
                       (unsigned)S->res.ms, (unsigned)S->res.live_max, S->arena, S->heap_before, S->block_before, S->heap_after, S->block_after,
                       S->res.error, S->res.where);
        nucleo_app_request_draw();
    }
    FILE *f = fopen(BENCH "/report.txt", "wb");
    if (f) { fwrite(rep, 1, (size_t)rl, f); fclose(f); }
    scan(AWS);
    S->olen = 0; S->out[0] = 0; out_fn(nullptr, rep, (size_t)rl);
    snprintf(S->ran, sizeof S->ran, "bench -> report.txt");
    S->output = true; S->scroll = 0;
    free(rep);
}

// ---- drawing ------------------------------------------------------------------------------------------
static const char *row_label(int i, void *) { return S->files[i]; }
static const char *row_right(int, void *) { return ""; }
static unsigned short row_color(int, void *) { return C_INK; }

static void draw_output(int y)
{
    int bottom = nucleo_app_content_top() + nucleo_app_content_height();
    d.fillRect(0, y, 240, bottom - y, C_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    char st[96];
    if (S->res.ok) snprintf(st, sizeof st, "ok %ums  live %uB  arena %uK  heap %uK", (unsigned)S->res.ms, (unsigned)S->res.live_max, S->arena / 1024, S->heap_after / 1024);
    else snprintf(st, sizeof st, "%s %s", S->res.where, S->res.error);
    d.setTextColor(S->res.ok ? 0x07E0 : 0xF800, C_BG); d.drawString(st, 2, y); y += 10;
    d.setTextColor(C_INK, C_BG);
    int line = 0; const char *p = S->out;
    while (*p && y + 8 <= bottom) {
        const char *e = p; int cols = 0;
        while (*e && *e != '\n' && cols < 39) { e++; cols++; }
        if (line++ >= S->scroll) { char b[41]; memcpy(b, p, (size_t)(e - p)); b[e - p] = 0; d.drawString(b, 2, y); y += 9; }
        p = *e == '\n' ? e + 1 : e;
    }
}

// ---- two screens (CLAUDE.md "never a mirror"): the 320x240 panel holds the files (tap = pick, tap again = run)
// and the run: result, time, memory, output; the Cardputer becomes the companion (the pick, the verdict, keys).
static const char *row_right_lg(int i, void *) { return (S->ran[0] && !strcmp(S->files[i], S->ran)) ? (S->res.ok ? "ok" : "!") : ""; }
static unsigned short row_color_lg(int i, void *) { return (S->ran[0] && !strcmp(S->files[i], S->ran) && !S->res.ok) ? 0xF800 : C_INK; }
static int s_lg_scroll;

static void draw_large(void)
{
    nucleo_touch_clear();
    const int W = d.width(), H = d.height(), LW = 128;
    int y = lg::header("Code", "work/main", ACCENT);
    lg::text_list(0, y, LW, H - y, S->nfiles, S->sel, &s_lg_scroll, row_label, row_right_lg, row_color_lg, nullptr, ACCENT);
    const int x = LW + 4, w = W - x - 4;
    lg::panel(x, y + 2, w, H - y - 4, lg::card());
    if (!S->ran[0]) {
        lg::message(x, y + 2, w, H - y - 4, L("Tocca un file", "Tap a file", "Toca un archivo", "Touchez un fichier", "Tippe eine Datei"),
                    L("tocca ancora per eseguirlo, B per il banco", "tap again to run it, B for the bench", "otra vez para ejecutarlo, B banco",
                      "encore pour le lancer, B banc", "nochmal zum Starten, B Bench"), ACCENT);
        nucleo_touch_commit();
        return;
    }
    lg::font(1);
    d.setTextDatum(top_left);
    char st[64];
    d.setTextColor(S->res.ok ? 0x07E0 : 0xF800, lg::card());
    if (S->res.ok) snprintf(st, sizeof st, "%s  ok  %u ms", S->ran, (unsigned)S->res.ms);
    else snprintf(st, sizeof st, "%s  %s", S->ran, S->res.timeout ? "timeout" : S->res.oom ? "memory" : "error");
    d.drawString(st, x + 6, y + 8);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_DIM, lg::card());
    snprintf(st, sizeof st, "live %u B / arena %u KB   heap %u KB", (unsigned)S->res.live_max, S->arena / 1024, S->heap_after / 1024);
    d.drawString(st, x + 6, y + 26);
    int yy = y + 38;
    if (!S->res.ok) {
        d.setTextColor(0xF800, lg::card());
        char er[200]; snprintf(er, sizeof er, "%s %s", S->res.where, S->res.error);
        yy += 10 * lg::wrap(er, x + 6, yy, w - 12, 10, 3);
    }
    d.setTextColor(C_INK, lg::card());
    const int cols = (w - 12) / 6;                          // Font0: 6 px a column
    int line = 0; const char *p = S->out;
    while (*p && yy + 9 <= H - 6) {
        const char *e = p; int c = 0;
        while (*e && *e != '\n' && c < cols && c < 60) { e++; c++; }
        if (line++ >= S->scroll) { char b[64]; memcpy(b, p, (size_t)(e - p)); b[e - p] = 0; d.drawString(b, x + 6, yy); yy += 10; }
        p = *e == '\n' ? e + 1 : e;
    }
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    d.fillRect(0, nucleo_app_content_top(), 240, nucleo_app_content_height(), C_BG);
    int y = lg::comp_head("Code", ACCENT);
    d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextDatum(top_left);
    d.setTextColor(C_INK, C_BG);
    d.drawString(S->nfiles ? S->files[S->sel] : "-", 8, y + 6);
    if (S->ran[0]) {
        char st[64];
        snprintf(st, sizeof st, "%s: %s %ums", S->ran, S->res.ok ? "ok" : (S->res.where[0] ? S->res.where : "error"), (unsigned)S->res.ms);
        d.setTextColor(S->res.ok ? 0x07E0 : 0xF800, C_BG);
        d.drawString(st, 8, y + 28);
    }
    lg::comp_keys(L("invio esegui  b banco  su/giu", "enter run  b bench  up/dn", "intro ejecuta  b banco", "entree lancer  b banc", "enter starten  b Bench"));
}

static void files_draw(void)
{
    s_cleared = false;
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int y = app_ui_title("Code", ACCENT, S->output ? S->ran : "work/main");
    if (S->output) { draw_output(y); nucleo_app_set_hint(L("su/giu scorri  invio lista  b banco", "up/dn scroll  enter list  b bench", "arr/ab  intro lista  b banco", "haut/bas  entree liste  b banc", "auf/ab  enter Liste  b Bench")); return; }
    int bottom = nucleo_app_content_top() + nucleo_app_content_height();
    app_ui_list(y, bottom - y, S->nfiles, S->sel, row_label, row_right, row_color, nullptr);
    nucleo_app_set_hint(L("invio esegui  b banco  /agent", "enter run  b bench  /agent", "intro ejecuta  b banco", "entree lancer  b banc", "enter starten  b Bench"));
}

static void files_key(int key, char ch)
{
    if (ch == 'b' || ch == 'B') { bench_all(); nucleo_app_request_draw(); return; }
    if (ch == '/') { S->agent_view = true; nucleo_app_request_draw(); return; }
    if (nucleo_app_two_screens()) {                          // taps on the panel's list arrive as keys
        int t = lg::row_tap(key, ch, s_lg_scroll, S->nfiles, &S->sel);
        if (t == 2 && S->nfiles) { run_file(AWS, S->files[S->sel], 0, true); S->output = true; }
        if (t) { nucleo_app_request_draw(); return; }
        if (key == NK_UP && S->scroll > 0 && S->output) { S->scroll--; nucleo_app_request_draw(); return; }
    }
    if (S->output) {
        if (key == NK_UP && S->scroll > 0) S->scroll--;
        else if (key == NK_DOWN) S->scroll++;
        else if (key == NK_ENTER) S->output = false;
        nucleo_app_request_draw(); return;
    }
    if (key == NK_UP && S->nfiles) S->sel = (S->sel + S->nfiles - 1) % S->nfiles;
    else if (key == NK_DOWN && S->nfiles) S->sel = (S->sel + 1) % S->nfiles;
    else if (key == NK_ENTER && S->nfiles) { run_file(AWS, S->files[S->sel], 0, true); S->output = true; }
    nucleo_app_request_draw();
}

// ==== the agent (docs/nucleo-code.md 9): status bar, the conversation, the input; driven from the tick ====
#define TR_CAP    6144                  // the conversation shown (raw; the oldest half goes when full)
#define TOOL_CAP  (16 * 1024)           // one tool call's input (a whole file for create)

static void tr_add(const char *s, size_t n)
{
    if (n >= TR_CAP / 2) { s += n - TR_CAP / 2; n = TR_CAP / 2; }
    if ((size_t)S->trlen + n >= TR_CAP - 1) {                 // drop the oldest half, at a line start
        int cut = S->trlen / 2; while (cut < S->trlen && S->tr[cut] != '\n') cut++;
        memmove(S->tr, S->tr + cut, (size_t)(S->trlen - cut)); S->trlen -= cut;
    }
    memcpy(S->tr + S->trlen, s, n); S->trlen += (int)n; S->tr[S->trlen] = 0;
    S->tscroll = 0;
    nucleo_app_request_draw();
}
static void tr_puts(const char *s) { tr_add(s, strlen(s)); }
// The conversation is lines that start with their role (ANIMA's chat: who said it decides how it looks); a line
// without one continues the role above it. RM_TOOL is a step being done, rewritten to OK / FAIL when it ends.
enum { RM_USER = 1, RM_TEXT, RM_TOOL, RM_OK, RM_FAIL, RM_META };
static char s_role;                       // the role the transcript's last line has
static void tr_role(char role)            // start a line of this role (a new line unless it continues one)
{
    if (s_role == role && role == RM_TEXT) return;   // the model's words stream into the same block
    if (S->trlen && S->tr[S->trlen - 1] != '\n') tr_add("\n", 1);
    tr_add(&role, 1);
    s_role = role;
}
static void tr_line(char role, const char *t) { tr_role(role); tr_puts(t); tr_add("\n", 1); s_role = 0; }
// a note of Code's own ("[no network...]"): one meta line, without the brackets and the blank lines around it
static void tr_note(const char *t)
{
    while (*t == '\n' || *t == '[' || *t == ' ') t++;
    char b[240]; snprintf(b, sizeof b, "%s", t);
    size_t n = strlen(b); while (n && (b[n - 1] == '\n' || b[n - 1] == ']' || b[n - 1] == ' ')) b[--n] = 0;
    if (n) tr_line(RM_META, b);
}
static void ui_text(void *, const char *s, size_t n)
{
    while (n && s_role != RM_TEXT && (*s == '\n' || *s == ' ')) { s++; n--; }   // no blank first line
    if (!n) return;
    tr_role(RM_TEXT);
    tr_add(s, n);
    size_t room = sizeof S->last_text - 1 - (size_t)S->last_tlen;
    if (n > room) n = room;
    memcpy(S->last_text + S->last_tlen, s, n); S->last_tlen += (int)n; S->last_text[S->last_tlen] = 0;
}
// "create /sd/work/main/snake.js +105", "edit snake.js:12 ..." -> "snake.js" when it is a script
static void note_file(const char *summary)
{
    const char *p = strchr(summary, ' ');
    if (!p || (strncmp(summary, "create", 6) && strncmp(summary, "rewrite", 7) && strncmp(summary, "edit", 4))) return;
    p++;
    if (!strncmp(p, AWS "/", strlen(AWS) + 1)) p += strlen(AWS) + 1;
    while (p[0] == '.' && p[1] == '/') p += 2;
    char f[48]; size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != ':' && n < sizeof f - 1) { f[n] = p[n]; n++; }
    f[n] = 0;
    if (n > 3 && !strcmp(f + n - 3, ".js") && !strchr(f, '/')) snprintf(S->last_js, sizeof S->last_js, "%s", f);
}
static int s_tool_at = -1;                // where the step being done starts in the transcript
static void ui_tool(void *, const char *, const char *summary)
{
    tr_role(RM_TOOL); s_tool_at = S->trlen - 1;
    tr_puts(summary); tr_add("\n", 1); s_role = 0;
    snprintf(S->phase, sizeof S->phase, "%.30s", summary); S->phase_t0 = now_ms();
}
static void ui_tool_done(void *, const char *, bool ok, const char *summary)
{
    if (s_tool_at >= 0 && s_tool_at < S->trlen && S->tr[s_tool_at] == RM_TOOL) { S->trlen = s_tool_at; S->tr[S->trlen] = 0; }   // the step, as it ended
    s_tool_at = -1; s_role = 0;
    tr_line(ok ? RM_OK : RM_FAIL, summary);
    if (ok) note_file(summary);
    S->phase[0] = 0;
}
static void ui_todo(void *, const char *t) { snprintf(S->todo, sizeof S->todo, "%s", t); nucleo_app_request_draw(); }

#ifdef NUCLEO_UI_HOST
static bool s_host_running;               // the ui-host gallery's "at work" scene (no agent runs there)
#endif
static bool agent_running(void)
{
#ifdef NUCLEO_UI_HOST
    if (s_host_running) return true;
#endif
    agent_state_t s = S->ag ? agent_state(S->ag) : AG_IDLE;
    return s == AG_SEND || s == AG_RECV || s == AG_TOOLS || s == AG_ASK;
}

// a visual test's frames, live: the small layout on the Cardputer, the large one on the external panel when there is one
static void ui_show(void *, bool large, int x, int y, int w, int h, const uint16_t *px)
{
    s_scr = large ? (nucleo_surface_large() ? &d : nullptr) : small_panel();
    scr_blit(nullptr, x, y, w, h, px);
    s_scr = nullptr;
}
static void ui_shown(void *) { s_cleared = false; nucleo_app_force_repaint(); nucleo_app_request_draw(); }
static void agent_open(void)
{
    if (S->ag) return;
    mkdir(WS, 0775); mkdir(AWS, 0775);
    S->have_key = nucleo_llm_config(S->model, sizeof S->model, S->effort, sizeof S->effort);
    if (!S->have_key) return;
    agent_cfg_t cfg = {};
    cfg.root = AWS; cfg.model = S->model; cfg.effort = S->effort;
    cfg.lang = L("Italian", "English", "Spanish", "French", "German");
    cfg.max_calls = 30; cfg.max_tokens = 16000; cfg.arena = nullptr; cfg.arena_size = 32768;
    cfg.toolbuf = nullptr; cfg.toolcap = TOOL_CAP; cfg.ask_writes = S->ask;   // the agent holds it only while a reply streams
    agent_io_t io = {};
    io.on_text = ui_text; io.on_tool = ui_tool; io.on_tool_done = ui_tool_done; io.on_todo = ui_todo;
    io.now_ms = now_ms; io.sleep_ms = sleep_ms;
    io.show = ui_show; io.shown = ui_shown;
    S->ag = agent_new(&cfg, &io);
}
static void agent_close(void)
{
    if (nucleo_llm_busy()) { nucleo_llm_cancel(); for (int i = 0; i < 40 && nucleo_llm_busy(); i++) vTaskDelay(pdMS_TO_TICKS(50)); }
    if (S->ag) { agent_free(S->ag); S->ag = nullptr; }
    free(S->toolbuf); S->toolbuf = nullptr;
}

// ---- a job from ANIMA (docs/nucleo-code.md 4.6) ----------------------------------------------------------------
#define INBOX  AWS "/.code/inbox.txt"
#define OUTBOX AWS "/.code/outbox.txt"
static void code_outbox(const char *error)
{
    S->outbox_done = true;
    FILE *f = fopen(OUTBOX, "wb");
    if (!f) return;
    static char plain[sizeof S->last_text];
    code_md_plain(S->last_text, plain, sizeof plain);
    for (char *c = plain; *c; c++) if (*c == '\n') *c = ' ';
    if (error) fprintf(f, "%s %s", L("non ho finito:", "I did not finish:", "no termine:", "pas fini :", "nicht fertig:"), error);
    else fputs(plain[0] ? plain : L("fatto.", "done.", "hecho.", "fait.", "fertig."), f);
    if (S->last_js[0]) fprintf(f, " [%s]", S->last_js);
    fclose(f);
}
static void inbox_read(void)
{
    FILE *f = fopen(INBOX, "rb");
    if (!f) return;
    char b[sizeof S->in + 8]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    remove(INBOX);                                          // a job is taken once
    char *nl = strchr(b, '\n'); if (!nl) return;
    *nl = 0; const char *arg = nl + 1;
    size_t al = strlen(arg); while (al && (arg[al - 1] == '\n' || arg[al - 1] == '\r')) al--;
    S->from_anima = true;
    if (!strcmp(b, "task") && al) { snprintf(S->in, sizeof S->in, "%.*s", (int)al, arg); S->inlen = (int)strlen(S->in); S->inbox_task = true; }
    else if (!strcmp(b, "run") && al && al < sizeof S->run_job) { memcpy(S->run_job, arg, al); S->run_job[al] = 0; }
}
// Esc: back to ANIMA when it asked (a running task stops first, so the history stays whole)
static bool back_key(int key)
{
    if (key != NK_BACK || !S->from_anima) return false;
    if (agent_running()) { nucleo_llm_cancel(); agent_cancel(S->ag, "stopped by the user"); }
    if (!S->outbox_done && S->last_tlen) code_outbox(nullptr);
    agent_close();
    nucleo_app_exit_to("anima");
    return true;
}
static void run_script_here(const char *f)                  // from the conversation: the app, then back to it
{
    run_file(AWS, f, 0, true);
    char b[160];
    if (S->res.ok) snprintf(b, sizeof b, "\n[%s: %s]\n", f, L("chiuso", "closed", "cerrado", "ferme", "beendet"));
    else snprintf(b, sizeof b, "\n[%s: %s %s]\n", f, S->res.where, S->res.error);
    tr_note(b);
    if (S->olen) tr_line(RM_META, S->out);
}
static void agent_send(void);
static void inbox_tick(void)
{
    if (S->run_job[0]) {                                    // "esegui snake.js": the app, then ANIMA again
        char f[48]; snprintf(f, sizeof f, "%s", S->run_job); S->run_job[0] = 0;
        run_script_here(f);
        if (S->from_anima && S->res.ok) { agent_close(); nucleo_app_exit_to("anima"); }
        return;
    }
    if (!S->inbox_task) return;
    if (!S->auto_t0) S->auto_t0 = now_ms();
    if (!nucleo_tcp_network_up() && now_ms() - S->auto_t0 < 40000) return;   // the Solo boot brings Wi-Fi up after us
    S->inbox_task = false;
    if (!nucleo_tcp_network_up()) { tr_note(L("\n[nessuna rete: il compito e' nell'input, Invio per riprovare]\n", "\n[no network: the task is in the input, Enter to retry]\n",
                                              "\n[sin red: Intro para reintentar]\n", "\n[pas de reseau : Entree pour reessayer]\n", "\n[kein Netz: Enter fuer neuen Versuch]\n")); return; }
    agent_send();
}

// the tick: the request goes out, its bytes come in, one tool runs, a question waits
static void agent_tick(void)
{
    if (!S->ag) return;
    switch (agent_state(S->ag)) {
    case AG_SEND:
        if (nucleo_llm_busy()) break;
        if (S->auto_run) trace("send");
        S->phase[0] = 0; S->phase_t0 = now_ms();
        agent_begin_response(S->ag);
        if (!nucleo_llm_start(agent_request_path(S->ag))) agent_end_response(S->ag, -1, "the connection could not start");
        S->thinking = true;
        break;
    case AG_RECV: {
        char b[1024];
        for (int k = 0; k < 16; k++) {
            int n = nucleo_llm_read(b, sizeof b);
            if (n > 0) { agent_feed(S->ag, b, n); continue; }
            if (n < 0) { char err[200] = ""; int st = nucleo_llm_status(err, sizeof err); agent_end_response(S->ag, st, err); S->thinking = false; }
            break;
        }
        break;
    }
    case AG_TOOLS:
        if (S->auto_run) trace("tool");                     // the self-test logs the heap around every tool
        agent_step(S->ag);
        if (S->auto_run) trace("tool done");
        nucleo_app_request_draw(); break;
    case AG_DONE: case AG_ERROR:
        if (S->was_running) {
            const agent_stats_t *st = agent_stats(S->ag);
            char b[240];
            if (agent_state(S->ag) == AG_ERROR) { snprintf(b, sizeof b, "%s", st->error); tr_note(b); }
            s_role = 0;
            S->was_running = false;
            if (S->from_anima && !S->outbox_done) code_outbox(agent_state(S->ag) == AG_ERROR ? st->error : nullptr);
        }
        break;
    default: break;
    }
    if (agent_running()) S->was_running = true;
}

static void agent_send(void)
{
    if (!S->inlen) return;
    S->in[S->inlen] = 0;
    if (S->in[0] == '/') {                                    // commands
        if (!strcmp(S->in, "/files")) { S->agent_view = false; scan(AWS); }
        else if (!strcmp(S->in, "/new")) { if (S->ag) agent_reset(S->ag); S->trlen = 0; S->tr[0] = 0; S->todo[0] = 0; S->last_js[0] = 0; s_role = 0; s_cleared = false; }
        else if (!strcmp(S->in, "/mode")) { S->ask = !S->ask; agent_close(); agent_open(); tr_note(S->ask ? "mode: Ask" : "mode: Auto"); }
        else tr_note(L("\n[comandi: /files /new /mode]\n", "\n[commands: /files /new /mode]\n", "\n[/files /new /mode]\n", "\n[/files /new /mode]\n", "\n[/files /new /mode]\n"));
        S->inlen = 0; S->in[0] = 0; nucleo_app_request_draw(); return;
    }
    agent_open();
    if (!S->ag) { tr_note(L("\n[nessuna chiave Claude: aggiungila nella web OS, Impostazioni > IA]\n", "\n[no Claude key: add one in the web OS, Settings > AI]\n",
                            "\n[sin clave Claude: web OS, Ajustes > IA]\n", "\n[pas de cle Claude : web OS, Reglages > IA]\n", "\n[kein Claude-Schluessel: Web OS, Einstellungen > KI]\n")); return; }
    tr_line(RM_USER, S->in);
    S->last_tlen = 0; S->last_text[0] = 0; S->outbox_done = false;
    agent_submit(S->ag, S->in);
    S->inlen = 0; S->in[0] = 0;
    S->was_running = true;
    nucleo_app_request_draw();
}

// ---- the agent view, drawn like ANIMA's chat (docs/nucleo-code.md 9) --------------------------------------------
// A header with the model and what it is doing, the conversation by role (your lines on the right, Code's on the
// left behind an accent bar, each step on a compact row with its mark), the input field. Straight on the panel
// and flicker-free: every row paints its own background, nothing is cleared first.
static const unsigned short C_USR = 0x6E1F, C_OK = 0x47E8, C_BAD = 0xF8A6, C_ASK = 0xFFE0;
static lgfx::LovyanGFX *G_ = &d;          // the panel a view draws on
#define G (*G_)
// opaque text in the band [bx, bx+bw) x [by, by+bh): only what the glyphs leave uncovered is filled
static int box_text(int bx, int by, int bw, int bh, int tx, int ty, const char *s, unsigned short fg, unsigned short bg)
{
    const int fh = (int)G.fontHeight(), w = (s && s[0]) ? (int)G.textWidth(s) : 0, bx1 = bx + bw, by1 = by + bh;
    if (tx < bx) tx = bx;
    const int gy0 = ty < by ? by : ty, gy1 = ty + fh > by1 ? by1 : ty + fh;
    if (gy0 > by) G.fillRect(bx, by, bw, gy0 - by, bg);
    if (gy1 < by1) G.fillRect(bx, gy1, bw, by1 - gy1, bg);
    if (gy1 > gy0) {
        if (tx > bx) G.fillRect(bx, gy0, tx - bx, gy1 - gy0, bg);
        if (tx + w < bx1) G.fillRect(tx + w, gy0, bx1 - tx - w, gy1 - gy0, bg);
    }
    if (w) { G.setTextColor(fg, bg); G.setCursor(tx, ty); G.print(s); }
    return tx + w;
}
struct VRow { const char *p; unsigned short len; char role; bool first; };
#define VROW_MAX 48
static VRow s_vr[VROW_MAX]; static int s_vrn;
static void vr_push(const char *p, int len, char role, bool first)
{
    if (s_vrn == VROW_MAX) { memmove(&s_vr[0], &s_vr[1], sizeof(VRow) * (VROW_MAX - 1)); s_vrn--; }
    s_vr[s_vrn++] = VRow{ p, (unsigned short)len, role, first };
}
static const lgfx::IFont *role_font(char role, bool big)
{
    if (role == RM_USER || role == RM_TEXT) return big ? (const lgfx::IFont *)&fonts::FreeSans9pt7b : (const lgfx::IFont *)&fonts::Font2;
    return big ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font0;
}
static int role_h(char role, bool big) { return (role == RM_USER || role == RM_TEXT) ? (big ? 20 : 15) : (big ? 15 : 11); }
static int meas(const char *p, int n) { char t[200]; if (n > 199) n = 199; memcpy(t, p, (size_t)n); t[n] = 0; return (int)G.textWidth(t); }
// The last part of the conversation, wrapped by pixel width (at most VROW_MAX rows are kept: the newest)
static void wrap_view(const char *text, int w, bool big)
{
    s_vrn = 0;
    const char *p = text; char role = RM_META;
    while (*p) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        bool first = false;
        if (*p >= RM_USER && *p <= RM_META) { role = *p++; first = true; }
        G.setFont(role_font(role, big));
        const int avail = role == RM_USER ? w - 30 : role == RM_TEXT ? w - 12 : w - 16;
        const char *ls = p;
        if (ls == e) vr_push(ls, 0, role, first);
        while (ls < e) {                                    // greedy word wrap
            const char *q = ls, *best = nullptr;
            while (q < e) {
                const char *we = q; while (we < e && *we == ' ') we++; while (we < e && *we != ' ') we++;
                if (meas(ls, (int)(we - ls)) > avail) break;
                best = we; q = we;
            }
            if (!best) { int take = 1; while (ls + take < e && meas(ls, take + 1) <= avail) take++; best = ls + take; }
            vr_push(ls, (int)(best - ls), role, first); first = false;
            ls = best; while (ls < e && *ls == ' ') ls++;
        }
        p = *e ? e + 1 : e;
    }
}
// a step's mark: running (amber dot), done (green tick), failed (red cross)
static void mark(int x, int y, int sz, char role)
{
    if (role == RM_OK) { G.drawLine(x, y + sz / 2, x + sz / 3, y + sz - 1, C_OK); G.drawLine(x + sz / 3, y + sz - 1, x + sz - 1, y + 1, C_OK);
                         G.drawLine(x, y + sz / 2 + 1, x + sz / 3, y + sz, C_OK); G.drawLine(x + sz / 3, y + sz, x + sz - 1, y + 2, C_OK); }
    else if (role == RM_FAIL) { G.drawLine(x + 1, y + 1, x + sz - 2, y + sz - 2, C_BAD); G.drawLine(x + sz - 2, y + 1, x + 1, y + sz - 2, C_BAD);
                                G.drawLine(x + 2, y + 1, x + sz - 1, y + sz - 2, C_BAD); G.drawLine(x + sz - 1, y + 1, x + 2, y + sz - 2, C_BAD); }
    else G.fillCircle(x + sz / 2, y + sz / 2, sz / 3, ACCENT);
}
// The conversation in [x, x+w) x [y, y+h): the newest rows at the bottom, S->tscroll rows back from there
static void draw_convo(int x, int y, int w, int h, bool big)
{
    const char *text = S->tr;
    if (S->trlen > 2400) { text = S->tr + S->trlen - 2400; while (*text && *text != '\n') text++; if (*text) text++; }   // the tail
    static char plain[2600];                               // the model's markdown made plain (it is asked for plain text)
    code_md_plain(text, plain, sizeof plain);
    wrap_view(plain, w, big);
    int hsum = 0, first = s_vrn;
    const int skip = S->tscroll < s_vrn ? S->tscroll : s_vrn;
    for (int i = s_vrn - 1 - skip; i >= 0; i--) { int rh = role_h(s_vr[i].role, big) + (s_vr[i].first ? 3 : 0); if (hsum + rh > h) break; hsum += rh; first = i; }
    if (S->tscroll > s_vrn - 1) S->tscroll = s_vrn ? s_vrn - 1 : 0;
    int yy = y;
    for (int i = first; i < s_vrn - skip && yy < y + h; i++) {
        const VRow &r = s_vr[i];
        const int gap = r.first ? 3 : 0, rh = role_h(r.role, big) + gap;
        if (gap) G.fillRect(x, yy, w, gap, C_BG);
        G.setFont(role_font(r.role, big));
        char t[200]; int n = r.len < 199 ? r.len : 199; memcpy(t, r.p, (size_t)n); t[n] = 0;
        const int ty = yy + gap, fh = role_h(r.role, big);
        if (r.role == RM_USER) {
            int tw = (int)G.textWidth(t);
            box_text(x, ty, w, fh, x + w - 6 - tw, ty, t, C_USR, C_BG);
        } else if (r.role == RM_TEXT) {
            G.fillRect(x, ty, 3, fh, C_BG); G.fillRect(x + 3, ty, 2, fh, ACCENT);            // Code's accent bar
            box_text(x + 5, ty, w - 5, fh, x + 10, ty, t, C_INK, C_BG);
        } else if (r.role == RM_META) {
            box_text(x, ty, w, fh, x + 4, ty + 1, t, C_DIM, C_BG);
        } else {
            const int sz = big ? 11 : 8;
            G.fillRect(x, ty, 12, fh, C_BG);
            if (r.first) mark(x + 4, ty + (fh - sz) / 2, sz, r.role);
            box_text(x + 12 + sz - 8, ty, w - 12 - (sz - 8), fh, x + 16 + (sz - 8), ty + (big ? 0 : 1), t,
                     r.role == RM_FAIL ? C_BAD : C_DIM, C_BG);
        }
        yy += rh;
    }
    if (yy < y + h) G.fillRect(x, yy, w, y + h - yy, C_BG);
}

static void status_text(char *b, size_t n)
{
    const agent_stats_t *st = S->ag ? agent_stats(S->ag) : nullptr;
    char m[24]; const char *mm = S->model[0] ? S->model : "-";
    if (!strncmp(mm, "claude-", 7)) mm += 7;
    snprintf(m, sizeof m, "%s", mm);
    bool num = false;                                       // "sonnet-5-5" -> "sonnet 5.5"
    for (char *c = m; *c; c++) if (*c == '-' && c[1] >= '0' && c[1] <= '9') { *c = num ? '.' : ' '; num = true; }
    const char *ef = S->effort[0] ? S->effort : "-";
    if (!strcmp(ef, "medium")) ef = "med";
    if (agent_running()) {
        const int sec = (int)((now_ms() - S->phase_t0) / 1000);
        const char *ph = S->phase[0] ? S->phase : S->thinking ? L("pensa", "thinking", "piensa", "reflechit", "denkt") : L("lavora", "working", "trabaja", "travaille", "arbeitet");
        snprintf(b, n, "%s  %ds", ph, sec < 0 ? 0 : sec);
    } else snprintf(b, n, "%s %s  %uk  $%.2f%s", m, ef, st ? (unsigned)(st->last_context / 1000) : 0u,
                    st ? st->usd : 0.0, S->ask ? "  ASK" : "");
}

static const char *agent_hint(void)
{
    agent_state_t s = S->ag ? agent_state(S->ag) : AG_IDLE;
    if (s == AG_ASK) return L("y si  n no  (approva)", "y yes  n no  (approve)", "y si  n no", "y oui  n non", "y ja  n nein");
    if (agent_running()) return L("del ferma  su/giu scorri", "del stops  up/dn scroll", "del para", "suppr arrete", "entf stoppt");
    static char h[64];
    if (!S->inlen && S->last_js[0]) {
        snprintf(h, sizeof h, "%s %s  %s", L("invio esegui", "enter run", "intro ejecuta", "entree lance", "enter startet"), S->last_js,
                 S->from_anima ? "esc ANIMA" : "/files");
        return h;
    }
    if (!S->trlen && !S->inlen) return L("1-3 esempi  /files  /new  /mode", "1-3 examples  /files /new /mode", "1-3 ejemplos  /files", "1-3 exemples  /files", "1-3 Beispiele  /files");
    if (S->from_anima) return L("scrivi e invio  esc torna ad ANIMA", "type, enter  esc back to ANIMA", "escribe  esc a ANIMA", "ecris  esc vers ANIMA", "tippen  esc zu ANIMA");
    return L("scrivi e invio  /files /new /mode", "type, enter  /files /new /mode", "escribe e intro  /files /new", "ecris, entree  /files /new", "tippen, enter  /files /new");
}

// the empty conversation: what Code is for, and three tasks one key away (1-3 fill the input)
static const char *example(int i)
{
    switch (i) {
    case 0: return L("Scrivi un gioco del serpente per le frecce e provalo", "Write a snake game for the arrow keys and test it", "Escribe un juego de la serpiente y pruebalo",
                     "Ecris un jeu du serpent et teste-le", "Schreib ein Snake-Spiel und teste es");
    case 1: return L("Fai un'app che conta i giorni a una data, con lo schermo", "Make an app that counts the days to a date, on screen", "Haz una app que cuente los dias a una fecha",
                     "Fais une app qui compte les jours jusqu'a une date", "Mach eine App, die die Tage bis zu einem Datum zaehlt");
    default: return L("Scrivi note.txt con una lista della spesa e un riassunto", "Write notes.txt with a shopping list and a summary", "Escribe notes.txt con una lista de compras",
                      "Ecris notes.txt avec une liste de courses", "Schreib notes.txt mit einer Einkaufsliste");
    }
}
static void draw_welcome(int x, int y, int w, int h, bool big)
{
    G.fillRect(x, y, w, h, C_BG);
    G.setFont(big ? (const lgfx::IFont *)&fonts::FreeSansBold9pt7b : (const lgfx::IFont *)&fonts::Font2);
    G.setTextColor(ACCENT, C_BG); G.setCursor(x + 6, y + 4);
    G.print(L("Cosa costruiamo?", "What shall we build?", "Que construimos?", "On construit quoi ?", "Was bauen wir?"));
    G.setFont(big ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font0);
    const int lh = big ? 22 : 15, y0 = y + (big ? 30 : 24);
    for (int i = 0; i < 3 && y0 + i * lh + lh <= y + h; i++) {
        const int yy = y0 + i * lh;
        G.fillRoundRect(x + 6, yy, big ? 18 : 12, big ? 18 : 11, 3, lg::card());
        G.setTextColor(ACCENT, lg::card()); G.setCursor(x + (big ? 11 : 9), yy + (big ? 2 : 2)); char k[2] = { (char)('1' + i), 0 }; G.print(k);
        char t[80]; snprintf(t, sizeof t, "%s", example(i));
        while ((int)G.textWidth(t) > w - (big ? 34 : 26) && strlen(t) > 4) { size_t n = strlen(t); t[n - 4] = '.'; t[n - 3] = '.'; t[n - 2] = 0; }
        G.setTextColor(C_INK, C_BG); G.setCursor(x + (big ? 30 : 22), yy + (big ? 2 : 2)); G.print(t);
        if (big) nucleo_touch_add(x, yy - 2, w, lh, NK_CHAR, (char)('1' + i));   // on the panel: tap the example
    }
}

// the input field: what you type, the question waiting for y/n, or what Enter does now
static void draw_input(int x, int y, int w, int h, bool big)
{
    const char *q = S->ag ? agent_question(S->ag) : nullptr;
    G.fillRect(x, y, w, 1, C_BG);
    G.fillRoundRect(x + 2, y + 1, w - 4, h - 1, 4, lg::card());
    G.setFont(big ? (const lgfx::IFont *)&fonts::FreeSans9pt7b : (const lgfx::IFont *)&fonts::Font2);
    char b[96]; unsigned short col = C_INK;
    if (q) { snprintf(b, sizeof b, "%.60s?  y/n", q); col = C_ASK; }
    else if (!S->inlen) {
        col = C_DIM;
        if (agent_running()) snprintf(b, sizeof b, "%s", L("Code lavora...", "Code is working...", "Code trabaja...", "Code travaille...", "Code arbeitet..."));
        else if (S->last_js[0]) snprintf(b, sizeof b, "%s %s", L("Invio: esegui", "Enter: run", "Intro: ejecuta", "Entree : lance", "Enter: startet"), S->last_js);
        else snprintf(b, sizeof b, "%s", L("Chiedi a Code...", "Ask Code...", "Pide a Code...", "Demande a Code...", "Frag Code..."));
    } else {
        int off = 0;                                       // the end of what you type stays in view
        while (off < S->inlen && meas(S->in + off, S->inlen - off) > w - 24) off++;
        snprintf(b, sizeof b, "%s%s", S->in + off, (esp_timer_get_time() / 500000) & 1 ? "_" : " ");
    }
    G.setTextColor(col, lg::card()); G.setCursor(x + 8, y + (h - (int)G.fontHeight()) / 2 + 1); G.print(b);
}

static void draw_header(int x, int y, int w, int h, bool big)
{
    G.fillRect(x, y, w, h, lg::card());
    G.setFont(big ? (const lgfx::IFont *)&fonts::FreeSansBold9pt7b : (const lgfx::IFont *)&fonts::Font2);
    G.setTextColor(ACCENT, lg::card()); G.setCursor(x + 6, y + (h - (int)G.fontHeight()) / 2 + 1); G.print("Code");
    char st[64]; status_text(st, sizeof st);
    G.setFont(big ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font0);
    const int tw = (int)G.textWidth(st);
    G.setTextColor(agent_running() ? ACCENT : C_DIM, lg::card()); G.setCursor(x + w - 6 - tw, y + (h - (int)G.fontHeight()) / 2 + 1); G.print(st);
    if (agent_running()) {                                 // a slim progress pulse under the header while it works
        const int t = (int)((now_ms() / 40) % (w + 40)) - 40;
        G.fillRect(x, y + h - 1, w, 1, lg::card());
        G.fillRect(x + (t < 0 ? 0 : t), y + h - 1, 40 + (t < 0 ? t : 0), 1, ACCENT);
    }
}

static void agent_draw(void)
{
    G_ = &d;
    if (nucleo_app_two_screens()) {                          // companion: the status and the input, the panel has the rest
        const int top = nucleo_app_content_top(), H = nucleo_app_content_height();
        if (!s_cleared) { s_cleared = true; d.fillRect(0, top, 240, H, C_BG); }
        draw_header(0, top, 240, 18, false);
        if (!S->trlen) { G.fillRect(0, top + 18, 240, H - 18 - 24, C_BG); G.setFont(&fonts::Font2); G.setTextColor(C_DIM, C_BG);
                         G.setCursor(6, top + 26); G.print(L("Scrivi qui il compito:", "Type the task here:", "Escribe aqui la tarea:", "Ecris la tache ici :", "Aufgabe hier tippen:")); }
        else draw_convo(0, top + 18, 240, H - 18 - 24, false);   // the newest rows, as you type the next task
        draw_input(0, top + H - 24, 240, 24, false);
        nucleo_app_set_hint(agent_hint());
        return;
    }
    const int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    if (!s_cleared) { s_cleared = true; d.fillRect(0, top, 240, bottom - top, C_BG); }
    draw_header(0, top, 240, 18, false);
    const int ih = 22, cy = top + 18, ch = bottom - top - 18 - ih;
    if (!S->trlen) draw_welcome(0, cy, 240, ch, false);
    else draw_convo(0, cy, 240, ch, false);
    draw_input(0, bottom - ih, 240, ih, false);
    nucleo_app_set_hint(agent_hint());
}

static void agent_draw_large(void)
{
    G_ = &d;
    nucleo_touch_clear();
    const int W = d.width(), H = d.height();
    int y = lg::header("Code", S->thinking ? L("pensa...", "thinking...", "piensa...", "reflechit...", "denkt...") : "work/main", ACCENT);
    char st[64]; status_text(st, sizeof st);
    d.setFont(&fonts::Font2); d.setTextColor(agent_running() ? ACCENT : C_DIM, C_BG);
    d.fillRect(0, y, W, 18, C_BG); d.setCursor(8, y + 2); d.print(st);
    int ty = y + 20;
    if (S->todo[0]) {                                        // the plan, when the agent keeps one
        int nl = 0; for (const char *p = S->todo; *p; p++) if (*p == '\n') nl++;
        if (nl > 4) nl = 4;
        const int plan_h = nl * 16 + 6;
        lg::panel(4, ty, W - 8, plan_h, lg::card());
        d.setFont(&fonts::Font2);
        int yy = ty + 3, k = 0;
        for (const char *p = S->todo; *p && k < 4; k++) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            char t[64]; size_t n = (size_t)(e - p) < 63 ? (size_t)(e - p) : 63; memcpy(t, p, n); t[n] = 0;
            d.setTextColor(t[1] == 'x' ? C_OK : t[1] == '>' ? ACCENT : C_DIM, lg::card()); d.setCursor(10, yy); d.print(t); yy += 16;
            p = *e ? e + 1 : e;
        }
        ty += plan_h + 4;
    }
    if (!S->trlen) draw_welcome(0, ty, W, H - ty, true);
    else draw_convo(2, ty, W - 4, H - ty - 2, true);
    nucleo_touch_commit();
}

static void agent_key(int key, char ch)
{
    agent_state_t s = S->ag ? agent_state(S->ag) : AG_IDLE;
    if (s == AG_ASK) {
        if (ch == 'y' || ch == 'Y' || key == NK_ENTER) agent_answer(S->ag, true);
        else if (ch == 'n' || ch == 'N') agent_answer(S->ag, false);
        nucleo_app_request_draw(); return;
    }
    if (agent_running()) {
        if (key == NK_DEL) { nucleo_llm_cancel(); agent_cancel(S->ag, nullptr); }
        else if (key == NK_UP) S->tscroll++;
        else if (key == NK_DOWN && S->tscroll > 0) S->tscroll--;
        nucleo_app_request_draw(); return;
    }
    if (key == NK_ENTER && !S->inlen && S->last_js[0]) { run_script_here(S->last_js); nucleo_app_request_draw(); return; }
    if (!S->trlen && !S->inlen && ch >= '1' && ch <= '3') {   // an example, ready to send or to edit
        snprintf(S->in, sizeof S->in, "%s", example(ch - '1')); S->inlen = (int)strlen(S->in); nucleo_app_request_draw(); return;
    }
    if (key == NK_ENTER) { agent_send(); return; }
    if (key == NK_DEL) { if (S->inlen) S->in[--S->inlen] = 0; }
    else if (key == NK_UP && !S->inlen) S->tscroll++;
    else if (key == NK_DOWN && S->tscroll > 0 && !S->inlen) S->tscroll--;
    else if (ch >= 32 && ch < 127 && S->inlen < (int)sizeof S->in - 1) { S->in[S->inlen++] = ch; S->in[S->inlen] = 0; }
    nucleo_app_request_draw();
}

#ifdef NUCLEO_UI_HOST
// ui-host gallery scenes (tools/ui-host/apps/code.txt): 0 = the welcome, 1 = a finished task, 2 = a task at work,
// 3 = a job from ANIMA, done. A fake conversation: what the callbacks would have written.
extern "C" void ui_host_scene(int n)
{
    S->trlen = 0; S->tr[0] = 0; s_role = 0; s_cleared = false; S->tscroll = 0; S->last_js[0] = 0; S->phase[0] = 0;
    s_host_running = n == 2; S->from_anima = n == 3; S->thinking = false;
    snprintf(S->model, sizeof S->model, "claude-sonnet-5-5"); snprintf(S->effort, sizeof S->effort, "medium");
    if (!n) return;
    tr_line(RM_USER, n == 3 ? "scrivimi un gioco del serpente da giocare con le frecce" : "Scrivi un gioco del serpente per le frecce e provalo");
    { const char *t = "Lo scrivo e lo provo sui due schermi."; ui_text(nullptr, t, strlen(t)); }
    ui_tool(nullptr, "todo_write", "plan 0/3"); ui_tool_done(nullptr, "todo_write", true, "plan 0/3");
    ui_tool(nullptr, "str_replace_based_edit_tool", "create snake.js");
    if (n == 2) { snprintf(S->phase, sizeof S->phase, "create snake.js"); return; }
    ui_tool_done(nullptr, "str_replace_based_edit_tool", true, "create snake.js +105");
    ui_tool(nullptr, "run_js", "run snake.js"); ui_tool_done(nullptr, "run_js", false, "run snake.js FAILED 31ms snake.js:12");
    ui_tool(nullptr, "str_replace_based_edit_tool", "edit snake.js"); ui_tool_done(nullptr, "str_replace_based_edit_tool", true, "edit snake.js:12 +2 -1");
    ui_tool(nullptr, "run_js", "run snake.js"); ui_tool_done(nullptr, "run_js", true, "run snake.js ok 2500ms +screen");
    s_role = 0;
    const char *t = "Fatto: snake.js. Le frecce muovono il serpente, la mela lo fa crescere, invio ricomincia dopo il game over. "
                    "L'ho provato sui due schermi guardando gli screenshot.";
    ui_text(nullptr, t, strlen(t));
}
#endif

// ---- routing --------------------------------------------------------------------------------------------
static void on_draw(void) { static bool t1; if (!t1) { t1 = true; trace("first draw"); } if (S->agent_view) agent_draw(); else files_draw(); if (!S->traced_draw) { S->traced_draw = true; trace("first draw done"); } }
static void on_draw_large(void) { if (S->agent_view) agent_draw_large(); else draw_large(); }
static void on_key(int key, char ch) { if (S->agent_view) agent_key(key, ch); else files_key(key, ch); }
// SELF-TEST (maintainer only): work/main/.code/selftest.txt holds one task; Code sends it once on open, then saves the
// conversation and the figures to work/main/.code/selftest-report.txt and goes back to the full OS (where the report
// can be read). The trigger is removed first.
#define SELFTEST_IN  AWS "/.code/selftest.txt"
#define SELFTEST_OUT AWS "/.code/selftest-report.txt"
static void selftest_tick(void)
{
    if (!S->auto_run) return;
    if (!S->auto_t0) S->auto_t0 = now_ms();
    const int64_t waited = now_ms() - S->auto_t0;
    if (!S->auto_sent) {
        if (!nucleo_tcp_network_up() && waited < 40000) return;  // Wi-Fi first (the Solo boot brings it up after us)
        if (!nucleo_tcp_network_up()) {                           // never a self-test stuck in the Solo boot
            trace("no network after 40 s");
            remove(SELFTEST_IN);
            FILE *o = fopen(SELFTEST_OUT, "wb"); if (o) { fputs("Code self-test: no network after 40 s\n", o); fclose(o); }
            S->auto_run = false; nucleo_app_exit(); return;
        }
        if (!S->traced_net) { S->traced_net = true; trace("net up"); }
        FILE *f = fopen(SELFTEST_IN, "rb"); if (!f) { S->auto_run = false; return; }
        size_t n = fread(S->in, 1, sizeof S->in - 1, f); fclose(f); remove(SELFTEST_IN);
        while (n && (S->in[n - 1] == '\n' || S->in[n - 1] == '\r')) n--;
        S->in[n] = 0; S->inlen = (int)n; S->auto_sent = true;
        trace("submit");
        agent_send();
        trace("submitted");
        return;
    }
    if (S->ag && agent_running() && waited > 600000) {          // a whole task bounded at 10 minutes
        trace("self-test: 10 minutes, stopping"); nucleo_llm_cancel(); agent_cancel(S->ag, "self-test time limit");
    }
    if (agent_running() || !S->ag) return;
    FILE *o = fopen(SELFTEST_OUT, "wb");
    if (o) {
        const agent_stats_t *st = agent_stats(S->ag);
        fprintf(o, "Code self-test  model=%s effort=%s  state=%d  calls=%u tools=%u errors=%u  in=%llu out=%llu cache=%llu  $%.4f\n"
                   "error: %s\nfree %u largest %u\n\n%s\n\nplan:\n%s\n",
                S->model, S->effort, (int)agent_state(S->ag), (unsigned)st->calls, (unsigned)st->tools, (unsigned)st->tool_errors, (unsigned long long)st->in,
                (unsigned long long)st->out, (unsigned long long)st->cache_read, st->usd, st->error,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), S->tr, S->todo);
        fclose(o);
    }
    S->auto_run = false;
    nucleo_app_exit();
}
static void on_tick(void)
{
    agent_tick(); selftest_tick(); inbox_tick();
    static int64_t blink;                                   // the input cursor: two redraws a second, nothing more
    const int every = agent_running() ? 250 : 500;          // the cursor, and the header's pulse while it works
    if (S->agent_view && !nucleo_app_two_screens() && now_ms() - blink >= every) { blink = now_ms(); nucleo_app_request_draw(); }
}
static void on_enter(void)
{
    trace("enter");
    nucleo_app_set_direct_draw(true); s_cleared = false;                       // the launcher's 32 KB canvas goes back to the heap: it is the TLS
    ensure_bench(); mkdir(AWS, 0775); scan(AWS); S->output = false; s_lg_scroll = 0;
    S->agent_view = true; S->ask = false;
    { struct stat sst; S->auto_run = stat(SELFTEST_IN, &sst) == 0; S->auto_sent = false; }
    S->from_anima = S->inbox_task = S->outbox_done = false; S->run_job[0] = 0; S->last_js[0] = 0; S->last_tlen = 0;
    if (!S->auto_run) inbox_read();                         // a job from ANIMA
    nucleo_app_set_back_handler(back_key);
    S->have_key = nucleo_llm_config(S->model, sizeof S->model, S->effort, sizeof S->effort);
    S->trlen = 0; S->tr[0] = 0; s_role = 0; S->tscroll = 0; S->phase[0] = 0;   // the welcome screen, or the job
    if (!S->have_key) tr_note(L("Serve una chiave Claude: web OS > Impostazioni > IA.", "A Claude key is needed: web OS > Settings > AI.",
                                "Hace falta una clave Claude: web OS > Ajustes > IA.", "Une cle Claude est requise : web OS > Reglages > IA.",
                                "Ein Claude-Schluessel fehlt: Web OS > Einstellungen > KI."));
    trace("enter done");
    nucleo_app_request_draw();
}
static void on_exit(void) { trace("exit"); agent_close(); }

static const nucleo_app_ram_t APP_RAM[] = { { (void **)&S, sizeof(State) }, { nullptr, 0 } };

extern "C" void nucleo_register_code(void)
{
    static const nucleo_app_def_t app = {
        "code", "Code", "Tools", "Nucleo Code",
        'C', ACCENT, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_SOLO,          // a fresh heap with Wi-Fi up: the model's TLS and the engine's arena take turns
        APP_RAM, on_draw_large
    };
    nucleo_app_register(&app);
}
#endif  // NAP_MODULE
