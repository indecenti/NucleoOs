// Streaming GET /api/fs/list body — see fslist.h for the why and the output shape.
#include "fslist.h"
#include "nucleo_fsprotect.h"   // nucleo_fs_is_protected()
#include "nucleo_fsfactory.h"   // factory scope + the per-entry fallback scan
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>

// Factory-name hash set cap: 4 B per ".factory" line (2048 lines = 8 KB). A bigger manifest, or
// no heap for the set, uses the zero-heap per-entry scan instead (slower, same answer).
#ifndef FSLIST_FACTORY_MAX
#define FSLIST_FACTORY_MAX 2048
#endif

typedef struct {
    char *buf; size_t cap, n;
    fslist_sink_t sink; void *ctx;
    bool ok;                                   // false once the sink failed: everything else no-ops
} fsl_out_t;

static void out_flush(fsl_out_t *o)
{
    if (o->ok && o->n) o->ok = o->sink(o->ctx, o->buf, o->n);
    o->n = 0;
}

static void out_mem(fsl_out_t *o, const char *s, size_t len)
{
    while (len && o->ok) {
        size_t k = o->cap - o->n;
        if (k > len) k = len;
        memcpy(o->buf + o->n, s, k);
        o->n += k; s += k; len -= k;
        if (o->n == o->cap) out_flush(o);
    }
}

static void out_str(fsl_out_t *o, const char *s) { out_mem(o, s, strlen(s)); }

// JSON string body (no quotes), escaped the way cJSON does: \" \\ \b \f \n \r \t, any other
// control byte as \u00XX, UTF-8 passed through raw.
static void out_json(fsl_out_t *o, const char *s)
{
    const char *run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out_mem(o, run, (size_t)(s - run));
        char e[8];
        switch (c) {
        case '"':  strcpy(e, "\\\""); break;
        case '\\': strcpy(e, "\\\\"); break;
        case '\b': strcpy(e, "\\b");  break;
        case '\f': strcpy(e, "\\f");  break;
        case '\n': strcpy(e, "\\n");  break;
        case '\r': strcpy(e, "\\r");  break;
        case '\t': strcpy(e, "\\t");  break;
        default:   snprintf(e, sizeof e, "\\u%04x", c); break;
        }
        out_str(o, e);
        run = s + 1;
    }
    out_mem(o, run, (size_t)(s - run));
}

// ---- factory lock flag (UX only; delete/move enforce it exactly via nucleo_fs_is_factory) ----

// Case-insensitive FNV-1a: FATFS names and nucleo_fs_factory_line_eq's strncasecmp both fold case.
static uint32_t fac_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)tolower((unsigned char)s[i]); h *= 16777619u; }
    return h;
}

// The name one ".factory" line carries, trimmed by nucleo_fs_factory_line_eq's exact rules.
// false = blank or '#' comment line.
static bool fac_span(const char *line, const char **b, size_t *n)
{
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == '\0' || *s == '\n' || *s == '\r') return false;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
    *b = s; *n = (size_t)(e - s);
    return true;
}

typedef struct {
    bool manifest;                             // this folder is in scope and has a ".factory"
    bool scan;                                 // no hash set: ask nucleo_fs_factory_file_has per entry
    uint32_t *h; int n;
} fsl_fac_t;

// Read the folder's ".factory" once into a small hash set (two streaming passes: count, fill).
static void fac_load(fsl_fac_t *f, const char *abs)
{
    memset(f, 0, sizeof *f);
    if (!nucleo_fs_factory_scope(abs)) return;
    char path[300];
    snprintf(path, sizeof path, "%s/.factory", abs);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    f->manifest = true;
    char line[160];
    const char *b; size_t n;
    int cnt = 0;
    while (fgets(line, sizeof line, fp)) if (fac_span(line, &b, &n)) cnt++;
    if (cnt > 0 && cnt <= FSLIST_FACTORY_MAX) f->h = malloc((size_t)cnt * sizeof *f->h);
    if (cnt > 0 && !f->h) { f->scan = true; fclose(fp); return; }
    rewind(fp);
    while (f->n < cnt && fgets(line, sizeof line, fp)) if (fac_span(line, &b, &n)) f->h[f->n++] = fac_hash(b, n);
    fclose(fp);
}

static bool fac_has(const fsl_fac_t *f, const char *abs, const char *name)
{
    if (!f->manifest) return false;
    if (!strcasecmp(name, ".factory")) return true;           // the manifest pins itself
    if (f->scan) return nucleo_fs_factory_file_has(abs, name);
    uint32_t h = fac_hash(name, strlen(name));
    for (int i = 0; i < f->n; i++) if (f->h[i] == h) return true;
    return false;
}

// Does the folder hold at least one sub-folder? (Lets the tree view hide a dead expander.)
static bool has_subdirs(const char *full)
{
    DIR *d = opendir(full);
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[360];
        snprintf(sub, sizeof sub, "%s/%s", full, de->d_name);
        struct stat st = {0};
        found = stat(sub, &st) == 0 && S_ISDIR(st.st_mode);
    }
    closedir(d);
    return found;
}

bool fslist_stream(DIR *dir, const char *abs, char *buf, size_t cap, fslist_sink_t sink, void *ctx)
{
    fsl_out_t o = { buf, cap, 0, sink, ctx, cap >= 16 };
    fsl_fac_t fac;
    fac_load(&fac, abs);

    out_str(&o, "{\"entries\":[");
    bool first = true;
    struct dirent *de;
    while (o.ok && (de = readdir(dir)) != NULL) {
        char full[300];
        snprintf(full, sizeof full, "%s/%s", abs, de->d_name);
        struct stat st = {0};
        stat(full, &st);
        bool is_dir = S_ISDIR(st.st_mode);

        out_str(&o, first ? "{\"name\":\"" : ",{\"name\":\"");
        first = false;
        out_json(&o, de->d_name);
        out_str(&o, is_dir ? "\",\"type\":\"dir\",\"size\":" : "\",\"type\":\"file\",\"size\":");
        char num[16];
        snprintf(num, sizeof num, "%lu", (unsigned long)st.st_size);   // FAT32 < 4 GB: exact in 32 bits
        out_str(&o, num);
        // Lock flag for protected system files and bundled factory games: UX only (the client
        // greys out delete/rename/cut); enforcement is server-side in delete/move.
        if (nucleo_fs_is_protected(full) || fac_has(&fac, abs, de->d_name)) out_str(&o, ",\"protected\":true");
        if (is_dir) out_str(&o, has_subdirs(full) ? ",\"has_subdirs\":true}" : ",\"has_subdirs\":false}");
        else out_str(&o, "}");
    }
    free(fac.h);
    out_str(&o, "]}");
    out_flush(&o);
    return o.ok;
}
