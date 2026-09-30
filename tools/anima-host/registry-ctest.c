// Host gate for the firmware app registry (firmware/components/nucleo_registry/nucleo_registry.c), compiled
// on the PC against the IDF cJSON and the ui-host shims (NUCLEO_SD_MOUNT = "sd", relative). Driven by
// tools/anima-host/registry-check.mjs, which builds each fixture card and the expected values.
//   registry-ctest <fixture-dir> <expected.tsv> <expected-count>
// expected.tsv: one line per app, "id\tname\troute\ticon\tenabled" — exactly what /api/apps reported with
// the old 157-byte entries (manifest strings verbatim, "" when absent). The gate proves the compact 94-byte
// entry reproduces them byte for byte, and that the cap holds user (Agent) apps.
#include "nucleo_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: registry-ctest <dir> <expected.tsv> <count>\n"); return 2; }
    FILE *ex = fopen(argv[2], "rb");
    if (!ex) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    if (chdir(argv[1]) != 0) { fprintf(stderr, "cannot chdir %s\n", argv[1]); return 2; }
    int want_count = atoi(argv[3]);
    int fail = 0, checked = 0;

    if (nucleo_registry_load() != 0) { printf("FAIL load\n"); return 1; }
    int n = nucleo_registry_count();
    if (n != want_count) { printf("FAIL count %d != %d\n", n, want_count); fail++; }
    const nucleo_app_t *apps = nucleo_registry_apps();

    char line[512];
    int row = 0;
    while (fgets(line, sizeof line, ex)) {
        line[strcspn(line, "\r\n")] = 0;
        char *f[5] = {0};
        char *p = line;
        for (int k = 0; k < 5; k++) { f[k] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (!f[4]) { printf("FAIL bad expected line %d\n", row); fail++; break; }
        if (row >= n) { printf("FAIL app %s missing (row %d)\n", f[0], row); fail++; row++; continue; }
        const nucleo_app_t *a = &apps[row];
        char r[64], ic[64];
        nucleo_registry_route(a, r, sizeof r);
        nucleo_registry_icon(a, ic, sizeof ic);
        if (strcmp(a->id, f[0]))    { printf("FAIL [%d] id '%s' != '%s'\n", row, a->id, f[0]); fail++; }
        if (strcmp(a->name, f[1]))  { printf("FAIL [%s] name '%s' != '%s'\n", f[0], a->name, f[1]); fail++; }
        if (strcmp(r, f[2]))        { printf("FAIL [%s] route '%s' != '%s'\n", f[0], r, f[2]); fail++; }
        if (strcmp(ic, f[3]))       { printf("FAIL [%s] icon '%s' != '%s'\n", f[0], ic, f[3]); fail++; }
        if (a->enabled != (f[4][0] == '1')) { printf("FAIL [%s] enabled\n", f[0]); fail++; }
        row++; checked++;
    }
    fclose(ex);
    printf("registry-ctest: %d apps loaded, %d checked, %d failure(s)\n", n, checked, fail);
    return fail ? 1 : 0;
}
