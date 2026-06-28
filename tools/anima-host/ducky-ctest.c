// Host test for the DuckyScript engine (firmware/components/nucleo_ducky/nucleo_ducky.c). Compiled +
// run by ducky-check.mjs with MinGW gcc — no ESP-IDF, no USB/BLE. Proves the parser (commands, combos,
// REPEAT, DEFAULT_DELAY), the US/IT layout maps, and the dry-run analysis BEFORE any payload runs on a
// real host. FOR AUTHORIZED TESTING — validates byte/keycode logic only, not host behavior.
#include "nucleo_ducky.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

// Capturing backend
static uint8_t cap_mod[512], cap_key[512]; static int cap_n; static uint32_t cap_delay;
static bool b_ready(void *c) { (void)c; return true; }
static void b_key(void *c, uint8_t m, uint8_t k) { (void)c; if (cap_n < 512) { cap_mod[cap_n] = m; cap_key[cap_n] = k; cap_n++; } }
static void b_delay(void *c, uint32_t ms) { (void)c; cap_delay += ms; }
static bool b_abort(void *c) { (void)c; return false; }
static void reset_cap(void) { cap_n = 0; cap_delay = 0; }

int main(void)
{
    printf("== nucleo_ducky host test ==\n");
    uint8_t m, k;

    // 1) US char map
    CHECK(nucleo_ducky_char('a', DUCKY_LAYOUT_US, &m, &k) && m == 0 && k == 0x04, "us a");
    CHECK(nucleo_ducky_char('A', DUCKY_LAYOUT_US, &m, &k) && m == DUCKY_MOD_SHIFT && k == 0x04, "us A");
    CHECK(nucleo_ducky_char('1', DUCKY_LAYOUT_US, &m, &k) && k == 0x1E, "us 1");
    CHECK(nucleo_ducky_char('!', DUCKY_LAYOUT_US, &m, &k) && m == DUCKY_MOD_SHIFT && k == 0x1E, "us !");
    CHECK(nucleo_ducky_char('@', DUCKY_LAYOUT_US, &m, &k) && m == DUCKY_MOD_SHIFT && k == 0x1F, "us @");
    CHECK(nucleo_ducky_char(' ', DUCKY_LAYOUT_US, &m, &k) && k == 0x2C, "us space");

    // 2) IT char map: letters/digits identical, IT-specific symbols differ
    CHECK(nucleo_ducky_char('a', DUCKY_LAYOUT_IT, &m, &k) && k == 0x04, "it a parity");
    CHECK(nucleo_ducky_char('5', DUCKY_LAYOUT_IT, &m, &k) && k == 0x22, "it 5 parity");
    CHECK(nucleo_ducky_char('@', DUCKY_LAYOUT_IT, &m, &k) && m == DUCKY_MOD_ALTGR && k == 0x33, "it @ altgr");
    CHECK(nucleo_ducky_char('"', DUCKY_LAYOUT_IT, &m, &k) && m == DUCKY_MOD_SHIFT && k == 0x1F, "it dquote shift2");
    CHECK(nucleo_ducky_char('#', DUCKY_LAYOUT_IT, &m, &k) && m == DUCKY_MOD_ALTGR && k == 0x34, "it # altgr");

    // 3) key names
    CHECK(nucleo_ducky_keyname("ENTER", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x28, "name ENTER");
    CHECK(nucleo_ducky_keyname("escape", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x29, "name escape ci");
    CHECK(nucleo_ducky_keyname("GUI", DUCKY_LAYOUT_US, &m, &k) == 2 && m == DUCKY_MOD_GUI, "name GUI mod");
    CHECK(nucleo_ducky_keyname("CTRL", DUCKY_LAYOUT_US, &m, &k) == 2 && m == DUCKY_MOD_CTRL, "name CTRL mod");
    CHECK(nucleo_ducky_keyname("F5", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x3E, "name F5");
    CHECK(nucleo_ducky_keyname("F12", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x45, "name F12");
    CHECK(nucleo_ducky_keyname("r", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x15, "name single r");
    CHECK(nucleo_ducky_keyname("C", DUCKY_LAYOUT_US, &m, &k) == 1 && k == 0x06 && m == 0, "name C base (no shift)");
    CHECK(nucleo_ducky_keyname("NOPENOTAKEY", DUCKY_LAYOUT_US, &m, &k) == 0, "name unknown");

    // 4) dry-run analysis
    {
        const char *s =
            "REM open run and launch notepad\n"
            "DEFAULT_DELAY 10\n"
            "GUI r\n"
            "DELAY 200\n"
            "STRING notepad\n"
            "ENTER\n";
        nucleo_ducky_stat_t st;
        nucleo_ducky_analyze(s, strlen(s), DUCKY_LAYOUT_US, &st);
        CHECK(st.lines == 4, "analyze lines 4 (REM excluded)");
        CHECK(st.strings == 1, "analyze strings 1");
        CHECK(st.keystrokes == 9, "analyze keystrokes 9 (GUI+r, 7 chars, ENTER)");
        CHECK(st.errors == 0, "analyze no errors");
        CHECK(st.est_ms == 200 + 9*12 + 4*10, "analyze est_ms");
    }

    // 5) full run through the capturing backend — exact key sequence
    {
        const char *s = "GUI r\nSTRING ab\nENTER\n";
        nucleo_ducky_backend_t be = { b_ready, b_key, b_delay, b_abort, NULL, NULL };
        reset_cap();
        int lines = nucleo_ducky_run(s, strlen(s), DUCKY_LAYOUT_US, &be);
        CHECK(lines == 3, "run 3 lines");
        CHECK(cap_n == 4, "run 4 keystrokes");
        CHECK(cap_mod[0] == DUCKY_MOD_GUI && cap_key[0] == 0x15, "run GUI r");
        CHECK(cap_mod[1] == 0 && cap_key[1] == 0x04, "run a");
        CHECK(cap_mod[2] == 0 && cap_key[2] == 0x05, "run b");
        CHECK(cap_mod[3] == 0 && cap_key[3] == 0x28, "run ENTER");
    }

    // 6) combos with multiple modifiers + REPEAT
    {
        const char *s = "CTRL ALT DELETE\nREPEAT 2\n";   // combo, then repeat it twice = 3 total
        nucleo_ducky_backend_t be = { b_ready, b_key, b_delay, b_abort, NULL, NULL };
        reset_cap();
        nucleo_ducky_run(s, strlen(s), DUCKY_LAYOUT_US, &be);
        CHECK(cap_n == 3, "repeat -> 3 emits");
        CHECK(cap_mod[0] == (DUCKY_MOD_CTRL | DUCKY_MOD_ALT) && cap_key[0] == 0x4C, "ctrl alt del");
        CHECK(cap_mod[2] == (DUCKY_MOD_CTRL | DUCKY_MOD_ALT) && cap_key[2] == 0x4C, "repeat copy");
    }

    // 7) lone modifier taps that modifier; bad token flagged
    {
        nucleo_ducky_stat_t st;
        const char *bad = "GUI\nFLOOByTAG\n";
        nucleo_ducky_analyze(bad, strlen(bad), DUCKY_LAYOUT_US, &st);
        CHECK(st.errors == 1, "one bad token");
        CHECK(strcmp(st.first_error, "FLOOByTAG") == 0, "first_error captured");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
