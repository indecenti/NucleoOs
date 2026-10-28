// GameFront — see gamefront.h. The Games gallery: systems, then titles, on a HyperSpin-style wheel.
//
// One painter for both screens, composed with lg::frame in RGB565 (exact colours, no flicker, no RAM of its own):
//   panel 320x240   header (Back) · search · picture + title on the left, a 7-title wheel on the right
//   Cardputer       picture + caption + title on the left, search and a 5-title wheel on the right
// With the panel attached the Cardputer shows the companion: the search, the selection, its blurb and the keys.
//
// RAM: the launcher runs on a fragmented ~18 KB heap. The native list is a few dozen shorts; a console's shelf is
// one block (~4.3 KB, MAXR names) while its titles are shown, freed on the way out. Pictures never live in RAM: each
// is converted once into a raw .pic file (the 32 KB shared canvas is the scratch for that) and its rows are read
// from the card into the strip being composed.
#include "gamefront.h"
#include "gameshots.h"        // GF_DIR: where the covers are
#include "nucleo_app.h"
#include "app_gfx.h"          // `d` = the draw target (a strip of lg::frame while composing)
#include "app_large.h"
#include "launcher_theme.h"
#include "launcher_render.h"  // the launcher's vector icons: the posters of games without a cover
#include "nucleo_kbd.h"
#include "nucleo_touch.h"
#include "nucleo_extdisp.h"   // two screens: the companion on the Cardputer
#include "gbemu_art.h"        // the consoles' photos (public domain, deflated)
#include "miniz.h"            // tinfl, in the chip's ROM
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
extern "C" {
#include "nucleo_board.h"
#include "nucleo_i18n.h"
}

int                     nucleo_app_count(void);   // the app registry (nucleo_app.cpp)
const nucleo_app_def_t *nucleo_app_at(int i);
extern "C" const char  *launcher_app_localized_name(const char *id);
extern "C" bool         nucleo_store_app_is_native(const char *id);   // store_apps.cpp: installed from the Store

#define ROMS     NUCLEO_SD_MOUNT "/data/ROMs"
#define CACHE    GF_DIR "/.cache"
#define NAT_MAX  32          // native games held (built in + Store)
#define MAXR     120         // cartridges held at once: typing narrows a bigger shelf, as in the emulators
#define POOL     4096        // their file names, packed end to end
#define NAMEMAX  64
#define PATHMAX  160
#define FILT     16

// ── what is in the gallery ──────────────────────────────────────────────────────────────────────────
// Native games: their play modes and a one-line blurb (shown beside the picture and on the companion).
enum { M1P = 1, MCPU = 2, M2P = 4, MLAN = 8, MCOOP = 16 };
struct Meta { const char *id; unsigned char modes; const char *tag[5]; };
static const Meta META[] = {
    { "reactor",  M1P,          { "Bilancia potenza e raffreddamento. Evita il meltdown.", "Balance power and cooling. Avoid the meltdown.",
                                  "Equilibra potencia y refrigeracion. Evita la fusion.", "Equilibre puissance et refroidissement. Evite la fusion.",
                                  "Leistung und Kuehlung im Gleichgewicht. Keine Kernschmelze." } },
    { "stelle",   M1P,          { "Commercia e combatti fra le stelle.", "Trade and fight among the stars.", "Comercia y lucha entre las estrellas.",
                                  "Commerce et combats parmi les etoiles.", "Handeln und kaempfen zwischen den Sternen." } },
    { "giardino", M1P,          { "Unisci le piante e fai crescere l'albero d'oro.", "Merge plants and grow the golden tree.",
                                  "Une plantas y cultiva el arbol de oro.", "Fusionne les plantes, fais pousser l'arbre d'or.",
                                  "Pflanzen vereinen und den Goldbaum ziehen." } },
    { "slots",    M1P,          { "Tira la leva e insegui il jackpot.", "Pull the lever and chase the jackpot.", "Tira de la palanca y persigue el bote.",
                                  "Tire le levier et vise le jackpot.", "Zieh den Hebel und jag den Jackpot." } },
    { "poker",    M1P,          { "5 carte: tieni le buone, punta alla scala reale.", "5 cards: hold the good ones, go for the royal flush.",
                                  "5 cartas: guarda las buenas, busca la escalera real.", "5 cartes : garde les bonnes, vise la quinte royale.",
                                  "5 Karten: behalte die guten, ziel auf den Royal Flush." } },
    { "pinball",  M1P,          { "Flipper verticale: respingenti, spinner, punti.", "Vertical pinball: bumpers, spinners, points.",
                                  "Pinball vertical: bumpers, spinners, puntos.", "Flipper vertical : bumpers, spinners, points.",
                                  "Senkrechter Flipper: Bumper, Spinner, Punkte." } },
    { "pong",     MCPU | MLAN,  { "1v1 in rete fra due Cardputer, o contro la CPU.", "1v1 between two Cardputers, or against the CPU.",
                                  "1v1 entre dos Cardputer, o contra la CPU.", "1v1 entre deux Cardputer, ou contre le CPU.",
                                  "1v1 zwischen zwei Cardputern oder gegen die CPU." } },
    { "tanks",    MCPU | M2P,   { "Artiglieria a turni: terreno distruttibile, vento, 29 armi.", "Turn-based artillery: destructible terrain, wind, 29 weapons.",
                                  "Artilleria por turnos: terreno destructible, viento, 29 armas.", "Artillerie au tour par tour : terrain destructible, vent, 29 armes.",
                                  "Rundenartillerie: zerstoerbares Gelaende, Wind, 29 Waffen." } },
    { "brawler",  M1P | MCOOP,  { "Picchiaduro noir a scorrimento, anche in co-op.", "Side-scrolling noir brawler, co-op too.",
                                  "Beat'em up noir de scroll lateral, tambien cooperativo.", "Beat'em up noir a defilement, aussi en coop.",
                                  "Noir-Pruegelspiel mit Scrolling, auch im Koop." } },
    { "dice",     M1P,          { "Tiro di dadi: scuoti il device o premi invio.", "Roll the dice: shake the device or press enter.",
                                  "Tira los dados: agita el aparato o pulsa enter.", "Lance les des : secoue l'appareil ou appuie sur entree.",
                                  "Wuerfeln: Geraet schuetteln oder Enter druecken." } },
    { "snake",    MCPU | MLAN,  { "Serpente 1v1 in rete (ESP-NOW), o contro l'IA.", "1v1 snake over ESP-NOW, or against the AI.",
                                  "Serpiente 1v1 por ESP-NOW, o contra la IA.", "Serpent 1v1 en ESP-NOW, ou contre l'IA.",
                                  "Snake 1v1 ueber ESP-NOW oder gegen die KI." } },
    { "tankd",    MCPU | MLAN,  { "Arena 1v1 in rete: shop conteso, 4 carri, upgrade.", "1v1 arena: contested shop, 4 tanks, upgrades.",
                                  "Arena 1v1: tienda disputada, 4 tanques, mejoras.", "Arene 1v1 : boutique disputee, 4 chars, ameliorations.",
                                  "1v1-Arena: umkaempfter Shop, 4 Panzer, Upgrades." } },
    { "yahtzee",  M2P | MCPU,   { "Yahtzee a turni, 1-4 giocatori + CPU, dadi 3D.", "Turn-based Yahtzee, 1-4 players + CPU, 3D dice.",
                                  "Yahtzee por turnos, 1-4 jugadores + CPU, dados 3D.", "Yahtzee au tour par tour, 1-4 joueurs + CPU, des 3D.",
                                  "Yahtzee in Runden, 1-4 Spieler + CPU, 3D-Wuerfel." } },
    { "orde",     M1P,          { "Sopravvivi alle orde: le armi sparano da sole.", "Survive the hordes: your weapons fire on their own.",
                                  "Sobrevive a las hordas: tus armas disparan solas.", "Survis aux hordes : tes armes tirent toutes seules.",
                                  "Ueberlebe die Horden: deine Waffen feuern von selbst." } },
    { "cardler",  M1P,          { "Mini RPG: esplora, parla con gli NPC, raccogli oro.", "Mini RPG: explore, talk to NPCs, collect gold.",
                                  "Mini RPG: explora, habla con los NPC, junta oro.", "Mini RPG : explore, parle aux PNJ, ramasse de l'or.",
                                  "Mini-RPG: erkunden, mit NPCs reden, Gold sammeln." } },
};

// The consoles, in the order the gallery lists them. Two share an emulator each (one shelf for both in the
// emulator itself), but here every console is its own system with its own folder.
struct Art { const uint8_t *z; uint16_t n; uint8_t w, h; };
struct System {
    const char *key, *dir, *app, *name, *maker;
    unsigned short col;
    bool sega;                 // .gg / .sms / .zip; else .gb / .gbc
    Art art;
};
static const System SYS[] = {
    { "gb",  ROMS "/gb",  "gbemu", "Game Boy",       "Nintendo, 1989", 0x8FF3, false, { ART_GB_Z,  sizeof ART_GB_Z,  ART_GB_W,  ART_GB_H } },
    { "gbc", ROMS "/gbc", "gbemu", "Game Boy Color", "Nintendo, 1998", 0xC5F5, false, { ART_GB_Z,  sizeof ART_GB_Z,  ART_GB_W,  ART_GB_H } },
    { "gg",  ROMS "/gg",  "ggemu", "Game Gear",      "Sega, 1990",     0x5D7F, true,  { ART_GG_Z,  sizeof ART_GG_Z,  ART_GG_W,  ART_GG_H } },
    { "sms", ROMS "/sms", "ggemu", "Master System",  "Sega, 1986",     0xF96B, true,  { ART_SMS_Z, sizeof ART_SMS_Z, ART_SMS_W, ART_SMS_H } },
};
#define NSYS ((int)(sizeof SYS / sizeof SYS[0]))
#define NATIVE (-1)            // the group of the native games
#define ACC_NATIVE 0xFB4D      // the Games category's colour

// ── state ───────────────────────────────────────────────────────────────────────────────────────────
static int     s_level;                    // 0 = systems, 1 = the titles of s_group
static int     s_group = NATIVE;
static int     s_grp[1 + NSYS], s_ngrp;    // the systems shown (NATIVE or a SYS index), in order
static int     s_grp_n[1 + NSYS];          // ...how many titles each has (MAXR + 1 = "more")
static int16_t s_nat[NAT_MAX];             // native games (registry indices), by title
static uint8_t s_nat_cover[NAT_MAX];       // ...which cover files GameShots has for each (CV_* bits)
static int     s_nnat;
static int16_t s_ix[NAT_MAX];              // the list shown: level 0 = s_grp slots, native = s_nat slots
static int     s_n, s_total, s_sel;        // titles shown, titles that exist (a shelf may hold fewer), selection
static int     s_sel0;                     // the systems' selection while a system is open
static char    s_filter[FILT + 1];
static int     s_flen;
static char    s_launch[PATHMAX + 24];     // the cartridge GF_LAUNCH asks for ("" = an app of its own)
static bool    s_restore;                  // gamefront_open() comes back on the RTC snapshot (gamefront_take_return)
static bool    s_opened;                   // opened at least once since boot

struct Shelf { uint16_t off[MAXR]; char pool[POOL]; int used; };
static Shelf  *s_sh;                       // the open console's cartridges (heap; only while level 1 shows one)

static const char *rom_name(int i) { return s_sh->pool + s_sh->off[i]; }
static bool on_roms(void) { return s_level == 1 && s_group != NATIVE; }
static unsigned short acc_of(int group) { return group == NATIVE ? (unsigned short)ACC_NATIVE : SYS[group].col; }
static unsigned short acc_now(void) { return acc_of(s_level == 0 ? (s_n ? s_grp[s_ix[s_sel]] : NATIVE) : s_group); }

// ── names ───────────────────────────────────────────────────────────────────────────────────────────
static const char *native_title(int reg)
{
    const nucleo_app_def_t *a = nucleo_app_at(reg);
    const char *n = launcher_app_localized_name(a->id);
    return n ? n : a->name;
}
static const char *group_title(int g)
{
    return g == NATIVE ? TR5("Giochi nativi", "Native games", "Juegos nativos", "Jeux natifs", "Native Spiele") : SYS[g].name;
}
// A cartridge's title without the extension and the region / dump tags every set carries.
static void pretty(const char *file, char *out, size_t n)
{
    char t[PATHMAX];
    snprintf(t, sizeof t, "%s", file);
    char *p = strrchr(t, '.'); if (p) *p = '\0';
    static const char *const CUT[] = { " (", " [", "-(", "_(" };
    for (const char *c : CUT) { p = strstr(t, c); if (p && p > t) *p = '\0'; }
    snprintf(out, n, "%s", t);
}
static const char *title_of(int i)
{
    static char t[NAMEMAX];
    if (s_level == 0) return group_title(s_grp[s_ix[i]]);
    if (s_group == NATIVE) return native_title(s_nat[s_ix[i]]);
    pretty(rom_name(i), t, sizeof t);
    return t;
}
static const Meta *meta_of(const char *id)
{
    for (const Meta &m : META) if (!strcmp(m.id, id)) return &m;
    return nullptr;
}
static bool contains(const char *hay, const char *needle)
{
    if (!needle[0]) return true;
    const size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) if (!strncasecmp(p, needle, nl)) return true;
    return false;
}

// ── the lists ───────────────────────────────────────────────────────────────────────────────────────
static int find_app(const char *id)
{
    for (int i = 0, n = nucleo_app_count(); i < n; i++) if (!strcmp(nucleo_app_at(i)->id, id)) return i;
    return -1;
}
static bool is_rom(const System &s, const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot || n[0] == '.') return false;
    if (s.sega) return !strcasecmp(dot, ".gg") || !strcasecmp(dot, ".sms") || !strcasecmp(dot, ".zip");
    return !strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc");
}
// Walk a console's folder: count the cartridges matching the filter (stops past `cap`), keep them when `keep`.
// A folder of thousands is walked over SPI: the watchdog is fed on the way.
static int walk(const System &s, int cap, bool keep)
{
    DIR *dp = opendir(s.dir);
    if (!dp) return 0;
    int n = 0;
    unsigned seen = 0;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL && n <= cap) {
        if ((++seen & 63) == 0) esp_task_wdt_reset();
        if (!is_rom(s, e->d_name) || !contains(e->d_name, s_filter)) continue;
        n++;
        const size_t len = strlen(e->d_name) + 1;
        if (keep && s_n < MAXR && len <= PATHMAX && s_sh->used + (int)len <= POOL) {
            memcpy(s_sh->pool + s_sh->used, e->d_name, len);
            s_sh->off[s_n++] = (uint16_t)s_sh->used;
            s_sh->used += (int)len;
        }
    }
    closedir(dp);
    return n;
}
static int cmp_rom(const void *a, const void *b)
{ return strcasecmp(s_sh->pool + *(const uint16_t *)a, s_sh->pool + *(const uint16_t *)b); }
static int cmp_nat(const void *a, const void *b)
{ return strcasecmp(native_title(*(const int16_t *)a), native_title(*(const int16_t *)b)); }

static void shelf_free(void) { free(s_sh); s_sh = nullptr; }

// The list the current level shows, under the current filter. Keeps the selection on the same title if it stays.
static void refilter(void)
{
    if (s_level == 0) {
        s_n = 0;
        for (int k = 0; k < s_ngrp; k++) if (contains(group_title(s_grp[k]), s_filter)) s_ix[s_n++] = (int16_t)k;
        s_total = s_n;
    } else if (s_group == NATIVE) {
        s_n = 0;
        for (int k = 0; k < s_nnat; k++) if (contains(native_title(s_nat[k]), s_filter)) s_ix[s_n++] = (int16_t)k;
        s_total = s_n;
    } else {
        if (!s_sh) s_sh = (Shelf *)malloc(sizeof(Shelf));
        s_n = 0;
        if (!s_sh) { s_total = 0; return; }
        s_sh->used = 0;
        s_total = walk(SYS[s_group], 100000, true);
        if (s_n > 1) qsort(s_sh->off, s_n, sizeof s_sh->off[0], cmp_rom);
    }
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    if (s_sel < 0) s_sel = 0;
}

// What GameShots holds, read ONCE per opening (one listing): a missed fopen in the big /data folder costs ~0.5 s.
enum { CV_PNG = 1, CV_JPG = 2, CV_BMP = 4 };
static void covers_scan(void)
{
    memset(s_nat_cover, 0, sizeof s_nat_cover);
    DIR *dp = opendir(GF_DIR);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || dot == e->d_name) continue;
        const uint8_t bit = !strcasecmp(dot, ".png") ? CV_PNG : !strcasecmp(dot, ".jpg") ? CV_JPG : !strcasecmp(dot, ".bmp") ? CV_BMP : 0;
        if (!bit) continue;
        for (int k = 0; k < s_nnat; k++) {
            const char *id = nucleo_app_at(s_nat[k])->id;
            if (strlen(id) == (size_t)(dot - e->d_name) && !strncasecmp(id, e->d_name, dot - e->d_name)) { s_nat_cover[k] |= bit; break; }
        }
    }
    closedir(dp);
}

// The systems: the native games when there are any, then each console whose emulator is installed and whose folder
// has cartridges. An installed emulator with no cartridge anywhere still shows its first console, which then says
// where the cartridges go.
static void systems_build(void)
{
    s_nnat = 0;
    for (int i = 0, n = nucleo_app_count(); i < n && s_nnat < NAT_MAX; i++) {
        const nucleo_app_def_t *a = nucleo_app_at(i);
        if (!a->category || strcmp(a->category, "Games")) continue;
        bool emu = false;
        for (const System &s : SYS) if (!strcmp(a->id, s.app)) emu = true;
        if (!emu) s_nat[s_nnat++] = (int16_t)i;
    }
    if (s_nnat > 1) qsort(s_nat, s_nnat, sizeof s_nat[0], cmp_nat);
    s_ngrp = 0;
    if (s_nnat) { s_grp_n[s_ngrp] = s_nnat; s_grp[s_ngrp++] = NATIVE; }
    char saved[FILT + 1];
    snprintf(saved, sizeof saved, "%s", s_filter);
    s_filter[0] = '\0';                            // the counts are of everything, not of a search
    int roms[NSYS];
    for (int g = 0; g < NSYS; g++) roms[g] = find_app(SYS[g].app) >= 0 ? walk(SYS[g], MAXR, false) : -1;
    snprintf(s_filter, sizeof s_filter, "%s", saved);
    for (int g = 0; g < NSYS; g++) {
        if (roms[g] < 0) continue;
        bool any = false;                          // any cartridge for this emulator at all?
        for (int h = 0; h < NSYS; h++) if (!strcmp(SYS[h].app, SYS[g].app) && roms[h] > 0) any = true;
        bool first = true;                         // this emulator's first console
        for (int h = 0; h < g; h++) if (!strcmp(SYS[h].app, SYS[g].app)) first = false;
        if (roms[g] > 0 || (!any && first)) { s_grp_n[s_ngrp] = roms[g]; s_grp[s_ngrp++] = g; }
    }
}

// ── the return trip across a game's Solo reboot ─────────────────────────────────────────────────────
// A game launched from here boots on its own (a fresh heap) and its exit reboots into the full OS. The level, the
// system, the search and the title survive in RTC memory, so that boot reopens the gallery where the player was.
#define RET_MAGIC 0x47465254u   // 'GFRT'
struct Ret { uint32_t magic; int8_t level, group; char filter[FILT + 1]; uint32_t key; };
RTC_NOINIT_ATTR static Ret s_ret;

static uint32_t fnv(const char *s, uint32_t h = 2166136261u)
{
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static uint32_t key_of(int i)            // what identifies title i across reboots
{
    if (s_level == 0) return fnv(group_title(s_grp[s_ix[i]]));
    if (s_group == NATIVE) return fnv(nucleo_app_at(s_nat[s_ix[i]])->id);
    return fnv(rom_name(i));
}
static void ret_capture(void)
{
    s_ret.magic = RET_MAGIC;
    s_ret.level = (int8_t)s_level; s_ret.group = (int8_t)s_group;
    snprintf(s_ret.filter, sizeof s_ret.filter, "%s", s_filter);
    s_ret.key = s_n ? key_of(s_sel) : 0;
}
bool gamefront_take_return(void)
{
    const bool ok = s_ret.magic == RET_MAGIC && esp_reset_reason() == ESP_RST_SW;
    s_ret.magic = 0;
    s_restore = ok;
    return ok;
}

// ── open / navigate ─────────────────────────────────────────────────────────────────────────────────
static void select_key(uint32_t key)
{
    for (int i = 0; i < s_n; i++) if (key_of(i) == key) { s_sel = i; return; }
}
static void enter_group(int g)
{
    s_sel0 = s_sel;
    s_level = 1; s_group = g;
    s_filter[0] = '\0'; s_flen = 0;
    s_sel = 0;
    refilter();
}
static void leave_group(void)
{
    shelf_free();
    const int g = s_group;
    s_level = 0;
    s_filter[0] = '\0'; s_flen = 0;
    refilter();
    s_sel = s_sel0 < s_n ? s_sel0 : 0;
    for (int i = 0; i < s_n; i++) if (s_grp[s_ix[i]] == g) s_sel = i;
}

static void pic_forget(void);
void gamefront_open(void)
{
    systems_build();
    covers_scan();
    pic_forget();
    if (s_restore) {                               // back from a game's Solo boot: where the player was
        s_restore = false;
        s_level = s_ret.level; s_group = s_ret.group;
        snprintf(s_filter, sizeof s_filter, "%s", s_ret.filter);
        s_sel = 0; s_sel0 = 0;
    } else if (!s_opened) {                        // the first time: the systems (or the only one there is)
        s_level = 0; s_group = NATIVE; s_sel = 0; s_sel0 = 0;
        s_filter[0] = '\0';
    }
    s_opened = true;
    // The system that was open may be gone (an emulator removed, its folder emptied): back to the systems.
    bool still = s_level == 0;
    for (int k = 0; k < s_ngrp; k++) if (s_grp[k] == s_group) still = true;
    if (!still) { s_level = 0; s_filter[0] = '\0'; }
    if (s_ngrp == 1) { s_level = 1; s_group = s_grp[0]; }   // one system: straight to its titles
    else if (s_ngrp == 0) { s_level = 0; s_group = NATIVE; }
    s_flen = (int)strlen(s_filter);
    const uint32_t want = s_ret.key;
    refilter();
    if (s_ret.magic == 0 && want) select_key(want);
    s_ret.key = 0;
    s_ret.magic = 0;                               // a snapshot is consumed by the first opening after it
}

static void step(int by)
{
    if (s_n <= 1) return;
    s_sel = ((s_sel + by) % s_n + s_n) % s_n;     // a wheel: the ends meet
}
static void page(int by)
{
    s_sel += by;
    if (s_sel < 0) s_sel = 0;
    if (s_sel > s_n - 1) s_sel = s_n - 1;
}

static int open_sel(char *launch_id, int cap)
{
    if (!s_n) return GF_NONE;
    if (s_level == 0) { enter_group(s_grp[s_ix[s_sel]]); return GF_REDRAW; }
    if (s_group == NATIVE) {
        snprintf(launch_id, cap, "%s", nucleo_app_at(s_nat[s_ix[s_sel]])->id);
        s_launch[0] = '\0';
    } else {
        snprintf(launch_id, cap, "%s", SYS[s_group].app);
        snprintf(s_launch, sizeof s_launch, "%s/%s", SYS[s_group].dir, rom_name(s_sel));
    }
    ret_capture();
    pic_forget();
    shelf_free();                                  // the game gets the RAM; gamefront_open() rebuilds the shelf
    return GF_LAUNCH;
}

const char *gamefront_launch_file(void) { return s_launch[0] ? s_launch : nullptr; }

#define SLOTS_BIG   7
#define SLOTS_SMALL 5
static int slots(void) { return d.width() >= 300 ? SLOTS_BIG : SLOTS_SMALL; }

int gamefront_key(int key, char ch, char *launch_id, int cap)
{
    if (nucleo_touch_is_row(key, ch)) {            // the wheel: a tap brings a title to the middle, the middle one opens
        const int k = ch - NUCLEO_TOUCH_ROW, i = lg::wheel_at(s_sel, s_n, slots(), k);
        if (k == slots() / 2) return open_sel(launch_id, cap);
        if (i >= 0) s_sel = i;
        return GF_REDRAW;
    }
    switch (key) {
    case NK_UP:    step(-1); return GF_REDRAW;
    case NK_DOWN:  step(1);  return GF_REDRAW;
    case NK_LEFT:  page(-5); return GF_REDRAW;
    case NK_RIGHT: page(5);  return GF_REDRAW;
    case NK_ENTER: return open_sel(launch_id, cap);
    case NK_BACK:                                  // the search first, then up a level, then out
        if (s_flen) { s_filter[0] = '\0'; s_flen = 0; refilter(); return GF_REDRAW; }
        if (s_level == 1 && s_ngrp > 1) { leave_group(); return GF_REDRAW; }
        shelf_free(); pic_forget();
        return GF_CLOSE;
    case NK_DEL:
        if (s_flen) { s_filter[--s_flen] = '\0'; refilter(); }
        return GF_REDRAW;
    case NK_CHAR:
        // 1..7 (1..5 on the Cardputer) open the title in that slot of the wheel, counted from its top.
        if (ch >= '1' && ch < '1' + slots()) {
            const int i = lg::wheel_at(s_sel, s_n, slots(), ch - '1');
            if (i < 0) return GF_NONE;
            s_sel = i;
            return open_sel(launch_id, cap);
        }
        if (ch > ' ' && ch < 127 && s_flen < FILT) {   // anything else printable searches
            s_filter[s_flen++] = ch; s_filter[s_flen] = '\0';
            s_sel = 0;
            refilter();
            return GF_REDRAW;
        }
        return GF_NONE;
    default: return GF_NONE;
    }
}

// ── pictures ────────────────────────────────────────────────────────────────────────────────────────
// A picture is a .pic file: "PIC1", u16 w, u16 h, u32 source size, u32 source mtime, then w*h RGB565 in the panel's
// byte order. The source fields make it go stale on its own when a cover or a screenshot is replaced.
struct PicHead { char magic[4]; uint16_t w, h; uint32_t src_size, src_time; };
enum { PIC_NONE, PIC_FILE, PIC_POSTER };
static struct {
    int kind;
    char path[PATHMAX + 48];
    int w, h;
    bool dark;                                     // a screenshot / cover (dark tile) or a console photo (light card)
    char poster[24];                               // the icon of a poster
    uint32_t key;                                  // whose picture it is
} s_pic;

static void pic_forget(void) { s_pic.key = 0; s_pic.kind = PIC_NONE; }

static bool pic_valid(const char *path, uint32_t size, uint32_t time, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    PicHead ph;
    const bool ok = fread(&ph, 1, sizeof ph, f) == sizeof ph && !memcmp(ph.magic, "PIC1", 4) && ph.src_size == size
                    && ph.src_time == time && ph.w && ph.h && ph.w <= 160 && ph.h <= 160;
    fclose(f);
    if (ok) { *w = ph.w; *h = ph.h; }
    return ok;
}
static bool pic_write(const char *path, const uint16_t *px, int w, int h, uint32_t size, uint32_t time)
{
    char tmp[PATHMAX + 56];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    PicHead ph = { { 'P', 'I', 'C', '1' }, (uint16_t)w, (uint16_t)h, size, time };
    bool ok = fwrite(&ph, 1, sizeof ph, f) == sizeof ph && fwrite(px, 2, (size_t)w * h, f) == (size_t)w * h;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    if (!ok) remove(tmp);
    return ok;
}

// Raw deflate, from memory (zf null) or from a file (zn bytes), into out (non-wrapping): exactly `need` bytes.
// The ~11 KB inflater state lives only for the call; a file is read 512 bytes at a time.
static bool inflate_to(const uint8_t *mem, FILE *zf, size_t zn, uint8_t *out, size_t need)
{
    tinfl_decompressor *z = (tinfl_decompressor *)malloc(sizeof *z);
    if (!z) return false;
    tinfl_init(z);
    uint8_t buf[512];
    const uint8_t *src = mem;
    size_t avail = mem ? zn : 0, left = mem ? 0 : zn, done = 0;
    bool ok = false;
    for (;;) {
        if (!avail && left) {
            avail = fread(buf, 1, left < sizeof buf ? left : sizeof buf, zf);
            if (!avail) break;
            left -= avail; src = buf;
        }
        size_t in = avail, outn = need - done;
        const tinfl_status r = tinfl_decompress(z, src, &in, out, out + done, &outn,
                                                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (left ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        src += in; avail -= in; done += outn;
        if (r == TINFL_STATUS_DONE) { ok = done == need; break; }
        if (r < 0 || done >= need || (r == TINFL_STATUS_NEEDS_MORE_INPUT && !avail && !left)) break;
    }
    free(z);
    return ok;
}

// The shared 32 KB canvas is the scratch a picture is unpacked into (nothing is composed at this point).
static uint16_t *scratch(size_t need)
{
    M5Canvas *cv = nucleo_screen();
    return cv && cv->bufferLength() >= need ? (uint16_t *)cv->getBuffer() : nullptr;
}
static void swap_all(uint16_t *px, size_t n) { for (size_t i = 0; i < n; i++) px[i] = (uint16_t)(px[i] >> 8 | px[i] << 8); }

// A console's photo, from the firmware, as GameShots/.cache/sys_<key>.pic.
static bool art_pic(const System &s, char *path, size_t n, int *w, int *h)
{
    snprintf(path, n, "%s/sys_%s.pic", CACHE, s.key);
    if (pic_valid(path, s.art.n, 0, w, h)) return true;
    const size_t need = (size_t)s.art.w * s.art.h * 2;
    uint16_t *px = scratch(need);
    if (!px || !inflate_to(s.art.z, nullptr, s.art.n, (uint8_t *)px, need)) return false;
    swap_all(px, need / 2);
    mkdir(GF_DIR, 0775); mkdir(CACHE, 0775);
    *w = s.art.w; *h = s.art.h;
    return pic_write(path, px, s.art.w, s.art.h, s.art.n, 0);
}

// A cartridge's screenshot: <folder>/.snaps/<file>.snp ("SNP1", u16 w, u16 h, u32 deflated size, raw-deflated RGB565
// in native byte order) -> <file>.pic beside it.
static bool snap_pic(const System &s, const char *rom, char *path, size_t n, int *w, int *h)
{
    char snp[PATHMAX + 48];
    snprintf(snp, sizeof snp, "%s/.snaps/%s.snp", s.dir, rom);
    struct stat st;
    if (stat(snp, &st) != 0) return false;
    snprintf(path, n, "%s/.snaps/%s.pic", s.dir, rom);
    if (pic_valid(path, (uint32_t)st.st_size, (uint32_t)st.st_mtime, w, h)) return true;
    FILE *f = fopen(snp, "rb");
    if (!f) return false;
    uint8_t hd[12];
    bool ok = false;
    if (fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, "SNP1", 4)) {
        const int sw = hd[4] | hd[5] << 8, sh = hd[6] | hd[7] << 8;
        const uint32_t zn = hd[8] | hd[9] << 8 | (uint32_t)hd[10] << 16 | (uint32_t)hd[11] << 24;
        uint16_t *px = sw > 0 && sh > 0 && sw <= 160 && sh <= 160 ? scratch((size_t)sw * sh * 2) : nullptr;
        if (px && zn && inflate_to(nullptr, f, zn, (uint8_t *)px, (size_t)sw * sh * 2)) {
            swap_all(px, (size_t)sw * sh);
            *w = sw; *h = sh;
            ok = true;
            fclose(f); f = nullptr;
            ok = pic_write(path, px, sw, sh, (uint32_t)st.st_size, (uint32_t)st.st_mtime);
        }
    }
    if (f) fclose(f);
    return ok;
}

// The decoders read a cover through this: a file the gallery opened itself (one open for all the strips).
struct CoverFile : lgfx::DataWrapper {
    FILE *f;
    explicit CoverFile(FILE *fp) : f(fp) {}
    int read(uint8_t *buf, uint32_t len) override { return (int)fread(buf, 1, len, f); }
    void skip(int32_t off) override { fseek(f, off, SEEK_CUR); }
    bool seek(uint32_t off) override { return fseek(f, (long)off, SEEK_SET) == 0; }
    void close(void) override { if (f) fclose(f); f = nullptr; }
    int32_t tell(void) override { return (int32_t)ftell(f); }
};

// A native game's cover (png / jpg / bmp, any size), fitted to w x h -> GameShots/.cache/<id>_<w>x<h>.pic. Decoded
// in strips of a few rows (one small sprite, freed at once).
static bool cover_pic(int k, int bw, int bh, char *path, size_t n, int *w, int *h)
{
    static const char *const EXT[3] = { "png", "jpg", "bmp" };
    const char *id = nucleo_app_at(s_nat[k])->id;
    char src[96];
    int e = 0;
    while (e < 3 && !(s_nat_cover[k] & (1 << e))) e++;
    if (e == 3) return false;
    snprintf(src, sizeof src, "%s/%s.%s", GF_DIR, id, EXT[e]);
    struct stat st;
    if (stat(src, &st) != 0) return false;
    snprintf(path, n, "%s/%s_%dx%d.pic", CACHE, id, bw, bh);
    if (pic_valid(path, (uint32_t)st.st_size, (uint32_t)st.st_mtime, w, h)) return true;
    M5Canvas strip;
    strip.setPsram(false);
    strip.setColorDepth(16);
    int rows = 24;
    while (rows >= 4 && !strip.createSprite(bw, rows)) rows /= 2;
    if (rows < 4) return false;
    mkdir(GF_DIR, 0775); mkdir(CACHE, 0775);
    char tmp[PATHMAX + 56];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(src, "rb");
    CoverFile dw(in);
    FILE *f = in ? fopen(tmp, "wb") : nullptr;
    bool ok = f != nullptr;
    PicHead ph = { { 'P', 'I', 'C', '1' }, (uint16_t)bw, (uint16_t)bh, (uint32_t)st.st_size, (uint32_t)st.st_mtime };
    if (ok) ok = fwrite(&ph, 1, sizeof ph, f) == sizeof ph;
    for (int y0 = 0; y0 < bh && ok; y0 += rows) {
        const int rn = bh - y0 < rows ? bh - y0 : rows;
        strip.fillSprite(0);
        fseek(in, 0, SEEK_SET);
        // The whole cover fitted to bw x bh, shifted up so its rows y0.. land in the strip (the rest is clipped).
        if (e == 0)      strip.drawPng(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        else if (e == 1) strip.drawJpg(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        else             strip.drawBmp(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        ok = fwrite(strip.getBuffer(), 2, (size_t)bw * rn, f) == (size_t)bw * rn;
    }
    if (f) ok = fclose(f) == 0 && ok;
    dw.close();
    strip.deleteSprite();
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    else remove(tmp);
    if (ok) { *w = bw; *h = bh; }
    return ok;
}

// The selection's picture, worked out once per selection (and per screen size): a .pic to stream, or a poster.
static void pic_prepare(bool big)
{
    if (!s_n) { s_pic.kind = PIC_NONE; s_pic.key = 0; return; }
    const uint32_t key = key_of(s_sel) ^ (uint32_t)(s_level << 24) ^ (uint32_t)((s_group + 2) << 20) ^ (big ? 0x80000000u : 0);
    if (s_pic.key == key) return;
    s_pic.key = key;
    s_pic.kind = PIC_POSTER;
    s_pic.dark = true;
    const size_t pn = sizeof s_pic.path;
    if (s_level == 0) {
        const int g = s_grp[s_ix[s_sel]];
        if (g == NATIVE) { snprintf(s_pic.poster, sizeof s_pic.poster, "Games"); return; }
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", SYS[g].app);
        if (art_pic(SYS[g], s_pic.path, pn, &s_pic.w, &s_pic.h)) { s_pic.kind = PIC_FILE; s_pic.dark = false; }
    } else if (s_group == NATIVE) {
        const int k = s_ix[s_sel];
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", nucleo_app_at(s_nat[k])->id);
        if (cover_pic(k, big ? 144 : 112, big ? 81 : 63, s_pic.path, pn, &s_pic.w, &s_pic.h)) s_pic.kind = PIC_FILE;
    } else {
        const System &s = SYS[s_group];
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", s.app);
        if (snap_pic(s, rom_name(s_sel), s_pic.path, pn, &s_pic.w, &s_pic.h)) s_pic.kind = PIC_FILE;
        else if (art_pic(s, s_pic.path, pn, &s_pic.w, &s_pic.h)) { s_pic.kind = PIC_FILE; s_pic.dark = false; }
    }
}

static uint16_t mix565(uint16_t a, uint16_t b, int t)   // t = 0 (a) .. 256 (b)
{
    const int r = (((a >> 11) & 31) * (256 - t) + ((b >> 11) & 31) * t) >> 8;
    const int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    const int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// The picture tile at (x, y, w, h): the .pic's rows that fall inside the strip being composed are read from the card
// straight into it; a poster is the item's accent deepening downwards with its icon.
static void draw_pic(int x, int y, int w, int h, unsigned short acc)
{
    if (s_pic.kind == PIC_FILE) {
        d.fillRoundRect(x, y, w, h, 8, s_pic.dark ? (unsigned short)0x0841 : (unsigned short)ART_TILE);
        const int px = x + (w - s_pic.w) / 2, py = y + (h - s_pic.h) / 2;
        int32_t cx, cy, cw, ch;
        d.getClipRect(&cx, &cy, &cw, &ch);
        const int y0 = py > cy ? py : cy, y1 = py + s_pic.h < cy + ch ? py + s_pic.h : cy + ch;
        FILE *f = y0 < y1 ? fopen(s_pic.path, "rb") : nullptr;
        if (f) {
            uint16_t row[160];
            fseek(f, (long)(sizeof(PicHead) + (size_t)(y0 - py) * s_pic.w * 2), SEEK_SET);
            for (int ry = y0; ry < y1 && fread(row, 2, s_pic.w, f) == (size_t)s_pic.w; ry++)
                d.pushImage(px, ry, s_pic.w, 1, (const lgfx::swap565_t *)row);
            fclose(f);
        }
        if (s_pic.dark) d.drawRect(px - 1, py - 1, s_pic.w + 2, s_pic.h + 2, LINE);
        return;
    }
    if (s_pic.kind != PIC_POSTER) { d.fillRoundRect(x, y, w, h, 8, lg::card()); return; }
    d.fillRoundRect(x, y, w, h, 8, mix565(acc, INK, 150));
    for (int r = 6; r < h - 6; r++) d.drawFastHLine(x + 2, y + r, w - 4, mix565(acc, INK, 90 + 130 * r / h));
    int ir = (w < h ? w : h) / 2 - 12;
    if (ir > 40) ir = 40;
    launcher_draw_icon(&d, x + w / 2, y + h / 2, ir, s_pic.poster, s_pic.poster[0], mix565(acc, FG, 220), mix565(acc, INK, 160));
}

// The line under the picture: the system and its maker, a cartridge's console, a native game's play modes.
static void caption(char *out, size_t n)
{
    out[0] = '\0';
    if (!s_n) return;
    if (s_level == 0) {
        const int k = s_ix[s_sel], g = s_grp[k], c = s_grp_n[k];
        char num[24];
        if (c > MAXR) snprintf(num, sizeof num, "%d+", MAXR);
        else snprintf(num, sizeof num, "%d", c);
        snprintf(out, n, "%s %s%s%s", num, c == 1 ? TR5("gioco", "game", "juego", "jeu", "Spiel") : TR5("giochi", "games", "juegos", "jeux", "Spiele"),
                 g == NATIVE ? "" : "  -  ", g == NATIVE ? "" : SYS[g].maker);
        return;
    }
    if (s_group != NATIVE) { snprintf(out, n, "%s", SYS[s_group].name); return; }
    const Meta *m = meta_of(nucleo_app_at(s_nat[s_ix[s_sel]])->id);
    const unsigned modes = m ? m->modes : M1P;
    const char *lab[5] = { TR5("1 giocatore", "1 player", "1 jugador", "1 joueur", "1 Spieler"), "vs CPU",
                           TR5("2 in locale", "2 local", "2 en local", "2 en local", "2 lokal"), "LAN", "co-op" };
    for (int b = 0; b < 5; b++)
        if (modes & (1 << b)) { if (out[0]) strncat(out, " - ", n - strlen(out) - 1); strncat(out, lab[b], n - strlen(out) - 1); }
}
static const char *blurb(void)
{
    if (!s_n || s_level != 1 || s_group != NATIVE) return nullptr;
    const nucleo_app_def_t *a = nucleo_app_at(s_nat[s_ix[s_sel]]);
    const Meta *m = meta_of(a->id);
    if (m) return TR5(m->tag[0], m->tag[1], m->tag[2], m->tag[3], m->tag[4]);
    return a->desc && a->desc[0] ? a->desc : nullptr;
}
static const char *empty_line(void)
{
    if (s_flen) return TR5("Nessun risultato", "No match", "Sin resultados", "Aucun resultat", "Keine Treffer");
    if (s_level == 0) return TR5("Nessun gioco", "No games", "Sin juegos", "Aucun jeu", "Keine Spiele");
    if (s_group == NATIVE) return TR5("Nessun gioco nativo", "No native games", "Sin juegos nativos", "Aucun jeu natif", "Keine nativen Spiele");
    static char t[64];
    snprintf(t, sizeof t, "%s %s", TR5("Nessuna ROM in", "No ROMs in", "Ninguna ROM en", "Aucune ROM dans", "Keine ROMs in"), SYS[s_group].dir + strlen(NUCLEO_SD_MOUNT));
    return t;
}
static const char *empty_sub(void)
{
    if (s_flen) return TR5("Canc per correggere", "Backspace to edit", "Retroceso para editar", "Retour pour modifier", "Ruecktaste zum Bearbeiten");
    return s_level == 1 && s_group != NATIVE ? "" : TR5("Si installano dallo Store", "Install them from the Store", "Se instalan desde la Store",
                                                        "A installer depuis le Store", "Aus dem Store installieren");
}
static const char *level_title(void)
{
    return s_level == 0 ? TR5("Giochi", "Games", "Juegos", "Jeux", "Spiele") : group_title(s_group);
}
static const char *search_hint(void) { return TR5("Scrivi per cercare", "Type to search", "Escribe para buscar", "Tapez pour chercher", "Tippen zum Suchen"); }
static void count_text(char *out, size_t n)
{
    if (!s_n)                 snprintf(out, n, "0");
    else if (s_total > s_n)   snprintf(out, n, "%d/%d+", s_sel + 1, s_n);
    else                      snprintf(out, n, "%d/%d", s_sel + 1, s_n);
}
static const char *wheel_label(int i, void *) { return title_of(i); }

// ── the screens ─────────────────────────────────────────────────────────────────────────────────────
static int s_H;                                    // the screen's height (inside a frame d.height() is the strip's)
static const unsigned char WHEEL_BIG[SLOTS_BIG] = { 21, 23, 25, 32, 25, 23, 21 };
static const unsigned char WHEEL_SMALL[SLOTS_SMALL] = { 19, 21, 27, 21, 19 };

// The panel, 320x240: header with Back, search, then the picture and the title on the left and the wheel on the right.
#define WX 168                                     // the wheel's left edge
static void paint_big(void *)
{
    const int W = d.width(), H = s_H;
    const unsigned short acc = acc_now();
    char cnt[24]; count_text(cnt, sizeof cnt);
    int y = lg::header(level_title(), nullptr, acc_of(s_level ? s_group : NATIVE), NK_BACK);
    y = lg::search(0, y, W, 36, s_filter, search_hint(), cnt, s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, acc);
    const int top = y, bh = H - top;
    d.fillRect(0, top, W, bh, BG);
    if (!s_n) { lg::message(0, top, W, bh, empty_line(), empty_sub(), MUTED); return; }
    const int tx = 8, tw = WX - 16, ph = 108;
    draw_pic(tx, top + 2, tw, ph, acc);
    nucleo_touch_add(tx, top + 2, tw, ph, NK_ENTER, 0);   // the picture opens the selection too
    char cap[64]; caption(cap, sizeof cap);
    int ly = top + ph + 5;
    lg::font(0); d.setTextColor(acc, BG);
    char fitted[64]; lg::fit(fitted, sizeof fitted, cap, tw - 2);
    d.drawString(fitted, tx + 2, ly);
    ly += 19;
    lg::font(1); d.setTextColor(FG, BG);
    lg::wrap(title_of(s_sel), tx + 2, ly, tw - 4, 19, H - ly >= 58 ? 3 : 2);
    const lg::Wheel wl = { SLOTS_BIG, WHEEL_BIG, s_n, s_sel, wheel_label, nullptr, nullptr, acc, acc };
    lg::wheel(WX, top, W - WX, bh, wl);
}

// The Cardputer alone, 240x135: the picture with its caption and the whole title on the left; the search and a
// five-title wheel on the right.
#define CX 122
static void paint_small(void *)
{
    const int W = d.width(), H = s_H;
    const unsigned short acc = acc_now();
    d.fillRect(0, 0, W, H, BG);
    char cnt[24]; count_text(cnt, sizeof cnt);
    lg::search(CX, 0, W - CX, 22, s_filter, TR5("Cerca", "Search", "Buscar", "Chercher", "Suchen"), nullptr, MUTED, acc);
    if (!s_n) {
        lg::message(0, 22, W, H - 22, empty_line(), empty_sub(), MUTED);
        return;
    }
    const int ph = 103;
    draw_pic(1, 1, CX - 2, ph, acc);
    char cap[48]; caption(cap, sizeof cap);
    char fitted[48];
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    d.setTextDatum(top_right); d.setTextColor(s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, BG);
    d.drawString(cnt, CX - 3, ph + 4);                 // the position: beside the caption, the search line is narrow
    const int cw = d.textWidth(cnt) + 6;
    d.setTextDatum(top_left);
    lg::fit(fitted, sizeof fitted, cap, CX - 6 - cw);
    d.setTextColor(acc, BG); d.drawString(fitted, 3, ph + 4);
    d.setTextColor(FG, BG);
    lg::wrap(title_of(s_sel), 3, ph + 14, CX - 6, 9, 2);
    const lg::Wheel wl = { SLOTS_SMALL, WHEEL_SMALL, s_n, s_sel, wheel_label, nullptr, nullptr, acc, acc };
    lg::wheel(CX, 22, W - CX, H - 22, wl);
}

// The details of the selection, for the Cardputer's card: label / value pairs.
struct Detail { const char *k; char v[48]; };
static int details(Detail *out, int max)
{
    int n = 0;
    auto add = [&](const char *k, const char *v) { if (n < max && v && v[0]) { out[n].k = k; snprintf(out[n].v, sizeof out[n].v, "%s", v); n++; } };
    if (!s_n) return 0;
    char t[64];
    if (s_level == 0) {
        const int k = s_ix[s_sel], g = s_grp[k], c = s_grp_n[k];
        if (c > MAXR) snprintf(t, sizeof t, "%d+", MAXR); else snprintf(t, sizeof t, "%d", c);
        add(TR5("Giochi", "Games", "Juegos", "Jeux", "Spiele"), t);
        if (g == NATIVE) add(TR5("Dove", "Where", "Donde", "Ou", "Wo"), TR5("Integrati e dallo Store", "Built in and from the Store", "Integrados y de la Store", "Integres et du Store", "Eingebaut und aus dem Store"));
        else {
            add(TR5("Produttore", "Maker", "Fabricante", "Fabricant", "Hersteller"), SYS[g].maker);
            snprintf(t, sizeof t, "%s/", SYS[g].dir + strlen(NUCLEO_SD_MOUNT));
            add(TR5("Cartella", "Folder", "Carpeta", "Dossier", "Ordner"), t);
        }
        return n;
    }
    if (s_group == NATIVE) {
        caption(t, sizeof t);
        add(TR5("Giocatori", "Players", "Jugadores", "Joueurs", "Spieler"), t);
        add(TR5("Origine", "Source", "Origen", "Origine", "Herkunft"),
            nucleo_store_app_is_native(nucleo_app_at(s_nat[s_ix[s_sel]])->id) ? "Store" : TR5("Integrato", "Built in", "Integrado", "Integre", "Eingebaut"));
        return n;
    }
    const System &sy = SYS[s_group];
    const char *file = rom_name(s_sel);
    add(TR5("Sistema", "System", "Sistema", "Systeme", "System"), sy.name);
    add(TR5("Produttore", "Maker", "Fabricante", "Fabricant", "Hersteller"), sy.maker);
    const char *open = strchr(file, '(');                // the region tag of a No-Intro name: "(USA, Europe)"
    if (open) {
        const char *close = strchr(open, ')');
        if (close && close - open > 1) { snprintf(t, sizeof t, "%.*s", (int)(close - open - 1), open + 1); add(TR5("Regione", "Region", "Region", "Region", "Region"), t); }
    }
    static uint32_t s_size_for; static long s_size;      // one stat per selection, not per paint
    if (s_size_for != key_of(s_sel)) {
        char path[PATHMAX + 24]; struct stat st;
        snprintf(path, sizeof path, "%s/%s", sy.dir, file);
        s_size = stat(path, &st) == 0 ? (long)st.st_size : -1;
        s_size_for = key_of(s_sel);
    }
    const char *dot = strrchr(file, '.');
    if (s_size >= 0) snprintf(t, sizeof t, "%s  %ld KB", dot ? dot + 1 : "", (s_size + 1023) / 1024);
    else snprintf(t, sizeof t, "%s", dot ? dot + 1 : "");
    for (char *p = t; *p && *p != ' '; p++) *p = (char)toupper((unsigned char)*p);
    add(TR5("Cartuccia", "Cartridge", "Cartucho", "Cartouche", "Modul"), t);
    return n;
}

// The Cardputer while the gallery is on the panel: the selection's card — its whole title, what it is (system,
// region, cartridge; players and blurb; maker and count) — the search while typing, the keys. The wheel and the
// picture stay on the panel.
void gamefront_draw_companion(void *gfx)
{
    nucleo_app_set_gfx((LovyanGFX *)gfx);
    const int top = nucleo_app_content_top(), ch = nucleo_app_content_height(), bottom = top + ch - 13;
    const unsigned short acc = acc_now();
    d.fillRect(0, top, 240, ch, BG);
    int y = lg::comp_head(level_title(), acc_of(s_level ? s_group : NATIVE));
    if (s_flen) {                                      // the search, while there is one
        d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG); d.drawString(TR5("Cerca", "Search", "Buscar", "Cherche", "Suche"), 8, y);
        const int lx = 14 + d.textWidth(TR5("Cerca", "Search", "Buscar", "Cherche", "Suche"));
        const char *q = s_filter;
        while (*q && d.textWidth(q) > 220 - lx) q++;
        d.setTextColor(FG, BG); d.drawString(q, lx, y);
        d.fillRect(lx + 1 + d.textWidth(q), y + 2, 2, 12, acc);
        y += 17;
    }
    if (!s_n) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(MUTED, BG);
        lg::wrap(empty_line(), 8, y + 6, 224, 19, 2);
    } else {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(acc, BG);
        y += 2 + lg::wrap(title_of(s_sel), 8, y + 2, 224, 19, 2) * 19;
        Detail dt[5];
        const int n = details(dt, 5);
        d.setFont(&fonts::Font2);
        int kw = 0;
        for (int i = 0; i < n; i++) if (d.textWidth(dt[i].k) > kw) kw = d.textWidth(dt[i].k);
        for (int i = 0; i < n && y + 15 <= bottom; i++, y += 15) {
            d.setTextColor(MUTED, BG); d.drawString(dt[i].k, 8, y);
            char v[48]; lg::fit(v, sizeof v, dt[i].v, 224 - kw - 10);
            d.setTextColor(FG, BG); d.drawString(v, 8 + kw + 10, y);
        }
        const char *b = blurb();
        if (b && y + 10 <= bottom) {
            d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
            lg::wrap(b, 8, y + 3, 224, 10, (bottom - y - 3) / 10);
        }
    }
    d.setFont(&fonts::Font0);
    lg::comp_keys(s_level == 0 ? TR5("INVIO apri  ESC esci", "ENTER open  ESC quit", "INTRO abre  ESC sale", "ENTREE ouvre  ESC sort", "ENTER oeffnet  ESC Ende")
                               : TR5("INVIO gioca  ESC indietro", "ENTER play  ESC back", "INTRO juega  ESC atras", "ENTREE joue  ESC retour", "ENTER spielt  ESC zurueck"));
    char pos[16]; count_text(pos, sizeof pos);         // where the selection is, at the right of the keys
    d.setTextDatum(top_right); d.setTextColor(s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, BG);
    d.drawString(pos, 232, top + ch - 11); d.setTextDatum(top_left);
    nucleo_app_set_gfx(nullptr);
}
static void companion_cb(void *gfx)
{
    LovyanGFX *g = (LovyanGFX *)gfx;
    g->startWrite();
    gamefront_draw_companion(gfx);
    g->endWrite();
}

void gamefront_invalidate(void) { pic_forget(); }

void gamefront_render(void)
{
    const bool big = d.width() >= 300;
    s_H = d.height();
    if (on_roms() && !s_sh) refilter();            // the shelf left RAM (a failed allocation): rebuild it
    pic_prepare(big);
    nucleo_touch_clear();
    lg::frame(big ? paint_big : paint_small, nullptr, 0, 0, true);   // exact: the pictures and the cards in true colour
    nucleo_touch_commit();
    if (big) nucleo_extdisp_draw_internal(companion_cb);
}
