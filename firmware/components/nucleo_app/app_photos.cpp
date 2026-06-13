// Photos app: a native image viewer for the Cardputer. Lists the pictures on the SD
// card (/data/Pictures), then shows the selected one full-screen, auto-fitted and
// centred. JPEG/PNG decoding is done by M5GFX itself — no extra decoder. Keyboard:
// up/down to move, Enter to view, up/down to flip through images, Backspace back to
// the list, Esc to leave.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
extern "C" {
#include "nucleo_board.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, LINE = 0x2945, INK = 0x0000, ACC = 0xFE8C;
#define PIC_DIR NUCLEO_SD_MOUNT "/data/Pictures"
#define PIC_MAX 64

// The name list (64*56 = 3584 B) is malloc'd while Photos is OPEN and freed on exit_app(), so it
// doesn't sit in .bss when closed — that RAM goes to the heap ANIMA L1 draws from. Guarded: if the
// alloc fails, s_n stays 0 (empty listing) and every access is bounded by s_n — never a crash.
static char (*s_names)[56] = nullptr;
static int s_n, s_sel;
static bool s_view;          // false = list, true = full-screen viewer

static bool is_img(const char *n)
{
    const char *dot = strrchr(n, '.'); if (!dot) return false;
    return !strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg");
}
static int cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void scan(void)
{
    s_n = 0; s_sel = 0; s_view = false;
    if (!s_names) return;                                // no buffer (alloc failed / not entered) -> empty
    DIR *dir = opendir(PIC_DIR);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && s_n < PIC_MAX)
        if (e->d_name[0] != '.' && is_img(e->d_name)) snprintf(s_names[s_n++], 56, "%s", e->d_name);
    closedir(dir);
    qsort(s_names, s_n, 56, cmp);
}

static void enter(void)
{
    if (!s_names) s_names = (char (*)[56])malloc((size_t)PIC_MAX * 56);   // ~3.5 KB, only while open
    scan(); nucleo_app_set_hint(";/. move  enter view  esc back");
}
static void tick(void) { if (!s_view && app_ui_list_animating()) nucleo_app_request_draw(); }  // only while the list animates

// Switch into the full-screen viewer. Shared by Enter and the 1-9 shortcut so the hint,
// buffer release and direct-draw flag stay in sync on both paths.
static void enter_view(void)
{
    s_view = true;
    nucleo_app_set_hint(";/. prev/next  del list  esc back");
    nucleo_app_release_buffers();
    nucleo_app_set_direct_draw(true);
}

static const char *ph_label(int i, void *) { return s_names[i]; }
static unsigned short ph_color(int, void *) { return 0xFE8C; }

static void on_key(int key, char ch)
{
    if (s_view) {
        if (key == NK_UP)        { if (s_n) s_sel = (s_sel + s_n - 1) % s_n; }
        else if (key == NK_DOWN) { if (s_n) s_sel = (s_sel + 1) % s_n; }
        else if (key == NK_DEL)  { 
            s_view = false; 
            nucleo_app_set_direct_draw(false); 
            d.releasePngMemory();
        }
        else return;
    } else {
        if (key == NK_UP)        { if (s_n) s_sel = (s_sel + s_n - 1) % s_n; }
        else if (key == NK_DOWN) { if (s_n) s_sel = (s_sel + 1) % s_n; }
        else if (key == NK_ENTER && s_n) { enter_view(); }
        else if (key == NK_CHAR && ch >= '1' && ch <= '9') {
            int i = ch - '1';
            if (i < s_n) { s_sel = i; enter_view(); }
        }
        else return;
    }
    nucleo_app_request_draw();
}

static void draw_view(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, INK);
    char abs[200]; snprintf(abs, sizeof(abs), "%s/%s", PIC_DIR, s_names[s_sel]);
    const char *dot = strrchr(s_names[s_sel], '.');
    // Auto-fit to the content box, centred. scale 0 => M5GFX scales to maxWidth/Height.
    // NOTE: M5GFX file decode — verify on hardware.
    if (dot && !strcasecmp(dot, ".png")) d.drawPngFile(abs, 120, top + h / 2, 240, h - 12, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    else                                 d.drawJpgFile(abs, 120, top + h / 2, 240, h - 12, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    // caption strip
    char cap[40]; snprintf(cap, sizeof(cap), "%.26s  %d/%d", s_names[s_sel], s_sel + 1, s_n);
    d.setTextSize(1); d.setTextColor(FG, INK); d.setCursor(6, top + h - 10); d.print(cap);
}

static void draw_list(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    char c[16]; snprintf(c, sizeof(c), "%d image%s", s_n, s_n == 1 ? "" : "s");
    int y0 = app_ui_title("Photos", ACC, c);
    if (s_n == 0) { d.setTextColor(DIM, BG); d.setCursor(12, y0 + 16); d.print("No images in /data/Pictures"); return; }
    app_ui_list(y0, top + h - y0, s_n, s_sel, ph_label, nullptr, ph_color, nullptr);
}

static void draw(void) { if (s_view) draw_view(); else draw_list(); }

static void exit_app(void) { d.releasePngMemory(); if (s_names) { free(s_names); s_names = nullptr; s_n = 0; } }   // return ~3.5 KB to the heap for ANIMA

extern "C" void nucleo_register_photos(void)
{
    static const nucleo_app_def_t app = {
        "photos", "Photos", "Media", "Browse images on the SD card",
        'P', 0xFE8C, enter, on_key, tick, draw, exit_app
    };
    nucleo_app_register(&app);
}
