// app_store.cpp — System > Store: browse, install, update and remove apps from the signed NucleoOS Cardputer
// store (indecenti/nucleoos-cardputer-store). The work is nucleo_store (engine + HTTPS on its own task); this
// file is the UI. Upgrade rules it respects (docs/app-store.md): it only ever writes /sd/apps/<id>/, never
// touches an app it did not install, and an install that fails leaves the previous version in place.
//
// Sections (TAB): New (newest first) / Games / Categories (a list of categories, OK opens one) / Mine
// (installed, updates first; the tab shows how many). OK opens an app's page: tile, name, author, version,
// size, category, description, permissions, the action (Install / Update), DEL removes. Type-ahead and
// 1-9 quick-select work in every list (app_ui_list).
// RAM: NX_NET_APP while open; the catalog stays on the SD card, only the rows the lists need are kept,
// in per-app RAM (zero when closed). During a download the 32 KB canvas is lent to the TLS handshake.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_large.h"      // two screens: the store grid + install card on the panel, the Cardputer as companion
#include "nucleo_touch.h"
#include <M5GFX.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
extern "C" {
#include "nucleo_i18n.h"
#include "nucleo_board.h"
#include "nucleo_exclusive.h"
#include "nucleo_store.h"
extern const char *nucleo_setup_ip(void);
void nucleo_store_apps_sync(void);
}

static const unsigned short ACC = 0x4D1F;
#define MAX_ROWS 96

enum { TAB_NEW, TAB_GAMES, TAB_CATS, TAB_MINE, NTABS };
enum { V_LIST, V_CAT, V_APP, V_CONFIRM_REMOVE, V_QR };
enum { S_NEW, S_UPDATE, S_OK, S_SYSTEM, S_NEWER_OS, S_UNSUPPORTED };

// The store's categories (docs/format.md), with their colour and name in the five languages.
struct Cat { const char *id; unsigned short col; const char *n[5]; };
static const Cat CATS[] = {
    { "games",    0xFB4D, { "Giochi", "Games", "Juegos", "Jeux", "Spiele" } },
    { "tools",    0x4D1F, { "Strumenti", "Tools", "Herramientas", "Outils", "Werkzeuge" } },
    { "media",    0xC31F, { "Multimedia", "Media", "Multimedia", "Multimedia", "Medien" } },
    { "office",   0x2E8B, { "Produttivita'", "Office", "Oficina", "Bureau", "Buero" } },
    { "learn",    0xFDA0, { "Imparare", "Learning", "Aprender", "Apprendre", "Lernen" } },
    { "network",  0x05DF, { "Rete", "Network", "Red", "Reseau", "Netzwerk" } },
    { "security", 0xF96B, { "Sicurezza", "Security", "Seguridad", "Securite", "Sicherheit" } },
    { "system",   0x8C71, { "Sistema", "System", "Sistema", "Systeme", "System" } },
};
#define NCATS ((int)(sizeof CATS / sizeof CATS[0]))

struct Row {
    char id[ST_ID_MAX + 1];
    char name[ST_NAME_MAX + 1];
    uint32_t size, updated;
    uint8_t state, cat;
};
struct Ram {
    Row rows[MAX_ROWS];
    int16_t vis[MAX_ROWS];      // the current list: indices into rows
    int16_t cat_count[NCATS];
    st_app_t app;               // the open app's full entry
    char queue[16][ST_ID_MAX + 1];   // "update all": the ids still to update
};
static Ram *M;
static const nucleo_app_ram_t APP_RAM[] = { { (void **)&M, sizeof(Ram) }, { nullptr, 0 } };

static int s_n, s_nvis, s_sel, s_tab, s_view, s_cat, s_cat_sel, s_updates, s_app_from, s_qn, s_qi, s_desc_off;
static int s_lg_scroll;   // first visible row on the panel list
static bool s_yes, s_busy_seen, s_refresh_pending, s_canvas_lent, s_direct_full;
static nst_status_t s_last_status;
static const char *s_note;            // one-line notice on the app page, cleared by the next key

static int lang_idx(void) { return TR5("0", "1", "2", "3", "4")[0] - '0'; }

static int cat_index(const char *id)
{
    for (int i = 0; i < NCATS; i++) if (!strcmp(CATS[i].id, id)) return i;
    return NCATS - 1;
}

static uint8_t state_of(const st_app_t &a)
{
    if (strcmp(a.kind, "web") && strcmp(a.kind, "data")) return S_UNSUPPORTED;
    bool system = false;
    uint32_t have = st_installed(NUCLEO_SD_MOUNT, a.id, &system);
    if (system) return S_SYSTEM;
    if (have && have >= a.code) return S_OK;
    if (st_ver_cmp(nucleo_store_os_version(), a.min_os) < 0) return S_NEWER_OS;
    return have ? S_UPDATE : S_NEW;
}

// ── lists ─────────────────────────────────────────────────────────────────────
static const Row *g_sort;
static int by_name(const void *a, const void *b)
{
    return strcasecmp(g_sort[*(const int16_t *)a].name, g_sort[*(const int16_t *)b].name);
}
static int by_new(const void *a, const void *b)
{
    const Row &x = g_sort[*(const int16_t *)a], &y = g_sort[*(const int16_t *)b];
    if (x.updated != y.updated) return x.updated < y.updated ? 1 : -1;
    return strcasecmp(x.name, y.name);
}
static int by_mine(const void *a, const void *b)   // updates first, then by name
{
    const Row &x = g_sort[*(const int16_t *)a], &y = g_sort[*(const int16_t *)b];
    if ((x.state == S_UPDATE) != (y.state == S_UPDATE)) return x.state == S_UPDATE ? -1 : 1;
    return strcasecmp(x.name, y.name);
}

// The list the current section shows.
static void build_list(void)
{
    s_nvis = 0;
    for (int i = 0; i < s_n; i++) {
        const Row &r = M->rows[i];
        if (r.state == S_UNSUPPORTED) continue;
        bool in = s_view == V_CAT ? r.cat == s_cat
                : s_tab == TAB_NEW ? true
                : s_tab == TAB_GAMES ? !strcmp(CATS[r.cat].id, "games")
                : s_tab == TAB_MINE ? (r.state == S_OK || r.state == S_UPDATE)
                : false;
        if (in) M->vis[s_nvis++] = (int16_t)i;
    }
    g_sort = M->rows;
    qsort(M->vis, s_nvis, sizeof M->vis[0], s_view != V_CAT && s_tab == TAB_NEW ? by_new
                                              : s_view != V_CAT && s_tab == TAB_MINE ? by_mine : by_name);
    if (s_sel >= s_nvis) s_sel = s_nvis ? s_nvis - 1 : 0;
}

static void cpy(char *dst, size_t cap, const char *src)   // both live in M: no snprintf (restrict)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memmove(dst, src, n);
    dst[n] = 0;
}

// The catalog lives on the SD card; only the rows the lists need are kept in RAM.
static void load(void)
{
    s_n = 0; s_updates = 0;
    memset(M->cat_count, 0, sizeof M->cat_count);
    FILE *f = st_catalog_open(NUCLEO_SD_MOUNT);
    if (f) {
        int li = lang_idx();
        while (s_n < MAX_ROWS && st_catalog_next(f, &M->app)) {
            Row &r = M->rows[s_n];
            r.state = state_of(M->app);
            if (r.state == S_UNSUPPORTED) continue;          // a kind this firmware cannot install: not shown
            cpy(r.id, sizeof r.id, M->app.id);
            cpy(r.name, sizeof r.name, M->app.name[li]);
            r.size = M->app.size;
            r.updated = M->app.updated;
            r.cat = (uint8_t)cat_index(M->app.category);
            M->cat_count[r.cat]++;
            if (r.state == S_UPDATE) s_updates++;
            s_n++;
        }
        fclose(f);
    }
    M->app.id[0] = 0;
    build_list();
}

// ── words ─────────────────────────────────────────────────────────────────────
static void kb(char *o, size_t n, uint32_t bytes)
{
    if (bytes < 1024 * 1024) snprintf(o, n, "%lu KB", (unsigned long)((bytes + 1023) / 1024));
    else snprintf(o, n, "%lu.%lu MB", (unsigned long)(bytes / 1048576), (unsigned long)(bytes % 1048576 * 10 / 1048576));
}

static const char *state_word(uint8_t s)
{
    switch (s) {
        case S_NEW:      return TR5("Installa", "Install", "Instalar", "Installer", "Installieren");
        case S_UPDATE:   return TR5("Aggiorna", "Update", "Actualizar", "Mettre a jour", "Aktualisieren");
        case S_OK:       return TR5("Installata", "Installed", "Instalada", "Installee", "Installiert");
        case S_SYSTEM:   return TR5("Di sistema", "Built-in", "Del sistema", "Integree", "Eingebaut");
        default:         return TR5("Serve OS nuovo", "Needs newer OS", "OS mas nuevo", "OS plus recent", "Neueres OS");
    }
}
// The same states, short enough for a list row's right column next to a long, focused name.
static const char *list_word(uint8_t s)
{
    switch (s) {
        case S_UPDATE:   return TR5("Aggiorna", "Update", "Actualiz.", "MAJ", "Update");
        case S_OK:       return TR5("Installata", "Installed", "Instalada", "Installee", "Vorhanden");
        case S_SYSTEM:   return TR5("Sistema", "Built-in", "Sistema", "Integree", "Eingebaut");
        case S_NEW:      return TR5("Nuova", "New", "Nueva", "Nouveau", "Neu");
        default:         return TR5("OS nuovo", "Newer OS", "OS nuevo", "OS recent", "Neues OS");
    }
}

static unsigned short state_col(uint8_t s)
{
    return s == S_UPDATE ? C_YELLOW : s == S_OK ? C_GREEN : s == S_NEW ? ACC : DIM;
}

// "today", "3 d", "2 wk", "5 mo" since `t` (the catalog's day-precision date).
static const char *ago(uint32_t t)
{
    static char b[16];
    time_t now = time(NULL);
    if (!t || now < 1700000000) return "";
    long days = ((long)now - (long)t) / 86400;
    if (days < 1) return TR5("oggi", "today", "hoy", "auj.", "heute");
    if (days < 14) snprintf(b, sizeof b, TR5("%ld g", "%ld d", "%ld d", "%ld j", "%ld T"), days);
    else if (days < 60) snprintf(b, sizeof b, TR5("%ld sett", "%ld wk", "%ld sem", "%ld sem", "%ld Wo"), days / 7);
    else snprintf(b, sizeof b, TR5("%ld mesi", "%ld mo", "%ld meses", "%ld mois", "%ld Mon"), days / 30);
    return b;
}

static const char *row_label(int i, void *) { return M->rows[M->vis[i]].name; }
static const char *row_right(int i, void *)
{
    const Row &r = M->rows[M->vis[i]];
    if (s_view != V_CAT && s_tab == TAB_NEW && r.state == S_NEW) return ago(r.updated);
    if (r.state == S_NEW) { static char b[16]; kb(b, sizeof b, r.size); return b; }
    return list_word(r.state);
}
static unsigned short row_color(int i, void *) { return state_col(M->rows[M->vis[i]].state); }

static const char *cat_label(int i, void *) { return CATS[i].n[lang_idx()]; }
static const char *cat_right(int i, void *) { static char b[8]; snprintf(b, sizeof b, "%d", M->cat_count[i]); return b; }
static unsigned short cat_color(int i, void *) { return M->cat_count[i] ? CATS[i].col : DIM; }

static void set_hint(void)
{
    if (nucleo_store_status() == NST_BUSY)
        nucleo_app_set_hint(TR5("attendi...", "please wait...", "espera...", "patientez...", "bitte warten..."));
    else if (s_view == V_APP) {
        // Only the keys that do something for THIS app.
        uint8_t stt = state_of(M->app);
        bool mine = st_installed(NUCLEO_SD_MOUNT, M->app.id, nullptr) != 0;
        if (stt == S_NEW)
            nucleo_app_set_hint(TR5("ok installa   esc indietro", "ok install   esc back", "ok instalar   esc atras", "ok installer   esc retour", "ok install.   esc zurueck"));
        else if (stt == S_UPDATE)
            nucleo_app_set_hint(TR5("ok aggiorna  del disinstalla", "ok update  del uninstall", "ok actualizar  del desinst.", "ok maj  del desinstaller", "ok update  del entfernen"));
        else if (mine)
            nucleo_app_set_hint(TR5("ok apri   del disinstalla   esc", "ok open   del uninstall   esc", "ok abrir   del desinstalar",
                                    "ok ouvrir   del desinstaller", "ok oeffnen   del entfernen"));
        else
            nucleo_app_set_hint(TR5("esc indietro", "esc back", "esc atras", "esc retour", "esc zurueck"));
    }
    else if (s_view == V_QR)
        nucleo_app_set_hint(TR5("un tasto qualsiasi: indietro", "any key: back", "cualquier tecla: atras", "une touche : retour", "beliebige Taste: zurueck"));
    else if (s_view == V_CAT)
        nucleo_app_set_hint(TR5("ok apri   esc categorie", "ok open   esc categories", "ok abrir   esc categorias",
                                "ok ouvrir   esc categories", "ok oeffnen   esc Kategorien"));
    else
        nucleo_app_set_hint(TR5("tab sezione  r aggiorna  ok apri", "tab section  r refresh  ok open", "tab seccion  r recargar  ok",
                                "tab section  r actualiser  ok", "tab Bereich  r neu laden  ok"));
}

static void go(int view)
{
    s_view = view;
    s_lg_scroll = 0;
    if (view == V_LIST || view == V_CAT) build_list();
    set_hint();
    nucleo_app_request_draw();
}

static void open_app(void)
{
    if (!s_nvis) return;
    s_app_from = s_view;                         // ESC returns there (a list or a category)
    s_desc_off = 0;
    if (st_find(NUCLEO_SD_MOUNT, M->rows[M->vis[s_sel]].id, &M->app)) go(V_APP);
}

// ── network operations ────────────────────────────────────────────────────────
// The TLS handshake needs a ~22 KB CONTIGUOUS block, and even with NX_NET_APP the heap is too fragmented
// for it (measured on the device: 55 KB free, largest block 21.5 KB). The 32 KB screen canvas is one block:
// lend it for the duration of a network operation, as the SD-content installer and Sniffer do, and draw
// direct meanwhile. It comes back as soon as the operation ends.
static void canvas_lend(void)
{
    if (s_canvas_lent) return;
    nucleo_app_set_direct_draw(true);
    nucleo_screen_release();
    s_canvas_lent = true;
    s_direct_full = true;                        // the first direct frame repaints everything once
}
static void canvas_return(void)
{
    if (!s_canvas_lent) return;
    nucleo_app_set_direct_draw(false);
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    s_canvas_lent = false;
    nucleo_app_force_repaint();
}

static void start(nst_op_t op, const char *id)
{
    if (op != NST_OP_REMOVE) canvas_lend();
    if (nucleo_store_start(op, id)) { s_busy_seen = true; set_hint(); }
    else canvas_return();
    nucleo_app_request_draw();
}

// ── drawing ───────────────────────────────────────────────────────────────────
static void txt(const lgfx::IFont *f, int x, int y, const char *s, uint16_t c, textdatum_t dt = textdatum_t::top_left)
{
    d.setFont(f); d.setTextSize(1); d.setTextDatum(dt); d.setTextColor(c); d.drawString(s, x, y);
}

static void txt_fit(const lgfx::IFont *f, int x, int y, int w, const char *s, uint16_t c)
{
    char b[64]; snprintf(b, sizeof b, "%s", s);
    d.setFont(f);
    while (b[0] && d.textWidth(b) > w) b[strlen(b) - 1] = 0;
    txt(f, x, y, b, c);
}

// Word-wrapped text in Font2: skips the first `skip` lines, draws at most `maxl`; returns the y after it.
// *more (optional) = text remains below.
static int wrap(int x, int y, int w, const char *s, int maxl, uint16_t c, int skip = 0, bool *more = nullptr)
{
    d.setFont(&fonts::Font2);
    char ln[64];
    int lines = 0;
    while (*s && lines < maxl + skip) {
        int n = 0, cut = 0;
        while (s[n] && n < (int)sizeof ln - 1) {
            ln[n] = s[n]; ln[n + 1] = 0;
            if (d.textWidth(ln) > w) break;
            if (s[n] == ' ') cut = n;
            n++;
        }
        if (!s[n] || !cut) cut = n;
        ln[cut] = 0;
        if (lines >= skip) txt(&fonts::Font2, x, y + (lines - skip) * 16, ln, c);
        s += cut;
        while (*s == ' ') s++;
        lines++;
    }
    if (more) *more = *s != 0;
    return y + (lines - skip) * 16;
}

// The app's tile: its category colour with its initial.
static void tile(int x, int y, int sz, const char *name, int cat)
{
    d.fillRoundRect(x, y, sz, sz, sz / 4, CATS[cat].col);
    char l[2] = { name[0] ? name[0] : '?', 0 };
    txt(&fonts::Font4, x + sz / 2, y + sz / 2 + 1, l, INK, textdatum_t::middle_center);
}

static void draw_progress(int top, int h)
{
    const st_state_t *st = nucleo_store_state();
    const char *what = nucleo_store_op() == NST_OP_REFRESH ? TR5("Aggiorno il catalogo", "Updating the catalog", "Actualizando catalogo", "Mise a jour du catalogue", "Katalog wird geladen")
                     : nucleo_store_op() == NST_OP_REMOVE ? TR5("Rimuovo", "Removing", "Quitando", "Suppression", "Entferne")
                     : st->phase == ST_PH_INSTALL ? TR5("Installo", "Installing", "Instalando", "Installation", "Installiere")
                     : TR5("Scarico e verifico", "Downloading, verifying", "Descargando y verificando", "Telechargement, verification", "Lade und pruefe");
    d.fillRoundRect(10, top + 20, 220, 78, 10, LINE);
    txt(&fonts::Font2, 120, top + 28, what, FG, textdatum_t::top_center);
    int pct = st->pct < 0 ? 0 : st->pct > 100 ? 100 : st->pct;
    d.fillRoundRect(22, top + 52, 196, 10, 5, BG);
    if (pct) d.fillRoundRect(22, top + 52, 196 * pct / 100 < 10 ? 10 : 196 * pct / 100, 10, 5, ACC);
    char s[48];
    if (st->files_total) snprintf(s, sizeof s, "%d/%d  %.22s", st->files_done, st->files_total, st->cur);
    else snprintf(s, sizeof s, "%d%%", pct);
    txt(&fonts::Font2, 120, top + 70, s, MUTED, textdatum_t::top_center);
}

static void empty_state(int y, const char *msg, uint16_t col)
{
    wrap(14, y + 16, 212, msg, 4, col);
}

static void draw_tabs_and_list(int top, int h)
{
    static const char *const T[5][NTABS] = {
        { "Novita'", "Giochi", "Categorie", "Mie" }, { "New", "Games", "Categories", "Mine" },
        { "Nuevas", "Juegos", "Categorias", "Mias" }, { "Neuf", "Jeux", "Categ.", "Mes apps" },
        { "Neu", "Spiele", "Kategorien", "Meine" } };
    const char *names[NTABS];
    static char mine[16];
    for (int i = 0; i < NTABS; i++) names[i] = T[lang_idx()][i];
    if (s_updates) { snprintf(mine, sizeof mine, "%s %d", T[lang_idx()][TAB_MINE], s_updates); names[TAB_MINE] = mine; }
    int y = app_ui_tabs(top, names, NTABS, s_tab, ACC), lh = h - (y - top);
    if (s_tab == TAB_CATS) {
        app_ui_list(y, lh, NCATS, s_cat_sel, cat_label, cat_right, cat_color, nullptr);
        return;
    }
    if (s_tab == TAB_MINE && s_updates) {               // one key updates them all
        char b[48];
        snprintf(b, sizeof b, TR5("%d da aggiornare  -  u: tutte", "%d to update  -  u: all", "%d por actualizar  -  u: todas",
                                  "%d MAJ  -  u : tout", "%d Updates  -  u: alle"), s_updates);
        d.fillRoundRect(4, y + 1, 232, 16, 6, C_YELLOW);
        txt_fit(&fonts::Font2, 10, y + 1, 222, b, INK);
        y += 19; lh -= 19;
        d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
    }
    if (s_nvis) { app_ui_list(y, lh, s_nvis, s_sel, row_label, row_right, row_color, nullptr); return; }
    if (!s_n)
        empty_state(y, nucleo_store_status() == NST_FAILED ? nucleo_store_error()
                       : TR5("Catalogo non ancora scaricato: serve il Wi-Fi (r per riprovare).",
                             "Catalog not downloaded yet: it needs Wi-Fi (r to retry).",
                             "Catalogo aun no descargado: necesita Wi-Fi (r para reintentar).",
                             "Catalogue pas encore telecharge : il faut le Wi-Fi (r pour reessayer).",
                             "Katalog noch nicht geladen: WLAN noetig (r fuer neu)."),
                    nucleo_store_status() == NST_FAILED ? C_RED : MUTED);
    else if (s_tab == TAB_MINE)
        empty_state(y, TR5("Nessuna app installata dallo store. Le trovi in Novita' e Categorie.",
                           "No apps installed from the store yet. Find them in New and Categories.",
                           "Aun no hay apps de la tienda. Buscalas en Nuevas y Categorias.",
                           "Aucune app de la boutique. Voyez Nouveau et Categories.",
                           "Noch keine Store-Apps. Siehe Neu und Kategorien."), MUTED);
    else empty_state(y, TR5("Niente qui, per ora.", "Nothing here yet.", "Nada aqui por ahora.", "Rien ici pour l'instant.", "Noch nichts hier."), MUTED);
}

static void draw_category(int top, int h)
{
    // Header: the category's colour bar and name, then its apps.
    d.fillRoundRect(4, top + 2, 232, 18, 6, CATS[s_cat].col);
    char t[40];
    snprintf(t, sizeof t, "%s  (%d)", CATS[s_cat].n[lang_idx()], s_nvis);
    txt(&fonts::Font2, 12, top + 3, t, INK);
    int y = top + 22;
    d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
    if (s_nvis) app_ui_list(y, h - 22, s_nvis, s_sel, row_label, row_right, row_color, nullptr);
    else empty_state(y, TR5("Nessuna app in questa categoria.", "No apps in this category.", "Sin apps en esta categoria.", "Aucune app ici.", "Keine Apps hier."), MUTED);
}

static void draw_app(int top, int h)
{
    const st_app_t &a = M->app;
    int li = lang_idx(), cat = cat_index(a.category);
    uint8_t stt = state_of(a);
    // Header: tile, name, author.
    tile(6, top + 4, 30, a.name[li], cat);
    txt_fit(&fonts::Font4, 42, top + 2, 194, a.name[li], FG);
    char s[72], sz[16];
    kb(sz, sizeof sz, a.size);
    // Facts on one line: version, size, category.
    snprintf(s, sizeof s, "v%s  %s  %s", a.version, sz, CATS[cat].n[li]);
    txt_fit(&fonts::Font0, 42, top + 27, 194, s, MUTED);
    d.drawFastHLine(6, top + 38, 228, LINE);
    bool more = false;
    int y = wrap(6, top + 42, 228, a.desc[li], a.perms[0] ? 2 : 3, FG, s_desc_off, &more);
    if (more || s_desc_off) {                            // there is more text: say which way it scrolls
        txt(&fonts::Font0, 234, top + 42, s_desc_off ? "^" : "", DIM, textdatum_t::top_right);
        if (more) txt(&fonts::Font0, 234, y - 9, "v", DIM, textdatum_t::top_right);
    }
    if (s_note) txt_fit(&fonts::Font0, 6, y + 2, 228, s_note, C_RED);
    else if (a.perms[0]) {
        snprintf(s, sizeof s, "%s %s", TR5("Usa:", "Uses:", "Usa:", "Utilise :", "Nutzt:"), a.perms);
        txt_fit(&fonts::Font0, 6, y + 2, 228, s, C_YELLOW);
    }
    // The action, and what else can be done.
    int by = top + h - 22;
    bool system = false;
    bool mine = st_installed(NUCLEO_SD_MOUNT, a.id, &system) != 0;
    // Main action on OK: Install / Update, or Open once it is installed (a web app opens on a phone or PC).
    const char *act = stt == S_OK ? TR5("Apri", "Open", "Abrir", "Ouvrir", "Oeffnen") : state_word(stt);
    unsigned short col = stt == S_OK ? C_GREEN : state_col(stt);
    d.fillRoundRect(6, by, 110, 20, 8, col);
    txt(&fonts::Font2, 61, by + 10, act, INK, textdatum_t::middle_center);
    if (mine) {                                          // DEL: uninstall, always visible for the store's apps
        d.drawRoundRect(122, by, 112, 20, 8, C_RED);
        txt(&fonts::Font2, 178, by + 10, TR5("Disinstalla", "Uninstall", "Desinstalar", "Desinstaller", "Entfernen"), C_RED, textdatum_t::middle_center);
    } else if (a.author[0]) {                            // otherwise who made it
        snprintf(s, sizeof s, "%s %s", TR5("di", "by", "de", "par", "von"), a.author);
        d.setFont(&fonts::Font0);
        while (strlen(s) > 3 && d.textWidth(s) > 106) s[strlen(s) - 1] = 0;
        txt(&fonts::Font0, 234, by + 6, s, DIM, textdatum_t::top_right);
    }
}

// An installed web app opens in a browser: show its address as a QR code for the phone.
static void draw_qr(int top, int h)
{
    char url[96];
    snprintf(url, sizeof url, "http://%s/apps/%s/", nucleo_setup_ip() ? nucleo_setup_ip() : "", M->app.id);
    int q = h - 4;
    d.fillRect(2, top + 2, q, q, 0xFFFF);
    d.qrcode(url, 2, top + 2, q, 1, true);
    int x = q + 10, w = 240 - x - 4;
    txt_fit(&fonts::Font2, x, top + 6, w, M->app.name[lang_idx()], FG);
    wrap(x, top + 26, w, TR5("Inquadralo col telefono per aprire l'app nel browser.", "Scan it with your phone to open the app in the browser.",
                             "Escanealo con el movil para abrir la app.", "Scannez-le pour ouvrir l'app.", "Mit dem Handy scannen, um die App zu oeffnen."), 4, MUTED);
    // The address on two lines: host, then path (Font0 is 6 px wide; the column is ~100 px).
    char host[40];
    snprintf(host, sizeof host, "http://%s", nucleo_setup_ip() ? nucleo_setup_ip() : "");
    txt_fit(&fonts::Font0, x, top + h - 22, w, host, ACC);
    txt_fit(&fonts::Font0, x, top + h - 11, w, url + strlen(host), ACC);
}

// ===== two screens ==============================================================================================
// The panel carries the store: the sections as a tab bar, the apps as a touch list, and an app's page as a real
// install card (big tile, name, facts, description, the Install/Update/Open + Uninstall buttons). Categories, the
// remove confirm and the open-on-phone QR get their own panel layouts. The Cardputer becomes the companion: the
// section / selection / app state and the key map; during a download, the progress.
static const char *const LG_TABS[5][NTABS] = {
    { "Novita'", "Giochi", "Categorie", "Mie" }, { "New", "Games", "Categories", "Mine" },
    { "Nuevas", "Juegos", "Categorias", "Mias" }, { "Neuf", "Jeux", "Categ.", "Mes apps" },
    { "Neu", "Spiele", "Kategorien", "Meine" } };

static void lg_tile(int x, int y, int sz, const char *name, int cat)
{
    d.fillRoundRect(x, y, sz, sz, sz / 4, CATS[cat].col);
    char l[2] = { name && name[0] ? name[0] : '?', 0 };
    lg::font(3); d.setTextDatum(middle_center); d.setTextColor(INK, CATS[cat].col);
    d.drawString(l, x + sz / 2, y + sz / 2 + 1); d.setTextDatum(top_left);
}

static void lg_progress(void)
{
    const int W = d.width(), H = d.height();
    d.fillRect(0, 0, W, H, BG);
    const st_state_t *st = nucleo_store_state();
    const char *what = nucleo_store_op() == NST_OP_REFRESH ? TR5("Aggiorno il catalogo", "Updating the catalog", "Actualizando catalogo", "Mise a jour du catalogue", "Katalog wird geladen")
                     : nucleo_store_op() == NST_OP_REMOVE ? TR5("Rimuovo", "Removing", "Quitando", "Suppression", "Entferne")
                     : st->phase == ST_PH_INSTALL ? TR5("Installo", "Installing", "Instalando", "Installation", "Installiere")
                     : TR5("Scarico e verifico", "Downloading, verifying", "Descargando y verificando", "Telechargement, verification", "Lade und pruefe");
    lg::font(2); d.setTextDatum(top_center); d.setTextColor(FG, BG); d.drawString(what, W / 2, H / 2 - 44); d.setTextDatum(top_left);
    const int pct = st->pct < 0 ? 0 : st->pct > 100 ? 100 : st->pct;
    lg::bar(30, H / 2, W - 60, 16, pct, ACC);
    char s[48];
    if (st->files_total) snprintf(s, sizeof s, "%d/%d  %.24s", st->files_done, st->files_total, st->cur);
    else snprintf(s, sizeof s, "%d%%", pct);
    lg::font(0); d.setTextDatum(top_center); d.setTextColor(MUTED, BG); d.drawString(s, W / 2, H / 2 + 30); d.setTextDatum(top_left);
}

static void lg_app(void)
{
    const int W = d.width(), H = d.height();
    const st_app_t &a = M->app;
    const int li = lang_idx(), cat = cat_index(a.category);
    const uint8_t stt = state_of(a);
    const int y = lg::header(a.name[li], nullptr, ACC, NK_BACK);
    d.fillRect(0, lg::HEAD, W, H - lg::HEAD, BG);
    lg_tile(14, y + 8, 56, a.name[li], cat);
    lg::font(2); d.setTextColor(FG, BG); char nm[48]; lg::fit(nm, sizeof nm, a.name[li], W - 92); d.drawString(nm, 82, y + 10);
    char sz[16]; kb(sz, sizeof sz, a.size);
    char facts[80]; snprintf(facts, sizeof facts, "v%s  %s  %s", a.version, sz, CATS[cat].n[li]);
    lg::font(0); d.setTextColor(MUTED, BG); char ff[80]; lg::fit(ff, sizeof ff, facts, W - 92); d.drawString(ff, 82, y + 42);
    if (a.author[0]) { char au[48]; snprintf(au, sizeof au, "%s %s", TR5("di", "by", "de", "par", "von"), a.author); char af[48]; lg::fit(af, sizeof af, au, W - 92); d.setTextColor(DIM, BG); d.drawString(af, 82, y + 58); }
    int dy = y + 80;
    d.drawFastHLine(14, dy, W - 28, LINE); dy += 8;
    const int descBot = a.perms[0] ? H - 74 : H - 62;
    const int maxl = (descBot - dy) / 16; bool more = false;
    int ey = wrap(16, dy, W - 32, a.desc[li], maxl > 0 ? maxl : 1, FG, s_desc_off, &more);
    if (s_desc_off) { lg::font(0); d.setTextColor(DIM, BG); d.setTextDatum(top_right); d.drawString("^", W - 16, dy); d.setTextDatum(top_left); }
    if (more)       { lg::font(0); d.setTextColor(DIM, BG); d.setTextDatum(top_right); d.drawString("v", W - 16, ey - 12); d.setTextDatum(top_left); }
    nucleo_touch_add_scroll(14, dy, W - 28, descBot - dy, 16, NK_UP, 0, NK_DOWN, 0);
    if (a.perms[0]) { char p[72]; snprintf(p, sizeof p, "%s %s", TR5("Usa:", "Uses:", "Usa:", "Utilise :", "Nutzt:"), a.perms); lg::font(0); d.setTextColor(C_YELLOW, BG); char pf[72]; lg::fit(pf, sizeof pf, p, W - 28); d.drawString(pf, 16, H - 70); }
    bool system = false;
    const bool mine = st_installed(NUCLEO_SD_MOUNT, a.id, &system) != 0;
    const char *act = stt == S_OK ? TR5("Apri", "Open", "Abrir", "Ouvrir", "Oeffnen") : state_word(stt);
    const unsigned short col = stt == S_OK ? C_GREEN : state_col(stt);
    if (mine) {
        const int bw = (W - 28 - 8) / 2;
        lg::button(14, H - 52, bw, 44, act, col, INK, NK_ENTER, 0);
        lg::button(22 + bw, H - 52, bw, 44, TR5("Disinstalla", "Uninstall", "Desinstalar", "Desinstaller", "Entfernen"), C_RED, INK, NK_DEL, 0);
    } else {
        const int bw = W > 260 ? 220 : W - 40;
        lg::button((W - bw) / 2, H - 52, bw, 44, act, col, INK, NK_ENTER, 0);
    }
    if (s_view == V_CONFIRM_REMOVE) {
        const int cw = W - 56, chh = 118, cx = 28, cy = (H - chh) / 2;
        lg::panel(cx, cy, cw, chh, INK); d.drawRoundRect(cx, cy, cw, chh, 8, C_RED);
        lg::font(1); d.setTextColor(C_RED, INK); d.setTextDatum(top_center); d.drawString(TR5("Disinstallare?", "Uninstall?", "Desinstalar?", "Desinstaller ?", "Entfernen?"), cx + cw / 2, cy + 12);
        lg::font(0); d.setTextColor(FG, INK); char nmc[48]; lg::fit(nmc, sizeof nmc, a.name[li], cw - 24); d.drawString(nmc, cx + cw / 2, cy + 40); d.setTextDatum(top_left);
        const int bw = (cw - 36) / 2;
        lg::button(cx + 12, cy + chh - 50, bw, 40, "No", s_yes ? LINE : ACC, s_yes ? FG : INK, NK_CHAR, 'n');
        lg::button(cx + 24 + bw, cy + chh - 50, bw, 40, TR5("Disinstalla", "Uninstall", "Desinstalar", "Desinstaller", "Entfernen"), s_yes ? C_RED : LINE, s_yes ? INK : FG, NK_CHAR, 'y');
    }
}

static void lg_qr(void)
{
    const int W = d.width(), H = d.height();
    const int y = lg::header(M->app.name[lang_idx()], nullptr, ACC, NK_BACK);
    d.fillRect(0, y, W, H - y, BG);
    char url[96]; snprintf(url, sizeof url, "http://%s/apps/%s/", nucleo_setup_ip() ? nucleo_setup_ip() : "", M->app.id);
    int q = H - y - 20; if (q > 184) q = 184;
    d.fillRect(14, y + 10, q, q, 0xFFFF);
    d.qrcode(url, 14, y + 10, q, 1, true);
    const int tx = 14 + q + 16, tw = W - tx - 10;
    lg::font(1); d.setTextColor(FG, BG); char t[48]; lg::fit(t, sizeof t, M->app.name[lang_idx()], tw); d.drawString(t, tx, y + 14);
    lg::font(0); d.setTextColor(MUTED, BG);
    lg::wrap(TR5("Inquadralo col telefono per aprire l'app nel browser.", "Scan it with your phone to open the app in the browser.",
                 "Escanealo con el movil para abrir la app.", "Scannez-le pour ouvrir l'app.", "Mit dem Handy scannen, um die App zu oeffnen."), tx, y + 42, tw, 17, 5);
    lg::font(0); d.setTextColor(ACC, BG); char uf[48]; lg::fit(uf, sizeof uf, url, tw); d.drawString(uf, tx, H - 24);
    nucleo_touch_add(0, y, W, H - y, NK_BACK, 0);   // any tap steps back
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    if (nucleo_store_status() == NST_BUSY) { lg_progress(); nucleo_touch_commit(); return; }
    if (s_view == V_APP || s_view == V_CONFIRM_REMOVE) { lg_app(); nucleo_touch_commit(); return; }
    if (s_view == V_QR) { lg_qr(); nucleo_touch_commit(); return; }
    if (s_view == V_CAT) {
        char cnt[12]; snprintf(cnt, sizeof cnt, "%d", s_nvis);
        const int y = lg::header(CATS[s_cat].n[lang_idx()], cnt, CATS[s_cat].col, NK_BACK);
        if (s_nvis) lg::text_list(0, y + 2, W, H - y - 4, s_nvis, s_sel, &s_lg_scroll, row_label, row_right, row_color, nullptr, ACC);
        else lg::message(0, y, W, H - y, TR5("Nessuna app", "No apps", "Sin apps", "Aucune app", "Keine Apps"), nullptr, MUTED);
        nucleo_touch_commit();
        return;
    }
    // V_LIST: section tabs + list
    const int y0 = lg::header("Store", nullptr, ACC);
    const int li = lang_idx();
    const char *names[NTABS]; static char mine[20];
    for (int i = 0; i < NTABS; i++) names[i] = LG_TABS[li][i];
    if (s_updates) { snprintf(mine, sizeof mine, "%s %d", LG_TABS[li][TAB_MINE], s_updates); names[TAB_MINE] = mine; }
    int y = lg::tabs(y0, names, NTABS, s_tab, ACC, nullptr);
    if (s_tab == TAB_CATS) {
        lg::text_list(0, y + 2, W, H - y - 4, NCATS, s_cat_sel, &s_lg_scroll, cat_label, cat_right, cat_color, nullptr, ACC);
        nucleo_touch_commit();
        return;
    }
    int listy = y + 2, listh = H - listy - 4;
    if (s_tab == TAB_MINE && s_updates) {
        char b[40]; snprintf(b, sizeof b, TR5("Aggiorna tutte (%d)", "Update all (%d)", "Actualizar todas (%d)", "Tout mettre a jour (%d)", "Alle aktualisieren (%d)"), s_updates);
        lg::button(8, y + 2, W - 16, 32, b, C_YELLOW, INK, NK_CHAR, 'u');
        listy = y + 38; listh = H - listy - 4;
    }
    if (s_nvis) lg::text_list(0, listy, W, listh, s_nvis, s_sel, &s_lg_scroll, row_label, row_right, row_color, nullptr, ACC);
    else {
        const char *msg = nucleo_store_status() == NST_FAILED ? nucleo_store_error()
                        : !s_n ? TR5("Catalogo non ancora scaricato: serve il Wi-Fi.", "Catalog not downloaded yet: it needs Wi-Fi.", "Catalogo aun no descargado: necesita Wi-Fi.", "Catalogue pas encore telecharge : il faut le Wi-Fi.", "Katalog noch nicht geladen: WLAN noetig.")
                        : s_tab == TAB_MINE ? TR5("Nessuna app dallo store. Guarda in Novita'.", "No store apps yet. Look in New.", "Sin apps de la tienda. Mira en Nuevas.", "Aucune app. Voyez Nouveau.", "Noch keine Store-Apps. Siehe Neu.")
                        : TR5("Niente qui, per ora.", "Nothing here yet.", "Nada aqui por ahora.", "Rien ici pour l'instant.", "Noch nichts hier.");
        lg::message(0, listy, W, listh, msg, nullptr, nucleo_store_status() == NST_FAILED ? C_RED : MUTED);
    }
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    const int y = lg::comp_head("STORE", ACC);
    if (nucleo_store_status() == NST_BUSY) {
        const st_state_t *st = nucleo_store_state();
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
        d.drawString(nucleo_store_op() == NST_OP_REMOVE ? TR5("Rimuovo...", "Removing...", "Quitando...", "Suppression...", "Entferne...")
                   : nucleo_store_op() == NST_OP_REFRESH ? TR5("Catalogo...", "Catalog...", "Catalogo...", "Catalogue...", "Katalog...")
                   : TR5("Installo...", "Installing...", "Instalando...", "Installation...", "Installiere..."), 8, y + 4);
        const int pct = st->pct < 0 ? 0 : st->pct;
        lg::bar(8, y + 30, 224, 6, pct, ACC);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        char s[44]; if (st->files_total) snprintf(s, sizeof s, "%d/%d  %.18s", st->files_done, st->files_total, st->cur); else snprintf(s, sizeof s, "%d%%", pct);
        d.drawString(s, 8, y + 44);
        lg::comp_keys(TR5("attendi...", "please wait...", "espera...", "patientez...", "bitte warten..."));
        return;
    }
    const int li = lang_idx();
    if (s_view == V_APP || s_view == V_CONFIRM_REMOVE) {
        const st_app_t &a = M->app; const uint8_t stt = state_of(a);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG); char nm[40]; snprintf(nm, sizeof nm, "%.26s", a.name[li]); d.drawString(nm, 8, y + 4);
        char sz[16]; kb(sz, sizeof sz, a.size);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); char f[40]; snprintf(f, sizeof f, "v%s  %s  %s", a.version, sz, CATS[cat_index(a.category)].n[li]); d.drawString(f, 8, y + 26);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(state_col(stt), BG); d.drawString(state_word(stt), 8, y + 42);
        bool system = false; const bool mine = st_installed(NUCLEO_SD_MOUNT, a.id, &system) != 0;
        lg::comp_keys(s_view == V_CONFIRM_REMOVE ? TR5("Y/N conferma", "Y/N confirm", "Y/N confirma", "Y/N confirme", "Y/N bestaet.")
                    : stt == S_OK ? TR5("INVIO apri  DEL disinstalla", "ENTER open  DEL uninstall", "INTRO abrir  DEL desinst.", "ENTREE ouvrir  DEL desinst.", "ENTER oeffnen  DEL entfernen")
                    : mine ? TR5("INVIO aggiorna  DEL disinstalla", "ENTER update  DEL uninstall", "INTRO actualiz  DEL desinst.", "ENTREE maj  DEL desinst.", "ENTER update  DEL entfernen")
                    : TR5("INVIO installa  ESC indietro", "ENTER install  ESC back", "INTRO instalar  ESC atras", "ENTREE installer  ESC retour", "ENTER install.  ESC zurueck"));
        return;
    }
    if (s_view == V_QR) {
        const st_app_t &a = M->app;
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG); char nm[40]; snprintf(nm, sizeof nm, "%.26s", a.name[li]); d.drawString(nm, 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        lg::wrap(TR5("Inquadra il QR sul pannello per aprirla nel browser.", "Scan the QR on the panel to open it in a browser.", "Escanea el QR del panel.", "Scannez le QR du panneau.", "QR am Panel scannen."), 8, y + 24, 224, 14, 3);
        lg::comp_keys(TR5("un tasto: indietro", "any key: back", "una tecla: atras", "une touche : retour", "Taste: zurueck"));
        return;
    }
    if (s_view == V_CAT) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(CATS[s_cat].col, BG); d.drawString(CATS[s_cat].n[li], 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); char c[32]; snprintf(c, sizeof c, TR5("%d app", "%d apps", "%d apps", "%d apps", "%d Apps"), s_nvis); d.drawString(c, 8, y + 26);
        if (s_nvis) { d.setTextColor(FG, BG); char b[40]; snprintf(b, sizeof b, "-> %.22s", row_label(s_sel, nullptr)); d.drawString(b, 8, y + 40); }
        lg::comp_keys(TR5("INVIO apri  ESC categorie", "ENTER open  ESC categories", "INTRO abrir  ESC categ.", "ENTREE ouvrir  ESC categ.", "ENTER oeffnen  ESC Kateg."));
        return;
    }
    // V_LIST
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(ACC, BG);
    char tb[24]; snprintf(tb, sizeof tb, "%s", LG_TABS[li][s_tab]); d.drawString(tb, 8, y + 4);
    d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
    if (s_tab == TAB_CATS) { char c[24]; snprintf(c, sizeof c, TR5("%d categorie", "%d categories", "%d categorias", "%d categories", "%d Kategorien"), NCATS); d.drawString(c, 8, y + 26); d.setTextColor(FG, BG); char b[40]; snprintf(b, sizeof b, "-> %.22s", cat_label(s_cat_sel, nullptr)); d.drawString(b, 8, y + 40); }
    else {
        char c[32]; snprintf(c, sizeof c, TR5("%d app  -  %d da aggiornare", "%d apps  -  %d to update", "%d apps  -  %d por actualizar", "%d apps  -  %d a jour", "%d Apps  -  %d Updates"), s_nvis, s_updates); d.drawString(c, 8, y + 26);
        if (s_nvis) { d.setTextColor(FG, BG); char b[40]; snprintf(b, sizeof b, "-> %.22s", row_label(s_sel, nullptr)); d.drawString(b, 8, y + 40); }
    }
    lg::comp_keys(TR5("TAB sezione  INVIO apri  r ricarica", "TAB section  ENTER open  r reload", "TAB seccion  INTRO abrir  r", "TAB section  ENTREE ouvrir  r", "TAB Bereich  ENTER oeffnen  r"));
}

static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    if (s_canvas_lent && !s_direct_full && nucleo_store_status() == NST_BUSY) {   // direct: only the card changes
        draw_progress(top, h);
        d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
        return;
    }
    s_direct_full = false;
    d.fillRect(0, top, 240, h, BG);
    if (s_view == V_QR) { draw_qr(top, h); d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left); return; }
    if (s_view == V_APP || s_view == V_CONFIRM_REMOVE) draw_app(top, h);
    else if (s_view == V_CAT) draw_category(top, h);
    else draw_tabs_and_list(top, h);
    if (nucleo_store_status() == NST_BUSY) draw_progress(top, h);
    else if (s_view == V_CONFIRM_REMOVE)
        app_ui_confirm(TR5("Disinstallare?", "Uninstall?", "Desinstalar?", "Desinstaller ?", "Entfernen?"), M->app.name[lang_idx()], s_yes);
    else if (nucleo_store_status() == NST_FAILED && s_n) {
        // What failed, and how old the catalog on screen is.
        struct stat st;
        const char *age = stat(NUCLEO_SD_MOUNT "/apps/.store/index.txt", &st) == 0 ? ago((uint32_t)st.st_mtime) : "";
        d.fillRect(0, top + h - 27, 240, 27, BG);
        d.drawFastHLine(0, top + h - 27, 240, LINE);
        txt_fit(&fonts::Font2, 4, top + h - 26, 232, nucleo_store_error(), C_RED);
        char e[64];
        if (age[0]) snprintf(e, sizeof e, TR5("Ultimo catalogo: %s.  r = riprova", "Last catalog: %s.  r = retry",
                                              "Ultimo catalogo: %s.  r = reintentar", "Dernier catalogue : %s.  r", "Letzter Katalog: %s.  r = erneut"), age);
        else snprintf(e, sizeof e, "%s", TR5("r per riprovare", "r to retry", "r para reintentar", "r pour reessayer", "r = erneut"));
        txt_fit(&fonts::Font0, 4, top + h - 9, 232, e, MUTED);
    }
    d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
}

// ── input ─────────────────────────────────────────────────────────────────────
// The store task runs on its own: watch it, and refresh the rows when it ends.
static bool poll(void)
{
    if (s_refresh_pending) { s_refresh_pending = false; start(NST_OP_REFRESH, nullptr); return true; }
    nst_status_t st = nucleo_store_status();
    if (st == NST_BUSY) {
        s_last_status = NST_BUSY;                        // so the DONE after it is always seen, even after a DONE
        static int64_t last;
        int64_t now = esp_timer_get_time();
        if (now - last < 150000) return false;
        last = now;
        return true;
    }
    if (st != s_last_status) {
        s_last_status = st;
        if (s_busy_seen && (st == NST_DONE || st == NST_FAILED)) {
            s_busy_seen = false;
            canvas_return();
            if (st == NST_DONE && nucleo_store_op() != NST_OP_REFRESH) nucleo_store_apps_sync();   // the launcher follows
            if (s_qn && st == NST_DONE && s_qi < s_qn) {   // "update all": the next one
                start(NST_OP_INSTALL, M->queue[s_qi++]);
                return true;
            }
            s_qn = s_qi = 0;
            char keep[ST_ID_MAX + 1];
            snprintf(keep, sizeof keep, "%s", M->app.id);
            load();
            if (s_view == V_APP && keep[0] && !st_find(NUCLEO_SD_MOUNT, keep, &M->app)) s_view = V_LIST;
            set_hint();
        }
        return true;
    }
    return false;
}

// Open an installed app. A web app runs in a browser: its address, as a QR code and as text, for a phone or a
// PC on the same network. Without Wi-Fi there is no address to give: say so instead.
static void open_installed(void)
{
    if (!strcmp(M->app.kind, "web") && nucleo_setup_ip() && nucleo_setup_ip()[0]) go(V_QR);
    else { s_note = TR5("Serve il Wi-Fi per aprirla", "Needs Wi-Fi to open", "Necesita Wi-Fi", "Il faut le Wi-Fi", "WLAN noetig"); nucleo_app_request_draw(); }
}

static void on_key(int key, char ch)
{
    if (nucleo_store_status() == NST_BUSY) return;
    if (s_view == V_CONFIRM_REMOVE) {
        int r = app_ui_confirm_key(key, ch, &s_yes);
        if (r == 1) { s_view = V_APP; start(NST_OP_REMOVE, M->app.id); }
        else if (r == 0) s_view = V_APP;
        nucleo_app_request_draw();
        return;
    }
    if (s_view == V_QR) { go(V_APP); return; }
    if (s_view == V_APP) {
        s_note = nullptr;
        uint8_t stt = state_of(M->app);
        if (key == NK_UP && s_desc_off) { s_desc_off--; nucleo_app_request_draw(); return; }
        if (key == NK_DOWN && s_desc_off < 6) { s_desc_off++; nucleo_app_request_draw(); return; }
        if (key == NK_CHAR && (ch == 'q' || ch == 'Q') && stt == S_OK && !strcmp(M->app.kind, "web")
            && nucleo_setup_ip() && nucleo_setup_ip()[0]) { go(V_QR); return; }
        if (key == NK_ENTER && (stt == S_NEW || stt == S_UPDATE)) start(NST_OP_INSTALL, M->app.id);
        else if (key == NK_ENTER && stt == S_OK) open_installed();
        else if (key == NK_DEL && st_installed(NUCLEO_SD_MOUNT, M->app.id, nullptr)) { s_yes = false; s_view = V_CONFIRM_REMOVE; }
        nucleo_app_request_draw();
        return;
    }
    if (nucleo_touch_is_row(key, ch)) {                   // a tapped list / category row on the panel
        if (s_view == V_LIST && s_tab == TAB_CATS) {
            if (lg::row_tap(key, ch, s_lg_scroll, NCATS, &s_cat_sel) == 2) { s_cat = s_cat_sel; s_sel = 0; go(V_CAT); }
            else nucleo_app_request_draw();
            return;
        }
        if (lg::row_tap(key, ch, s_lg_scroll, s_nvis, &s_sel) == 2) open_app();
        else nucleo_app_request_draw();
        return;
    }
    if (key == NK_CHAR && (ch == 'r' || ch == 'R')) { start(NST_OP_REFRESH, nullptr); return; }
    if (key == NK_CHAR && (ch == 'u' || ch == 'U') && s_view == V_LIST && s_tab == TAB_MINE && s_updates) {
        s_qn = s_qi = 0;
        for (int i = 0; i < s_n && s_qn < 16; i++)
            if (M->rows[i].state == S_UPDATE) cpy(M->queue[s_qn++], sizeof M->queue[0], M->rows[i].id);
        if (s_qn) start(NST_OP_INSTALL, M->queue[s_qi++]);
        return;
    }
    if (s_view == V_LIST && s_tab == TAB_CATS) {
        if (key == NK_ENTER) { s_cat = s_cat_sel; s_sel = 0; go(V_CAT); return; }
        app_ui_list_key(key, ch, &s_cat_sel, NCATS, cat_label, nullptr);
    } else if (key == NK_ENTER) open_app();
    else app_ui_list_key(key, ch, &s_sel, s_nvis, row_label, nullptr);
    nucleo_app_request_draw();
}

static void on_tab(void)
{
    if (nucleo_store_status() == NST_BUSY || s_view == V_APP || s_view == V_CONFIRM_REMOVE) return;
    s_tab = (s_tab + 1) % NTABS;
    s_sel = 0;
    go(V_LIST);
}

static bool on_back(int key)
{
    (void)key;
    if (nucleo_store_status() == NST_BUSY) return true;            // never leave mid-install
    if (s_view == V_CONFIRM_REMOVE || s_view == V_QR) { go(V_APP); return true; }
    if (s_view == V_APP) { go(s_app_from); return true; }
    if (s_view == V_CAT) { go(V_LIST); return true; }
    return false;
}

static void on_enter(void)
{
    s_view = V_LIST; s_tab = TAB_NEW; s_sel = 0; s_cat = 0; s_cat_sel = 0; s_busy_seen = false;
    load();                                                        // the last catalog, instantly, from the SD
    s_last_status = nucleo_store_status();
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    set_hint();
    s_refresh_pending = nucleo_setup_ip() && nucleo_setup_ip()[0];   // then the fresh one, from the first poll:
    nucleo_app_request_draw();                                     // by then NX_NET_APP has freed its RAM
}

static void on_exit(void) { canvas_return(); }

extern "C" void nucleo_register_store(void)
{
    static const nucleo_app_def_t app = {
        "store", "Store", "System", "Install apps from the signed NucleoOS store",
        'S', 0x4D1F, on_enter, on_key, nullptr, on_draw, on_exit, NX_NET_APP, APP_RAM, draw_large
    };
    nucleo_app_register(&app);
}
