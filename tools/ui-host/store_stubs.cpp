// ui-host: the native Store app (app_store.cpp) over the REAL store engine (nucleo_store/store_engine.c reads
// the catalog from the sandbox SD the scene seeds); only the network task of nucleo_store.c is stubbed: a
// scene sets what it is "doing".
#include "host_state.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <direct.h>
extern "C" {
#include "nucleo_store.h"
}

extern "C" {
nst_status_t g_host_store_status = NST_IDLE;
nst_op_t     g_host_store_op = NST_OP_REFRESH;
st_state_t   g_host_store_st;
const char  *g_host_store_err = "";

bool nucleo_store_start(nst_op_t, const char *) { return false; }
nst_status_t nucleo_store_status(void) { return g_host_store_status; }
nst_op_t nucleo_store_op(void) { return g_host_store_op; }
const st_state_t *nucleo_store_state(void) { return &g_host_store_st; }
const char *nucleo_store_error(void) { return g_host_store_status == NST_FAILED ? g_host_store_err : ""; }
const char *nucleo_store_os_version(void) { return "0.8.0"; }
void nucleo_app_set_poll_handler(bool (*)(void)) {}
}

// A catalog of a dozen apps across the categories, dated relative to the scene's wall clock.
struct SeedApp { const char *id, *cat; int code; int days_ago; uint32_t size; const char *perms; const char *n[5]; const char *dsc[5]; };
static const SeedApp SEED[] = {
    { "neon-snake", "games", 3, 1, 48211, "-", { "Neon Snake", "Neon Snake", "Neon Snake", "Neon Snake", "Neon Snake" },
      { "Il serpente al neon: velocita' crescente, muri che si spostano, 20 livelli.", "The neon snake: rising speed, moving walls, 20 levels.",
        "La serpiente de neon: velocidad creciente, muros moviles, 20 niveles.", "Le serpent neon : vitesse croissante, murs mobiles, 20 niveaux.",
        "Die Neon-Schlange: steigendes Tempo, wandernde Waende, 20 Level." } },
    { "sky-pilot", "games", 1, 4, 132044, "audio", { "Sky Pilot", "Sky Pilot", "Sky Pilot", "Sky Pilot", "Sky Pilot" },
      { "Pilota un aereo tra le nuvole e raccogli il carburante.", "Fly a plane through the clouds and collect fuel.",
        "Pilota un avion entre las nubes y recoge combustible.", "Pilote un avion dans les nuages et ramasse du carburant.",
        "Flieg durch die Wolken und sammle Treibstoff." } },
    { "sudoku", "games", 2, 40, 22311, "-", { "Sudoku", "Sudoku", "Sudoku", "Sudoku", "Sudoku" },
      { "Mille griglie in quattro difficolta', con suggerimenti.", "A thousand grids in four levels, with hints.",
        "Mil cuadriculas en cuatro niveles, con pistas.", "Mille grilles en quatre niveaux, avec indices.", "Tausend Gitter in vier Stufen, mit Tipps." } },
    { "metronome", "media", 1, 2, 34715, "mic", { "Metronomo", "Metronome", "Metronomo", "Metronome", "Metronom" },
      { "Metronomo con tap tempo, accenti e suddivisioni, piu' un accordatore cromatico.", "Metronome with tap tempo, accents and subdivisions, plus a chromatic tuner.",
        "Metronomo con tap tempo, acentos y subdivisiones, y afinador.", "Metronome avec tap tempo, accents et subdivisions, plus un accordeur.",
        "Metronom mit Tap-Tempo, Akzenten und Stimmgeraet." } },
    { "unit-convert", "tools", 4, 9, 18003, "-", { "Convertitore", "Unit Converter", "Conversor", "Convertisseur", "Umrechner" },
      { "Lunghezze, pesi, temperature, valute offline.", "Lengths, weights, temperatures, offline currencies.",
        "Longitudes, pesos, temperaturas, divisas sin red.", "Longueurs, poids, temperatures, devises hors ligne.", "Laengen, Gewichte, Temperaturen, Waehrungen offline." } },
    { "qr-studio", "tools", 2, 70, 41022, "-", { "QR Studio", "QR Studio", "QR Studio", "QR Studio", "QR Studio" },
      { "Crea codici QR per Wi-Fi, contatti e link.", "Make QR codes for Wi-Fi, contacts and links.",
        "Crea codigos QR para Wi-Fi, contactos y enlaces.", "Cree des QR pour Wi-Fi, contacts et liens.", "QR-Codes fuer WLAN, Kontakte und Links." } },
    { "flashcards", "learn", 1, 3, 210300, "-", { "Flashcard", "Flashcards", "Tarjetas", "Cartes memo", "Lernkarten" },
      { "Ripasso a intervalli: lingue, capitali, formule.", "Spaced repetition: languages, capitals, formulas.",
        "Repaso espaciado: idiomas, capitales, formulas.", "Repetition espacee : langues, capitales, formules.", "Wiederholung mit Abstand: Sprachen, Hauptstaedte." } },
    { "net-scan", "network", 1, 20, 27400, "net,wifi", { "Scanner rete", "Net Scanner", "Escaner red", "Scanner reseau", "Netzscanner" },
      { "Trova i dispositivi sulla tua rete e le porte aperte.", "Find the devices on your network and their open ports.",
        "Encuentra los dispositivos de tu red y puertos abiertos.", "Trouve les appareils du reseau et leurs ports.", "Findet Geraete im Netz und offene Ports." } },
    { "notes-md", "office", 5, 12, 64200, "fs", { "Note Markdown", "Markdown Notes", "Notas Markdown", "Notes Markdown", "Markdown-Notizen" },
      { "Scrivi in Markdown dal browser, salvato sulla SD.", "Write Markdown from the browser, saved on the SD.",
        "Escribe Markdown desde el navegador, en la SD.", "Ecrivez en Markdown depuis le navigateur, sur la SD.", "Markdown im Browser schreiben, auf der SD." } },
    { "hash-tool", "security", 1, 30, 15200, "-", { "Hash", "Hash Tool", "Hash", "Hash", "Hash" },
      { "SHA-256, MD5 e CRC dei file sulla SD.", "SHA-256, MD5 and CRC of files on the SD.",
        "SHA-256, MD5 y CRC de archivos de la SD.", "SHA-256, MD5 et CRC des fichiers de la SD.", "SHA-256, MD5 und CRC von SD-Dateien." } },
    { "sys-info", "system", 1, 50, 9100, "-", { "Info sistema", "System Info", "Info sistema", "Info systeme", "Systeminfo" },
      { "Memoria, temperatura, batteria e rete in un colpo d'occhio.", "Memory, temperature, battery and network at a glance.",
        "Memoria, temperatura, bateria y red de un vistazo.", "Memoire, temperature, batterie et reseau.", "Speicher, Temperatur, Akku und Netz." } },
};

// Seeds sd/apps/.store/index.txt plus the installed state: netscan/unit-convert/notes-md installed by the
// store (notes-md and unit-convert one version behind), metronome present as a built-in app.
extern "C" void host_store_seed(bool with_catalog)
{
    _mkdir("sd"); _mkdir("sd/apps"); _mkdir("sd/apps/.store");
    if (with_catalog) {
        FILE *f = fopen("sd/apps/.store/index.txt", "wb");
        fprintf(f, "nucleoos-cardputer-index-v1\nserial 7\nexpires 1999999999\napps %d\n", (int)(sizeof SEED / sizeof SEED[0]));
        for (const SeedApp &a : SEED) {
            uint32_t day = (uint32_t)(g_host.wall / 86400 * 86400) - (uint32_t)a.days_ago * 86400;
            fprintf(f, "app\t%s\tweb\t%d\t1.%d.0\t0.8.0\t%s\t%lu\t%064d\t%s\t%s|%s|%s|%s|%s\t%s|%s|%s|%s|%s\t%lu\tNucleo Labs\n",
                    a.id, a.code, a.code, a.cat, (unsigned long)a.size, 0, a.perms,
                    a.n[0], a.n[1], a.n[2], a.n[3], a.n[4], a.dsc[0], a.dsc[1], a.dsc[2], a.dsc[3], a.dsc[4], (unsigned long)day);
        }
        fclose(f);
    }
    struct { const char *id; int code; } inst[] = { { "net-scan", 1 }, { "unit-convert", 3 }, { "notes-md", 4 } };
    for (auto &i : inst) {
        char p[96];
        snprintf(p, sizeof p, "sd/apps/%s", i.id); _mkdir(p);
        snprintf(p, sizeof p, "sd/apps/%s/.store-version", i.id);
        FILE *v = fopen(p, "wb"); fprintf(v, "code %d\nversion 1.%d.0\nkind web\n", i.code, i.code); fclose(v);
    }
    _mkdir("sd/apps/metronome");                       // a built-in app with the same id: "Built-in"
}
