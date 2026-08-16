// ANIMA host driver — runs the real firmware cascade (nucleo_anima_query) on a PC.
//   anima.exe "che ore sono"      one-shot
//   anima.exe --en "what time is it"
//   anima.exe                     interactive: one query per line, EOF to quit
//   type queries.txt | anima.exe  batch
// In-REPL commands: /en /it (switch language), /reset (forget conversation).
//
// Windows specifics, so accented Italian survives and L1 data is always found:
//   * argv is taken from the WIDE command line and converted to UTF-8 (the CRT's argv is
//     ANSI/CP1252, which would mangle "perché"); stdin is read in binary so piped UTF-8
//     passes through verbatim.
//   * we chdir to the exe's own directory, so "./sd" (L1 encoder/index, session) resolves
//     next to anima.exe no matter what working directory the caller (npm, tests) used.
// On POSIX hosts argv is already UTF-8 and stdin is already binary, so only the
// chdir-to-harness-root logic needs a platform port (via /proc/self/exe or argv[0]).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <direct.h>   // _wchdir
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#endif
#include "nucleo_anima.h"

// Diagnostics from the L1 tier (internal): the last query's top-2 cosines. Declared here (not via
// the internal header) so the host driver can print how close a miss was under ANIMA_TRACE.
void nucleo_anima_l1_last_band(float *c1, float *c2);

// The REAL device encoder (nucleo_anima_l1.c), exposed to the host so the encoder-quality gate can
// measure the shipped weights instead of a reimplementation that could silently drift from them.
int nucleo_anima_l1_dim(void);
int nucleo_anima_l1_encode(const char *text, int8_t *out, int cap);

static const char *tier_name(anima_tier_t t) {
    switch (t) {
        case ANIMA_TIER_COMMAND: return "L0/command";
        case ANIMA_TIER_FACT:    return "L1/fact";
        case ANIMA_TIER_STITCH:  return "L2/stitch";
        case ANIMA_TIER_REMOTE:  return "L3/remote";
        default:                 return "none";
    }
}
static const char *act_name(anima_action_t a) {
    switch (a) {
        case ANIMA_ACT_LAUNCH: return "launch";
        case ANIMA_ACT_SYSTEM: return "system";
        case ANIMA_ACT_ANSWER: return "answer";
        case ANIMA_ACT_TOOL:   return "tool";
        default:               return "none";
    }
}

static void print_result(const char *q, const anima_result_t *r) {
    printf("Q: %s\n", q);
    if (r->corrected[0]) printf("   (interpretato: \"%s\")\n", r->corrected);
    printf("   tier=%s  action=%s  intent=%s  conf=%d%s\n",
           tier_name(r->tier), act_name(r->action), r->intent, r->confidence,
           r->awaiting ? "  [attende follow-up]" : "");
    if (r->arg[0])   printf("   arg=%s\n", r->arg);
    if (r->state[0]) printf("   state=%s  budget=%d  from_mem=%d\n", r->state, r->budget, r->from_memory);
    if (r->trace[0]) printf("   trace: %s\n", r->trace);
    { const char *body = nucleo_anima_tool_content(); if (body && body[0]) printf("   content: %s\n", body); }
    if (getenv("ANIMA_TRACE")) {                       // how close was L1 (borderline vs true miss)?
        float c1 = -2, c2 = -2; nucleo_anima_l1_last_band(&c1, &c2);
        printf("   [trace] L1 top1=%.3f top2=%.3f\n", c1, c2);
    }
    printf("   reply: %s\n\n", r->reply[0] ? r->reply : "(vuoto)");
}

#ifdef _WIN32
// Wide string -> freshly malloc'd UTF-8.
static char *w2u8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
#endif

// Skip a leading UTF-8 BOM (PowerShell pipes sometimes prepend one).
static char *skip_bom(char *p) {
    unsigned char *u = (unsigned char *)p;
    return (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) ? p + 3 : p;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);   // piped UTF-8 passes through unchanged

    // Rebuild argv as UTF-8 from the WIDE command line (the CRT's argv is ANSI/CP1252,
    // which would mangle "perché"). On POSIX argv is already UTF-8 — used as-is.
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char **u8argv = (char **)calloc((size_t)wargc, sizeof(char *));
        if (u8argv) {
            for (int i = 0; i < wargc; i++) u8argv[i] = w2u8(wargv[i]);
            argv = u8argv;
            argc = wargc;
        }
        LocalFree(wargv);
    }
#endif

    // REC8 device-dim gate: ANIMA_SD_ROOT overrides the SD root so the SAME binary (D is read from the
    // encoder header at runtime) can run against the D=192 DEVICE tree — point it at a dir whose ./sd is a
    // junction to deploy/sd-safe — instead of the default D=256 host fixture. Lets us MEASURE real device
    // recall (e.g. does a fix recover nixon at D=192) on host, without a hardware round-trip.
    const char *sd_override = getenv("ANIMA_SD_ROOT");
    if (sd_override && sd_override[0]) {
#ifdef _WIN32
        _chdir(sd_override);
#else
        if (chdir(sd_override) != 0) { /* fall through to whatever cwd we have */ }
#endif
    } else {
    // chdir to the harness root (the nearest ancestor of the exe that contains an "sd" dir),
    // so ./sd (L1 encoder/index, session) is found regardless of caller cwd or exe nesting
    // (the exe lives in build/, the data in the harness root one level up).
#ifdef _WIN32
    wchar_t exedir[MAX_PATH];
    DWORD elen = GetModuleFileNameW(NULL, exedir, MAX_PATH);
    if (elen > 0 && elen < MAX_PATH) {
        for (DWORD i = elen; i > 0; --i)
            if (exedir[i-1] == L'\\' || exedir[i-1] == L'/') { exedir[i-1] = 0; break; }
        _wchdir(exedir);
        for (int up = 0; up < 4; up++) {
            DWORD attr = GetFileAttributesW(L"sd");
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) break;
            if (_wchdir(L"..") != 0) break;
        }
    }
#else
    char exepath[PATH_MAX]; exepath[0] = 0;
    ssize_t elen = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
    if (elen > 0) exepath[elen] = 0;                    // Linux
    else if (argv[0] && strchr(argv[0], '/'))           // fallback: invocation path
        snprintf(exepath, sizeof exepath, "%s", argv[0]);
    char *slash = exepath[0] ? strrchr(exepath, '/') : NULL;
    if (slash) {
        *slash = 0;
        if (chdir(exepath) == 0) {
            for (int up = 0; up < 4; up++) {
                struct stat st;
                if (stat("sd", &st) == 0 && S_ISDIR(st.st_mode)) break;
                if (chdir("..") != 0) break;
            }
        }
    }
#endif
    }

    const char *lang = "it";
    char query[1024]; query[0] = 0;
    int do_verify = 0;            // --verify "kind|key|asserted" -> grounded verdict (ANIMA Forge)
    int do_cos = 0;               // --cos: stdin "a<TAB>b" per line -> cosine under the device encoder
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if      (!strcmp(a, "--en")) lang = "en";
        else if (!strcmp(a, "--it")) lang = "it";
        else if (!strcmp(a, "--verify")) do_verify = 1;
        else if (!strcmp(a, "--cos")) do_cos = 1;
        else {
            if (query[0]) strncat(query, " ", sizeof(query) - strlen(query) - 1);
            strncat(query, a, sizeof(query) - strlen(query) - 1);
        }
    }

    nucleo_anima_init(lang);

    // --verify "kind|key|asserted": drive the grounded cross-substrate verifier and print the verdict.
    if (do_verify) {
        char *k1 = query, *k2 = strchr(query, '|'), *k3 = NULL;
        if (k2) { *k2++ = 0; k3 = strchr(k2, '|'); if (k3) *k3++ = 0; }
        char ev[256];
        anima_verify_t v = nucleo_anima_verify_claim(k1, k2 ? k2 : "", k3 ? k3 : "", lang, ev, sizeof ev);
        const char *vs = v == ANIMA_VERIFY_CONFIRMED ? "confirmed" : v == ANIMA_VERIFY_CONTRADICTED ? "contradicted" : "unknown";
        printf("VERDICT=%s\tEVIDENCE=%s\n", vs, ev);
        return 0;
    }

    // --cos: pure ENCODER mode. Each stdin line is "textA<TAB>textB"; we print their cosine under the
    // shipped device encoder, one number per line ("nan" when either side can't be embedded). No cascade,
    // no index — this measures the embedding space itself, which is what the encoder-quality gate scores.
    if (do_cos) {
        int D = nucleo_anima_l1_dim();
        if (D <= 0) { fprintf(stderr, "no encoder loaded (need ./sd/data/anima/anima-it-encoder.bin)\n"); return 2; }
        int8_t *va = (int8_t *)malloc((size_t)D), *vb = (int8_t *)malloc((size_t)D);
        if (!va || !vb) { fprintf(stderr, "oom\n"); return 2; }
        char line[2048];
        while (fgets(line, sizeof(line), stdin)) {
            char *s = skip_bom(line);
            size_t n = strlen(s);
            while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0;
            char *tab = strchr(s, '\t');
            if (!tab) { printf("nan\n"); fflush(stdout); continue; }
            *tab = 0;
            if (!nucleo_anima_l1_encode(s, va, D) || !nucleo_anima_l1_encode(tab + 1, vb, D)) {
                printf("nan\n"); fflush(stdout); continue;
            }
            double dot = 0, na = 0, nb = 0;
            for (int i = 0; i < D; i++) {
                dot += (double)va[i] * (double)vb[i];
                na  += (double)va[i] * (double)va[i];
                nb  += (double)vb[i] * (double)vb[i];
            }
            printf("%.6f\n", (na > 0 && nb > 0) ? dot / (sqrt(na) * sqrt(nb)) : 0.0);
            fflush(stdout);
        }
        free(va); free(vb);
        return 0;
    }

    if (query[0]) {
        anima_result_t r = nucleo_anima_query(query, lang);
        print_result(query, &r);
        return 0;
    }

    fprintf(stderr,
#ifdef _WIN32
        "ANIMA host (lang=%s). Una query per riga, EOF (Ctrl+Z Invio) per uscire.\n"
#else
        "ANIMA host (lang=%s). Una query per riga, EOF (Ctrl+D) per uscire.\n"
#endif
        "Comandi: /en /it /reset\n\n", lang);

    char buf[1024];
    while (fgets(buf, sizeof(buf), stdin)) {
        char *line = skip_bom(buf);
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (!strcmp(line, "/en"))    { lang = "en"; nucleo_anima_init(lang); continue; }
        if (!strcmp(line, "/it"))    { lang = "it"; nucleo_anima_init(lang); continue; }
        if (!strcmp(line, "/reset")) { nucleo_anima_reset_session(); fprintf(stderr, "(sessione azzerata)\n"); continue; }
        anima_result_t r = nucleo_anima_query(line, lang);
        print_result(line, &r);
    }
    return 0;
}
