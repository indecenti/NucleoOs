// store_apps.cpp — apps installed from the store are apps of the system: in the launcher under their
// category (games in the Games gallery), with their own pixel icon, in search, Recent and pins.
//
// A NATIVE app (kind "native") is loaded from the SD card when it is opened (native_apps.cpp): the framework
// calls nucleo_store_app_prepare() on its registry slot first, which swaps the placeholder callbacks for the
// module's own. When the module is unloaded (another native app took the scratch region, or the store is
// replacing it) the slot gets its placeholder back before the code goes away. A module that fails to load
// leaves the placeholder, which says why.
//
// Each installed app (/sd/apps/<id>/.store-version, written by the store engine) becomes a registered app
// in the tail of the registry (nucleo_app_set_dynamic): its name in the current language, the launcher
// category its store category maps to, the category colour, and its 32x32 device-icon.txt. Opening one
// shows how to use it: a web app runs in a browser, so its address as a QR code and as text.
// RAM: one heap block for the installed apps (~200 B each), zero when none; rebuilt at boot and after the
// Store installs or removes something.
#include "nucleo_app.h"
#include "launcher_menu.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "nucleo_i18n.h"
#include "nucleo_board.h"
#include "store_engine.h"
extern const char *nucleo_setup_ip(void);
const nucleo_app_def_t *nucleo_native_load(const char *id);
const char *nucleo_native_error(void);
void nucleo_native_on_unload(void (*cb)(const char *id));
void nucleo_native_forget(const char *id);
}

struct StoreApp {
    char id[33];
    char name[25];
    char kind[8];
    const char *cat;            // launcher category ("Games", "Tools", ...)
    unsigned short color;
    bool has_icon;
    uint8_t icon[128];          // 32 x 32, 1 bit per pixel, MSB first
};

// store category -> launcher category + colour. "learn" opens a new launcher category, shown only when an
// app is in it (the launcher creates a category from its first app).
static const struct { const char *store, *launcher; unsigned short col; } MAP[] = {
    { "games", "Games", 0xFB4D }, { "tools", "Tools", 0x4D1F }, { "media", "Media", 0xC31F },
    { "office", "Office", 0x2E8B }, { "learn", "Learn", 0xFDA0 }, { "network", "Connect", 0x05DF },
    { "security", "Security", 0xF96B }, { "system", "System", 0x8C71 },
};

static StoreApp *s_apps, *s_old;
static nucleo_app_def_t *s_patched;     // the registry slot currently running a native module (nullptr = none)
static nucleo_app_def_t s_placeholder;  // what that slot held before
static unsigned s_old_gen;              // launcher tree generation when s_old was retired
static int s_n;

static int lang_idx(void) { return TR5("0", "1", "2", "3", "4")[0] - '0'; }

static const StoreApp *find(const char *id)
{
    for (int i = 0; id && i < s_n; i++) if (!strcmp(s_apps[i].id, id)) return &s_apps[i];
    return nullptr;
}

extern "C" const uint8_t *nucleo_store_app_icon(const char *id)
{
    const StoreApp *a = find(id);
    return a && a->has_icon ? a->icon : nullptr;
}

// .store-version: "code N", "version x", "kind web", "category games", "name it|en|es|fr|de".
static bool read_record(const char *id, StoreApp *a)
{
    char p[96], line[160];
    snprintf(p, sizeof p, NUCLEO_SD_MOUNT "/apps/%s/.store-version", id);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    snprintf(a->id, sizeof a->id, "%s", id);
    snprintf(a->name, sizeof a->name, "%s", id);
    snprintf(a->kind, sizeof a->kind, "web");
    a->cat = "Tools"; a->color = 0x4D1F;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "kind ", 5)) snprintf(a->kind, sizeof a->kind, "%s", line + 5);
        else if (!strncmp(line, "category ", 9)) {
            for (auto &m : MAP) if (!strcmp(m.store, line + 9)) { a->cat = m.launcher; a->color = m.col; }
        } else if (!strncmp(line, "name ", 5)) {
            const char *s = line + 5;
            for (int i = 0; i < lang_idx() && s; i++) { s = strchr(s, '|'); if (s) s++; }
            if (s && *s) { size_t n = strcspn(s, "|"); if (n > sizeof a->name - 1) n = sizeof a->name - 1; memcpy(a->name, s, n); a->name[n] = 0; }
        }
    }
    fclose(f);
    // The launcher icon: 32 rows of 32 '#' / '.'.
    snprintf(p, sizeof p, NUCLEO_SD_MOUNT "/apps/%s/device-icon.txt", id);
    memset(a->icon, 0, sizeof a->icon);
    a->has_icon = false;
    if ((f = fopen(p, "rb"))) {
        int y = 0;
        while (y < 32 && fgets(line, sizeof line, f)) {
            if (strspn(line, "#.") < 32) break;
            for (int x = 0; x < 32; x++) if (line[x] == '#') a->icon[y * 4 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
            y++;
        }
        a->has_icon = y == 32;
        fclose(f);
    }
    return true;
}

// ── opening a store app ───────────────────────────────────────────────────────
static void txt(const lgfx::IFont *f, int x, int y, const char *s, uint16_t c, textdatum_t dt = textdatum_t::top_left)
{
    d.setFont(f); d.setTextSize(1); d.setTextDatum(dt); d.setTextColor(c); d.drawString(s, x, y);
}

static void icon_at(const StoreApp *a, int x, int y, int px)
{
    d.fillRoundRect(x, y, 32 * px + 8, 32 * px + 8, 8, a->color);
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++)
            if (a->icon[r * 4 + c / 8] & (0x80 >> (c % 8))) d.fillRect(x + 4 + c * px, y + 4 + r * px, px, px, INK);
}

static void opener_enter(void)
{
    nucleo_app_set_hint(TR5("esc indietro", "esc back", "esc atras", "esc retour", "esc zurueck"));
    nucleo_app_request_draw();
}

static void opener_draw(void)
{
    const StoreApp *a = find(nucleo_app_active_id());
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (!a) return;
    const char *ip = nucleo_setup_ip();
    bool net = ip && ip[0];
    if (strcmp(a->kind, "web")) {                          // a kind this firmware cannot run yet
        icon_at(a, 6, top + 6, 1);
        txt(&fonts::Font4, 52, top + 10, a->name, FG);
        txt(&fonts::Font2, 8, top + 56, TR5("Richiede un firmware piu' nuovo.", "Needs a newer firmware.", "Requiere firmware nuevo.",
                                            "Firmware plus recent requis.", "Neuere Firmware noetig."), C_YELLOW);
        return;
    }
    if (!net) {
        icon_at(a, 6, top + 6, 1);
        txt(&fonts::Font4, 52, top + 10, a->name, FG);
        txt(&fonts::Font2, 8, top + 54, TR5("Si apre nel browser: connetti", "It opens in a browser: join", "Se abre en el navegador:", "S'ouvre dans le navigateur :", "Oeffnet im Browser: erst"), MUTED);
        txt(&fonts::Font2, 8, top + 72, TR5("prima il Wi-Fi (Impostazioni).", "a Wi-Fi first (Settings).", "conecta el Wi-Fi (Ajustes).", "connectez le Wi-Fi.", "WLAN verbinden."), MUTED);
        return;
    }
    // A web app: its address as a QR code (right) for a phone, and as text (left) for a PC.
    char url[96];
    snprintf(url, sizeof url, "http://%s/apps/%s/", ip, a->id);
    int q = h - 4, qx = 240 - q - 2;
    d.fillRect(qx, top + 2, q, q, 0xFFFF);
    d.qrcode(url, qx, top + 2, q, 1, true);
    int w = qx - 10;
    icon_at(a, 4, top + 4, 1);
    d.setFont(&fonts::Font2);
    char nm[26]; snprintf(nm, sizeof nm, "%s", a->name);
    while (nm[0] && d.textWidth(nm) > w - 46) nm[strlen(nm) - 1] = 0;
    txt(&fonts::Font2, 48, top + 10, nm, FG);
    txt(&fonts::Font0, 48, top + 28, TR5("app web", "web app", "app web", "app web", "Web-App"), DIM);
    txt(&fonts::Font0, 4, top + 48, TR5("Inquadra il QR col telefono", "Scan the QR with a phone", "Escanea el QR con el movil", "Scannez le QR", "QR mit dem Handy scannen"), MUTED);
    txt(&fonts::Font0, 4, top + 60, TR5("o apri sul PC:", "or open on a PC:", "o abre en el PC:", "ou ouvrez sur PC :", "oder am PC oeffnen:"), MUTED);
    char host[40];
    snprintf(host, sizeof host, "http://%s", ip);
    txt(&fonts::Font0, 4, top + 76, host, 0x4D1F);
    snprintf(host, sizeof host, "/apps/%s/", a->id);
    txt(&fonts::Font0, 4, top + 88, host, 0x4D1F);
    d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
}

static void opener_key(int, char) {}

// ── native apps ───────────────────────────────────────────────────────────────
// The placeholder a native app's slot holds while its module is not loaded: only seen when loading failed.
static void native_draw(void)
{
    const StoreApp *a = find(nucleo_app_active_id());
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (!a) return;
    icon_at(a, 6, top + 6, 1);
    txt(&fonts::Font4, 52, top + 10, a->name, FG);
    txt(&fonts::Font2, 8, top + 50, TR5("Non si avvia. Reinstallala dallo Store.", "It does not start. Reinstall it from the Store.",
                                        "No arranca. Reinstalala desde la Tienda.", "Ne demarre pas. Reinstallez-la.",
                                        "Startet nicht. Im Store neu installieren."), C_YELLOW);
    char e[48];
    snprintf(e, sizeof e, "%.46s", nucleo_native_error());
    txt(&fonts::Font0, 8, top + 72, e, MUTED);
}

static void native_unloaded(const char *id)
{
    if (s_patched && s_patched->id && !strcmp(s_patched->id, id)) *s_patched = s_placeholder;
    s_patched = nullptr;
}

extern "C" bool nucleo_store_app_is_native(const char *id)
{
    const StoreApp *a = find(id);
    return a && !strcmp(a->kind, "native");
}

// The launcher's quick menu (a long press on a tile): a store app's facts - its channel, the version installed and
// the catalog's description in the OS language (empty when the catalog is not on the card; the entry is read into a
// heap block freed at once). False = not an app the store installed.
extern "C" bool nucleo_store_app_facts(const char *id, char *version, int vn, char *kind, int kn, char *desc, int dn)
{
    const StoreApp *a = find(id);
    if (!a) return false;
    snprintf(kind, kn, "%s", a->kind);
    version[0] = 0; desc[0] = 0;
    char p[96], l[80];
    snprintf(p, sizeof p, NUCLEO_SD_MOUNT "/apps/%s/.store-version", id);
    FILE *f = fopen(p, "r");
    while (f && fgets(l, sizeof l, f))
        if (!strncmp(l, "version ", 8)) { snprintf(version, vn, "%s", l + 8); version[strcspn(version, "\r\n")] = 0; }
    if (f) fclose(f);
    st_app_t *e = (st_app_t *)malloc(sizeof(st_app_t));
    if (e) {
        if (st_find(NUCLEO_SD_MOUNT, !strcmp(a->kind, "native") ? ST_CH_NATIVE : ST_CH_WEB, id, e)) snprintf(desc, dn, "%s", e->desc[lang_idx()]);
        free(e);
    }
    return true;
}

extern "C" void nucleo_store_apps_sync(void);   // below

// The quick menu's Uninstall: what the Store does for it - a native module is unloaded first, the folder and (for a
// web app) its registry entry go, and the launcher follows. Never touches an app the store did not install.
extern "C" bool nucleo_store_app_uninstall(const char *id)
{
    if (!find(id)) return false;
    char keep[33];
    snprintf(keep, sizeof keep, "%s", id);                 // `id` may point into the table the sync rebuilds
    if (nucleo_store_app_is_native(keep)) nucleo_native_forget(keep);
    const bool ok = st_remove(NUCLEO_SD_MOUNT, keep) == ST_OK;
    nucleo_store_apps_sync();
    return ok;
}

// Called by the framework right before it opens a store app (in the boot that will run it: a game's Solo boot).
extern "C" void nucleo_store_app_prepare(nucleo_app_def_t *slot)
{
    if (!slot || !nucleo_store_app_is_native(slot->id)) return;
    if (s_patched == slot) return;                      // already the module's
    const nucleo_app_def_t *m = nucleo_native_load(slot->id);
    if (!m) return;                                     // the placeholder explains it
    s_placeholder = *slot;
    // The store owns the identity (id, name, category, icon, colour); the module brings the behaviour.
    slot->on_enter = m->on_enter; slot->on_key = m->on_key; slot->on_tick = m->on_tick; slot->on_draw = m->on_draw;
    slot->on_exit = m->on_exit; slot->exclusive_flags = m->exclusive_flags; slot->ram = m->ram;
    slot->on_draw_large = m->on_draw_large;
    // Nothing that points into the module goes where the launcher keeps it (its tree copies name / desc):
    // those stay the store's, so a later unload can never leave the launcher reading unmapped flash.
    s_patched = slot;
}

// ── sync ──────────────────────────────────────────────────────────────────────
extern "C" void nucleo_store_apps_sync(void)
{
    int room = nucleo_app_dynamic_room();
    DIR *dir = opendir(NUCLEO_SD_MOUNT "/apps");
    int cap = 0;
    struct dirent *e;
    if (dir) {
        while ((e = readdir(dir)) != NULL) if (e->d_name[0] != '.') cap++;
        rewinddir(dir);
    }
    if (cap > room) cap = room;
    StoreApp *apps = cap ? (StoreApp *)calloc(cap, sizeof *apps) : nullptr;
    int n = 0;
    while (dir && apps && n < cap && (e = readdir(dir)) != NULL)
        if (e->d_name[0] != '.' && strlen(e->d_name) <= 32 && read_record(e->d_name, &apps[n])) n++;
    if (dir) closedir(dir);
    nucleo_app_def_t *defs = n ? (nucleo_app_def_t *)calloc(n, sizeof *defs) : nullptr;
    for (int i = 0; i < n && defs; i++) {
        nucleo_app_def_t &d0 = defs[i];
        bool native = !strcmp(apps[i].kind, "native");
        d0.id = apps[i].id;
        d0.name = apps[i].name;
        d0.category = apps[i].cat;
        d0.desc = "";
        d0.icon = apps[i].name[0];
        d0.color = apps[i].color;
        d0.on_enter = opener_enter;
        d0.on_key = opener_key;
        d0.on_draw = native ? native_draw : opener_draw;
    }
    // Every slot is rewritten below: none runs a module any more (a resident module is simply re-attached
    // by the next nucleo_store_app_prepare, without reloading).
    nucleo_native_on_unload(native_unloaded);
    s_patched = nullptr;
    // The launcher tree holds pointers into the app-definition strings (= into s_apps). The old block is
    // safe to free only once the tree has been rebuilt since it was retired (its generation moved on);
    // otherwise keep it one more round. The newly replaced block (s_apps) becomes the next "old".
    if (s_old && launcher_tree_gen() != s_old_gen) { free(s_old); s_old = nullptr; }
    if (!s_old) { s_old = s_apps; s_old_gen = launcher_tree_gen(); }
    else { free(s_apps); }                        // rare: two syncs with no rebuild between -> drop the middle one
    s_apps = apps;
    s_n = defs ? n : 0;
    nucleo_app_set_dynamic(defs, s_n);
    free(defs);                                  // the registry copied the definitions
}
