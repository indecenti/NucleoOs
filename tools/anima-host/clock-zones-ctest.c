// Host driver for clock-zones-check.mjs: dumps the REAL table and resolves every id given on the command line.
#include "clock_zones.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    printf("default\t%d\n", clock_zone_default());
    for (int i = 0; i < clock_zone_count(); i++) {
        const clock_zone_t *z = clock_zone_at(i);
        printf("zone\t%s\t%s\t%s\n", z->id, z->label, z->posix);
    }
    if (clock_zone_at(-1) || clock_zone_at(clock_zone_count())) { printf("bounds\tFAIL\n"); return 1; }
    if (clock_zone_find(NULL) != -1 || clock_zone_find("") != -1) { printf("empty\tFAIL\n"); return 1; }
    for (int a = 1; a < argc; a++) printf("find\t%s\t%d\n", argv[a], clock_zone_find(argv[a]));
    return 0;
}
