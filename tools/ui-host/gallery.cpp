// ui-host app gallery: ONE native app (app_<id>.cpp, the real source) rendered on the PC in its three looks —
// compact (the Cardputer alone), companion (the Cardputer while the app owns the external panel) and large
// (on_draw_large on the 320x240 panel, with its touch targets). Built per app by apps.mjs: everything the app
// links that the host does not provide is an auto-generated "return 0" stub, so any app renders without
// hand-written stubs; an app that needs real data gets it from its scene file (tools/ui-host/apps/<id>.txt).
//   gallery_<id>.exe <outdir> <shot> <variant: compact|companion|large> <lang> [key tokens...]
// Key tokens: U D L R ENTER BACK TAB DEL SPACE | c:<ch> | t:<text> | row:<n> (a tapped list row) |
//             tick:<n> (n on_tick calls, 200 ms each) | adv
#include <M5GFX.h>
#include "host_state.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_touch.h"
#include "nucleo_i18n.h"
#include "nucleo_theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern M5GFX d;
void host_display_begin(void);
M5Canvas *nucleo_screen(void);
void nucleo_app_set_gfx(LovyanGFX *g);
bool nucleo_app_is_buffered(void);
void launcher_render_hint_bar(void);
extern "C" void GALLERY_REG(void);

// ---- the framework surface a native app sees (nucleo_app.cpp) -----------------------------------------------
static nucleo_app_def_t s_apps[8];
static int s_app_n;
static const nucleo_app_def_t *s_app;
static void (*s_tab)(void);
static bool (*s_back)(int);
static bool (*s_poll)(void);
static unsigned s_gen = 1;
static bool s_fullscreen;
extern "C" void nucleo_app_register(const nucleo_app_def_t *a) { if (s_app_n < 8) s_apps[s_app_n++] = *a; }
extern "C" void nucleo_app_set_dynamic(const nucleo_app_def_t *, int) {}
extern "C" int  nucleo_app_dynamic_room(void) { return 0; }
extern "C" const char *nucleo_app_active_id(void) { return s_app ? s_app->id : nullptr; }
extern "C" void nucleo_app_set_tab_handler(void (*fn)(void)) { s_tab = fn; }
extern "C" void nucleo_app_set_back_handler(bool (*fn)(int)) { s_back = fn; }
extern "C" void nucleo_app_set_poll_handler(bool (*fn)(void)) { s_poll = fn; }
extern "C" void nucleo_app_set_ptt_handler(void (*)(bool)) {}
extern "C" void nucleo_app_set_direct_draw(bool) {}
extern "C" void nucleo_app_set_fullscreen(bool on) { s_fullscreen = on; }
extern "C" void nucleo_app_set_keep_awake(bool) {}
extern "C" void nucleo_app_display_hold(bool) {}
extern "C" unsigned int nucleo_app_repaint_gen(void) { return s_gen; }
extern "C" bool nucleo_app_launch_id(const char *) { return false; }
extern "C" bool nucleo_app_launch_file(const char *) { return false; }
extern "C" const char *nucleo_app_take_open_file(void) { return nullptr; }
extern "C" void nucleo_app_exit(void) {}
extern "C" void nucleo_app_request_draw(void) {}
extern "C" void nucleo_app_force_repaint(void) {}
extern "C" void nucleo_app_release_buffers(void) {}
extern "C" bool nucleo_app_two_screens(void) { return g_host.two_screens; }
extern "C" int  nucleo_app_content_top(void) { return 0; }
extern "C" int  nucleo_app_content_height(void)
{
    if (g_host.large && !nucleo_app_is_buffered()) return d.height();
    return s_fullscreen ? 135 : 135 - 14;
}
extern "C" const char *nucleo_app_native_id(const char *id) { return id; }

// ---- touch targets: recorded, so the large shot gets an overlay of what a tap would hit ---------------------
struct TRect { int x, y, w, h, key; char ch; bool scroll; };
static TRect s_t[NUCLEO_TOUCH_MAX + NUCLEO_TOUCH_SCROLL_MAX], s_tc[NUCLEO_TOUCH_MAX + NUCLEO_TOUCH_SCROLL_MAX];
static int s_tn, s_tcn, s_tsn;
static int s_toverflow;
extern "C" void nucleo_touch_clear(void) { s_tn = 0; s_tsn = 0; }
extern "C" void nucleo_touch_add(int x, int y, int w, int h, int key, char ch)
{
    int n = s_tn - s_tsn;
    if (n >= NUCLEO_TOUCH_MAX) { s_toverflow++; return; }
    s_t[s_tn++] = { x, y, w, h, key, ch, false };
}
extern "C" void nucleo_touch_add_scroll(int x, int y, int w, int h, int, int, char, int, char)
{
    if (s_tsn >= NUCLEO_TOUCH_SCROLL_MAX) { s_toverflow++; return; }
    s_t[s_tn++] = { x, y, w, h, 0, 0, true }; s_tsn++;
}
extern "C" void nucleo_touch_commit(void) { memcpy(s_tc, s_t, sizeof s_t); s_tcn = s_tn; }

// ---- driving the app -----------------------------------------------------------------------------------------
static void tick(int n) { while (n-- > 0) { g_host.now_us += 200000; if (s_app && s_app->on_tick) s_app->on_tick(); if (s_poll) s_poll(); } }
static void key(int k, char ch = 0)
{
    g_host.now_us += 400000;
    if ((k == NK_BACK || k == NK_LEFT) && s_back && s_back(k)) return;
    if (k == NK_TAB && s_tab) { s_tab(); return; }
    if (s_app && s_app->on_key) s_app->on_key(k, ch);
    tick(1);
}
static void token(const char *t)
{
    static const struct { const char *n; int k; char ch; } K[] = {
        { "U", NK_UP, ';' }, { "D", NK_DOWN, '.' }, { "L", NK_LEFT, ',' }, { "R", NK_RIGHT, '/' },
        { "ENTER", NK_ENTER, '\n' }, { "BACK", NK_BACK, 0 }, { "TAB", NK_TAB, '\t' }, { "DEL", NK_DEL, 8 },
        { "SPACE", NK_CHAR, ' ' },
    };
    for (auto &e : K) if (!strcmp(t, e.n)) { key(e.k, e.ch); return; }
    if (!strncmp(t, "c:", 2)) { key(NK_CHAR, t[2]); return; }
    if (!strncmp(t, "t:", 2)) { for (const char *s = t + 2; *s; s++) key(NK_CHAR, *s == '_' ? ' ' : *s); return; }
    if (!strncmp(t, "row:", 4)) { key(NK_CHAR, (char)(NUCLEO_TOUCH_ROW + atoi(t + 4))); return; }
    if (!strncmp(t, "tick:", 5)) { tick(atoi(t + 5)); return; }
    fprintf(stderr, "unknown key token '%s'\n", t);
    exit(3);
}

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    for (int y = 0; y < d.height(); y++)
        for (int x = 0; x < d.width(); x++) {
            uint16_t c = (uint16_t)d.readPixel(x, y);
            unsigned char px[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                    (unsigned char)((c & 31) * 255 / 31) };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    _putenv("TZ=UTC0"); _tzset();
    if (argc < 5) { fprintf(stderr, "usage: gallery <outdir> <shot> <compact|companion|large> <lang> [keys...]\n"); return 2; }
    const char *out = argv[1], *shot = argv[2], *variant = argv[3];
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "adv")) g_host.adv = true;
    g_host.large = !strcmp(variant, "large");
    g_host.two_screens = strcmp(variant, "compact") != 0;
    host_display_begin();
    nucleo_theme_init();
    nucleo_theme_set("classic");
    nucleo_i18n_set_lang(argv[4]);
    GALLERY_REG();
    if (!s_app_n) { fprintf(stderr, "the app registered nothing\n"); return 2; }
    s_app = &s_apps[0];
    const char *want = getenv("GALLERY_APP");
    for (int i = 0; want && i < s_app_n; i++) if (s_apps[i].id && !strcmp(s_apps[i].id, want)) s_app = &s_apps[i];
    if (g_host.two_screens && !s_app->on_draw_large) printf("gallery-note: %s has no on_draw_large (mirrored)\n", s_app->id);

    if (s_app->ram) for (const nucleo_app_ram_t *r = s_app->ram; r->ptr; r++) *r->ptr = calloc(1, r->bytes);
    if (s_app->on_enter) s_app->on_enter();
    tick(2);
    for (int i = 5; i < argc; i++) if (strcmp(argv[i], "adv")) token(argv[i]);
    tick(6);                                            // let toasts / eases settle
    s_gen++;

    char p[512];
    d.fillScreen(THEME_BG);
    if (g_host.large && s_app->on_draw_large) {
        nucleo_touch_clear();
        d.startWrite(); s_app->on_draw_large(); d.endWrite();
    } else {
        M5Canvas *cv = nucleo_screen();
        nucleo_app_set_gfx(cv);
        cv->fillSprite(THEME_BG);
        nucleo_touch_clear();
        if (s_app->on_draw) s_app->on_draw();
        nucleo_touch_commit();
        nucleo_app_set_gfx(nullptr);
        if (g_host.two_screens) cv->pushSprite(0, 0);   // the companion: the whole canvas, no footer (compact_cb)
        else {
            if (!s_fullscreen) d.setClipRect(0, 0, 240, 135 - 14);
            cv->pushSprite(0, 0);
            d.clearClipRect();
            if (!s_fullscreen) launcher_render_hint_bar();
        }
    }
    snprintf(p, sizeof p, "%s/%s.%s.rgb", out, shot, variant); dump(p);
    if (g_host.large) {                                  // the same frame with every touch target outlined
        if (s_toverflow) printf("gallery-check: %s: %d touch targets past the %d-target table\n", shot, s_toverflow, NUCLEO_TOUCH_MAX);
        for (int i = 0; i < s_tcn; i++) {
            const TRect &r = s_tc[i];
            if (r.scroll) { d.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0x07FF); continue; }
            d.drawRect(r.x, r.y, r.w, r.h, 0xF81F);
            d.fillRect(r.x, r.y, 3, 3, 0xF81F);
        }
        snprintf(p, sizeof p, "%s/%s.touch.rgb", out, shot); dump(p);
        printf("gallery-touch: %s %d targets\n", shot, s_tcn);
    }
    return 0;
}
