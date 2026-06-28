// nucleo_ducky — see nucleo_ducky.h. Pure DuckyScript engine: parse + drive an injected backend.
// FOR AUTHORIZED TESTING ONLY. No NimBLE/USB/ESP deps here — host-testable (gate: ducky-check.mjs).
#include "nucleo_ducky.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// ---- keyboard layouts: char -> (modifier, HID keycode) -------------------------------------------
// US is complete and host-tested. IT covers letters/digits/space + the IT-specific shifted-number and
// AltGr symbols; anything it can't map returns false (skipped) rather than mistyping. (IT beyond letters
// /digits needs on-an-IT-host verification — that's why the layout is selectable, not assumed.)
static bool us_char(char c, uint8_t *m, uint8_t *k)
{
    uint8_t mod = 0, key = 0;
    if (c >= 'a' && c <= 'z') key = 0x04 + (c - 'a');
    else if (c >= 'A' && c <= 'Z') { mod = DUCKY_MOD_SHIFT; key = 0x04 + (c - 'A'); }
    else if (c >= '1' && c <= '9') key = 0x1E + (c - '1');
    else if (c == '0') key = 0x27;
    else switch (c) {
        case '\n': case '\r': key = 0x28; break; case '\t': key = 0x2B; break; case ' ': key = 0x2C; break;
        case '-': key = 0x2D; break;  case '_': mod = DUCKY_MOD_SHIFT; key = 0x2D; break;
        case '=': key = 0x2E; break;  case '+': mod = DUCKY_MOD_SHIFT; key = 0x2E; break;
        case '[': key = 0x2F; break;  case '{': mod = DUCKY_MOD_SHIFT; key = 0x2F; break;
        case ']': key = 0x30; break;  case '}': mod = DUCKY_MOD_SHIFT; key = 0x30; break;
        case '\\': key = 0x31; break; case '|': mod = DUCKY_MOD_SHIFT; key = 0x31; break;
        case ';': key = 0x33; break;  case ':': mod = DUCKY_MOD_SHIFT; key = 0x33; break;
        case '\'': key = 0x34; break; case '"': mod = DUCKY_MOD_SHIFT; key = 0x34; break;
        case '`': key = 0x35; break;  case '~': mod = DUCKY_MOD_SHIFT; key = 0x35; break;
        case ',': key = 0x36; break;  case '<': mod = DUCKY_MOD_SHIFT; key = 0x36; break;
        case '.': key = 0x37; break;  case '>': mod = DUCKY_MOD_SHIFT; key = 0x37; break;
        case '/': key = 0x38; break;  case '?': mod = DUCKY_MOD_SHIFT; key = 0x38; break;
        case '!': mod = DUCKY_MOD_SHIFT; key = 0x1E; break;  case '@': mod = DUCKY_MOD_SHIFT; key = 0x1F; break;
        case '#': mod = DUCKY_MOD_SHIFT; key = 0x20; break;  case '$': mod = DUCKY_MOD_SHIFT; key = 0x21; break;
        case '%': mod = DUCKY_MOD_SHIFT; key = 0x22; break;  case '^': mod = DUCKY_MOD_SHIFT; key = 0x23; break;
        case '&': mod = DUCKY_MOD_SHIFT; key = 0x24; break;  case '*': mod = DUCKY_MOD_SHIFT; key = 0x25; break;
        case '(': mod = DUCKY_MOD_SHIFT; key = 0x26; break;  case ')': mod = DUCKY_MOD_SHIFT; key = 0x27; break;
        default: return false;
    }
    *m = mod; *k = key; return true;
}

static bool it_char(char c, uint8_t *m, uint8_t *k)
{
    uint8_t mod = 0, key = 0;
    if (c >= 'a' && c <= 'z') key = 0x04 + (c - 'a');                 // letters: same physical keys as US
    else if (c >= 'A' && c <= 'Z') { mod = DUCKY_MOD_SHIFT; key = 0x04 + (c - 'A'); }
    else if (c >= '1' && c <= '9') key = 0x1E + (c - '1');           // digits unshifted: same
    else if (c == '0') key = 0x27;
    else switch (c) {
        case '\n': case '\r': key = 0x28; break; case '\t': key = 0x2B; break; case ' ': key = 0x2C; break;
        // IT shifted number row: 1! 2" 4$ 5% 6& 7/ 8( 9) 0=
        case '!': mod = DUCKY_MOD_SHIFT; key = 0x1E; break;  case '"': mod = DUCKY_MOD_SHIFT; key = 0x1F; break;
        case '$': mod = DUCKY_MOD_SHIFT; key = 0x21; break;  case '%': mod = DUCKY_MOD_SHIFT; key = 0x22; break;
        case '&': mod = DUCKY_MOD_SHIFT; key = 0x23; break;  case '/': mod = DUCKY_MOD_SHIFT; key = 0x24; break;
        case '(': mod = DUCKY_MOD_SHIFT; key = 0x25; break;  case ')': mod = DUCKY_MOD_SHIFT; key = 0x26; break;
        case '=': mod = DUCKY_MOD_SHIFT; key = 0x27; break;
        case '\'': key = 0x2D; break;  case '?': mod = DUCKY_MOD_SHIFT; key = 0x2D; break;   // ' / ?
        case ',': key = 0x36; break;   case ';': mod = DUCKY_MOD_SHIFT; key = 0x36; break;
        case '.': key = 0x37; break;   case ':': mod = DUCKY_MOD_SHIFT; key = 0x37; break;
        case '-': key = 0x38; break;   case '_': mod = DUCKY_MOD_SHIFT; key = 0x38; break;
        case '+': key = 0x30; break;   case '*': mod = DUCKY_MOD_SHIFT; key = 0x30; break;
        case '<': key = 0x64; break;   case '>': mod = DUCKY_MOD_SHIFT; key = 0x64; break;   // ISO key
        case '@': mod = DUCKY_MOD_ALTGR; key = 0x33; break;   case '#': mod = DUCKY_MOD_ALTGR; key = 0x34; break;
        case '[': mod = DUCKY_MOD_ALTGR; key = 0x2F; break;   case ']': mod = DUCKY_MOD_ALTGR; key = 0x30; break;
        case '{': mod = DUCKY_MOD_ALTGR | DUCKY_MOD_SHIFT; key = 0x2F; break;
        case '}': mod = DUCKY_MOD_ALTGR | DUCKY_MOD_SHIFT; key = 0x30; break;
        default: return false;
    }
    *m = mod; *k = key; return true;
}

bool nucleo_ducky_char(char c, nucleo_ducky_layout_t layout, uint8_t *mod, uint8_t *keycode)
{
    return layout == DUCKY_LAYOUT_IT ? it_char(c, mod, keycode) : us_char(c, mod, keycode);
}

// ---- key name resolution -------------------------------------------------------------------------
static bool ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

int nucleo_ducky_keyname(const char *name, nucleo_ducky_layout_t layout, uint8_t *mod, uint8_t *keycode)
{
    *mod = 0; *keycode = 0;
    if (!name || !*name) return 0;
    // modifiers (return 2)
    if (ieq(name, "CTRL") || ieq(name, "CONTROL")) { *mod = DUCKY_MOD_CTRL; return 2; }
    if (ieq(name, "SHIFT"))                          { *mod = DUCKY_MOD_SHIFT; return 2; }
    if (ieq(name, "ALT") || ieq(name, "OPTION"))     { *mod = DUCKY_MOD_ALT; return 2; }
    if (ieq(name, "ALTGR") || ieq(name, "RALT"))     { *mod = DUCKY_MOD_ALTGR; return 2; }
    if (ieq(name, "GUI") || ieq(name, "WINDOWS") || ieq(name, "WIN") || ieq(name, "COMMAND") || ieq(name, "META"))
        { *mod = DUCKY_MOD_GUI; return 2; }
    // named keys (return 1)
    static const struct { const char *n; uint8_t k; } T[] = {
        {"ENTER",0x28},{"RETURN",0x28},{"ESC",0x29},{"ESCAPE",0x29},{"BACKSPACE",0x2A},{"BKSP",0x2A},
        {"TAB",0x2B},{"SPACE",0x2C},{"CAPSLOCK",0x39},{"CAPS",0x39},{"DELETE",0x4C},{"DEL",0x4C},
        {"INSERT",0x49},{"INS",0x49},{"HOME",0x4A},{"END",0x4D},{"PAGEUP",0x4B},{"PGUP",0x4B},
        {"PAGEDOWN",0x4E},{"PGDN",0x4E},{"UP",0x52},{"UPARROW",0x52},{"DOWN",0x51},{"DOWNARROW",0x51},
        {"LEFT",0x50},{"LEFTARROW",0x50},{"RIGHT",0x4F},{"RIGHTARROW",0x4F},{"PRINTSCREEN",0x46},
        {"SCROLLLOCK",0x47},{"PAUSE",0x48},{"BREAK",0x48},{"MENU",0x65},{"APP",0x65},{"NUMLOCK",0x53},
    };
    for (unsigned i = 0; i < sizeof(T)/sizeof(T[0]); i++) if (ieq(name, T[i].n)) { *keycode = T[i].k; return 1; }
    // function keys F1..F12
    if ((name[0] == 'F' || name[0] == 'f') && name[1]) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) { *keycode = 0x3A + (n - 1); return 1; }
    }
    // single character -> base keycode (letters case-insensitive so "CTRL c" works)
    if (name[1] == 0) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') { *keycode = 0x04 + (c - 'a'); return 1; }
        uint8_t m, k;
        if (nucleo_ducky_char(c, layout, &m, &k)) { *mod = m; *keycode = k; return 1; }
    }
    return 0;
}

// ---- engine (shared by analyze + run) ------------------------------------------------------------
static uint32_t atou(const char *s) { while (*s == ' ') s++; uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0'); return v; }

static void emit_key(const nucleo_ducky_backend_t *be, nucleo_ducky_stat_t *st, uint8_t m, uint8_t k)
{
    if (be) be->key(be->ctx, m, k);
    if (st) { st->keystrokes++; st->est_ms += 12; }
}

static void type_string(const char *s, nucleo_ducky_layout_t layout, const nucleo_ducky_backend_t *be,
                        nucleo_ducky_stat_t *st, uint32_t str_delay)
{
    for (; *s; s++) {
        uint8_t m, k;
        if (!nucleo_ducky_char(*s, layout, &m, &k)) continue;   // unmappable char -> skipped, not mistyped
        emit_key(be, st, m, k);
        if (str_delay) { if (be) be->delay(be->ctx, str_delay); if (st) st->est_ms += str_delay; }
    }
}

static void exec_combo(const char *line, nucleo_ducky_layout_t layout, const nucleo_ducky_backend_t *be,
                       nucleo_ducky_stat_t *st)
{
    uint8_t mod = 0, key = 0; bool have = false, err = false; char tok[24];
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int i = 0; while (*p && *p != ' ' && *p != '\t' && i < 23) tok[i++] = *p++;
        tok[i] = 0;
        uint8_t m = 0, k = 0; int r = nucleo_ducky_keyname(tok, layout, &m, &k);
        if (r == 2) mod |= m;
        else if (r == 1) { if (!have) { key = k; have = true; } mod |= m; }
        else { err = true; if (st && st->errors == 0) { strncpy(st->first_error, tok, sizeof(st->first_error)-1); st->first_error[sizeof(st->first_error)-1]=0; } }
    }
    if (err) { if (st) st->errors++; return; }
    if (!have && !mod) return;    // empty / no-op line emits nothing
    emit_key(be, st, mod, key);   // lone modifier (key=0) taps that modifier (e.g. GUI = open Start)
}

// Run ONE command line (no REPEAT, no default-delay application — the caller owns those). Returns true
// if the line was a CONFIG directive (DEFAULT_DELAY / STRING_DELAY): those set state, are not "actions",
// so the caller must NOT count them as a line nor apply the inter-command delay after them.
static bool run_command(const char *line, nucleo_ducky_layout_t layout, const nucleo_ducky_backend_t *be,
                        nucleo_ducky_stat_t *st, uint32_t *def_delay, uint32_t *str_delay)
{
    char cmd[20]; int i = 0; const char *q = line;
    while (*q && *q != ' ' && i < 19) { cmd[i++] = (char)toupper((unsigned char)*q); q++; }
    cmd[i] = 0;
    const char *arg = (*q == ' ') ? q + 1 : q;

    if (!strcmp(cmd, "STRING"))        { type_string(arg, layout, be, st, *str_delay); if (st) st->strings++; }
    else if (!strcmp(cmd, "STRINGLN")) { type_string(arg, layout, be, st, *str_delay); emit_key(be, st, 0, 0x28); if (st) st->strings++; }
    else if (!strcmp(cmd, "DELAY"))    { uint32_t n = atou(arg); if (be) be->delay(be->ctx, n); if (st) st->est_ms += n; }
    else if (!strcmp(cmd, "DEFAULTDELAY") || !strcmp(cmd, "DEFAULT_DELAY")) { *def_delay = atou(arg); return true; }
    else if (!strcmp(cmd, "STRINGDELAY") || !strcmp(cmd, "STRING_DELAY"))   { *str_delay = atou(arg); return true; }
    else { exec_combo(line, layout, be, st); }
    return false;
}

static bool is_rem(const char *line) { return (line[0]=='R'||line[0]=='r') && (line[1]=='E'||line[1]=='e') && (line[2]=='M'||line[2]=='m') && (line[3]==0||line[3]==' '||line[3]=='\t'); }

static int process(const char *script, size_t len, nucleo_ducky_layout_t layout,
                   const nucleo_ducky_backend_t *be, nucleo_ducky_stat_t *st, int total)
{
    uint32_t def_delay = 0, str_delay = 0;
    char line[256], prev[256]; prev[0] = 0;
    const char *p = script, *e = script + len;
    int idx = 0;

    while (p < e) {
        const char *ls = p;
        while (p < e && *p != '\n') p++;
        int n = (int)(p - ls); if (p < e) p++;                  // consume '\n'
        if (n > 0 && ls[n-1] == '\r') n--;                      // strip CR
        if (n > 255) n = 255;
        memcpy(line, ls, n); line[n] = 0;
        char *t = line; while (*t == ' ' || *t == '\t') t++;    // leading trim
        if (!*t) continue;                                      // blank line
        if (is_rem(t)) continue;                                // comment

        if (!strncasecmp(t, "REPEAT", 6) && (t[6]==0 || t[6]==' ')) {
            uint32_t r = atou(t + 6);
            for (uint32_t k = 0; k < r; k++) {
                if (be && (be->aborted(be->ctx) || !be->ready(be->ctx))) return idx;
                run_command(prev, layout, be, st, &def_delay, &str_delay);
                if (def_delay) { if (be) be->delay(be->ctx, def_delay); if (st) st->est_ms += def_delay; }
                idx++; if (st) st->lines++;
                if (be && be->progress) be->progress(be->ctx, idx, total);
            }
            continue;
        }

        if (be && (be->aborted(be->ctx) || !be->ready(be->ctx))) return idx;
        if (run_command(t, layout, be, st, &def_delay, &str_delay)) continue;   // config directive: don't count
        if (def_delay) { if (be) be->delay(be->ctx, def_delay); if (st) st->est_ms += def_delay; }
        strncpy(prev, t, sizeof(prev) - 1); prev[sizeof(prev)-1] = 0;
        idx++; if (st) st->lines++;
        if (be && be->progress) be->progress(be->ctx, idx, total);
    }
    return idx;
}

void nucleo_ducky_analyze(const char *script, size_t len, nucleo_ducky_layout_t layout, nucleo_ducky_stat_t *out)
{
    memset(out, 0, sizeof(*out));
    process(script, len, layout, NULL, out, 0);
}

int nucleo_ducky_run(const char *script, size_t len, nucleo_ducky_layout_t layout, const nucleo_ducky_backend_t *be)
{
    nucleo_ducky_stat_t st; nucleo_ducky_analyze(script, len, layout, &st);
    return process(script, len, layout, be, NULL, st.lines);
}
