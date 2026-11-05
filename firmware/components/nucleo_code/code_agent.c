// code_agent.c: Nucleo Code's agent loop (see include/code_agent.h, docs/nucleo-code.md 4.2 and 6).
// Store-only: compiled into the Code module (NAP_MODULE) and by the host gate, never into the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "code_agent.h"
#include "code_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cJSON.h"
#ifdef _WIN32                                          // the host gate on Windows (MinGW): mkdir takes no mode
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif
#ifndef TAG
#define TAG "code"
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(t, ...) ((void)0)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(t, ...) ((void)0)
#endif
typedef struct { const char *q, *a; } anima_turn_t;   // anima_claude.h's chat helpers need it; the agent does not
#include "anima_claude.h"                             // the stream parser (anth_stream_t) and effort per model

#define VIEW_MAX     12000         // characters of a file a view returns
#define RESULT_MAX   6000          // characters of any other tool result
#define FILE_MAX     (256 * 1024)  // a file the agent may write
#define SEARCH_MAX   40            // matches a search returns
#define PATHCAP      256

struct agent {
    agent_cfg_t   cfg;
    agent_io_t    io;
    agent_stats_t st;
    agent_state_t state;
    char          hist[PATHCAP], req[PATHCAP];   // the session (JSONL) and the request being sent, on the SD card
    int           calls_turn;
    // the reply being streamed
    anth_stream_t *s;
    cJSON        *content;         // the assistant message's content blocks, in order
    char         *text; size_t tlen, tcap;   // the text block being written
    char          stop[24];
    anth_usage_t  usage;
    bool          stream_error; char stream_msg[160];
    // the tool calls being answered
    cJSON        *amsg, *rmsg;     // the assistant message with the calls, the user message with the results
    int           tool_i;
    uint32_t      big_mask;        // blocks whose input did not fit the device
    char          ask[160];        // the action waiting for the user's yes
    char          todo[1024];
};

// ---- the system prompt and the tools -------------------------------------------------------------------
static const char *SYS =
    "You are Nucleo Code, a coding and writing agent running on the user's M5Stack Cardputer (ESP32-S3, 512 KB of RAM) "
    "under NucleoOS. You work inside the workspace folder named in <env>; every path you use is relative to it.\n\n"
    "How you work:\n"
    "- Understand the task, then act. For work with more than two steps, write a short plan with todo_write and keep it current.\n"
    "- Look before you change: view a file before editing it. Make focused edits with str_replace; create new files with create. "
    "Never rewrite a whole file to change a few lines.\n"
    "- Verify: after writing or changing a program, run it with run_js, read the output and any error, fix it and run it again "
    "until it works. Never say a program works without having run it.\n"
    "- Keep programs small, clear and commented where it helps; one file unless the task needs more.\n"
    "- If the task itself looks wrong (contradictory, impossible, a typo in the data), say so and propose the fix instead of forcing it.\n"
    "- When you are done, answer in 2-4 short lines: what you made, the file names, and how the user runs it (in Code: select "
    "the file and press Enter). The screen is 240x135: plain text only, no markdown (no **, no backticks, no headings, no "
    "tables), no long prose, and never name your internal tools.\n\n"
    "JavaScript on this device is MicroQuickJS, a strict ES5 subset. Follow it exactly:\n"
    "- Use var and function declarations only. NOT available: let, const, arrow functions, template literals, classes, "
    "destructuring, spread, default parameters, Symbol, Promise, async/await, generators, modules.\n"
    "- Added by the device (loaded only when your code names them): Object.assign/values/entries; Array find/findIndex/"
    "includes/fill, Array.from/of; String includes/startsWith/endsWith/padStart/padEnd; Number.isInteger/isFinite; and a "
    "small Map and Set (new Map(pairs), get/set/has/delete/forEach, size is a property, keys()/values()/entries() return "
    "arrays, no for...of over them).\n"
    "- Available: Object.keys, Array map/filter/forEach/reduce/sort/indexOf/slice/splice/concat/join, for...of over arrays, "
    "String split/replace/replaceAll/trim/repeat/charCodeAt/toUpperCase (ASCII only), RegExp, JSON, Math, typed arrays, "
    "try/catch, Date.now() (no other Date method).\n"
    "- Arrays: never write past the end (a[a.length + 1] = x throws); use push. Strict mode is always on.\n"
    "- Each catch variable needs its own name within a function, different from every var there (catch (e1), catch (e2)...): "
    "reusing one is a SyntaxError.\n"
    "- To share code between files: (0, eval)(fs.read('lib.js')) runs lib.js in the global scope.\n"
    "- Device API, synchronous: print(...) and console.log(...); fs.read(path) -> string or null; fs.write(path, s); "
    "fs.append(path, s); fs.list(dir) -> array of names; fs.exists(path); fs.remove(path); sys.sleep(ms); sys.time() -> ms; "
    "sys.mem() -> {used, liveMax} bytes.\n"
    "- Limits: a run stops after 10 s, and the engine has about 24-40 KB of memory: no huge arrays or strings, print at most a "
    "few hundred lines.\n"
    "Text files (notes, chapters, CSV) are written with the editor tool directly: no code needed.";

static const char *TOOLS_JSON =
    "[{\"type\":\"text_editor_20250728\",\"name\":\"str_replace_based_edit_tool\",\"max_characters\":12000},"
    "{\"name\":\"run_js\",\"description\":\"Run a JavaScript file of the workspace on the device (MicroQuickJS) and return its output, "
    "the error with file:line if it failed, the time and the memory it needed.\",\"input_schema\":{\"type\":\"object\",\"properties\":"
    "{\"path\":{\"type\":\"string\",\"description\":\"file to run, relative to the workspace\"},\"timeout_ms\":{\"type\":\"integer\","
    "\"description\":\"wall clock limit, at most 10000\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"search\",\"description\":\"Find a text (case-insensitive) in the workspace's files; returns file:line: line, at most 40.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"path\":{\"type\":\"string\","
    "\"description\":\"folder to search, relative to the workspace (default: all)\"}},\"required\":[\"text\"]}},"
    "{\"name\":\"todo_write\",\"description\":\"Write the plan the user sees: the whole list each time, one item in_progress at a time.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"todos\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":"
    "{\"content\":{\"type\":\"string\"},\"status\":{\"type\":\"string\",\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
    "\"required\":[\"content\",\"status\"]}}},\"required\":[\"todos\"],\"additionalProperties\":false},"
    "\"cache_control\":{\"type\":\"ephemeral\"}}]";

// ---- small helpers --------------------------------------------------------------------------------------
static int64_t now_ms(agent_t *a) { return a->io.now_ms ? a->io.now_ms() : 0; }
static const char *jstr(cJSON *o, const char *k) { cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL; return cJSON_IsString(v) ? v->valuestring : NULL; }

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    if (!b) { fclose(f); return NULL; }
    size_t n = fread(b, 1, max, f); fclose(f);
    b[n] = 0; if (len) *len = n;
    return b;
}
static bool write_file(const char *path, const char *s, size_t n)
{
    // make the folders on the way (one level of "a/b/c.js" at a time)
    char tmp[PATHCAP]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(s, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}
// tool result text, bounded
typedef struct { char *b; size_t n, cap; bool cut; } sb_t;
static void sb_add(sb_t *s, const char *t, size_t n)
{
    if (s->n + n >= s->cap) { n = s->cap > s->n + 1 ? s->cap - s->n - 1 : 0; s->cut = true; }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}
static void sb_printf(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sb_printf(sb_t *s, const char *fmt, ...)
{
    char tmp[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n > 0) sb_add(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

// ---- tools ----------------------------------------------------------------------------------------------
static bool resolve(agent_t *a, const char *rel, char *out)
{
    if (!rel) return false;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;                 // "./a.js"
    if (!strcmp(rel, ".") || !rel[0]) { snprintf(out, PATHCAP, "%s", a->cfg.root); return true; }
    return code_path(a->cfg.root, rel, out, PATHCAP);
}

static bool tool_editor(agent_t *a, cJSON *in, sb_t *r, char *summary, size_t scap)
{
    const char *cmd = jstr(in, "command"), *rel = jstr(in, "path");
    char p[PATHCAP];
    if (!cmd || !rel) { sb_printf(r, "error: command and path are required"); return false; }
    snprintf(summary, scap, "%s %s", cmd, rel);
    if (!resolve(a, rel, p)) { sb_printf(r, "error: %s is outside the workspace", rel); return false; }
    struct stat st;
    if (!strcmp(cmd, "view")) {
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *d = opendir(p); if (!d) { sb_printf(r, "error: cannot list %s", rel); return false; }
            struct dirent *e; int n = 0;
            while ((e = readdir(d)) && n < 200) {
                if (e->d_name[0] == '.') continue;
                char q[PATHCAP]; snprintf(q, sizeof q, "%s/%s", p, e->d_name);
                struct stat s2; bool dir = stat(q, &s2) == 0 && S_ISDIR(s2.st_mode);
                sb_printf(r, "%s%s  %ld\n", e->d_name, dir ? "/" : "", dir ? 0L : (long)s2.st_size); n++;
            }
            closedir(d);
            if (!n) sb_printf(r, "(empty)");
            return true;
        }
        size_t len; char *t = read_file(p, FILE_MAX, &len);
        if (!t) { sb_printf(r, "error: %s does not exist", rel); return false; }
        int from = 1, to = 1 << 30;
        cJSON *vr = cJSON_GetObjectItem(in, "view_range");
        if (cJSON_IsArray(vr) && cJSON_GetArraySize(vr) == 2) {
            from = (int)cJSON_GetArrayItem(vr, 0)->valuedouble; to = (int)cJSON_GetArrayItem(vr, 1)->valuedouble;
            if (to < 0) to = 1 << 30;
        }
        int line = 1; const char *s = t;
        size_t cap0 = r->cap; if (r->cap > VIEW_MAX) r->cap = VIEW_MAX;
        while (*s && !r->cut) {
            const char *e = strchr(s, '\n'); size_t n = e ? (size_t)(e - s) : strlen(s);
            if (line >= from && line <= to) { sb_printf(r, "%6d\t", line); sb_add(r, s, n); sb_add(r, "\n", 1); }
            line++; if (!e) break; s = e + 1;
        }
        if (r->cut) sb_printf(r, "\n[cut: view a range with view_range]");
        r->cap = cap0;
        free(t);
        return true;
    }
    if (!strcmp(cmd, "create")) {
        const char *txt = jstr(in, "file_text");
        if (!txt) { sb_printf(r, "error: file_text is required"); return false; }
        size_t n = strlen(txt);
        if (n > FILE_MAX) { sb_printf(r, "error: more than %d bytes: split the file", FILE_MAX); return false; }
        bool existed = stat(p, &st) == 0;
        if (!write_file(p, txt, n)) { sb_printf(r, "error: cannot write %s", rel); return false; }
        int lines = 1; for (const char *c = txt; *c; c++) if (*c == '\n') lines++;
        sb_printf(r, "%s %s (%d lines, %u bytes)", existed ? "Overwrote" : "Created", rel, lines, (unsigned)n);
        snprintf(summary, scap, "%s %s +%d", existed ? "rewrite" : "create", rel, lines);
        return true;
    }
    if (!strcmp(cmd, "str_replace")) {
        const char *o = jstr(in, "old_str"), *nw = jstr(in, "new_str");
        if (!o || !*o) { sb_printf(r, "error: old_str is required"); return false; }
        if (!nw) nw = "";
        size_t len; char *t = read_file(p, FILE_MAX, &len);
        if (!t) { sb_printf(r, "error: %s does not exist", rel); return false; }
        char *hit = strstr(t, o);
        if (!hit) { free(t); sb_printf(r, "error: old_str not found in %s (it must match exactly, whitespace included)", rel); return false; }
        if (strstr(hit + 1, o)) { free(t); sb_printf(r, "error: old_str matches more than once in %s: add context", rel); return false; }
        size_t ol = strlen(o), nl = strlen(nw), out_n = len - ol + nl;
        char *u = malloc(out_n + 1);
        if (!u) { free(t); sb_printf(r, "error: no memory"); return false; }
        size_t pre = (size_t)(hit - t);
        memcpy(u, t, pre); memcpy(u + pre, nw, nl); memcpy(u + pre + nl, hit + ol, len - pre - ol); u[out_n] = 0;
        int line = 1; for (char *c = t; c < hit; c++) if (*c == '\n') line++;
        bool ok = write_file(p, u, out_n);
        free(t); free(u);
        if (!ok) { sb_printf(r, "error: cannot write %s", rel); return false; }
        int ml = 0, pl = 0; for (const char *c = o; *c; c++) if (*c == '\n') ml++; for (const char *c = nw; *c; c++) if (*c == '\n') pl++;
        sb_printf(r, "Edited %s at line %d", rel, line);
        snprintf(summary, scap, "edit %s:%d +%d -%d", rel, line, pl + 1, ml + 1);
        return true;
    }
    if (!strcmp(cmd, "insert")) {
        const char *it = jstr(in, "insert_text"); cJSON *ln = cJSON_GetObjectItem(in, "insert_line");
        if (!it || !cJSON_IsNumber(ln)) { sb_printf(r, "error: insert_line and insert_text are required"); return false; }
        size_t len = 0; char *t = read_file(p, FILE_MAX, &len);
        if (!t) { t = calloc(1, 1); len = 0; if (!t) { sb_printf(r, "error: no memory"); return false; } }
        int at = (int)ln->valuedouble, line = 0; size_t pos = 0;
        while (line < at && pos < len) { char *e = memchr(t + pos, '\n', len - pos); if (!e) { pos = len; break; } pos = (size_t)(e - t) + 1; line++; }
        size_t il = strlen(it); bool nl = il && it[il - 1] != '\n';
        size_t out_n = len + il + (nl ? 1 : 0);
        char *u = malloc(out_n + 1);
        if (!u) { free(t); sb_printf(r, "error: no memory"); return false; }
        memcpy(u, t, pos); memcpy(u + pos, it, il); if (nl) u[pos + il] = '\n'; memcpy(u + pos + il + (nl ? 1 : 0), t + pos, len - pos); u[out_n] = 0;
        bool ok = write_file(p, u, out_n);
        free(t); free(u);
        if (!ok) { sb_printf(r, "error: cannot write %s", rel); return false; }
        sb_printf(r, "Inserted into %s after line %d", rel, at);
        snprintf(summary, scap, "insert %s:%d", rel, at);
        return true;
    }
    sb_printf(r, "error: unknown command %s (view, create, str_replace, insert)", cmd);
    return false;
}

static void run_out(void *op, const char *s, size_t n) { sb_add((sb_t *)op, s, n); }
static bool run_cancel(void *op) { (void)op; return false; }

static bool tool_run(agent_t *a, cJSON *in, sb_t *r, char *summary, size_t scap)
{
    const char *rel = jstr(in, "path"); char p[PATHCAP];
    if (!rel || !resolve(a, rel, p)) { sb_printf(r, "error: a path inside the workspace is required"); return false; }
    snprintf(summary, scap, "run %s", rel);
    size_t len; char *src = read_file(p, 64 * 1024, &len);
    if (!src) { sb_printf(r, "error: %s does not exist", rel); return false; }
    cJSON *t = cJSON_GetObjectItem(in, "timeout_ms");
    uint32_t tmo = cJSON_IsNumber(t) && t->valuedouble > 0 ? (uint32_t)t->valuedouble : 10000; if (tmo > 10000) tmo = 10000;
    char *outb = malloc(4096); sb_t o = { outb, 0, 4096, false }; if (outb) outb[0] = 0;
    code_env_t env = { a->cfg.root, run_out, &o, a->io.now_ms, a->io.sleep_ms, run_cancel, tmo };
    code_result_t res;
    // The arena: the configured one, or (on the device) one taken now and given back after the run, so the engine
    // and the model's TLS session never hold RAM at the same time. It shrinks until a block is there.
    void *arena = a->cfg.arena; size_t asz = a->cfg.arena_size;
    if (!arena) for (size_t want = asz ? asz : 32768; want >= 12288 && !arena; want -= 4096) { arena = malloc(want); asz = want; }
    bool ok = arena && outb && code_run(arena, asz, src, len, rel, &env, &res);
    if (arena && arena != a->cfg.arena) free(arena);
    free(src);
    if (!arena || !outb) { free(outb); sb_printf(r, "error: no memory for the engine"); return false; }
    if (o.n) { sb_printf(r, "output:\n"); sb_add(r, outb, o.n); if (o.cut) sb_printf(r, "\n[output cut at 4 KB]"); sb_printf(r, "\n"); }
    else sb_printf(r, "output: (none)\n");
    if (ok) sb_printf(r, "exit: ok in %u ms, memory %u bytes of %u", (unsigned)res.ms, (unsigned)res.live_max, (unsigned)res.arena);
    else sb_printf(r, "exit: FAILED %s%s%s (after %u ms)", res.where, res.where[0] ? ": " : "", res.error, (unsigned)res.ms);
    snprintf(summary, scap, "run %s %s %ums%s%s", rel, ok ? "ok" : "FAILED", (unsigned)res.ms, ok ? "" : " ", ok ? "" : res.where);
    free(outb);
    return ok;
}

static void search_dir(agent_t *a, const char *dir, const char *rel, const char *needle, sb_t *r, int *hits, int depth)
{
    if (depth > 4 || *hits >= SEARCH_MAX) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *hits < SEARCH_MAX) {
        if (e->d_name[0] == '.') continue;
        char q[PATHCAP], qr[PATHCAP];
        snprintf(q, sizeof q, "%s/%s", dir, e->d_name);
        snprintf(qr, sizeof qr, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st; if (stat(q, &st)) continue;
        if (S_ISDIR(st.st_mode)) { search_dir(a, q, qr, needle, r, hits, depth + 1); continue; }
        if (st.st_size > 256 * 1024) continue;
        size_t len; char *t = read_file(q, (size_t)st.st_size, &len); if (!t) continue;
        int line = 1; const char *s = t;
        while (*s && *hits < SEARCH_MAX) {
            const char *eol = strchr(s, '\n'); size_t n = eol ? (size_t)(eol - s) : strlen(s);
            // case-insensitive find within the line
            size_t nl = strlen(needle); bool f = false;
            for (size_t i = 0; i + nl <= n && !f; i++) { size_t j = 0; while (j < nl && tolower((unsigned char)s[i + j]) == tolower((unsigned char)needle[j])) j++; f = j == nl; }
            if (f) { sb_printf(r, "%s:%d: ", qr, line); sb_add(r, s, n > 160 ? 160 : n); sb_add(r, "\n", 1); (*hits)++; }
            line++; if (!eol) break; s = eol + 1;
        }
        free(t);
    }
    closedir(d);
}
static bool tool_search(agent_t *a, cJSON *in, sb_t *r, char *summary, size_t scap)
{
    const char *txt = jstr(in, "text"), *rel = jstr(in, "path"); char p[PATHCAP];
    if (!txt || !*txt) { sb_printf(r, "error: text is required"); return false; }
    if (!resolve(a, rel ? rel : ".", p)) { sb_printf(r, "error: path outside the workspace"); return false; }
    snprintf(summary, scap, "search \"%.40s\"", txt);
    int hits = 0;
    search_dir(a, p, rel && strcmp(rel, ".") ? rel : "", txt, r, &hits, 0);
    if (!hits) sb_printf(r, "no matches");
    else if (hits >= SEARCH_MAX) sb_printf(r, "[first %d matches]", SEARCH_MAX);
    return true;
}

static bool tool_todo(agent_t *a, cJSON *in, sb_t *r, char *summary, size_t scap)
{
    cJSON *t = cJSON_GetObjectItem(in, "todos");
    if (!cJSON_IsArray(t)) { sb_printf(r, "error: todos must be an array"); return false; }
    int n = cJSON_GetArraySize(t), done = 0; size_t k = 0;
    a->todo[0] = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(t, i); const char *c = jstr(it, "content"), *s = jstr(it, "status");
        if (!c) continue;
        if (s && !strcmp(s, "completed")) done++;
        int w = snprintf(a->todo + k, sizeof a->todo - k, "%s %s\n", s && !strcmp(s, "completed") ? "[x]" : s && !strcmp(s, "in_progress") ? "[>]" : "[ ]", c);
        if (w > 0 && k + (size_t)w < sizeof a->todo) k += (size_t)w;
    }
    if (a->io.on_todo) a->io.on_todo(a->io.ui_op, a->todo);
    snprintf(summary, scap, "plan %d/%d", done, n);
    sb_printf(r, "plan saved (%d/%d done)", done, n);
    return true;
}

static bool tool_mutates(const char *name, cJSON *in)
{
    if (!strcmp(name, "run_js")) return true;
    if (!strcmp(name, "str_replace_based_edit_tool")) { const char *c = jstr(in, "command"); return !c || strcmp(c, "view"); }
    return false;
}

static bool run_tool(agent_t *a, const char *name, cJSON *in, sb_t *r)
{
    char summary[160]; summary[0] = 0;
    snprintf(summary, sizeof summary, "%s", name);
    if (in && !strcmp(name, "str_replace_based_edit_tool")) { const char *c = jstr(in, "command"), *p = jstr(in, "path"); snprintf(summary, sizeof summary, "%s %s", c ? c : "?", p ? p : "?"); }
    else if (in && !strcmp(name, "run_js")) { const char *p = jstr(in, "path"); snprintf(summary, sizeof summary, "run %s", p ? p : "?"); }
    if (a->io.on_tool) a->io.on_tool(a->io.ui_op, name, summary);
    if (in && tool_mutates(name, in) && a->io.approve && !a->io.approve(a->io.ui_op, name, summary)) {
        sb_printf(r, "The user declined this action.");
        if (a->io.on_tool_done) a->io.on_tool_done(a->io.ui_op, name, false, "declined");
        return false;
    }
    bool ok;
    if (!in) { sb_printf(r, "error: the input was too large for the device; write the file in smaller parts (create a short file, then insert)"); ok = false; }
    else if (!strcmp(name, "str_replace_based_edit_tool")) ok = tool_editor(a, in, r, summary, sizeof summary);
    else if (!strcmp(name, "run_js")) ok = tool_run(a, in, r, summary, sizeof summary);
    else if (!strcmp(name, "search")) ok = tool_search(a, in, r, summary, sizeof summary);
    else if (!strcmp(name, "todo_write")) ok = tool_todo(a, in, r, summary, sizeof summary);
    else { sb_printf(r, "error: unknown tool %s", name); ok = false; }
    if (a->io.on_tool_done) a->io.on_tool_done(a->io.ui_op, name, ok, summary);
    return ok;
}

// ---- the stream -> the assistant message ----------------------------------------------------------------
static void flush_text(agent_t *a)
{
    if (!a->tlen) return;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "text");
    a->text[a->tlen] = 0;
    cJSON_AddStringToObject(b, "text", a->text);
    cJSON_AddItemToArray(a->content, b);
    a->tlen = 0;
}
static void ev_text(void *op, const char *s, size_t n)
{
    agent_t *a = op;
    if (a->tlen + n + 1 > a->tcap) {
        size_t nc = (a->tlen + n + 1) * 2; char *t = realloc(a->text, nc);
        if (!t) return;
        a->text = t; a->tcap = nc;
    }
    memcpy(a->text + a->tlen, s, n); a->tlen += n;
    if (a->io.on_text) a->io.on_text(a->io.ui_op, s, n);
}
static void ev_thinking(void *op, const char *sig)
{
    agent_t *a = op;
    flush_text(a);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "thinking");
    cJSON_AddStringToObject(b, "thinking", "");
    cJSON_AddStringToObject(b, "signature", sig);
    cJSON_AddItemToArray(a->content, b);
}
static void ev_tool(void *op, const char *id, const char *name, const char *json, size_t n)
{
    agent_t *a = op;
    flush_text(a);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "tool_use");
    cJSON_AddStringToObject(b, "id", id);
    cJSON_AddStringToObject(b, "name", name);
    cJSON *in = json ? cJSON_ParseWithLength(json, n) : NULL;
    // too large / unparsable: keep a valid empty object in history; the result explains
    cJSON_AddItemToObject(b, "input", in ? in : cJSON_CreateObject());
    if (!in) cJSON_AddBoolToObject(b, "_too_large", true);   // stripped before sending (see send_request)
    cJSON_AddItemToArray(a->content, b);
}
static void ev_done(void *op, const char *stop, const anth_usage_t *u) { agent_t *a = op; snprintf(a->stop, sizeof a->stop, "%s", stop); a->usage = *u; }
static void ev_error(void *op, const char *m) { agent_t *a = op; a->stream_error = true; snprintf(a->stream_msg, sizeof a->stream_msg, "%s", m); }

// price per million tokens: input, output (cache read 10% of input, cache write 125%)
static void price(const char *model, double *in, double *out)
{
    *in = 2.0; *out = 10.0;                                           // Sonnet 5.5
    if (strstr(model, "opus-5-5")) { *in = 4.0; *out = 20.0; }
    else if (strstr(model, "opus")) { *in = 5.0; *out = 25.0; }
    else if (strstr(model, "haiku-5")) { *in = 0.10; *out = 0.50; }
    else if (strstr(model, "haiku")) { *in = 1.0; *out = 5.0; }
    else if (strstr(model, "fable")) { *in = 10.0; *out = 50.0; }
}

// ---- the conversation lives on the SD card ---------------------------------------------------------------
// <root>/.code/session.jsonl: one Anthropic message per line, appended. The request is WRITTEN TO A FILE
// (<root>/.code/request.json) by streaming that history through, so RAM holds one message at a time however
// long the session is; the transport sends the file. The last message gets the moving cache breakpoint.
static bool hist_append(agent_t *a, cJSON *msg)
{
    char *t = cJSON_PrintUnformatted(msg);
    if (!t) return false;
    FILE *f = fopen(a->hist, "ab");
    bool ok = f && fputs(t, f) >= 0 && fputc('\n', f) != EOF;
    if (f) ok = (fclose(f) == 0) && ok;
    cJSON_free(t);
    return ok;
}
// one line of any length (realloc as it grows); NULL at the end
static char *read_line(FILE *f, char **buf, size_t *cap)
{
    size_t n = 0;
    if (!*buf) { *cap = 4096; *buf = malloc(*cap); if (!*buf) return NULL; }
    for (;;) {
        if (!fgets(*buf + n, (int)(*cap - n), f)) return n ? *buf : NULL;
        n += strlen(*buf + n);
        if (n && (*buf)[n - 1] == '\n') { (*buf)[--n] = 0; return *buf; }
        if (n + 1 >= *cap) { size_t nc = *cap * 2; char *g = realloc(*buf, nc); if (!g) return NULL; *buf = g; *cap = nc; }
    }
}

static bool build_request(agent_t *a)
{
    FILE *out = fopen(a->req, "wb");
    if (!out) { snprintf(a->st.error, sizeof a->st.error, "cannot write the request on the SD card"); return false; }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", a->cfg.model);
    cJSON_AddNumberToObject(root, "max_tokens", a->cfg.max_tokens ? a->cfg.max_tokens : 16000);
    cJSON_AddBoolToObject(root, "stream", true);
    bool th; const char *eff = anth_effort_for(a->cfg.model, a->cfg.effort, &th);
    if (eff) { cJSON *oc = cJSON_AddObjectToObject(root, "output_config"); cJSON_AddStringToObject(oc, "effort", eff); }
    char env[480];
    snprintf(env, sizeof env, "<env>\nWorkspace: %s\nModel: %s\nAnswer in: %s (code, file names and identifiers stay as they are)\n</env>",
             a->cfg.root, a->cfg.model, a->cfg.lang ? a->cfg.lang : "the language of the user's message");
    cJSON *sys = cJSON_AddArrayToObject(root, "system");
    cJSON *s1 = cJSON_CreateObject(); cJSON_AddStringToObject(s1, "type", "text"); cJSON_AddStringToObject(s1, "text", SYS);
    cJSON *cc = cJSON_AddObjectToObject(s1, "cache_control"); cJSON_AddStringToObject(cc, "type", "ephemeral");
    cJSON_AddItemToArray(sys, s1);
    cJSON *s2 = cJSON_CreateObject(); cJSON_AddStringToObject(s2, "type", "text"); cJSON_AddStringToObject(s2, "text", env);
    cJSON_AddItemToArray(sys, s2);
    cJSON_AddItemToObject(root, "tools", cJSON_Parse(TOOLS_JSON));
    char *head = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = head != NULL;
    if (ok) { size_t hl = strlen(head); ok = fwrite(head, 1, hl - 1, out) == hl - 1 && fputs(",\"messages\":[", out) >= 0; }
    cJSON_free(head);
    FILE *h = ok ? fopen(a->hist, "rb") : NULL;
    char *cur = NULL, *nxt = NULL; size_t ccap = 0, ncap = 0;
    bool first = true;
    if (h) {
        char *l = read_line(h, &cur, &ccap);
        while (ok && l) {
            char *m = read_line(h, &nxt, &ncap);                 // look ahead: is `cur` the last line?
            if (!first) ok = fputc(',', out) != EOF;
            first = false;
            if (m) ok = ok && fputs(cur, out) >= 0;
            else {                                              // the last message: the cache breakpoint on its last block
                cJSON *msg = cJSON_Parse(cur);
                cJSON *ct = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
                cJSON *lb = cJSON_IsArray(ct) ? cJSON_GetArrayItem(ct, cJSON_GetArraySize(ct) - 1) : NULL;
                if (lb) { cJSON *c2 = cJSON_AddObjectToObject(lb, "cache_control"); cJSON_AddStringToObject(c2, "type", "ephemeral"); }
                char *t = msg ? cJSON_PrintUnformatted(msg) : NULL;
                ok = ok && fputs(t ? t : cur, out) >= 0;
                cJSON_free(t); cJSON_Delete(msg);
            }
            char *sw = cur; size_t sc = ccap; cur = nxt; ccap = ncap; nxt = sw; ncap = sc;   // next becomes current
            l = m ? cur : NULL;
        }
        fclose(h);
    }
    free(cur); free(nxt);
    ok = ok && fputs("]}", out) >= 0;
    ok = (fclose(out) == 0) && ok;
    if (!ok) snprintf(a->st.error, sizeof a->st.error, "cannot write the request on the SD card");
    return ok;
}

// ---- the state machine -----------------------------------------------------------------------------------
static void set_error(agent_t *a, const char *m) { snprintf(a->st.error, sizeof a->st.error, "%s", m); a->state = AG_ERROR; }

// every tool call of the current assistant message still unanswered gets an answer, so the history stays valid
static void answer_pending(agent_t *a, const char *why)
{
    if (!a->amsg) return;
    cJSON *ac = cJSON_GetObjectItem(a->amsg, "content");
    int n = cJSON_GetArraySize(ac);
    for (int i = a->tool_i; i < n; i++) {
        cJSON *b = cJSON_GetArrayItem(ac, i);
        const char *ty = jstr(b, "type");
        if (!ty || strcmp(ty, "tool_use")) continue;
        cJSON *tr = cJSON_CreateObject();
        cJSON_AddStringToObject(tr, "type", "tool_result");
        cJSON_AddStringToObject(tr, "tool_use_id", jstr(b, "id") ? jstr(b, "id") : "");
        cJSON_AddStringToObject(tr, "content", why);
        cJSON_AddBoolToObject(tr, "is_error", true);
        cJSON_AddItemToArray(cJSON_GetObjectItem(a->rmsg, "content"), tr);
    }
    hist_append(a, a->rmsg);
    cJSON_Delete(a->amsg); a->amsg = NULL;
    cJSON_Delete(a->rmsg); a->rmsg = NULL;
}

static void next_request(agent_t *a)
{
    if (a->calls_turn >= (a->cfg.max_calls ? a->cfg.max_calls : 30)) { set_error(a, "stopped after the model-call limit of this turn"); return; }
    if (!build_request(a)) { a->state = AG_ERROR; return; }
    a->state = AG_SEND;
}

bool agent_submit(agent_t *a, const char *user_text)
{
    if (a->state == AG_SEND || a->state == AG_RECV || a->state == AG_TOOLS || a->state == AG_ASK) return false;
    a->st.error[0] = 0; a->calls_turn = 0;
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON *uc = cJSON_AddArrayToObject(u, "content");
    cJSON *ut = cJSON_CreateObject(); cJSON_AddStringToObject(ut, "type", "text"); cJSON_AddStringToObject(ut, "text", user_text);
    cJSON_AddItemToArray(uc, ut);
    bool ok = hist_append(a, u);
    cJSON_Delete(u);
    if (!ok) { set_error(a, "cannot write the session on the SD card"); return false; }
    next_request(a);
    return a->state == AG_SEND;
}

const char *agent_request_path(const agent_t *a) { return a->req; }

void agent_begin_response(agent_t *a)
{
    free(a->s);
    a->s = calloc(1, sizeof *a->s);
    if (!a->s) { set_error(a, "no memory for the stream"); return; }
    anth_cb_t cb = { ev_text, ev_tool, ev_done, ev_error, ev_thinking };
    anth_stream_init(a->s, &cb, a, a->cfg.toolbuf, a->cfg.toolcap);
    cJSON_Delete(a->content);
    a->content = cJSON_CreateArray(); a->tlen = 0; a->stop[0] = 0; a->stream_error = false; memset(&a->usage, 0, sizeof a->usage);
    a->big_mask = 0;
    a->state = AG_RECV;
}

void agent_feed(agent_t *a, const char *b, int n) { if (a->state == AG_RECV && a->s) anth_stream_feed(a->s, b, n); }

void agent_end_response(agent_t *a, int status, const char *err)
{
    if (a->state != AG_RECV) return;
    flush_text(a);
    bool ok = status == 200 && !a->stream_error && a->s && a->s->done;
    free(a->s); a->s = NULL;
    a->st.calls++; a->calls_turn++;
    a->st.in += a->usage.in; a->st.out += a->usage.out; a->st.cache_read += a->usage.cache_read; a->st.cache_write += a->usage.cache_write;
    a->st.last_context = a->usage.in + a->usage.cache_read + a->usage.cache_write;
    double pi, po; price(a->cfg.model, &pi, &po);
    a->st.usd += (a->usage.in * pi + a->usage.cache_read * pi * 0.1 + a->usage.cache_write * pi * 1.25 + a->usage.out * po) / 1e6;
    snprintf(a->st.stop, sizeof a->st.stop, "%s", a->stop);
    if (!ok) {
        char m[200];
        if (a->stream_error) snprintf(m, sizeof m, "%s", a->stream_msg);
        else if (status != 200) snprintf(m, sizeof m, "HTTP %d: %s", status, err && err[0] ? err : (status < 0 ? "no connection" : "error"));
        else snprintf(m, sizeof m, "the answer was cut off");
        cJSON_Delete(a->content); a->content = NULL;
        set_error(a, m);
        return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    if (cJSON_GetArraySize(a->content) == 0) { cJSON *t = cJSON_CreateObject(); cJSON_AddStringToObject(t, "type", "text"); cJSON_AddStringToObject(t, "text", "(no answer)"); cJSON_AddItemToArray(a->content, t); }
    // our own "_too_large" marker never reaches the history
    int n = cJSON_GetArraySize(a->content);
    for (int i = 0; i < n; i++) {
        cJSON *b = cJSON_GetArrayItem(a->content, i), *big = cJSON_GetObjectItem(b, "_too_large");
        if (big) for (int k = 0; k < cJSON_GetArraySize(b); k++) if (cJSON_GetArrayItem(b, k) == big) { cJSON_DeleteItemFromArray(b, k); a->big_mask |= 1u << (i & 31); break; }
    }
    cJSON_AddItemToObject(msg, "content", a->content); a->content = NULL;
    hist_append(a, msg);
    if (!strcmp(a->stop, "tool_use")) {
        a->amsg = msg; a->tool_i = 0;
        a->rmsg = cJSON_CreateObject(); cJSON_AddStringToObject(a->rmsg, "role", "user"); cJSON_AddArrayToObject(a->rmsg, "content");
        a->state = AG_TOOLS;
        return;
    }
    cJSON_Delete(msg);
    a->big_mask = 0;
    if (strcmp(a->stop, "end_turn") && strcmp(a->stop, "stop_sequence")) { char m[64]; snprintf(m, sizeof m, "the model stopped: %s", a->stop); set_error(a, m); return; }
    a->state = AG_DONE;
}

// the tool at a->tool_i, or -1 when none is left
static int next_tool(agent_t *a)
{
    cJSON *ac = cJSON_GetObjectItem(a->amsg, "content");
    int n = cJSON_GetArraySize(ac);
    while (a->tool_i < n) { const char *ty = jstr(cJSON_GetArrayItem(ac, a->tool_i), "type"); if (ty && !strcmp(ty, "tool_use")) return a->tool_i; a->tool_i++; }
    return -1;
}

static void do_tool(agent_t *a, int i, bool declined)
{
    cJSON *b = cJSON_GetArrayItem(cJSON_GetObjectItem(a->amsg, "content"), i);
    const char *name = jstr(b, "name") ? jstr(b, "name") : "?";
    bool too_large = (a->big_mask >> (i & 31)) & 1u;
    char *rb = malloc(VIEW_MAX + 256);
    sb_t r = { rb, 0, (strcmp(name, "str_replace_based_edit_tool") ? RESULT_MAX : VIEW_MAX) + 256, false };
    bool ok;
    if (!rb) ok = false;
    else if (declined) { rb[0] = 0; sb_printf(&r, "The user declined this action."); ok = false; if (a->io.on_tool_done) a->io.on_tool_done(a->io.ui_op, name, false, "declined"); }
    else { rb[0] = 0; ok = run_tool(a, name, too_large ? NULL : cJSON_GetObjectItem(b, "input"), &r); }
    a->st.tools++; if (!ok) a->st.tool_errors++;
    cJSON *tr = cJSON_CreateObject();
    cJSON_AddStringToObject(tr, "type", "tool_result");
    cJSON_AddStringToObject(tr, "tool_use_id", jstr(b, "id") ? jstr(b, "id") : "");
    cJSON_AddStringToObject(tr, "content", rb && rb[0] ? rb : "(no memory for the result)");
    if (!ok) cJSON_AddBoolToObject(tr, "is_error", true);
    cJSON_AddItemToArray(cJSON_GetObjectItem(a->rmsg, "content"), tr);
    free(rb);
    a->tool_i++;
}

static bool needs_approval(agent_t *a, int i)
{
    if (!a->cfg.ask_writes) return false;
    cJSON *b = cJSON_GetArrayItem(cJSON_GetObjectItem(a->amsg, "content"), i);
    const char *name = jstr(b, "name");
    cJSON *in = cJSON_GetObjectItem(b, "input");
    return name && in && tool_mutates(name, in);
}

agent_state_t agent_step(agent_t *a)
{
    if (a->state == AG_TOOLS) {
        int i = next_tool(a);
        if (i < 0) {                                          // all answered: they go in one user message
            hist_append(a, a->rmsg);
            cJSON_Delete(a->amsg); a->amsg = NULL; cJSON_Delete(a->rmsg); a->rmsg = NULL; a->big_mask = 0;
            if (a->io.cancel && a->io.cancel(a->io.ui_op)) { set_error(a, "stopped by the user"); return a->state; }
            next_request(a);
        } else if (needs_approval(a, i)) {
            cJSON *b = cJSON_GetArrayItem(cJSON_GetObjectItem(a->amsg, "content"), i);
            cJSON *in = cJSON_GetObjectItem(b, "input");
            const char *nm = jstr(b, "name"), *c = jstr(in, "command"), *p = jstr(in, "path");
            snprintf(a->ask, sizeof a->ask, "%s %s", !strcmp(nm, "run_js") ? "run" : (c ? c : nm), p ? p : "");
            a->state = AG_ASK;
        } else do_tool(a, i, false);
    }
    return a->state;
}
const char *agent_question(const agent_t *a) { return a->state == AG_ASK ? a->ask : NULL; }
void agent_answer(agent_t *a, bool yes)
{
    if (a->state != AG_ASK) return;
    a->state = AG_TOOLS;
    do_tool(a, a->tool_i, !yes);
}
void agent_cancel(agent_t *a, const char *why)
{
    if (a->state == AG_TOOLS || a->state == AG_ASK) answer_pending(a, "stopped by the user");
    if (a->state == AG_SEND || a->state == AG_RECV || a->state == AG_TOOLS || a->state == AG_ASK) set_error(a, why ? why : "stopped by the user");
    free(a->s); a->s = NULL;
}
agent_state_t agent_state(const agent_t *a) { return a->state; }
void agent_reset(agent_t *a) { remove(a->hist); a->st = (agent_stats_t){0}; a->state = AG_IDLE; a->todo[0] = 0; }

// ---- public ---------------------------------------------------------------------------------------------
agent_t *agent_new(const agent_cfg_t *cfg, const agent_io_t *io)
{
    agent_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->cfg = *cfg; a->io = *io;
    a->tcap = 1024; a->text = malloc(a->tcap);
    char dir[PATHCAP]; snprintf(dir, sizeof dir, "%s/.code", cfg->root); mkdir(dir, 0775);
    snprintf(a->hist, sizeof a->hist, "%s/.code/session.jsonl", cfg->root);
    snprintf(a->req, sizeof a->req, "%s/.code/request.json", cfg->root);
    if (!a->text) { agent_free(a); return NULL; }
    return a;
}
void agent_free(agent_t *a)
{
    if (!a) return;
    cJSON_Delete(a->content); cJSON_Delete(a->amsg); cJSON_Delete(a->rmsg); free(a->s); free(a->text); free(a);
}
const agent_stats_t *agent_stats(const agent_t *a) { return &a->st; }
const char *agent_todo(const agent_t *a) { return a->todo; }

// The whole turn, blocking: the host bench (and any caller with a blocking transport).
bool agent_turn(agent_t *a, const char *user_text)
{
    if (!agent_submit(a, user_text)) return false;
    char err[256];
    for (;;) {
        agent_state_t s = agent_state(a);
        if (s == AG_SEND) {
            agent_begin_response(a);
            if (a->state != AG_RECV) break;
            err[0] = 0;
            int status = a->io.post_file(a->io.post_op, a->req, (agent_bytes_fn)agent_feed, a, err, sizeof err);
            agent_end_response(a, status, err);
        } else if (s == AG_TOOLS) agent_step(a);
        else if (s == AG_ASK) agent_answer(a, a->io.approve ? a->io.approve(a->io.ui_op, "tool", a->ask) : true);
        else break;
    }
    return a->state == AG_DONE;
}
#endif
