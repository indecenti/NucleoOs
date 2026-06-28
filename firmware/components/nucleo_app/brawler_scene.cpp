// brawler_scene.cpp — SCORRIBANDA: piano profondita' belt + fondale MINIMAL su carta bianca.
//
// Direzione artistica: niente noir scuro. Fondo "carta" caldo-bianco (BR_PAPER), forme a SOLA LINEA in
// grigio (contorni, non riquadri pieni), molto spazio bianco, parallasse MULTI-STRATO e poche piccole
// animazioni di riposo. Niente griglia a terra: la profondita' si legge dalla SCALA dei combattenti.
// I personaggi restano silhouette nere pulite; il sangue e' l'unico rosso. Tutto via le primitive
// riusabili fxscene (ridge/props/clean_floor) — solo grigi del fondale, nessun altro colore qui.
//
// Possiede: BR_BELT (il piano profondita' su cui ogni modulo misura piedi/scala) + scene_draw().

#include "brawler.h"
#include <math.h>

// Mappatura profondita' del nastro: z=0 (lontano, piccolo, in alto) .. z=1 (vicino, grande, in basso).
//   horizonY = 60  : linea d'orizzonte, dove poggia lo skyline e inizia il terreno.
//   frontY   = 130 : bordo vicino del nastro (lascia respiro all'HUD sotto su 135px).
//   farS/nearS = 12/42 : scala figura per-unita' lontano .. vicino. RANGE AMPIO -> chi va verso il fondo
//                        rimpicciolisce in modo visibile (profondita' pronunciata, richiesta utente).
const fxfig::Belt BR_BELT = { 60.0f, 130.0f, 12.0f, 42.0f };

// Piedi (screen-y) per una profondita' — usato da sangue/ombre/ordinamento di disegno.
float scene_floor_y(float z) { return fxfig::belt_y(BR_BELT, z); }

// Scala figura per una profondita'.
float scene_scale(float z) { return fxfig::belt_s(BR_BELT, z); }

// Fondale: carta bianca -> ridge lontano -> prop di mezzo -> prop vicini -> terreno pulito (no griglia).
// Composizione back-to-front, parallasse guidata da g.camx, animazione da br_now_ms()/g.floorscroll.
// Varieta' per livello tramite i seed (g.level) e una sfumatura grigia dai campi LevelDef (ora grigi).
void scene_draw(void)
{
    const LevelDef *L = brawler_level(g.level);
    int hy = (int)BR_BELT.horizonY;

    // Tempo per le animazioni di riposo: deriva lenta dello skyline + ondeggio dei prop vicini.
    float t = br_now_ms() * 0.001f;

    // (1) base: carta bianca su tutto il cielo (sopra l'orizzonte). Tanto bianco per design.
    d.fillRect(0, 0, BR_SW, hy, BR_PAPER);

    // I grigi del fondale vengono dal LevelDef (riempito in brawler_levels): far->near si scuriscono di
    // un filo salendo di livello -> ogni stage ha la sua tonalita' restando dentro la palette.
    uint16_t c_far  = L->build_far;     // ridge lontano + base prop di mezzo
    uint16_t c_near = L->build_near;     // ridge medio
    uint16_t c_line = L->floor_line;     // prop vicini + linea d'orizzonte (grigio piu' deciso)

    // (2) ridge LONTANO: profilo basso, passo fitto, parallasse lenta + leggera deriva nel tempo.
    //     Riempimento tenue (un soffio piu' scuro della carta) cosi' la massa lontana si legge morbida.
    uint16_t far_fill = fx3d::mix(BR_PAPER, c_far, 60);
    fxscene::ridge(hy, /*minH*/8, /*maxH*/26, /*spacing*/22, c_far, far_fill,
                   /*parallax*/0.10f, g.camx, /*seed*/0x51u + g.level,
                   /*drift*/sinf(t * 0.15f) * 6.0f);

    // (2b) ridge MEDIO-LONTANO: una seconda fascia di citta' per profondita', passo e parallasse intermedi.
    fxscene::ridge(hy, /*minH*/12, /*maxH*/34, /*spacing*/28, fx3d::mix(c_far, c_near, 90), 0,
                   /*parallax*/0.16f, g.camx, /*seed*/0x77u + g.level * 11u,
                   /*drift*/sinf(t * 0.11f + 1.0f) * 4.0f);

    // (3) ridge MEDIO: piu' alto e sparso, parallasse media, niente riempimento (solo linea).
    fxscene::ridge(hy, /*minH*/16, /*maxH*/44, /*spacing*/34, c_near, 0,
                   /*parallax*/0.24f, g.camx, /*seed*/0xA3u + g.level * 7u, /*drift*/0.0f);

    // (4) prop di MEZZO: vetrine/archi a contorno, parallasse media, fermi (statici).
    fxscene::props(hy, /*spacing*/64, c_near, /*parallax*/0.40f, g.camx,
                   /*seed*/0x1Du + g.level * 3u, /*phase*/0.0f, /*kind*/2 /*vetrine*/);

    // (5) prop VICINI: lampioni e fogliame a contorno, parallasse veloce, lieve ondeggio.
    fxscene::props(hy, /*spacing*/92, c_line, /*parallax*/0.70f, g.camx,
                   /*seed*/0xC7u + g.level * 5u, /*phase*/t * 1.6f, /*kind*/-1 /*misto*/);

    // (5b) prop di PRIMO PIANO: pochi elementi grandi vicinissimi, parallasse quasi 1:1, ondeggio piu' ampio.
    fxscene::props(hy + 6, /*spacing*/140, c_line, /*parallax*/0.95f, g.camx,
                   /*seed*/0x2Fu + g.level * 13u, /*phase*/t * 2.1f, /*kind*/-1 /*misto*/);

    // (6) terreno PULITO: niente griglia. Banda grigia appena sotto la carta + una sola linea d'orizzonte,
    //     entrambe dalla palette del livello -> varieta' senza uscire dai grigi.
    fxscene::clean_floor(BR_BELT, L->floor_near, c_line);
}
