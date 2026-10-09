// SD content self-install — pure decision core. See content_policy.h. No ESP-IDF, no I/O: compiled
// unchanged into the firmware AND by the host gate (tools/anima-host/sdcontent-check.mjs).
#include "content_policy.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static bool is_hex64(const char *s)
{
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[64] == 0 || s[64] == ' ';
}

bool sdc_path_hygienic(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 200) return false;
    if (path[0] == '/') return false;                 // must be relative
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c >= 0x7f) return false;       // printable ASCII only (FAT names on a shared card)
        if (c == '\\') return false;                   // no backslash
    }
    if (strstr(path, "..")) return false;              // no parent traversal (covers "..", "a/../b")
    if (strstr(path, "//")) return false;              // no empty segment
    return true;
}

// A path is under `root` (exact prefix ending at a '/').
static bool under(const char *path, const char *root)
{
    size_t rl = strlen(root);
    return strncmp(path, root, rl) == 0;
}

bool sdc_path_writable(const char *path)
{
    if (!sdc_path_hygienic(path)) return false;

    // www/shell/ — the whole web desktop.
    if (under(path, "www/shell/")) return true;

    // apps/<id>/... — a bundled web app. Never the shared theme pointer, never an app's user data dir.
    if (under(path, "apps/")) {
        if (strcmp(path, "apps/theme.cfg") == 0) return false;
        // refuse apps/<id>/data/...
        const char *rest = path + 5;                   // after "apps/"
        const char *slash = strchr(rest, '/');
        if (!slash) return false;                      // "apps/<id>" with no file
        if (strncmp(slash, "/data/", 6) == 0) return false;
        return true;
    }

    // system/registry/, system/ir/ and system/i18n/ — the app table, the IR preset pack and the native
    // games' es/fr/de language packs (game_text.cpp).
    if (under(path, "system/registry/")) return true;
    if (under(path, "system/ir/")) return true;
    if (under(path, "system/i18n/")) return true;

    // data/anima/ — brain files ONLY; the API-key vault, learned caches, sessions and profile are the
    // user's and must never be shipped over.
    if (under(path, "data/anima/")) {
        const char *rest = path + strlen("data/anima/");
        if (under(path, "data/anima/akb5/")) return true;
        if (strncmp(rest, "anima-", 6) == 0) return true;
        if (strncmp(rest, "dict-", 5) == 0) return true;
        if (strncmp(rest, "commands", 8) == 0) return true;
        // learned/: only the firmware-pinned facet seeds, never the runtime caches next to them.
        if (strcmp(rest, "learned/facets.it.jsonl") == 0) return true;
        if (strcmp(rest, "learned/facets.en.jsonl") == 0) return true;
        return false;
    }

    // TTS voice banks under data/tts/<lang>/ (it, en) — kept in lockstep with the release pipeline's
    // allow-list (sd_deploy.py release_path_allowed). The banks are out of scope for `core` today, but a
    // release that ships them must land where the user's own data/tts/ runtime cache (.cfg/.wav) does not.
    if (under(path, "data/tts/it/")) return true;
    if (under(path, "data/tts/en/")) return true;

    // Wallpapers and the evil-portal page templates.
    if (under(path, "wallpapers/")) return true;
    if (under(path, "evilportal/")) return true;

    return false;
}

bool sdc_parse_header(const char *line, sdc_header_t *out)
{
    if (!line || !out) return false;
    sdc_header_t h; memset(&h, 0, sizeof h);
    // "#nucleoos-sd <ver> <tag> <files> <bytes>"
    const char *p = line;
    while (*p == ' ') p++;
    if (strncmp(p, "#nucleoos-sd", 12) != 0) return false;
    p += 12;
    char tag[24];
    unsigned long long bytes = 0; unsigned long files = 0; int ver = 0;
    int consumed = 0;
    if (sscanf(p, " %d %23s %lu %llu%n", &ver, tag, &files, &bytes, &consumed) < 4) return false;
    if (ver != 1) return false;
    h.version = ver; h.files = (uint32_t)files; h.bytes = (uint64_t)bytes;
    snprintf(h.tag, sizeof h.tag, "%s", tag);
    *out = h;
    return true;
}

static bool parse_mode(const char *s, sdc_mode_t *m)
{
    if (s[0] == 0 || (s[1] != 0 && s[1] != ' ')) return false;   // exactly one letter
    switch (s[0]) {
        case 'w': *m = SDC_MODE_WRITE;  return true;
        case 'c': *m = SDC_MODE_CREATE; return true;
        case 'm': *m = SDC_MODE_MERGE;  return true;
        default:  return false;
    }
}

bool sdc_parse_file(const char *line, sdc_file_t *out, bool *is_comment)
{
    if (is_comment) *is_comment = false;
    if (!line || !out) return false;
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == 0 || *p == '\r' || *p == '\n') { if (is_comment) *is_comment = true; return false; }
    if (*p == '#') { if (is_comment) *is_comment = true; return false; }   // header / #pack / comment

    sdc_file_t f; memset(&f, 0, sizeof f);
    // "<sha64> <size> <pack> <mode> <path...>"
    if (!is_hex64(p)) return false;
    memcpy(f.sha, p, 64); f.sha[64] = 0;
    p += 64;
    unsigned long size = 0; char pack[16]; char mode[4];
    int n1 = 0;
    if (sscanf(p, " %lu %15s %3s %n", &size, pack, mode, &n1) < 3) return false;
    if (!parse_mode(mode, &f.mode)) return false;
    f.size = (uint32_t)size;
    snprintf(f.pack, sizeof f.pack, "%s", pack);
    // The rest of the line is the path (it may legitimately contain spaces? no — SD names here don't;
    // take up to EOL/CR, trimmed).
    const char *path = p + n1;
    char buf[208]; size_t k = 0;   // sdc_path_writable caps the real length at 200
    while (path[k] && path[k] != '\r' && path[k] != '\n' && k < sizeof buf - 1) { buf[k] = path[k]; k++; }
    buf[k] = 0;
    while (k > 0 && buf[k - 1] == ' ') buf[--k] = 0;   // trailing spaces
    if (!sdc_path_writable(buf)) return false;          // hygiene + allow-list; a refused line fails the run
    snprintf(f.path, sizeof f.path, "%s", buf);
    *out = f;
    return true;
}

bool sdc_tag_matches(const char *firmware_tag, const char *manifest_tag)
{
    if (!firmware_tag || !manifest_tag) return false;
    int a0, a1, a2, b0, b1, b2;
    if (sscanf(firmware_tag, "%d.%d.%d", &a0, &a1, &a2) < 3) return false;
    // the manifest tag may be "v0.5.0" or "0.5.0"
    const char *m = manifest_tag; if (*m == 'v' || *m == 'V') m++;
    if (sscanf(m, "%d.%d.%d", &b0, &b1, &b2) < 3) return false;
    return a0 == b0 && a1 == b1 && a2 == b2;
}

sdc_plan_t sdc_plan(const sdc_file_t *f, bool on_disk, uint32_t disk_size, const char *disk_sha)
{
    if (f->mode == SDC_MODE_MERGE) return SDC_PLAN_MERGE;
    if (f->mode == SDC_MODE_CREATE && on_disk) return SDC_PLAN_CREATE_SKIP;   // seed: never clobber
    if (on_disk && disk_size == f->size && disk_sha && strncmp(disk_sha, f->sha, 64) == 0)
        return SDC_PLAN_SKIP;                                                  // already identical
    return SDC_PLAN_WRITE;
}
