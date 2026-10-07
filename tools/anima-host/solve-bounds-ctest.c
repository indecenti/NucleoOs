// SOLVE-BOUNDS unit test — memory-safety of the offline math engine's string/number helpers.
//
// The helpers under test are `static` in anima_solve.c, so we #include the REAL translation unit (as
// stitch-dedup-ctest.c does for l1) — the test can never drift from what the device runs. The runner
// (solve-bounds-check.mjs) links it against the same host sources as anima.exe, minus that file and main.
//
// Why a unit test: the host exe has no sanitizer, so a few-byte stack overrun in a_subst_regs (the
// `o += snprintf(...)` return-value trap) never crashes an end-to-end probe. A guard band after the output
// buffer makes the overrun visible deterministically.
#include "anima_solve.c"

#include <stdio.h>
#include <limits.h>

static int failures = 0;
static void check(const char *what, int cond, const char *detail)
{
    printf("  %s %s%s%s\n", cond ? "ok  " : "FAIL", what, detail ? " — " : "", detail ? detail : "");
    if (!cond) failures++;
}

int main(void)
{
    printf("[solve-bounds] a_subst_regs (register substitution into a fixed buffer):\n");
    anima_reg_set("a", 123456789.0);                      // every "a" expands 1 -> 9 chars
    struct { char out[64]; unsigned char guard[32]; } g;
    memset(&g, 0x5A, sizeof g);
    int n = a_subst_regs("a + a + a + a + a + a + a + a + a + a", g.out, sizeof g.out);
    int clean = 1; for (size_t i = 0; i < sizeof g.guard; i++) if (g.guard[i] != 0x5A) clean = 0;
    char d[96]; snprintf(d, sizeof d, "nsub=%d len=%d", n, (int)strnlen(g.out, sizeof g.out));
    check("no write past cap", clean, d);
    check("output NUL-terminated inside cap", memchr(g.out, 0, sizeof g.out) != NULL, NULL);
    check("overflow reported (-1), not a clipped expression", n == -1, d);
    struct { char out[16]; unsigned char guard[16]; } f; memset(&f, 0x5A, sizeof f);
    n = a_subst_regs("a + 1", f.out, sizeof f.out);
    check("fitting input substitutes", n == 1 && !strcmp(f.out, "123456789 + 1"), f.out);
    // exact fit / tiny caps must also stay inside
    struct { char out[4]; unsigned char guard[16]; } t; memset(&t, 0x5A, sizeof t);
    n = a_subst_regs("a", t.out, sizeof t.out);
    clean = 1; for (size_t i = 0; i < sizeof t.guard; i++) if (t.guard[i] != 0x5A) clean = 0;
    check("tiny cap (4) stays inside + reports -1", clean && memchr(t.out, 0, sizeof t.out) != NULL && n == -1, t.out);

    printf("[solve-bounds] a_from_base (radix parse must DECLINE on overflow, not wrap):\n");
    unsigned long long v = 0;
    check("16 x F fits u64",   a_from_base("ffffffffffffffff", 16, &v) && v == ULLONG_MAX, NULL);
    check("17 x F declines",   !a_from_base("fffffffffffffffff", 16, &v), "2^68 would silently wrap");
    check("base36 13 chars declines", !a_from_base("zzzzzzzzzzzzz", 36, &v), "36^13 > 2^64");
    check("u64 max decimal fits", a_from_base("18446744073709551615", 10, &v) && v == ULLONG_MAX, NULL);
    check("u64 max + 1 declines", !a_from_base("18446744073709551616", 10, &v), NULL);
    check("plain value ok",    a_from_base("ff", 16, &v) && v == 255, NULL);

    printf("[solve-bounds] a_solve_base (a token cut by the 19-char tokenizer must decline):\n");
    anima_result_t r; memset(&r, 0, sizeof r);
    bool got = a_solve_base("converti 1111111111111111111111111 da binario in decimale", false, &r);
    check("25-digit binary declines", !got, got ? r.reply : NULL);
    memset(&r, 0, sizeof r);
    got = a_solve_base("converti 1111 da binario in decimale", false, &r);
    check("short binary still answers", got && strstr(r.reply, "15"), r.reply);

    printf(failures ? "[solve-bounds] %d FAILED\n" : "[solve-bounds] all checks passed\n", failures);
    return failures ? 1 : 0;
}
