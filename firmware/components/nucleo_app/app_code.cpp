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
    // the agent
    bool agent_view, ask, have_key, thinking, was_running;
    agent_t *ag; char *toolbuf;
    char model[48], effort[8];
    char tr[6144]; int trlen, tscroll;
    char in[160]; int inlen;
    char todo[512];
    bool auto_run, auto_sent, traced_draw, traced_net;
    int64_t auto_t0;     // the maintainer's self-test (work/main/.code/selftest.txt)
};
static State *S;

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

static bool s_cleared;                // the agent view painted its background once since it came up
static void files_draw(void)
{
    s_cleared = false;
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int y = app_ui_title("Code", ACCENT, S->output ? S->ran : "work/bench");
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

// ==== the agent (docs/nucleo-code.md 9): status bar, the conversation, the input; driven from the tick ====
#define AWS       WS "/main"            // the workspace the agent works in
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
static void ui_text(void *, const char *s, size_t n) { tr_add(s, n); }
static void ui_tool(void *, const char *, const char *summary) { char b[180]; snprintf(b, sizeof b, "\n  > %s", summary); tr_puts(b); }
static void ui_tool_done(void *, const char *, bool ok, const char *summary) { char b[180]; snprintf(b, sizeof b, "\n  %s %s\n", ok ? "ok" : "!!", summary); tr_puts(b); }
static void ui_todo(void *, const char *t) { snprintf(S->todo, sizeof S->todo, "%s", t); nucleo_app_request_draw(); }

static bool agent_running(void)
{
    agent_state_t s = S->ag ? agent_state(S->ag) : AG_IDLE;
    return s == AG_SEND || s == AG_RECV || s == AG_TOOLS || s == AG_ASK;
}

static void agent_open(void)
{
    if (S->ag) return;
    mkdir(WS, 0775); mkdir(AWS, 0775);
    S->have_key = nucleo_llm_config(S->model, sizeof S->model, S->effort, sizeof S->effort);
    if (!S->have_key) return;
    if (!S->toolbuf) S->toolbuf = (char *)malloc(TOOL_CAP);
    if (!S->toolbuf) return;
    agent_cfg_t cfg = {};
    cfg.root = AWS; cfg.model = S->model; cfg.effort = S->effort;
    cfg.lang = L("Italian", "English", "Spanish", "French", "German");
    cfg.max_calls = 30; cfg.max_tokens = 16000; cfg.arena = nullptr; cfg.arena_size = 32768;
    cfg.toolbuf = S->toolbuf; cfg.toolcap = TOOL_CAP; cfg.ask_writes = S->ask;
    agent_io_t io = {};
    io.on_text = ui_text; io.on_tool = ui_tool; io.on_tool_done = ui_tool_done; io.on_todo = ui_todo;
    io.now_ms = now_ms; io.sleep_ms = sleep_ms;
    S->ag = agent_new(&cfg, &io);
}
static void agent_close(void)
{
    if (nucleo_llm_busy()) { nucleo_llm_cancel(); for (int i = 0; i < 40 && nucleo_llm_busy(); i++) vTaskDelay(pdMS_TO_TICKS(50)); }
    if (S->ag) { agent_free(S->ag); S->ag = nullptr; }
    free(S->toolbuf); S->toolbuf = nullptr;
}

// the tick: the request goes out, its bytes come in, one tool runs, a question waits
static void agent_tick(void)
{
    if (!S->ag) return;
    switch (agent_state(S->ag)) {
    case AG_SEND:
        if (nucleo_llm_busy()) break;
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
    case AG_TOOLS: agent_step(S->ag); nucleo_app_request_draw(); break;
    case AG_DONE: case AG_ERROR:
        if (S->was_running) {
            const agent_stats_t *st = agent_stats(S->ag);
            char b[240];
            if (agent_state(S->ag) == AG_ERROR) snprintf(b, sizeof b, "\n[%s]\n", st->error);
            else snprintf(b, sizeof b, "\n");
            tr_puts(b);
            S->was_running = false;
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
        if (!strcmp(S->in, "/files")) S->agent_view = false;
        else if (!strcmp(S->in, "/new")) { if (S->ag) agent_reset(S->ag); S->trlen = 0; S->tr[0] = 0; S->todo[0] = 0; }
        else if (!strcmp(S->in, "/mode")) { S->ask = !S->ask; agent_close(); agent_open(); tr_puts(S->ask ? "\n[mode: Ask]\n" : "\n[mode: Auto]\n"); }
        else tr_puts(L("\n[comandi: /files /new /mode]\n", "\n[commands: /files /new /mode]\n", "\n[/files /new /mode]\n", "\n[/files /new /mode]\n", "\n[/files /new /mode]\n"));
        S->inlen = 0; S->in[0] = 0; nucleo_app_request_draw(); return;
    }
    agent_open();
    if (!S->ag) { tr_puts(L("\n[nessuna chiave Claude: aggiungila nella web OS, Impostazioni > IA]\n", "\n[no Claude key: add one in the web OS, Settings > AI]\n",
                            "\n[sin clave Claude: web OS, Ajustes > IA]\n", "\n[pas de cle Claude : web OS, Reglages > IA]\n", "\n[kein Claude-Schluessel: Web OS, Einstellungen > KI]\n")); return; }
    char b[200]; snprintf(b, sizeof b, "\n> %s\n", S->in); tr_puts(b);
    agent_submit(S->ag, S->in);
    S->inlen = 0; S->in[0] = 0;
    S->was_running = true;
    nucleo_app_request_draw();
}

// the conversation as plain lines of `cols` columns; the last `rows` lines (minus the scroll) are drawn
static void draw_lines(int x, int y, int w, int rows, int lh, unsigned short bg)
{
    static char plain[TR_CAP];
    code_md_plain(S->tr, plain, sizeof plain);
    const int cols = w / 6 > 60 ? 60 : w / 6;
    // count wrapped lines, then draw the window
    int total = 0;
    for (const char *p = plain; *p; ) { const char *e = p; int c = 0; while (*e && *e != '\n' && c < cols) { e++; c++; } total++; p = *e == '\n' ? e + 1 : e; }
    int first = total - rows - S->tscroll; if (first < 0) first = 0;
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    int line = 0, yy = y;
    for (const char *p = plain; *p && yy + lh <= y + rows * lh; ) {
        const char *e = p; int c = 0; while (*e && *e != '\n' && c < cols) { e++; c++; }
        if (line >= first) {
            char t[64]; int tl = (int)(e - p); memcpy(t, p, (size_t)tl);
            while (tl < cols) t[tl++] = ' ';             // padded to the width: it overwrites the old row, no fill needed
            t[tl] = 0;
            unsigned short col = t[0] == '>' ? ACCENT : (t[0] == ' ' && t[1] == ' ' && t[2] == '!') ? 0xF800 : (t[0] == ' ' && t[1] == ' ') ? C_DIM : t[0] == '[' ? 0xFFE0 : C_INK;
            d.setTextColor(col, bg); d.drawString(t, x, yy); yy += lh;
        }
        line++; p = *e == '\n' ? e + 1 : e;
    }
    if (yy < y + rows * lh) d.fillRect(x, yy, w, y + rows * lh - yy, bg);   // below a short conversation
}

static void status_text(char *b, size_t n)
{
    const agent_stats_t *st = S->ag ? agent_stats(S->ag) : nullptr;
    char m[24]; const char *mm = S->model[0] ? S->model : "-";
    if (!strncmp(mm, "claude-", 7)) mm += 7;
    snprintf(m, sizeof m, "%s", mm);
    snprintf(b, n, "main  %s  %s  ctx %uk  $%.3f  %s", m, S->effort[0] ? S->effort : "-", st ? (unsigned)(st->last_context / 1000) : 0u,
             st ? st->usd : 0.0, S->ask ? "ASK" : "AUTO");
}

static const char *agent_hint(void)
{
    agent_state_t s = S->ag ? agent_state(S->ag) : AG_IDLE;
    if (s == AG_ASK) return L("y si  n no  (approva)", "y yes  n no  (approve)", "y si  n no", "y oui  n non", "y ja  n nein");
    if (agent_running()) return L("lavoro...  del: ferma", "working...  del: stop", "trabajando...  del: parar", "travail...  del: stop", "arbeite...  del: stopp");
    return L("scrivi e invio  /files /new /mode", "type, enter  /files /new /mode", "escribe e intro  /files /new", "ecris, entree  /files /new", "tippen, enter  /files /new");
}

static void agent_draw(void)
{
    char st[96]; status_text(st, sizeof st);
    if (nucleo_app_two_screens()) {                          // companion: the status, the question or the input
        d.fillRect(0, nucleo_app_content_top(), 240, nucleo_app_content_height(), C_BG);
        int y = lg::comp_head("Code", ACCENT);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(C_DIM, C_BG); d.drawString(st, 4, y + 2);
        d.setFont(&fonts::Font2);
        const char *q = S->ag ? agent_question(S->ag) : nullptr;
        char b[200];
        if (q) { snprintf(b, sizeof b, "%s?", q); d.setTextColor(0xFFE0, C_BG); }
        else { snprintf(b, sizeof b, "> %s_", S->in); d.setTextColor(C_INK, C_BG); }
        lg::font(1); lg::wrap(b, 4, y + 16, 232, 16, 3);
        lg::comp_keys(agent_hint());
        return;
    }
    // Drawn straight on the panel (the 32 KB canvas is the model's TLS): every row overwrites itself, nothing is
    // cleared first, so a redraw never flickers.
    const int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    if (!s_cleared) { s_cleared = true; d.fillRect(0, top, 240, bottom - top, C_BG); }   // once: the gaps between rows
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    { int sl = (int)strlen(st); while (sl < 40 && sl < (int)sizeof st - 1) st[sl++] = ' '; st[sl] = 0; }
    // the status band (top .. top+9): its text row overwrites itself, the edges around it are filled
    d.fillRect(0, top, 240, 1, lg::card()); d.fillRect(0, top + 1, 2, 8, lg::card()); d.fillRect(0, top + 9, 240, 1, lg::card());
    d.setTextColor(C_DIM, lg::card()); d.drawString(st, 2, top + 1);
    // the conversation (top+10 .. bottom-12): 39 padded columns from x 2, the margins filled
    const int rows = (bottom - top - 10 - 12) / 9, lines_end = top + 11 + rows * 9;
    d.fillRect(0, top + 10, 240, 1, C_BG);
    d.fillRect(0, top + 11, 2, rows * 9, C_BG); d.fillRect(236, top + 11, 4, rows * 9, C_BG);
    if (lines_end < bottom - 11) d.fillRect(0, lines_end, 240, bottom - 11 - lines_end, C_BG);
    draw_lines(2, top + 11, 234, rows, 9, C_BG);
    const char *q = S->ag ? agent_question(S->ag) : nullptr;
    char b[64];
    if (q) snprintf(b, sizeof b, "%.50s? y/n", q);
    else { int off = S->inlen > 36 ? S->inlen - 36 : 0; snprintf(b, sizeof b, "> %s%s", S->in + off, (esp_timer_get_time() / 500000) & 1 ? "_" : " "); }
    { int bl = (int)strlen(b); while (bl < 40 && bl < (int)sizeof b - 1) b[bl++] = ' '; b[bl] = 0; }
    d.fillRect(0, bottom - 11, 240, 1, lg::card()); d.fillRect(0, bottom - 10, 2, 8, lg::card()); d.fillRect(0, bottom - 2, 240, 2, lg::card());
    d.setTextColor(q ? 0xFFE0 : C_INK, lg::card()); d.drawString(b, 2, bottom - 10);
    nucleo_app_set_hint(agent_hint());
}

static void agent_draw_large(void)
{
    nucleo_touch_clear();
    const int W = d.width(), H = d.height();
    char st[96]; status_text(st, sizeof st);
    int y = lg::header("Code", S->thinking ? L("pensa...", "thinking...", "piensa...", "reflechit...", "denkt...") : "main", ACCENT);
    d.fillRect(0, y, W, H - y, C_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left); d.setTextColor(C_DIM, C_BG); d.drawString(st, 6, y + 2);
    int ty = y + 14, plan_h = 0;
    if (S->todo[0]) {                                        // the plan, when the agent keeps one
        int nl = 0; for (const char *p = S->todo; *p; p++) if (*p == '\n') nl++;
        if (nl > 5) nl = 5;
        plan_h = nl * 10 + 4;
        lg::panel(4, ty, W - 8, plan_h, lg::card());
        d.setTextColor(0xFFE0, lg::card());
        int yy = ty + 2, k = 0;
        for (const char *p = S->todo; *p && k < 5; k++) { const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p); char t[56]; size_t n = (size_t)(e - p) < 55 ? (size_t)(e - p) : 55; memcpy(t, p, n); t[n] = 0; d.drawString(t, 8, yy); yy += 10; p = *e ? e + 1 : e; }
        ty += plan_h + 4;
    }
    draw_lines(6, ty, W - 12, (H - ty - 4) / 10, 10, C_BG);
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
    if (key == NK_ENTER) { agent_send(); return; }
    if (key == NK_DEL) { if (S->inlen) S->in[--S->inlen] = 0; }
    else if (key == NK_UP && !S->inlen) S->tscroll++;
    else if (key == NK_DOWN && S->tscroll > 0 && !S->inlen) S->tscroll--;
    else if (ch >= 32 && ch < 127 && S->inlen < (int)sizeof S->in - 1) { S->in[S->inlen++] = ch; S->in[S->inlen] = 0; }
    nucleo_app_request_draw();
}

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
    agent_tick(); selftest_tick();
    static int64_t blink;                                   // the input cursor: two redraws a second, nothing more
    if (S->agent_view && !nucleo_app_two_screens() && now_ms() - blink >= 500) { blink = now_ms(); nucleo_app_request_draw(); }
}
static void on_enter(void)
{
    trace("enter");
    nucleo_app_set_direct_draw(true); s_cleared = false;                       // the launcher's 32 KB canvas goes back to the heap: it is the TLS
    ensure_bench(); scan(); S->output = false; s_lg_scroll = 0;
    S->agent_view = true; S->ask = false;
    { struct stat sst; S->auto_run = stat(SELFTEST_IN, &sst) == 0; S->auto_sent = false; }
    S->have_key = nucleo_llm_config(S->model, sizeof S->model, S->effort, sizeof S->effort);
    tr_puts(S->have_key ? L("Nucleo Code. Chiedimi di scrivere, provare e correggere programmi o testi in work/main.\n",
                            "Nucleo Code. Ask me to write, run and fix programs or texts in work/main.\n",
                            "Nucleo Code. Pideme escribir, probar y corregir programas o textos.\n",
                            "Nucleo Code. Demande-moi d'ecrire, tester et corriger programmes ou textes.\n",
                            "Nucleo Code. Lass mich Programme oder Texte schreiben, testen, korrigieren.\n")
                        : L("Serve una chiave Claude: web OS > Impostazioni > IA.\n", "A Claude key is needed: web OS > Settings > AI.\n",
                            "Hace falta una clave Claude: web OS > Ajustes > IA.\n", "Une cle Claude est requise : web OS > Reglages > IA.\n",
                            "Ein Claude-Schluessel fehlt: Web OS > Einstellungen > KI.\n"));
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
