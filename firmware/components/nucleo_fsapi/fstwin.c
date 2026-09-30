// fstwin.c — see include/fstwin.h.
#include "fstwin.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strncasecmp/strcasecmp: FATFS names are case-insensitive
#include <sys/stat.h>

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

bool nucleo_fs_twin_scope(const char *abs)
{
    if (!abs) return false;
    static const char root[] = NUCLEO_SD_MOUNT "/";
    if (strncasecmp(abs, root, sizeof root - 1) != 0) return false;
    const char *rel = abs + sizeof root - 1;
    if (strncasecmp(rel, "www/shell/", 10) == 0) return rel[10] != '\0';
    if (strncasecmp(rel, "apps/", 5) == 0) {
        const char *id = rel + 5;
        const char *slash = strchr(id, '/');
        if (!slash || slash == id) return false;                    // "apps/<id>" itself, or "apps//"
        return strncasecmp(slash, "/www/", 5) == 0 && slash[5] != '\0';
    }
    return false;
}

bool nucleo_fs_drop_stale_twin(const char *abs)
{
    if (!nucleo_fs_twin_scope(abs) || ends_with(abs, ".gz")) return false;
    char twin[272];
    int n = snprintf(twin, sizeof twin, "%s.gz", abs);
    if (n <= 0 || n >= (int)sizeof twin) return false;              // never act on a truncated path
    struct stat st;
    if (stat(twin, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return remove(twin) == 0;
}
