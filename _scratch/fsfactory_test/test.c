// Host unit test for nucleo_fs_factory_* (bundled-game protection).
// Build & run:
//   gcc -I firmware/components/nucleo_board/include _scratch/fsfactory_test/test.c -o /tmp/fsfactest && /tmp/fsfactest
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nucleo_fsfactory.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0775)
#endif

static int fails = 0, total = 0;

static void ck(const char *what, bool got, bool want)
{
    total++;
    if (got != want) {
        fails++;
        printf("  FAIL  %-52s expected %s got %s\n", what,
               want ? "true" : "false", got ? "true" : "false");
    }
}

int main(void)
{
    // ---- nucleo_fs_factory_line_eq: the core match logic ----
    ck("line exact",            nucleo_fs_factory_line_eq("doom.jsdos\n", "doom.jsdos"), true);
    ck("line case-insensitive", nucleo_fs_factory_line_eq("DOOM.JSDOS\n", "doom.jsdos"), true);
    ck("line CRLF",             nucleo_fs_factory_line_eq("doom.jsdos\r\n", "doom.jsdos"), true);
    ck("line no newline",       nucleo_fs_factory_line_eq("doom.jsdos",   "doom.jsdos"), true);
    ck("line trailing spaces",  nucleo_fs_factory_line_eq("doom.jsdos   \n", "doom.jsdos"), true);
    ck("line leading spaces",   nucleo_fs_factory_line_eq("   doom.jsdos\n", "doom.jsdos"), true);
    ck("line spaces in name",   nucleo_fs_factory_line_eq("Aladdin (USA).gb\n", "Aladdin (USA).gb"), true);
    ck("line comment skipped",  nucleo_fs_factory_line_eq("# header here\n", "header"), false);
    ck("line blank",            nucleo_fs_factory_line_eq("\n", "doom.jsdos"), false);
    ck("line empty",            nucleo_fs_factory_line_eq("", "doom.jsdos"), false);
    ck("line prefix no-match",  nucleo_fs_factory_line_eq("doom\n", "doom.jsdos"), false);
    ck("line suffix no-match",  nucleo_fs_factory_line_eq("doom.jsdos\n", "doom"), false);
    ck("line other game",       nucleo_fs_factory_line_eq("tetris.jsdos\n", "doom.jsdos"), false);
    ck("line empty base",       nucleo_fs_factory_line_eq("doom.jsdos\n", ""), false);

    // ---- nucleo_fs_factory_file_has: streaming read from a real temp folder ----
    const char *dir = "_scratch/fsfactory_test/_tmp";
    MKDIR("_scratch/fsfactory_test");
    MKDIR(dir);
    FILE *f = fopen("_scratch/fsfactory_test/_tmp/.factory", "w");
    fputs("# Bundled factory games — do not edit.\n", f);
    fputs("doom.jsdos\n", f);
    fputs("monkey1.jsdos\n", f);
    fputs("Aladdin (USA).gb\n", f);
    fputs("\n", f);
    fclose(f);

    ck("file listed",          nucleo_fs_factory_file_has(dir, "doom.jsdos"), true);
    ck("file listed spaces",   nucleo_fs_factory_file_has(dir, "Aladdin (USA).gb"), true);
    ck("file listed case",     nucleo_fs_factory_file_has(dir, "MONKEY1.JSDOS"), true);
    ck("file user import",     nucleo_fs_factory_file_has(dir, "mygame.jsdos"), false);  // not bundled → deletable
    ck("file header not name", nucleo_fs_factory_file_has(dir, "Bundled"), false);
    ck("file missing dir",     nucleo_fs_factory_file_has("_scratch/fsfactory_test/_nope", "x"), false);

    // ---- nucleo_fs_is_factory: scope gate + manifest pin (no-I/O branches) ----
    ck("scope: music free",    nucleo_fs_is_factory("/sd/data/Music/song.mp3"), false);
    ck("scope: docs free",     nucleo_fs_is_factory("/sd/data/Documents/n.txt"), false);
    ck("pin .factory in DOS",  nucleo_fs_is_factory("/sd/data/DOS/.factory"), true);
    ck("pin .factory in ROMs", nucleo_fs_is_factory("/sd/data/ROMs/gb/.factory"), true);
    ck("pin case+slashes",     nucleo_fs_is_factory("/sd/DATA//DOS/.FACTORY"), true);
    ck("DOS dir itself free",  nucleo_fs_is_factory("/sd/data/DOS"), false);             // no game, no manifest there
    ck("null safe",            nucleo_fs_is_factory(NULL), false);

    // cleanup
    remove("_scratch/fsfactory_test/_tmp/.factory");

    printf("\n%s  %d/%d passed (%d failed)\n",
           fails ? "RESULT: FAIL" : "RESULT: OK", total - fails, total, fails);
    return fails ? 1 : 0;
}
