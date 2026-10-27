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
#include "nucleo_i18n.h"        // TR(it,en): UI labels follow the system language
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, LINE = 0x2945, INK = 0x0000, ACC = 0xFE8C;
#define PIC_DIR NUCLEO_SD_MOUNT "/data/Pictures"
#define PIC_MAX 64

// The name list (64*56 = 3584 B) is malloc'd while Photos is OPEN and freed on exit_app(), so it
// doesn't sit in .bss when closed — that RAM goes to the heap ANIMA L1 draws from. Guarded: if the
// alloc fails, s_n stays 0 (empty listing) and every access is bounded by s_n — never a crash.
static char (*s_names)[56] = nullptr;
static int s_n, s_sel;
static bool s_view;          // false = list, true = full-screen viewer
static char s_open_abs[256]; // full path when opened from Files ("open with"); empty = browsing PIC_DIR
static void enter_view(void);
static void on_key(int key, char ch);
static bool ph_back(int key);

static bool is_img(const char *n)
{
    const char *dot = strrchr(n, '.'); if (!dot) return false;
    return !strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg");
}
static int cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

// ---- oversize guard --------------------------------------------------------------------
// The on-device M5GFX decoder has no PSRAM to fall back on, so a camera-size photo (720p/1080p)
// overflows RAM and crashes. We read just the intrinsic WxH from the file HEADER (no decode) and
// refuse anything past the budget below; the file manager then points the user to the web app.
#define IMG_MAX_DIM 1024        // longest side the decoder handles comfortably on this no-PSRAM board
#define IMG_MAX_PIX 700000L     // ...and total pixels (~0.7 MP); 720p (0.92 MP) and 1080p exceed it
static bool img_dims(const char *path, int *w, int *h)
{
    *w = *h = 0;
    FILE *f = fopen(path, "rb"); if (!f) return false;
    unsigned char b[26]; size_t n = fread(b, 1, sizeof b, f);
    bool ok = false;
    if (n >= 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {            // PNG IHDR @16/@20 (BE)
        *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23]; ok = true;
    } else if (n >= 10 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {                      // GIF @6/@8 (LE)
        *w = b[6] | (b[7] << 8); *h = b[8] | (b[9] << 8); ok = true;
    } else if (n >= 26 && b[0] == 'B' && b[1] == 'M') {                                     // BMP @18/@22 (LE)
        *w = b[18] | (b[19] << 8) | (b[20] << 16) | (b[21] << 24);
        *h = b[22] | (b[23] << 8) | (b[24] << 16) | (b[25] << 24); ok = true;
    } else if (n >= 2 && b[0] == 0xFF && b[1] == 0xD8) {                                    // JPEG: walk to a SOFn marker
        fseek(f, 2, SEEK_SET);
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c != 0xFF) continue;
            int m; do { m = fgetc(f); } while (m == 0xFF);
            if (m == EOF) break;
            if (m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) continue;                            // standalone marker, no length
            int lh = fgetc(f), ll = fgetc(f); if (lh == EOF || ll == EOF) break;
            int len = (lh << 8) | ll;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {            // SOF0..15 (baseline/progressive)
                fgetc(f);                                                                  // precision byte
                int hh = fgetc(f), hl = fgetc(f), wh = fgetc(f), wl = fgetc(f);
                if (wl != EOF) { *h = (hh << 8) | hl; *w = (wh << 8) | wl; ok = true; }
                break;
            }
            fseek(f, len - 2, SEEK_CUR);
        }
    }
    fclose(f);
    return ok && *w > 0 && *h > 0;
}
extern "C" bool nucleo_app_image_oversize(const char *path)
{
    int w, h;
    if (!img_dims(path, &w, &h)) return false;                                             // unknown header -> let it try
    return (w > IMG_MAX_DIM || h > IMG_MAX_DIM || (long)w * h > IMG_MAX_PIX);
}

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
    scan();
    s_open_abs[0] = 0;
    nucleo_app_set_back_handler(ph_back);
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) {
        if (nucleo_app_image_oversize(of)) { nucleo_app_set_hint(TR5("Troppo grande: usa l'app web", "Too large: use the web app", "Muy grande: usa la app web", "Trop gros: utilisez l'app web", "zu gross: nutze die web-app")); return; }   // safety net: never decode a giant
        snprintf(s_open_abs, sizeof s_open_abs, "%s", of); enter_view(); return;   // opened from Files -> show THAT image
    }
    nucleo_app_set_hint(TR5(";/. muovi  invio apri  esc indietro", ";/. move  enter view  esc back", ";/. mueve  enter ver  esc volver", ";/. depl.  entree voir  esc ret.", ";/. bew.  enter zeigen  esc ret."));
}
static void tick(void) { if (!s_view && app_ui_list_animating()) nucleo_app_request_draw(); }  // only while the list animates

// Switch into the full-screen viewer. Shared by Enter and the 1-9 shortcut so the hint,
// buffer release and direct-draw flag stay in sync on both paths.
static void enter_view(void)
{
    s_view = true;
    nucleo_app_set_hint(TR5(";/. prec/succ  del lista  esc indietro", ";/. prev/next  del list  esc back", ";/. prec/sig  del lista  esc volver", ";/. prec/suiv  del liste  esc retour", ";/. prec/naech  del liste  esc ret."));
    nucleo_app_release_buffers();
    nucleo_app_set_direct_draw(true);
}

static const char *ph_label(int i, void *) { return s_names[i]; }
static unsigned short ph_color(int, void *) { return 0xFE8C; }

// Open the highlighted image, unless it's too big for the no-PSRAM decoder (then warn + stay in the list).
static void open_selected(void)
{
    char abs[256]; snprintf(abs, sizeof abs, "%s/%s", PIC_DIR, s_names[s_sel]);
    if (nucleo_app_image_oversize(abs)) nucleo_app_set_hint(TR5("Troppo grande: usa l'app web", "Too large: use the web app", "Muy grande: usa la app web", "Trop gros: utilisez l'app web", "zu gross: nutze die web-app"));
    else enter_view();
}

static void view_to_list(const char *hint)
{
    s_view = false;
    nucleo_app_set_direct_draw(false);
    d.releasePngMemory();
    nucleo_app_set_hint(hint);
}


// ===== two screens ==============================================================================================
// The panel carries the pictures: in the list, the names at the left and the picked one's card at the right (type,
// size in pixels, file size, the too-large warning) with an Open button — no thumbnail is decoded there, so
// browsing never costs the decoder's RAM while the shared canvas is still held. Opening a picture takes the same
// RAM posture as the compact viewer (canvas released, direct draw) and decodes it ONCE fitted to the whole panel,
// straight into the panel (no buffer); a later redraw that shows the same picture skips the decode and repaints
// only the caption. Touch: tap a name to pick it, again (or Open) to view. The viewer's picture carries no targets,
// so the panel's own gestures read it like a photo stack: a swipe left / up = next, right / down = previous (the
// panel's natural-scroll arrows), a tap on the left / right third = previous / next, a tap in the middle (or the
// caption's chevron) = back to the list. Left is the viewer's own there (ph_back), not "close the app".
// The Cardputer, meanwhile, is the companion: what is picked or shown, its numbers, the keys.
#include "nucleo_theme.h"
#include "nucleo_touch.h"
#include "app_large.h"
#include <sys/stat.h>

static const unsigned short C_RED_PH = 0xF96B, BLACK_PH = 0x0000;   // the OS red; the viewer's mat
static const int LG_LISTW = 150, LG_CAP = 24;           // list width; the viewer's caption band
static int s_lg_scroll;
static int s_lg_pic_sel = -1; static unsigned s_lg_pic_gen; static bool s_lg_pic_on;   // what the panel shows now

// The picked picture's path (the "open with" file, or the folder's s_sel) and its bare name.
static const char *lg_path(char *abs, size_t n)
{
    if (s_open_abs[0]) { snprintf(abs, n, "%s", s_open_abs); const char *nm = strrchr(s_open_abs, '/'); return nm ? nm + 1 : s_open_abs; }
    if (s_n <= 0) { abs[0] = 0; return ""; }
    snprintf(abs, n, "%s/%s", PIC_DIR, s_names[s_sel]);
    return s_names[s_sel];
}
static const char *lg_kind(const char *nm)
{
    const char *dot = strrchr(nm, '.');
    return dot && !strcasecmp(dot, ".png") ? "PNG" : "JPEG";
}

static void lg_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    char nm[56]; snprintf(nm, sizeof nm, "%s", s_names[i]);
    char *dot = strrchr(nm, '.'); if (dot && dot != nm) *dot = 0;              // the extension shows on the card
    lg::row_text(x, y, w, h, focus, bg, nm, nullptr, THEME_FG);
}

static void lg_list(void)
{
    const int W = d.width(), H = d.height();
    s_lg_pic_on = false;
    char c[24]; snprintf(c, sizeof c, TR("%d immagini", "%d images"), s_n);
    const int y = lg::header("Photos", c, ACC);
    if (s_n == 0) {
        lg::message(0, y, W, H - y, TR("Nessuna immagine", "No images"), TR("Copia JPEG o PNG in /data/Pictures (dal web o dalla scheda).", "Copy JPEG or PNG files into /data/Pictures (from the web or the card)."), THEME_MUTED);
        return;
    }
    lg::list(0, y + 2, LG_LISTW, H - y - 4, lg::ROW, s_n, s_sel, &s_lg_scroll, lg_row, nullptr, ACC);
    d.fillRect(LG_LISTW, y, W - LG_LISTW, H - y, THEME_BG);
    const int cx = LG_LISTW + 4, cy = y + 6, cw = W - LG_LISTW - 10, ch = H - cy - 6;
    lg::panel(cx, cy, cw, ch);
    const unsigned short cb = lg::card();
    char abs[256]; const char *nm = lg_path(abs, sizeof abs);
    char t[64]; lg::font(1); d.setTextColor(ACC, cb);
    char base[56]; snprintf(base, sizeof base, "%s", nm); char *dot = strrchr(base, '.'); if (dot && dot != base) *dot = 0;
    lg::fit(t, sizeof t, base, cw - 20); d.drawString(t, cx + 10, cy + 8);
    d.drawFastHLine(cx + 10, cy + 30, cw - 20, THEME_LINE);
    int iw = 0, ih = 0; const bool dims = img_dims(abs, &iw, &ih);
    const bool big = dims && (iw > IMG_MAX_DIM || ih > IMG_MAX_DIM || (long)iw * ih > IMG_MAX_PIX);
    int ly = cy + 38;
    lg::kv(cx + 10, ly, cw - 20, TR("Tipo", "Type"), lg_kind(nm), THEME_FG, cb); ly += 22;
    if (dims) { snprintf(t, sizeof t, "%d x %d", iw, ih); lg::kv(cx + 10, ly, cw - 20, TR("Pixel", "Pixels"), t, big ? C_RED_PH : THEME_FG, cb); ly += 22; }
    struct stat st;
    if (stat(abs, &st) == 0) {
        if (st.st_size < 1024) snprintf(t, sizeof t, "%ld B", (long)st.st_size); else snprintf(t, sizeof t, "%ld KB", (long)((st.st_size + 1023) / 1024));
        lg::kv(cx + 10, ly, cw - 20, TR("File", "File"), t, THEME_FG, cb); ly += 22;
    }
    snprintf(t, sizeof t, "%d / %d", s_sel + 1, s_n);
    lg::kv(cx + 10, ly, cw - 20, TR("Posizione", "Position"), t, THEME_MUTED, cb);
    if (big) {                                               // the no-PSRAM decoder would crash on it: say where it opens
        lg::font(0); d.setTextColor(C_RED_PH, cb);
        lg::wrap(TR("Troppo grande per il Cardputer: aprila dall'app web.", "Too large for the Cardputer: open it in the web app."), cx + 10, cy + ch - 56, cw - 20, 17, 3);
    } else {
        lg::button(cx + 8, cy + ch - 40, cw - 16, 32, TR("Apri", "Open"), ACC, THEME_INK, NK_ENTER, 0);
        nucleo_touch_add(cx, cy, cw, ch - 44, NK_ENTER, 0);   // the card itself opens too
    }
}

static void lg_view(void)
{
    const int W = d.width(), H = d.height(), PH = H - LG_CAP;
    char abs[256]; const char *nm = lg_path(abs, sizeof abs);
    const unsigned gen = nucleo_app_repaint_gen();
    const int key = s_open_abs[0] ? -2 : s_sel;
    if (!s_lg_pic_on || s_lg_pic_sel != key || s_lg_pic_gen != gen) {   // a new picture (or the panel was overdrawn)
        d.fillRect(0, 0, W, PH, BLACK_PH);
        if (abs[0]) {
            const char *dot = strrchr(nm, '.');
            if (dot && !strcasecmp(dot, ".png")) d.drawPngFile(abs, W / 2, PH / 2, W, PH, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
            else                                 d.drawJpgFile(abs, W / 2, PH / 2, W, PH, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        }
        s_lg_pic_on = true; s_lg_pic_sel = key; s_lg_pic_gen = gen;
    }
    // the caption band: back chevron + name, the position, the step arrows
    d.fillRect(0, PH, W, LG_CAP, THEME_BG);
    const int cy = PH + LG_CAP / 2;
    d.drawLine(16, cy, 22, cy - 6, ACC); d.drawLine(16, cy, 22, cy + 6, ACC); d.drawLine(17, cy, 23, cy - 6, ACC); d.drawLine(17, cy, 23, cy + 6, ACC);
    lg::font(0); d.setTextDatum(middle_left); d.setTextColor(THEME_FG, THEME_BG);
    char t[64]; lg::fit(t, sizeof t, nm, W - 120); d.drawString(t, 32, cy);
    if (!s_open_abs[0]) {
        char pos[16]; snprintf(pos, sizeof pos, "%d / %d", s_sel + 1, s_n);
        d.setTextDatum(middle_right); d.setTextColor(THEME_MUTED, THEME_BG); d.drawString(pos, W - 10, cy);
    }
    d.setTextDatum(top_left);
    nucleo_touch_add(0, PH, W / 2, LG_CAP, NK_DEL, 8);                   // the caption's chevron: back to the list
}                                                                       // the picture: the panel's gestures (above)

// Two screens, in the viewer: Left (a swipe right / a tap on the left third) = the previous picture, not "close".
static bool ph_back(int key)
{
    if (key != NK_LEFT || !s_view || s_open_abs[0] || !nucleo_app_two_screens()) return false;
    on_key(NK_UP, ';');
    return true;
}

static void draw_large(void)
{
    nucleo_touch_clear();
    if (s_view) lg_view(); else lg_list();
    nucleo_touch_commit();
}

static void lg_tap_list(int key, char ch, bool *done)                    // a tapped name: pick it, again = open it
{
    *done = false;
    if (s_view || !nucleo_touch_is_row(key, ch)) return;
    *done = true;
    if (lg::row_tap(key, ch, s_lg_scroll, s_n, &s_sel) == 2) open_selected();
    nucleo_app_request_draw();
}

// The Cardputer while the panel shows the pictures: the picked / shown one, its numbers, the keys.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, THEME_BG);
    int y = lg::comp_head("PHOTOS", ACC);
    if (s_n == 0 && !s_open_abs[0]) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(THEME_FG, THEME_BG); d.drawString(TR("Nessuna immagine", "No images"), 8, y + 6);
        d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG); d.drawString("/data/Pictures", 8, y + 30);
        lg::comp_keys(TR("ESC esci", "ESC exit"));
        return;
    }
    char abs[256]; const char *nm = lg_path(abs, sizeof abs);
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(THEME_FG, THEME_BG);
    const int nl = lg::wrap(nm, 8, y + 4, 224, 20, 2);
    d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG);
    int iw = 0, ih = 0; char m[64];
    if (img_dims(abs, &iw, &ih)) snprintf(m, sizeof m, "%s  %d x %d", lg_kind(nm), iw, ih); else snprintf(m, sizeof m, "%s", lg_kind(nm));
    d.drawString(m, 8, y + 10 + nl * 20);
    if (!s_open_abs[0]) { snprintf(m, sizeof m, TR("%d di %d", "%d of %d"), s_sel + 1, s_n); d.drawString(m, 8, y + 22 + nl * 20); }
    if (s_view) { d.setTextColor(ACC, THEME_BG); d.drawString(TR("sul pannello", "on the panel"), 160, y + 22 + nl * 20); }
    lg::comp_keys(s_view ? (s_open_abs[0] ? TR("CANC chiudi  ESC esci", "DEL close  ESC exit") : TR(";/. prec/succ  CANC lista  ESC esci", ";/. prev/next  DEL list  ESC exit"))
                         : TR(";/. scegli  INVIO apri  1-9 apri", ";/. pick  ENTER view  1-9 view"));
}

static void on_key(int key, char ch)
{
    { bool done; lg_tap_list(key, ch, &done); if (done) return; }      // the panel's taps (two screens)
    if (s_view && nucleo_app_two_screens() && !s_open_abs[0]) {        // the panel's picture gestures (ph_back)
        if (key == NK_RIGHT) key = NK_DOWN;                            // swipe left / tap the right third: next
        else if (key == NK_ENTER) key = NK_DEL;                        // tap the middle: back to the list
    }
    if (s_view) {
        if (s_open_abs[0]) {                          // external "open with" image: single file, no prev/next
            if (key == NK_DEL) { s_open_abs[0] = 0; s_view = false; nucleo_app_set_direct_draw(false); d.releasePngMemory(); nucleo_app_set_hint(TR5(";/. muovi  invio apri  esc indietro", ";/. move  enter view  esc back", ";/. mueve  enter ver  esc volver", ";/. depl.  entree voir  esc ret.", ";/. bew.  enter zeigen  esc ret.")); }
            else return;
        }
        else if (key == NK_UP || key == NK_DOWN) {
            if (!s_n) return;
            s_sel = (key == NK_UP) ? (s_sel + s_n - 1) % s_n : (s_sel + 1) % s_n;
            // prev/next must pass the same oversize guard as Enter: a camera-size neighbour crashes the decoder
            char abs[256]; snprintf(abs, sizeof abs, "%s/%s", PIC_DIR, s_names[s_sel]);
            if (nucleo_app_image_oversize(abs)) view_to_list(TR5("Troppo grande: usa l'app web", "Too large: use the web app", "Muy grande: usa la app web", "Trop gros: utilisez l'app web", "zu gross: nutze die web-app"));
        }
        else if (key == NK_DEL)  view_to_list(TR5(";/. muovi  invio apri  esc indietro", ";/. move  enter view  esc back", ";/. mueve  enter ver  esc volver", ";/. depl.  entree voir  esc ret.", ";/. bew.  enter zeigen  esc ret."));
        else return;
    } else {
        if (key == NK_UP)        { if (s_n) s_sel = (s_sel + s_n - 1) % s_n; }
        else if (key == NK_DOWN) { if (s_n) s_sel = (s_sel + 1) % s_n; }
        else if (key == NK_ENTER && s_n) { open_selected(); }
        else if (key == NK_CHAR && ch >= '1' && ch <= '9') {
            int i = ch - '1';
            if (i < s_n) { s_sel = i; open_selected(); }
        }
        else return;
    }
    nucleo_app_request_draw();
}

static void draw_view(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, INK);
    char abs[256]; const char *nm;
    if (s_open_abs[0]) { snprintf(abs, sizeof(abs), "%s", s_open_abs); nm = strrchr(s_open_abs, '/'); nm = nm ? nm + 1 : s_open_abs; }
    else {
        if (s_n <= 0) return;                                 // nothing to show
        snprintf(abs, sizeof(abs), "%s/%s", PIC_DIR, s_names[s_sel]); nm = s_names[s_sel];
    }
    const char *dot = strrchr(nm, '.');
    // Auto-fit to the content box, centred. scale 0 => M5GFX scales to maxWidth/Height.
    // NOTE: M5GFX file decode — verify on hardware.
    if (dot && !strcasecmp(dot, ".png")) d.drawPngFile(abs, 120, top + h / 2, 240, h - 12, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    else                                 d.drawJpgFile(abs, 120, top + h / 2, 240, h - 12, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    // caption strip
    char cap[44];
    if (s_open_abs[0]) snprintf(cap, sizeof(cap), "%.34s", nm);
    else               snprintf(cap, sizeof(cap), "%.26s  %d/%d", nm, s_sel + 1, s_n);
    d.setTextSize(1); d.setTextColor(FG, INK); d.setCursor(6, top + h - 10); d.print(cap);
}

static void draw_list(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    char c[16]; snprintf(c, sizeof(c), "%d image%s", s_n, s_n == 1 ? "" : "s");
    int y0 = app_ui_title("Photos", ACC, c);
    if (s_n == 0) { d.setTextColor(DIM, BG); d.setCursor(12, y0 + 16); d.print(TR5("Nessuna immagine in /data/Pictures", "No images in /data/Pictures", "Sin imagenes en /data/Pictures", "Pas images /data/Pictures", "keine bilder in /data/Pictures")); return; }
    app_ui_list(y0, top + h - y0, s_n, s_sel, ph_label, nullptr, ph_color, nullptr);
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    if (s_view) draw_view(); else draw_list();
}

static void exit_app(void) { d.releasePngMemory(); if (s_names) { free(s_names); s_names = nullptr; s_n = 0; } }   // return ~3.5 KB to the heap for ANIMA

extern "C" void nucleo_register_photos(void)
{
    static const nucleo_app_def_t app = {
        "photos", "Photos", "Media", "Browse images on the SD card",
        'P', 0xFE8C, enter, on_key, tick, draw, exit_app, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
