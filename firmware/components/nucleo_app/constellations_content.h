// constellations_content.h — static content for the "Costellazioni" space-trader game.
//
// PURE DATA: text pairs {IT, EN} (Spanish/French/German come from the game's language pack, keyed by the
// English text: tools/game-i18n/stelle.json), goods, economies, factions, narrative events and cinematic
// copy. Included by exactly ONE translation unit (app_constellations.cpp); every table is `const` all the
// way down (pointer arrays too: `const char *const`), so it lives in flash (.rodata) — ZERO RAM cost.
// All display strings are ASCII only: the TFT fonts have no accents, so Italian uses the apostrophe form.
// tools/gen-constellations-content.mjs parses GOODS / ECON_NAME / ECONMOD / FAC_NAME and the enums
// from this file for the web twin — keep their shape.
#pragma once

// ---- goods ------------------------------------------------------------------
enum { G_GRANO = 0, G_ACQUA, G_MINERALI, G_LEGHE, G_COMP, G_MEDIC, G_RELIQ, G_CONTRA, NGOODS };
enum { CAT_FOOD = 0, CAT_RAW, CAT_IND, CAT_TECH, CAT_RARE, CAT_ILLEGAL };

struct Good { const char *name[2]; int base; int cat; };
static const Good GOODS[NGOODS] = {
    { { "Grano",        "Grain"      },  12, CAT_FOOD    },
    { { "Acqua",        "Water"      },   8, CAT_FOOD    },
    { { "Minerali",     "Ore"        },  20, CAT_RAW     },
    { { "Leghe",        "Alloys"     },  46, CAT_IND     },
    { { "Componenti",   "Parts"      },  95, CAT_TECH    },
    { { "Medicinali",   "Medicine"   }, 135, CAT_TECH    },
    { { "Reliquie",     "Relics"     }, 270, CAT_RARE    },
    { { "Contrabbando", "Contraband" }, 210, CAT_ILLEGAL },
};

// ---- economies --------------------------------------------------------------
enum { EC_AGRI = 0, EC_MINE, EC_INDU, EC_TECH, EC_REFU, NECON };
static const char *const ECON_NAME[NECON][2] = {
    { "Agricolo", "Agri" }, { "Minerario", "Mining" }, { "Industriale", "Industry" },
    { "Tecnologico", "HiTech" }, { "Rifugio", "Refuge" },
};
// price modifier, percent of base, per (econ, good). <100 = produced/cheap, >100 = demanded/dear.
static const int ECONMOD[NECON][NGOODS] = {
    /* AGRI */ {  58,  52, 112, 116, 132, 122, 105, 134 },
    /* MINE */ { 122, 116,  54,  78, 120, 126, 110, 122 },
    /* INDU */ { 116, 112,  92,  58,  80, 112, 106, 116 },
    /* TECH */ { 126, 120, 116, 102,  56,  68, 122, 108 },
    /* REFU */ { 102, 100, 106, 106, 110,  94, 132,  88 },
};

// ---- factions ---------------------------------------------------------------
enum { F_GILDA = 0, F_CUSTODI, F_RELITTI, F_ECO, NFAC };
static const char *const FAC_NAME[NFAC][2] = {
    { "Gilda", "Guild" }, { "Custodi", "Keepers" }, { "Relitti", "Wrecks" }, { "Eco", "Echo" },
};

// ---- systems ----------------------------------------------------------------
// PROCEDURAL: the universe is an infinite chain of sectors. Each sector holds NSYS systems
// generated deterministically from (seed, sector) by the shared generator (pg_* in the .cpp,
// byte-identical to apps/games/www/games/constellations-gen.js). The firmware keeps the current
// sector in a regenerated cache `cur_sys[NSYS]` and `#define SYSTEMS cur_sys`, so the rest of the
// code reads systems exactly as before. Names are coined ASCII (language-neutral).
#define NSYS 10                 // systems per sector
struct Sys { char name[16]; int x, y, econ, faction, beacon; };

// ---- story flags (bit indices in g.flags) -----------------------------------
enum {
    FL_INTRO = 0,      // intro cinematic seen
    FL_MET_ECHO,       // spoken with the Echo
    FL_GUILD_PACT,     // signed the Guild contract
    FL_KEEPER_TRUST,   // earned a Keeper's blessing
    FL_SMUGGLER,       // ran contraband at least once
    FL_ACE_DEAD,       // shot down Scarlet Gutter, the Wrecks' pirate ace (lore.md)
};

// ---- sound-effect ids (procedural chiptune, file-overridable) ---------------
enum {
    SFX_NONE = 0, SFX_MOVE, SFX_OK, SFX_BACK, SFX_BUY, SFX_DENY,
    SFX_JUMP, SFX_EVENT, SFX_BEACON, SFX_TITLE, SFX_WIN, SFX_LOSE,
    SFX_LASER, SFX_HIT, SFX_BOOM, SFX_LAUNCH, SFX_LOCK,
    SFX_HULL, SFX_SHIELD_DOWN, SFX_ALARM, SFX_PASS, NSFX
};

// ---- cinematics -------------------------------------------------------------
enum { CINE_INTRO = 0, CINE_JUMP, CINE_BEACON, CINE_LOSE, CINE_SECTOR };

// The cutscene names the pilot (lore.md: Ren Calloway, shown in cutscenes, never in choices). <= 38 chars a line.
static const char *const INTRO_LINES[][2] = {
    { "I Costellatori accesero i Fari.",        "The Costellatori lit the Beacons." },
    { "Poi l'Oscuramento: si spensero.",        "Then the Dimming: they went dark." },
    { "Si salta alla cieca, isola per isola.",  "Ships jump blind, isle to isle." },
    { "Ren Calloway, corriere. Indebitato.",    "Ren Calloway, courier. In debt." },
    { "Riaccendi i Fari. L'Eco ti aspetta.",    "Relight the Beacons. The Echo waits." },
};
#define NINTRO ((int)(sizeof(INTRO_LINES)/sizeof(INTRO_LINES[0])))

static const char *const LOSE_LINES[][2] = {
    { "La Lucciola si spegne nel vuoto.", "The Firefly goes dark in the void." },
    { "Un altro relitto alla deriva nel buio.", "One more wreck adrift in the dark." },
};
#define NLOSE ((int)(sizeof(LOSE_LINES)/sizeof(LOSE_LINES[0])))

// ---- narrative events -------------------------------------------------------
// Effect applied when a choice is taken. Sentinels: -1 = none for rep_fac/flag/good/next.
enum { ACT_NONE = 0, ACT_RELIGHT, ACT_REFUEL, ACT_GAMEOVER };
// Small fixed ranges, so the fields are packed (int16 / int8): the whole table is ~0.75 KB of flash instead of 1.8.
struct Effect {
    int16_t dcred; int8_t dfuel, dhull;   // resource deltas
    int8_t rep_fac, drep;      // reputation: faction index (-1 none) and delta
    int8_t flag;               // story flag bit to SET (-1 none)
    int8_t good, qty;          // cargo grant/seizure (good idx -1 none; qty may be negative, -99 = all)
    int8_t act;                // ACT_* special action
    int8_t sfx;                // sound to play
    int8_t next;               // chain to event id (-1 none)
};
struct Choice { const char *label[2]; Effect eff; };
struct Event {
    const char *title[2];
    const char *body[2];
    Choice ch[3];
    int8_t nch;
    int8_t at_sys;       // -1 = any; else only at this system
    int8_t req_flag;     // -1 none; else flag bit must be SET
    int8_t forbid_flag;  // -1 none; else flag bit must be CLEAR
    int8_t need_faction; // -1 none; else only at a system owned by this faction
    int8_t story;        // 1 = story (always wins selection), 0 = random flavor
    int8_t weight;       // random selection weight
};

enum {
    EV_PIRATI = 0, EV_DERELITTO, EV_SOS, EV_DOGANA, EV_CUSTODE,
    EV_FARO, EV_ECO, EV_MERCATONERO, EV_TEMPESTA
};

static const Event EVENTS[] = {
    // EV_PIRATI — Wrecks ambush (random, dangerous)
    { { "Predoni Relitti", "Wreck Raiders" },
      { "I predoni di Scarlet Gutter ti tagliano la rotta. Vogliono pedaggio.",
        "Scarlet Gutter's raiders cut your course. They want a toll." },
      { { { "Paga (-120 cr)", "Pay (-120 cr)" },  { -120,0,0,  F_RELITTI,5,  -1, -1,0, ACT_NONE, SFX_OK,   -1 } },
        { { "Sfreccia via",   "Run for it"     },  {    0,-1,-8, F_RELITTI,-3, -1, -1,0, ACT_NONE, SFX_DENY, -1 } },
        { { "Combatti",       "Fight"          },  {  160,0,-22, F_RELITTI,-8, -1, -1,0, ACT_NONE, SFX_DENY, -1 } } },
      3, -1, -1, -1, -1, 0, 10 },

    // EV_DERELITTO — derelict ship (random, reward/risk)
    { { "Relitto alla deriva", "Drifting Hulk" },
      { "Uno scafo morto galleggia silenzioso. I sensori captano qualcosa nella stiva.",
        "A dead hull drifts silent. Sensors ping something in the hold." },
      { { { "Abborda e fruga", "Board and scavenge" }, { 60,0,-6, -1,0, -1, G_LEGHE,3, ACT_NONE, SFX_BUY, -1 } },
        { { "Prendi e vai",    "Grab and go"        }, {  0,2,0,  -1,0, -1, -1,0,     ACT_NONE, SFX_OK,  -1 } },
        { { "Lascia stare",    "Leave it"           }, {  0,0,0,  -1,0, -1, -1,0,     ACT_NONE, SFX_BACK,-1 } } },
      3, -1, -1, -1, -1, 0, 9 },

    // EV_SOS — distress call (random, moral)
    { { "Segnale di soccorso", "Distress Call" },
      { "Una voce gracchia su una frequenza morta: profughi senza celle ne' acqua.",
        "A voice crackles on a dead band: refugees with no cells, no water." },
      { { { "Dai aiuto (-1 acqua)", "Give aid (-1 water)" }, { 30,0,0, F_CUSTODI,8, -1, G_ACQUA,-1, ACT_NONE, SFX_OK,   -1 } },
        { { "Ignora",               "Ignore"              }, {  0,0,0, F_CUSTODI,-5,-1, -1,0,       ACT_NONE, SFX_BACK, -1 } } },
      2, -1, -1, -1, -1, 0, 8 },

    // EV_DOGANA — Guild customs (only at Guild systems carrying contraband)
    { { "Dogana della Gilda", "Guild Customs" },
      { "L'incrociatore dell'ammiraglio Raske ti aggancia. Scanner sul carico illecito.",
        "Admiral Raske's cruiser locks on. Scanners sweep your illicit hold." },
      { { { "Corrompi (-200 cr)", "Bribe (-200 cr)" }, { -200,0,0, F_GILDA,2,  FL_SMUGGLER, -1,0,        ACT_NONE, SFX_OK,   -1 } },
        { { "Getta il carico",    "Dump the cargo"  }, {    0,0,0, F_GILDA,4,  -1,           G_CONTRA,-99,ACT_NONE, SFX_DENY, -1 } },
        { { "Sfreccia via",       "Run for it"      }, {    0,-2,-12,F_GILDA,-12,FL_SMUGGLER, -1,0,        ACT_NONE, SFX_DENY, -1 } } },
      3, -1, -1, -1, F_GILDA, 0, 0 },   // weight 0: triggered conditionally by engine, not the random pool

    // EV_CUSTODE — Keeper riddle (at Keeper systems)
    { { "L'enigma del Custode", "The Keeper's Riddle" },
      { "Madre Ilse ti porge un disco caldo: \"Cosa lega le stelle, e tace?\"",
        "Mother Ilse offers a warm disc: \"What binds the stars, yet is silent?\"" },
      { { { "\"La luce\"",   "\"The light\""   }, { 0,0,0, F_CUSTODI,10, FL_KEEPER_TRUST, G_RELIQ,1, ACT_NONE, SFX_OK,   -1 } },
        { { "\"Il Silenzio\"","\"The Silence\"" }, { 0,0,0, F_CUSTODI,4,  -1,              -1,0,      ACT_NONE, SFX_BACK, -1 } },
        { { "Resta in silenzio","Stay silent"  }, { 0,0,0, F_CUSTODI,6,  -1,              -1,0,      ACT_NONE, SFX_OK,   -1 } } },
      3, -1, -1, -1, F_CUSTODI, 0, 7 },

    // EV_FARO — relight a Beacon (engine forces this at a dark Beacon system)
    { { "Il Faro spento", "The Dark Beacon" },
      { "Il Faro incombe, freddo. Una Reliquia e celle bastano a ridestarlo. Lo riaccendi?",
        "The Beacon looms, cold. A Relic and cells could wake it. Do you relight it?" },
      { { { "Riaccendi (-1 reliquia,-300 cr)", "Relight (-1 relic,-300 cr)" },
            { -300,0,0, F_CUSTODI,12, -1, G_RELIQ,-1, ACT_RELIGHT, SFX_BEACON, -1 } },
        { { "Non ora", "Not now" }, { 0,0,0, -1,0, -1, -1,0, ACT_NONE, SFX_BACK, -1 } } },
      2, -1, -1, -1, -1, 1, 0 },

    // EV_ECO — meet the Echo (story, first arrival at the Abyss)
    { { "L'Eco", "The Echo" },
      { "Nel vuoto si desta la Voce: \"Pilota. I Fari sono i miei occhi. Rendimeli.\"",
        "The Voice stirs in the void: \"Pilot. The Beacons are my eyes. Give them back.\"" },
      { { { "\"Lo faro'\"",   "\"I will\""      }, { 200,2,0, F_ECO,15, FL_MET_ECHO, -1,0, ACT_NONE, SFX_OK,   -1 } },
        { { "\"A che prezzo?\"","\"At what cost?\"" }, { 0,0,0, F_ECO,6,  FL_MET_ECHO, -1,0, ACT_NONE, SFX_BACK, -1 } } },
      2, -1, -1, FL_MET_ECHO, -1, 1, 0 },   // at_sys -1: gated by faction F_ECO in arrive() (procedural)

    // EV_MERCATONERO — black market (at Wreck systems)
    { { "Mercato nero", "Black Market" },
      { "Sotto i moli One-Eye Bram sogghigna: contrabbando a buon prezzo, se hai fegato.",
        "Below the docks One-Eye Bram grins: cheap contraband, if you have the nerve." },
      { { { "Compra (-180 cr)", "Buy (-180 cr)" }, { -180,0,0, F_RELITTI,4, FL_SMUGGLER, G_CONTRA,1, ACT_NONE, SFX_BUY,  -1 } },
        { { "Solo un'occhiata", "Just looking"  }, {    0,0,0, -1,0,        -1,          -1,0,       ACT_NONE, SFX_BACK, -1 } } },
      2, -1, -1, -1, F_RELITTI, 0, 6 },

    // EV_TEMPESTA — ion storm (random hazard)
    { { "Tempesta di ioni", "Ion Storm" },
      { "Un fronte di plasma investe lo scafo. I sistemi sfrigolano.",
        "A plasma front slams the hull. Systems sizzle." },
      { { { "Tieni la rotta", "Hold course" }, { 0,-1,-10, -1,0, -1, -1,0, ACT_NONE, SFX_DENY, -1 } },
        { { "Devia (-2 celle)","Divert (-2 cells)" }, { 0,-2,-2, -1,0, -1, -1,0, ACT_NONE, SFX_OK, -1 } } },
      2, -1, -1, -1, -1, 0, 7 },
};
#define NEVENTS ((int)(sizeof(EVENTS)/sizeof(EVENTS[0])))

// ---- action missions (Wing-Commander-style dogfights) -----------------------
// A mission is a self-contained "sortie": dock -> Mission Bay -> briefing -> launch a real-time dogfight
// in the system's space -> debrief & reward. Missions are PROCEDURAL (pg_mission / cur_mission in the
// .cpp, byte-identical to the web generator); this is the shape they are templated into.
enum { MT_PATROL = 0, MT_BOUNTY, MT_ESCORT, MT_DEFEND };  // mission archetypes
enum { FOE_FIGHTER = 0, FOE_SCOUT, FOE_HEAVY, FOE_ACE };  // enemy ship classes (scout=fast/weak, heavy=tanky)

struct Mission {
    const char *name, *brief, *win;  // in the OS language (composed by cur_mission)
    int type;                  // MT_*
    int offer_fac;             // faction offering it (reputation reward)
    int foe_fac;               // enemy colour/allegiance
    int waves, per_wave;       // dogfight shape
    int foe_hp, foe_dmg;       // per-fighter toughness / bite
    int foe_speed_pml;         // fighter speed in px/s * 10 (e.g. 850 -> ~76 px/s)
    int ace;                   // 1 = the final wave also spawns an Ace
    int reward_cr, kill_cr;    // base payout + bonus per kill
    int rep_gain;              // reputation to offer_fac on success
    int enemy_rep_loss;        // reputation lost with foe_fac on success
};
