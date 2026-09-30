// Host gate for firmware/components/nucleo_fsapi/fstwin.c (stale .gz twin on write), with REAL files in a
// temp dir. NUCLEO_SD_MOUNT = "sd" (tools/ui-host/shim/nucleo_board.h), so paths are "sd/…" relative to the
// fixture dir given as argv[1]. Driven by tools/anima-host/fstwin-check.mjs.
#include "fstwin.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define mkdir_p(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0775)
#endif

static int g_fail = 0, g_total = 0;
#define CHECK(c, name) do { g_total++; if (!(c)) { g_fail++; printf("FAIL %s\n", name); } } while (0)

static void mkdirs(const char *path)
{
    char b[300]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir_p(b); *p = '/'; }
}
static void put(const char *path, const char *body)
{
    mkdirs(path);
    FILE *f = fopen(path, "wb"); if (f) { fputs(body, f); fclose(f); }
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(int argc, char **argv)
{
    // "scope <sd-relative path>...": one 0/1 line per path — the shared vectors (tools/lib/twin-scope-vectors.json)
    if (argc >= 2 && !strcmp(argv[1], "scope")) {
        for (int i = 2; i < argc; i++) {
            char abs[320];
            snprintf(abs, sizeof abs, "sd/%s", argv[i]);
            printf("%d\n", nucleo_fs_twin_scope(abs) ? 1 : 0);
        }
        return 0;
    }
    if (argc < 2 || chdir(argv[1]) != 0) { printf("usage: fstwin-ctest <dir> | scope <path>...\n"); return 2; }

    // scope: exactly the trees webfs serves gz-first
    CHECK(nucleo_fs_twin_scope("sd/www/shell/index.html"), "scope www/shell file");
    CHECK(nucleo_fs_twin_scope("sd/www/shell/i18n/shell.it.json"), "scope www/shell nested");
    CHECK(nucleo_fs_twin_scope("sd/apps/notepad/www/app.js"), "scope apps/<id>/www");
    CHECK(nucleo_fs_twin_scope("sd/APPS/Notepad/WWW/app.js"), "scope is case-insensitive (FAT)");
    CHECK(!nucleo_fs_twin_scope("sd/www/shell/"), "not the www/shell dir itself");
    CHECK(!nucleo_fs_twin_scope("sd/www/other/x.js"), "not www outside shell");
    CHECK(!nucleo_fs_twin_scope("sd/apps/notepad/manifest.json"), "not an app's manifest");
    CHECK(!nucleo_fs_twin_scope("sd/apps/terminal/data/history.js"), "not apps/<id>/data");
    CHECK(!nucleo_fs_twin_scope("sd/apps/theme.cfg"), "not apps/theme.cfg");
    CHECK(!nucleo_fs_twin_scope("sd/apps//www/x.js"), "not an empty app id");
    CHECK(!nucleo_fs_twin_scope("sd/data/backup.tar"), "not user data");
    CHECK(!nucleo_fs_twin_scope("sd/system/registry/apps.json"), "not the registry");
    CHECK(!nucleo_fs_twin_scope("other/www/shell/x.js"), "not outside the SD mount");
    CHECK(!nucleo_fs_twin_scope(NULL), "NULL");

    // drop: in scope, raw file written, stale twin present -> removed
    put("sd/www/shell/a.js", "new"); put("sd/www/shell/a.js.gz", "OLD-TWIN");
    CHECK(nucleo_fs_drop_stale_twin("sd/www/shell/a.js") && !exists("sd/www/shell/a.js.gz"), "shell twin dropped");
    CHECK(exists("sd/www/shell/a.js"), "raw file kept");
    put("sd/apps/x/www/b.css", "new"); put("sd/apps/x/www/b.css.gz", "OLD");
    CHECK(nucleo_fs_drop_stale_twin("sd/apps/x/www/b.css") && !exists("sd/apps/x/www/b.css.gz"), "app twin dropped");
    // writing the twin itself never removes anything
    put("sd/www/shell/c.js", "raw"); put("sd/www/shell/c.js.gz", "fresh");
    CHECK(!nucleo_fs_drop_stale_twin("sd/www/shell/c.js.gz") && exists("sd/www/shell/c.js.gz") && exists("sd/www/shell/c.js"), "writing X.gz is a no-op");
    CHECK(!nucleo_fs_drop_stale_twin("sd/www/shell/c.js.GZ"), "writing X.GZ is a no-op too");
    // no twin -> nothing to do
    put("sd/www/shell/d.js", "raw");
    CHECK(!nucleo_fs_drop_stale_twin("sd/www/shell/d.js"), "no twin -> false");
    // out of scope: an independent .gz next to a same-named file is NEVER touched
    put("sd/data/backup.tar", "tar"); put("sd/data/backup.tar.gz", "USER-ARCHIVE");
    CHECK(!nucleo_fs_drop_stale_twin("sd/data/backup.tar") && exists("sd/data/backup.tar.gz"), "user .tar.gz untouched");
    put("sd/apps/terminal/data/h.js", "x"); put("sd/apps/terminal/data/h.js.gz", "keep");
    CHECK(!nucleo_fs_drop_stale_twin("sd/apps/terminal/data/h.js") && exists("sd/apps/terminal/data/h.js.gz"), "app data .gz untouched");
    // a DIRECTORY named like a twin is not a file: left alone
    put("sd/www/shell/e.js", "raw"); mkdirs("sd/www/shell/e.js.gz/inner"); mkdir_p("sd/www/shell/e.js.gz");
    CHECK(!nucleo_fs_drop_stale_twin("sd/www/shell/e.js") && exists("sd/www/shell/e.js.gz"), "directory twin untouched");
    // a path so long the twin name would truncate -> refuse
    char longp[300]; memset(longp, 0, sizeof longp); strcpy(longp, "sd/www/shell/");
    for (size_t i = strlen(longp); i < 268; i++) longp[i] = 'a';
    CHECK(!nucleo_fs_drop_stale_twin(longp), "over-long path refused");

    printf("fstwin: %d/%d passed%s\n", g_total - g_fail, g_total, g_fail ? "  (FAILURES)" : "");
    return g_fail ? 1 : 0;
}
