// Notes — read + edit text/Markdown on the SD card (/data/Notes), with a laid-out VIEWER and a
// lightweight note MANAGER.
//
// VIEW renders Markdown (headings, bullet/numbered lists, block-quotes, rules, fenced + inline code,
// inline emphasis) with real word-wrap, paging and a live reading-progress %. Plain .txt/.json get
// clean word-wrapped text. TAB opens a settings card (font S/M/L, render Auto/Markdown/Plain).
// RIGHT (on the list) opens an Actions card: Open · Rename · Duplicate · Details · Delete.
// LEFT/BACK is a real "back": step out of settings → view → list → exit (and it no longer eats the
// comma key in the editor). EDIT is an append editor. Palette comes from launcher_theme.h so Notes
// follows the OS theme; all user strings are bilingual via TR().
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
#include <time.h>
extern "C" {
#include "nucleo_board.h"
}
#include "nucleo_i18n.h"        // TR("it","en")
#include "launcher_theme.h"     // shared OS palette — Notes follows the theme like every other app
#include "nucleo_audio.h"       // soft clicks on discrete actions
#include "app_gfx.h"
#include "app_large.h"         // two screens: the notes on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

// Semantic accents mapped onto the shared palette (theme-following, coherent Markdown highlighting).
#define ACC     C_BLUE
#define GRN     C_GREEN
#define H1C     C_BLUE         // # heading
#define H2C     C_GREEN        // ## heading
#define H3C     C_PINK         // ### heading
#define CODEC   C_YELLOW       // code text
#define CODEBG  INK            // code block background
#define QUOTC   C_GREY         // block-quote
#define RULEC   LINE           // horizontal rule

#define NOTES_DIR NUCLEO_SD_MOUNT "/data/Notes"
#define CFG_PATH  NOTES_DIR "/.viewcfg"
#define MAXBUF 4096

enum { M_LIST, M_VIEW, M_EDIT, M_SET, M_ACT, M_INPUT, M_DETAILS };
static char (*s_names)[56] = nullptr;   // [48][56], heap-allocated on enter()
static int s_n, s_sel, s_mode, s_vscroll;
static bool s_dirty;
static bool s_trunc;                     // file larger than the buffer: read-only (a save would cut it)
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

// ---- manager (Actions / rename / details) ----
#define ACT_N 5
static int  s_actsel;
static char s_target[64];                // note the Actions/Details/Rename card acts on
static char s_input[64];                 // rename text buffer
static long s_stat_bytes, s_stat_lines, s_stat_words;

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

// ---- hints (bilingual) ----
static void list_hint(void) { nucleo_app_set_hint(TR5("INVIO apri  ->azioni  n nuova  d elim", "ENTER open  ->actions  n new  d del", "INTRO abre  ->accion  n nuevo  d borra", "ENTREE ouvre  ->action  n nouv  d sup", "ENTER abre  ->Aktion  n neu  d loescht")); }
static void view_hint(void) { nucleo_app_set_hint(TR5(";/. scorri invio modif tab opz <-lista", ";/. scroll  enter edit  tab opt  <-list", ";/. desplaza  intro edit  tab opt  <-li", ";/. defil  enter edit  tab opt  <-list", ";/. scroll  enter edit  tab opt  <-lst")); }
static void edit_hint(void) { nucleo_app_set_hint(TR5("INVIO a capo  CANC cancella  <-salva", "ENTER newline  DEL backspace  <-save", "INTRO linea  SUPR atras  <-guardar", "ENTREE nouv ligne  SUPP ret  <-sauver", "ENTER zeile  ENTF zurueck  <-speich.")); }

static void load(const char *name)
{
    s_abs[0] = 0;
    snprintf(s_file, sizeof(s_file), "%s", name);
    char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, name);
    FILE *f = fopen(abs, "rb");
    s_len = 0; s_trunc = false;
    if (f) { if (s_buf) { s_len = (int)fread(s_buf, 1, MAXBUF - 1, f); s_trunc = (s_len == MAXBUF - 1 && fgetc(f) != EOF); } fclose(f); }
    if (s_buf) s_buf[s_len] = 0;
    s_dirty = false; s_mode = M_VIEW; s_vscroll = 0;
    view_hint();
}
static void load_abs(const char *abs)
{
    snprintf(s_abs, sizeof(s_abs), "%s", abs);
    const char *bn = strrchr(abs, '/'); bn = bn ? bn + 1 : abs;
    snprintf(s_file, sizeof(s_file), "%s", bn);
    FILE *f = fopen(abs, "rb");
    s_len = 0; s_trunc = false;
    if (f) { if (s_buf) { s_len = (int)fread(s_buf, 1, MAXBUF - 1, f); s_trunc = (s_len == MAXBUF - 1 && fgetc(f) != EOF); } fclose(f); }
    if (s_buf) s_buf[s_len] = 0;
    s_dirty = false; s_mode = M_VIEW; s_vscroll = 0;
    view_hint();
}
static void save(void)
{
    if (!s_dirty || !s_file[0] || !s_buf || s_trunc) return;
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
    s_len = 0; if (s_buf) s_buf[0] = 0; s_dirty = true; s_trunc = false; s_mode = M_EDIT; s_vscroll = 0;
    edit_hint();
}

// ---- manager helpers ----
static void dup_note(const char *name)
{
    char src[200]; snprintf(src, sizeof src, "%s/%s", NOTES_DIR, name);
    char base[56]; snprintf(base, sizeof base, "%s", name);
    char ext[12] = ""; char *dot = strrchr(base, '.'); if (dot) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    if (!ext[0]) snprintf(ext, sizeof ext, ".md");
    char dst[240]; int k = 0;
    do {
        if (k == 0) snprintf(dst, sizeof dst, "%s/%s-copy%s",  NOTES_DIR, base, ext);
        else        snprintf(dst, sizeof dst, "%s/%s-copy%d%s", NOTES_DIR, base, k, ext);
        k++;
    } while (access(dst, F_OK) == 0 && k < 50);
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (in && out) { char b[512]; size_t r; while ((r = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, r, out); }
    if (in) fclose(in);
    if (out) fclose(out);
    nucleo_app_set_hint(TR5("Duplicata", "Duplicated", "Duplicada", "Dupliquee", "Dupliziert"));
}
static void count_stats(const char *name)
{
    char abs[200]; snprintf(abs, sizeof abs, "%s/%s", NOTES_DIR, name);
    s_stat_bytes = s_stat_words = 0; long nl = 0; bool inword = false;
    FILE *f = fopen(abs, "rb"); if (f) {
        char b[256]; size_t r;
        while ((r = fread(b, 1, sizeof b, f)) > 0) {
            s_stat_bytes += (long)r;
            for (size_t i = 0; i < r; i++) {
                char c = b[i]; if (c == '\n') nl++;
                bool ws = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
                if (!ws && !inword) { s_stat_words++; inword = true; } else if (ws) inword = false;
            }
        }
        fclose(f);
    }
    s_stat_lines = nl + (s_stat_bytes > 0 ? 1 : 0);
}

// ---- Markdown / text renderer ---------------------------------------------------------------------
static int body_size(void) { return s_fontsz + 1; }               // S/M/L -> 1/2/3

static void draw_seg(int x, int y, int sz, uint16_t col, uint16_t bg, const char *s, bool bold)
{
    d.setTextSize(sz); d.setTextColor(col, bg); d.setCursor(x, y); d.print(s);
    if (bold) { d.setCursor(x + 1, y); d.print(s); }              // draw again offset -> heavier stroke
}

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

// Where render_doc lays the document out: the Cardputer's content area, or the panel's body (two screens).
static int s_doc_x = 8, s_doc_w = 224, s_doc_top = -1, s_doc_bot = -1;
static int render_doc(int scroll, bool measure)
{
    const int MARGL = s_doc_x, top = s_doc_top >= 0 ? s_doc_top : nucleo_app_content_top() + 20;
    const int bottom = s_doc_bot >= 0 ? s_doc_bot : nucleo_app_content_top() + nucleo_app_content_height() - 2;
    int bsz = body_size();
    int cw = 6 * bsz;
    int contentW = s_doc_w;
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
            else if (!strncmp(raw, "### ", 4)) { sz = bsz; col = H3C; bold = true; text = raw + 4; }
            else if (!strncmp(raw, "## ", 3))  { sz = bsz + 1; col = H2C; bold = true; text = raw + 3; }
            else if (!strncmp(raw, "# ", 2))   { sz = bsz + 1; col = H1C; bold = true; text = raw + 2; }
            else if ((raw[0] == '-' || raw[0] == '*' || raw[0] == '+') && raw[1] == ' ') { bullet = 1; text = raw + 2; indent = MARGL + 8; }
            else if (raw[0] == '>') { quote = true; col = QUOTC; text = raw + (raw[1] == ' ' ? 2 : 1); indent = MARGL + 6; }
            else if (!strcmp(raw, "---") || !strcmp(raw, "***") || !strcmp(raw, "___")) { rule = true; }
        }

        if (rule) {
            int rh = bsz * 8 + 2;
            if (measure) dline++;
            else if (dline < scroll) dline++;
            else if (y + rh <= bottom) { d.drawFastHLine(MARGL, y + rh / 2, contentW, RULEC); y += rh; dline++; }
            else stop = 1;
            continue;
        }

        if (md && !code) strip_inline(text, clean, sizeof clean); else snprintf(clean, sizeof clean, "%s", text);

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
static bool notes_on_back(int key);   // forward (LEFT/BACK handler)

// The framework never hands TAB to on_key: unclaimed, it toggles the Control Center. Claim it
// (view-options card in M_VIEW / M_SET).
static void on_key(int key, char ch);
static void notes_tab(void) { on_key(NK_TAB, 0); }
static void enter(void)
{
    nucleo_app_set_direct_draw(true);
    nucleo_app_set_back_handler(notes_on_back);
    nucleo_app_set_tab_handler(notes_tab);
    if (!s_names) s_names = (char (*)[56])calloc(48, sizeof *s_names);
    if (!s_buf)   s_buf   = (char *)calloc(MAXBUF, 1);
    cfg_load();
    s_dirty = false; s_file[0] = 0; s_abs[0] = 0; s_confirm_del = false;
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) { load_abs(of); return; }
    s_mode = M_LIST; scan(); list_hint();
}
static void tick(void) { if (s_mode == M_LIST && app_ui_list_animating()) nucleo_app_request_draw(); }
static void leave(void) { save(); free(s_buf); s_buf = nullptr; free(s_names); s_names = nullptr; }

static const char *nl_label(int i, void *) { return (i == 0 || !s_names) ? TR5("+ Nuova nota", "+ New note", "+ Nueva nota", "+ New note", "+ Neue notiz") : s_names[i - 1]; }
static unsigned short nl_color(int i, void *) { return i == 0 ? GRN : ACC; }

static const int LG_DOC_TOP = lg::HEAD + 6;              // the document on the panel: under the header, full height
static int view_vis(void)
{
    int h = nucleo_app_two_screens() ? 240 - LG_DOC_TOP - 4 : nucleo_app_content_height() - 22;
    int lh = body_size() * 8; return h / lh < 1 ? 1 : h / lh;
}
static int s_lg_scroll;                                   // the panel list's first visible row

// ---- Actions / Rename / Details ----
static void open_actions(void)
{
    snprintf(s_target, sizeof s_target, "%s", s_names[s_sel - 1]);
    s_mode = M_ACT; s_actsel = 0; nucleo_audio_tone(1500, 20, 45);
    nucleo_app_set_hint(TR5("su/giu scegli  INVIO ok  <-indietro", "up/dn pick  ENTER ok  <-back", "ar/ab elige  INTRO ok  <-atras", "haut/bas choix  ENTREE ok  <-re", "auf/ab waehle  ENTER ok  <-zu"));
    nucleo_app_request_draw();
}
static void input_commit(void)
{
    if (!s_input[0]) { nucleo_app_set_hint(TR5("Nome vuoto", "Empty name", "Nombre vacio", "Nom vide", "Name leer")); return; }
    char nm[72]; snprintf(nm, sizeof nm, "%s", s_input);
    if (!strchr(nm, '.')) { const char *dot = strrchr(s_target, '.'); snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "%s", dot ? dot : ".md"); }
    char oldp[200], newp[200];
    snprintf(oldp, sizeof oldp, "%s/%s", NOTES_DIR, s_target);
    snprintf(newp, sizeof newp, "%s/%s", NOTES_DIR, nm);
    if (access(newp, F_OK) == 0) { nucleo_app_set_hint(TR5("Esiste gia'", "Already exists", "Ya existe", "Existe deja", "Existiert bereits")); return; }
    nucleo_app_set_hint(rename(oldp, newp) == 0 ? TR5("Rinominata", "Renamed", "Renombrada", "Renommee", "Umbenannt") : TR5("Rinomina fallita", "Rename failed", "Error al renombr", "Echec renomm.", "Fehler beim Umb"));
    s_mode = M_LIST; scan(); nucleo_app_request_draw();
}
static void input_key(int key, char ch)
{
    if (key == NK_ENTER) { input_commit(); return; }
    if (key == NK_DEL)   { int l=(int)strlen(s_input); if (l) { s_input[l-1]=0; nucleo_app_request_draw(); } return; }
    if (ch >= ' ' && ch < 127 && ch != '/') {
        int l=(int)strlen(s_input); if (l < (int)sizeof(s_input)-1) { s_input[l]=ch; s_input[l+1]=0; nucleo_app_request_draw(); }
    }
}
static void act_perform(int idx)
{
    nucleo_audio_tone(1600, 20, 45);
    switch (idx) {
        case 0: load(s_target); break;                                                 // Open  -> M_VIEW
        case 1: s_mode = M_INPUT; snprintf(s_input, sizeof s_input, "%s", s_target);    // Rename
                nucleo_app_set_hint(TR5("digita  INVIO conferma  <-annulla", "type  ENTER confirm  <-cancel", "escribe  INTRO ok  <-cancela", "tapez  ENTREE confirme  <-annule", "gib ein  ENTER ok  <-abbr")); break;
        case 2: dup_note(s_target); s_mode = M_LIST; scan(); break;                     // Duplicate
        case 3: count_stats(s_target); s_mode = M_DETAILS;                              // Details
                nucleo_app_set_hint(TR5("un tasto per chiudere", "press a key to close", "una tecla para cerrar", "une touche pour fermer", "taste zum schliessen")); break;
        case 4: s_mode = M_LIST; s_confirm_del = true; s_del_yes = false; break;        // Delete -> confirm on s_sel
    }
    nucleo_app_request_draw();
}
static void actions_key(int key, char ch)
{
    if (nucleo_touch_is_row(key, ch)) { int i = ch - NUCLEO_TOUCH_ROW; if (i < ACT_N) act_perform(i); return; }   // the panel's action rows
    if (key == NK_UP)        s_actsel = (s_actsel + ACT_N - 1) % ACT_N;
    else if (key == NK_DOWN) s_actsel = (s_actsel + 1) % ACT_N;
    else if (key == NK_ENTER) { act_perform(s_actsel); return; }
    else return;
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_mode == M_INPUT)   { input_key(key, ch); return; }
    if (s_mode == M_DETAILS) { s_mode = M_LIST; list_hint(); nucleo_app_request_draw(); return; }   // any key closes
    if (s_mode == M_ACT)     { actions_key(key, ch); return; }

    if (s_mode == M_SET) {                                        // settings card
        if (key == NK_TAB) { s_mode = M_VIEW; cfg_save(); view_hint(); }
        else if (nucleo_touch_is_row(key, ch)) {                                       // a tapped option: pick it and step it
            s_setsel = (ch - NUCLEO_TOUCH_ROW) & 1;
            if (s_setsel == 0) s_fontsz = (s_fontsz + 1) % 3; else s_mdmode = (s_mdmode + 1) % 3;
        }
        else if (key == NK_UP || key == NK_DOWN) s_setsel = (s_setsel + 1) & 1;
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
                unlink(abs); d.fillRect(0, 0, 240, 121, C_RED); lgfx::v1::delay(40);
                scan(); if (s_sel > s_n) s_sel = s_n;
            }
            if (r >= 0) s_confirm_del = false;
        }
        else if (int t = lg::row_tap(key, ch, s_lg_scroll, items, &s_sel)) {           // a tapped row: pick, or open
            if (t == 2) { if (s_sel == 0) new_note(); else load(s_names[s_sel - 1]); }
        }
        else if (key == NK_RIGHT && s_sel > 0) { open_actions(); return; }
        else if (app_ui_list_key(key, ch, &s_sel, items, nl_label, nullptr)) { }
        else if (key == NK_ENTER) { if (s_sel == 0) new_note(); else load(s_names[s_sel - 1]); }
        else if ((ch == 'n' || ch == 'N') && s_sel == 0) new_note();
        else if (ch == 'd' && s_sel > 0) { s_confirm_del = true; s_del_yes = false; }
        else return;
    } else if (s_mode == M_VIEW) {
        int vis = view_vis(), maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
        if (key == NK_UP)        { if (s_vscroll > 0) s_vscroll--; }
        else if (key == NK_DOWN) { if (s_vscroll < maxs) s_vscroll++; }
        else if (key == NK_TAB)  { s_mode = M_SET; s_setsel = 0; nucleo_app_set_hint(TR5("SU/GIU voce  DX cambia  TAB chiudi", "UP/DN row  RIGHT change  TAB close", "AR/AB linea  DX cambia  TAB cierra", "HAUT/BAS ligne  DROIT chge  TAB ferme", "AUF/AB zeile  RECHTS aend  TAB close")); }
        else if (key == NK_ENTER) {
            if (s_trunc) nucleo_app_set_hint(TR5("File >4KB: sola lettura (usa l'app web)", "File >4KB: read-only (use the web app)", "Arch >4KB: soloLectura (usa app web)", "Fich >4KB: lecture seule (app web)", "Datei >4KB: nur Lese (web app)"));
            else { s_mode = M_EDIT; edit_hint(); }
        }
        else if (key == NK_DEL) { save(); s_mode = M_LIST; scan(); s_abs[0] = 0; list_hint(); }
        else return;
    } else {                                                     // M_EDIT
        if (!s_buf) return;
        if (key == NK_ENTER) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = '\n'; s_buf[s_len] = 0; s_dirty = true; } }
        else if (key == NK_DEL) { if (s_len > 0) { s_buf[--s_len] = 0; s_dirty = true; } }
        else if (ch >= 32 && ch < 127) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = ch; s_buf[s_len] = 0; s_dirty = true; } }
        else return;
    }
    nucleo_app_request_draw();
}

// LEFT + BACK: distinguish the two. BACK steps out one level; LEFT edits settings / is ignored where
// it would otherwise eat a typed comma. Fixes the old bugs where ',' (NK_LEFT) closed the app in EDIT
// and LEFT closed the app in the settings card.
static bool notes_on_back(int key)
{
    bool left = (key == NK_LEFT);
    if (s_mode == M_INPUT)   { if (left) return true; s_mode = M_LIST; list_hint(); nucleo_app_request_draw(); return true; }
    if (s_mode == M_DETAILS) { s_mode = M_LIST; list_hint(); nucleo_app_request_draw(); return true; }
    if (s_mode == M_ACT)     { s_mode = M_LIST; list_hint(); nucleo_app_request_draw(); return true; }
    if (s_mode == M_SET) {
        if (left) { if (s_setsel == 0) s_fontsz = (s_fontsz + 2) % 3; else s_mdmode = (s_mdmode + 2) % 3; nucleo_app_request_draw(); return true; }
        s_mode = M_VIEW; cfg_save(); view_hint(); nucleo_app_request_draw(); return true;
    }
    if (s_mode == M_VIEW || s_mode == M_EDIT) {
        if (left && s_mode == M_EDIT) return true;   // ignore comma-key in the editor (don't exit)
        save(); s_mode = M_LIST; scan(); s_abs[0] = 0; list_hint(); nucleo_app_request_draw(); return true;
    }
    // M_LIST
    if (s_confirm_del) { s_confirm_del = false; nucleo_app_request_draw(); return true; }
    if (left) return true;   // swallow stray LEFT at the list
    return false;            // BACK at the list root -> exit the app
}

// ---- draw ----
static void draw_list(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    char c[16]; snprintf(c, sizeof(c), TR5("%d note", "%d notes", "%d notas", "%d notes", "%d Notizen"), s_n);
    int y0 = app_ui_title("Notes", ACC, c);
    app_ui_list(y0, top + h - y0, s_n + 1, s_sel, nl_label, nullptr, nl_color, nullptr);
}

static void draw_text(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    bool editing = (s_mode == M_EDIT);
    d.fillRect(0, top, 240, h, BG);

    int vis = view_vis();
    if (!editing) {   // measure first so the header can show reading progress
        s_doc_lines = render_doc(0, true);
        int maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
        if (s_vscroll > maxs) s_vscroll = maxs;
        if (s_vscroll < 0) s_vscroll = 0;
    }

    char title[44]; snprintf(title, sizeof(title), "%s%s", s_file, s_dirty ? " *" : "");
    d.setTextSize(1); d.setTextColor(ACC, BG); d.setCursor(8, top + 3); d.print(title);
    char tag[16];
    if (editing) snprintf(tag, sizeof tag, "EDIT");
    else {
        int pct = s_doc_lines > 0 ? (int)((long)(s_vscroll + vis) * 100 / s_doc_lines) : 100; if (pct > 100) pct = 100;
        snprintf(tag, sizeof tag, "%s %d%%", file_is_md() ? "MD" : "TXT", pct);
    }
    d.setTextColor(editing ? GRN : MUTED, BG); d.setCursor(240 - (int)strlen(tag) * 6 - 6, top + 3); d.print(tag);
    d.drawFastHLine(0, top + 14, 240, LINE);

    if (editing) {
        const int LINEH = 16, WRAP = 19; d.setTextSize(2);
        // The editor appends at the end and shows the tail, so wrap the WHOLE buffer through a small ring
        // and keep only the last RING rows. (A fixed [40] array stopped laying out after 40 rows: text
        // typed into a longer note was invisible and the cursor vanished.)
        const int RING = 16;
        int evis = (h - 20) / LINEH; if (evis < 1) evis = 1; if (evis > RING) evis = RING;
        static char lines[RING][WRAP + 1]; int nlines = 0, col = 0;
        for (int i = 0; s_buf && i < s_len; i++) {
            char cc = s_buf[i]; char *cur = lines[nlines % RING];
            if (cc == '\n') { cur[col] = 0; nlines++; col = 0; continue; }
            cur[col++] = cc; if (col >= WRAP) { cur[col] = 0; nlines++; col = 0; }
        }
        lines[nlines % RING][col] = 0;
        int total = nlines + 1;
        int first = total > evis ? total - evis : 0, y = top + 20;
        for (int l = first; l < total; l++) {
            d.setTextColor(FG, BG); d.setCursor(6, y); d.print(lines[l % RING]);
            if (l == nlines) d.fillRect(6 + col * 12, y, 6, 14, GRN);
            y += LINEH;
        }
        if (s_len == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(6, top + 20); d.print(TR5("Scrivi...", "Type...", "Escribe...", "Tapez...", "Gib ein...")); }
        return;
    }

    render_doc(s_vscroll, false);
    if (s_len == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(6, top + 20); d.print(TR5("(vuota)", "(empty)", "(vacia)", "(vide)", "(leer)")); }

    if (s_doc_lines > vis) {
        int maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
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
    d.setTextSize(1); d.setTextColor(GRN, INK); d.setCursor(cx + 10, cy + 8); d.print(TR5("OPZIONI VISTA", "VIEW OPTIONS", "OPCIONES VISTA", "OPT.AFFICHAGE", "ANZEIGEOPT."));
    const char *szn[3] = { TR5("Piccolo","Small", "Pequeno", "Petit", "Klein"), TR5("Medio","Medium", "Medio", "Moyen", "Mittel"), TR5("Grande","Large", "Grande", "Grand", "Gross") };
    const char *mdn[3] = { "Auto", "Markdown", TR5("Testo","Plain", "Texto", "Texte", "Text") };
    const char *lbl[2] = { TR5("Dimensione font","Font size", "Tam.fuente", "Taille police", "Police"), TR5("Formato","Format", "Formato", "Format", "Format") };
    const char *val[2] = { szn[s_fontsz], mdn[s_mdmode] };
    for (int i = 0; i < 2; i++) {
        int y = cy + 26 + i * 20, sel = (i == s_setsel);
        if (sel) d.fillRoundRect(cx + 6, y - 2, cw - 12, 17, 4, ACC);
        d.setTextSize(1); d.setTextColor(sel ? INK : MUTED, sel ? ACC : INK); d.setCursor(cx + 12, y + 2); d.print(lbl[i]);
        d.setTextColor(sel ? INK : CODEC, sel ? ACC : INK);
        int vw = (int)strlen(val[i]) * 6 + 12; d.setCursor(cx + cw - vw - 8, y + 2);
        d.print("< "); d.print(val[i]);
    }
}

static void draw_actions(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    char t[24]; snprintf(t, sizeof t, "%.20s", s_target);
    int y0 = app_ui_title(t, ACC, nullptr);
    const char *items[ACT_N] = { TR5("Apri","Open", "Abre", "Ouvre", "Oeffne"), TR5("Rinomina","Rename", "Renombra", "Renomme", "Umbenennen"), TR5("Duplica","Duplicate", "Duplica", "Duplique", "Duplizieren"), TR5("Dettagli","Details", "Detalles", "Details", "Info"), TR5("Elimina","Delete", "Borra", "Supprime", "Loeschen") };
    const char icons[ACT_N] = { '>', 'R', 'C', 'i', 'x' };
    const int ROW = 17;
    for (int i = 0; i < ACT_N; i++) {
        int ry = y0 + 2 + i*ROW; bool sel = (i == s_actsel);
        unsigned short ac = (i == 4) ? C_RED : ACC;
        if (sel) d.fillRoundRect(6, ry, 228, ROW-2, (ROW-2)/2, ac);
        char ic[2] = { icons[i], 0 };
        d.setTextSize(1); d.setTextColor(sel ? INK : MUTED, sel ? ac : BG); d.setCursor(14, ry+5); d.print(ic);
        d.setTextSize(2); d.setTextColor(sel ? INK : (i==4 ? C_RED : FG), sel ? ac : BG); d.setCursor(30, ry+1); d.print(items[i]);
    }
}

static void draw_details(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y0 = app_ui_title(TR5("Dettagli","Details", "Detalles", "Details", "Info"), ACC, nullptr);
    int y = y0 + 4;
    char nm[24]; snprintf(nm, sizeof nm, "%.20s", s_target);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, y); d.print(nm); y += 24;

    d.setTextSize(1);
    char line[52];
    struct stat st; char abs[200]; snprintf(abs, sizeof abs, "%s/%s", NOTES_DIR, s_target);
    if (stat(abs, &st) == 0) {
        char sz[16];
        if (st.st_size < 1024) snprintf(sz, sizeof sz, "%ld B",  (long)st.st_size);
        else                   snprintf(sz, sizeof sz, "%ld KB", (long)((st.st_size + 1023) / 1024));
        snprintf(line, sizeof line, TR5("Dimensione: %s", "Size: %s", "Tamano: %s", "Taille: %s", "Groesse: %s"), sz);
        d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print(line); y += 14;
        time_t mt = st.st_mtime; struct tm *tm = localtime(&mt);
        if (tm) { char db[24]; snprintf(db, sizeof db, "%02d/%02d/%04d %02d:%02d", tm->tm_mday, tm->tm_mon+1, 1900+tm->tm_year, tm->tm_hour, tm->tm_min);
                  snprintf(line, sizeof line, TR5("Modificato: %s", "Modified: %s", "Modificado: %s", "Modifie: %s", "Geaendert: %s"), db);
                  d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print(line); y += 14; }
    }
    snprintf(line, sizeof line, TR5("Righe: %ld   Parole: %ld", "Lines: %ld   Words: %ld", "Lineas: %ld   Palabras:%ld", "Lignes: %ld   Mots: %ld", "Zeilen: %ld   Woerter: %ld"), s_stat_lines, s_stat_words);
    d.setTextColor(MUTED, BG); d.setCursor(8, y); d.print(line); y += 14;
    snprintf(line, sizeof line, TR5("Byte: %ld", "Bytes: %ld", "Bytes: %ld", "Octets: %ld", "Bytes: %ld"), s_stat_bytes);
    d.setTextColor(DIM, BG); d.setCursor(8, y); d.print(line);
}

static void draw_input(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top+8); d.print(TR5("Rinomina nota", "Rename note", "Renombra nota", "Renomme note", "Umbenne notiz"));
    int bx = 8, by = top+38, bw = 224, bh = 24;
    d.fillRoundRect(bx, by, bw, bh, 4, INK);
    d.drawRoundRect(bx, by, bw, bh, 4, s_input[0] ? ACC : LINE);
    int ql = (int)strlen(s_input);
    const char *show = s_input + (ql > 16 ? ql-16 : 0);
    char disp[20]; snprintf(disp, sizeof disp, "%s_", show);
    d.setTextSize(2); d.setTextColor(FG, INK); d.setCursor(bx+4, by+4); d.print(disp);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, by+bh+8);
    d.print(TR5("INVIO conferma   <- annulla", "ENTER confirm   <- cancel", "INTRO confirma   <- cancela", "ENTREE confirm   <- annule", "ENTER bestaetigt   <- abbr."));
}

// ===== two screens ==============================================================================================
// The panel carries the notes: the list on the left with the picked note's opening lines beside it, the document
// full width while reading or writing, the actions / options / details / rename as panels of their own. Touch is
// the content: tap a note to pick it, again (or its preview) to open it; drag the document to scroll it; the
// header's chevron steps back. The Cardputer, where the typing happens, becomes the companion: what is picked or
// open, and while writing the line under the cursor in large type.
static const int LG_LISTW = 150;
static char s_prev[200];                                  // the picked note's first bytes, for the preview card
static int  s_prev_of = -1;

static void lg_preview(int x, int y, int w, int h)
{
    lg::panel(x, y, w, h);
    const unsigned short cb = lg::card();
    if (s_sel == 0) {                                      // "+ New note": what a tap does
        lg::font(1); d.setTextColor(GRN, cb); d.drawString(TR("Nuova nota", "New note"), x + 10, y + 10);
        lg::font(0); d.setTextColor(MUTED, cb);
        lg::wrap(TR("Tocca qui o premi INVIO: si apre vuota e scrivi sulla tastiera.",
                    "Tap here or press ENTER: it opens empty and you type on the keyboard."), x + 10, y + 36, w - 20, 17, 6);
        nucleo_touch_add(x, y, w, h, NK_ENTER, 0);
        return;
    }
    if (s_prev_of != s_sel) {                              // read the opening bytes once per pick
        s_prev_of = s_sel; s_prev[0] = 0;
        char abs[200]; snprintf(abs, sizeof abs, "%s/%s", NOTES_DIR, s_names[s_sel - 1]);
        FILE *f = fopen(abs, "rb");
        if (f) { size_t n = fread(s_prev, 1, sizeof s_prev - 1, f); s_prev[n] = 0; fclose(f); }
        for (char *c = s_prev; *c; c++) if (*c == '#' || *c == '*' || *c == '`' || *c == '\r') *c = ' ';
    }
    lg::font(1); d.setTextColor(ACC, cb);
    char t[56]; lg::fit(t, sizeof t, s_names[s_sel - 1], w - 20); d.drawString(t, x + 10, y + 8);
    d.drawFastHLine(x + 10, y + 30, w - 20, LINE);
    lg::font(0); d.setTextColor(s_prev[0] ? FG : DIM, cb);
    const int bh = 30, lines = (h - 40 - bh - 14) / 17;
    lg::wrap(s_prev[0] ? s_prev : TR("(vuota)", "(empty)"), x + 10, y + 38, w - 20, 17, lines);
    const int bw = (w - 26) / 2, by = y + h - bh - 8;      // the two things to do with it
    lg::button(x + 8, by, bw, bh, TR("Apri", "Open"), ACC, INK, NK_ENTER, 0);
    lg::button(x + 18 + bw, by, bw, bh, TR("Azioni", "Actions"), LINE, FG, NK_RIGHT, '/');
    nucleo_touch_add(x, y, w, by - y, NK_ENTER, 0);        // the note itself opens too
}

static void lg_list_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    if (i == 0) { lg::row_text(x, y, w, h, focus, bg, TR("+ Nuova nota", "+ New note"), nullptr, GRN); return; }
    char nm[56]; snprintf(nm, sizeof nm, "%s", s_names[i - 1]);
    char *dot = strrchr(nm, '.'); if (dot && dot != nm) *dot = 0;           // the extension is noise in the list
    lg::row_text(x, y, w, h, focus, bg, nm, nullptr, FG);
}

static void lg_doc(void)
{
    const int W = d.width(), H = d.height();
    const bool editing = (s_mode == M_EDIT);
    char title[48]; snprintf(title, sizeof title, "%s%s", s_file, s_dirty ? " *" : "");
    s_doc_x = 12; s_doc_w = W - 30; s_doc_top = LG_DOC_TOP; s_doc_bot = H - 4;
    const int vis = view_vis();
    char tag[16];
    if (editing) snprintf(tag, sizeof tag, "%s", TR("SCRIVI", "EDIT"));
    else {
        s_doc_lines = render_doc(0, true);
        int maxs = s_doc_lines - vis; if (maxs < 0) maxs = 0;
        if (s_vscroll > maxs) s_vscroll = maxs;
        if (s_vscroll < 0) s_vscroll = 0;
        int pct = s_doc_lines > 0 ? (int)((long)(s_vscroll + vis) * 100 / s_doc_lines) : 100; if (pct > 100) pct = 100;
        snprintf(tag, sizeof tag, "%s %d%%", file_is_md() ? "MD" : "TXT", pct);
    }
    lg::header(title, tag, ACC, NK_BACK, editing ? 0 : NK_TAB);
    d.fillRect(0, lg::HEAD, W, H - lg::HEAD, BG);
    d.setFont(&fonts::Font0);                              // the document's own type: the bitmap face, sized 1..3
    if (editing) {
        const int sz = body_size() < 2 ? 2 : body_size(), LINEH = sz * 8 + 2, WRAP = (W - 24) / (6 * sz);
        const int RING = 24;
        int evis = (H - LG_DOC_TOP - 4) / LINEH; if (evis > RING) evis = RING;
        static int rows_at[RING]; int nlines = 0, col = 0;               // where each row starts in the buffer (ring)
        rows_at[0] = 0;
        for (int i = 0; s_buf && i < s_len; i++) {
            if (s_buf[i] == '\n' || ++col >= WRAP) { nlines++; col = 0; rows_at[nlines % RING] = i + 1; }
        }
        const int total = nlines + 1, first = total > evis ? total - evis : 0;
        int y = LG_DOC_TOP;
        for (int l = first; l < total; l++, y += LINEH) {
            const int a = rows_at[l % RING], b = l == nlines ? s_len : rows_at[(l + 1) % RING];
            char row[64]; int n = b - a; if (n > 0 && s_buf[a + n - 1] == '\n') n--; if (n > 63) n = 63; if (n < 0) n = 0;
            memcpy(row, s_buf + a, n); row[n] = 0;
            d.setTextSize(sz); d.setTextColor(FG, BG); d.setCursor(12, y); d.print(row);
            if (l == nlines) d.fillRect(12 + n * 6 * sz, y, 3, sz * 8, GRN);
        }
        if (s_len == 0) { lg::font(0); d.setTextColor(DIM, BG); d.drawString(TR("Scrivi sulla tastiera...", "Type on the keyboard..."), 24, LG_DOC_TOP); }
        d.setTextSize(1);
        return;
    }
    render_doc(s_vscroll, false);
    d.setTextSize(1);
    if (s_len == 0) { lg::font(0); d.setTextColor(DIM, BG); d.drawString(TR("(vuota)", "(empty)"), 12, LG_DOC_TOP); }
    if (s_doc_lines > vis) {
        const int maxs = s_doc_lines - vis, track = H - LG_DOC_TOP - 8;
        int kh = track * vis / s_doc_lines; if (kh < 14) kh = 14;
        d.fillRect(W - 6, LG_DOC_TOP, 2, track, LINE);
        d.fillRect(W - 6, LG_DOC_TOP + (track - kh) * s_vscroll / (maxs > 0 ? maxs : 1), 2, kh, ACC);
    }
    nucleo_touch_add_scroll(0, lg::HEAD, W, H - lg::HEAD, body_size() * 8, NK_UP, 0, NK_DOWN, 0);   // drag = scroll
    if (!s_trunc) nucleo_touch_add(W / 3, H - 60, W / 3, 60, NK_ENTER, 0);   // a tap low in the middle: write
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    if (s_mode == M_VIEW || s_mode == M_EDIT || s_mode == M_SET) {
        lg_doc();
        if (s_mode == M_SET) {                             // the view options, over the document's foot
            const int ch = 116, cy = H - ch - 8, cx = 20, cw = W - 40;
            lg::panel(cx, cy, cw, ch, INK); d.drawRoundRect(cx, cy, cw, ch, 8, ACC);
            lg::font(1); d.setTextColor(GRN, INK); d.drawString(TR("Opzioni vista", "View options"), cx + 12, cy + 8);
            const char *szn[3] = { TR("Piccolo", "Small"), TR("Medio", "Medium"), TR("Grande", "Large") };
            const char *mdn[3] = { "Auto", "Markdown", TR("Testo", "Plain") };
            const char *lbl[2] = { TR("Dimensione", "Font size"), TR("Formato", "Format") };
            const char *val[2] = { szn[s_fontsz], mdn[s_mdmode] };
            for (int i = 0; i < 2; i++) {
                const int ry = cy + 34 + i * 36; const bool on = i == s_setsel;
                const unsigned short f = on ? ACC : INK;
                d.fillRoundRect(cx + 8, ry, cw - 16, 32, 8, f);
                lg::row_text(cx + 8, ry, cw - 16, 32, on, f, lbl[i], val[i], MUTED);
                nucleo_touch_add(cx, ry, cw, 32, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
            }
        }
        nucleo_touch_commit();
        return;
    }
    if (s_mode == M_ACT || s_mode == M_DETAILS || s_mode == M_INPUT) {
        lg::header(s_target, nullptr, ACC, NK_BACK);
        const int y = lg::HEAD + 6;
        d.fillRect(0, lg::HEAD, W, H - lg::HEAD, BG);
        if (s_mode == M_ACT) {
            const char *items[ACT_N] = { TR("Apri", "Open"), TR("Rinomina", "Rename"), TR("Duplica", "Duplicate"), TR("Dettagli", "Details"), TR("Elimina", "Delete") };
            for (int i = 0; i < ACT_N; i++) {
                const int ry = y + i * 40; const bool on = i == s_actsel;
                const unsigned short ac = i == 4 ? C_RED : ACC, f = on ? ac : lg::card();
                d.fillRoundRect(12, ry, W - 24, 34, 9, f);
                lg::font(1); d.setTextDatum(middle_left); d.setTextColor(on ? INK : (i == 4 ? C_RED : FG), f);
                d.drawString(items[i], 28, ry + 18); d.setTextDatum(top_left);
                nucleo_touch_add(12, ry, W - 24, 34, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
            }
        } else if (s_mode == M_DETAILS) {
            lg::panel(12, y, W - 24, H - y - 10);
            const unsigned short cb = lg::card();
            char line[48]; int ly = y + 12;
            struct stat st; char abs[200]; snprintf(abs, sizeof abs, "%s/%s", NOTES_DIR, s_target);
            if (stat(abs, &st) == 0) {
                if (st.st_size < 1024) snprintf(line, sizeof line, "%ld B", (long)st.st_size);
                else snprintf(line, sizeof line, "%ld KB", (long)((st.st_size + 1023) / 1024));
                lg::kv(26, ly, W - 52, TR("Dimensione", "Size"), line, FG, cb); ly += 26;
                time_t mt = st.st_mtime; struct tm *tm = localtime(&mt);
                if (tm) { snprintf(line, sizeof line, "%02d/%02d/%04d %02d:%02d", tm->tm_mday, tm->tm_mon + 1, 1900 + tm->tm_year, tm->tm_hour, tm->tm_min);
                          lg::kv(26, ly, W - 52, TR("Modificata", "Modified"), line, FG, cb); ly += 26; }
            }
            snprintf(line, sizeof line, "%ld", s_stat_lines); lg::kv(26, ly, W - 52, TR("Righe", "Lines"), line, FG, cb); ly += 26;
            snprintf(line, sizeof line, "%ld", s_stat_words); lg::kv(26, ly, W - 52, TR("Parole", "Words"), line, FG, cb); ly += 26;
            snprintf(line, sizeof line, "%ld", s_stat_bytes); lg::kv(26, ly, W - 52, "Byte", line, MUTED, cb);
            nucleo_touch_add(0, lg::HEAD, W, H - lg::HEAD, NK_ENTER, 0);   // any tap closes, like any key
        } else {                                            // rename: the field here, the typing on the Cardputer
            lg::font(1); d.setTextColor(FG, BG); d.drawString(TR("Nuovo nome", "New name"), 16, y + 8);
            d.fillRoundRect(12, y + 34, W - 24, 40, 8, INK); d.drawRoundRect(12, y + 34, W - 24, 40, 8, s_input[0] ? ACC : LINE);
            char shown[72]; snprintf(shown, sizeof shown, "%s_", s_input);
            char f[72]; lg::font(2); lg::fit(f, sizeof f, shown, W - 48);
            d.setTextDatum(middle_left); d.setTextColor(FG, INK); d.drawString(f, 24, y + 55); d.setTextDatum(top_left);
            const int bw = (W - 36) / 2;
            lg::button(12, H - 54, bw, 40, TR("Annulla", "Cancel"), LINE, FG, NK_BACK, 0);
            lg::button(24 + bw, H - 54, bw, 40, TR("Rinomina", "Rename"), ACC, INK, NK_ENTER, 0);
        }
        nucleo_touch_commit();
        return;
    }
    // M_LIST
    char c[16]; snprintf(c, sizeof c, TR("%d note", "%d notes"), s_n);
    const int y = lg::header("Notes", c, ACC);
    lg::list(0, y + 2, LG_LISTW, H - y - 4, lg::ROW, s_n + 1, s_sel, &s_lg_scroll, lg_list_row, nullptr, ACC);
    d.fillRect(LG_LISTW, y, W - LG_LISTW, H - y, BG);
    lg_preview(LG_LISTW + 4, y + 6, W - LG_LISTW - 10, H - y - 12);
    if (s_confirm_del && s_sel > 0) {                       // delete? over the whole body, Yes / No to tap
        const int cy = 70, ch = 120;
        lg::panel(24, cy, W - 48, ch, INK); d.drawRoundRect(24, cy, W - 48, ch, 8, C_RED);
        lg::font(1); d.setTextColor(C_RED, INK); d.drawString(TR("Eliminare la nota?", "Delete note?"), 40, cy + 12);
        lg::font(0); d.setTextColor(FG, INK); char nm[56]; lg::fit(nm, sizeof nm, s_names[s_sel - 1], W - 96); d.drawString(nm, 40, cy + 38);
        const int bw = (W - 48 - 36) / 2;
        lg::button(36, cy + ch - 48, bw, 36, "No", s_del_yes ? LINE : ACC, s_del_yes ? FG : INK, NK_CHAR, 'n');
        lg::button(48 + bw, cy + ch - 48, bw, 36, TR("Elimina", "Delete"), s_del_yes ? C_RED : LINE, s_del_yes ? INK : FG, NK_CHAR, 'y');
    }
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the notes: the pick (or the open note), and while writing the cursor's line.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("NOTES", ACC);
    if (s_mode == M_EDIT) {                                 // the line being typed, large, with the cursor
        int a = s_len; while (a > 0 && s_buf[a - 1] != '\n') a--;
        const char *line = s_buf + a; const int n = s_len - a;
        const char *tail = n > 17 ? line + n - 17 : line;
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(ACC, BG);
        char t[48]; snprintf(t, sizeof t, "%.36s%s", s_file, s_dirty ? " *" : ""); d.drawString(t, 8, y + 2);
        d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(8, y + 34); d.print(tail);
        d.fillRect(8 + (int)strlen(tail) * 12, y + 32, 3, 18, GRN); d.setTextSize(1);
        char m[40]; snprintf(m, sizeof m, TR("%d caratteri", "%d characters"), s_len);
        d.setTextColor(MUTED, BG); d.setCursor(8, y + 62); d.print(m);
        lg::comp_keys(TR("INVIO a capo  CANC cancella  ESC salva", "ENTER newline  DEL erase  ESC save"));
        return;
    }
    if (s_mode == M_VIEW || s_mode == M_SET) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
        lg::wrap(s_file, 8, y + 4, 224, 20, 2);
        const int vis = view_vis();
        int pct = s_doc_lines > 0 ? (int)((long)(s_vscroll + vis) * 100 / s_doc_lines) : 100; if (pct > 100) pct = 100;
        lg::bar(8, y + 52, 224, 6, pct, ACC);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        char m[40]; snprintf(m, sizeof m, "%s  %d%%", file_is_md() ? "Markdown" : TR("Testo", "Text"), pct); d.drawString(m, 8, y + 64);
        lg::comp_keys(s_trunc ? TR(";/. scorri  sola lettura  ESC lista", ";/. scroll  read-only  ESC list")
                              : TR(";/. scorri  INVIO scrivi  TAB opz", ";/. scroll  ENTER write  TAB opts"));
        return;
    }
    const char *what = s_mode == M_LIST ? (s_sel == 0 ? TR("+ Nuova nota", "+ New note") : s_names[s_sel - 1]) : s_target;
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(s_mode == M_LIST && s_sel == 0 ? GRN : FG, BG);
    const int nl = lg::wrap(what, 8, y + 4, 224, 20, 3);
    d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
    char m[40]; snprintf(m, sizeof m, TR("%d di %d", "%d of %d"), s_sel, s_n);
    if (s_mode == M_LIST && s_sel > 0) d.drawString(m, 8, y + 8 + nl * 20);
    lg::comp_keys(s_mode == M_INPUT ? TR("scrivi il nome  INVIO ok  ESC annulla", "type the name  ENTER ok  ESC cancel")
                : s_mode == M_LIST ? TR("INVIO apri  -> azioni  n nuova  d elim", "ENTER open  -> actions  n new  d del")
                : TR("su/giu  INVIO ok  ESC indietro", "up/dn  ENTER ok  ESC back"));
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    s_doc_x = 8; s_doc_w = 224; s_doc_top = -1; s_doc_bot = -1;   // the compact layout (the panel's may be left set)
    if (s_mode == M_INPUT)   { draw_input();   return; }
    if (s_mode == M_DETAILS) { draw_details();  return; }
    if (s_mode == M_ACT)     { draw_actions();  return; }
    if (s_mode == M_LIST) draw_list();
    else draw_text();
    if (s_mode == M_LIST && s_confirm_del && s_sel > 0)
        app_ui_confirm(TR5("Eliminare la nota?", "Delete note?", "Borrar la nota?", "Supprimer note?", "Notiz loeschen?"), s_names[s_sel - 1], s_del_yes);
    if (s_mode == M_SET) draw_settings_card();
}

extern "C" void nucleo_register_notepad(void)
{
    static const nucleo_app_def_t app = {
        "notepad", "Notes", "Office", "Leggi e scrivi testo e Markdown",
        'n', C_BLUE, enter, on_key, tick, draw, leave, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
