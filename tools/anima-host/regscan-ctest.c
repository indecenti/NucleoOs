// Host gate for the registry loader's object scanner: compile the REAL
// firmware/components/nucleo_registry/registry_scan.c and prove it on the PC.
// Wired as `npm run regscan:test` (tools/anima-host/regscan-check.mjs).
//
// Invariants under test:
//   1. Every object of the real registry/apps.json is found, in order, each one a complete {...}.
//   2. Braces / brackets / quotes INSIDE strings (escaped quotes too) never end an object early.
//   3. "installed" as a string VALUE is not the array; an empty array yields no object.
//   4. Truncated or malformed text is an error (-1) — the loader then keeps the previous table.
#include "registry_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_total = 0;
#define CHECK(cond, name) do { g_total++; if (!(cond)) { g_fail++; printf("FAIL %-60s\n", name); } } while (0)

static int count(const char *s, int *err)
{
    size_t cur = 0, b, e; int n = 0, r;
    while ((r = registry_scan_installed(s, strlen(s), &cur, &b, &e)) == 1) {
        if (s[b] != '{' || s[e - 1] != '}') { *err = 1; return n; }
        n++;
    }
    *err = r < 0;
    return n;
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f); return b;
}

int main(int argc, char **argv)
{
    int err;
    // 1. the real registry (path + expected count from the runner, which parsed it with JSON.parse)
    if (argc >= 3) {
        char *txt = slurp(argv[1]);
        CHECK(txt != NULL, "real registry readable");
        if (txt) {
            int n = count(txt, &err);
            CHECK(!err && n == atoi(argv[2]), "every app of the real registry/apps.json is found");
            // first object is the first app
            size_t cur = 0, b, e;
            CHECK(registry_scan_installed(txt, strlen(txt), &cur, &b, &e) == 1 && strstr(txt + b, "\"id\"") < txt + e, "objects carry their id");
            free(txt);
        }
    }
    // 2. tricky strings
    const char *tricky = "{\"schema\":1,\"installed\":[{\"id\":\"a\",\"note\":\"} ] { [\"},"
                         "{\"id\":\"b\",\"q\":\"say \\\"hi}\\\" now\",\"perm\":[\"x\",\"y\"]},{\"id\":\"c\",\"m\":{\"k\":{}}}]}";
    CHECK(count(tricky, &err) == 3 && !err, "braces and escaped quotes inside strings");
    // 3. "installed" as a value is skipped; empty array
    CHECK(count("{\"status\":\"installed\",\"installed\":[{\"id\":\"z\"}]}", &err) == 1 && !err, "\"installed\" as a value is not the array");
    CHECK(count("{\"installed\":[]}", &err) == 0 && !err, "empty array: no objects, no error");
    CHECK(count("{\"installed\" : [ \n { \"id\" : \"w\" } \n ] }", &err) == 1 && !err, "whitespace anywhere");
    // 4. errors
    count("{\"installed\":[{\"id\":\"a\"},{\"id\":\"b\"", &err); CHECK(err, "truncated object is an error");
    count("{\"installed\":[{\"id\":\"a\"}", &err); CHECK(err, "missing ] is an error");
    count("{\"apps\":[{\"id\":\"a\"}]}", &err); CHECK(err, "no installed array is an error");
    count("{\"installed\":[1,2]}", &err); CHECK(err, "non-object entries are an error");
    size_t cur = 0, b, e;
    CHECK(registry_scan_installed(NULL, 0, &cur, &b, &e) == -1, "NULL input");

    printf("regscan: %d/%d checks pass\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
