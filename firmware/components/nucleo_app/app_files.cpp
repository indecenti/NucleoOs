// Files app: native SD browser.
// Tab key opens a 2-page quick-panel:
//   Page 0 – Recent: last 10 files + 5 most-visited dirs (history in /nucleo/files-hist.txt).
//   Page 1 – Search: type query, ENTER → recursive walk → results in main list.
// HistRec[15] (~1.2 KB) and Entry[96] (~6 KB) are malloc'd on enter(), freed on leave() — 0 RAM at rest.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "nucleo_board.h"
}
#include "nucleo_fsprotect.h"
#include "nucleo_fsfactory.h"
#include "nucleo_i18n.h"
#include "app_gfx.h"

static const unsigned short
    BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
    LINE = 0x2945, INK = 0x0000, DIRC = 0xFE8C, ACC = 0x4D1F,
    PNL  = 0x18C3;   // panel bg — slightly lighter than BG

// ── Listing ────────────────────────────────────────────────────────────────────
#define MAXE 96
struct Entry { char name[56]; bool dir; uint32_t kb; };
static Entry *s_e    = nullptr;   // malloc on enter
static int    s_n, s_sel;
static int    s_del_arm = -1;
static char   s_path[192] = "/";

// ── History ────────────────────────────────────────────────────────────────────
#define HIST_FILES 10
#define HIST_DIRS   5
#define HIST_TOTAL (HIST_FILES + HIST_DIRS)
struct HistRec { char path[72]; int visits; bool is_dir; };
static HistRec *s_hist   = nullptr;  // malloc on enter
static int      s_hist_n = 0;

// ── Tab-panel state ────────────────────────────────────────────────────────────
static bool s_tab_open    = false;
static int  s_tab_page    = 0;      // 0 = Recent, 1 = Search
static int  s_tab_sel     = 0;
static char s_query[28]   = "";
static bool s_search_mode = false;  // main list = search results; name = full web-path

// ── Sorting / scan ─────────────────────────────────────────────────────────────
static int cmp(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan(void)
{
    s_n = 0; s_sel = 0;
    if (!s_e) return;
    char abs[256]; snprintf(abs, sizeof abs, "%s%s", NUCLEO_SD_MOUNT, s_path);
    DIR *dir = opendir(abs); if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && s_n < MAXE) {
        if (e->d_name[0] == '.') continue;
        Entry *r = &s_e[s_n];
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        char c[320]; snprintf(c, sizeof c, "%s%s", abs, e->d_name);
        struct stat st; if (stat(c, &st) != 0) continue;
        r->dir = S_ISDIR(st.st_mode);
        r->kb  = r->dir ? 0 : (uint32_t)((st.st_size + 1023) / 1024);
        s_n++;
    }
    closedir(dir);
    qsort(s_e, s_n, sizeof(Entry), cmp);
}

static void go_up(void)
{
    if (!strcmp(s_path, "/")) return;
    int l = strlen(s_path); if (l && s_path[l-1]=='/') s_path[--l]=0;
    char *sl = strrchr(s_path, '/');
    if (sl) sl[1] = 0; else strcpy(s_path, "/");
    scan();
}

// ── History I/O ────────────────────────────────────────────────────────────────
static const char HIST_FILE[] = NUCLEO_SD_MOUNT "/nucleo/files-hist.txt";

static void hist_load(void)
{
    if (!s_hist) return;
    s_hist_n = 0;
    FILE *f = fopen(HIST_FILE, "r"); if (!f) return;
    char line[88];
    while (fgets(line, sizeof line, f) && s_hist_n < HIST_TOTAL) {
        int len = (int)strlen(line);
        while (len && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (len < 3 || line[1] != ':') continue;
        HistRec *r = &s_hist[s_hist_n];
        r->is_dir = (line[0] == 'D');
        r->visits = 1;
        char *p = line + 2;
        if (r->is_dir) {
            char *cnt = strrchr(p, ':');
            if (cnt && cnt != p) { r->visits = atoi(cnt+1); *cnt = 0; }
        }
        snprintf(r->path, sizeof r->path, "%s", p);
        s_hist_n++;
    }
    fclose(f);
}

static void hist_save(void)
{
    if (!s_hist) return;
    FILE *f = fopen(HIST_FILE, "w"); if (!f) return;
    for (int i = 0; i < s_hist_n; i++) {
        HistRec *r = &s_hist[i];
        if (r->is_dir) fprintf(f, "D:%s:%d\n", r->path, r->visits);
        else           fprintf(f, "F:%s\n",     r->path);
    }
    fclose(f);
}

static void hist_record_file(const char *web)
{
    if (!s_hist) return;
    for (int i = 0; i < s_hist_n; i++) {
        if (!s_hist[i].is_dir && !strcmp(s_hist[i].path, web)) {
            HistRec tmp = s_hist[i];
            memmove(&s_hist[1], &s_hist[0], i * sizeof(HistRec));
            s_hist[0] = tmp; hist_save(); return;
        }
    }
    int fc = 0;
    for (int i = 0; i < s_hist_n; i++) if (!s_hist[i].is_dir) fc++;
    if (fc >= HIST_FILES)
        for (int i = s_hist_n-1; i >= 0; i--)
            if (!s_hist[i].is_dir) { memmove(&s_hist[i],&s_hist[i+1],(s_hist_n-i-1)*sizeof(HistRec)); s_hist_n--; break; }
    if (s_hist_n < HIST_TOTAL) {
        memmove(&s_hist[1], &s_hist[0], s_hist_n * sizeof(HistRec));
        memset(&s_hist[0], 0, sizeof(HistRec));
        snprintf(s_hist[0].path, sizeof s_hist[0].path, "%s", web);
        s_hist[0].visits = 1; s_hist[0].is_dir = false;
        s_hist_n++; hist_save();
    }
}

static void hist_record_dir(const char *web)
{
    if (!s_hist || !strcmp(web, "/")) return;
    for (int i = 0; i < s_hist_n; i++) {
        if (s_hist[i].is_dir && !strcmp(s_hist[i].path, web)) {
            s_hist[i].visits++; hist_save(); return;
        }
    }
    if (s_hist_n < HIST_TOTAL) {
        memset(&s_hist[s_hist_n], 0, sizeof(HistRec));
        snprintf(s_hist[s_hist_n].path, sizeof s_hist[0].path, "%s", web);
        s_hist[s_hist_n].visits = 1; s_hist[s_hist_n].is_dir = true;
        s_hist_n++; hist_save();
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────────
static void update_hint(void);   // forward

static void enter(void)
{
    nucleo_app_set_direct_draw(true);
    if (!s_e)    s_e    = (Entry *)  malloc(sizeof(Entry)   * MAXE);
    if (!s_hist) s_hist = (HistRec *)malloc(sizeof(HistRec) * HIST_TOTAL);
    s_del_arm = -1; s_tab_open = false; s_search_mode = false; s_query[0] = 0;
    strcpy(s_path, "/"); scan();
    if (s_hist) hist_load();
    update_hint();
}
static void leave(void)
{
    if (s_e)    { free(s_e);    s_e    = nullptr; s_n = 0; }
    if (s_hist) { free(s_hist); s_hist = nullptr; s_hist_n = 0; }
}

// ── Virtual-row helpers (".." at index 0 when not at root) ────────────────────
static bool   at_root()        { return !strcmp(s_path, "/"); }
static int    total_n()        { return s_n + (at_root() ? 0 : 1); }
static bool   is_parent(int i) { return !at_root() && i == 0; }
static Entry *entry_at(int i)  { return &s_e[at_root() ? i : i-1]; }

// ── Type badges ────────────────────────────────────────────────────────────────
static char entry_ic(const Entry *e)
{
    if (e->dir) return 'D';
    const char *dot = strrchr(e->name, '.');  if (!dot) return '?';
    if (!strcasecmp(dot,".mp3")||!strcasecmp(dot,".wav")||!strcasecmp(dot,".ogg")) return 'A';
    if (!strcasecmp(dot,".jpg")||!strcasecmp(dot,".jpeg")||!strcasecmp(dot,".png")||
        !strcasecmp(dot,".bmp")||!strcasecmp(dot,".gif"))  return 'P';
    if (!strcasecmp(dot,".txt")||!strcasecmp(dot,".md") ||!strcasecmp(dot,".json")||
        !strcasecmp(dot,".log")||!strcasecmp(dot,".csv")||!strcasecmp(dot,".ini")) return 'T';
    if (!strcasecmp(dot,".nfv")||!strcasecmp(dot,".mp4")||!strcasecmp(dot,".avi")||
        !strcasecmp(dot,".mkv")||!strcasecmp(dot,".mov")) return 'V';
    return 'F';
}
static unsigned short entry_col(const Entry *e)
{
    if (e->dir) return DIRC;
    const char *dot = strrchr(e->name, '.');  if (!dot) return MUTED;
    if (!strcasecmp(dot,".mp3")||!strcasecmp(dot,".wav")||!strcasecmp(dot,".ogg")) return 0xFBB6;
    if (!strcasecmp(dot,".jpg")||!strcasecmp(dot,".jpeg")||!strcasecmp(dot,".png")||
        !strcasecmp(dot,".bmp")||!strcasecmp(dot,".gif")) return 0x8FF3;
    if (!strcasecmp(dot,".txt")||!strcasecmp(dot,".md") ||!strcasecmp(dot,".json")||
        !strcasecmp(dot,".log")||!strcasecmp(dot,".csv")||!strcasecmp(dot,".ini")) return 0x4D1F;
    return MUTED;
}
static const char *entry_right(const Entry *e)
{
    if (e->dir) return ">";
    static char b[12];
    uint32_t kb = e->kb;
    if      (kb < 1000)          snprintf(b, sizeof b, "%uK",   (unsigned)kb);
    else if (kb < 1024u*1000u)   snprintf(b, sizeof b, "%u.%uM",(unsigned)(kb/1024),(unsigned)((kb%1024)*10/1024));
    else                         snprintf(b, sizeof b, "%u.%uG",(unsigned)(kb/1048576u),(unsigned)((kb%1048576u)*10/1048576u));
    return b;
}

// ── Search ─────────────────────────────────────────────────────────────────────
static bool ci_contains(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && tolower((unsigned char)hay[i+j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nl) return true;
    }
    return false;
}

// abs_dir and web_dir must end in '/'.  Results go into s_e (name = full web-path).
static void search_walk(const char *abs_dir, const char *web_dir, int depth)
{
    if (s_n >= MAXE || depth > 3) return;
    DIR *dir = opendir(abs_dir); if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && s_n < MAXE) {
        if (de->d_name[0] == '.') continue;
        char abs_c[200]; snprintf(abs_c, sizeof abs_c, "%s%s", abs_dir, de->d_name);
        struct stat st; if (stat(abs_c, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char asub[200], wsub[128];
            snprintf(asub, sizeof asub, "%s%s/", abs_dir, de->d_name);
            snprintf(wsub, sizeof wsub, "%s%s/", web_dir, de->d_name);
            search_walk(asub, wsub, depth+1);
        } else if (ci_contains(de->d_name, s_query)) {
            Entry *r = &s_e[s_n];
            snprintf(r->name, sizeof r->name, "%s%s", web_dir, de->d_name);
            r->dir = false;
            r->kb  = (uint32_t)((st.st_size + 1023) / 1024);
            s_n++;
        }
    }
    closedir(dir);
}

static void do_search(void)
{
    s_n = 0; s_sel = 0;
    if (!s_e || !s_query[0]) return;
    char aroot[192]; snprintf(aroot, sizeof aroot, "%s/", NUCLEO_SD_MOUNT);
    search_walk(aroot, "/", 0);
    s_search_mode = true;
}

// For search results: label = basename, right = parent dir (abbreviated).
static const char *search_label(const char *full)
{
    const char *sl = strrchr(full, '/');
    return sl ? sl+1 : full;
}
static const char *search_parent(const char *full)
{
    static char b[24];
    const char *sl = strrchr(full, '/');
    if (!sl) { b[0]=0; return b; }
    int len = (int)(sl - full) + 1;
    if (len > 23) {
        // Abbreviate to ".../folder/"
        const char *prev = sl-1;
        while (prev > full && *prev != '/') prev--;
        snprintf(b, sizeof b, "...%.*s", (int)(sl-prev+1), prev);
    } else {
        snprintf(b, sizeof b, "%.*s", len, full);
    }
    return b;
}

// ── Hint ───────────────────────────────────────────────────────────────────────
static void update_hint(void)
{
    if (s_search_mode) {
        static char h[52];
        snprintf(h, sizeof h, TR("'%s'  DEL=reset filtro", "'%s'  DEL=clear filter"), s_query);
        nucleo_app_set_hint(h);
    } else if (!strcmp(s_path, "/")) {
        nucleo_app_set_hint(TR("INVIO apri  D=del  /=recenti/cerca  ESC esci",
                               "ENTER open  D=del  /=recent/search  ESC exit"));
    } else {
        nucleo_app_set_hint(TR("INVIO apri/su  D=del  DEL su  /=pannello",
                               "ENTER open/up  D=del  DEL up  /=panel"));
    }
}

// ── Virtual label for list_key quick-jump ─────────────────────────────────────
static const char *fl_label_virt(int i, void *)
{
    if (s_search_mode) return (s_e && i < s_n) ? search_label(s_e[i].name) : "";
    return is_parent(i) ? ".." : entry_at(i)->name;
}

// ── Extension helpers ──────────────────────────────────────────────────────────
static bool is_image_ext(const char *ext)
{
    return !strcasecmp(ext,".jpg")||!strcasecmp(ext,".jpeg")||!strcasecmp(ext,".png")||
           !strcasecmp(ext,".bmp")||!strcasecmp(ext,".gif");
}
static bool is_web_video_ext(const char *ext)
{
    return !strcasecmp(ext,".mp4")||!strcasecmp(ext,".avi")||!strcasecmp(ext,".mkv")||
           !strcasecmp(ext,".mov")||!strcasecmp(ext,".webm")||!strcasecmp(ext,".flv")||
           !strcasecmp(ext,".wmv")||!strcasecmp(ext,".m4v")||!strcasecmp(ext,".mpg")||!strcasecmp(ext,".mpeg");
}

// ── Tab-panel key handler (called first from on_key) ──────────────────────────
static bool tab_key(int key, char ch)
{
    if (!s_tab_open) return false;

    if (ch == '/') {   // / while panel open: cycle page 0→1, page 1→close
        if (s_tab_page == 0) { s_tab_page = 1; s_tab_sel = 0; }
        else                 { s_tab_open = false; update_hint(); }
        nucleo_app_request_draw(); return true;
    }
    if (key == NK_LEFT || key == NK_RIGHT) {
        s_tab_page ^= 1; s_tab_sel = 0;
        nucleo_app_request_draw(); return true;
    }

    // ── Page 0: Recent ──
    if (s_tab_page == 0) {
        int n = s_hist ? s_hist_n : 0;
        if (key == NK_UP   && n > 0) { s_tab_sel = (s_tab_sel+n-1)%n; nucleo_app_request_draw(); return true; }
        if (key == NK_DOWN && n > 0) { s_tab_sel = (s_tab_sel+1)%n;   nucleo_app_request_draw(); return true; }
        if (key == NK_ENTER && s_hist && s_tab_sel < n) {
            HistRec *r = &s_hist[s_tab_sel];
            if (r->is_dir) {
                snprintf(s_path, sizeof s_path, "%s", r->path);
                scan();
            } else {
                // Navigate to parent folder of the file
                char parent[80]; snprintf(parent, sizeof parent, "%s", r->path);
                char *sl = strrchr(parent, '/');
                if (sl && sl != parent) { sl[1]=0; snprintf(s_path, sizeof s_path, "%s", parent); }
                else strcpy(s_path, "/");
                scan();
            }
            s_tab_open = false; update_hint(); nucleo_app_request_draw(); return true;
        }
        return true;
    }

    // ── Page 1: Search ──
    if (key == NK_ENTER) {
        if (s_query[0]) { s_tab_open = false; do_search(); update_hint(); }
        else            { s_tab_open = false; update_hint(); }
        nucleo_app_request_draw(); return true;
    }
    if (key == NK_DEL) {   // backspace in query
        int l = (int)strlen(s_query); if (l) { s_query[l-1]=0; nucleo_app_request_draw(); }
        return true;
    }
    if (key == NK_CHAR && ch >= ' ' && ch < 127 && ch != '/') {
        int l = (int)strlen(s_query);
        if (l < (int)sizeof(s_query)-1) { s_query[l]=ch; s_query[l+1]=0; nucleo_app_request_draw(); }
        return true;
    }
    return true;   // swallow everything while panel is open
}

// ── Main key handler ───────────────────────────────────────────────────────────
static void on_key(int key, char ch)
{
    if (tab_key(key, ch)) return;

    if (ch == '/') {   // / opens panel (page 0); / again → page 1; / again → close
        s_tab_open = true; s_tab_page = 0; s_tab_sel = 0;
        nucleo_app_set_hint(TR("/=pg2/chiudi  SU/GIU sel  INVIO apri", "/=pg2/close  UP/DN sel  ENTER open"));
        nucleo_app_request_draw(); return;
    }

    // Reset search filter
    if (key == NK_DEL && s_search_mode) {
        s_search_mode = false; s_query[0] = 0;
        scan(); update_hint(); nucleo_app_request_draw(); return;
    }

    int tn = s_search_mode ? s_n : total_n();

    // Delete file (D key)
    if ((ch=='d'||ch=='D') && !s_search_mode && s_sel < tn && !is_parent(s_sel) && !entry_at(s_sel)->dir) {
        Entry *e = entry_at(s_sel);
        char abs[256]; snprintf(abs, sizeof abs, "%s%s%s", NUCLEO_SD_MOUNT, s_path, e->name);
        if (nucleo_fs_is_protected(abs)||nucleo_fs_is_factory(abs)) {
            s_del_arm = -1; nucleo_app_set_hint(TR("Protetto: file di sistema","Protected: system file"));
        } else if (s_del_arm != s_sel) {
            s_del_arm = s_sel; nucleo_app_set_hint(TR("Premi D per eliminare","Press D to delete"));
        } else {
            int keep = s_sel; unlink(abs); s_del_arm = -1; scan();
            tn = total_n();
            if (keep >= tn) keep = tn-1;
            if (keep < 0)   keep = 0;
            s_sel = keep; update_hint();
        }
        nucleo_app_request_draw(); return;
    }
    bool was_armed = (s_del_arm >= 0); s_del_arm = -1;

    if (app_ui_list_key(key, ch, &s_sel, tn, fl_label_virt, nullptr)) {
        if (was_armed) update_hint();
    }
    else if (key == NK_DEL && !s_search_mode) { go_up(); update_hint(); }
    else if (key == NK_ENTER && s_sel < tn) {
        if (!s_search_mode && is_parent(s_sel)) {
            go_up(); update_hint();
        } else {
            Entry *e = s_search_mode ? &s_e[s_sel] : entry_at(s_sel);
            char abs[256];
            if (s_search_mode)
                snprintf(abs, sizeof abs, "%s%s", NUCLEO_SD_MOUNT, e->name);
            else
                snprintf(abs, sizeof abs, "%s%s%s", NUCLEO_SD_MOUNT, s_path, e->name);

            if (!s_search_mode) {
                if (e->dir) {
                    char web[192]; snprintf(web, sizeof web, "%s%s/", s_path, e->name);
                    hist_record_dir(web);
                    int l = (int)strlen(s_path);
                    if (l+(int)strlen(e->name)+2 < (int)sizeof(s_path)) {
                        snprintf(s_path+l, sizeof(s_path)-l, "%s/", e->name);
                        scan(); update_hint();
                    } else nucleo_app_set_hint(TR("Percorso troppo lungo","Path too long"));
                    nucleo_app_request_draw(); return;
                } else {
                    char web[192]; snprintf(web, sizeof web, "%s%s", s_path, e->name);
                    hist_record_file(web);
                }
            }
            // Open file
            const char *dot = strrchr(s_search_mode ? e->name : e->name, '.');
            if (!s_search_mode) dot = strrchr(e->name, '.');
            else                dot = strrchr(abs, '.');
            if (dot && is_image_ext(dot)) {
                if (nucleo_app_image_oversize(abs)) nucleo_app_set_hint(TR("Troppo grande: usa l'app web","Too large: use the web app"));
                else nucleo_app_launch_file(abs);
            } else if (dot && is_web_video_ext(dot)) {
                nucleo_app_set_hint(TR("Video non .nfv: usa l'app web","Non-.nfv video: use the web app"));
            } else if (!nucleo_app_launch_file(abs)) {
                nucleo_app_set_hint(TR("nessuna app per questo tipo","no app for this type"));
            }
        }
    }
    else { if (was_armed) { update_hint(); nucleo_app_request_draw(); } return; }
    nucleo_app_request_draw();
}

static void tick(void) { if (app_ui_list_animating()) nucleo_app_request_draw(); }

// ── Draw helpers ───────────────────────────────────────────────────────────────
static void draw_list(int y0, int list_h)
{
    const int STEP = 20;
    int tn  = s_search_mode ? s_n : total_n();
    d.fillRect(0, y0, 240, list_h, BG);
    if (tn == 0) {
        d.setTextColor(DIM, BG); d.setCursor(14, y0+8);
        d.print(s_search_mode ? TR("Nessun risultato","No results") : TR("Cartella vuota","Empty folder"));
        return;
    }
    int vis    = list_h / STEP;
    int scroll = s_sel - 1;
    if (scroll > tn - vis) scroll = tn - vis;
    if (scroll < 0) scroll = 0;

    d.setClipRect(0, y0, 240, list_h);
    unsigned short sel_col = MUTED;

    for (int i = scroll; i < tn; i++) {
        int y = y0 + (i-scroll)*STEP + STEP/2;
        if (y > y0+list_h) break;
        bool focus = (i == s_sel);
        bool par   = !s_search_mode && is_parent(i);
        const Entry *e = (par || s_search_mode) ? (s_search_mode && i < s_n ? &s_e[i] : nullptr) : entry_at(i);
        if (!e && !par) continue;

        unsigned short col = par ? MUTED : entry_col(e);
        char ic_ch = par ? '^' : entry_ic(e);
        char ic[2] = {ic_ch, 0};

        const char *lab, *rt;
        if (par) { lab = ".."; rt = nullptr; }
        else if (s_search_mode) { lab = search_label(e->name); rt = search_parent(e->name); }
        else { lab = e->name; rt = entry_right(e); }

        if (focus) {
            sel_col = col;
            const int ph = STEP - 2;
            d.fillRoundRect(6, y-ph/2, 234, ph, ph/2, col);
            d.setTextSize(1); d.setTextColor(INK, col);
            d.setCursor(10, y-3); d.print(ic);
            int rx = 228;
            if (rt && rt[0]) {
                int w = (int)strlen(rt)*6;
                d.setCursor(rx-w, y-3); d.print(rt); rx -= w+6;
            }
            int maxc = (rx-22)/12; if (maxc<1) maxc=1; if (maxc>25) maxc=25;
            char lb[26]; snprintf(lb, sizeof lb, "%.*s", maxc, lab);
            d.setTextSize(2); d.setTextColor(INK, col);
            d.setCursor(22, y-7); d.print(lb);
        } else {
            bool near = (abs(i-s_sel)==1);
            unsigned short bb = near ? col : DIM;
            d.fillRect(3, y-4, 9, 9, bb);
            d.setTextSize(1); d.setTextColor(INK, bb);
            d.setCursor(4, y-3); d.print(ic);
            int rx = 233;
            if (rt && rt[0]) {
                int w = (int)strlen(rt)*6;
                d.setTextSize(1); d.setTextColor(near ? MUTED : DIM, BG);
                d.setCursor(232-w, y-3); d.print(rt);
                rx = 232-w-4;
            }
            int tsz = near?2:1, chw=tsz*6, ty=near?y-7:y-3;
            int maxc = (rx-16)/chw; if (maxc<1) maxc=1; if (maxc>34) maxc=34;
            char b[36]; snprintf(b, sizeof b, "%.*s", maxc, lab);
            d.setTextSize(tsz); d.setTextColor(near?FG:DIM, BG);
            d.setCursor(16, ty); d.print(b);
        }
    }
    if (tn > vis && vis > 0) {
        int track = list_h-8, kh = track*vis/tn; if (kh<10) kh=10;
        int ky = y0+4+(track-kh)*s_sel/(tn>1?tn-1:1);
        d.fillRoundRect(236, y0+4, 3, track, 1, LINE);
        d.fillRoundRect(236, ky,   3, kh,    1, sel_col);
    }
    d.clearClipRect();
}

static void draw_panel(int y0, int list_h)
{
    d.fillRect(0, y0, 240, list_h, PNL);

    // Tab buttons
    const int BTN_H = 14;
    bool p0 = (s_tab_page == 0);
    d.fillRoundRect(4,   y0+1, 114, BTN_H, 3, p0 ? DIRC : DIM);
    d.fillRoundRect(122, y0+1, 114, BTN_H, 3, p0 ? DIM : DIRC);
    d.setTextSize(1);
    d.setTextColor(p0 ? INK : MUTED, p0 ? DIRC : DIM);
    d.setCursor(20, y0+4); d.print(TR("RECENTI", "RECENT"));
    d.setTextColor(p0 ? MUTED : INK, p0 ? DIM : DIRC);
    d.setCursor(148, y0+4); d.print(TR("CERCA", "SEARCH"));

    int cy = y0 + BTN_H + 2;
    int content_h = list_h - BTN_H - 2;

    if (s_tab_page == 0) {
        // Recent list
        if (!s_hist || s_hist_n == 0) {
            d.setTextColor(DIM, PNL); d.setTextSize(1);
            d.setCursor(10, cy+10); d.print(TR("Nessuna cronologia", "No history yet"));
        } else {
            const int ROW = 14;
            int max_show = content_h / ROW; if (max_show > s_hist_n) max_show = s_hist_n;
            for (int i = 0; i < max_show; i++) {
                int y = cy + i*ROW + ROW/2;
                bool focus = (i == s_tab_sel);
                HistRec *r = &s_hist[i];
                unsigned short rc = r->is_dir ? DIRC : ACC;
                if (focus) {
                    d.fillRoundRect(4, y-ROW/2+1, 232, ROW-2, 2, rc);
                    d.setTextColor(INK, rc);
                }
                // type badge
                char tic[2] = {r->is_dir ? 'D' : 'F', 0};
                unsigned short tb = focus ? INK : (r->is_dir ? DIRC : MUTED);
                if (!focus) { d.fillRect(4, y-4, 8, 8, tb); d.setTextColor(INK, tb); }
                d.setTextSize(1); d.setCursor(5, y-3); d.print(tic);

                // label: basename for files, path for dirs
                const char *lab2 = r->path;
                if (!r->is_dir) { const char *sl=strrchr(r->path,'/'); if (sl) lab2=sl+1; }
                if (!focus) d.setTextColor(r->is_dir ? DIRC : FG, PNL);
                int maxc2 = (r->is_dir ? 26 : 30);
                char lb2[32]; snprintf(lb2, sizeof lb2, "%.*s", maxc2, lab2);
                d.setTextSize(1); d.setCursor(16, y-3); d.print(lb2);

                // visit count for dirs
                if (r->is_dir && r->visits > 0) {
                    char vc[6]; snprintf(vc, sizeof vc, "%d", r->visits);
                    if (!focus) d.setTextColor(DIM, PNL);
                    d.setCursor(238-(int)strlen(vc)*6, y-3); d.print(vc);
                }
            }
        }
    } else {
        // Search page
        d.setTextColor(MUTED, PNL); d.setTextSize(1);
        d.setCursor(8, cy+3); d.print(TR("Cerca ovunque:", "Search everywhere:"));

        // Input box
        int bx = 8, by = cy+14, bw = 224, bh = 20;
        d.fillRoundRect(bx, by, bw, bh, 3, 0x2104);
        d.drawRoundRect(bx, by, bw, bh, 3, s_query[0] ? DIRC : LINE);
        // Show last 17 chars of query + blinking cursor
        int ql = (int)strlen(s_query);
        const char *qshow = s_query + (ql > 17 ? ql-17 : 0);
        char qdisp[20]; snprintf(qdisp, sizeof qdisp, "%s_", qshow);
        d.setTextSize(2); d.setTextColor(FG, 0x2104);
        d.setCursor(bx+4, by+2); d.print(qdisp);

        d.setTextSize(1); d.setTextColor(DIM, PNL);
        d.setCursor(8, by+bh+4);
        d.print(TR("INVIO=cerca  CANC=back  /=chiudi",
                   "ENTER=search  DEL=back  /=close"));
    }
}

// ── Draw ───────────────────────────────────────────────────────────────────────
static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();

    char title[20];
    if (s_search_mode) snprintf(title, sizeof title, TR("Risultati", "Results"));
    else if (!strcmp(s_path, "/")) snprintf(title, sizeof title, "Files");
    else {
        char tmp[192]; snprintf(tmp, sizeof tmp, "%s", s_path);
        int l=(int)strlen(tmp); if (l && tmp[l-1]=='/') tmp[l-1]=0;
        const char *bn=strrchr(tmp,'/'); bn=bn?bn+1:tmp;
        snprintf(title, sizeof title, "%.18s", bn);
    }
    char cnt[12];
    if (s_search_mode)   snprintf(cnt, sizeof cnt, "~%s", s_query[0]?s_query:"?");
    else if (s_n>=MAXE)  snprintf(cnt, sizeof cnt, "%d+", MAXE);
    else                 snprintf(cnt, sizeof cnt, "%d",  s_n);
    int y0 = app_ui_title(title, s_search_mode ? ACC : DIRC, cnt);
    int list_h = top + h - y0;

    if (s_tab_open) draw_panel(y0, list_h);
    else            draw_list(y0, list_h);
}

extern "C" void nucleo_register_files(void)
{
    static const nucleo_app_def_t app = {
        "files", "Files", "Tools", "Browse and open SD card files",
        'f', 0xFE8C, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
