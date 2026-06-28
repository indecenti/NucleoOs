// Notes app: read and edit small text files on the SD card (/data/Notes).
// Smartwatch feel: the note list uses the shared enlarged-focus widget; opening a note
// shows it in a big, readable VIEW (text size 2, scroll with the arrows), and Enter
// switches to EDIT (append at the end, view auto-follows the cursor). Esc saves & exits.
// Two modes are needed because the matrix keyboard reuses ; . / for both navigation and
// typing — VIEW gives arrow-scroll, EDIT types every printable key (incl. '.', '/', ';').
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "nucleo_board.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, INK = 0x0000, ACC = 0x4D1F, GRN = 0x8FF3, LINE = 0x2945;
#define NOTES_DIR NUCLEO_SD_MOUNT "/data/Notes"
#define WRAP 19          // characters per line at text size 2 (240px / 12px)
#define MAXBUF 2048

enum { M_LIST, M_VIEW, M_EDIT };
static char (*s_names)[56] = nullptr;   // [48][56], heap-allocated on enter() (zero RAM when closed)
static int s_n, s_sel, s_mode, s_vscroll;
static bool s_dirty;
static char *s_buf = nullptr;           // [MAXBUF], heap-allocated on enter()
static int s_len;
static char s_file[64];
static char s_abs[256];   // full path when opened from Files ("open with"); empty = a note under NOTES_DIR

static bool is_text(const char *n)
{
    const char *dot = strrchr(n, '.'); if (!dot) return false;
    return !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".json");
}
static int cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

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
    s_abs[0] = 0;                                 // a note in NOTES_DIR (not an external "open with" file)
    snprintf(s_file, sizeof(s_file), "%s", name);
    char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, name);
    FILE *f = fopen(abs, "rb");
    s_len = 0;
    if (f) { if (s_buf) s_len = (int)fread(s_buf, 1, MAXBUF - 1, f); fclose(f); }
    if (s_buf) s_buf[s_len] = 0;
    s_dirty = false; s_mode = M_VIEW; s_vscroll = 0;
    nucleo_app_set_hint(";/. scroll  enter edit");
}

// Open an arbitrary file chosen in Files ("open with"): load by ABSOLUTE path and remember it so
// edits save back to the SAME file (not NOTES_DIR). The basename becomes the title.
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
    nucleo_app_set_hint(";/. scroll  enter edit");
}

static void save(void)
{
    if (!s_dirty || !s_file[0] || !s_buf) return;
    char abs[256];
    if (s_abs[0]) snprintf(abs, sizeof(abs), "%s", s_abs);   // a file opened from Files: write it back in place
    else { mkdir(NOTES_DIR, 0775); snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, s_file); }
    FILE *f = fopen(abs, "wb");
    if (f) { fwrite(s_buf, 1, s_len, f); fclose(f); s_dirty = false; }
}

static void new_note(void)
{
    int max = 0;
    if (s_names) for (int i = 0; i < s_n; i++) { int v; if (sscanf(s_names[i], "note-%d", &v) == 1 && v > max) max = v; }
    snprintf(s_file, sizeof(s_file), "note-%d.txt", max + 1);
    s_len = 0; if (s_buf) s_buf[0] = 0; s_dirty = true; s_mode = M_EDIT; s_vscroll = 0;
    nucleo_app_set_hint("enter newline  del bksp");
}

// Count wrapped display lines (mirrors the renderer's wrap rule). Capped at 40 to match the
// renderer's line buffer, so scrolling can't run past the last drawable row.
static int wrapped_lines(void)
{
    int n = 1, col = 0;
    if (s_buf) for (int i = 0; i < s_len; i++) { if (s_buf[i] == '\n') { n++; col = 0; } else if (++col >= WRAP) { n++; col = 0; } }
    return n > 40 ? 40 : n;
}

static void enter(void)
{
    nucleo_app_set_direct_draw(true);            // static UI: draw direct, free the 32 KB menu buffer
    if (!s_names) s_names = (char (*)[56])calloc(48, sizeof *s_names);   // ~2688 B, only while open
    if (!s_buf)   s_buf   = (char *)calloc(MAXBUF, 1);                  // ~2048 B, only while open
    s_dirty = false; s_file[0] = 0; s_abs[0] = 0;
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) { load_abs(of); return; }   // opened from Files ("open with") -> show THAT file
    s_mode = M_LIST; scan(); nucleo_app_set_hint("enter open  d delete");
}
static void tick(void) { if (s_mode == M_LIST && app_ui_list_animating()) nucleo_app_request_draw(); }  // only while the list animates
static void leave(void) { save(); free(s_buf); s_buf = nullptr; free(s_names); s_names = nullptr; }

static const char *nl_label(int i, void *) { return (i == 0 || !s_names) ? "+ New note" : s_names[i - 1]; }

static void on_key(int key, char ch)
{
    if (s_mode == M_LIST) {
        int items = s_n + 1;
        if (app_ui_list_key(key, ch, &s_sel, items, nl_label, nullptr)) {
            // handled natively
        }
        else if (key == NK_ENTER) { if (s_sel == 0) new_note(); else load(s_names[s_sel - 1]); }
        else if (ch == 'd' && s_sel > 0) {
            char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", NOTES_DIR, s_names[s_sel - 1]);
            unlink(abs);
            d.fillRect(0, 0, 240, 121, 0xA000); lgfx::v1::delay(40);
            scan();
            if (s_sel > s_n) s_sel = s_n;
        }
        else return;
    } else if (s_mode == M_VIEW) {
        int vis = (nucleo_app_content_height() - 18) / 16; if (vis < 1) vis = 1;
        int maxs = wrapped_lines() - 1; if (maxs < 0) maxs = 0;
        if (key == NK_UP)        { if (s_vscroll > 0) s_vscroll--; }
        else if (key == NK_DOWN) { if (s_vscroll < maxs) s_vscroll++; }
        else if (key == NK_ENTER) { s_mode = M_EDIT; nucleo_app_set_hint("enter newline  del bksp"); }
        else if (key == NK_DEL) { save(); s_mode = M_LIST; scan(); s_abs[0] = 0; nucleo_app_set_hint("enter open  d delete"); }
        else return;
    } else {                                              // M_EDIT: every printable key types
        if (!s_buf) return;
        if (key == NK_ENTER) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = '\n'; s_buf[s_len] = 0; s_dirty = true; } }
        else if (key == NK_DEL) { if (s_len > 0) { s_buf[--s_len] = 0; s_dirty = true; } }
        else if (key == NK_BACK) { save(); s_mode = M_LIST; scan(); s_abs[0] = 0; nucleo_app_set_hint("enter open  d delete"); return; } // prevent app closing and intercept it instead!
        else if (ch >= 32 && ch < 127) { if (s_len < MAXBUF - 1) { s_buf[s_len++] = ch; s_buf[s_len] = 0; s_dirty = true; } }
        else return;
    }
    nucleo_app_request_draw();
}

// app_ui_list providers for the note list (index 0 is the "New note" action).
static unsigned short nl_color(int i, void *) { return i == 0 ? GRN : ACC; }

static void draw_list(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    char c[16]; snprintf(c, sizeof(c), "%d note%s", s_n, s_n == 1 ? "" : "s");
    int y0 = app_ui_title("Notes", ACC, c);
    app_ui_list(y0, top + h - y0, s_n + 1, s_sel, nl_label, nullptr, nl_color, nullptr);
}

// Big, readable text (size 2). VIEW uses the scroll offset; EDIT auto-follows the cursor.
static void draw_text(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    bool editing = (s_mode == M_EDIT);
    d.fillRect(0, top, 240, h, BG);
    char title[44]; snprintf(title, sizeof(title), "%s%s", s_file, s_dirty ? " *" : "");
    d.setTextSize(1); d.setTextColor(ACC, BG); d.setCursor(8, top + 3); d.print(title);
    d.setTextColor(editing ? GRN : MUTED, BG); d.setCursor(240 - 30, top + 3); d.print(editing ? "EDIT" : "VIEW");
    d.drawFastHLine(0, top + 14, 240, LINE);

    const int LINEH = 16, text_top = top + 20; int vis = (h - 18) / LINEH; if (vis < 1) vis = 1;

    // Fill at most MAXLINES wrapped rows. Past that we stop writing (no '% MAXLINES' wrap,
    // which used to overwrite earlier rows and corrupt the view) and flag truncation below.
    const int MAXLINES = 40;
    static char lines[MAXLINES][WRAP + 1];
    int nlines = 0, col = 0;
    bool truncated = false;
    for (int i = 0; s_buf && i < s_len; i++) {
        char c = s_buf[i];
        if (nlines >= MAXLINES) { truncated = true; break; }   // buffer full: stop, don't wrap
        if (c == '\n') { lines[nlines][col] = 0; nlines++; col = 0; continue; }
        lines[nlines][col++] = c;
        if (col >= WRAP) { lines[nlines][col] = 0; nlines++; col = 0; }
    }
    if (nlines < MAXLINES) lines[nlines][col] = 0;             // terminate the in-progress row
    int total = nlines + (truncated ? 0 : 1);
    if (total > MAXLINES) total = MAXLINES;
    int first = editing ? (total > vis ? total - vis : 0)
                        : (s_vscroll > total - vis ? (total > vis ? total - vis : 0) : s_vscroll);

    d.setTextSize(2);
    int y = text_top;
    for (int l = first; l < total && l < first + vis; l++) {
        d.setTextColor(FG, BG); d.setCursor(6, y); d.print(lines[l]);
        if (editing && !truncated && l == nlines) d.fillRect(6 + col * 12, y, 6, 14, GRN);   // cursor
        y += LINEH;
    }
    if (s_len == 0) { d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(6, text_top); d.print(editing ? "Type to write..." : "(empty)"); }
    if (truncated) {   // file longer than the view buffer: tell the user we stopped, no silent corruption
        d.setTextSize(1); d.setTextColor(0xFD20, BG);
        d.setCursor(6, top + h - 10); d.print("(file truncated in view)");
    }

    if (!editing && total > vis) {        // scroll indicator
        int track = h - 22, kh = track * vis / total; if (kh < 8) kh = 8;
        int ky = text_top + (track - kh) * first / (total - vis > 0 ? total - vis : 1);
        d.fillRect(237, text_top, 2, track, LINE);
        d.fillRect(237, ky, 2, kh, ACC);
    }
}

static void draw(void) { if (s_mode == M_LIST) draw_list(); else draw_text(); }

extern "C" void nucleo_register_notepad(void)
{
    static const nucleo_app_def_t app = {
        "notepad", "Notes", "Tools", "Read and edit text files",
        'n', 0x4D1F, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
