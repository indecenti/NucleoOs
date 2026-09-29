// Host gate for the native launcher model: compiles the REAL
// firmware/components/nucleo_app/launcher_menu.cpp and drives it against (a) the real app set,
// extracted from the firmware sources by check.mjs into build/launcher-host/apps.gen.h, and
// (b) synthetic registries that push the capacity limits. No device, no display.
#include "nucleo_app.h"
#include "launcher_menu.h"
#include "esp_system.h"
#include "nucleo_board.h"      // NUCLEO_CFG_MOUNT (stub: a sandbox under build/)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "apps.gen.h"          // REAL_APPS[], REAL_APP_N, REAL_CALL_SITES

// ---- host implementations of what launcher_menu.cpp links against ----------------------------
static nucleo_app_def_t g_reg[NUCLEO_APP_MAX];
static int g_reg_n;
int nucleo_app_count(void) { return g_reg_n; }
const nucleo_app_def_t *nucleo_app_at(int i) { return &g_reg[i]; }

static esp_reset_reason_t g_reason = ESP_RST_SW;
esp_reset_reason_t esp_reset_reason(void) { return g_reason; }

// Title table stand-in (the real one lives in launcher_render.cpp): enough to prove that Spotlight
// scores an app by its title in every language, not only by its registered label.
static int g_title_calls;
extern "C" const char *launcher_app_title(const char *id, int lang)
{
    g_title_calls++;
    static const char *const WEATHER[] = { "Meteo", "Weather", "Tiempo", "Meteo", "Wetter" };
    static const char *const SETTINGS[] = { "Impostazioni", "Settings", "Ajustes", "Reglages", "Einstellungen" };
    const char *const *t = !strcmp(id, "weather") ? WEATHER : !strcmp(id, "wifi") ? SETTINGS : nullptr;
    return t && lang >= 0 && lang < 5 ? t[lang] : nullptr;
}

// Settings search stand-in (the real scan lives in app_wifi.cpp): "bri" finds 2 settings, "zz" none.
static int g_setq_calls;
extern "C" int nucleo_settings_search_count(const char *q)
{
    g_setq_calls++;
    return !strcmp(q, "bri") ? 2 : !strcmp(q, "brig") ? 1 : 0;
}

// ---- tiny test framework ---------------------------------------------------------------------
static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_names[NUCLEO_APP_MAX][3][24];
static void reg_clear(void) { g_reg_n = 0; }
static void reg_add(const char *id, const char *name, const char *cat)
{
    if (g_reg_n >= NUCLEO_APP_MAX) return;
    nucleo_app_def_t a = {};
    snprintf(g_names[g_reg_n][0], 24, "%s", id);
    snprintf(g_names[g_reg_n][1], 24, "%s", name);
    snprintf(g_names[g_reg_n][2], 24, "%s", cat);
    a.id = g_names[g_reg_n][0]; a.name = g_names[g_reg_n][1]; a.category = g_names[g_reg_n][2];
    a.icon = id[0]; a.color = 0x1234;
    g_reg[g_reg_n++] = a;
}
static void reg_real(void)
{
    reg_clear();
    for (int i = 0; i < REAL_APP_N; i++) reg_add(REAL_APPS[i].id, REAL_APPS[i].name, REAL_APPS[i].cat);
}
static void pins_wipe(void) { remove(NUCLEO_CFG_MOUNT "/config/pins.txt"); remove(NUCLEO_CFG_MOUNT "/config/recents.txt"); }
static void fresh(void) { launcher_build_menu(); launcher_reset(); }

static int home_index_of(const char *id)
{
    const MenuNode *const *it = launcher_node()->items;
    for (int i = 0; it && it[i]; i++) if (!strcmp(it[i]->id, id)) return i;
    return -1;
}
static bool enter_category(const char *cat)
{
    launcher_reset();
    int i = home_index_of(cat);
    if (i < 0) return false;
    launcher_set_sel(i);
    return launcher_enter() == nullptr && launcher_depth() == 1;
}
static void type(const char *s) { while (*s) launcher_filter_push(*s++); }

// ---- tests -----------------------------------------------------------------------------------
static void t_capacity_real(void)
{
    printf("capacity: original %d apps, ADV %d apps, %d register call sites, NUCLEO_APP_MAX %d\n",
           REAL_ORIGINAL_N, REAL_ADV_N, REAL_CALL_SITES, NUCLEO_APP_MAX);
    CHECK(REAL_ORIGINAL_N <= NUCLEO_APP_MAX, "original Cardputer registers %d apps > %d", REAL_ORIGINAL_N, NUCLEO_APP_MAX);
    CHECK(REAL_ADV_N <= NUCLEO_APP_MAX, "Cardputer ADV registers %d apps > %d", REAL_ADV_N, NUCLEO_APP_MAX);
    // Every call site could run on some board (ADV registers the most): the registry must hold them all.
    CHECK(REAL_CALL_SITES <= NUCLEO_APP_MAX, "%d nucleo_app_register call sites > NUCLEO_APP_MAX %d: apps would vanish",
          REAL_CALL_SITES, NUCLEO_APP_MAX);
    CHECK(REAL_CALL_SITES + 8 <= NUCLEO_APP_MAX, "only %d free app slots left (keep >= 8 headroom)", NUCLEO_APP_MAX - REAL_CALL_SITES);
}

static void t_real_tree(void)
{
    pins_wipe(); reg_real(); fresh();
    CHECK(home_index_of("anima") == 0, "ANIMA must lead Home");
    // Every non-ANIMA app reachable exactly once through its category.
    int seen[REAL_APP_N] = { 0 };
    const MenuNode *const *home = launcher_node()->items;
    int cats = 0;
    for (int h = 0; home[h]; h++) {
        if (home[h]->kind != N_MENU) continue;
        cats++;
        CHECK(home[h]->items != nullptr, "category %s has no list", home[h]->id);
        for (int k = 0; home[h]->items && home[h]->items[k]; k++) {
            const MenuNode *a = home[h]->items[k];
            bool found = false;
            for (int i = 0; i < REAL_APP_N; i++)
                if (!strcmp(REAL_APPS[i].id, a->id)) {
                    seen[i]++; found = true;
                    CHECK(!strcmp(REAL_APPS[i].cat, home[h]->id), "%s filed under %s, declares %s", a->id, home[h]->id, REAL_APPS[i].cat);
                }
            CHECK(found, "unknown node %s in %s", a->id, home[h]->id);
        }
    }
    for (int i = 0; i < REAL_APP_N; i++) {
        if (!strcmp(REAL_APPS[i].id, "anima")) { CHECK(seen[i] == 0, "ANIMA duplicated in a category"); continue; }
        CHECK(seen[i] == 1, "app %s reachable %d times (want 1)", REAL_APPS[i].id, seen[i]);
    }
    printf("real tree: %d apps in %d categories\n", REAL_APP_N, cats);
}

static void t_capacity_synthetic(void)
{
    // Fill the registry to the brim across 10 categories: nothing dropped, every list intact.
    pins_wipe(); reg_clear();
    static const char *CATS[] = { "Office", "Tools", "Media", "Web OS", "Games", "Measure", "Connect", "Messaging", "System", "Security" };
    char id[16], nm[16];
    for (int i = 0; i < NUCLEO_APP_MAX; i++) {
        snprintf(id, sizeof id, "app%02d", i); snprintf(nm, sizeof nm, "App %02d", i);
        reg_add(id, nm, CATS[i % 10]);
    }
    fresh();
    int total = 0;
    const MenuNode *const *home = launcher_node()->items;
    for (int h = 0; home[h]; h++)
        for (int k = 0; home[h]->items && home[h]->items[k]; k++) {
            int idx = atoi(home[h]->items[k]->id + 3);
            CHECK(!strcmp(CATS[idx % 10], home[h]->id), "%s in wrong category %s", home[h]->items[k]->id, home[h]->id);
            total++;
        }
    CHECK(total == NUCLEO_APP_MAX, "full registry: %d of %d reachable", total, NUCLEO_APP_MAX);
    // Registration order is kept inside a category.
    CHECK(enter_category("Office"), "enter Office");
    CHECK(!strcmp(launcher_nth_visible(0)->id, "app00") && !strcmp(launcher_nth_visible(1)->id, "app10"), "order inside category");
}

static void t_navigation(void)
{
    pins_wipe(); reg_real(); fresh();
    int n = launcher_visible_count();
    CHECK(n >= 2, "Home rows");
    launcher_set_sel(-1);           CHECK(launcher_sel() == n - 1, "wrap up from 0 -> last");
    launcher_set_sel(n);            CHECK(launcher_sel() == 0, "wrap down past last -> 0");
    CHECK(!launcher_back(), "back at root with no filter is a no-op");
    CHECK(enter_category("System"), "enter System");
    const MenuNode *app = launcher_enter();
    CHECK(app && app->kind == N_APP, "Enter on an app returns it to the caller");
    CHECK(launcher_depth() == 1, "launching does not move the stack");
    CHECK(launcher_back() && launcher_depth() == 0, "back pops to Home");
}

static void t_filter(void)
{
    pins_wipe(); reg_real(); fresh();
    type("sett");
    CHECK(launcher_depth() == 0 && !strcmp(launcher_filter(), "sett"), "filter kept at Home");
    bool hit = false;
    for (int i = 0; i < launcher_visible_count(); i++) if (!strcmp(launcher_nth_visible(i)->id, "wifi")) hit = true;
    CHECK(hit, "Spotlight at Home finds Settings (id wifi) across categories");
    for (int i = 0; i < launcher_visible_count(); i++) CHECK(launcher_nth_visible(i)->kind == N_APP, "Spotlight lists apps only");
    // Any language: "wetter" (de) and "ajustes" (es) find their apps whatever the OS language is.
    static const char *const Q[][2] = { { "wetter", "weather" }, { "ajustes", "wifi" }, { "einst", "wifi" } };
    for (auto &qq : Q) {
        launcher_reset(); type(qq[0]);
        hit = false;
        for (int i = 0; i < launcher_visible_count(); i++) if (!strcmp(launcher_nth_visible(i)->id, qq[1])) hit = true;
        CHECK(hit, "Spotlight \"%s\" finds %s by its title in another language", qq[0], qq[1]);
    }
    // Case-insensitive, and never overflows the 15-char buffer.
    launcher_reset(); type("SETT");
    CHECK(!strcmp(launcher_filter(), "sett"), "filter lowercases");
    launcher_reset(); type("abcdefghijklmnopqrstuvwxyz");
    CHECK(strlen(launcher_filter()) == 15, "filter capped at 15 chars (got %d)", (int)strlen(launcher_filter()));
    CHECK(launcher_visible_count() == 0 && launcher_focused() == nullptr, "no match -> empty, no focus");
    launcher_filter_backspace();
    CHECK(strlen(launcher_filter()) == 14, "backspace");
    CHECK(launcher_back() && launcher_filter()[0] == 0 && launcher_depth() == 0, "back clears the filter first");
    // Inside a category the filter narrows only that category.
    CHECK(enter_category("System"), "enter System");
    int all = launcher_visible_count();
    type("zzz");
    CHECK(launcher_visible_count() == 0, "category filter");
    launcher_back();
    CHECK(launcher_visible_count() == all && launcher_depth() == 1, "back clears category filter, stays in category");
}

static void t_ranking(void)
{
    // Prefix beats word-start beats substring, whatever the registration order; ties keep that order.
    pins_wipe(); reg_clear();
    reg_add("recalc", "Recalc", "Tools");          // "calc" inside a word      -> tier 3
    reg_add("mycalc", "My Calc", "Tools");         // a word starts with it     -> tier 2
    reg_add("calc", "Calculator", "Office");       // the title starts with it  -> tier 1
    reg_add("calc2", "Calc Pro", "Office");        // also tier 1, registered later
    reg_add("other", "Clock", "Office");           // no match
    fresh(); type("calc");
    CHECK(launcher_visible_count() == 4, "4 matches (got %d)", launcher_visible_count());
    const char *want[4] = { "calc", "calc2", "mycalc", "recalc" };
    for (int i = 0; i < 4; i++)
        CHECK(launcher_nth_visible(i) && !strcmp(launcher_nth_visible(i)->id, want[i]), "rank %d: want %s got %s", i, want[i],
              launcher_nth_visible(i) ? launcher_nth_visible(i)->id : "(none)");
    CHECK(launcher_nth_visible(4) == nullptr && launcher_nth_visible(-1) == nullptr, "out of range");
    CHECK(!strcmp(launcher_focused()->id, "calc"), "focus lands on the best match");
    // A frame asks many times; the strings are matched once per query, not once per call.
    int calls = g_title_calls;
    for (int i = 0; i < 40; i++) { launcher_visible_count(); launcher_nth_visible(i % 4); launcher_focused(); }
    CHECK(g_title_calls == calls, "rank cached across calls (%d extra title lookups)", g_title_calls - calls);
    launcher_filter_backspace();
    launcher_visible_count();
    CHECK(g_title_calls > calls, "a new query re-ranks");
    // A title in another language ranks like the label: "wet" is a prefix of "Wetter" (de).
    reg_clear(); reg_add("weather", "Meteo", "Tools"); reg_add("wetsuit", "Big Wetsuit", "Tools"); fresh(); type("wet");
    CHECK(!strcmp(launcher_nth_visible(0)->id, "weather"), "prefix of a translated title ranks first");
}

static void t_settings_result(void)
{
    pins_wipe(); reg_real(); fresh();
    type("bri");
    int n = launcher_visible_count();
    CHECK(n >= 1, "results");
    const MenuNode *last = launcher_nth_visible(n - 1);
    CHECK(last && !strcmp(last->id, LAUNCHER_SETTINGS_SEARCH_ID), "the Settings result closes the Spotlight list");
    CHECK(launcher_settings_hits() == 2, "hit count exposed for the title (got %d)", launcher_settings_hits());
    launcher_set_sel(n - 1);
    CHECK(launcher_enter() == last, "Enter hands the Settings result to the run loop");
    // Scored once per query, not once per frame.
    int before = g_setq_calls;
    for (int i = 0; i < 50; i++) { launcher_visible_count(); launcher_nth_visible(0); }
    CHECK(g_setq_calls == before, "count cached per query (%d extra scans)", g_setq_calls - before);
    launcher_filter_push('g');
    CHECK(launcher_settings_hits() == 1, "a new query rescans");
    // Not offered when nothing matches, nor inside a category, and never pinnable.
    launcher_reset(); type("zz");
    for (int i = 0; i < launcher_visible_count(); i++) CHECK(strcmp(launcher_nth_visible(i)->id, LAUNCHER_SETTINGS_SEARCH_ID), "no match -> no tile");
    CHECK(enter_category("System"), "enter System"); launcher_filter_push('b'); launcher_filter_push('r'); launcher_filter_push('i');
    CHECK(launcher_depth() == 1, "typing inside a category stays there");
    for (int i = 0; i < launcher_visible_count(); i++) CHECK(strcmp(launcher_nth_visible(i)->id, LAUNCHER_SETTINGS_SEARCH_ID), "category filter -> no tile");
    launcher_toggle_pin(LAUNCHER_SETTINGS_SEARCH_ID);
    CHECK(!launcher_is_pinned(LAUNCHER_SETTINGS_SEARCH_ID), "the Settings result is never pinned");
}

static void t_recents(void)
{
    pins_wipe(); reg_real(); fresh();
    CHECK(home_index_of(LAUNCHER_RECENT_ID) < 0, "no Recent tile before the first launch");
    int office = home_index_of("Office");
    launcher_set_sel(office);
    launcher_note_launch("wifi");
    CHECK(home_index_of(LAUNCHER_RECENT_ID) == 1, "Recent tile right after ANIMA (at %d)", home_index_of(LAUNCHER_RECENT_ID));
    CHECK(launcher_focused() && !strcmp(launcher_focused()->id, "Office"), "focus stays on Office when the tile appears");
    // Newest first, deduplicated, capped at 5; system launches and ANIMA never enter it.
    const char *ids[6] = { "clock", "calc", "wifi", "files", "weather", "notepad" };
    for (const char *id : ids) launcher_note_launch(id);
    launcher_note_launch("screensaver"); launcher_note_launch("remote"); launcher_note_launch("updates"); launcher_note_launch("anima");
    CHECK(enter_category(LAUNCHER_RECENT_ID), "enter Recent");
    const char *want[5] = { "notepad", "weather", "files", "wifi", "calc" };
    CHECK(launcher_visible_count() == 5, "5 recents (got %d)", launcher_visible_count());
    for (int i = 0; i < 5; i++)
        CHECK(launcher_nth_visible(i) && !strcmp(launcher_nth_visible(i)->id, want[i]), "recent %d: want %s got %s", i, want[i],
              launcher_nth_visible(i) ? launcher_nth_visible(i)->id : "(none)");
    // Opening one from inside the tile moves it to the front and the focus follows it.
    launcher_set_sel(3);                                        // wifi
    launcher_note_launch("wifi");
    CHECK(launcher_depth() == 1 && launcher_sel() == 0 && !strcmp(launcher_focused()->id, "wifi"), "focus follows the reordered app");
    // Persisted: a reboot (fresh build) shows the same list; an unknown id is stored but never shown.
    launcher_note_launch("no-such-app");
    fresh();
    CHECK(enter_category(LAUNCHER_RECENT_ID), "Recent survives a reboot");
    CHECK(launcher_visible_count() == 4 && !strcmp(launcher_nth_visible(0)->id, "wifi"), "unknown id hidden, order kept");
    // Same newest app again: no change.
    launcher_note_launch("wifi");
    CHECK(!strcmp(launcher_nth_visible(0)->id, "wifi") && launcher_visible_count() == 4, "repeat launch is a no-op");
    // Return cursor into the Recent tile.
    launcher_set_sel(2); const char *f = launcher_focused()->id;
    launcher_capture_return(); g_reason = ESP_RST_SW; fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 1 && !strcmp(launcher_node()->id, LAUNCHER_RECENT_ID) && !strcmp(launcher_focused()->id, f),
          "Solo return lands back inside Recent");
    pins_wipe();
}

static void t_pins(void)
{
    pins_wipe(); reg_real(); fresh();
    int home0 = launcher_visible_count();
    launcher_toggle_pin("wifi");
    CHECK(launcher_is_pinned("wifi"), "pinned");
    CHECK(home_index_of("wifi") == 1, "pinned app rides right after ANIMA (at %d)", home_index_of("wifi"));
    CHECK(launcher_visible_count() == home0 + 1, "Home grew by one");
    // Persisted: a fresh build (reboot) reloads it.
    launcher_build_menu(); launcher_reset();
    CHECK(home_index_of("wifi") == 1, "pin survives a rebuild from the store");
    launcher_toggle_pin("anima");
    CHECK(!launcher_is_pinned("anima") && launcher_visible_count() == home0 + 1, "ANIMA is never pinned");
    launcher_toggle_pin("no-such-app");
    CHECK(launcher_visible_count() == home0 + 1, "unknown id pinned in store but not shown");
    launcher_toggle_pin("no-such-app");
    // Focus on the LAST Home row, then unpin: the list shrinks under the cursor -> clamped, never past the end.
    launcher_reset();
    launcher_set_sel(launcher_visible_count() - 1);
    launcher_toggle_pin("wifi");
    CHECK(!launcher_is_pinned("wifi"), "unpinned");
    CHECK(launcher_sel() < launcher_visible_count() && launcher_focused() != nullptr, "sel clamped after unpin (sel %d n %d)",
          launcher_sel(), launcher_visible_count());
    // Store full: the oldest pin is evicted.
    const char *ids[7] = { 0 };
    int k = 0;
    for (int i = 0; i < REAL_APP_N && k < 7; i++) if (strcmp(REAL_APPS[i].id, "anima")) ids[k++] = REAL_APPS[i].id;
    for (int i = 0; i < 7; i++) launcher_toggle_pin(ids[i]);
    CHECK(!launcher_is_pinned(ids[0]) && launcher_is_pinned(ids[6]), "7th pin evicts the oldest");
    CHECK(home_index_of(ids[1]) == 1 && home_index_of(ids[6]) == 6, "pin order = oldest first after ANIMA");
    pins_wipe();
}

static void t_return_cursor(void)
{
    pins_wipe(); reg_real(); fresh();
    CHECK(enter_category("Games"), "enter Games");
    type("o");
    int vis = launcher_visible_count();
    launcher_set_sel(vis > 1 ? 1 : 0);
    int sel = launcher_sel();
    const char *focus = launcher_focused() ? launcher_focused()->id : "";
    launcher_capture_return();
    // Warm reboot: tree rebuilt, reset to Home, snapshot re-applied.
    g_reason = ESP_RST_SW; fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 1 && !strcmp(launcher_node()->id, "Games"), "back in Games");
    CHECK(!strcmp(launcher_filter(), "o") && launcher_sel() == sel, "filter + row restored");
    CHECK(launcher_focused() && !strcmp(launcher_focused()->id, focus), "same app focused");
    // One-shot: a second apply (e.g. the next ordinary boot) does nothing.
    fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 0 && launcher_sel() == 0, "snapshot consumed once");
    // Cold boot / crash never teleports the cursor, even with a valid-looking snapshot.
    CHECK(enter_category("Games"), "enter Games");
    launcher_capture_return();
    g_reason = ESP_RST_POWERON; fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 0, "power-on ignores the snapshot");
    g_reason = ESP_RST_PANIC; launcher_capture_return(); fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 0, "panic ignores the snapshot");
    // Home Spotlight frame round-trips too, and an out-of-range row is clamped.
    g_reason = ESP_RST_SW; fresh(); type("a"); launcher_set_sel(launcher_visible_count() - 1);
    int want = launcher_sel();
    launcher_capture_return(); fresh(); launcher_apply_return();
    CHECK(launcher_depth() == 0 && !strcmp(launcher_filter(), "a") && launcher_sel() == want, "Spotlight frame restored");
}

int main(void)
{
    t_capacity_real();
    t_real_tree();
    t_capacity_synthetic();
    t_navigation();
    t_filter();
    t_ranking();
    t_settings_result();
    t_recents();
    t_pins();
    t_return_cursor();
    printf("launcher-model: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
