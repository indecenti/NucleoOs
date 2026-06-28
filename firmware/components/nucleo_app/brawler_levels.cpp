// brawler_levels.cpp — SCORRIBANDA: i 6 livelli (ondate) + il cancello "Double Dragon".
//
// Tutto data-driven: ogni livello e' un LevelDef con palette "carta bianca" (sfondo BR_PAPER, linee solo
// nei grigi BR_GREY_*, appena piu' decise salendo di livello) e una tabella di ondate (WaveDef[]). Aggiungere
// voci a LEVELS estende automaticamente il gioco (brawler_level_count() = dimensione dell'array). Il blu dei
// nemici e il rosso del sangue vivono in altri moduli: qui solo bianco e grigi.
//
// Progressione tipo Double Dragon: gli eroi non possono superare g.gatex finche' l'ondata non e' pulita;
// a ondata pulita il cancello scorre avanti e parte l'ondata successiva. Tetto vivi BR_MAXENEMY: gli
// extra restano in coda e colano in scena man mano che si liberano gli slot.
//
// Implementa la sezione "levels / waves" di brawler.h:
//   brawler_level_count, brawler_level, levels_begin, levels_step, levels_is_clear
// Chiama gli altri moduli SOLO via brawler.h (brawler_spawn_enemy / brawler_live_enemies / br_*).

#include "brawler.h"
#include <math.h>

// ----------------------------------------------------------------- palette helper (look bianco)
// Look minimal "carta bianca": sfondo BR_PAPER, linee solo nei grigi BR_GREY_*. Niente altri colori qui
// (il blu nemici e il rosso sangue vivono in altri moduli). I livelli variano SOLO mescolando i grigi.
// fx3d::mix(a,b,t) con t 0..255: t=0 -> a, t=255 -> b. Lo usiamo per spostarci dentro la palette frozen.

// ----------------------------------------------------------------- tabelle ondate
// Tipi nemici 0..3 (vedi brawler_enemies.cpp): 0=teppista, 1=picchiatore, 2=lanciatore, 3=CAPOBANDA(boss).
// I livelli avanzati mescolano tipi piu' duri; l'ultima ondata di L6 chiude col boss (tipo 3).

// L1 — Il Vicolo: introduzione, solo type0/1.
static const WaveDef WAVES_L1[] = {
    { {0,0},     2 },
    { {0,1,0},   3 },
    { {1,0,1},   3 },
};
// L2 — Il Mercato: piu' nemici, compare il lanciatore (type2).
static const WaveDef WAVES_L2[] = {
    { {0,1,0},     3 },
    { {1,2,1},     3 },
    { {0,1,2,0},   4 },
};
// L3 — La Metro: ondate dense, mix 0/1/2.
static const WaveDef WAVES_L3[] = {
    { {1,0,1,0},   4 },
    { {2,1,2},     3 },
    { {0,1,2,1,0}, 5 },
    { {1,2,1,2},   4 },
};
// L4 — I Magazzini: piu' picchiatori e lanciatori.
static const WaveDef WAVES_L4[] = {
    { {1,1,0,1},   4 },
    { {2,1,2,1},   4 },
    { {1,2,1,2,1}, 5 },
    { {2,2,1,1},   4 },
};
// L5 — I Tetti: quasi-boss difficolta', ondate piene.
static const WaveDef WAVES_L5[] = {
    { {1,2,1,2},     4 },
    { {2,1,2,1,2},   5 },
    { {1,1,2,2,1,2}, 6 },
    { {2,2,1,2,1},   5 },
};
// L6 — Il Porto: culmine; l'ultima ondata e' il CAPOBANDA (type3) con scorta.
static const WaveDef WAVES_L6[] = {
    { {1,2,1,2,1},   5 },
    { {2,1,2,2,1},   5 },
    { {1,2,2,1,2,1}, 6 },
    { {2,2,1,2,1,2}, 6 },
    { {3,1,2},       3 },   // boss + due gregari
};

// ----------------------------------------------------------------- definizioni livello
// Palette: grigi che si fanno via via piu' scuri e freddi. (sky_top, sky_bot, build_far, build_near,
// floor_far, floor_near, floor_line). build_near piu' scuro di build_far -> profondita'.
// NB: NON const — i campi colore si calcolano a runtime (br_rgb non e' constexpr); un const finirebbe
// in flash/.rodata e la scrittura crasherebbe sull'ESP32. I puntatori WaveDef restano a tabelle const.
static LevelDef LEVELS[] = {
    // L1 — Il Vicolo (notte tiepida, grigi medi)
    { "Il Vicolo", "The Alley", 1000.0f,
      0,  0,  0,  0,  0,  0,  0,   // placeholder palette riempita sotto a init (vedi nota)
      (uint8_t)(sizeof(WAVES_L1)/sizeof(WAVES_L1[0])), WAVES_L1 },
    // L2 — Il Mercato
    { "Il Mercato", "The Market", 1100.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L2)/sizeof(WAVES_L2[0])), WAVES_L2 },
    // L3 — La Metro
    { "La Metro", "The Subway", 1200.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L3)/sizeof(WAVES_L3[0])), WAVES_L3 },
    // L4 — I Magazzini
    { "I Magazzini", "The Warehouse", 1300.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L4)/sizeof(WAVES_L4[0])), WAVES_L4 },
    // L5 — I Tetti
    { "I Tetti", "The Rooftops", 1400.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L5)/sizeof(WAVES_L5[0])), WAVES_L5 },
    // L6 — Il Porto (culmine, boss)
    { "Il Porto", "The Docks", 1500.0f,
      0,0,0,0,0,0,0,
      (uint8_t)(sizeof(WAVES_L6)/sizeof(WAVES_L6[0])), WAVES_L6 },
};

// Le palette sono calcolate a runtime (br_rgb non e' constexpr): un piccolo init lazy le scrive negli slot
// 565 della tabella la prima volta che si chiede un livello. La tabella resta data-driven.
// Look bianco: il cielo e' carta (BR_PAPER), le linee degli edifici nei grigi (far->mid), il pavimento
// resta chiarissimo (carta -> grigio appena percepibile) e la linea di terra e' un grigio tenue. Saliti di
// livello mescoliamo i grigi di un filo (t cresce) per dare profondita' senza MAI uscire dalla palette.
static bool s_pal_init = false;
static void levels_palettes_init(void) {
    if (s_pal_init) return;
    s_pal_init = true;
    const int N = (int)(sizeof(LEVELS) / sizeof(LEVELS[0]));
    for (int i = 0; i < N; i++) {
        // 0..40: avanzando i grigi si fanno appena piu' decisi (passo piccolo, restiamo "carta").
        int t = (N > 1) ? (i * 40 / (N - 1)) : 0;             // 0 (L1) .. 40 (L6) — variazione sottile
        LevelDef *L = &LEVELS[i];
        // Cielo: praticamente carta. In alto BR_PAPER pieno, in basso un soffio verso il grigio far.
        L->sky_top    = BR_PAPER;
        L->sky_bot    = fx3d::mix(BR_PAPER, BR_GREY_FAR, 28 + t);   // velo di grigio piu' su
        // Edifici (line-art): lontani grigio far, vicini grigio mid; un filo piu' decisi salendo.
        L->build_far  = fx3d::mix(BR_GREY_FAR, BR_GREY_MID, t);
        L->build_near = fx3d::mix(BR_GREY_MID, BR_GREY_NEAR, t);
        // Pavimento: resta carta. In fondo carta pulita, vicino un grigio appena accennato.
        L->floor_far  = BR_PAPER;
        L->floor_near = fx3d::mix(BR_PAPER, BR_GREY_FAR, 40 + t);
        // Linea di terra: grigio near ma tenue (mescolato verso il mid) -> presenza senza griglia.
        L->floor_line = fx3d::mix(BR_GREY_MID, BR_GREY_NEAR, 90 + t);
    }
}

// ----------------------------------------------------------------- API tabella
int brawler_level_count(void) { return (int)(sizeof(LEVELS) / sizeof(LEVELS[0])); }

const LevelDef *brawler_level(int i) {
    levels_palettes_init();
    int n = brawler_level_count();
    if (n <= 0) return 0;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return &LEVELS[i];
}

// ----------------------------------------------------------------- stato runtime (coda/flag)
// Piccola struct statica: nemici "in coda" non ancora entrati per il tetto vivi, piu' i flag di ondata.
struct LevelRun {
    uint8_t qtype[BR_MAXENEMY * 2];  // tipi in coda da far entrare appena si libera uno slot
    int     qn;                      // quanti in coda
    int     qhead;                   // indice prossimo da spawnare
    float   drip;                    // timer di ingresso scaglionato (s)
    bool    wave_done;               // tutte le ondate processate -> cancello tutto aperto
    bool    began;                   // levels_begin chiamato
};
static LevelRun s_run = {0};

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Eroe di riferimento per posizionare gli spawn (player 0; fallback player 1).
static Fighter *ref_hero(void) {
    Fighter *h = br_hero(0);
    if (!h || !h->on) h = br_hero(1);
    return h;
}

// ----------------------------------------------------------------- spawn ondata
// Mette in coda l'ondata corrente; quel che entra subito (sotto il tetto) lo fa qui, il resto cola in step.
static void spawn_wave(void) {
    const LevelDef *L = brawler_level(g.level);
    if (!L) return;
    if (g.wave < 0 || g.wave >= L->nwaves) return;
    const WaveDef *w = &L->waves[g.wave];

    // Carica i tipi dell'ondata nella coda.
    s_run.qn = 0;
    s_run.qhead = 0;
    s_run.drip = 0.0f;
    int cnt = w->count;
    if (cnt > BR_MAXENEMY) cnt = BR_MAXENEMY;             // WaveDef.types ha BR_MAXENEMY slot
    int cap = (int)(sizeof(s_run.qtype) / sizeof(s_run.qtype[0]));
    for (int i = 0; i < cnt && s_run.qn < cap; i++) {
        s_run.qtype[s_run.qn++] = w->types[i];
    }
    // Il primo drip in levels_step fara' entrare i nemici sotto il tetto vivi.
}

// Fa entrare il prossimo nemico in coda davanti/dietro all'eroe, dentro i confini del livello.
static bool drip_one(void) {
    if (s_run.qhead >= s_run.qn) return false;
    if (brawler_live_enemies() >= BR_MAXENEMY) return false;

    Fighter *h = ref_hero();
    float hx = h ? h->x : g.camx + BR_SW * 0.5f;
    float side = (br_frnd() < 0.5f) ? -1.0f : 1.0f;
    float x = hx + side * (BR_SW * 0.55f + br_frnd() * 60.0f);
    x = clampf(x, 40.0f, g.level_len - 40.0f);
    float z = 0.3f + br_frnd() * 0.6f;                    // profondita' belt 0.3..0.9

    int type = s_run.qtype[s_run.qhead];
    Fighter *e = brawler_spawn_enemy(type, x, z);
    if (!e) return false;                                 // nessuno slot: riprova al prossimo step
    s_run.qhead++;
    return true;
}

// ----------------------------------------------------------------- begin
void levels_begin(int level) {
    levels_palettes_init();
    int n = brawler_level_count();
    if (level < 0) level = 0;
    if (level >= n) level = n - 1;

    g.level = level;
    const LevelDef *L = brawler_level(level);
    g.level_len = L ? L->length : 1000.0f;
    g.camx = 0.0f;
    g.wave = 0;
    g.gatex = BR_SW * 0.9f;                               // primo cancello davanti all'eroe

    // Pulisci gli slot nemici (lascia gli eroi).
    for (int i = 0; i < BR_MAXF; i++) {
        if (!g.f[i].is_hero) g.f[i].on = false;
    }

    s_run.qn = 0;
    s_run.qhead = 0;
    s_run.drip = 0.0f;
    s_run.wave_done = false;
    s_run.began = true;

    spawn_wave();                                         // accoda l'ondata 0 (entrera' in step)
}

// ----------------------------------------------------------------- step
// Avanza il cancello quando l'ondata e' pulita; fa colare i nemici in coda sotto il tetto vivi.
void levels_step(float dt) {
    if (!s_run.began) return;
    const LevelDef *L = brawler_level(g.level);
    if (!L) return;

    // 1) Drip: porta in scena i nemici accodati, scaglionati nel tempo.
    if (s_run.qhead < s_run.qn) {
        s_run.drip -= dt;
        while (s_run.drip <= 0.0f && s_run.qhead < s_run.qn &&
               brawler_live_enemies() < BR_MAXENEMY) {
            if (!drip_one()) break;                       // niente slot al volo: aspetta il prossimo step
            s_run.drip += 0.45f + br_frnd() * 0.5f;       // ingressi scaglionati ~0.45..0.95 s
        }
    }

    // 2) Avanzamento ondata: solo quando NON ci sono vivi e la coda corrente e' esaurita.
    bool queue_empty = (s_run.qhead >= s_run.qn);
    if (brawler_live_enemies() == 0 && queue_empty) {
        if (g.wave < L->nwaves - 1) {
            // Ancora ondate: apri il cancello di una schermata e lancia la prossima.
            g.wave++;
            g.gatex = fminf(g.gatex + (float)BR_SW, g.level_len);
            spawn_wave();
        } else {
            // Ultima ondata pulita: cancello tutto aperto, l'eroe puo' raggiungere l'uscita.
            g.gatex = g.level_len;
            s_run.wave_done = true;
        }
    }
}

// ----------------------------------------------------------------- clear
// Livello pulito quando l'ultima ondata e' sconfitta (vivi==0, coda vuota, ultima ondata) E un eroe ha
// raggiunto l'uscita (x > level_len - 40).
bool levels_is_clear(void) {
    if (!s_run.began) return false;
    const LevelDef *L = brawler_level(g.level);
    if (!L) return false;

    bool last_wave   = (g.wave >= L->nwaves - 1);
    bool queue_empty = (s_run.qhead >= s_run.qn);
    bool defeated    = (brawler_live_enemies() == 0) && last_wave && queue_empty;
    if (!defeated) return false;

    // Un eroe deve aver raggiunto il bordo destro del livello.
    float exitx = g.level_len - 40.0f;
    Fighter *h0 = br_hero(0);
    Fighter *h1 = br_hero(1);
    bool reached = (h0 && h0->on && h0->x > exitx) ||
                   (h1 && h1->on && h1->x > exitx);
    return reached;
}
