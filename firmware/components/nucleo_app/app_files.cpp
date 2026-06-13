// Files app: a native SD-card browser for the Cardputer. Folder-first list with
// per-type glyphs and sizes, a breadcrumb, smooth keyboard navigation (up/down,
// Enter to open a folder, Backspace to go up, Esc to leave). Reads the card directly
// over FATFS — no network, works fully offline.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "nucleo_board.h"
}
#include "nucleo_fsprotect.h"   // same system-file policy the web fs API enforces

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            LINE = 0x2945, INK = 0x0000, DIRC = 0xFE8C, ACC = 0x4D1F;

#define MAXE 96
struct Entry { char name[56]; bool dir; uint32_t kb; };
// The directory listing (96*~64 = ~6 KB) is malloc'd while the Files app is OPEN and freed on leave(),
// so it doesn't sit in .bss when the app is closed — that RAM goes to ANIMA. Guarded: if the alloc
// fails, s_n stays 0 (empty listing) and every access is bounded by s_n / (s_sel < s_n) — never a crash.
static Entry *s_e = nullptr;
static int s_n, s_sel;
static char s_path[192] = "/";          // web path under the SD mount, always ends in '/'

static int cmp(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;       // folders first
    return strcasecmp(x->name, y->name);
}

static void scan(void)
{
    s_n = 0; s_sel = 0;
    if (!s_e) return;                                   // no buffer (alloc failed / app not entered) -> empty listing
    char abs[256]; snprintf(abs, sizeof(abs), "%s%s", NUCLEO_SD_MOUNT, s_path);
    DIR *dir = opendir(abs);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && s_n < MAXE) {
        if (e->d_name[0] == '.') continue;
        Entry *r = &s_e[s_n];
        snprintf(r->name, sizeof(r->name), "%s", e->d_name);
        char c[320]; snprintf(c, sizeof(c), "%s%s", abs, e->d_name);
        struct stat st;
        if (stat(c, &st) != 0) continue;
        r->dir = S_ISDIR(st.st_mode);
        r->kb = r->dir ? 0 : (uint32_t)((st.st_size + 1023) / 1024);
        s_n++;
    }
    closedir(dir);
    qsort(s_e, s_n, sizeof(Entry), cmp);
}

static void go_up(void)
{
    if (!strcmp(s_path, "/")) return;
    int l = strlen(s_path); if (l && s_path[l - 1] == '/') s_path[--l] = 0;   // drop trailing '/'
    char *slash = strrchr(s_path, '/');
    if (slash) slash[1] = 0; else strcpy(s_path, "/");
    scan();
}

static void enter(void)
{
    if (!s_e) s_e = (Entry *)malloc(sizeof(Entry) * MAXE);   // ~6 KB, only while the app is open
    strcpy(s_path, "/"); scan();
    nucleo_app_set_hint("enter open  del up  d delete");
}
static void leave(void) { if (s_e) { free(s_e); s_e = nullptr; s_n = 0; } }   // return the ~6 KB to the heap for ANIMA

// app_ui_list row providers (smartwatch focused list).
static const char *fl_label(int i, void *) { return s_e[i].name; }
static const char *fl_right(int i, void *)
{
    static char b[12];
    if (s_e[i].dir) snprintf(b, sizeof(b), ">"); else snprintf(b, sizeof(b), "%uK", (unsigned)s_e[i].kb);
    return b;
}
static unsigned short fl_color(int i, void *) { return s_e[i].dir ? DIRC : 0x4D1F; }

static void on_key(int key, char ch)
{
    if (app_ui_list_key(key, ch, &s_sel, s_n, fl_label, nullptr)) {
        // handled natively
    }
    else if (key == NK_DEL) { go_up(); }
    else if (key == NK_ENTER && s_sel < s_n) {
        if (s_e[s_sel].dir) {
            int l = strlen(s_path);
            snprintf(s_path + l, sizeof(s_path) - l, "%s/", s_e[s_sel].name);
            scan();
        } else {
            char abs[256]; snprintf(abs, sizeof(abs), "%s%s%s", NUCLEO_SD_MOUNT, s_path, s_e[s_sel].name);
            nucleo_app_launch_file(abs);
        }
    }
    else if (ch == 'd' && s_sel < s_n && !s_e[s_sel].dir) {
        char abs[256]; snprintf(abs, sizeof(abs), "%s%s%s", NUCLEO_SD_MOUNT, s_path, s_e[s_sel].name);
        if (nucleo_fs_is_protected(abs)) {
            nucleo_app_set_hint("protetto: file di sistema");   // system file — refuse to delete
        } else {
            unlink(abs);
            scan();
        }
    }
    else return;
    
    nucleo_app_request_draw();
}



static void tick(void) { if (app_ui_list_animating()) nucleo_app_request_draw(); }  // redraw only while the list animates

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();

    const char *p = s_path; int pl = strlen(p); if (pl > 24) p += pl - 24;
    int y0 = app_ui_title("Files", DIRC, p);

    if (s_n == 0) { 
        d.fillRect(0, y0, 240, top + h - y0, BG); 
        d.setTextColor(DIM, BG); 
        d.setCursor(12, y0 + 10); 
        d.print("(empty folder)"); 
        return; 
    }
    app_ui_list(y0, top + h - y0, s_n, s_sel, fl_label, fl_right, fl_color, nullptr);
}

extern "C" void nucleo_register_files(void)
{
    static const nucleo_app_def_t app = {
        "files", "Files", "Tools", "Browse and open SD card files",
        'f', 0xFE8C, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
