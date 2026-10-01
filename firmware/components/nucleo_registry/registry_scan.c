#include "registry_scan.h"
#include <string.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Position just after the '[' of the "installed": [ array, or 0 if there is none. The first such key wins, with
// no string/depth tracking: apps.json is machine-written ({"schema":…,"installed":[…]}), and every object found
// is still validated by cJSON before it is used.
static size_t find_array(const char *s, size_t len)
{
    static const char key[] = "\"installed\"";
    const size_t kl = sizeof key - 1;
    for (size_t i = 0; i + kl <= len; i++) {
        if (memcmp(s + i, key, kl) != 0) continue;
        size_t j = i + kl;
        while (j < len && is_ws(s[j])) j++;
        if (j >= len || s[j] != ':') continue;          // the word as a VALUE, not the key
        j++;
        while (j < len && is_ws(s[j])) j++;
        if (j < len && s[j] == '[') return j + 1;
    }
    return 0;
}

int registry_scan_installed(const char *s, size_t len, size_t *cursor, size_t *start, size_t *end)
{
    if (!s || !cursor || !start || !end) return -1;
    size_t i = *cursor;
    if (i == 0) { i = find_array(s, len); if (!i) return -1; }
    while (i < len && (is_ws(s[i]) || s[i] == ',')) i++;
    if (i >= len) return -1;                             // truncated: no closing ]
    if (s[i] == ']') { *cursor = i + 1; return 0; }
    if (s[i] != '{') return -1;
    size_t b = i;
    int depth = 0, in_str = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }              // skip the escaped char (\" never ends the string)
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (--depth == 0) { *start = b; *end = i + 1; *cursor = i + 1; return 1; }
            if (depth < 0) return -1;
        }
    }
    return -1;                                           // ran off the end inside an object
}
