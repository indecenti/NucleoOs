// app_code.cpp: Nucleo Code (docs/nucleo-code.md). PHASE 1 = the runtime and its bench on the device: the
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
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "nucleo_exclusive.h"  // NX_SOLO
#include "nucleo_theme.h"
#include "app_large.h"        // lg:: two-screen kit
#include "nucleo_touch.h"
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
};
static State *S;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
static void out_fn(void *op, const char *s, size_t n)
{
    (void)op;
    size_t room = OUT_CAP - 1 - (size_t)S->olen;
    if (n > room) n = room;
    memcpy(S->out + S->olen, s, n); S->olen += (int)n; S->out[S->olen] = 0;
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

static void scan(void)
{
    S->nfiles = 0;
    DIR *dd = opendir(BENCH); if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd)) && S->nfiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n > 3 && n < sizeof S->files[0] && !strcmp(e->d_name + n - 3, ".js")) snprintf(S->files[S->nfiles++], sizeof S->files[0], "%s", e->d_name);
    }
    closedir(dd);
    if (S->sel >= S->nfiles) S->sel = 0;
}

// One run: the arena is the largest block minus a margin, capped; it exists only for the run.
static bool run_file(const char *name, uint32_t timeout_ms)
{
    S->olen = 0; S->out[0] = 0; S->scroll = 0;
    snprintf(S->ran, sizeof S->ran, "%s", name);
    char p[96]; snprintf(p, sizeof p, BENCH "/%s", name);
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(S->res.error, sizeof S->res.error, "cannot open %s", name); S->res.ok = false; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 32 * 1024) { fclose(f); snprintf(S->res.error, sizeof S->res.error, "%s: bigger than 32 KB", name); S->res.ok = false; return false; }
    char *src = (char *)malloc((size_t)n + 1);
    if (!src) { fclose(f); snprintf(S->res.error, sizeof S->res.error, "no RAM for the source"); S->res.ok = false; return false; }
    fread(src, 1, (size_t)n, f); fclose(f); src[n] = 0;

    S->heap_before  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    S->block_before = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t arena = S->block_before > 4096 ? S->block_before - 2048 : 0;
    if (arena > ARENA_MAX) arena = ARENA_MAX;
    arena &= ~(size_t)7;
    void *mem = arena ? heap_caps_malloc(arena, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
    S->arena = (unsigned)(mem ? arena : 0);
    if (!mem) { free(src); snprintf(S->res.error, sizeof S->res.error, "no block for the arena"); S->res.ok = false; return false; }
    code_env_t env = {};
    env.root = BENCH; env.out = out_fn; env.now_ms = now_ms; env.sleep_ms = sleep_ms; env.timeout_ms = timeout_ms;
    bool ok = code_run(mem, arena, src, (size_t)n, name, &env, &S->res);
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
    for (int i = 0; i < S->nfiles && rl < 3900; i++) {
        run_file(S->files[i], 3000);
        rl += snprintf(rep + rl, 4096 - rl, "%-16s %-3s %-6u %-8u  %-6u  %6u/%-6u      %6u/%-6u  %s %s\n", S->files[i], S->res.ok ? "yes" : "no",
                       (unsigned)S->res.ms, (unsigned)S->res.live_max, S->arena, S->heap_before, S->block_before, S->heap_after, S->block_after,
                       S->res.error, S->res.where);
        nucleo_app_request_draw();
    }
    FILE *f = fopen(BENCH "/report.txt", "wb");
    if (f) { fwrite(rep, 1, (size_t)rl, f); fclose(f); }
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
    int y = lg::header("Code", "work/bench", ACCENT);
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

static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int y = app_ui_title("Code", ACCENT, S->output ? S->ran : "phase 1: runtime");
    if (S->output) { draw_output(y); nucleo_app_set_hint(L("su/giu scorri  invio lista  b banco", "up/dn scroll  enter list  b bench", "arr/ab  intro lista  b banco", "haut/bas  entree liste  b banc", "auf/ab  enter Liste  b Bench")); return; }
    int bottom = nucleo_app_content_top() + nucleo_app_content_height();
    app_ui_list(y, bottom - y, S->nfiles, S->sel, row_label, row_right, row_color, nullptr);
    nucleo_app_set_hint(L("invio esegui  b banco completo", "enter run  b full bench", "intro ejecuta  b banco", "entree lancer  b banc", "enter starten  b Bench"));
}

static void on_key(int key, char ch)
{
    if (ch == 'b' || ch == 'B') { bench_all(); nucleo_app_request_draw(); return; }
    if (nucleo_app_two_screens()) {                          // taps on the panel's list arrive as keys
        int t = lg::row_tap(key, ch, s_lg_scroll, S->nfiles, &S->sel);
        if (t == 2 && S->nfiles) { run_file(S->files[S->sel], 5000); S->output = true; }
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
    else if (key == NK_ENTER && S->nfiles) { run_file(S->files[S->sel], 5000); S->output = true; }
    nucleo_app_request_draw();
}

static void on_enter(void) { ensure_bench(); scan(); S->output = false; s_lg_scroll = 0; nucleo_app_request_draw(); }
static void on_exit(void) {}

static const nucleo_app_ram_t APP_RAM[] = { { (void **)&S, sizeof(State) }, { nullptr, 0 } };

extern "C" void nucleo_register_code(void)
{
    static const nucleo_app_def_t app = {
        "code", "Code", "Tools", "Nucleo Code",
        'C', ACCENT, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_SOLO,          // a fresh heap with Wi-Fi up: the model's TLS and the engine's arena take turns
        APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
#endif  // NAP_MODULE
