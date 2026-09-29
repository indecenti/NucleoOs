// ui-host scene driver: builds the REAL launcher tree from the real app registry, drives the REAL
// renderer (launcher_render.cpp) into the in-memory 240x135 display, and dumps every scene as a
// raw RGB image (run.mjs turns them into PNGs + a hash manifest for the golden gate).
//   ui_host.exe <outdir> [filter]
#include <M5GFX.h>
#include "host_state.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_menu.h"
#include "launcher_render.h"
#include "nucleo_i18n.h"
#include "nucleo_theme.h"
#include "apps.gen.h"          // HOST_APPS[], HOST_APP_N (run.mjs, from tools/launcher-host/apps.mjs)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern M5GFX d;
void host_display_begin(void);
void nucleo_screen_release(void);
M5Canvas *nucleo_screen(void);
void nucleo_app_set_gfx(LovyanGFX *g);

// ---- registry: the real app table, gated per board like register_builtins -----------------------
static nucleo_app_def_t s_reg[NUCLEO_APP_MAX];
static int s_reg_n;
int nucleo_app_count(void) { return s_reg_n; }
const nucleo_app_def_t *nucleo_app_at(int i) { return &s_reg[i]; }
static void registry_load(bool adv)
{
    s_reg_n = 0;
    for (int i = 0; i < HOST_APP_N && s_reg_n < NUCLEO_APP_MAX; i++) {
        const HostApp &a = HOST_APPS[i];
        if (a.adv_only && !adv) continue;
        nucleo_app_def_t def = {};
        def.id = a.id; def.name = a.name; def.category = a.cat; def.desc = a.desc;
        def.icon = a.icon; def.color = a.color;
        s_reg[s_reg_n++] = def;
    }
}

// ---- output ---------------------------------------------------------------------------------------
static const char *s_out = "build/ui-host/shots";
static int s_written;
static void dump(const char *name)
{
    char p[256]; snprintf(p, sizeof p, "%s/%s.rgb", s_out, name);
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p); exit(2); }
    for (int y = 0; y < 135; y++)
        for (int x = 0; x < 240; x++) {
            uint16_t c = (uint16_t)d.readPixel(x, y);          // RGB565
            unsigned char px[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31),
                                    (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                    (unsigned char)((c & 31) * 255 / 31) };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    s_written++;
}

// ---- scene helpers --------------------------------------------------------------------------------
static void settle_list(void)
{
    for (int i = 0; i < 400; i++) {                 // the carousel eases toward the focus: let it land
        bool moving = launcher_render_step_scroll();
        launcher_render_list();
        g_host.now_us += 16000;
        if (!moving) break;
    }
}
static void draw_launcher(void)
{
    d.fillScreen(THEME_BG);
    launcher_render_update_chrome();
    launcher_render_chrome();
    settle_list();
    launcher_render_hint_bar();
}
static int home_index(const char *id)
{
    const MenuNode *const *it = launcher_node()->items;
    for (int i = 0; it && it[i]; i++) if (!strcmp(it[i]->id, id)) return i;
    return 0;
}
static void go_category(const char *cat, int row)
{
    launcher_reset();
    launcher_set_sel(home_index(cat));
    launcher_enter();
    launcher_set_sel(row);
}
static void type(const char *s) { launcher_reset(); while (*s) launcher_filter_push(*s++); }

struct Scene { const char *name; void (*run)(void); };
static const Scene SCENES[] = {
    { "home",        [] { launcher_reset(); draw_launcher(); } },
    { "home-row3",   [] { launcher_reset(); launcher_set_sel(3); draw_launcher(); } },
    { "cat-office",  [] { go_category("Office", 0); draw_launcher(); } },
    { "cat-system",  [] { go_category("System", 0); draw_launcher(); } },
    { "cat-games",   [] { go_category("Games", 2); draw_launcher(); } },
    { "cat-security",[] { go_category("Security", 0); draw_launcher(); } },
    { "search-set",  [] { type("set"); draw_launcher(); } },
    { "search-rank", [] { type("ca"); draw_launcher(); } },
    { "search-settings", [] { type("bri"); launcher_set_sel(launcher_visible_count() - 1); draw_launcher(); } },
    { "home-recent", [] { launcher_reset(); for (const char *id : { "files", "weather", "wifi" }) launcher_note_launch(id);
                          launcher_reset(); launcher_set_sel(home_index(LAUNCHER_RECENT_ID)); draw_launcher(); } },
    { "cat-recent",  [] { launcher_reset(); for (const char *id : { "files", "weather", "wifi" }) launcher_note_launch(id);
                          go_category(LAUNCHER_RECENT_ID, 0); draw_launcher(); } },
    { "search-none", [] { type("zzq"); draw_launcher(); } },
    { "cc",          [] { launcher_reset(); draw_launcher(); launcher_render_control_center_open();
                          launcher_render_control_center_invalidate(); launcher_render_control_center(); } },
};

// ---- foreground-app framework (what nucleo_app.cpp gives a native app) -----------------------------
static nucleo_app_def_t s_live[8];
static int s_live_n;
static void (*s_tab)(void);
static bool (*s_back)(int);
static unsigned s_gen;
extern "C" void nucleo_app_register(const nucleo_app_def_t *a) { if (s_live_n < 8) s_live[s_live_n++] = *a; }
extern "C" void nucleo_app_set_tab_handler(void (*fn)(void)) { s_tab = fn; }
extern "C" void nucleo_app_set_back_handler(bool (*fn)(int)) { s_back = fn; }
extern "C" unsigned int nucleo_app_repaint_gen(void) { return s_gen; }
extern "C" bool nucleo_app_launch_id(const char *) { return false; }
extern "C" void nucleo_register_wifi(void);
extern "C" void nucleo_settings_search_preset(const char *q);
void launcher_render_hint_bar(void);

static const nucleo_app_def_t *s_app;
static void app_open(const char *id)
{
    if (s_app && s_app->on_exit) s_app->on_exit();
    s_app = nullptr; s_tab = nullptr; s_back = nullptr;
    for (int i = 0; i < s_live_n; i++) if (!strcmp(s_live[i].id, id)) s_app = &s_live[i];
    s_gen++;
    if (s_app && s_app->on_enter) s_app->on_enter();
}
// One key through the same routing as the run loop: Esc/LEFT -> back handler, TAB -> tab handler.
static void app_key(int key, char ch = 0)
{
    g_host.now_us += 400000;                     // past every hold/accelerate/throttle window
    if ((key == NK_BACK || key == NK_LEFT) && s_back && s_back(key)) return;
    if (key == NK_TAB && s_tab) { s_tab(); return; }
    if (s_app && s_app->on_key) s_app->on_key(key, ch);
}
static void app_type(const char *s) { while (*s) app_key(NK_CHAR, *s++); }
static void down(int n) { while (n-- > 0) app_key(NK_DOWN); }
static void app_draw(void)
{
    for (int t = 0; t < 12; t++) { g_host.now_us += 200000; if (s_app->on_tick) s_app->on_tick(); }   // let toasts/eases settle
    s_gen++;                                     // a fresh frame: incremental painters repaint in full
    M5Canvas *cv = nucleo_screen();
    d.fillScreen(THEME_BG);
    if (cv) {                                    // the buffered path: compose off-screen, blit the content area
        nucleo_app_set_gfx(cv);
        cv->fillSprite(THEME_BG);
        s_app->on_draw();
        nucleo_app_set_gfx(nullptr);
        d.setClipRect(0, 0, 240, 135 - 14);
        cv->pushSprite(0, 0);
        d.clearClipRect();
    } else {                                     // the direct path (ADV after Wi-Fi): draw straight to the panel
        s_app->on_draw();
    }
    launcher_render_hint_bar();
}

static const Scene SETTINGS[] = {
    { "root",      [] { app_open("wifi"); app_draw(); } },
    { "root-row4", [] { app_open("wifi"); down(4); app_draw(); } },
    { "root-end",  [] { app_open("wifi"); app_key(NK_UP); app_draw(); } },
    { "wifi",      [] { app_open("wifi"); app_key(NK_ENTER); app_draw(); } },
    { "nets",      [] { app_open("wifi"); app_key(NK_ENTER); app_key(NK_ENTER); app_key(NK_DOWN); app_draw(); } },
    { "hotspot",   [] { app_open("wifi"); app_key(NK_DOWN); app_key(NK_ENTER); app_draw(); } },
    { "display",   [] { app_open("wifi"); down(3); app_key(NK_ENTER); app_draw(); } },
    { "sound",     [] { app_open("wifi"); down(4); app_key(NK_ENTER); app_draw(); } },
    { "sound-tts", [] { app_open("wifi"); down(4); app_key(NK_ENTER);
                        down(3); app_key(NK_RIGHT); app_draw(); } },
    { "anima",     [] { app_open("wifi"); down(5); app_draw(); } },
    { "language",  [] { app_open("wifi"); down(6); app_draw(); } },
    { "datetime",  [] { app_open("wifi"); down(7); app_key(NK_ENTER); app_draw(); } },
    { "device",    [] { app_open("wifi"); down(8); app_key(NK_ENTER); app_draw(); } },
    { "device-sd", [] { app_open("wifi"); down(8); app_key(NK_ENTER);
                        down(6); app_draw(); } },
    { "reset",     [] { app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); app_draw(); } },
    { "search",    [] { app_open("wifi"); app_type("lum"); app_draw(); } },
    { "search-none", [] { app_open("wifi"); app_type("qqz"); app_draw(); } },
    { "from-spotlight", [] { nucleo_settings_search_preset("bri"); app_open("wifi"); app_draw(); } },
};

static const char *LANGS[]  = { "en", "it", "es", "fr", "de" };
static const char *THEMES[] = { "classic", "nano_banana", "hacker", "amoled" };

// Every scene runs in its OWN process (run.mjs fans them out): Settings and the Control Center keep
// their focus across opens on purpose (resume), so a shared process would leak one scene into the next.
static const Scene *find_scene(const char *surface, const char *scene)
{
    const Scene *set = !strcmp(surface, "launcher") ? SCENES : !strcmp(surface, "settings") ? SETTINGS : nullptr;
    int n = set == SCENES ? (int)(sizeof SCENES / sizeof SCENES[0]) : (int)(sizeof SETTINGS / sizeof SETTINGS[0]);
    for (int i = 0; set && i < n; i++) if (!strcmp(set[i].name, scene)) return &set[i];
    return nullptr;
}

static void list_all(void)
{
    struct { const char *surface; const Scene *set; int n; } S[] = {
        { "launcher", SCENES, (int)(sizeof SCENES / sizeof SCENES[0]) },
        { "settings", SETTINGS, (int)(sizeof SETTINGS / sizeof SETTINGS[0]) },
    };
    for (auto &g : S)
        for (int i = 0; i < g.n; i++) {
            for (const char *l : LANGS) printf("%s.%s.%s.classic\n", g.surface, g.set[i].name, l);
            for (const char *t : THEMES) if (strcmp(t, "classic")) printf("%s.%s.en.%s\n", g.surface, g.set[i].name, t);
            printf("%s.%s.en.classic.direct\n", g.surface, g.set[i].name);
        }
}

static int render_one(const char *full)
{
    char buf[128]; snprintf(buf, sizeof buf, "%s", full);
    char *part[5] = { 0 }; int np = 0;
    for (char *t = strtok(buf, "."); t && np < 5; t = strtok(nullptr, ".")) part[np++] = t;
    if (np < 4) { fprintf(stderr, "bad scene name %s\n", full); return 2; }
    const Scene *sc = find_scene(part[0], part[1]);
    if (!sc) { fprintf(stderr, "unknown scene %s\n", full); return 2; }
    bool direct = np == 5 && !strcmp(part[4], "direct");
    nucleo_theme_set(part[3]);
    nucleo_i18n_set_lang(part[2]);
    g_host.canvas_ok = !direct;
    if (direct) nucleo_screen_release();
    launcher_build_menu();
    g_host.now_us += 1000000;
    sc->run();
    dump(full);
    // The hint bar prints at most 39 Font0 glyphs (launcher_render_hint_bar): a longer hint is cut.
    if (strcmp(part[1], "cc") && strlen(launcher_render_hint()) > 39)
        printf("ui-check: %s: hint is %d chars, the bar shows 39: \"%s\"\n", full, (int)strlen(launcher_render_hint()), launcher_render_hint());
    return 0;
}

int main(int argc, char **argv)
{
    _putenv("TZ=UTC0"); _tzset();
    if (argc > 1 && !strcmp(argv[1], "--list")) { list_all(); return 0; }
    if (argc < 3) { fprintf(stderr, "usage: ui_host <outdir> <scene>... | --list\n"); return 2; }
    s_out = argv[1];
    host_display_begin();
    nucleo_theme_init();
    nucleo_register_wifi();
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        registry_load(strstr(argv[i], ".adv") != nullptr);
        rc |= render_one(argv[i]);
    }
    return rc;
}
