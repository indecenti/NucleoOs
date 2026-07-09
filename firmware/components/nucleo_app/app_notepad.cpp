// Notes app: read and edit text files on the SD card (/data/Notes), with a proper laid-out VIEWER.
//
// VIEW renders **Markdown** (headings, bullet/numbered lists, block-quotes, horizontal rules, fenced +
// inline code, and inline emphasis) with real word-wrap and paging — .md files read like a document, not
// raw source. Plain .txt/.json get clean word-wrapped body text. TAB opens a settings card: font size
// (S/M/L) and render mode (Auto / Markdown / Plain), persisted to /data/Notes/.viewcfg.
//
// EDIT is a simple append editor (the matrix keyboard reuses ; . / for nav + typing, so EDIT types every
// printable key). Esc/Back saves & returns to the list.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "nucleo_board.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, INK = 0x0000,
                            ACC = 0x4D1F, GRN = 0x8FF3, LINE = 0x2945, CODEC = 0xFE60, QUOTC = 0x7BEF,
                            RULEC = 0x3A4C, CODEBG = 0x18E3;
#define NOTES_DIR NUCLEO_SD_MOUNT "/data/Notes"
#define CFG_PATH  NOTES_DIR "/.viewcfg"
#define MAXBUF 4096

enum { M_LIST, M_VIEW, M_EDIT, M_SET };
static char (*s_names)[56] = nullptr;   // [48][56], heap-allocated on enter()
static int s_n, s_sel, s_mode, s_vscroll;
static bool s_dirty;
static bool s_confirm_del, s_del_yes;
static char *s_buf = nullptr;           // [MAXBUF], heap-allocated on enter()
static int s_len;
static char s_file[64];
static char s_abs[256];

// ---- view settings (Tab) ----
static int s_fontsz = 1;                 // 0=S,1=M,2=L
static int s_mdmode = 0;                 // 0=Auto (md by extension), 1=Markdown always, 2=Plain always
static int s_setsel = 0;
static int s_doc_lines = 1;              // total display lines (measured each draw) for scroll clamp

static bool is_text(const char *n)
{
    const char *dot = strrchr(n, '.'); if (!dot) return false;
    return !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown") ||
           !strcasecmp(dot, ".json") || !strcasecmp(dot, ".log") || !strcasecmp(dot, ".csv") ||
           !strcasecmp(dot, ".cfg") || !strcasecmp(dot, ".ini");
}
static bool file_is_md(void)
{
    if (s_mdmode == 1) return true;
    if (s_mdmode == 2) return false;
    const char *dot = strrchr(s_file, '.');
    return dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown"));
}
static int cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void cfg_save(void)
{
    mkdir(NOTES_DIR, 0775);
    FILE *f = fopen(CFG_PATH, "wb"); if (!f) return;
    fprintf(f, "%d %d\n", s_fontsz, s_mdmode); fclose(f);
}
static void cfg_load(void)
{
    FILE *f = fopen(CFG_PATH, "rb"); if (!f) return;
    char b[32]; int n = (int)fread(b, 1, sizeof b - 1, f); fclose(f); if (n < 0) n = 0; b[n] = 0;
    int a = 1, m = 0; sscanf(b, "%d %d", &a, &m);
    s_fontsz = (a < 0 || a > 2) ? 1 : a; s_mdmode = (m < 0 || m > 2) ? 0 : m;
}

static void scan(void)
{
    s_n = 0; s_sel = 0;
    if (!s_names) return;
    DIR *dir = opendir(NOTES_DIR);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && s_n < 48)
            if (e->d_name[0] != '.' && is_text(e->d_name)) snprintf(s_names[s_n++], 56, "%s", e->d_name);
        closedir(dir);
        qsort(s_names, s_n, 56, cmp);
    }
}

static void load(const char *name)
{
    s_abs[0] = 0;
    snprintf(s_file, sizeof(s_file), "%s", name);
    char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, name);
    FILE *f = fopen(abs, "rb");
    s_len = 0;
    if (f) { if (s_buf) s_len = (int)fread(s_buf, 1, MAXBUF - 1, f); fclose(f); }
    if (s_buf) s_buf[s_len] = 0;
    s_dirty = false; s_mode = M_VIEW; s_vscroll = 0;
    nucleo_app_set_hint(";/. scorri  Invio modifica  Tab opzioni");
}
static void load_abs(const char *abs)
{
    snprintf(s_abs, sizeof(s_abs), "%s", abs);
    const char *bn = strrchr(abs, '/'); bn = bn ? bn + 1 : abs;
    snprintf(s_file, sizeof(s_file), "%s", bn);
    FILE *f = fopen(abs, "rb");
    s_len = 0;
    if (f) { if (s_buf) s_len = (int)fread(s_buf, 1, MAXBUF - 1, f); fclose(f); }
    if (s_buf) s_buf[s_len] = 0;
    s_dirty = false; s_mode = M_VIEW; s_vscroll = 0;
    nucleo_app_set_hint(";/. scorri  Invio modifica  Tab opzioni");
}
static void save(void)
{
    if (!s_dirty || !s_file[0] || !s_buf) return;
    char abs[256];
    if (s_abs[0]) snprintf(abs, sizeof(abs), "%s", s_abs);
    else { mkdir(NOTES_DIR, 0775); snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, s_file); }
    FILE *f = fopen(abs, "wb");
    if (f) { fwrite(s_buf, 1, s_len, f); fclose(f); s_dirty = false; }
}
static void new_note(void)
{
    int max = 0;
    if (s_names) for (int i = 0; i < s_n; i++) { int v; if (sscanf(s_names[i], "note-%d", &v) == 1 && v > max) max = v; }
    snprintf(s_file, sizeof(s_file), "note-%d.md", max + 1);
    s_len = 0; if (s_buf) s_buf[0] = 0; s_dirty = true; s_mode = M_EDIT; s_vscroll = 0;
    nucleo_app_set_hint("Invio a capo  Canc cancella  esc salva");
}

// ---- Markdown / text renderer ---------------------------------------------------------------------
// One pass over the buffer: split into logical lines, classify each (heading / list / quote / rule /
// code), word-wrap the body to the content width, and either COUNT display rows (measure) or DRAW the
// window [scroll, scroll+vis). Returns the total display-row count so the caller can clamp scrolling.
static int body_size(void) { return s_fontsz + 1; }               // S/M/L -> 1/2/3

// draw one wrapped segment of plain text (markers already stripped) in `col` on `bg`, faux-bold if asked.
static void draw_seg(int x, int y, int sz, uint16_t col, uint16_t bg, const char *s, bool bold)
{
    d.setTextSize(sz); d.setTextColor(col, bg); d.setCursor(x, y); d.print(s);
    if (bold) { d.setCursor(x + 1, y); d.print(s); }              // draw again offset -> heavier stroke
}

// strip inline markdown markers (**bold**, *em*, `code`, __x__) into a clean string; note if the whole
// segment was bold-wrapped (common one-word emphasis) so the renderer can faux-bold it.
static void strip_inline(const char *src, char *dst, int cap)
{
    int j = 0;
    for (int i = 0; src[i] && j < cap - 1; i++) {
        char c = src[i];
        if (c == '`') continue;
        if ((c == '*' || c == '_') && (src[i + 1] == c)) { i++; continue; }   // ** or __
        if (c == '*' || c == '_') continue;                                   // single * / _
        dst[j++] = c;
    }
    dst[j] = 0;
}

// Renders (or measures) the document. CRITICAL: `y` (the draw cursor) advances ONLY for rows that are
// actually inside the visible window — rows above `scroll` are counted (dline++) but do NOT move y, so
// the first visible row always starts at `top` (not pushed off-screen). Drawing stops once a row would
// cross the bottom, so nothing spills below the content area (no ghost/artefact rows). `measure` counts
// every display row (for the scroll clamp) and draws nothing.
static int render_doc(int scroll, bool measure)
{
    const int MARGL = 8, MARGR = 8, top = nucleo_app_content_top() + 20;
    const int bottom = nucleo_app_content_top() + nucleo_app_content_height() - 2;
    int bsz = body_size();
    int cw = 6 * bsz;
    int contentW = 240 - MARGL - MARGR;
    bool md = file_is_md();
    int dline = 0, y = top, stop = 0;

    const char *p = s_buf ? s_buf : "";
    const char *end = p + s_len;
    bool code = false;
    char raw[200], clean[200];

    while (p < end && !stop) {
        int rl = 0; while (p < end && *p != '\n' && rl < (int)sizeof raw - 1) raw[rl++] = *p++;
        raw[rl] = 0; if (p < end && *p == '\n') p++;

        int sz = bsz, indent = MARGL; uint16_t col = FG; bool bold = false, rule = false, quote = false;
        const char *text = raw; char bullet = 0;
        if (md) {
            if (!strncmp(raw, "```", 3)) { code = !code; continue; }
            if (code) { col = CODEC; text = raw; }
            else if (!strncmp(raw, "### ", 4)) { sz = bsz; col = QUOTC; bold = true; text = raw + 4; }
            else if (!strncmp(raw, "## ", 3))  { sz = bsz + 1; col = GRN; bold = true; text = raw + 3; }
            else if (!strncmp(raw, "# ", 2))   { sz = bsz + 1; col = 0x5FFB; bold = true; text = raw + 2; }
            else if ((raw[0] == '-' || raw[0] == '*' || raw[0] == '+') && raw[1] == ' ') { bullet = 1; text = raw + 2; indent = MARGL + 8; }
            else if (raw[0] == '>') { quote = true; col = QUOTC; text = raw + (raw[1] == ' ' ? 2 : 1); indent = MARGL + 6; }
            else if (!strcmp(raw, "---") || !strcmp(raw, "***") || !strcmp(raw, "___")) { rule = true; }
        }

        // ---- emit a horizontal-rule row ----
        if (rule) {
            int rh = bsz * 8 + 2;
            if (measure) dline++;
            else if (dline < scroll) dline++;
            else if (y + rh <= bottom) { d.drawFastHLine(MARGL, y + rh / 2, contentW, RULEC); y += rh; dline++; }
            else stop = 1;
            continue;
        }

        if (md && !code) strip_inline(text, clean, sizeof clean); else snprintf(clean, sizeof clean, "%s", text);

        // ---- blank line = a vertical gap (one display row, nothing drawn) ----
        if (!clean[0]) {
            int rh = sz * 8;
            if (measure) dline++;
            else if (dline < scroll) dline++;
            else if (y + rh <= bottom) { y += rh; dline++; }
            else stop = 1;
            continue;
        }

        int lineW = contentW - (indent - MARGL) - (bullet ? cw : 0);
        int maxch = lineW / (6 * sz); if (maxch < 1) maxch = 1;

        int len = (int)strlen(clean), i = 0; bool firstrow = true;
        while (i < len && !stop) {
            int take = len - i;
            if (take > maxch) {
                take = maxch;
                int brk = -1; for (int k = 0; k < maxch; k++) if (clean[i + k] == ' ') brk = k;
                if (brk > 0) take = brk;
            }
            char row[64]; int rn = take < 63 ? take : 63; memcpy(row, clean + i, rn); row[rn] = 0;
            int rh = sz * 8;
            if (measure) dline++;
            else if (dline < scroll) dline++;
            else if (y + rh <= bottom) {
                int rx = indent;
                if (code) d.fillRect(MARGL - 2, y - 1, contentW + 4, rh + 1, CODEBG);
                if (quote && firstrow) d.fillRect(MARGL, y, 2, rh, QUOTC);
                if (bullet && firstrow) d.fillCircle(MARGL + 4, y + rh / 2, 2, GRN);
                draw_seg(rx, y, sz, col, code ? CODEBG : BG, row, bold);
                y += rh; dline++;
            } else stop = 1;
            firstrow = false;
            i += take; while (i < len && clean[i] == ' ') i++;
        }
        // a touch of air under a heading (one thin display row)
        if (sz > bsz && !stop) {
            if (measure) dline++;
            else if (dline < scroll) dline++;
            else if (y + 3 <= bottom) { y += 3; dline++; }
            else stop = 1;
        }
    }
    return dline < 1 ? 1 : dline;
}

// ---- lifecycle ----
static void enter(void)
{
    nucleo_app_set_direct_draw(true);
    if (!s_names) s_names = (char (*)[56])calloc(48, sizeof *s_names);
    if (!s_buf)   s_buf   = (char *)calloc(MAXBUF, 1);
    cfg_load();
    s_dirty = false; s_file[0] = 0; s_abs[0] = 0; s_confirm_del = false;
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) { load_abs(of); return; }
    s_mode = M_LIST; scan(); nucleo_app_set_hint("Invio apri  n nuovo  d elimina");
}
static void tick(void) { if (s_mode == M_LIST && app_ui_list_animating()) nucleo_app_request_draw(); }
static void leave(void) { save(); free(s_buf); s_buf = nullptr; free(s_names); s_names = nullptr; }

static const char *nl_label(int i, void *) { return (i == 0 || !s_names) ? "+ Nuova nota" : s_names[i - 1]; }
static unsigned short nl_color(int i, void *) { return i == 0 ? GRN : ACC; }

static int view_vis(void) { int h = nucleo_app_content_height() - 22; int lh = body_size() * 8; return h / lh < 1 ? 1 : h / lh; }

static void on_key(int key, char ch)
{
    if (s_mode == M_SET) {                                        // settings card
        if (key == NK_TAB || key == NK_BACK) { s_mode = M_VIEW; cfg_save(); nucleo_app_set_hint(";/. scorri  Invio modifica  Tab opzioni"); }
        else if (key == NK_UP)   s_setsel = (s_setsel + 1) & 1;
        else if (key == NK_DOWN) s_setsel = (s_setsel + 1) & 1;
        else if (key == NK_LEFT) { if (s_setsel == 0) s_fontsz = (s_fontsz + 2) % 3; else s_mdmode = (s_mdmode + 2) % 3; }
        else if (key == NK_RIGHT || key == NK_ENTER) { if (s_setsel == 0) s_fontsz = (s_fontsz + 1) % 3; else s_mdmode = (s_mdmode + 1) % 3; }
        else return;
        nucleo_app_request_draw(); return;
    }
    if (s_mode == M_LIST) {
        int items = s_n + 1;
        if (s_confirm_del) {
            int r = app_ui_confirm_key(key, ch, &s_del_yes);
            if (r == 1 && s_sel > 0) {
                char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, s_names[s_sel - 1]);
                unlink(abs); d.fillRect(0, 0, 240, 121, 0xA000); lgfx::v1::delay(40);
                scan(); if (s_sel > s_n) s_sel = s_n;
            }
            if (r >= 0) s_confirm_del = false;
        }
        else if (app_ui_list_key(key, ch, &s_sel, items, nl_label, nullptr)) { }
        else if (key == NK_ENTER) { if (s_sel == 0) new_note(); else load(s_names[s_sel - 1]); }
        else if ((ch == 'n' || ch == 'N') && s_sel == 0) new_note();
        else if (ch == 'd' && s_sel > 0) { s_confirm_del = true; s_del_yes = false; }
        else return;
    } else if (s_mode == M_VIEW) {
        int vis = view_vis(), maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
        if (key == NK_UP)        { if (s_vscroll > 0) s_vscroll--; }
        else if (key == NK_DOWN) { if (s_vscroll < maxs) s_vscroll++; }
        else if (key == NK_TAB)  { s_mode = M_SET; s_setsel = 0; nucleo_app_set_hint("SU/GIU voce  SX/DX cambia  Tab chiudi"); }
        else if (key == NK_ENTER) { s_mode = M_EDIT; nucleo_app_set_hint("Invio a capo  Canc cancella  esc salva"); }
        else if (key == NK_DEL) { save(); s_mode = M_LIST; scan(); s_abs[0] = 0; nucleo_app_set_hint("Invio apri  n nuovo  d elimina"); }
        else return;
    } else {                                                     // M_EDIT
        if (!s_buf) return;
        if (key == NK_ENTER) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = '\n'; s_buf[s_len] = 0; s_dirty = true; } }
        else if (key == NK_DEL) { if (s_len > 0) { s_buf[--s_len] = 0; s_dirty = true; } }
        else if (key == NK_BACK) { save(); s_mode = M_LIST; scan(); s_abs[0] = 0; nucleo_app_set_hint("Invio apri  n nuovo  d elimina"); return; }
        else if (ch >= 32 && ch < 127) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = ch; s_buf[s_len] = 0; s_dirty = true; } }
        else return;
    }
    nucleo_app_request_draw();
}

static void draw_list(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    char c[16]; snprintf(c, sizeof(c), "%d not%s", s_n, s_n == 1 ? "a" : "e");
    int y0 = app_ui_title("Notes", ACC, c);
    app_ui_list(y0, top + h - y0, s_n + 1, s_sel, nl_label, nullptr, nl_color, nullptr);
}

static void draw_text(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    bool editing = (s_mode == M_EDIT);
    d.fillRect(0, top, 240, h, BG);
    char title[44]; snprintf(title, sizeof(title), "%s%s", s_file, s_dirty ? " *" : "");
    d.setTextSize(1); d.setTextColor(ACC, BG); d.setCursor(8, top + 3); d.print(title);
    const char *tag = editing ? "EDIT" : (file_is_md() ? "MD" : "TXT");
    d.setTextColor(editing ? GRN : MUTED, BG); d.setCursor(240 - (int)strlen(tag) * 6 - 6, top + 3); d.print(tag);
    d.drawFastHLine(0, top + 14, 240, LINE);

    if (editing) {
        // EDIT: plain monospace body (size 2), auto-follow the tail so typing is always visible
        const int LINEH = 16, WRAP = 19; d.setTextSize(2);
        int vis = (h - 20) / LINEH; if (vis < 1) vis = 1;
        static char lines[40][WRAP + 1]; int nlines = 0, col = 0;
        for (int i = 0; s_buf && i < s_len && nlines < 40; i++) {
            char cc = s_buf[i];
            if (cc == '\n') { lines[nlines][col] = 0; nlines++; col = 0; continue; }
            lines[nlines][col++] = cc; if (col >= WRAP) { lines[nlines][col] = 0; nlines++; col = 0; }
        }
        if (nlines < 40) lines[nlines][col] = 0;
        int total = nlines + 1, first = total > vis ? total - vis : 0, y = top + 20;
        for (int l = first; l < total && l < first + vis; l++) {
            d.setTextColor(FG, BG); d.setCursor(6, y); d.print(lines[l]);
            if (l == nlines) d.fillRect(6 + col * 12, y, 6, 14, GRN);
            y += LINEH;
        }
        if (s_len == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(6, top + 20); d.print("Scrivi..."); }
        return;
    }

    // VIEW: measure the document, clamp scroll, render the window
    int vis = view_vis();
    s_doc_lines = render_doc(0, true);
    int maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
    if (s_vscroll > maxs) s_vscroll = maxs;
    if (s_vscroll < 0) s_vscroll = 0;
    render_doc(s_vscroll, false);
    if (s_len == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(6, top + 20); d.print("(vuota)"); }

    if (s_doc_lines > vis) {                                      // scrollbar
        int track = h - 24, kh = track * vis / s_doc_lines; if (kh < 8) kh = 8;
        int ky = top + 20 + (track - kh) * s_vscroll / (maxs > 0 ? maxs : 1);
        d.fillRect(237, top + 20, 2, track, LINE);
        d.fillRect(237, ky, 2, kh, ACC);
    }
}

static void draw_settings_card(void)
{
    int cw = 200, cx = (240 - cw) / 2, cy = 30, chh = 74;
    d.fillRoundRect(cx, cy, cw, chh, 8, INK);
    d.drawRoundRect(cx, cy, cw, chh, 8, ACC);
    d.setTextSize(1); d.setTextColor(GRN, INK); d.setCursor(cx + 10, cy + 8); d.print("OPZIONI VISTA");
    const char *szn[3] = { "Piccolo", "Medio", "Grande" };
    const char *mdn[3] = { "Auto", "Markdown", "Testo" };
    const char *lbl[2] = { "Dimensione font", "Formato" };
    const char *val[2] = { szn[s_fontsz], mdn[s_mdmode] };
    for (int i = 0; i < 2; i++) {
        int y = cy + 26 + i * 20, sel = (i == s_setsel);
        if (sel) { d.fillRoundRect(cx + 6, y - 2, cw - 12, 17, 4, 0x2124); d.drawRoundRect(cx + 6, y - 2, cw - 12, 17, 4, ACC); }
        d.setTextSize(1); d.setTextColor(sel ? FG : MUTED, sel ? 0x2124 : INK); d.setCursor(cx + 12, y + 2); d.print(lbl[i]);
        d.setTextColor(0xFE60, sel ? 0x2124 : INK);
        int vw = (int)strlen(val[i]) * 6; d.setCursor(cx + cw - vw - 16, y + 2);
        d.print("< "); d.print(val[i]);
    }
}

static void draw(void)
{
    if (s_mode == M_LIST) draw_list();
    else draw_text();
    if (s_mode == M_LIST && s_confirm_del && s_sel > 0)
        app_ui_confirm("Eliminare la nota?", s_names[s_sel - 1], s_del_yes);
    if (s_mode == M_SET) draw_settings_card();
}

extern "C" void nucleo_register_notepad(void)
{
    static const nucleo_app_def_t app = {
        "notepad", "Notes", "Office", "Leggi e scrivi testo e Markdown",
        'n', 0x4D1F, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
