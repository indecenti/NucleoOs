// GameFront — see gamefront.h. The Games gallery: systems, then titles, on a HyperSpin-style wheel.
//
// PLUG AND PLAY. Nothing is configured: the native games are the registry's (built in, or installed from the
// Store), and the cartridges are FOUND — the whole card is walked for .gb / .gbc / .gg / .sms / .nes files (and zips that
// hold a Game Gear / Master System cartridge), wherever they are, and the result kept in an index on the card. The
// walk runs a few milliseconds at a time while the gallery is open (gamefront_step), so the gallery is usable at
// once with the last index and updates itself when the walk finds something new: once per boot, and again when
// the gallery is opened after a while.
//
// One painter for both screens, composed with lg::frame in RGB565 (exact colours, no flicker, no RAM of its own):
//   panel 320x240   header (Back) · search · picture + title on the left, a 7-title wheel on the right
//   Cardputer       picture + caption + title on the left, search and a 5-title wheel on the right
// With the panel attached the Cardputer shows the selection's details instead (gamefront_draw_companion).
//
// RAM: the launcher runs on a fragmented ~18 KB heap. The native list is a few dozen shorts; a console's shelf is ONE
// block sized to what it holds (~1.2 KB for 30 cartridges, ~5 KB at most) while its titles are shown. Pictures never
// live in RAM: each is a raw .pic file on the card (written by tools/fetch-rom-snaps.py, or converted once here with
// the 32 KB shared canvas as the scratch), kept open while it is the selection's and read into the strip being
// composed. The walk holds one directory handle per level (5 at most).
// SPEED: the index is read, never the folders, to list a console; a search filters the shelf in RAM; each folder's
// .snaps is listed once (no lookup ever misses on a big card); a picture is opened once per selection.
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
#include "gamefront_art.h"    // the consoles' photos and marks, cut out (tools/gen-gallery-art.py)
#include "miniz.h"            // tinfl, in the chip's ROM
#include "gamefront_sfx.h"    // the interface sounds (tools/gen-ui-sounds.py)
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
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
#include "nucleo_audio.h"
}

int                     nucleo_app_count(void);   // the app registry (nucleo_app.cpp)
const nucleo_app_def_t *nucleo_app_at(int i);
extern "C" const char  *launcher_app_localized_name(const char *id);
extern "C" bool         nucleo_store_app_is_native(const char *id);   // store_apps.cpp: installed from the Store

#define CACHE      GF_DIR "/.cache"
#define INDEX      CACHE "/roms.idx"
#define LAST_FILE  NUCLEO_SD_MOUNT "/system/config/gamefront.last"
#define NAT_MAX    32          // native games held (built in + Store)
#define MAXR       120         // cartridges held at once: a search reaches the rest of a bigger collection
#define POOL       4096        // their names and folders, packed end to end
#define NAMEMAX    64
#define PATHMAX    160
#define FILT       16
#define SCAN_DEPTH 5           // folders below the card's root the walk goes into
#define SCAN_IDLE_US 800000      // the walk runs only after this long without a key (it never slows the wheel)

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

// The consoles. Their index here is the system digit in the cartridge index (new ones go at the end); the gallery lists
// them in SHOW_ORDER instead: by maker, then by year. Two share an emulator each, but every console is its own system.
struct Art { const uint8_t *z; uint16_t n; uint8_t w, h; };   // RGB565 + 4-bit alpha, deflated (gamefront_art.h)
// A console's NAME is never written in the gallery (it is searched by, not shown): its mark is drawn instead.
struct System {
    const char *key, *app, *name, *maker;
    unsigned short col;
    Art art, mark;             // the photo (a 118x80 box) and the small mark beside its name
};
static const System SYS[] = {
#define GFA(k) { GFA_##k##_Z, sizeof GFA_##k##_Z, GFA_##k##_W, GFA_##k##_H }
    { "gb",  "gbemu", "Game Boy",       "Nintendo, 1989", 0x8FF3, GFA(GB),  GFA(GB_S) },
    { "gbc", "gbemu", "Game Boy Color", "Nintendo, 1998", 0xC5F5, GFA(GBC), GFA(GBC_S) },
    { "gg",  "ggemu", "Game Gear",      "Sega, 1990",     0x5D7F, GFA(GG),  GFA(GG_S) },
    { "sms", "ggemu", "Master System",  "Sega, 1986",     0xF96B, GFA(SMS), GFA(SMS_S) },
    { "nes", "nesemu", "NES",           "Nintendo, 1985", 0xAD55, GFA(NES), GFA(NES_S) },
};
// The native games' entry has a picture and a mark too (a pocket computer, painted for it): a "system" of its own.
static const System NATIVE_SYS = { "native", "", "", "", 0xFB4D, GFA(NATIVE), GFA(NATIVE_S) };
#undef GFA

#define NSYS ((int)(sizeof SYS / sizeof SYS[0]))
static const System &sysg(int g) { return g < 0 ? NATIVE_SYS : SYS[g]; }   // a group's pictures (NATIVE included)
static const uint8_t SHOW_ORDER[] = { 4, 0, 1, 3, 2 };   // NES, Game Boy, Game Boy Color, Master System, Game Gear
static_assert(sizeof SHOW_ORDER == NSYS, "every console has its place in SHOW_ORDER");
#define NATIVE (-1)            // the group of the native games
#define ACC_NATIVE 0xFB4D      // the Games category's colour
#define C_LAST 0x8FF3          // "last played"

// ── state ───────────────────────────────────────────────────────────────────────────────────────────
static int     s_level;                    // 0 = systems, 1 = the titles of s_group
static int     s_group = NATIVE;
static int     s_grp[1 + NSYS], s_ngrp;    // the systems shown (NATIVE or a SYS index), in order
static int     s_grp_n[1 + NSYS];          // ...how many titles each has
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
static uint32_t s_cover_ready;             // native games whose cover file is known to be made (bit per s_nat slot)
static bool    s_have_index;               // an index of the card is there to list from
static int64_t s_input_at;                 // the last key
static int     s_H;                        // the screen's height (inside a frame d.height() is the strip's)

// A console's cartridges while its titles are shown: ONE heap block sized to what it holds — names and folders packed
// end to end, their order by title, which have a picture, and the view the search leaves. When it holds every match
// of the search it was read with (the usual case: up to MAXR cartridges) a longer search filters it in RAM; only a
// bigger collection is read again from the index, keeping the first MAXR matches.
enum { SN_PIC = 1, SN_SNP = 2 };
struct Shelf {
    int n, total;              // names held; names matching `with` in the index
    char with[FILT + 1];       // the search it was read with ("" = all of it)
    uint16_t *off;             // name i = pool + off[i], by title
    uint16_t *dir;             // its folder = pool + dir[i]
    uint16_t *view;            // the names the current search shows, in order
    uint8_t *snap;             // SN_* per name, from one listing of each folder's .snaps
    char *pool;
};
static Shelf *s_sh;
static const char *rom_name(int i) { return s_sh->pool + s_sh->off[s_sh->view[i]]; }
static const char *rom_dir(int i)  { return s_sh->pool + s_sh->dir[s_sh->view[i]]; }

// What was played last, per system, and which system: the selection lands there (one 28-byte file).
struct Last { char magic[4]; int8_t group; uint8_t pad[3]; uint32_t key[1 + NSYS]; };
static Last s_last;

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
    if (s_level == 0) return group_title(s_grp[s_ix[i]]);   // a console: its mark and its name
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
static uint32_t fnv(const char *s, uint32_t h = 2166136261u)
{
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static uint32_t fnvn(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}
static int find_app(const char *id)
{
    for (int i = 0, n = nucleo_app_count(); i < n; i++) if (!strcmp(nucleo_app_at(i)->id, id)) return i;
    return -1;
}

// ── the cartridges on the card ──────────────────────────────────────────────────────────────────────
// Which console a file is for: by extension; a zip by the name of the first file inside it (the emulators open a
// zipped Game Gear / Master System cartridge, not a zipped Game Boy one). -1 = not a cartridge.
static int zip_sys(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[30];
    char name[64];
    int sys = -1;
    if (fread(h, 1, sizeof h, f) == sizeof h && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) {
        const int nl = h[26] | h[27] << 8;
        if (nl > 4 && nl < (int)sizeof name && fread(name, 1, nl, f) == (size_t)nl) {
            name[nl] = '\0';
            const char *dot = strrchr(name, '.');
            if (dot && !strcasecmp(dot, ".gg")) sys = 2;
            else if (dot && !strcasecmp(dot, ".sms")) sys = 3;
        }
    }
    fclose(f);
    return sys;
}
static int sys_of(const char *name)            // by extension; -2 = a zip, look inside
{
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.') return -1;
    if (!strcasecmp(dot, ".gb"))  return 0;
    if (!strcasecmp(dot, ".gbc")) return 1;
    if (!strcasecmp(dot, ".gg"))  return 2;
    if (!strcasecmp(dot, ".sms")) return 3;
    if (!strcasecmp(dot, ".nes")) return 4;
    if (!strcasecmp(dot, ".zip")) return -2;
    return -1;
}

// The index: "GIX2 <cartridges> <hash>", then for each folder that has some "D <folder>" and one line per cartridge,
// "<system digit> <file>", and after them "P <file>" for each screenshot in its .snaps (<rom>.pic / <rom>.snp): so
// opening a console never lists a .snaps folder. Text, so a person can read it, and read a line at a time (no RAM).
static FILE *idx_open(int *count, uint32_t *hash);
static bool idx_ok(void) { FILE *f = idx_open(nullptr, nullptr); if (f) fclose(f); return f != nullptr; }
static FILE *idx_open(int *count, uint32_t *hash)
{
    FILE *f = fopen(INDEX, "r");
    if (!f) return nullptr;
    setvbuf(f, nullptr, _IOFBF, 1024);             // read a line at a time from 1 KB blocks, not 128-byte ones
    char hd[48];
    unsigned c = 0, h = 0;
    if (!fgets(hd, sizeof hd, f) || sscanf(hd, "GIX2 %u %u", &c, &h) != 2) { fclose(f); return nullptr; }
    if (count) *count = (int)c;
    if (hash) *hash = h;
    return f;
}
// The next cartridge (its system) or screenshot (sys -1), its file name in line (valid until the next call); dir keeps
// the current folder.
static bool idx_next(FILE *f, char *line, size_t ln, char *dir, size_t dn, int *sys, const char **name)
{
    while (fgets(line, (int)ln, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l > 2 && line[0] == 'D' && line[1] == ' ') { snprintf(dir, dn, "%s", line + 2); continue; }
        if (l > 2 && line[0] >= '0' && line[0] < '0' + NSYS && line[1] == ' ') { *sys = line[0] - '0'; *name = line + 2; return true; }
        if (l > 2 && line[0] == 'P' && line[1] == ' ') { *sys = -1; *name = line + 2; return true; }
    }
    return false;
}

// The walk of the card, a few milliseconds per call. One open directory per level; the index is written to a temporary
// file and replaces the old one only when it is complete and different.
static struct {
    bool on, done;                  // walking; walked at least once this boot
    int depth;
    DIR *dp[SCAN_DEPTH + 1];
    int plen[SCAN_DEPTH + 1];       // the path's length at each level
    char path[PATHMAX];
    bool dir_out;                   // the current folder's "D" line is written
    bool had[SCAN_DEPTH + 1];       // the folder at each level has cartridges (its .snaps is listed when it ends)
    FILE *out;
    int found;
    uint32_t hash;
    int64_t at;                     // when the last walk ended
} s_scan;

static void scan_close(void)
{
    for (int i = 0; i <= s_scan.depth && i <= SCAN_DEPTH; i++) if (s_scan.dp[i]) { closedir(s_scan.dp[i]); s_scan.dp[i] = nullptr; }
    if (s_scan.out) { fclose(s_scan.out); s_scan.out = nullptr; }
    s_scan.on = false;
}
static void scan_abort(void)
{
    if (!s_scan.on) return;
    scan_close();
    remove(INDEX ".tmp");
}
static void scan_start(void)
{
    if (s_scan.on) return;
    mkdir(GF_DIR, 0775); mkdir(CACHE, 0775);
    memset(s_scan.dp, 0, sizeof s_scan.dp);
    s_scan.out = fopen(INDEX ".tmp", "w");
    s_scan.dp[0] = s_scan.out ? opendir(NUCLEO_SD_MOUNT) : nullptr;
    if (!s_scan.dp[0]) { if (s_scan.out) fclose(s_scan.out); s_scan.out = nullptr; remove(INDEX ".tmp"); return; }
    fputs("GIX2 0000000000 0000000000\n", s_scan.out);   // the real counts are written over it at the end
    snprintf(s_scan.path, sizeof s_scan.path, "%s", NUCLEO_SD_MOUNT);
    s_scan.plen[0] = (int)strlen(s_scan.path);
    s_scan.depth = 0; s_scan.found = 0; s_scan.hash = 2166136261u; s_scan.dir_out = false;
    memset(s_scan.had, 0, sizeof s_scan.had);
    s_scan.on = true;
}
// Folders the walk leaves alone: the system's own, the apps', the web OS, and the gallery's caches.
static bool scan_skip(int depth, const char *name)
{
    if (name[0] == '.' || !strcasecmp(name, "System Volume Information") || !strcasecmp(name, "LOST.DIR")) return true;
    static const char *const ROOT[] = { "system", "apps", "www", "evilportal", "wallpapers", "models" };
    if (depth == 0) for (const char *r : ROOT) if (!strcasecmp(name, r)) return true;
    static const char *const DATA[] = { "GameShots", "Screenshots", "anima" };
    if (depth == 1 && !strncmp(s_scan.path + s_scan.plen[0], "/data/", 6)) for (const char *r : DATA) if (!strcasecmp(name, r)) return true;
    return false;
}
static bool is_dir(struct dirent *e, const char *path)
{
#if defined(DT_DIR) && !defined(_WIN32)
    if (e->d_type == DT_DIR) return true;
    if (e->d_type == DT_REG) return false;
#endif
    (void)e;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static void systems_build(void);
static void refilter(void);
// One slice of the walk (~budget us): 0 = walking (or idle), 1 = it just ended with the same index, 2 = it just
// ended with a different one (the lists change).
static int scan_step(int64_t budget)
{
    if (!s_scan.on) return 0;
    const int64_t until = esp_timer_get_time() + budget;
    while (esp_timer_get_time() < until) {
        struct dirent *e = readdir(s_scan.dp[s_scan.depth]);
        if (!e) {                                       // this folder is done: its screenshots, then up a level
            closedir(s_scan.dp[s_scan.depth]); s_scan.dp[s_scan.depth] = nullptr;
            if (s_scan.had[s_scan.depth]) {
                const int pl = s_scan.plen[s_scan.depth];
                snprintf(s_scan.path + pl, PATHMAX - pl, "/.snaps");
                DIR *sp = opendir(s_scan.path);
                s_scan.path[pl] = '\0';
                if (sp) {
                    fprintf(s_scan.out, "D %s\n", s_scan.path);
                    struct dirent *x;
                    while ((x = readdir(sp)) != NULL) {
                        const size_t xl = strlen(x->d_name);
                        if (xl < 5 || (strcasecmp(x->d_name + xl - 4, ".pic") && strcasecmp(x->d_name + xl - 4, ".snp"))) continue;
                        fprintf(s_scan.out, "P %s\n", x->d_name);
                        s_scan.hash = fnv(x->d_name, s_scan.hash);
                    }
                    closedir(sp);
                }
                s_scan.had[s_scan.depth] = false;
            }
            if (s_scan.depth == 0) break;
            s_scan.depth--;
            s_scan.path[s_scan.plen[s_scan.depth]] = '\0';
            s_scan.dir_out = false;
            continue;
        }
        const int pl = s_scan.plen[s_scan.depth];
        if (pl + 2 + (int)strlen(e->d_name) >= PATHMAX) continue;
        snprintf(s_scan.path + pl, PATHMAX - pl, "/%s", e->d_name);
        int sys = sys_of(e->d_name);
        if (sys == -1) {
            if (s_scan.depth < SCAN_DEPTH && !scan_skip(s_scan.depth, e->d_name) && is_dir(e, s_scan.path)) {
                DIR *sub = opendir(s_scan.path);
                if (sub) {                              // into it
                    s_scan.dp[++s_scan.depth] = sub;
                    s_scan.plen[s_scan.depth] = (int)strlen(s_scan.path);
                    s_scan.had[s_scan.depth] = false;
                    s_scan.dir_out = false;
                    continue;
                }
            }
            s_scan.path[pl] = '\0';
            continue;
        }
        if (sys == -2) sys = zip_sys(s_scan.path);
        s_scan.path[pl] = '\0';
        if (sys < 0) continue;
        if (!s_scan.dir_out) { fprintf(s_scan.out, "D %s\n", s_scan.path); s_scan.hash = fnv(s_scan.path, s_scan.hash); s_scan.dir_out = true; }
        fprintf(s_scan.out, "%d %s\n", sys, e->d_name);
        s_scan.had[s_scan.depth] = true;
        s_scan.hash = fnv(e->d_name, s_scan.hash + (uint32_t)sys);
        s_scan.found++;
    }
    esp_task_wdt_reset();
    if (s_scan.dp[0]) return 0;                         // more to walk
    // The walk is complete: its header, then it replaces the index if anything differs.
    fseek(s_scan.out, 0, SEEK_SET);
    fprintf(s_scan.out, "GIX2 %010u %010u", (unsigned)s_scan.found, (unsigned)s_scan.hash);
    const bool ok = fclose(s_scan.out) == 0;
    s_scan.out = nullptr;
    s_scan.on = false; s_scan.done = true; s_scan.at = esp_timer_get_time();
    uint32_t old = 0;
    int oldn = -1;
    FILE *f = idx_open(&oldn, &old);
    if (f) fclose(f);
    if (!ok || (f && old == s_scan.hash && oldn == s_scan.found)) { remove(INDEX ".tmp"); return 1; }
    remove(INDEX);
    return rename(INDEX ".tmp", INDEX) == 0 ? 2 : 1;
}

// ── the lists ───────────────────────────────────────────────────────────────────────────────────────
static int cmp_nat(const void *a, const void *b)
{ return strcasecmp(native_title(*(const int16_t *)a), native_title(*(const int16_t *)b)); }

static void shelf_free(void) { free(s_sh); s_sh = nullptr; }

// The open console's cartridges whose name contains `filt`, from the index, into a new shelf sized to fit.
struct Gather { uint16_t off[MAXR], dir[MAXR]; uint32_t hash[MAXR]; uint8_t snap[MAXR]; char pool[POOL]; int n, used; };
static bool shelf_build(const char *filt)
{
    int count = 0;
    FILE *f = idx_open(&count, nullptr);
    Gather *g = (Gather *)malloc(sizeof *g);
    if (!g) { if (f) fclose(f); return false; }
    g->n = g->used = 0;
    int total = 0, dir_at = -1;                         // the current folder's place in the pool (-1 = not yet)
    if (f) {
        char line[PATHMAX + 8], dir[PATHMAX];
        dir[0] = '\0';
        const char *name;
        int sys, seen = 0;
        char last_dir[PATHMAX] = "";
        while (idx_next(f, line, sizeof line, dir, sizeof dir, &sys, &name)) {
            if ((++seen & 255) == 0) esp_task_wdt_reset();
            if (sys < 0) {                              // a screenshot: whose, in this folder?
                const size_t pl = strlen(name);
                const uint8_t bit = !strcasecmp(name + pl - 4, ".pic") ? SN_PIC : SN_SNP;
                const uint32_t k = fnvn(name, pl - 4);
                for (int i = g->n - 1; i >= 0; i--)
                    if (g->hash[i] == k && !strcmp(g->pool + g->dir[i], dir)) { g->snap[i] |= bit; break; }
                continue;
            }
            if (sys != s_group || !contains(name, filt)) continue;
            total++;
            const int nl = (int)strlen(name) + 1;
            if (strcmp(dir, last_dir)) {                // a new folder: its name goes in the pool once
                snprintf(last_dir, sizeof last_dir, "%s", dir);
                const int dl = (int)strlen(dir) + 1;
                if (g->n < MAXR && g->used + dl + nl <= POOL) { memcpy(g->pool + g->used, dir, dl); dir_at = g->used; g->used += dl; }
                else dir_at = -1;
            }
            if (dir_at < 0 || g->n >= MAXR || g->used + nl > POOL) continue;
            memcpy(g->pool + g->used, name, nl);
            g->off[g->n] = (uint16_t)g->used; g->dir[g->n] = (uint16_t)dir_at;
            g->hash[g->n] = fnv(name); g->snap[g->n] = 0; g->n++;
            g->used += nl;
        }
        fclose(f);
    }
    // By title: the order is sorted on an index array so each name keeps its folder.
    const int n = g->n;
    uint16_t *ord = n ? (uint16_t *)malloc((size_t)n * 2) : nullptr;
    for (int i = 0; ord && i < n; i++) ord[i] = (uint16_t)i;
    static const Gather *s_g;
    s_g = g;
    if (ord && n > 1)
        qsort(ord, n, 2, [](const void *a, const void *b) {
            return strcasecmp(s_g->pool + s_g->off[*(const uint16_t *)a], s_g->pool + s_g->off[*(const uint16_t *)b]);
        });
    Shelf *sh = (n == 0 || ord) ? (Shelf *)malloc(sizeof(Shelf) + (size_t)n * 7 + g->used) : nullptr;
    if (sh) {
        sh->n = n; sh->total = total;
        snprintf(sh->with, sizeof sh->with, "%s", filt);
        sh->off = (uint16_t *)(sh + 1);
        sh->dir = sh->off + n;
        sh->view = sh->dir + n;
        sh->snap = (uint8_t *)(sh->view + n);
        sh->pool = (char *)(sh->snap + n);
        for (int i = 0; i < n; i++) { sh->off[i] = g->off[ord[i]]; sh->dir[i] = g->dir[ord[i]]; sh->snap[i] = g->snap[ord[i]]; }
        memcpy(sh->pool, g->pool, g->used);
    }
    free(ord);
    free(g);
    if (!sh) return false;
    shelf_free();
    s_sh = sh;
    return true;
}

// The list the current level shows, under the current search.
static void refilter(void)
{
    s_n = 0;
    if (s_level == 0) {
        for (int k = 0; k < s_ngrp; k++) if (contains(group_title(s_grp[k]), s_filter)) s_ix[s_n++] = (int16_t)k;
        s_total = s_n;
    } else if (s_group == NATIVE) {
        for (int k = 0; k < s_nnat; k++) if (contains(native_title(s_nat[k]), s_filter)) s_ix[s_n++] = (int16_t)k;
        s_total = s_n;
    } else {
        // In RAM when the shelf holds every match of a search this one narrows; else read again.
        const bool fits = s_sh && s_sh->n == s_sh->total && contains(s_filter, s_sh->with);
        if (!fits && !shelf_build(s_filter)) { s_total = 0; s_sel = 0; return; }
        for (int i = 0; i < s_sh->n; i++)
            if (contains(s_sh->pool + s_sh->off[i], s_filter)) s_sh->view[s_n++] = (uint16_t)i;
        s_total = s_sh->n == s_sh->total ? s_n : s_sh->total;
    }
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    if (s_sel < 0) s_sel = 0;
}

// What GameShots holds, read ONCE per opening (one listing): a missed fopen in the big /data folder costs ~0.5 s.
enum { CV_BMP = 1, CV_PNG = 2, CV_JPG = 4 };       // in this order of preference: a cover captured on the device (bmp) wins
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

// The systems: the native games when there are any, then each console whose emulator is installed and that has
// cartridges on the card. An installed emulator with none anywhere still shows its first console, which says so.
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
    int roms[NSYS] = { 0 };
    FILE *f = idx_open(nullptr, nullptr);
    if (f) {
        char line[PATHMAX + 8], dir[PATHMAX];
        const char *name;
        int sys;
        while (idx_next(f, line, sizeof line, dir, sizeof dir, &sys, &name)) roms[sys]++;
        fclose(f);
    }
    for (int o = 0; o < NSYS; o++) {
        const int g = SHOW_ORDER[o];
        if (find_app(SYS[g].app) < 0) continue;
        bool any = false;                          // any cartridge for this emulator at all?
        for (int h = 0; h < NSYS; h++) if (!strcmp(SYS[h].app, SYS[g].app) && roms[h] > 0) any = true;
        bool first = true;                         // this emulator's first console
        for (int h = 0; h < g; h++) if (!strcmp(SYS[h].app, SYS[g].app)) first = false;
        if (roms[g] > 0 || (!any && first)) { s_grp_n[s_ngrp] = roms[g]; s_grp[s_ngrp++] = g; }
    }
}

// ── memory across visits ────────────────────────────────────────────────────────────────────────────
static uint32_t key_of(int i)            // what identifies title i across reboots
{
    if (s_level == 0) { const int g = s_grp[s_ix[i]]; return g == NATIVE ? 1u : 2u + (uint32_t)g; }
    if (s_group == NATIVE) return fnv(nucleo_app_at(s_nat[s_ix[i]])->id);
    return fnv(rom_name(i), fnv(rom_dir(i)));
}
static void last_load(void)
{
    FILE *f = fopen(LAST_FILE, "rb");
    const bool ok = f && fread(&s_last, 1, sizeof s_last, f) == sizeof s_last && !memcmp(s_last.magic, "GFL1", 4);
    if (f) fclose(f);
    if (!ok || s_last.group < NATIVE || s_last.group >= NSYS) { memset(&s_last, 0, sizeof s_last); s_last.group = NATIVE; }
}
static void last_save(void)
{
    memcpy(s_last.magic, "GFL1", 4);
    FILE *f = fopen(LAST_FILE, "wb");
    if (f) { fwrite(&s_last, 1, sizeof s_last, f); fclose(f); }
}
static bool is_last(int i)
{
    if (s_level == 0) return s_grp[s_ix[i]] == s_last.group && s_last.key[s_last.group + 1];
    return s_last.key[s_group + 1] && key_of(i) == s_last.key[s_group + 1];
}
static bool wheel_mark(int i, void *) { return is_last(i); }

// The return trip across a game's Solo reboot: the level, the system, the search and the title survive in RTC memory,
// so the full OS that boots after the game reopens the gallery where the player was.
#define RET_MAGIC 0x47465254u   // 'GFRT'
struct Ret { uint32_t magic; int8_t level, group; char filter[FILT + 1]; uint32_t key; };
RTC_NOINIT_ATTR static Ret s_ret;
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

// ── sounds ──────────────────────────────────────────────────────────────────────────────────────────
// Short and quiet, at a fraction of the system volume (muted = silent): into a system, back, a game starting.
#define SFX(n, v) nucleo_audio_sample(SFX_##n, (int)(sizeof SFX_##n / sizeof SFX_##n[0]), v)

// ── the turn ────────────────────────────────────────────────────────────────────────────────────────
// While the wheel turns, ONLY the wheel is redrawn: its rectangle (lg::frame_rect), a third of the panel, no card
// read, no Cardputer. Each step slides the titles through the fixed pill (TURN_US, ease-out) with the emulators' tick.
// When it has stopped for SETTLE_US the rest follows in one whole frame: the picture, the caption, the position and
// the Cardputer's details - read and drawn once per stop, not once per step (as in HyperSpin).
#define TURN_US   140000
#define SETTLE_US 120000
static int64_t s_turn_t0;
static int     s_turn_dir;
static int64_t s_settle_at;                        // a whole frame is due then (0 = none)
static bool    s_full = true;                      // the next render recomposes everything
static int     s_off;                              // the titles' offset in this render
static void turn(int dir)
{
    const int64_t now = esp_timer_get_time();
    s_turn_t0 = dir ? now : 0; s_turn_dir = dir;
    s_settle_at = now + SETTLE_US;
    s_full = false;                                // this key moves the wheel only
    static int64_t s_tick;                         // the wheel's click (the emulators' own): one per frame at most
    if (now - s_tick > 60000) { s_tick = now; nucleo_audio_tone(2600, 4, 12); }
}
static int turn_offset(int pill_h)
{
    if (!s_turn_t0) return 0;
    const int64_t t = esp_timer_get_time() - s_turn_t0;
    if (t >= TURN_US) { s_turn_t0 = 0; return 0; }
    const int rest = 256 - (int)(t * 256 / TURN_US);                         // ease-out: (1 - p)^2 of the step
    return s_turn_dir * pill_h * rest * rest / 65536;
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
    if (s_last.key[g + 1]) select_key(s_last.key[g + 1]);   // back on the game played last
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
// After the lists changed under the selection (the walk found something): the same title if it is still there.
static void relist(void)
{
    const uint32_t key = s_n ? key_of(s_sel) : 0;
    systems_build();
    bool still = s_level == 0;
    for (int k = 0; k < s_ngrp; k++) if (s_grp[k] == s_group) still = true;
    if (s_ngrp == 1) { s_level = 1; s_group = s_grp[0]; }
    else if (!still) { s_level = 0; s_filter[0] = '\0'; s_flen = 0; shelf_free(); }
    if (on_roms()) shelf_free();                   // read again from the new index
    refilter();
    if (key) select_key(key);
}

static void pic_forget(void);
static void marks_free(void);
static bool s_settle_frame;                        // the next render: what changes when the wheel stops (left column)
void gamefront_open(void)
{
    // Back from anything (a game, the screensaver): the whole gallery and the companion, never a leftover turn frame.
    s_full = true; s_turn_t0 = 0; s_settle_at = 0; s_settle_frame = false;
    s_cover_ready = 0;
    last_load();
    systems_build();
    covers_scan();
    pic_forget();
    uint32_t want = 0;
    if (s_restore) {                               // back from a game's Solo boot: where the player was
        s_restore = false;
        s_level = s_ret.level; s_group = s_ret.group;
        snprintf(s_filter, sizeof s_filter, "%s", s_ret.filter);
        s_sel = 0; s_sel0 = 0;
        want = s_ret.key;
    } else if (!s_opened) {                        // the first time this boot: the systems, on the one played last
        s_level = 0; s_group = NATIVE; s_sel = 0; s_sel0 = 0;
        s_filter[0] = '\0';
        want = s_last.key[s_last.group + 1] ? (s_last.group == NATIVE ? 1u : 2u + (uint32_t)s_last.group) : 0;
    }
    s_opened = true;
    s_ret.magic = 0; s_ret.key = 0;                // a snapshot is consumed by the first opening after it
    bool still = s_level == 0;                     // the system that was open may be gone: back to the systems
    for (int k = 0; k < s_ngrp; k++) if (s_grp[k] == s_group) still = true;
    if (!still) { s_level = 0; s_filter[0] = '\0'; }
    if (s_ngrp == 1) { s_level = 1; s_group = s_grp[0]; }   // one system: straight to its titles
    else if (s_ngrp == 0) { s_level = 0; s_group = NATIVE; }
    s_flen = (int)strlen(s_filter);
    shelf_free();
    refilter();
    if (want) select_key(want);
    // Look for cartridges: the first time this boot, when there is no index, or after a while.
    { struct stat ist; s_have_index = stat(INDEX, &ist) == 0 && idx_ok(); }
    if (!s_scan.done) scan_start();               // once per boot: the card may have changed while it was off
}

static bool prefetch_one(void);
bool gamefront_step(void)
{
    // The walk: freely while there is no index yet (nothing to browse); else in 5 ms slices, only when the player has been
    // still for a while - it never takes time from a turn, a stop or a key.
    const int64_t t = esp_timer_get_time();
    const bool fresh = s_scan.on && !s_have_index;
    const int r = fresh ? scan_step(15000)
                : (s_scan.on && !s_turn_t0 && !s_settle_at && t - s_input_at > SCAN_IDLE_US) ? scan_step(5000) : 0;
    if (r) s_have_index = true;
    if (r == 2) { relist(); pic_forget(); }
    if (r != 0) { s_full = true; return true; }    // the walk ended: its indicator goes, the lists may have changed
    if (s_turn_t0) return true;                    // the turn's next frame (the wheel only)
    const int64_t now = esp_timer_get_time();
    if (s_settle_at && now >= s_settle_at) { s_settle_at = 0; s_settle_frame = true; return true; }   // stopped
    if (!s_settle_at && !s_scan.on && now - s_input_at > 1500000) prefetch_one();   // well after the last key
    return false;
}
bool gamefront_scanning(void) { return s_scan.on; }

static void step(int by)                           // the wheel turns forever: past the last title, the first
{
    if (s_n <= 1) return;
    s_sel = ((s_sel + by) % s_n + s_n) % s_n;
    turn(by == 1 || by == -1 ? by : 0);           // one step slides; a jump lands at once (the wheel only, either way)
}

static int open_sel(char *launch_id, int cap)
{
    if (!s_n) return GF_NONE;
    if (s_level == 0) { enter_group(s_grp[s_ix[s_sel]]); SFX(OPEN, 40); return GF_REDRAW; }
    if (s_group == NATIVE) {
        snprintf(launch_id, cap, "%s", nucleo_app_at(s_nat[s_ix[s_sel]])->id);
        s_launch[0] = '\0';
    } else {
        snprintf(launch_id, cap, "%s", SYS[s_group].app);
        snprintf(s_launch, sizeof s_launch, "%s/%s", rom_dir(s_sel), rom_name(s_sel));
    }
    s_last.group = (int8_t)s_group;
    s_last.key[s_group + 1] = key_of(s_sel);
    last_save();
    ret_capture();
    scan_abort();
    pic_forget();
    marks_free();
    shelf_free();                                  // the game gets the RAM; gamefront_open() rebuilds the shelf
    SFX(LAUNCH, 45);
    return GF_LAUNCH;
}

const char *gamefront_launch_file(void) { return s_launch[0] ? s_launch : nullptr; }

#define SLOTS_BIG   7
#define SLOTS_SMALL 5
static int slots(void) { return d.width() >= 300 ? SLOTS_BIG : SLOTS_SMALL; }

int gamefront_key(int key, char ch, char *launch_id, int cap)
{
    s_full = true;                                 // a whole frame next, unless the key only turns the wheel (turn())
    s_input_at = esp_timer_get_time();
    if (nucleo_touch_is_row(key, ch)) {            // the wheel: a tap brings a title to the middle, the middle one opens
        const int k = ch - NUCLEO_TOUCH_ROW, i = lg::wheel_at(s_sel, s_n, slots(), k);
        if (k == slots() / 2) return open_sel(launch_id, cap);
        if (i >= 0 && i != s_sel) { s_sel = i; const int dk = k - slots() / 2; turn(dk == 1 || dk == -1 ? dk : 0); }
        return GF_REDRAW;
    }
    switch (key) {
    case NK_UP:    step(-1); return GF_REDRAW;
    case NK_DOWN:  step(1);  return GF_REDRAW;
    case NK_LEFT:  step(-5); return GF_REDRAW;
    case NK_RIGHT: step(5);  return GF_REDRAW;
    case NK_ENTER: return open_sel(launch_id, cap);
    case NK_BACK:                                  // the search first, then up a level, then out
        if (s_flen) { s_filter[0] = '\0'; s_flen = 0; refilter(); return GF_REDRAW; }
        SFX(BACK, 35);
        if (s_level == 1 && s_ngrp > 1) { leave_group(); return GF_REDRAW; }
        scan_abort(); shelf_free(); pic_forget(); marks_free();
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
// byte order. A cover's or a console photo's source fields make it go stale on its own when the source changes; a
// cartridge's screenshot (written beside its .snp by tools/fetch-rom-snaps.py) carries zeros there.
struct PicHead { char magic[4]; uint16_t w, h; uint32_t src_size, src_time; };
enum { PIC_NONE, PIC_FILE, PIC_POSTER };
static struct {
    int kind;
    FILE *f;                                       // the .pic, open while it is the selection's (unbuffered)
    int w, h;
    bool dark;                                     // a screenshot / cover (dark tile) or a console photo (light card)
    char poster[24];                               // the icon of a poster
    uint32_t key;                                  // whose picture it is
} s_pic;

static void pic_forget(void)
{
    if (s_pic.f) { fclose(s_pic.f); s_pic.f = nullptr; }
    s_pic.key = 0; s_pic.kind = PIC_NONE;
}
// Open a .pic for streaming. check: its source must be (size, time); else any source.
static bool pic_open(const char *path, bool check, uint32_t size, uint32_t time)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    setvbuf(f, nullptr, _IONBF, 0);                // rows are read in blocks: no stdio buffer
    PicHead ph;
    const bool ok = fread(&ph, 1, sizeof ph, f) == sizeof ph && !memcmp(ph.magic, "PIC1", 4) && ph.w && ph.h
                    && ph.w <= 160 && ph.h <= 160 && (!check || (ph.src_size == size && ph.src_time == time));
    if (!ok) { fclose(f); return false; }
    s_pic.f = f; s_pic.w = ph.w; s_pic.h = ph.h;
    return true;
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
// The ~11 KB inflater state goes in `state` when the caller has room for it (the canvas behind the picture): on the
// launcher's fragmented heap a fresh 11 KB block is often not there. Else it is allocated for the call.
static bool inflate_to(const uint8_t *mem, FILE *zf, size_t zn, uint8_t *out, size_t need, void *state = nullptr)
{
    tinfl_decompressor *z = state ? (tinfl_decompressor *)state : (tinfl_decompressor *)malloc(sizeof *z);
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
    if (!state) free(z);
    return ok;
}

// The shared 32 KB canvas is the scratch a picture is unpacked into (nothing is composed at this point): the
// picture at its start, and the inflater's state right behind it when both fit (*state, else null).
static uint16_t *scratch(size_t need, void **state = nullptr)
{
    M5Canvas *cv = nucleo_screen();
    if (!cv || cv->bufferLength() < need) return nullptr;
    uint8_t *b = (uint8_t *)cv->getBuffer();
    if (state) {
        const size_t at = (need + 7) & ~(size_t)7;
        *state = at + sizeof(tinfl_decompressor) <= cv->bufferLength() ? b + at : nullptr;
    }
    return (uint16_t *)b;
}
static void swap_all(uint16_t *px, size_t n) { for (size_t i = 0; i < n; i++) px[i] = (uint16_t)(px[i] >> 8 | px[i] << 8); }

// A picture with alpha (gamefront_art.h: RGB565, then 4-bit alpha two pixels a byte) blended on `bg`, in place.
static uint16_t blend565(uint16_t fg, uint16_t bg, int a)       // a = 0..15
{
    const int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (15 - a)) / 15;
    const int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (15 - a)) / 15;
    const int b = ((fg & 31) * a + (bg & 31) * (15 - a)) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline int alpha_at(const uint8_t *al, int i) { return i & 1 ? al[i >> 1] & 15 : al[i >> 1] >> 4; }
static size_t art_bytes(const Art &a) { return (size_t)a.w * a.h * 2 + ((size_t)a.w * a.h + 1) / 2; }

// A console's photo, from the firmware, blended on the theme's background, as GameShots/.cache/con_<key>_<bg>.pic:
// made once per card and theme, then streamed like any picture (it stands straight on the background, no tile).
static void art_path(const System &s, char *path, size_t n) { snprintf(path, n, "%s/con_%s_%04x.pic", CACHE, s.key, (unsigned)BG); }
#define GBIT(g) (1u << ((g) + 1))                  // a group's bit (NATIVE = bit 0)
static bool art_make(const System &s)
{
    char path[PATHMAX];
    art_path(s, path, sizeof path);
    const size_t need = art_bytes(s.art), npx = (size_t)s.art.w * s.art.h;
    void *state = nullptr;
    uint16_t *px = scratch(need, &state);
    if (!px || !inflate_to(s.art.z, nullptr, s.art.n, (uint8_t *)px, need, state)) return false;
    const uint8_t *al = (const uint8_t *)(px + npx);
    for (size_t i = 0; i < npx; i++) px[i] = blend565(px[i], BG, alpha_at(al, (int)i));
    swap_all(px, npx);
    mkdir(GF_DIR, 0775); mkdir(CACHE, 0775);
    return pic_write(path, px, s.art.w, s.art.h, s.art.n, 1);
}
static uint8_t s_art_ready;                        // groups whose photo file is known to be made (GBIT)
static bool art_pic(int g)
{
    const System &s = sysg(g);
    char path[PATHMAX];
    art_path(s, path, sizeof path);
    if (pic_open(path, true, s.art.n, 1)) { s_art_ready |= GBIT(g); return true; }
    if (!art_make(s)) return false;
    s_art_ready |= GBIT(g);
    return pic_open(path, true, s.art.n, 1);
}

// A cartridge's screenshot: <folder>/.snaps/<file>.pic, or its .snp ("SNP1", u16 w, u16 h, u32 deflated size,
// raw-deflated RGB565 in native byte order) turned into that .pic once.
static bool snap_make(const char *dir, const char *rom)
{
    char path[PATHMAX + 48];
    snprintf(path, sizeof path, "%s/.snaps/%s.pic", dir, rom);
    char snp[PATHMAX + 48];
    snprintf(snp, sizeof snp, "%s/.snaps/%s.snp", dir, rom);
    FILE *f = fopen(snp, "rb");
    if (!f) return false;
    uint8_t hd[12];
    bool ok = false;
    if (fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, "SNP1", 4)) {
        const int sw = hd[4] | hd[5] << 8, sh = hd[6] | hd[7] << 8;
        const uint32_t zn = hd[8] | hd[9] << 8 | (uint32_t)hd[10] << 16 | (uint32_t)hd[11] << 24;
        void *state = nullptr;
        uint16_t *px = sw > 0 && sh > 0 && sw <= 160 && sh <= 160 ? scratch((size_t)sw * sh * 2, &state) : nullptr;
        if (px && zn && inflate_to(nullptr, f, zn, (uint8_t *)px, (size_t)sw * sh * 2, state)) {
            swap_all(px, (size_t)sw * sh);
            fclose(f); f = nullptr;
            ok = pic_write(path, px, sw, sh, 0, 0);
        }
    }
    if (f) fclose(f);
    return ok;
}
static bool snap_pic(const char *dir, const char *rom, uint8_t have)
{
    char path[PATHMAX + 48];
    snprintf(path, sizeof path, "%s/.snaps/%s.pic", dir, rom);
    if ((have & (SN_PIC | SN_SNP)) && pic_open(path, false, 0, 0)) return true;   // also one made since the walk
    return (have & SN_SNP) && snap_make(dir, rom) && pic_open(path, false, 0, 0);
}

// Pictures made AHEAD, well after the last key: the console photos of the systems and the covers of the native games
// around the selection, one per call (each is made once per card). A cartridge's screenshot is not: making one takes
// ~100-300 ms that a key pressed meanwhile would wait for - it is made when the wheel stops on it (once), or comes
// ready from tools/fetch-rom-snaps.py.
static bool cover_pic(int k, int bw, int bh, bool open = true);
static bool prefetch_one(void)
{
    if (s_level == 0) {
        for (int k = 0; k < s_ngrp; k++) {
            const int g = s_grp[k];
            if (s_art_ready & GBIT(g)) continue;
            char path[PATHMAX]; struct stat st;
            art_path(sysg(g), path, sizeof path);
            if (stat(path, &st) != 0 && !art_make(sysg(g))) { s_art_ready |= GBIT(g); continue; }   // could not: let be
            s_art_ready |= GBIT(g);
            return true;
        }
        return false;
    }
    if (s_level == 1 && s_group == NATIVE) {         // the covers of the games around the selection
        for (int dist = 0; dist <= 4 && dist < s_n; dist++)
            for (int side = 0; side < 2; side++) {
                const int k = s_ix[((s_sel + (side ? -dist : dist)) % s_n + s_n) % s_n];
                if (!s_nat_cover[k] || (s_cover_ready & (1u << k))) continue;
                s_cover_ready |= 1u << k;          // made, or not makeable: either way not again this visit
                cover_pic(k, 112, 63, false);
                return true;
            }
        return false;
    }
    return false;
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
static bool cover_pic(int k, int bw, int bh, bool open)
{
    static const char *const EXT[3] = { "bmp", "png", "jpg" };
    const char *id = nucleo_app_at(s_nat[k])->id;
    char src[96], path[PATHMAX];
    int e = 0;
    while (e < 3 && !(s_nat_cover[k] & (1 << e))) e++;
    if (e == 3) return false;
    snprintf(src, sizeof src, "%s/%s.%s", GF_DIR, id, EXT[e]);
    struct stat st;
    if (stat(src, &st) != 0) return false;
    snprintf(path, sizeof path, "%s/%s_%dx%d.pic", CACHE, id, bw, bh);
    if (open && pic_open(path, true, (uint32_t)st.st_size, (uint32_t)st.st_mtime)) return true;
    if (!open) {                                   // made ahead: is it there already?
        FILE *f = fopen(path, "rb");
        PicHead ph;
        const bool there = f && fread(&ph, 1, sizeof ph, f) == sizeof ph && !memcmp(ph.magic, "PIC1", 4)
                           && ph.src_size == (uint32_t)st.st_size && ph.src_time == (uint32_t)st.st_mtime;
        if (f) fclose(f);
        if (there) return true;
    }
    M5Canvas strip;
    strip.setPsram(false);
    strip.setColorDepth(16);
    int rows = 24;
    while (rows >= 4 && !strip.createSprite(bw, rows)) rows /= 2;
    if (rows < 4) return false;
    mkdir(GF_DIR, 0775); mkdir(CACHE, 0775);
    char tmp[PATHMAX + 8];
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
        if (e == 0)      strip.drawBmp(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        else if (e == 1) strip.drawPng(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        else             strip.drawJpg(&dw, 0, -y0, bw, bh, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        ok = fwrite(strip.getBuffer(), 2, (size_t)bw * rn, f) == (size_t)bw * rn;
    }
    if (f) ok = fclose(f) == 0 && ok;
    dw.close();
    strip.deleteSprite();
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    else remove(tmp);
    return ok && (!open || pic_open(path, true, (uint32_t)st.st_size, (uint32_t)st.st_mtime));
}

// The selection's picture, worked out once per selection (and per screen size): an open .pic, or a poster.
static void pic_prepare(bool big)
{
    if (!s_n) { pic_forget(); return; }
    const uint32_t key = key_of(s_sel) ^ (uint32_t)(s_level << 24) ^ (uint32_t)((s_group + 2) << 20) ^ (big ? 0x80000000u : 0);
    if (s_pic.key == key) return;
    pic_forget();
    s_pic.key = key;
    s_pic.kind = PIC_POSTER;
    s_pic.dark = true;
    bool file = false;
    if (s_level == 0) {
        const int g = s_grp[s_ix[s_sel]];
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", g == NATIVE ? "Games" : SYS[g].app);
        if (art_pic(g)) { file = true; s_pic.dark = false; }
    } else if (s_group == NATIVE) {
        const int k = s_ix[s_sel];
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", nucleo_app_at(s_nat[k])->id);
        file = cover_pic(k, 112, 63);
    } else {
        snprintf(s_pic.poster, sizeof s_pic.poster, "%s", SYS[s_group].app);
        uint8_t &have = s_sh->snap[s_sh->view[s_sel]];
        file = snap_pic(rom_dir(s_sel), rom_name(s_sel), have);
        if (file) have |= SN_PIC;                  // made now: no conversion next time
        else have = 0;                             // none, or it could not be made: not tried again this visit
        if (!file && art_pic(s_group)) { file = true; s_pic.dark = false; }
    }
    if (file) s_pic.kind = PIC_FILE;
}

static uint16_t mix565(uint16_t a, uint16_t b, int t)   // t = 0 (a) .. 256 (b)
{
    const int r = (((a >> 11) & 31) * (256 - t) + ((b >> 11) & 31) * t) >> 8;
    const int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    const int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// The picture in its place (x, y, w, h): the .pic's rows that fall inside the strip being composed are read straight
// into it, four at a time; a poster is the item's accent deepening downwards with its icon.
static void draw_pic(int x, int y, int w, int h, unsigned short acc)
{
    if (s_pic.kind == PIC_FILE && s_pic.f) {         // on the background itself: no tile around it
        const int px = x + (w - s_pic.w) / 2, py = y + (h - s_pic.h) / 2;
        int32_t cx, cy, cw, ch;
        d.getClipRect(&cx, &cy, &cw, &ch);
        const int y0 = py > cy ? py : cy, y1 = py + s_pic.h < cy + ch ? py + s_pic.h : cy + ch;
        if (y0 < y1 && fseek(s_pic.f, (long)(sizeof(PicHead) + (size_t)(y0 - py) * s_pic.w * 2), SEEK_SET) == 0) {
            uint16_t rows[4 * 160];
            for (int ry = y0; ry < y1; ) {
                const int k = y1 - ry < 4 ? y1 - ry : 4;
                if (fread(rows, 2, (size_t)s_pic.w * k, s_pic.f) != (size_t)s_pic.w * k) break;
                d.pushImage(px, ry, s_pic.w, k, (const lgfx::swap565_t *)rows);
                ry += k;
            }
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

// ── words ───────────────────────────────────────────────────────────────────────────────────────────
// The region tag of a No-Intro name, "(USA, Europe)" -> "USA, Europe"; false when there is none.
static bool region_of(const char *file, char *out, size_t n)
{
    const char *open = strchr(file, '(');
    const char *close = open ? strchr(open, ')') : nullptr;
    if (!close || close - open < 2) return false;
    snprintf(out, n, "%.*s", (int)(close - open - 1), open + 1);
    return true;
}
static void count_games(char *out, size_t n, int c)
{
    snprintf(out, n, "%d %s", c, c == 1 ? TR5("gioco", "game", "juego", "jeu", "Spiel") : TR5("giochi", "games", "juegos", "jeux", "Spiele"));
}
// The line under the picture: a system's count, a cartridge's region (or console), a native game's play modes.
static void caption(char *out, size_t n)
{
    out[0] = '\0';
    if (!s_n) return;
    if (s_level == 0) { count_games(out, n, s_grp_n[s_ix[s_sel]]); return; }
    if (s_group != NATIVE) { region_of(rom_name(s_sel), out, n); return; }   // beside the console's mark
    const Meta *m = meta_of(nucleo_app_at(s_nat[s_ix[s_sel]])->id);
    const unsigned modes = m ? (unsigned)m->modes : (unsigned)M1P;
    const char *lab[5] = { TR5("1 giocatore", "1 player", "1 jugador", "1 joueur", "1 Spieler"), "vs CPU",
                           TR5("2 in locale", "2 local", "2 en local", "2 en local", "2 lokal"), "LAN", "co-op" };
    for (int b = 0; b < 5; b++)
        if (modes & (1 << b)) { if (out[0]) strncat(out, " - ", n - strlen(out) - 1); strncat(out, lab[b], n - strlen(out) - 1); }
}
// A muted line under the title: what the native games are.
static const char *subtitle(void)
{
    if (!s_n || s_level != 0 || s_grp[s_ix[s_sel]] != NATIVE) return nullptr;
    return TR5("Integrati + Store", "Built in + Store", "Integrados + Store", "Integres + Store", "Eingebaut + Store");
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
    if (s_scan.on) return TR5("Cerco i giochi sulla SD...", "Looking for games on the card...", "Buscando juegos en la SD...",
                              "Recherche des jeux sur la carte...", "Suche Spiele auf der Karte...");
    if (s_level == 0) return TR5("Nessun gioco", "No games", "Sin juegos", "Aucun jeu", "Keine Spiele");
    if (s_group == NATIVE) return TR5("Nessun gioco nativo", "No native games", "Sin juegos nativos", "Aucun jeu natif", "Keine nativen Spiele");
    return TR5("Nessuna ROM sulla SD", "No ROMs on the card", "Ninguna ROM en la SD", "Aucune ROM sur la carte", "Keine ROMs auf der Karte");
}
static const char *empty_sub(void)
{
    if (s_flen) return TR5("Canc per correggere", "Backspace to edit", "Retroceso para editar", "Retour pour modifier", "Ruecktaste zum Bearbeiten");
    if (s_scan.on) return "";
    if (s_level == 1 && s_group != NATIVE)
        return TR5("Copia le ROM dove vuoi sulla SD: le trovo da sola", "Copy the ROMs anywhere on the card: they are found on their own",
                   "Copia las ROM donde quieras en la SD: se encuentran solas", "Copiez les ROM n'importe ou sur la carte : elles sont trouvees",
                   "ROMs irgendwohin auf die Karte kopieren: sie werden gefunden");
    return TR5("Si installano dallo Store", "Install them from the Store", "Se instalan desde la Store", "A installer depuis le Store", "Aus dem Store installieren");
}
static const char *level_title(void)
{
    if (s_level == 0) return TR5("Giochi", "Games", "Juegos", "Jeux", "Spiele");
    return group_title(s_group);
}
static const char *search_word(void) { return TR5("Cerca", "Search", "Buscar", "Chercher", "Suchen"); }
static const char *search_hint(void) { return TR5("Scrivi per cercare", "Type to search", "Escribe para buscar", "Tapez pour chercher", "Tippen zum Suchen"); }
static void count_text(char *out, size_t n)
{
    if (!s_n)                 snprintf(out, n, "0");
    else if (s_total > s_n)   snprintf(out, n, "%d/%d+", s_sel + 1, s_n);
    else                      snprintf(out, n, "%d/%d", s_sel + 1, s_n);
}
static const char *wheel_label(int i, void *) { return title_of(i); }
static const char *last_label(void) { return TR5("ultimo giocato", "last played", "ultimo jugado", "dernier joue", "zuletzt gespielt"); }

// ── the consoles' marks ─────────────────────────────────────────────────────────────────────────────
// A mark is ~1.2 KB once inflated (colour + alpha): they are kept while the gallery is open (the systems' wheel shows
// them all) and freed when it closes or a game starts. Blended at draw time: on the background or on the pill.
static uint16_t *s_mark[NSYS + 1];                 // by group + 1 (the native games' mark first)
static bool s_mark_failed[NSYS + 1];                  // a mark that could not be made is not retried while the gallery is open
static void marks_free(void) { for (uint16_t *&m : s_mark) { free(m); m = nullptr; } memset(s_mark_failed, 0, sizeof s_mark_failed); }
// The marks the screen shows, made before a frame is composed (the canvas is free then: the inflater's state goes
// there, and a frame never waits on a failed allocation).
static void marks_prepare(void)
{
    for (int k = 0; k < s_ngrp; k++) {
        const int g = s_grp[k];
        if (s_mark[g + 1] || s_mark_failed[g + 1]) continue;
        const Art &a = sysg(g).mark;
        const size_t need = art_bytes(a);
        void *state = nullptr;
        uint16_t *sc = scratch(need, &state);
        uint16_t *px = (uint16_t *)malloc(need);
        if (px && sc && inflate_to(a.z, nullptr, a.n, (uint8_t *)sc, need, state)) { memcpy(px, sc, need); s_mark[g + 1] = px; }
        else { free(px); s_mark_failed[g + 1] = true; }
    }
}
// The console's mark at (x, y), centred in a 20 px band, blended on `bg`; returns its width.
static int chip(int g, int x, int y, uint16_t bg = BG)
{
    const Art &a = sysg(g).mark;
    const uint16_t *mk = s_mark[g + 1];
    if (mk) {
        const uint8_t *al = (const uint8_t *)(mk + a.w * a.h);
        uint16_t row[32];
        for (int r = 0; r < a.h; r++) {
            for (int i = 0; i < a.w; i++) row[i] = blend565(mk[r * a.w + i], bg, alpha_at(al, r * a.w + i));
            d.pushImage(x, y + (20 - a.h) / 2 + r, a.w, 1, (const lgfx::rgb565_t *)row);
        }
    }
    return a.w;
}
static int wheel_icon(int i, int x, int cy, bool mid, void *)
{
    if (s_level != 0) return 0;
    const int g = s_grp[s_ix[i]];
    return chip(g, x, cy - 10, mid ? acc_now() : BG);
}

// ── the screens ─────────────────────────────────────────────────────────────────────────────────────
static const unsigned char WHEEL_BIG[SLOTS_BIG] = { 21, 23, 25, 32, 25, 23, 21 };
static const unsigned char WHEEL_SMALL[SLOTS_SMALL] = { 19, 21, 27, 21, 19 };

// The header's right side: a pulsing "searching" dot while the card is walked.
static const char *header_right(void)
{
    return s_scan.on ? TR5("cerco...", "searching...", "buscando...", "recherche...", "suche...") : nullptr;
}

// The panel, 320x240: header with Back, search, then the picture and the title on the left and the wheel on the right.
#define WX 128                                     // the wheel's left edge
static void paint_left(void *);
static void paint_big(void *)
{
    const int W = d.width(), H = s_H;
    const unsigned short acc = acc_now();
    char cnt[24]; count_text(cnt, sizeof cnt);
    int32_t cx0, cy0, cw0, ch0;
    d.getClipRect(&cx0, &cy0, &cw0, &ch0);
    const int top = lg::HEAD + 36, bh = H - top;
    if (cy0 < top) {                               // only the strips that hold the header and the search draw them
        lg::header(level_title(), header_right(), acc_of(s_level ? s_group : NATIVE), NK_BACK);
        lg::search(0, lg::HEAD, W, 36, s_filter, search_hint(), cnt, s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, acc);
    } else nucleo_touch_add(0, 0, 96, lg::HEAD + 2, NK_BACK, 0);   // the header's Back, as lg::header registers it
    d.fillRect(0, top, W, bh, BG);
    if (!s_n) { lg::message(0, top, W, bh, empty_line(), empty_sub(), MUTED); return; }
    paint_left(nullptr);
    const lg::Wheel wl = { SLOTS_BIG, WHEEL_BIG, s_n, s_sel, wheel_label, wheel_mark, nullptr, acc, C_LAST, wheel_icon, s_off };
    lg::wheel(WX, top, W - WX, bh, wl);
}

// The left column below the search: the picture, the caption, the title. Also a frame of its own (lg::frame_rect at
// x 0) when the wheel stops: only this and the position change then.
static void paint_left(void *)
{
    const int H = s_H, top = lg::HEAD + 36;
    const unsigned short acc = acc_now();
    d.fillRect(0, top, WX, H - top, BG);
    const int tw = 118, tx = (WX - tw) / 2, ph = 102;   // the picture's place
    draw_pic(tx, top + 2, tw, ph, acc);
    nucleo_touch_add(tx, top + 2, tw, ph, NK_ENTER, 0);   // the picture opens the selection too
    char cap[64]; caption(cap, sizeof cap);
    int ly = top + ph + 5;
    lg::font(0);
    char fitted[64];
    int cx = tx + 2;
    if (on_roms()) cx += chip(s_group, tx + 2, ly - 2) + 6;   // a cartridge's console: its mark
    if (is_last(s_sel) && s_level == 1) {          // the game played last says so in place of its caption
        d.fillCircle(cx + 4, ly + 8, 3, C_LAST);
        d.setTextColor(C_LAST, BG); d.drawString(last_label(), cx + 12, ly);
    } else {
        lg::fit(fitted, sizeof fitted, cap, tx + tw - cx);
        d.setTextColor(acc, BG); d.drawString(fitted, cx, ly);
    }
    ly += 21;
    const char *sub = subtitle();
    lg::font(1); d.setTextColor(FG, BG);
    const int lines = lg::wrap(title_of(s_sel), tx + 2, ly, WX - tx - 6, 19, sub ? 1 : (H - ly >= 58 ? 3 : 2));
    if (sub) { lg::font(0); d.setTextColor(MUTED, BG); lg::fit(fitted, sizeof fitted, sub, WX - tx - 6); d.drawString(fitted, tx + 2, ly + lines * 19 + 1); }
}
static void paint_search(void *)
{
    char cnt[24]; count_text(cnt, sizeof cnt);
    lg::search(0, lg::HEAD, 320, 36, s_filter, search_hint(), cnt, s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, acc_now());
}

// The wheel alone, in its own rectangle (lg::frame_rect: x is relative to WX), while it turns.
static void paint_wheel_big(void *)
{
    const int top = lg::HEAD + 36;
    const lg::Wheel wl = { SLOTS_BIG, WHEEL_BIG, s_n, s_sel, wheel_label, wheel_mark, nullptr, acc_now(), C_LAST, wheel_icon, s_off };
    lg::wheel(0, top, 320 - WX, s_H - top, wl);
}

// The Cardputer alone, 240x135: the picture with its caption and the whole title on the left; the search and a
// five-title wheel on the right.
#define CX 118
static void paint_small(void *)
{
    const int W = d.width(), H = s_H;
    const unsigned short acc = acc_now();
    d.fillRect(0, 0, W, H, BG);
    char cnt[24]; count_text(cnt, sizeof cnt);
    lg::search(CX, 0, W - CX, 22, s_filter, s_scan.on ? header_right() : search_word(), nullptr, MUTED, acc);
    if (!s_n) {
        lg::message(0, 22, W, H - 22, empty_line(), empty_sub(), MUTED);
        return;
    }
    const int ph = 103;
    draw_pic(1, 1, CX - 2, ph, acc);
    char cap[48]; caption(cap, sizeof cap);
    char fitted[48];
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextDatum(top_right); d.setTextColor(s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, BG);
    d.drawString(cnt, CX - 3, ph + 4);                 // the position: beside the caption, the search line is narrow
    const int cw = d.textWidth(cnt) + 6;
    d.setTextDatum(top_left);
    // First row: the console's mark (a cartridge's, or the system's itself), then the caption or "last played".
    const int g0 = s_level == 0 ? s_grp[s_ix[s_sel]] : NATIVE, mg = on_roms() ? s_group : g0;
    int cx = 3, ry = ph + 4;
    if (mg != NATIVE) { cx = 6 + chip(mg, 2, ph + 3); ry = ph + 9; }
    if (is_last(s_sel) && s_level == 1) {
        d.fillCircle(cx + 2, ry + 3, 2, C_LAST); d.setTextColor(C_LAST, BG); d.drawString(last_label(), cx + 7, ry);
    } else {
        lg::fit(fitted, sizeof fitted, cap, CX - 6 - cw - cx);
        d.setTextColor(acc, BG); d.drawString(fitted, cx, ry);
    }
    // Then the whole title.
    const char *sub = subtitle();
    const int ty = mg != NATIVE ? ph + 23 : ph + 14;
    d.setTextColor(FG, BG);
    const int nl = lg::wrap(title_of(s_sel), 3, ty, CX - 6, 9, mg != NATIVE || sub ? 1 : 2);
    if (sub) { d.setTextColor(MUTED, BG); lg::fit(fitted, sizeof fitted, sub, CX - 6); d.drawString(fitted, 3, ty + nl * 9); }
    const lg::Wheel wl = { SLOTS_SMALL, WHEEL_SMALL, s_n, s_sel, wheel_label, wheel_mark, nullptr, acc, C_LAST, wheel_icon, s_off };
    lg::wheel(CX, 22, W - CX, H - 22, wl);
}

static void paint_wheel_small(void *)
{
    const lg::Wheel wl = { SLOTS_SMALL, WHEEL_SMALL, s_n, s_sel, wheel_label, wheel_mark, nullptr, acc_now(), C_LAST, wheel_icon, s_off };
    lg::wheel(0, 22, 240 - CX, s_H - 22, wl);
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
        const int k = s_ix[s_sel], g = s_grp[k];
        count_games(t, sizeof t, s_grp_n[k]);
        add(TR5("Giochi", "Games", "Juegos", "Jeux", "Spiele"), t);
        if (s_last.group == g && s_last.key[g + 1]) add(TR5("Ultimo", "Last", "Ultimo", "Dernier", "Zuletzt"), TR5("giocato qui", "played here", "jugado aqui", "joue ici", "hier gespielt"));
        return n;
    }
    if (s_group == NATIVE) {
        caption(t, sizeof t);
        add(TR5("Giocatori", "Players", "Jugadores", "Joueurs", "Spieler"), t);
        add(TR5("Origine", "Source", "Origen", "Origine", "Herkunft"),
            nucleo_store_app_is_native(nucleo_app_at(s_nat[s_ix[s_sel]])->id) ? "Store" : TR5("Integrato", "Built in", "Integrado", "Integre", "Eingebaut"));
        return n;
    }
    const char *file = rom_name(s_sel);
    if (region_of(file, t, sizeof t)) add(TR5("Regione", "Region", "Region", "Region", "Region"), t);
    snprintf(t, sizeof t, "%s/", rom_dir(s_sel) + strlen(NUCLEO_SD_MOUNT));
    add(TR5("Cartella", "Folder", "Carpeta", "Dossier", "Ordner"), t);
    const char *dot = strrchr(file, '.');
    snprintf(t, sizeof t, "%s", dot ? dot + 1 : "");
    for (char *p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (s_sh->snap[s_sh->view[s_sel]]) strncat(t, TR5("  con immagine", "  with picture", "  con imagen", "  avec image", "  mit Bild"), sizeof t - strlen(t) - 1);
    add(TR5("File", "File", "Archivo", "Fichier", "Datei"), t);
    return n;
}

// The Cardputer while the gallery is on the panel: the selection's card — its whole title, what it is (system,
// region, folder; players, source and blurb; count and maker) — the search while typing, the keys and the position.
// The wheel and the picture stay on the panel.
static int s_ctop, s_cch;                          // the companion's content area (asked before the frame)
static void paint_companion(void *)
{
    const int top = s_ctop, ch = s_cch, bottom = top + ch - 13;
    const unsigned short acc = acc_now();
    d.fillRect(0, 0, 240, 135, BG);                    // the whole screen: every row of the frame is pushed
    int y = lg::comp_head(level_title(), acc_of(s_level ? s_group : NATIVE));
    if (s_flen) {                                      // the search, while there is one
        d.setFont(&fonts::Font2); d.setTextColor(MUTED, BG); d.drawString(search_word(), 8, y);
        const int lx = 14 + d.textWidth(search_word());
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
        if (is_last(s_sel) && s_level == 1 && y + 10 <= bottom) {
            d.setFont(&fonts::Font0); d.fillCircle(11, bottom - 6, 2, C_LAST); d.setTextColor(C_LAST, BG); d.drawString(last_label(), 17, bottom - 10);
        }
    }
    d.setFont(&fonts::Font0);
    lg::comp_keys(s_level == 0 ? TR5("INVIO apri  ESC esci", "ENTER open  ESC quit", "INTRO abre  ESC sale", "ENTREE ouvre  ESC sort", "ENTER oeffnet  ESC Ende")
                               : TR5("INVIO gioca  ESC indietro", "ENTER play  ESC back", "INTRO juega  ESC atras", "ENTREE joue  ESC retour", "ENTER spielt  ESC zurueck"));
    char pos[16]; count_text(pos, sizeof pos);         // where the selection is, at the right of the keys
    d.setTextDatum(top_right); d.setTextColor(s_total > s_n ? (unsigned short)C_YELLOW : (unsigned short)MUTED, BG);
    d.drawString(s_scan.on ? header_right() : pos, 232, top + ch - 11); d.setTextDatum(top_left);
}
// Composed off screen like the panel and pushed in a few strips: drawn straight, its dozens of texts and fills were
// as many transfers to the Cardputer's display on every step of the wheel.
void gamefront_draw_companion(void *gfx)
{
    nucleo_app_set_gfx((LovyanGFX *)gfx);
    s_ctop = nucleo_app_content_top(); s_cch = nucleo_app_content_height();
    lg::frame(paint_companion, nullptr, 0, 0, true);
    nucleo_app_set_gfx(nullptr);
}
static void companion_cb(void *gfx)
{
    LovyanGFX *g = (LovyanGFX *)gfx;
    g->startWrite();
    gamefront_draw_companion(gfx);
    g->endWrite();
}

void gamefront_invalidate(void) { pic_forget(); s_full = true; }

void gamefront_render(void)
{
    const bool big = d.width() >= 300, full = s_full;
    s_H = d.height();
    s_full = false;
    s_off = turn_offset(big ? WHEEL_BIG[SLOTS_BIG / 2] : WHEEL_SMALL[SLOTS_SMALL / 2]);
    if (!full && s_n && !s_settle_frame) {         // the wheel turning: its rectangle only (the committed touch targets stay)
        nucleo_touch_clear();                      // ...the wheel's own registrations go nowhere
        const bool ok = big ? lg::frame_rect(paint_wheel_big, nullptr, WX, lg::HEAD + 36, 320 - WX, s_H - lg::HEAD - 36, true)
                            : lg::frame_rect(paint_wheel_small, nullptr, CX, 22, 240 - CX, s_H - 22, true);
        if (ok) return;
        s_settle_at = 0;                           // no canvas: the whole frame instead
    }
    const bool settle = s_settle_frame && !full && big && s_n;
    s_settle_frame = false;
    const int64_t t0 = esp_timer_get_time();
    if (on_roms() && !s_sh) refilter();            // the shelf left RAM (a failed allocation): read it again
    marks_prepare();
    pic_prepare(big);
    const int64_t t1 = esp_timer_get_time();
    bool drawn = false;
    if (settle) {                                  // the wheel stopped: the picture, the caption, the title, the position
        const int top = lg::HEAD + 36;
        drawn = lg::frame_rect(paint_left, nullptr, 0, top, WX, s_H - top, true)
             && lg::frame_rect(paint_search, nullptr, 0, lg::HEAD, 320, 36, true);
    }
    if (!drawn) {
        nucleo_touch_clear();
        lg::frame(big ? paint_big : paint_small, nullptr, 0, 0, true);   // exact: the pictures and the cards in true colour
        nucleo_touch_commit();
    }
    const int64_t t2 = esp_timer_get_time();
    if (big) nucleo_extdisp_draw_internal(companion_cb);
    const int64_t t3 = esp_timer_get_time();
    if (t3 - t0 > 40000)                           // where a slow frame went (/api/logs)
        ESP_LOGW("gallery", "%s frame: picture %d ms, panel %d ms, cardputer %d ms", settle ? "stop" : "whole",
                 (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000), (int)((t3 - t2) / 1000));
}
