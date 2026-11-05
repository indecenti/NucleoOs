// code_rt.c: Nucleo Code's script runtime (see include/code_rt.h, docs/nucleo-code.md 4.3).
// Store-only: compiled into the Code module (NAP_MODULE) and by the host gate, never into the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "code_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "mquickjs.h"

#define FS_READ_MAX   (64 * 1024)          // a script reads at most this much of one file
#define FS_WRITE_MAX  (256 * 1024)         // and writes at most this much to one
#define PATH_MAX_NC   192

// The run's state, reachable from the C functions through the context opaque.
typedef struct {
    const code_env_t *env;
    int64_t deadline;
    bool timeout, cancelled;
} rt_t;

static rt_t *RT(JSContext *ctx) { return (rt_t *)JS_GetContextOpaque(ctx); }
static void rt_out(JSContext *ctx, const char *s, size_t n) { rt_t *r = RT(ctx); if (r && r->env->out) r->env->out(r->env->op, s, n); }

bool code_path(const char *root, const char *rel, char *out, size_t cap)
{
    if (!root || !rel || !*rel) return false;
    if (rel[0] == '/' || rel[0] == '\\' || strchr(rel, '\\') || strchr(rel, ':')) return false;
    for (const char *p = rel; *p; ) {                      // no ".." segment anywhere
        const char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return false;
        if (!e) break;
        p = e + 1;
    }
    int w = snprintf(out, cap, "%s/%s", root, rel);
    return w > 0 && (size_t)w < cap;
}

// ---- the C side of the stdlib (code_stdlib.c names these) --------------------------------------------
static void print_vals(JSContext *ctx, int argc, JSValue *argv)
{
    for (int i = 0; i < argc; i++) {
        if (i) rt_out(ctx, " ", 1);
        if (JS_IsString(ctx, argv[i])) {
            JSCStringBuf b; size_t len; const char *s = JS_ToCStringLen(ctx, &len, argv[i], &b);
            if (s) rt_out(ctx, s, len);
        } else JS_PrintValueF(ctx, argv[i], JS_DUMP_LONG);   // goes through the log func -> out
    }
    rt_out(ctx, "\n", 1);
}
JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) { (void)this_val; print_vals(ctx, argc, argv); return JS_UNDEFINED; }
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) { (void)this_val; (void)argc; (void)argv; JS_GC(ctx); return JS_UNDEFINED; }

JSValue js_date_constructor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; argc &= ~FRAME_CF_CTOR;
    double v = RT(ctx)->env->now_ms ? (double)RT(ctx)->env->now_ms() : 0;
    if (argc >= 1 && JS_IsNumber(ctx, argv[0]) && JS_ToNumber(ctx, &v, argv[0])) return JS_EXCEPTION;
    return JS_NewDate(ctx, v);
}
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{ (void)this_val; (void)argc; (void)argv; return JS_NewInt64(ctx, RT(ctx)->env->now_ms ? RT(ctx)->env->now_ms() : 0); }
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{ (void)this_val; (void)argc; (void)argv; return JS_NewInt64(ctx, RT(ctx)->env->now_ms ? RT(ctx)->env->now_ms() : 0); }

// fs: workspace-confined, synchronous (the device has no event loop to hide)
static bool arg_path(JSContext *ctx, JSValue v, char *out, size_t cap)
{
    JSCStringBuf b; const char *s = JS_IsString(ctx, v) ? JS_ToCString(ctx, v, &b) : NULL;
    return s && code_path(RT(ctx)->env->root, s, out, cap);
}
JSValue js_nc_fs_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; char p[PATH_MAX_NC];
    if (argc < 1 || !arg_path(ctx, argv[0], p, sizeof p)) return JS_ThrowTypeError(ctx, "fs.read: path outside the workspace");
    FILE *f = fopen(p, "rb");
    if (!f) return JS_NULL;
    // a block of the file's own size: the engine's arena holds most of the heap, a 64 KB block never exists on the
    // device (every fs.read was "out of memory")
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    size_t want = sz > 0 ? (size_t)sz : 0; if (want > FS_READ_MAX) want = FS_READ_MAX;
    char *buf = malloc(want ? want : 1);
    if (!buf) { fclose(f); return JS_ThrowOutOfMemory(ctx); }
    size_t n = want ? fread(buf, 1, want, f) : 0; fclose(f);
    JSValue s = JS_NewStringLen(ctx, buf, n);
    free(buf);
    return s;
}
static JSValue fs_put(JSContext *ctx, int argc, JSValue *argv, const char *mode)
{
    char p[PATH_MAX_NC];
    if (argc < 2 || !arg_path(ctx, argv[0], p, sizeof p)) return JS_ThrowTypeError(ctx, "fs: path outside the workspace");
    JSCStringBuf b; size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1], &b);
    if (!s) return JS_EXCEPTION;
    if (len > FS_WRITE_MAX) return JS_ThrowRangeError(ctx, "fs: more than %d bytes", FS_WRITE_MAX);
    FILE *f = fopen(p, mode);
    if (!f) return JS_NewBool(0);
    bool ok = fwrite(s, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return JS_NewBool(ok);
}
JSValue js_nc_fs_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)  { (void)this_val; return fs_put(ctx, argc, argv, "wb"); }
JSValue js_nc_fs_append(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) { (void)this_val; return fs_put(ctx, argc, argv, "ab"); }
JSValue js_nc_fs_exists(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; char p[PATH_MAX_NC]; struct stat st;
    if (argc < 1 || !arg_path(ctx, argv[0], p, sizeof p)) return JS_NewBool(0);
    return JS_NewBool(stat(p, &st) == 0);
}
JSValue js_nc_fs_remove(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; char p[PATH_MAX_NC];
    if (argc < 1 || !arg_path(ctx, argv[0], p, sizeof p)) return JS_ThrowTypeError(ctx, "fs.remove: path outside the workspace");
    return JS_NewBool(remove(p) == 0);
}
JSValue js_nc_fs_list(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; char p[PATH_MAX_NC];
    if (argc >= 1 && JS_IsString(ctx, argv[0])) { if (!arg_path(ctx, argv[0], p, sizeof p)) return JS_ThrowTypeError(ctx, "fs.list: path outside the workspace"); }
    else snprintf(p, sizeof p, "%s", RT(ctx)->env->root);
    DIR *d = opendir(p);
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr) || !d) { if (d) closedir(d); return arr; }
    JSGCRef ref; JSValue *pa = JS_PushGCRef(ctx, &ref); *pa = arr;   // the GC may move it while we add
    struct dirent *e; uint32_t i = 0;
    while ((e = readdir(d)) && i < 512) {
        if (e->d_name[0] == '.') continue;
        JSValue s = JS_NewString(ctx, e->d_name);
        if (JS_IsException(s)) break;
        JS_SetPropertyUint32(ctx, *pa, i++, s);
    }
    closedir(d);
    return JS_PopGCRef(ctx, &ref);
}

// sys
JSValue js_nc_sys_mem(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSGCRef ref; JSValue *po = JS_PushGCRef(ctx, &ref);
    *po = JS_NewObject(ctx);
    if (!JS_IsException(*po)) {
        JS_SetPropertyStr(ctx, *po, "used", JS_NewInt32(ctx, (int32_t)JS_NucleoUsedNow(ctx)));
        JS_SetPropertyStr(ctx, *po, "liveMax", JS_NewInt32(ctx, (int32_t)JS_NucleoLiveMax(ctx)));
    }
    return JS_PopGCRef(ctx, &ref);
}
JSValue js_nc_sys_sleep(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; int ms = 0;
    if (argc >= 1 && JS_ToInt32(ctx, &ms, argv[0])) return JS_EXCEPTION;
    if (ms < 0) ms = 0;
    if (ms > 5000) ms = 5000;
    rt_t *r = RT(ctx);
    while (ms > 0) {                                     // in slices, so Esc and the deadline still act
        int s = ms > 50 ? 50 : ms;
        if (r->env->sleep_ms) r->env->sleep_ms((uint32_t)s);
        ms -= s;
        if ((r->env->cancel && r->env->cancel(r->env->op)) || (r->env->now_ms && r->env->now_ms() > r->deadline)) break;
    }
    return JS_UNDEFINED;
}
JSValue js_nc_sys_time(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{ (void)this_val; (void)argc; (void)argv; return JS_NewInt64(ctx, RT(ctx)->env->now_ms ? RT(ctx)->env->now_ms() : 0); }


size_t code_md_plain(const char *in, char *out, size_t cap)
{
    size_t o = 0; bool fence = false, bol = true;
    if (!cap) return 0;
    for (const char *p = in; *p && o + 4 < cap; ) {
        if (bol && !strncmp(p, "```", 3)) {                     // a fence line: drop it, indent what it holds
            fence = !fence;
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        if (bol && fence) { out[o++] = ' '; out[o++] = ' '; }
        if (bol && !fence && *p == '#') { while (*p == '#') p++; while (*p == ' ') p++; bol = false; continue; }
        bol = false;
        if (!fence && ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_'))) { p += 2; continue; }
        if (!fence && *p == '`') { p++; continue; }
        if (!fence && *p == '[') {                                // [text](url) -> text
            const char *e = strchr(p, ']');
            if (e && e[1] == '(' && strchr(e, ')')) { for (const char *q = p + 1; q < e && o + 4 < cap; q++) out[o++] = *q; p = strchr(e, ')') + 1; continue; }
        }
        if (*p == '\n') bol = true;
        out[o++] = *p++;
    }
    out[o] = 0;
    return o;
}

#include "code_stdlib.h"            // the ROM tables (gen/ on the device, build/code-host on the PC)
#include "code_compat.h"            // the compatibility chunks (code_compat.js)

// Does `src` name one of `needles` (space-separated)? A false positive only costs a chunk's memory.
static bool names_any(const char *src, size_t len, const char *needles)
{
    char w[40];
    for (const char *p = needles; *p; ) {
        while (*p == ' ') p++;
        size_t n = 0; while (p[n] && p[n] != ' ' && n < sizeof w - 1) { w[n] = p[n]; n++; }
        w[n] = 0; p += n;
        if (!n) break;
        for (size_t i = 0; i + n <= len; i++) if (src[i] == w[0] && !memcmp(src + i, w, n)) return true;
    }
    return false;
}

// ---- running -------------------------------------------------------------------------------------------
static int rt_interrupt(JSContext *ctx, void *opaque)
{
    (void)opaque;
    rt_t *r = RT(ctx);
    if (r->env->cancel && r->env->cancel(r->env->op)) { r->cancelled = true; return 1; }
    if (r->env->now_ms && r->env->now_ms() > r->deadline) { r->timeout = true; return 1; }
    return 0;
}
static void rt_log(void *opaque, const void *buf, size_t n)     // the engine passes the context's opaque
{
    rt_t *r = (rt_t *)opaque;
    if (r && r->env->out) r->env->out(r->env->op, (const char *)buf, n);
}

// "at main.js:31:7" / "main.js:31" out of a stack string -> "main.js:31"
static void where_from_stack(const char *stack, const char *filename, char *out, size_t cap)
{
    out[0] = 0;
    if (!stack || !filename) return;
    const char *p = strstr(stack, filename);
    if (!p) return;
    const char *q = p + strlen(filename);
    if (*q != ':') return;
    size_t n = (size_t)(q - p) + 1;
    q++;
    while (*q >= '0' && *q <= '9') { q++; n++; }
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
}

bool code_run(void *arena, size_t arena_size, const char *src, size_t len, const char *filename,
              const code_env_t *env, code_result_t *res)
{
    memset(res, 0, sizeof *res);
    res->arena = (uint32_t)arena_size;
    rt_t r = { env, 0, false, false };
    int64_t t0 = env->now_ms ? env->now_ms() : 0;
    r.deadline = t0 + (env->timeout_ms ? env->timeout_ms : 10000);
    JSContext *ctx = JS_NewContext(arena, arena_size, &js_stdlib);
    if (!ctx) { snprintf(res->error, sizeof res->error, "the arena is too small for the engine (%u B)", (unsigned)arena_size); res->oom = true; return false; }
    JS_SetContextOpaque(ctx, &r);
    JS_SetLogFunc(ctx, rt_log);
    JS_SetInterruptHandler(ctx, rt_interrupt);
    JSValue v = JS_UNDEFINED;
    for (size_t i = 0; i < sizeof CODE_COMPAT / sizeof CODE_COMPAT[0] && !JS_IsException(v); i++)
        if (names_any(src, len, CODE_COMPAT[i].needles)) v = JS_Eval(ctx, CODE_COMPAT[i].src, strlen(CODE_COMPAT[i].src), "compat", 0);
    if (!JS_IsException(v)) v = JS_Eval(ctx, src, len, filename, 0);
    if (JS_IsException(v)) {
        JSValue ex = JS_GetException(ctx);
        JSCStringBuf b;
        const char *m = JS_ToCString(ctx, ex, &b);
        snprintf(res->error, sizeof res->error, "%s", m ? m : "exception");
        if (strstr(res->error, "out of memory")) res->oom = true;
        if (JS_IsError(ctx, ex)) {
            JSValue st = JS_GetPropertyStr(ctx, ex, "stack");
            JSCStringBuf b2; const char *s = JS_IsString(ctx, st) ? JS_ToCString(ctx, st, &b2) : NULL;
            where_from_stack(s, filename, res->where, sizeof res->where);
        }
    } else res->ok = true;
    res->timeout = r.timeout; res->cancelled = r.cancelled;
    if (r.timeout) snprintf(res->error, sizeof res->error, "stopped: more than %u ms", (unsigned)(env->timeout_ms ? env->timeout_ms : 10000));
    if (r.cancelled) snprintf(res->error, sizeof res->error, "stopped by the user");
    if (r.timeout || r.cancelled) res->ok = false;
    res->used_end = JS_NucleoUsedNow(ctx);
    JS_GC(ctx);                                          // one last compacting GC: what was really live
    res->live_max = JS_NucleoLiveMax(ctx);
    JS_FreeContext(ctx);
    res->ms = (uint32_t)((env->now_ms ? env->now_ms() : 0) - t0);
    return res->ok;
}
#endif
