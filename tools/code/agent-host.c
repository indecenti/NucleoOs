// agent-host.c: Nucleo Code's agent (firmware/components/nucleo_code/code_agent.c, the code the module runs)
// driven against the real Anthropic API from the PC, for the task bench (tools/code/agent-bench.mjs).
// Transport: curl (the device uses the firmware's LLM service instead). The key is read from
// %USERPROFILE%\.nucleo\anthropic-key.txt into a header file that is deleted on exit: never printed, never in
// a command line.
//   agent-host <workspace> <model> <effort> <prompt-file> [arena-kb] [--verify file.js --expect text]...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>
#include "code_agent.h"
#include "code_rt.h"

static char g_hdr[MAX_PATH], g_body[MAX_PATH];
static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
static void sleep_ms(uint32_t ms) { Sleep(ms); }

static int post(void *op, const char *path, agent_bytes_fn bytes, void *bop, char *err, size_t errcap)
{
    (void)op;
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "curl.exe -sS -N --max-time 600 -H @\"%s\" --data-binary @\"%s\" -w \"\\n@@HTTP %%{http_code}\\n\" https://api.anthropic.com/v1/messages",
             g_hdr, path);
    FILE *p = _popen(cmd, "rb");
    if (!p) return -1;
    char buf[2048]; size_t n; int status = 0;
    static char tail[4096]; size_t tl = 0;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        bytes(bop, buf, (int)n);
        for (size_t i = 0; i < n; i++) { if (tl < sizeof tail - 1) tail[tl++] = buf[i]; else { memmove(tail, tail + 1, tl - 1); tail[tl - 1] = buf[i]; } }
    }
    _pclose(p);
    tail[tl] = 0;
    char *h = strstr(tail, "@@HTTP ");
    if (h) status = atoi(h + 7);
    if (status != 200 && err) { if (h) *h = 0; snprintf(err, errcap, "%.200s", tail); }
    return status;
}

static void on_text(void *op, const char *s, size_t n) { (void)op; fwrite(s, 1, n, stdout); fflush(stdout); }
static void on_tool(void *op, const char *name, const char *summary) { (void)op; (void)name; printf("\n  > %s\n", summary); fflush(stdout); }
static void on_tool_done(void *op, const char *name, bool ok, const char *summary) { (void)op; (void)name; printf("  %s %s\n", ok ? "ok" : "ERR", summary); fflush(stdout); }
static void on_todo(void *op, const char *t) { (void)op; printf("  plan:\n%s", t); }

static char g_vout[4096]; static size_t g_vlen;
static void vout(void *op, const char *s, size_t n) { (void)op; if (g_vlen + n < sizeof g_vout) { memcpy(g_vout + g_vlen, s, n); g_vlen += n; g_vout[g_vlen] = 0; } }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: agent-host <workspace> <model> <effort> <prompt-file> [arena-kb] [--verify f --expect t]...\n"); return 2; }
    const char *ws = argv[1], *model = argv[2], *effort = argv[3];
    size_t arena_kb = argc > 5 && argv[5][0] != '-' ? (size_t)atoi(argv[5]) : 64;
    // the key -> a header file only this process uses
    char kp[MAX_PATH]; snprintf(kp, sizeof kp, "%s\\.nucleo\\anthropic-key.txt", getenv("USERPROFILE"));
    FILE *kf = fopen(kp, "rb"); if (!kf) { fprintf(stderr, "no key file\n"); return 2; }
    char key[256] = {0}; size_t kn = fread(key, 1, sizeof key - 1, kf); fclose(kf);
    while (kn && (key[kn - 1] == '\n' || key[kn - 1] == '\r' || key[kn - 1] == ' ')) key[--kn] = 0;
    char *k0 = key; if ((unsigned char)k0[0] == 0xEF) k0 += 3;           // UTF-8 BOM from Notepad
    char tmpd[MAX_PATH]; GetTempPathA(sizeof tmpd, tmpd);
    snprintf(g_hdr, sizeof g_hdr, "%snc-agent-%lu.hdr", tmpd, GetCurrentProcessId());
    snprintf(g_body, sizeof g_body, "%snc-agent-%lu.json", tmpd, GetCurrentProcessId());
    FILE *hf = fopen(g_hdr, "wb"); if (!hf) return 2;
    fprintf(hf, "x-api-key: %s\nanthropic-version: 2023-06-01\ncontent-type: application/json\naccept: text/event-stream\n", k0);
    fclose(hf);
    memset(key, 0, sizeof key);

    FILE *pf = fopen(argv[4], "rb"); if (!pf) { remove(g_hdr); return 2; }
    static char prompt[16384]; size_t pn = fread(prompt, 1, sizeof prompt - 1, pf); fclose(pf); prompt[pn] = 0;

    _mkdir(ws);
    void *arena = malloc(arena_kb * 1024);
    static char toolbuf[64 * 1024];
    const char *lang = getenv("NC_LANG");                // the bench answers in the prompt's language unless set
    agent_cfg_t cfg = { ws, model, effort, lang, 30, 16000, arena, arena_kb * 1024, toolbuf, sizeof toolbuf, false };
    agent_io_t io = { post, NULL, on_text, on_tool, on_tool_done, NULL, on_todo, NULL, now_ms, sleep_ms, NULL };
    agent_t *a = agent_new(&cfg, &io);
    int64_t t0 = now_ms();
    bool ok = agent_turn(a, prompt);
    const agent_stats_t *st = agent_stats(a);
    int64_t ms = now_ms() - t0;

    // independent verification: run the files the task names, look for the expected text
    int vfail = 0, vtotal = 0;
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--verify") || i + 3 >= argc || strcmp(argv[i + 2], "--expect")) continue;
        const char *file = argv[i + 1], *expect = argv[i + 3]; i += 3; vtotal++;
        char p[512]; snprintf(p, sizeof p, "%s/%s", ws, file);
        FILE *f = fopen(p, "rb"); char *src = NULL; size_t n = 0;
        if (f) { src = malloc(65537); n = fread(src, 1, 65536, f); fclose(f); src[n] = 0; }
        g_vlen = 0; g_vout[0] = 0;
        code_result_t r = {0}; bool rok = false;
        if (src) { code_env_t env = { ws, vout, NULL, now_ms, sleep_ms, NULL, 10000 }; rok = code_run(arena, arena_kb * 1024, src, n, file, &env, &r); }
        bool pass = src && rok && strstr(g_vout, expect);
        if (!pass) vfail++;
        printf("\nVERIFY %s %s expect \"%s\"%s%s\n", pass ? "PASS" : "FAIL", file, expect, src ? "" : " (missing)", rok ? "" : r.error);
        free(src);
    }
    printf("\nSTATS {\"ended\":%s,\"calls\":%u,\"tools\":%u,\"tool_errors\":%u,\"in\":%llu,\"out\":%llu,\"cache_read\":%llu,\"cache_write\":%llu,"
           "\"usd\":%.4f,\"ms\":%lld,\"stop\":\"%s\",\"verify_total\":%d,\"verify_fail\":%d,\"error\":\"%s\"}\n",
           ok ? "true" : "false", st->calls, st->tools, st->tool_errors, (unsigned long long)st->in, (unsigned long long)st->out,
           (unsigned long long)st->cache_read, (unsigned long long)st->cache_write, st->usd, (long long)ms, st->stop, vtotal, vfail, st->error);
    agent_free(a); free(arena);
    remove(g_hdr);
    return ok && !vfail ? 0 : 1;
}
