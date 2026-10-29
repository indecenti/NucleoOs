#include <direct.h>   // _mkdir: the Video scenes lay out sd/data/Videos
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
#include "nucleo_ui.h"         // the blocking modals (nucleo_ui_modal.cpp) the wizard scenes drive
#include "nucleo_update.h"     // upd_phase_t for the native Updates app scenes
#include "nucleo_store.h"      // nst_status_t / st_state_t for the native Store app scenes
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
    for (int y = 0; y < d.height(); y++)
        for (int x = 0; x < d.width(); x++) {
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

// The Control Center over the launcher, exactly as TAB raises it on the device; keys < 0 are characters.
// Painted like the device run loop does: one frame per key, then a frame per iteration while the scroll eases.
static void cc_scene(std::initializer_list<int> keys)
{
    launcher_reset(); draw_launcher();
    launcher_render_control_center_reset();
    launcher_render_control_center_open();
    for (int k : keys) launcher_render_control_center_key(k < 0 ? NK_CHAR : k, k < 0 ? (char)-k : 0);
    launcher_render_control_center_invalidate();
    launcher_render_control_center();
    for (int i = 0; i < 40 && launcher_render_control_center_animating(); i++) launcher_render_control_center();
}

static int item_index(const char *id)   // the visible-list index of an item of the current menu
{
    for (int i = 0; i < launcher_visible_count(); i++) if (!strcmp(launcher_nth_visible(i)->id, id)) return i;
    return 0;
}

// A store app on the card: what the quick menu shows for one (its facts, Uninstall). The host launcher tree is the
// static registry, so an app already in it (Weather) is the one the card says the store installed.
extern "C" void nucleo_store_apps_sync(void);
static void store_weather(void)
{
    _mkdir("sd"); _mkdir("sd/apps"); _mkdir("sd/apps/weather");
    FILE *f = fopen("sd/apps/weather/.store-version", "wb");
    if (f) { fputs("code 1\nversion 1.0.0\nkind native\nchannel native\nabi 3\ncategory tools\nname Meteo|Weather|Tiempo|Meteo|Wetter\n", f); fclose(f); }
    nucleo_store_apps_sync();
    go_category("Tools", 0);
    launcher_set_sel(item_index("weather"));
}
static void store_weather_done(void) { remove("sd/apps/weather/.store-version"); _rmdir("sd/apps/weather"); nucleo_store_apps_sync(); }

struct Scene { const char *name; void (*run)(void); };
static const Scene SCENES[] = {
    { "home",        [] { launcher_reset(); draw_launcher(); } },
    { "home-row3",   [] { launcher_reset(); launcher_set_sel(3); draw_launcher(); } },
    { "home-end",    [] { launcher_reset(); launcher_set_sel(launcher_visible_count() - 1); draw_launcher(); } },   // the last tile: scrolled
    { "cat-office",  [] { go_category("Office", 0); draw_launcher(); } },
    { "cat-system",  [] { go_category("System", 0); draw_launcher(); } },
    { "cat-games",   [] { go_category("Games", 2); draw_launcher(); } },
    { "cat-games-end", [] { go_category("Games", 0); launcher_set_sel(launcher_visible_count() - 1); draw_launcher(); } },
    { "cat-security",[] { go_category("Security", 0); draw_launcher(); } },
    { "search-set",  [] { type("set"); draw_launcher(); } },
    { "search-rank", [] { type("ca"); draw_launcher(); } },
    { "search-settings", [] { type("bri"); launcher_set_sel(launcher_visible_count() - 1); draw_launcher(); } },
    { "home-recent", [] { launcher_reset(); for (const char *id : { "files", "weather", "wifi" }) launcher_note_launch(id);
                          launcher_reset(); launcher_set_sel(home_index(LAUNCHER_RECENT_ID)); draw_launcher(); } },
    { "cat-recent",  [] { launcher_reset(); for (const char *id : { "files", "weather", "wifi" }) launcher_note_launch(id);
                          go_category(LAUNCHER_RECENT_ID, 0); draw_launcher(); } },
    { "search-none", [] { type("zzq"); draw_launcher(); } },
    // The panel's quick menu (a long press on a tile, or '*'): a built-in app, a store app, its Info, the confirm.
    { "menu",         [] { go_category("Games", 2); launcher_qm_open(launcher_sel()); draw_launcher(); } },
    { "menu-store",   [] { store_weather(); launcher_qm_open(launcher_sel()); draw_launcher(); store_weather_done(); } },
    { "menu-info",    [] { store_weather(); launcher_qm_open(launcher_sel());
                           launcher_qm_key(NK_DOWN, 0); launcher_qm_key(NK_DOWN, 0); launcher_qm_key(NK_ENTER, 0); draw_launcher(); store_weather_done(); } },
    { "menu-confirm", [] { store_weather(); launcher_qm_open(launcher_sel());
                           for (int k = 0; k < 3; k++) launcher_qm_key(NK_DOWN, 0); launcher_qm_key(NK_ENTER, 0); draw_launcher(); store_weather_done(); } },
    // The Cardputer's own panel while the home is on the external one (duplicate mode): time, date, battery,
    // network; then with a track playing.
    { "companion",       [] { launcher_render_companion(&d); } },
    { "companion-music", [] { g_host.playing = true; g_host.track = "/sd/Music/Daft Punk - Veridis Quo.mp3";
                              launcher_render_companion(&d); } },
    { "cc",          [] { cc_scene({}); } },
    { "cc-bright",   [] { cc_scene({ NK_DOWN }); } },                              // brightness card centred
    { "cc-xbright",  [] { cc_scene({ NK_DOWN, NK_DOWN }); } },                     // ADV: the second screen's brightness
    { "cc-volume",   [] { cc_scene({ NK_DOWN, NK_DOWN, NK_DOWN }); } },            // scrolled to mid-column
    { "cc-web",      [] { cc_scene({ NK_DOWN, NK_DOWN, NK_DOWN, NK_DOWN, NK_RIGHT }); } },  // bottom: IP + pairing PIN
    { "cc-restart",  [] { cc_scene({ -'9' }); } },                                // key 9 arms Restart (red)
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
extern "C" void nucleo_app_set_dynamic(const nucleo_app_def_t *defs, int n) {   // store apps: append to the live table
    for (int i = 0; i < n && s_live_n < 8; i++) s_live[s_live_n++] = defs[i];
}
extern "C" int nucleo_app_dynamic_room(void) { return 8 - s_live_n; }
extern "C" void nucleo_register_wifi(void);
extern "C" void nucleo_settings_search_preset(const char *q);
void launcher_render_hint_bar(void);

static const nucleo_app_def_t *s_app;
extern "C" const char *nucleo_app_active_id(void) { return s_app ? s_app->id : ""; }
static void app_open(const char *id)
{
    if (s_app && s_app->on_exit) s_app->on_exit();
    if (s_app && s_app->ram) for (const nucleo_app_ram_t *r = s_app->ram; r->ptr; r++) { free(*r->ptr); *r->ptr = nullptr; }
    s_app = nullptr; s_tab = nullptr; s_back = nullptr;
    for (int i = 0; i < s_live_n; i++) if (!strcmp(s_live[i].id, id)) s_app = &s_live[i];
    s_gen++;
    // Per-app RAM, as the framework does: allocated (zeroed) before on_enter, freed after on_exit.
    if (s_app && s_app->ram) for (const nucleo_app_ram_t *r = s_app->ram; r->ptr; r++) *r->ptr = calloc(1, r->bytes);
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
// What the device does between full paints: the app redraws in place — no clear, no new generation —
// so the direct path repaints only the boxes that changed, through its strip sprite.
static void app_draw(void);
static void app_draw_incr(void)
{
    if (nucleo_screen()) { app_draw(); return; }   // buffered: the framework composes every frame anyway
    for (int t = 0; t < 3; t++) { g_host.now_us += 200000; if (s_app->on_tick) s_app->on_tick(); }
    s_app->on_draw();
    launcher_render_hint_bar();
}
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

extern bool g_host_guest;
extern bool g_host_ext_pending;                       // settings_stubs.cpp
extern bool g_host_onboard_sta;
extern int g_host_join_err;
extern bool g_host_shift_latch;
extern bool g_host_onboarding;                       // settings_stubs.cpp
extern bool g_host_sd_needed;
extern int  g_host_sdc_status;
extern "C" void nucleo_settings_onboard(void);
static const Scene SETTINGS[] = {
    { "root",      [] { app_open("wifi"); app_draw(); } },
    { "root-row4", [] { app_open("wifi"); down(4); app_draw(); } },
    { "root-end",  [] { app_open("wifi"); app_key(NK_UP); app_draw(); } },
    // First boot: Settings opens on Nearby networks; Esc means "use the hotspot". Then the join password.
    { "onboard",      [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_draw(); } },
    { "onboard-pass", [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); down(3); app_key(NK_ENTER); app_type("casa2026"); app_draw(); } },
    { "onboard-skip", [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_draw(); } },
    { "onboard-tab",  [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_TAB); app_key(NK_LEFT); app_draw(); } },   // locked: stays on the list
    { "onboard-done-ap", [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); } },
    { "onboard-done-sta", [] { g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); } },
    { "onboard-wrongpass", [] { g_host_join_err = 1; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); down(3); app_key(NK_ENTER); app_type("casa2026"); app_key(NK_ENTER); app_draw(); } },
    { "onboard-notfound",  [] { g_host_join_err = 2; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); down(3); app_key(NK_ENTER); app_type("casa2026"); app_key(NK_ENTER); app_draw(); } },
    { "onboard-shift", [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); down(3); app_key(NK_ENTER); app_type("casa"); g_host_shift_latch = true; app_draw(); } },
    // After a successful join with the SD missing the web OS: the skippable "SD content" step, then "All set".
    { "onboard-sd",       [] { g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard();
                               app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); } },
    { "onboard-sd-later", [] { g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard();
                               app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw();
                               app_key(NK_RIGHT); app_draw_incr(); } },                        // focus moves: buttons only
    { "onboard-sd-skip",  [] { g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard();
                               app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw();
                               app_key(NK_BACK); app_draw(); } },                              // Esc = later -> "All set"
    { "onboard-sd-update", [] { g_host_sdc_status = 3; g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); } },               // an older release on the card
    { "onboard-sd-resume", [] { g_host_sdc_status = 2; g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); } },               // an interrupted run
    { "onboard-sd-done",   [] { g_host_sd_needed = true; g_host_onboard_sta = true; g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); app_key(NK_BACK); app_key(NK_RIGHT); app_key(NK_ENTER); app_draw(); app_key(NK_BACK); app_draw(); app_key(NK_ENTER); app_draw(); } },   // Later -> explained -> All set
    { "onboard-reveal", [] { g_host_onboarding = true; nucleo_settings_onboard(); app_open("wifi"); down(3); app_key(NK_ENTER); app_type("casa2026"); app_key(NK_TAB); app_draw(); } },
    // Scrolling one row at a time with the heap of a busy ADV (strip sprite 12 rows): every step is an
    // INCREMENTAL repaint. The ADV showed half-drawn rows and a text-less focus chip here (2026-10-01).
    { "root-scroll", [] { g_host.largest_block = 9000; app_open("wifi"); app_draw();
                          for (int i = 0; i < 5; i++) { app_key(NK_DOWN); app_draw_incr(); } } },
    { "wifi",      [] { app_open("wifi"); app_key(NK_ENTER); app_draw(); } },
    { "nets",      [] { app_open("wifi"); app_key(NK_ENTER); app_key(NK_ENTER); app_key(NK_DOWN); app_draw(); } },
    { "hotspot",   [] { app_open("wifi"); app_key(NK_DOWN); app_key(NK_ENTER); app_draw(); } },
    { "display",   [] { app_open("wifi"); down(3); app_key(NK_ENTER); app_draw(); } },
    // ADV: the root gains "Second screen" under Display (row 4); its page, and a speed change waiting for a restart.
    { "root-adv",  [] { g_host.adv = true; app_open("wifi"); down(4); app_draw(); } },
    { "ext",       [] { g_host.adv = true; app_open("wifi"); down(4); app_key(NK_ENTER); app_draw(); } },
    { "ext-speed", [] { g_host.adv = true; g_host_ext_pending = true; app_open("wifi"); down(4); app_key(NK_ENTER); down(4); app_draw(); } },
    { "ext-end",   [] { g_host.adv = true; app_open("wifi"); down(4); app_key(NK_ENTER); app_key(NK_UP); app_key(NK_UP); app_draw(); } },
    { "sound",     [] { app_open("wifi"); down(4); app_key(NK_ENTER); app_draw(); } },
    { "sound-tts", [] { app_open("wifi"); down(4); app_key(NK_ENTER);
                        down(3); app_key(NK_RIGHT); app_draw(); } },
    { "anima",     [] { app_open("wifi"); down(5); app_draw(); } },
    { "language",  [] { app_open("wifi"); down(6); app_draw(); } },
    { "datetime",  [] { app_open("wifi"); down(7); app_key(NK_ENTER); app_draw(); } },
    { "device",    [] { app_open("wifi"); down(8); app_key(NK_ENTER); app_draw(); } },
    { "device-sd", [] { app_open("wifi"); down(8); app_key(NK_ENTER);
                        down(6); app_draw(); } },
    { "device-sdc",    [] { app_open("wifi"); down(8); app_key(NK_ENTER); down(7); app_draw(); } },
    { "device-sdc-manual", [] { g_host_sdc_status = 6; app_open("wifi"); down(8); app_key(NK_ENTER); down(7); app_draw(); } },
    { "device-sdc-old",    [] { g_host_sdc_status = 3; app_open("wifi"); down(8); app_key(NK_ENTER); down(7); app_draw(); } },
    { "device-sdc-ok", [] { g_host_sdc_status = 5; app_open("wifi"); down(8); app_key(NK_ENTER); down(7); app_draw(); } },
    { "device-sdc-ask", [] { app_open("wifi"); down(8); app_key(NK_ENTER); down(7); app_key(NK_ENTER); app_draw(); } },
    { "reset",     [] { app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); app_draw(); } },
    { "reset-sd",  [] { app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); down(2); app_draw(); } },
    { "reset-sd-armed", [] { app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); down(2); app_key(NK_ENTER); app_draw(); } },
    { "reset-sd-guest", [] { g_host_guest = true; app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); down(2); app_key(NK_ENTER); app_draw(); } },
    { "reset-wipe",  [] { app_open("wifi"); app_key(NK_UP); app_key(NK_ENTER); down(3); app_draw(); } },
    { "search",    [] { app_open("wifi"); app_type("lum"); app_draw(); } },
    { "search-none", [] { app_open("wifi"); app_type("qqz"); app_draw(); } },
    { "from-spotlight", [] { nucleo_settings_search_preset("bri"); app_open("wifi"); app_draw(); } },
};

// ---- first-boot wizard: the REAL blocking modals (nucleo_ui_modal.cpp) driven by scripted keys ----------
static void keys(std::initializer_list<int> ks)
{
    static nucleo_key_t q[32]; int n = 0;
    for (int k : ks) if (n < 32) { memset(&q[n], 0, sizeof q[n]); if (k > 0xFF) q[n].key = k >> 8; else { q[n].key = NK_CHAR; q[n].ch = (char)k; } n++; }
    g_host.kbd = q; g_host.kbd_n = n; g_host.kbd_i = 0;
}
#define K(code) ((int)(code) << 8)
static void wizard_lang(void)
{
    static const char *names[] = { "English", "Italiano", "Espanol", "Francais", "Deutsch" };   // nucleo_setup.c
    nucleo_ui_menu("Language / Lingua", names, 5);
}
// Mirror of nucleo_setup_run()'s welcome (nucleo_setup.c needs the whole Wi-Fi stack to compile here).
static const char *T5(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang();
    return !strcmp(l, "it") ? it : !strcmp(l, "es") ? es : !strcmp(l, "fr") ? fr : !strcmp(l, "de") ? de : en;
}
static void wizard_welcome(void)
{
    const char *welcome[] = {
        T5("Benvenuto in NucleoOS.", "Welcome to NucleoOS.", "Bienvenido a NucleoOS.", "Bienvenue dans NucleoOS.", "Willkommen bei NucleoOS."), "",
        T5("Ora scegli la rete Wi-Fi.", "Next: pick your Wi-Fi.", "Ahora elige tu Wi-Fi.", "Choisissez votre Wi-Fi.", "Jetzt WLAN waehlen."),
        T5("Esc = usa l'hotspot.", "Esc = use the hotspot.", "Esc = usar el hotspot.", "Esc = utiliser le hotspot.", "Esc = Hotspot nutzen.") };
    nucleo_ui_message("NucleoOS", welcome, 4);
}
static const Scene WIZARD[] = {
    { "lang",       [] { keys({}); wizard_lang(); } },
    { "lang-row3",  [] { keys({ K(NK_DOWN), K(NK_DOWN), K(NK_DOWN) }); wizard_lang(); } },
    { "lang-end",   [] { keys({ K(NK_UP) }); wizard_lang(); } },
    // The ADV's back-buffer is fitted to its largest block (130 rows): the hint bar must still be whole.
    { "lang-short", [] { g_host.canvas_rows = 130; nucleo_screen_release(); keys({ K(NK_DOWN) }); wizard_lang(); } },
    { "welcome",    [] { keys({}); wizard_welcome(); } },
    // The SD-content install boot: the live screen (direct to the panel, as on the device) and its verdicts.
    { "install-connect", [] { nucleo_ui_modal_direct(true); nucleo_ui_install_t v = {}; v.phase = NUI_INST_CONNECTING; v.pct = -1; v.eta_s = -1;
                              nucleo_ui_install_screen(&v); } },
    { "install-run",     [] { nucleo_ui_modal_direct(true); nucleo_ui_install_t v = {}; v.phase = NUI_INST_DOWNLOADING;
                              v.pct = 8; v.kb_done = 4300; v.kb_total = 53248; v.files_done = 96; v.files_total = 1274; v.eta_s = -1;
                              v.file = "shell.js"; nucleo_ui_install_screen(&v);
                              // later frames: incremental, every field shrinks or grows (stale glyphs would show)
                              v.pct = 63; v.kb_done = 33600; v.files_done = 803; v.eta_s = 412; v.file = "anima-it-akb5.bin";
                              nucleo_ui_install_screen(&v); } },
    { "install-done",    [] { nucleo_ui_modal_direct(true); keys({ K(NK_ENTER) }); nucleo_ui_install_t v = {}; v.phase = NUI_INST_DONE;
                              v.pct = 100; v.kb_done = v.kb_total = 53248; v.files_done = v.files_total = 1274;
                              nucleo_ui_install_result(&v, 1); } },
    { "install-failed",  [] { nucleo_ui_modal_direct(true); keys({ K(NK_ENTER) }); nucleo_ui_install_t v = {}; v.phase = NUI_INST_FAILED;
                              v.err = T5("Download interrotto (rete)", "Download interrupted (network)", "Descarga interrumpida (red)",
                                         "Telechargement coupe (reseau)", "Download abgebrochen (Netz)");
                              nucleo_ui_install_result(&v, 1); } },
    { "install-notfound", [] { nucleo_ui_modal_direct(true); keys({ K(NK_ENTER) }); nucleo_ui_install_t v = {}; v.phase = NUI_INST_FAILED;
                              v.err = T5("Contenuti non trovati: riprova piu tardi o aggiorna il firmware",
                                         "Content not found: retry later or update the firmware",
                                         "Contenido no encontrado: reintenta luego o actualiza el firmware",
                                         "Contenu introuvable : reessayez ou mettez a jour le firmware",
                                         "Inhalt nicht gefunden: erneut versuchen oder Firmware updaten");
                              nucleo_ui_install_result(&v, 1); } },
    { "input",      [] { keys({ 'c', 'a', 's', 'a' }); char b[33] = ""; nucleo_ui_input("Nome dispositivo", b, sizeof b, 0); } },
    { "input-pass", [] { keys({ 's', 'e', 'g', 'r', 'e', 't', 'o' }); char b[65] = ""; nucleo_ui_input("Password", b, sizeof b, 1); } },
    { "input-shift", [] { g_host_shift_latch = true; keys({ 'c', 'a', 's', 'a' }); char b[33] = ""; nucleo_ui_input("Nome dispositivo", b, sizeof b, 0); } },
    { "input-reveal", [] { keys({ 's', 'e', 'g', 'r', 'e', 't', 'o', K(NK_TAB) }); char b[65] = ""; nucleo_ui_input("Password", b, sizeof b, 1); } },
};
#undef K

// Native Updates app (app_updates.cpp): the boot dialog, and the card that says WHERE an update works
// (the web console on a stand-alone unit, the Launcher's OTA list on a guest). Engine stubbed in
// updates_stubs.cpp; the device "knows" v0.5.1 while it runs 0.4.0+host unless a scene says otherwise.
extern "C" const char *g_host_upd_latest;
extern bool g_host_layout_other;                     // settings_stubs.cpp
extern "C" upd_phase_t g_host_upd_phase;
extern "C" const char *g_host_upd_err;
extern "C" void nucleo_register_updates(void);
static const char *lowram_msg(void)          // the engine's own LOWRAM_* line (nucleo_update.c), per language
{
    const char *l = nucleo_i18n_lang();
    return l[0] == 'i' ? "Poca RAM: controlla dal browser" : l[0] == 'f' ? "RAM faible: verifiez via navigateur"
         : l[0] == 'd' ? "Wenig RAM: im Browser pruefen" : (l[0] == 'e' && l[1] == 's') ? "Poca RAM: comprueba en navegador"
         : "Low RAM: check from the browser";
}
static const Scene UPDATES[] = {
    { "main",      [] { app_open("updates"); app_draw(); } },                                   // v0.5.1 available
    { "how",       [] { app_open("updates"); app_key(NK_ENTER); app_draw(); } },                 // stand-alone: the browser
    { "how-guest", [] { g_host_guest = true; app_open("updates"); app_key(NK_ENTER); app_draw(); } },   // M5Launcher
    { "how-other", [] { g_host_layout_other = true; app_open("updates"); app_key(NK_ENTER); app_draw(); } },   // not our table: USB
    { "current",   [] { g_host_upd_latest = "v0.4.0"; app_open("updates"); app_draw(); } },     // up to date
    { "lowram",    [] { g_host_upd_latest = "v0.4.0"; g_host_upd_phase = UPD_CHECK_FAIL; g_host_upd_err = lowram_msg();
                        app_open("updates"); app_draw(); } },                                    // "Check now", heap too tight
};

// Native Store app (app_store.cpp): the REAL engine reads a catalog seeded on the sandbox SD
// (store_stubs.cpp: 11 apps, three installed by the store, two of them with an update, one built-in).
extern "C" void nucleo_register_store(void);
extern "C" void host_store_seed(bool with_catalog);
extern "C" nst_status_t g_host_store_status;
extern "C" nst_op_t g_host_store_op;
extern "C" st_state_t g_host_store_st;
extern "C" const char *g_host_store_err;
extern "C" uint32_t g_host_native_abi;
// The Store opens on the choice of store; the scenes below browse the web store unless they say otherwise.
static void store_choose(bool catalog = true) { host_store_seed(catalog); app_open("store"); }
static void store_open(bool catalog = true) { store_choose(catalog); app_key(NK_CHAR, '2'); }
static void native_open() { store_choose(); app_key(NK_CHAR, '1'); }
static const Scene STORE[] = {
    { "choose",    [] { store_choose(); app_draw(); } },
    { "choose-web",[] { store_choose(); down(1); app_draw(); } },
    { "choose-nonative", [] { g_host_native_abi = 0; store_choose(); app_key(NK_CHAR, '1'); app_draw(); g_host_native_abi = 1; } },
    { "native",    [] { native_open(); app_draw(); } },
    { "native-app",[] { native_open(); app_key(NK_ENTER); app_draw(); } },
    { "native-app-uninstall", [] { native_open(); app_key(NK_ENTER); app_key(NK_RIGHT); app_draw(); } },   // RIGHT focuses Uninstall
    { "native-mine",[] { native_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); app_draw(); } },
    { "new",       [] { store_open(); app_draw(); } },
    { "games",     [] { store_open(); app_key(NK_TAB); app_draw(); } },
    { "cats",      [] { store_open(); app_key(NK_TAB); app_key(NK_TAB); down(1); app_draw(); } },
    { "cat-tools", [] { store_open(); app_key(NK_TAB); app_key(NK_TAB); down(1); app_key(NK_ENTER); app_draw(); } },
    { "mine",      [] { store_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); app_draw(); } },
    { "app-new",   [] { store_open(); app_key(NK_ENTER); app_draw(); } },
    { "app-update",[] { store_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); app_key(NK_ENTER); app_draw(); } },
    { "app-system",[] { store_open(); app_type("met"); app_key(NK_ENTER); app_draw(); } },
    { "app-mine",  [] { store_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); down(2); app_key(NK_ENTER); app_draw(); } },
    { "remove",    [] { store_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); down(2); app_key(NK_ENTER); app_key(NK_DEL); app_draw(); } },
    { "busy",      [] { store_open(); app_key(NK_ENTER); g_host_store_status = NST_BUSY; g_host_store_op = NST_OP_INSTALL;
                        g_host_store_st.phase = ST_PH_FILES; g_host_store_st.pct = 42; g_host_store_st.files_done = 3;
                        g_host_store_st.files_total = 7; strcpy(g_host_store_st.cur, "index.html.gz"); app_draw(); } },
    { "qr",        [] { store_open(); for (int i = 0; i < 3; i++) app_key(NK_TAB); down(2); app_key(NK_ENTER); app_key(NK_CHAR, 'q'); app_draw(); } },
    { "error-cat", [] { g_host_store_status = NST_FAILED; g_host_store_err = "Store unreachable"; store_open(); app_draw(); } },
    { "empty",     [] { store_open(false); app_draw(); } },
    { "error",     [] { g_host_store_status = NST_FAILED; g_host_store_err = "Store unreachable"; store_open(false); app_draw(); } },
};

// ---- Video: the clip browser, compact / on the external panel / its Cardputer companion --------------------------
extern "C" void nucleo_register_video(void);
// A v4 clip header is all the browser reads (duration, picture size); older versions show as "old".
static void video_clip(const char *name, int ver, uint32_t dur_s, int vw, int vh)
{
    char p[160]; snprintf(p, sizeof p, "sd/data/Videos/%s", name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    uint8_t h[64] = { 'N', 'F', 'V', '1', (uint8_t)ver, 3 };
    auto w16 = [&](int o, uint32_t v) { h[o] = v & 255; h[o + 1] = (v >> 8) & 255; };
    auto w32 = [&](int o, uint32_t v) { w16(o, v & 0xffff); w16(o + 2, v >> 16); };
    w16(6, vw); w16(8, (vh + 15) & ~15); w16(10, 15); w32(20, dur_s * 1000); w32(40, vw); w32(44, vh);
    fwrite(h, 1, sizeof h, f); fclose(f);
}
static void video_open(bool clips = true)
{
    _mkdir("sd/data"); _mkdir("sd/data/Videos");
    if (clips) {
        _mkdir("sd/data/Videos/Cortometraggi");
        video_clip("Wallace e Gromit - Una Fantastica Gita.nfv", 4, 1296, 320, 240);
        video_clip("Wallace e Gromit - Il Mistero dei 12 Fornai Assassinati.nfv", 4, 1710, 240, 135);
        video_clip("Wallace e Gromit - I Pantaloni Sbagliati.nfv", 4, 1662, 240, 135);
        video_clip("Screamers - Urla dallo Spazio (1995).nfv", 1, 0, 240, 136);
        video_clip("Cortometraggi/Corto.nfv", 4, 300, 320, 240);
        FILE *r = fopen("sd/data/Videos/.nfv-resume", "w");
        if (r) { fputs("754|Wallace e Gromit - Il Mistero dei 12 Fornai Assassinati.nfv\n", r); fclose(r); }
    }
    app_open("video");
}
static void video_draw(void)
{
    if (g_host.large && s_app->on_draw_large) {               // the external panel: the app's own 320x240 view
        for (int t = 0; t < 12; t++) { g_host.now_us += 200000; if (s_app->on_tick) s_app->on_tick(); }
        d.fillScreen(THEME_BG);
        s_app->on_draw_large();
        return;
    }
    app_draw();
}
static const Scene VIDEO[] = {
    { "list",      [] { video_open(); down(2); video_draw(); } },
    { "companion", [] { g_host.two_screens = true; video_open(); down(2); video_draw(); } },
    { "folder",    [] { video_open(); video_draw(); } },
    { "subfolder", [] { video_open(); app_key(NK_ENTER); video_draw(); } },
    { "old",       [] { video_open(); down(1); video_draw(); } },
    { "resume",    [] { video_open(); down(3); video_draw(); } },
    { "empty",     [] { video_open(false); video_draw(); } },
};

static const char *LANGS[]  = { "en", "it", "es", "fr", "de" };
static const char *THEMES[] = { "classic", "nano_banana", "hacker", "amoled" };

// Every scene runs in its OWN process (run.mjs fans them out): Settings and the Control Center keep
// their focus across opens on purpose (resume), so a shared process would leak one scene into the next.
static const Scene *find_scene(const char *surface, const char *scene)
{
    const Scene *set = !strcmp(surface, "launcher") ? SCENES : !strcmp(surface, "settings") ? SETTINGS
                     : !strcmp(surface, "wizard") ? WIZARD : !strcmp(surface, "updates") ? UPDATES
                     : !strcmp(surface, "store") ? STORE : !strcmp(surface, "video") ? VIDEO : nullptr;
    int n = set == SCENES ? (int)(sizeof SCENES / sizeof SCENES[0]) : set == SETTINGS ? (int)(sizeof SETTINGS / sizeof SETTINGS[0])
          : set == UPDATES ? (int)(sizeof UPDATES / sizeof UPDATES[0]) : set == STORE ? (int)(sizeof STORE / sizeof STORE[0])
          : set == VIDEO ? (int)(sizeof VIDEO / sizeof VIDEO[0])
          : (int)(sizeof WIZARD / sizeof WIZARD[0]);
    for (int i = 0; set && i < n; i++) if (!strcmp(set[i].name, scene)) return &set[i];
    return nullptr;
}

static const char *const LARGE_SCENES[] = { "home", "home-row3", "home-end", "cat-office", "cat-games", "cat-games-end", "search-set", "search-none",
                                              "menu", "menu-store", "menu-info", "menu-confirm",
                                              "cc", "cc-xbright", "cc-volume", "cc-web", "cc-restart" };

static void list_all(void)
{
    struct { const char *surface; const Scene *set; int n; } S[] = {
        { "launcher", SCENES, (int)(sizeof SCENES / sizeof SCENES[0]) },
        { "settings", SETTINGS, (int)(sizeof SETTINGS / sizeof SETTINGS[0]) },
        { "wizard", WIZARD, (int)(sizeof WIZARD / sizeof WIZARD[0]) },
        { "updates", UPDATES, (int)(sizeof UPDATES / sizeof UPDATES[0]) },
        { "store", STORE, (int)(sizeof STORE / sizeof STORE[0]) },
        { "video", VIDEO, (int)(sizeof VIDEO / sizeof VIDEO[0]) },
    };
    for (auto &g : S)
        for (int i = 0; i < g.n; i++) {
            for (const char *l : LANGS) printf("%s.%s.%s.classic\n", g.surface, g.set[i].name, l);
            for (const char *t : THEMES) if (strcmp(t, "classic")) printf("%s.%s.en.%s\n", g.surface, g.set[i].name, t);
            printf("%s.%s.en.classic.direct\n", g.surface, g.set[i].name);
            printf("%s.%s.en.classic.short\n", g.surface, g.set[i].name);   // the ADV's fitted 240x130 canvas
        }
    // The launcher home on an external 320x240 panel (nucleo_surface.h): the only large screen so far.
    for (const char *sc : LARGE_SCENES) {
        printf("launcher.%s.en.classic.large\n", sc);
        printf("launcher.%s.it.classic.large\n", sc);
        printf("launcher.%s.en.amoled.large\n", sc);
        printf("launcher.%s.en.classic.large-short\n", sc);   // ADV's fitted canvas seen 320 wide (97 rows)
    }
    for (const char *sc : { "list", "folder", "subfolder", "old", "resume", "empty" }) {   // Video on the external panel (its own layout)
        printf("video.%s.it.classic.large\n", sc);
        printf("video.%s.en.classic.large\n", sc);
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
    bool large = np == 5 && !strncmp(part[4], "large", 5);
    bool shortc = np == 5 && (!strcmp(part[4], "short") || !strcmp(part[4], "large-short"));
    if (large != g_host.large) { fprintf(stderr, "%s: a .large scene runs in its own process\n", full); return 2; }
    nucleo_theme_set(part[3]);
    nucleo_i18n_set_lang(part[2]);
    g_host.canvas_ok = !direct;
    // Every scene states its canvas height (no leak from a previous scene in this process). 130 rows is what
    // nucleo_screen_acquire() fits on the ADV: (31,744 B largest block - 512) / 240.
    g_host.canvas_rows = shortc ? 130 : 135;
    g_host.stale_mark = shortc && !large; g_host.stale_px = 0;
    nucleo_screen_release();
    launcher_build_menu();
    g_host.now_us += 1000000;
    sc->run();
    dump(full);
    if (shortc && g_host.stale_px) {                  // rows the short canvas left showing the old frame
        int n = 0;
        for (int y = 130; y < 135; y++) for (int x = 0; x < 240; x++) if (d.readPixel(x, y) == g_host.stale_px) n++;
        if (n) printf("ui-check: %s: %d px in rows 130-134 never repainted (the surface blits a short canvas and leaves the old frame there)\n", full, n);
    }
    // The hint bar prints at most 39 Font0 glyphs (launcher_render_hint_bar): a longer hint is cut.
    if (strcmp(part[0], "wizard") && strcmp(part[1], "cc") && strlen(launcher_render_hint()) > 39)
        printf("ui-check: %s: hint is %d chars, the bar shows 39: \"%s\"\n", full, (int)strlen(launcher_render_hint()), launcher_render_hint());
    return 0;
}

int main(int argc, char **argv)
{
    _putenv("TZ=UTC0"); _tzset();
    if (argc > 1 && !strcmp(argv[1], "--list")) { list_all(); return 0; }
    if (argc < 3) { fprintf(stderr, "usage: ui_host <outdir> <scene>... | --list\n"); return 2; }
    s_out = argv[1];
    g_host.large = strstr(argv[2], ".large") != nullptr;   // the panel size is fixed per process (one scene each)
    host_display_begin();
    nucleo_theme_init();
    nucleo_register_wifi();
    nucleo_register_updates();
    nucleo_register_store();
    nucleo_register_video();
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        registry_load(strstr(argv[i], ".adv") != nullptr);
        rc |= render_one(argv[i]);
    }
    return rc;
}
