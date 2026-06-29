// ANIMA L0 orchestrator: the cheap, always-on tier of the cascade (docs/anima.md §2).
//
// Pipeline: normalize (lowercase, strip Italian accents, drop punctuation) -> tokenize
// -> score each intent by keyword overlap (prefix-tolerant, so inflections match) ->
// confidence gate. Allocation-free and bounded; runs in well under a millisecond.
//
// On an L0 miss this returns ANIMA_TIER_NONE — the higher tiers (L1 retrieval, ...) hang
// off the same entry point and will be tried before falling back to an honest "non lo so".
#include "nucleo_anima.h"
#include "anima_internal.h"
#include "anima_l1.h"
#include "nucleo_anima_online.h"
#include "nucleo_anima_learn.h"
#include "nucleo_anima_profile.h"
#include "nucleo_anima_translate.h"
#include "nucleo_board.h"
#ifndef ANIMA_HOST
#include "esp_attr.h"      // RTC_NOINIT_ATTR — DIAG breadcrumb that survives a warm reboot (device)
#else
#define RTC_NOINIT_ATTR    // host harness: plain global, no RTC section
#endif
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include <stdatomic.h>

// ANIMA spine gate: serialize nucleo_anima_query() across its TWO callers — the web handler (httpd
// task, nucleo_httpd.c) and the native app worker (app_anima.cpp). Exactly one cascade owns the shared
// L1/session globals (s_centroids, s_idx, the static qv/row scratch, s_session) at a time, which fixes
// the concurrent use-after-free panic (one caller frees the L1 index via nucleo_anima_l1_unload while
// the other is mid-read). Lock order is always gate -> (online TLS arbiter token), never the reverse,
// so there is no deadlock and no double-hold with the online tier's own arbiter use.
static atomic_flag s_anima_gate = ATOMIC_FLAG_INIT;
bool nucleo_anima_try_lock(void) { return !atomic_flag_test_and_set_explicit(&s_anima_gate, memory_order_acquire); }
void nucleo_anima_unlock(void)   { atomic_flag_clear_explicit(&s_anima_gate, memory_order_release); }

// Heap reclaim for OUTSIDE callers (proxy/llm fetchers, exclusive mode, UI transitions): free the L1
// index only if no cascade is mid-query. An unguarded unload from another task frees s_cdir/s_ec_row
// and closes the index FILE* under a running l1_query -> use-after-free. Fail-closed: if ANIMA is
// busy we skip the reclaim (the caller's own heap gate then refuses the work gracefully) rather than
// corrupt a live query. In-cascade callers hold the gate already and keep calling l1_unload directly.
bool nucleo_anima_l1_unload_if_idle(void)
{
    if (!nucleo_anima_try_lock()) return false;
    nucleo_anima_l1_unload();
    nucleo_anima_unlock();
    return true;
}

static const char *TAG = "anima";

#define SESSION_PATH   NUCLEO_SD_MOUNT "/data/anima/session.txt"
#define TELEMETRY_PATH NUCLEO_SD_MOUNT "/data/anima/telemetry.ndjson"
#define TELEMETRY_CAP  65536          // rotate the routing log past 64 KB (bounded SD use)

#define A_MAX_TOKENS   24
#define A_TOK_LEN      24
#define A_MAX_KW       28

// ---- intent table -----------------------------------------------------------
// Keywords are pre-normalized (lowercase ASCII, no accents). Matching is prefix-
// tolerant in both directions (min 3 chars), so "aprire"~"apri", "foto"~"fotografie".
typedef struct {
    const char    *id;
    anima_action_t action;
    const char    *arg;       // app id / system key, or NULL (generic "open the named app")
    const char    *reply_it;  // static reply IT, or NULL for LAUNCH/SYSTEM
    const char    *reply_en;  // static reply EN
    const char    *kw[A_MAX_KW];
} a_intent_t;

// <gen:app-alias> --- GENERATED from registry/app-aliases.json by tools/anima/gen_aliases.py.
// DO NOT EDIT BY HAND: edit the JSON and run `python tools/anima/gen_aliases.py`.
#define A_MAX_ALIAS 16
typedef struct { const char *id; const char *alias[A_MAX_ALIAS]; } a_alias_t;

// Words the user might type (IT+EN) -> the registry app id ANIMA opens. First match wins.
static const a_alias_t APP_ALIAS[] = {
    { "photo-viewer",       { "foto", "fotografie", "immagini", "galleria", "photos", "photo", "images", "pictures", "gallery", NULL } },
    { "paint",              { "paint", "disegno", "disegna", "disegnare", "pittura", "draw", "painting", NULL } },
    { "notepad",            { "note", "blocco", "appunti", "testo", "nota", "scrivi", "notes", "text", "write", NULL } },
    { "file-commander",     { "file", "files", "cartelle", "documenti", "esplora", "documents", "folders", NULL } },
    { "media-player",       { "musica", "brani", "canzoni", "audio", "lettore", "music", "songs", "song", "player", NULL } },
    { "radio",              { "radio", "radioline", "stazione", "fm", "station", NULL } },
    { "video-player",       { "video", "filmati", "filmato", "film", "videos", "movie", "movies", NULL } },
    { "spreadsheet",        { "excel", "spreadsheet", "foglio", "fogli", "tabella", "tabelle", "celle", "sheet", "csv", NULL } },   // Listed before calculator on purpose: "foglio di calcolo" must resolve to the sheet app.
    { "calculator",         { "calcolatrice", "calcoli", "calculator", "math", NULL } },   // No "calc"/"calcolo" alias on purpose: it prefix-collides with "foglio di calcolo" (spreadsheet).
    { "terminal",           { "terminale", "terminal", "shell", "console", "prompt", "bash", "cli", NULL } },
    { "clock",              { "orologio", "sveglia", "cronometro", "timer", "clock", "stopwatch", "alarm", NULL } },
    { "calendar",           { "calendario", "agenda", "appuntamenti", "eventi", "calendar", "events", NULL } },
    { "settings",           { "impostazioni", "settaggi", "configurazione", "opzioni", "settings", "options", "config", NULL } },
    { "browser",            { "browser", "naviga", "navigatore", "navigare", "navigazione", NULL } },
    { "tasks",              { "tasks", "task", "attivita", "compiti", "todo", NULL } },
    { "system-monitor",     { "monitor", "risorse", "stato", "prestazioni", "status", "resources", NULL } },
    { "ir-remote",          { "telecomando", "infrarossi", "remote", "infrared", NULL } },
    { "log-viewer",         { "log", "registro", "diagnostica", "logs", NULL } },
    { "swarm",              { "sciame", "swarm", NULL } },
    { "automation-studio",  { "automazioni", "automazione", "scenari", "automation", "macros", NULL } },
    { "recorder",           { "registratore", "registra", "voce", "microfono", "recorder", "record", "mic", "voice", "memo", "nota vocale", "promemoria vocale", "dettatura", "detta", "trascrivi", "registrazione", NULL } },
    { "dictation",          { "trascrizione", "sottotitoli", "stt", "transcription", "speech to text", NULL } },
    { "recycle-bin",        { "cestino", "eliminati", "spazzatura", "trash", "recycle", "bin", NULL } },
    { "updates",            { "aggiornamenti", "aggiorna", "update", "updates", NULL } },
    { "dosbox",             { "dos", "emulatore", "msdos", "dosbox", NULL } },
    { "games",              { "giochi", "gioco", "games", "game", "giocare", "partita", "multiplayer", "arcade", NULL } },
    { "code-runner",        { "runner", "playground", "script", "javascript", "coderunner", NULL } },
    { "miei-fatti",         { "fatti", "miei fatti", "i miei fatti", "i miei dati", "ricordi", "conoscenze", "facts", "my facts", "my data", "knowledge", NULL } },
    { "ethernet",           { "ethernet", "lan", "w5500", "rete cablata", "cablato", "cablata", "arp", "wired", "cable", NULL } },
    { "ble",                { "ble", "bluetooth", "bt", NULL } },
    { "payloads",           { "payload", "payloads", "ducky", "duckyscript", "badusb", "rubber ducky", "hid", NULL } },
    { "weather",            { "meteo", "tempo", "previsioni", "weather", "forecast", "che tempo fa", NULL } },
    { "mail",               { "mail", "email", "posta", "e-mail", "gmail", "invia mail", "manda mail", "scrivi mail", "send mail", "email", "smtp", NULL } },
};
// </gen:app-alias>

static const a_intent_t INTENTS[] = {
    // Generic "open <app>": arg NULL -> the named app is resolved from the query.
    { "open_app", ANIMA_ACT_LAUNCH, NULL, NULL, NULL,
      { "apri", "aprire", "avvia", "lancia", "esegui", "vai", "mostra", "mostrami",
        // NB: "vorrei"/"voglio" are DESIRE prefixes that DO carry some launches ("voglio disegnare
        // qualcosa" → Paint) but also precede knowledge ("vorrei sapere perché…"). They stay here, but
        // a_l0_suppress_desire_launch() vetoes an open_app win that rests only on a desire-prefix when the
        // query is a knowledge question (def/question cue + no STRONG open verb) — so "vorrei … test"
        // (test~testo→notepad) no longer hijacks a question into a launch.
        "vedere", "vedi", "guarda", "guardare", "vorrei", "voglio", "portami", "fammi",
        // play/put-on verbs: "metti la musica" / "riproduci la radio" / "play music" -> launch the app
        "metti", "riproduci", "suona", "ascolta", "accedi", "entra",
        "open", "show", "launch", "run", "play", "start" } },

    // Live system queries -> caller fills the localized value into the template.
    { "battery", ANIMA_ACT_SYSTEM, "battery", "Batteria: {value}.", "Battery: {value}.",
      { "batteria", "carica", "autonomia", "energia", "battery", NULL } },
    { "time",    ANIMA_ACT_SYSTEM, "time",    "{value}.", "{value}.",
      { "ora", "ore", "orario", "adesso", "time", "clock", NULL } },
    { "storage", ANIMA_ACT_SYSTEM, "storage", "Spazio SD: {value}.", "SD space: {value}.",
      // NB no bare "memoria": with the C/ESP knowledge packs it means RAM/allocation far more
      // often than SD space ("alloco memoria dinamica" must reach L1, not the storage reading).
      // RAM-free questions are handled by a_is_ram() instead.
      { "spazio", "disco", "scheda", "archiviazione", "space", "storage", "sd", "capacita", "gigabyte", NULL } },

    // Computed-from-state ("4th pillar"): the device answers from its own RTC, so any phrasing of
    // these works with zero cards and is always exact. The caller fills {value} from localtime().
    { "date",   ANIMA_ACT_SYSTEM, "date",   "{value}.", "{value}.",
      // NB no "today"/"calendario": "today" mis-fires ("bitcoin today") and "calendario" is the
      // Calendar app alias. "data"/"oggi"/"giorno"/"date" are specific enough.
      { "data", "oggi", "giorno", "date", NULL } },
    { "year",   ANIMA_ACT_SYSTEM, "year",   "Siamo nel {value}.", "It's {value}.",
      { "anno", "year", "annata", NULL } },
    { "season", ANIMA_ACT_SYSTEM, "season", "Siamo in {value}.", "It's {value}.",
      { "stagione", "season", NULL } },
    // Self-state the device reads from its own runtime (always exact, zero cards). Network/RAM
    // status are detectors (a_is_network/a_is_ram) — they must dodge the knowledge questions
    // ("cos'e il wifi", "alloco memoria") — but version/uptime have unambiguous keywords.
    { "version", ANIMA_ACT_SYSTEM, "version", "Eseguo {value}.", "Running {value}.",
      // NB no bare "firmware": "cos'e il firmware" is a definition (esp.arduino-fw card -> L1).
      { "versione", "version", NULL } },
    { "uptime",  ANIMA_ACT_SYSTEM, "uptime",  "Acceso da {value}.", "Up for {value}.",
      { "uptime", "acceso", "accesa", "avvio", NULL } },

    // Static FAQ answers (L0 covers the obvious questions with zero retrieval).
    { "whoami", ANIMA_ACT_ANSWER, NULL,
      "Sono ANIMA, l'assistente offline di NucleoOS. Funziono senza internet, sul dispositivo.",
      "I'm ANIMA, NucleoOS's offline assistant. I work with no internet, on the device.",
      // NB no bare "sei": it hijacks "sei un robot" / "sei viva" -> those reach L1 (self.* cards).
      // "chi sei" still matches via "chi"; "who are you" via "who".
      { "chi", "anima", "presentati", "who", "you", NULL } },
    // NB no static "help" intent: "cosa sai fare" / "aiuto" / "comandi" are caught by
    // a_is_capabilities() and answered DYNAMICALLY by the executor (live apps + pillars).
};

// ---- normalization & tokenization ------------------------------------------

// Append one normalized ASCII char to a bounded token; returns new length.
static int a_putc(char *tok, int len, char c)
{
    if (len < A_TOK_LEN - 1) tok[len++] = c;
    return len;
}

// Normalize `in` and split into lowercase ASCII tokens. Italian accented vowels
// (UTF-8 0xC3 0xA0..0xBA) fold to their base letter. Returns token count.
static int a_tokenize(const char *in, char tok[A_MAX_TOKENS][A_TOK_LEN])
{
    int n = 0, len = 0;
    char cur[A_TOK_LEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        unsigned char c = *p;
        char out = 0;

        if (c == 0xC3 && p[1]) {            // 2-byte UTF-8 Latin-1 supplement
            unsigned char d = *++p;
            switch (d) {
                case 0xA0: case 0xA1: case 0xA2: out = 'a'; break;  // à á â
                case 0xA8: case 0xA9: case 0xAA: out = 'e'; break;  // è é ê
                case 0xAC: case 0xAD: case 0xAE: out = 'i'; break;  // ì í î
                case 0xB2: case 0xB3: case 0xB4: out = 'o'; break;  // ò ó ô
                case 0xB9: case 0xBA: case 0xBB: out = 'u'; break;  // ù ú û
                default: out = 0; break;
            }
        } else if (isalnum(c)) {
            out = (char)tolower(c);
        }

        if (out) {
            len = a_putc(cur, len, out);
        } else {                            // separator or end -> flush token
            if (len > 0 && n < A_MAX_TOKENS) {
                cur[len] = 0;
                memcpy(tok[n++], cur, len + 1);
                len = 0;
            }
            if (c == 0) break;
        }
    }
    return n;
}

// Prefix-tolerant equality: equal, or one is a >=4-char prefix of the other. The >=4 (not 3)
// matters: 3-letter keywords are prefixes of countless words ("chi"->"chiedo", "ora"->"orario")
// and would hijack knowledge queries at L0 before L1 retrieval runs — so they must match exactly.
static bool a_match(const char *a, const char *b)
{
    if (strcmp(a, b) == 0) return true;
    size_t la = strlen(a), lb = strlen(b), m = la < lb ? la : lb;
    if (m < 4) return false;
    // Prefix tolerance is for inflections (giorno->giornata), which add a SHORT suffix.
    // Cap the length gap at 2 so a keyword can't bleed into a different word that merely
    // shares its start: "data"->"database"/"dataset" was hijacking knowledge Qs into intent=date.
    if ((la > lb ? la - lb : lb - la) > 2) return false;
    return strncmp(a, b, m) == 0;
}

// Count distinct intent keywords present in the token list.
static int a_score(const a_intent_t *it, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    int hits = 0;
    for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
        for (int t = 0; t < ntok; t++)
            if (a_match(it->kw[k], tok[t])) { hits++; break; }
    return hits;
}

// Resolve the app id named in the query (for the generic open_app intent).
static const char *a_resolve_app(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]); i++)
        for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j]; j++)
            for (int t = 0; t < ntok; t++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t]))
                    return APP_ALIAS[i].id;
    return NULL;
}

// A generic open_app win that rests ONLY on a DESIRE prefix ("vorrei"/"voglio") is really a knowledge
// request when the query also carries a definition/question cue AND has NO strong open verb: "vorrei
// SAPERE PERCHÉ scrivere test automatici" must answer, not open a fuzzily-matched app (test~testo→notepad).
// "voglio DISEGNARE qualcosa" still launches (strong verb, no cue). Returns true to VETO the launch.
static bool a_desire_only_launch(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const STRONG[] = { "apri","aprire","avvia","lancia","esegui","vai","mostra","mostrami",
        "open","launch","show","run","start","metti","riproduci","suona","ascolta","play","portami",
        "accedi","entra","disegna","disegnare","draw", NULL };
    static const char *const CUE[] = { "cos","cosa","spiega","spiegami","significa","significato","definizione",
        "differenza","perche","come","quanto","quanta","quanti","quale","quali","chi","quando","dove",
        "sapere","conoscere","capire","why","what","how","explain","mean","means", NULL };
    bool strong = false, cue = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; STRONG[i]; i++) if (!strcmp(STRONG[i], tok[t])) { strong = true; break; }
        for (int i = 0; CUE[i]; i++)    if (!strcmp(CUE[i], tok[t]))    { cue = true; break; }
        // a NEGATED desire ("non voglio creare un file") is never a launch command
        if (!strcmp(tok[t], "non") || !strcmp(tok[t], "not")) return true;
    }
    return cue && !strong;
}

// A QUESTION whose ONLY "open" signal is a WEAK content verb (play/suona/metti/run/start — which also
// mean to play a sport, to put, to execute) is not a launch command: "what team did Einstein's father
// PLAY for", "come si SUONA il piano". Require a STRONG explicit launch verb to fire under a question.
// Veto otherwise so the query reaches the knowledge/online tier. Returns true to VETO the launch.
static bool a_question_not_launch(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const strong[] = { "apri","aprire","avvia","avviare","lancia","lanciare","esegui",
        "open","launch","start","avviami","portami","accedi","entra","aprimi", NULL };   // explicit "open the app"
    static const char *const qw[] = { "chi","quale","quali","quando","dove","come","perche","quanti","quanto",
        "quanta","cosa","what","which","who","whom","whose","when","where","why","how","did","does","do", NULL };
    // A temporal/quantity interrogative FRAME ("(in) che anno/ora/giorno …", "qual è l'anno …"): the bare
    // word "che"/"qual" is too broad to be a qword on its own (it heads relative clauses — "le foto CHE ho
    // scattato" — so it isn't in qw above), but IMMEDIATELY before a time/quantity noun it heads a WH-question.
    // Without this, the false-premise "in che anno MORIRÀ javascript" (and its "mostra"-spellfix sibling) had
    // the weak open verb "mostra" launch code-runner instead of abstaining. Mirrors the qword+keyword
    // adjacency in a_ambient_ok; restricted to "che"/"qual" so a genuine "MOSTRA le foto" still launches.
    static const char *const tframe[] = { "che", "qual", NULL };
    static const char *const tnoun[]  = { "anno","anni","annata","ora","ore","orario","giorno","giorni",
        "data","mese","mesi","year","hour","hours","day","days","date","month","time", NULL };
    bool q = false, strong_open = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; qw[i];     i++) if (!strcmp(qw[i],     tok[t])) q = true;
        for (int i = 0; strong[i]; i++) if (!strcmp(strong[i], tok[t])) strong_open = true;
        if (t + 1 < ntok) {
            bool isframe = false, isnoun = false;
            for (int i = 0; tframe[i]; i++) if (!strcmp(tframe[i], tok[t]))   isframe = true;
            for (int i = 0; tnoun[i];  i++) if (!strcmp(tnoun[i],  tok[t+1])) isnoun  = true;
            if (isframe && isnoun) q = true;
        }
    }
    return q && !strong_open;
}

// A real launch is "VERB + OBJECT" — a launch verb AND a DISTINCT app name. Reject the degenerate case
// where every open/app signal collapses onto the SAME ambiguous token: "play"/"player" alone, where the
// verb "play" prefix-matches the media-player alias "player", is a single noun, not "open the player".
// LEGIT structure survives via a token that is a PURE verb ("open the player" — "open" is a verb that is no
// app) OR a PURE app ("play some music" — "music" is an app that is no verb). Returns true to VETO. The
// open-verb list mirrors the open_app intent's launch keywords (desire prefixes vorrei/voglio excluded:
// they are not the structural verb of a command). a_match (prefix-tolerant) is used so "play"~"player".
static bool a_launch_is_degenerate(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const verbs[] = { "apri","aprire","avvia","lancia","esegui","vai","mostra","mostrami",
        "vedere","vedi","guarda","guardare","portami","metti","riproduci","suona","ascolta","accedi","entra",
        "open","show","launch","run","play","start", NULL };
    for (int t = 0; t < ntok; t++) {
        bool isverb = false, isapp = false;
        for (int i = 0; verbs[i]; i++) if (a_match(verbs[i], tok[t])) { isverb = true; break; }
        for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]) && !isapp; i++)
            for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j]; j++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t])) { isapp = true; break; }
        if (isverb != isapp) return false;   // a PURE verb or a PURE app -> a real verb+object structure
    }
    return true;                             // only verb-AND-app (or filler) tokens -> ambiguous, not a command
}

// ============================================================================
// CORTEX — the cognitive front-end (roadmap "micro-thought"). Normalizes +
// classifies the query ONCE and emits a typed plan the cascade reads, instead
// of every tier re-parsing the input and deciding in isolation. Cheap: a single
// tokenize + a few word-list scans. Pure, allocation-free, O(tokens). The plan
// is the substrate the higher levers build on (gated spellfix, single memory
// reclaim, evidential gating).
// ============================================================================

// Query feature bits (computed once from the input).
enum {
    F_DIGIT    = 1u << 0,   // contains a number
    F_MATHOP   = 1u << 1,   // arithmetic operator (symbol, or word: per/diviso/piu/meno…)
    F_DEFWORD  = 1u << 2,   // a definition cue (cos'e / spiega / significa / differenza)
    F_QWORD    = 1u << 3,   // an interrogative (chi/quale/quando/dove/come/perche/quanti)
    F_OPENVERB = 1u << 4,   // an "open/launch app" verb
    F_CREATEVB = 1u << 5,   // a "create/new/write" verb
    F_TEMPORAL = 1u << 6,   // a time-bound word (domani/oggi/ieri/adesso/stamattina)
    F_WEATHER  = 1u << 7,   // weather (tempo/meteo/pioggia/sole/temperatura/gradi)
    F_NEWS     = 1u << 8,   // news/markets (notizie/prezzo/cambio/bitcoin/borsa)
    F_FILENOUN = 1u << 9,   // file/nota/documento/testo
    F_FOLLOWUP = 1u << 11,  // a bare pronoun-y follow-up (lo/la/quello/it/that)
};

typedef enum {
    ICLASS_UNKNOWN = 0,
    ICLASS_COMMAND,   // open app / read live state
    ICLASS_TOOL,      // create_file / math
    ICLASS_FACT,      // definition / entity question -> L1/online knowledge
    ICLASS_LIVE,      // weather/news/price -> live online tier (never cached)
    ICLASS_FOLLOWUP,  // refers to a previous turn
} anima_iclass_t;

typedef struct {
    uint16_t       feat;               // F_* bitfield
    anima_iclass_t klass;              // dominant intent class
    bool           allow_spellfix;     // the command-vocab typo rescue is safe for this query
    bool           reclaim_before_net; // free L1 ONCE before a heavy net/HDC tier (vs 3 scattered unloads)
} anima_plan_t;

// Does any token match a word in a NULL-terminated list (prefix-tolerant via a_match)?
static bool a_any(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, const char *const *list)
{
    for (int t = 0; t < ntok; t++)
        for (int i = 0; list[i]; i++)
            if (a_match(list[i], tok[t])) return true;
    return false;
}

// Build the typed plan for `raw`. The intelligence is which signals are present + a cheap
// dominant-class rule — deterministic, no model. Layers downstream read it instead of re-deciding.
static bool a_action_is_statement(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok);   // fwd: defined below
static void anima_cortex_plan(const char *raw, bool en, anima_plan_t *p)
{
    (void)en;
    memset(p, 0, sizeof(*p));
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int ntok = a_tokenize(raw, tok);

    // Raw scan for digits + symbol operators (the tokenizer drops punctuation). '-' is left out:
    // it appears in filenames/words far more than as "minus" (the word "meno" carries subtraction).
    for (const unsigned char *q = (const unsigned char *)raw; *q; q++) {
        if (isdigit(*q)) p->feat |= F_DIGIT;
        else if (*q=='+'||*q=='*'||*q=='/'||*q=='^'||*q=='%') p->feat |= F_MATHOP;
    }
    static const char *const w_mathop[] = { "per","diviso","fratto","piu","meno","moltiplicato","times","plus","minus","divided","radice","elevato","potenza","percento","percent", NULL };
    static const char *const w_def[]    = { "cos","cosa","spiega","spiegami","significa","significato","definizione","differenza","funziona","serve", NULL };
    static const char *const w_q[]      = { "chi","quale","quali","quando","dove","come","perche","quanti","quanto","quanta", NULL };
    static const char *const w_open[]   = { "apri","aprire","avvia","lancia","mostra","mostrami","open","launch","show","run","portami","metti","riproduci","suona","ascolta","play","start", NULL };
    static const char *const w_create[] = { "crea","creare","crei","nuovo","nuova","create","make","scrivi","annota","appunta","segna","prepara","genera","draft","jot", NULL };
    static const char *const w_temp[]   = { "domani","oggi","ieri","dopodomani","adesso","stamattina","stasera","stanotte","tonight","today","tomorrow","yesterday", NULL };
    static const char *const w_weather[]= { "tempo","meteo","pioggia","piove","sole","nuvoloso","temperatura","gradi","clima","weather","rain","forecast",
        "vento","neve","nevica","nebbia","umidita","temporale","grandine","sereno","soleggiato","piovoso","afa","ventoso","nuvole","piova","piovera","piovuto","wind","snow","sunny","cloudy", NULL };
    static const char *const w_news[]   = { "notizie","prezzo","cambio","bitcoin","borsa","quotazione","azioni","price","stock", NULL };
    static const char *const w_file[]   = { "file","nota","note","documento","document","testo","appunti","foglio", NULL };
    static const char *const w_fu[]     = { "lo","la","quello","quella","questo","questa","esso","essa","aprilo","aprila","it","that","this", NULL };
    if (a_any(tok,ntok,w_mathop)) p->feat |= F_MATHOP;
    if (a_any(tok,ntok,w_def))    p->feat |= F_DEFWORD;
    if (a_any(tok,ntok,w_q))      p->feat |= F_QWORD;
    if (a_any(tok,ntok,w_open))   p->feat |= F_OPENVERB;
    if (a_any(tok,ntok,w_create)) p->feat |= F_CREATEVB;
    if (a_any(tok,ntok,w_temp))   p->feat |= F_TEMPORAL;
    if (a_any(tok,ntok,w_weather))p->feat |= F_WEATHER;
    if (a_any(tok,ntok,w_news))   p->feat |= F_NEWS;
    if (a_any(tok,ntok,w_file))   p->feat |= F_FILENOUN;
    if (a_any(tok,ntok,w_fu))     p->feat |= F_FOLLOWUP;

    uint16_t f = p->feat;
    // Dominant class (cheapest decisive signal first). LIVE beats FACT (a weather phrasing carries a
    // q-word but must not be answered as a definition); math TOOL beats FACT for numeric ops.
    if ((f & F_WEATHER) || (f & F_NEWS) || ((f & F_TEMPORAL) && (f & F_QWORD)))
        p->klass = ICLASS_LIVE;
    else if ((f & F_DIGIT) && (f & F_MATHOP))
        p->klass = ICLASS_TOOL;
    else if ((f & F_CREATEVB) && (f & F_FILENOUN))
        p->klass = ICLASS_TOOL;
    else if ((f & F_DEFWORD) || ((f & F_QWORD) && !(f & F_OPENVERB)))
        p->klass = ICLASS_FACT;
    else if (f & F_OPENVERB)
        p->klass = ICLASS_COMMAND;
    else if (ntok <= 2 && (f & F_FOLLOWUP))
        p->klass = ICLASS_FOLLOWUP;
    else
        p->klass = ICLASS_UNKNOWN;

    // The command-vocab spellfix rescues brittle L0 command/tool words ("ofto"->"foto"). It must
    // NEVER touch a knowledge/live question: those route to L1/online (whose char-ngram encoder is
    // already typo-robust), and "correcting" a valid content word toward the command vocab corrupts
    // them — "che tempo fa domani" had "domani"->"comandi" hijack the query to "capabilities".
    // Also NEVER on an action STATEMENT/negation: the rescue would "correct" the valid past participle
    // "creato"->"crea" and the retry would arm a create the user only DESCRIBED ("ho creato un file").
    p->allow_spellfix = !(f & (F_DEFWORD | F_WEATHER | F_NEWS))
                        && p->klass != ICLASS_FACT && p->klass != ICLASS_LIVE
                        && !a_action_is_statement(tok, ntok);

    // The fact/live/unknown classes are the ones that may reach the heavy net/HDC tiers, which need a
    // large contiguous heap this PSRAM-less chip only has with L1 unloaded. Decide the reclaim ONCE.
    p->reclaim_before_net = (p->klass == ICLASS_FACT || p->klass == ICLASS_LIVE || p->klass == ICLASS_UNKNOWN);
}

// ---- tools (function calling) -----------------------------------------------
// An action verb in the PAST ("ho creato un file", "ho già abbassato la luce") or NEGATED ("non voglio
// creare un file") DESCRIBES or DECLINES an action — it must never EXECUTE one (a fabricated action is the
// action-side of a hallucination). Veto when: a negation is present, OR an auxiliary precedes a past
// participle (IT -ato/-ito/-uto, EN -ed), OR "già"/"already" marks completion. NB the matching imperative
// verbs whose participle prefix-collides are the -ato/-ito family (creato~crea, abbassato~abbassa); the
// auxiliary requirement keeps a genuine request safe ("ho bisogno di creARE un file" — infinitive, no aux+part).
static bool a_action_is_statement(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const neg[] = { "non", "mai", "senza", "not", "dont", "don", "never",
        "didnt", "doesnt", "wont", "cant", NULL };   // "don" covers the "don't"->"don"+"t" apostrophe split
    // Past participles of ACTION verbs that PREFIX-COLLIDE with their imperative (IT creato~crea,
    // abbassato~abbassa; EN created~create, lowered~lower): "ho creato un file" / "i already lowered the
    // brightness" DESCRIBE an action, never command one. Matched by stem + a past ending (IT -ato/-ito/-uto,
    // EN -ed) so it works even when the dropped 2-char auxiliary ("ho"/"i") leaves no other signal. The base
    // imperatives never carry these endings (crea/genera/abbassa/create/lower), so none are caught here.
    static const char *const pstem[] = { "creat", "prepar", "generat", "annotat", "segnat", "abbassat",
        "alzat", "aument", "diminu", "attivat", "disattivat", "impostat", "aggiornat", "modificat",
        "lower", "rais", "increas", "decreas", "turn", "add", NULL };   // EN stems (lowered/raised/added/...)
    // "non dimenticare / don't forget X" is a REMINDER idiom (the negation flips a forget-verb into
    // "remember"), NOT a declined command — so the negation veto must skip it.
    bool forget_idiom = false;
    for (int t = 0; t < ntok; t++)
        if (!strncmp(tok[t], "dimentic", 8) || !strncmp(tok[t], "scord", 5) || !strcmp(tok[t], "forget")) forget_idiom = true;
    for (int t = 0; t < ntok; t++) {
        if (!forget_idiom) for (int i = 0; neg[i]; i++) if (!strcmp(neg[i], tok[t])) return true;   // negated -> not a command
        size_t L = strlen(tok[t]); if (L < 5) continue;
        const char *e = tok[t] + L;
        bool past = (L >= 6 && (!strcmp(e-3,"ato") || !strcmp(e-3,"ito") || !strcmp(e-3,"uto"))) || !strcmp(e-2,"ed");
        if (!past) continue;
        for (int i = 0; pstem[i]; i++) if (!strncmp(tok[t], pstem[i], strlen(pstem[i]))) return true;
    }
    return false;
}

// Detect the create_file intent (IT+EN): a "create/new" verb AND a "file/note" noun.
static bool a_is_create_file(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (a_action_is_statement(tok, ntok)) return false;   // "ho creato…", "non voglio creare…" -> not a command
    // A READ request ("leggimi il file…", "read me the file I will write tomorrow") is not a create — the
    // "write" is a subordinate/future clause, the actual verb is to read. Don't arm a create on it.
    static const char *const readv[] = { "leggi","leggimi","leggere","read","apri","aprire","mostra","mostrami","show","open", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; readv[i]; i++) if (!strcmp(readv[i], tok[t])) return false;
    static const char *verbs[] = { "crea", "creare", "crei", "nuovo", "nuova", "new", "create", "make",
                                   "scrivi", "write", "annota", "appunta", "segna", "prepara", "genera", "draft", "jot", NULL };
    static const char *nouns[] = { "file", "documento", "document", "nota", "note", "testo", "text", "foglio", "appunto", NULL };
    bool v = false, n = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; verbs[i]; i++) if (a_match(verbs[i], tok[t])) v = true;
        for (int i = 0; nouns[i]; i++) if (a_match(nouns[i], tok[t])) n = true;
    }
    return v && n;
}

// Pull a filename from the RAW input (the normalizer would strip the extension dot).
// Prefer a token with a dot; else the token after a trigger word; sanitize + force .txt.
static bool a_extract_filename(const char *raw, char *out, size_t outsz)
{
    char words[A_MAX_TOKENS][40]; int nw = 0, len = 0; char cur[40];
    for (const char *p = raw; ; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == 0) {
            if (len) { cur[len] = 0; if (nw < A_MAX_TOKENS) { memcpy(words[nw++], cur, len + 1); } len = 0; }
            if (!c) break;
        } else if (len < 39) cur[len++] = c;
    }
    static const char *trig[] = { "chiamato", "chiamata", "nome", "named", "called", "titolo", "title", NULL };
    int cand = -1, trig_at = -1;
    for (int i = 0; i < nw && cand < 0; i++) {
        bool hasdot = false, hasalpha = false;
        for (char *q = words[i]; *q; q++) { if (*q == '.') hasdot = true; if (isalpha((unsigned char)*q)) hasalpha = true; }
        if (hasdot && hasalpha) { cand = i; break; }
        char low[40]; int k = 0; for (char *q = words[i]; *q && k < 39; q++) low[k++] = (char)tolower((unsigned char)*q); low[k] = 0;
        for (int j = 0; trig[j]; j++) if (!strcmp(trig[j], low)) trig_at = i;
    }
    if (cand < 0 && trig_at >= 0 && trig_at + 1 < nw) cand = trig_at + 1;
    if (cand < 0) return false;
    int o = 0;
    for (char *q = words[cand]; *q && o < (int)outsz - 5; q++) {
        char c = *q;
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out[o++] = c;
    }
    out[o] = 0;
    if (o == 0 || out[0] == '.') return false;
    if (!strchr(out, '.') && o < (int)outsz - 5) memcpy(out + o, ".txt", 5);   // default extension
    return true;
}


// ---------------------------------------------------------------------------
// The math/skills solver engine lives in anima_solve.c (extracted for scalability).
// Its entry points (anima_solve / a_try_calc / a_fmt_num / units_load) and the two
// symbols it borrows from here (a_norm_phrase / l0_legacy) are declared in
// anima_internal.h. See docs/anima.md �2.
// ---------------------------------------------------------------------------


// ============================================================================
// Agentic controller: a bounded, deterministic policy over a small FSM. No
// generation — the "intelligence" is state + tool dispatch + an uncertainty
// policy, layered on top of the L0/L1 cascade. All state is static (no malloc),
// O(1) RAM, so it fits the MCU. See docs/anima-controller.md.
// ============================================================================

#define ANIMA_RING 8           // working-memory window: last N turns (reference resolution)
#define ANIMA_CHAT 4           // online context window: last N (q,a) turns sent to the cloud teacher
#define ANIMA_REGS 8           // conversational math registers: user-named results across turns

// Session = decision-relevant conversational state. Persists across reboots on the SD.
static struct {
    char last_app[64];         // last launched app (sized to match result.arg)
    char last_file[64];        // last real file touched (routed paths: /data/<Folder>/<name>)
    char last_kind;            // 'a' app / 'f' file — recency tie-break for "aprilo"
    char last_topic[96];       // last knowledge query (for "tell me more")
    // FSM AWAITING_SLOT: a tool asked for a missing argument; next turn fills it.
    char pending_tool[16];     // "" = no pending slot
    char pending_slot[16];     // which argument we're waiting for ("filename" | "folder")
    char pending_arg[48];      // partial argument stashed between turns (e.g. the filename while
                               // we ask which folder it belongs in)
    // FSM CLARIFY: we offered an ambiguous choice; next turn may resolve by ordinal/name.
    char clarify_opt[2][32];   // L0 app clarify: the two app ids we proposed ("" = none pending)
    bool clarify_l1;           // L1 knowledge clarify pending (the dialogic band)
    long clarify_ans[2];       // the two AKB answer offsets offered, resolved by "1"/"2"
    char skill_clarify[32];    // knowledge<->skill clarify: the bare topic ("fisica") awaiting "spiegami" vs "calcola"
    // Working-memory ring (most recent ANIMA_RING turns). `input` is stored only for turns
    // that produced a real action (so "ripeti"/"again" can replay the last actionable one).
    struct { char input[64]; char intent[16]; char domain[12]; char arg[32]; } ring[ANIMA_RING];
    int  ring_head, ring_len;
    // Conversation transcript (q + reply) for the online teacher's MULTI-TURN context — the offline
    // ring above stores only inputs, which can't reconstruct a dialogue to send to Grok. RAM-only,
    // ~1.1 KB .bss; resolves running references ("divo 3?", "e lui?") several turns back, not just one.
    struct { char q[80]; char a[200]; } chat[ANIMA_CHAT];
    int  chat_head, chat_len;
    uint32_t turn;             // monotonic turn counter (telemetry)
    bool dirty;                // session changed since last persist
    // Last substantive answer, for dialogue acts ("sei sicuro?", "spiegati meglio"). A dialogue-act
    // turn does NOT overwrite this, so a meta-question always refers to the real previous answer.
    struct { char reply[200]; anima_tier_t tier; int conf; char intent[16]; } last;
    // Conversational FOCUS for the deductive tier: the (entity, relation) the KGE reasoner last anchored,
    // so a bare follow-up can re-aim it (swap entity or relation) without re-stating the other. RAM-only,
    // an 8-turn recency window via foc_turn (deterministic, no wall-clock); a structured token, never a
    // text fragment. See foc_template/foc_remember. Presence is foc_subject/foc_relation, never foc_turn.
    char foc_subject[48];
    char foc_relation[24];
    uint32_t foc_turn;
    // Conversational numeric memory for the math reasoning layer (anima_reason): the last computed
    // answer + a few user-named results ("chiamalo A"). RAM-only (NOT in the SD serializer), so a
    // reset wipes it; lets an offline cascade carry values across turns. See anima_reg_* below.
    struct { char name[12]; double val; bool used; } reg[ANIMA_REGS];
    double last_num; bool has_last;
} s_session;

#define s_mem s_session        // the old name still reads/writes the same fields

// Push the just-decided turn into the working-memory ring (newest at head). `input` is "" for
// non-actionable turns (a miss, a clarify question) so it never becomes a replay target.
static void ring_push(const char *input, const char *intent, const char *domain, const char *arg)
{
    int i = s_session.ring_head;
    snprintf(s_session.ring[i].input,  sizeof(s_session.ring[i].input),  "%s", input ? input : "");
    snprintf(s_session.ring[i].intent, sizeof(s_session.ring[i].intent), "%s", intent ? intent : "");
    snprintf(s_session.ring[i].domain, sizeof(s_session.ring[i].domain), "%s", domain ? domain : "");
    snprintf(s_session.ring[i].arg,    sizeof(s_session.ring[i].arg),    "%s", arg ? arg : "");
    s_session.ring_head = (i + 1) % ANIMA_RING;
    if (s_session.ring_len < ANIMA_RING) s_session.ring_len++;
}

// Most recent turn that produced a real action (newest first), or NULL. Drives "ripeti".
static const char *ring_last_input(void)
{
    for (int n = 0; n < s_session.ring_len; n++) {
        int i = (s_session.ring_head - 1 - n + ANIMA_RING * 2) % ANIMA_RING;
        if (s_session.ring[i].input[0]) return s_session.ring[i].input;
    }
    return NULL;
}

// Record a substantive (question, answer) pair into the online-context transcript (newest at head).
// Both must be non-empty — a miss/clarify carries no answer worth replaying to the teacher.
static void chat_push(const char *q, const char *a)
{
    if (!q || !q[0] || !a || !a[0]) return;
    int i = s_session.chat_head;
    snprintf(s_session.chat[i].q, sizeof s_session.chat[i].q, "%s", q);
    snprintf(s_session.chat[i].a, sizeof s_session.chat[i].a, "%s", a);
    s_session.chat_head = (i + 1) % ANIMA_CHAT;
    if (s_session.chat_len < ANIMA_CHAT) s_session.chat_len++;
}

// Snapshot the recent transcript into `turns` (cap entries), OLDEST first, for the multi-turn online
// chat. Pointers alias the session ring — valid only until the next chat_push (i.e. within one turn).
// Returns the number of turns filled.
static int chat_context(anima_turn_t *turns, int cap)
{
    int n = s_session.chat_len < cap ? s_session.chat_len : cap;
    for (int k = 0; k < n; k++) {
        int i = (s_session.chat_head - n + k + ANIMA_CHAT * 2) % ANIMA_CHAT;
        turns[k].q = s_session.chat[i].q;
        turns[k].a = s_session.chat[i].a;
    }
    return n;
}

// --- conversational numeric registers (the math reasoning layer's working memory) ---------------
// Backed by s_session, so a /reset (memset) clears them and they never leak between conversations.
// Names are expected pre-normalized (lowercase, no spaces) by the caller — anima_reason always folds
// them through a_flat — so a plain strcmp suffices (no locale-dependent strcasecmp on the MCU).
int anima_reg_last(double *out) { if (!s_session.has_last) return 0; if (out) *out = s_session.last_num; return 1; }
void anima_reg_set_last(double val) { s_session.last_num = val; s_session.has_last = true; }
int anima_reg_get(const char *name, double *out)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < ANIMA_REGS; i++)
        if (s_session.reg[i].used && !strcmp(s_session.reg[i].name, name)) { if (out) *out = s_session.reg[i].val; return 1; }
    return 0;
}
void anima_reg_set(const char *name, double val)
{
    if (!name || !name[0]) return;
    for (int i = 0; i < ANIMA_REGS; i++)                       // overwrite an existing binding
        if (s_session.reg[i].used && !strcmp(s_session.reg[i].name, name)) { s_session.reg[i].val = val; return; }
    for (int i = 0; i < ANIMA_REGS; i++)                       // else take a free slot
        if (!s_session.reg[i].used) { snprintf(s_session.reg[i].name, sizeof s_session.reg[i].name, "%s", name); s_session.reg[i].val = val; s_session.reg[i].used = true; return; }
    for (int i = 1; i < ANIMA_REGS; i++) s_session.reg[i-1] = s_session.reg[i];   // full: evict the oldest
    snprintf(s_session.reg[ANIMA_REGS-1].name, sizeof s_session.reg[ANIMA_REGS-1].name, "%s", name);
    s_session.reg[ANIMA_REGS-1].val = val; s_session.reg[ANIMA_REGS-1].used = true;
}

// "ripeti" / "di nuovo" / "again" / "repeat": replay the last action. Short inputs only, so it
// can't collide with a real request that merely contains the word ("crea di nuovo il file X").
static bool a_is_repeat(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0 || n > 3) return false;
    // "procedi"/"continua"/"vai avanti" also replay the last action — the user's "go ahead" after
    // ANIMA did something (the conversational confirm that makes it feel like an agent).
    static const char *solo[] = { "ripeti", "rifai", "again", "repeat",
                                  "procedi", "continua", "prosegui", "avanti", "proceed", "continue", NULL };
    for (int t = 0; t < n; t++)
        for (int i = 0; solo[i]; i++) if (!strcmp(solo[i], tok[t])) return true;
    bool di = false, nu = false;                 // "di nuovo"
    for (int t = 0; t < n; t++) { if (!strcmp(tok[t], "di")) di = true; if (!strcmp(tok[t], "nuovo")) nu = true; }
    return di && nu;
}

// "1"/"primo"/"first" -> 0, "2"/"secondo"/"second" -> 1, else -1 (resolving a clarify pick).
static int a_pick_ordinal(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0 || n > 3) return -1;
    for (int t = 0; t < n; t++) {
        if (!strcmp(tok[t],"1")||!strcmp(tok[t],"primo")||!strcmp(tok[t],"prima")||!strcmp(tok[t],"first")||!strcmp(tok[t],"uno")||!strcmp(tok[t],"one")) return 0;
        if (!strcmp(tok[t],"2")||!strcmp(tok[t],"secondo")||!strcmp(tok[t],"seconda")||!strcmp(tok[t],"second")||!strcmp(tok[t],"due")||!strcmp(tok[t],"two")) return 1;
    }
    return -1;
}

// Calendar agenda (computed-from-state): "che impegni ho oggi", "cosa devo fare oggi", "chi devo
// vedere oggi". The executor reads /system/config/calendar.json and filters by the RTC date.
static bool a_is_agenda(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *nouns[] = { "impegni","impegno","appuntamenti","appuntamento","agenda",
                                   "eventi","scadenze","scadenza","appointments","schedule", NULL };
    // a capability question ("sai/puoi mettere eventi?") is NOT a request to SHOW the agenda.
    static const char *const cap[] = { "sai","puoi","riesci","potresti","sapresti","can","could","able","how","come", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; cap[i]; i++) if (!strcmp(tok[t], cap[i])) return false;
    // the agenda readout is for TODAY. A different-day scope ("la settimana PROSSIMA", "il giorno della mia
    // NASCITA") is not answerable from today's events -> abstain instead of showing the wrong day's agenda.
    static const char *const otherday[] = { "prossima","prossimo","prossime","prossimi","scorsa","scorso",
        "scorse","scorsi","nascita","nato","nata","passato","futuro","next","last","past","future","born", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; otherday[i]; i++) if (!strcmp(tok[t], otherday[i])) return false;
    bool devo = false, fv = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; nouns[i]; i++) if (a_match(nouns[i], tok[t])) return true;
        if (!strcmp(tok[t], "devo")) devo = true;
        if (!strcmp(tok[t],"fare")||!strcmp(tok[t],"vedere")||!strcmp(tok[t],"ricordare")) fv = true;
    }
    return devo && fv;   // "cosa devo fare oggi", "chi devo vedere oggi"
}

// "What can you do" -> the DYNAMIC capabilities answer (executor builds it from the live app
// registry + the pillars), not a hardcoded list. Distinct from "sai cosa e X" (no "fare" -> L1).
static bool a_is_capabilities(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    // EXACT match (not fuzzy a_match): these are distinctive command words; fuzzy made "elefante"~"elenca"
    // and "adulto"~"abilita" answer "what I can do". Typos are handled upstream by the command spellfix.
    static const char *const kw[] = { "aiuto","help","comandi","funzioni","capacita","elenca","skill","skills","abilita","capabilities", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; kw[i]; i++) if (!strcmp(kw[i], tok[t])) return true;
    // "cosa sai FARE" / "che puoi FARE" / "cosa riesci a FARE": the ability verb must DIRECTLY follow the
    // modal (at most one "a"/"da" between). Without adjacency, "SAI dirmi FAI la somma" (a polite lead-in
    // plus an unrelated imperative) false-fired capabilities and leaked the {value} template.
    static const char *const modal[] = { "sai","puoi","sapresti","riesci","potresti", NULL };
    for (int t = 0; t < ntok; t++) {
        bool md = false; for (int i = 0; modal[i]; i++) if (!strcmp(modal[i], tok[t])) md = true;
        if (!md) continue;
        if (t + 1 < ntok && (!strcmp(tok[t+1],"fare")||!strcmp(tok[t+1],"fai"))) return true;
        if (t + 2 < ntok && (!strcmp(tok[t+1],"a")||!strcmp(tok[t+1],"da")) && (!strcmp(tok[t+2],"fare")||!strcmp(tok[t+2],"fai"))) return true;
    }
    return false;
}

// Network status (computed-from-state): "sei connesso?", "a che wifi sei?", "che IP hai?". The
// executor reads the live Wi-Fi mode/SSID/IP. Must NOT steal the knowledge question "cos'e il
// wifi" (-> L1): a topic word (wifi/rete) alone never fires — it needs a status/possessive
// signal AND no definition word; the strong words (connesso/ip…) fire on their own.
static bool a_is_network(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *strong[] = { "connesso","connessi","connessa","connessione","collegato",
                                    "collegata","collegati","online","connected","ip", NULL };
    static const char *net[]    = { "wifi","rete","internet","network", NULL };
    // NB no bare "che": with "che" any "in CHE anno internet si spegnerà" looked like a status query.
    static const char *state[]  = { "sei","sono","siamo","mia","mio","qual","quale","uso","usi","sto","attuale", NULL };
    static const char *def[]    = { "cos","cosa","spiega","significa","vuol","definizione","differenza","funziona","serve", NULL };
    // NB no "questo/this" nor the to-be verbs "sono/sei/siamo": "persone SONO connesse", "uno due tre SEI"
    // (sei = six) would falsely self-anchor a foreign network question. Only true first-person possessives.
    static const char *const self[] = { "mio","mia","miei","mie","ho","nostro","nostra","my","our", NULL };
    bool s = false, nt = false, st = false, d = false, poss = false, slf = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; def[i];    i++) if (a_match(def[i],    tok[t])) d  = true;
        for (int i = 0; strong[i]; i++) if (a_match(strong[i], tok[t])) s  = true;
        for (int i = 0; net[i];    i++) if (a_match(net[i],    tok[t])) nt = true;
        for (int i = 0; state[i];  i++) if (a_match(state[i],  tok[t])) st = true;
        for (int i = 0; self[i];   i++) if (!strcmp(self[i],   tok[t])) slf = true;
        // a possessive toward a FOREIGN thing ("l'indirizzo ip DEL paradiso") is not a self-status query
        if ((!strcmp(tok[t],"del")||!strcmp(tok[t],"della")||!strcmp(tok[t],"dello")||
             !strcmp(tok[t],"dei")||!strcmp(tok[t],"delle")||!strcmp(tok[t],"of")) && t + 1 < ntok) poss = true;
    }
    if (d) return false;            // it's a definition question -> let L1 answer
    if (poss && !slf) return false; // "ip del paradiso" / "rete della luna" -> not the device's own network
    // A device-status query is SHORT or self-anchored. A long sentence that merely contains "internet"/
    // "connesse" ("quante persone sono connesse a internet in questo istante") is a different question.
    bool concise = slf || ntok <= 5;
    return (s && concise) || (nt && st && concise);   // "che ip", "sei connesso" | "qual e la mia rete"
}

// RAM status (computed-from-state): "quanta RAM libera?", "memoria disponibile?". The executor
// reads the live free heap. Must NOT steal "alloco memoria"/"cos'e la RAM" (-> L1): a memory word
// needs an availability signal (libera/disponibile/quanta…) and no definition/allocation word.
static bool a_is_ram(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *mem[]   = { "ram","memoria", NULL };
    static const char *avail[] = { "libera","libero","liberi","disponibile","disponibili","occupata",
                                   "occupato","usata","resta","rimane","quanta","free", NULL };
    static const char *def[]   = { "cos","cosa","spiega","significa","differenza","alloco","alloca",
                                   "allocare","allocazione","dinamica","heap","stack","serve", NULL };
    bool m = false, a = false, d = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; def[i];   i++) if (a_match(def[i],   tok[t])) d = true;
        for (int i = 0; mem[i];   i++) if (a_match(mem[i],   tok[t])) m = true;
        for (int i = 0; avail[i]; i++) if (a_match(avail[i], tok[t])) a = true;
    }
    if (d) return false;
    return m && a;                  // "quanta ram libera", "memoria disponibile"
}

// Is this a SELF question (about ANIMA), not "chi è <someone-else>"? The whoami intent keys on
// "chi"/"who", which alone hijack "chi è Einstein" / "chi è X" away from the knowledge/teacher
// tier. Require a second-person/self token so only "chi sei (tu)", "come ti chiami", "who are
// you", "presentati", "chi è anima" match; a third-person entity ("chi è X") falls through.
// HOST-ONLY A/B switch: L0_LEGACY=1 reverts THIS round's L0 over-triggering guards (whoami tightening,
// a_ambient_ok, a_solve_date long-incidental) to pre-hardening behaviour, so a before/after diff proves
// the change is a strict improvement. Compiled out of the device build entirely.
bool l0_legacy(void)
{
#ifdef ANIMA_HOST
    return getenv("L0_LEGACY") != NULL;
#else
    return false;
#endif
}

// Genuine self/identity question? The whoami intent keys on bare "chi"/"who"/"you", which appear in
// countless non-identity queries ("do you love me", "what do you think", "who won the cup"). Measured
// against a 311-query OOS set those produced 12 false "Sono ANIMA" answers. So require the identity to
// be the SUBJECT: an interrogative bound to a self-reference (chi sei / who are you / cosa sei / sei
// anima), or "presentati". A bare "you/tu" in a longer sentence is incidental -> falls through to L1.
static bool a_whoami_self(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (l0_legacy()) {   // A/B: pre-hardening lax behaviour (any "you/tu/sei/..." token fires whoami)
        static const char *self[] = { "sei","siete","tu","te","ti","tuo","tua","presentati",
                                      "anima","chiami","you","your","yourself", NULL };
        for (int t=0;t<ntok;t++) for (int i=0;self[i];i++) if (!strcmp(self[i],tok[t])) return true;
        return false;
    }
    bool chi=false, who=false, what=false, are=false, you=false, sei=false,
         anima=false, presentati=false, cosa=false, parl=false;
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if      (!strncmp(w,"parl",4))                      parl = true;   // "con chi sto parlando/parlo"
        else if (!strcmp(w,"chi"))                          chi = true;
        else if (!strcmp(w,"who"))                          who = true;
        else if (!strcmp(w,"what"))                         what = true;
        else if (!strcmp(w,"cosa") || !strcmp(w,"cos"))     cosa = true;
        else if (!strcmp(w,"sei") || !strcmp(w,"siete"))  { sei = true; are = true; }
        else if (!strcmp(w,"are") || !strcmp(w,"r"))        are = true;
        else if (!strcmp(w,"you")||!strcmp(w,"tu")||!strcmp(w,"te")||!strcmp(w,"ti")||!strcmp(w,"your")) you = true;
        else if (!strcmp(w,"anima"))                        anima = true;
        else if (!strcmp(w,"presentati"))                   presentati = true;
    }
    if (presentati) return true;
    if (anima && (sei||you||chi||are||cosa||what||who)) return true;   // "sei anima","chi e anima","are you anima"
    if (chi && (sei||anima||parl))                      return true;   // "chi sei","chi e anima","con chi parlo" (not "chi...you" = song "shape of you")
    if ((who||what) && you && are)                      return true;   // "who/what are you"
    if (cosa && sei)                                    return true;   // "cosa sei"
    return false;
}

// Is `w` an interrogative/quantity question word that, before a keyword, marks it as the question's
// subject ("che ora", "what year", "quanta batteria")?  ("come"/"how" excluded — too broad.)
static bool a_qword(const char *w)
{
    static const char *q[] = { "che","qual","quale","quanta","quanto","quante","quanti",
                               "what","which","when","quando", NULL };
    for (int i = 0; q[i]; i++) if (!strcmp(q[i], w)) return true;
    return false;
}

// A PURE ambient time question ("che ora è", "in che anno siamo", "what year is it") names ONLY the
// time frame plus fillers (to-be, lead-ins, articles, "now/today"). A CONTENT word — a verb other than
// to-be, or a foreign entity — REFRAMES it into a different question the device-clock cannot answer:
//   "in che anno MORIRÀ javascript"   "a che ora TRAMONTA il sole su PLUTONE"   "che ore sono ad ATLANTIDE"
// The past-tense fact-verb guard in a_ambient_ok only caught "morì/nato/…"; future/other verbs and
// trailing entities slipped through the "che anno"/"che ora" adjacency. Any content token => not ambient.
static bool a_temporal_reframed(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const ok[] = {
        // to-be / aux / pronouns
        "e","sono","siamo","sei","siete","sta","stai","stiamo","is","are","am","be","been","it","we","you","there","s",
        // interrogatives (IT a_qword covers most; EN ones are listed explicitly so they aren't "content")
        "what","which","who","whom","whose","when","where","why","how",
        // lead-ins ("dimmi che ora è", "sai che giorno è", "mi dici l'anno", "ricordami che ore sono")
        "dimmi","dim","dici","dirmi","dammi","sai","sapere","puoi","potresti","mostra","mostrami","fammi","mi","ti",
        "tell","know","can","could","would","please","show","give","me","let","ricordami","remind",
        // time frame + now/today (the ambient vocabulary ITSELF never counts as a reframe — every keyword
        // of the date/time/year/season intents must appear here, or the keyword would look "foreign")
        "oggi","adesso","ora","ore","orario","orari","time","clock","attuale","corrente","attualmente","momento",
        "now","today","currently","right","moment","current","anno","anni","annata","year","years","stagione",
        "stagioni","season","seasons","data","date","giorno","giorni","day","days","settimana","week","oggigiorno","weekday",
        // articles / prepositions / conjunctions / fillers
        "il","lo","la","i","gli","le","un","uno","una","l","di","del","dello","della","dei","degli","delle",
        "in","a","ad","al","allo","alla","ai","agli","alle","nel","nello","nella","su","sul","per","qui","che",
        "the","of","at","on","to","here","this","della","and", NULL };
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if (strlen(w) <= 3) continue;                               // short tokens (fillers, typos like "sno") never reframe
        bool isnum = true; for (const char *c = w; *c; c++) if (!isdigit((unsigned char)*c)) { isnum = false; break; }
        if (isnum) continue;                                        // a bare number is not a reframing entity
        if (a_qword(w)) continue;
        bool filler = false;
        for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], w)) { filler = true; break; }
        if (!filler) return true;                                   // a content word -> reframed, not ambient
    }
    return false;
}

// Ambient SYSTEM intents (date/time/year/season/battery) answer from device state and key on very
// common words (time/year/oggi/giorno). They must fire only when the query is ACTUALLY about that —
// the keyword carries the question, not just appears in it. Measured against a 311-query OOS set these
// over-fired 35 times ("favorite band of all time", "this year", "ciao come stai oggi"). Accept only:
// a short query (keyword dominates), >=2 of the intent's keywords, an interrogative bound to a keyword,
// or a qword present in a <=4-word query. Otherwise the keyword is incidental -> let higher tiers run.
static bool a_ambient_ok(const a_intent_t *it, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, int best_score)
{
    // A reframing content word turns a time-readout into a different (often unanswerable) question.
    // Applies to the clock/calendar readouts only — battery keeps its own looser shape ("quanta carica
    // mi resta" legitimately carries the content verb "resta").
    if ((!strcmp(it->id,"date") || !strcmp(it->id,"time") || !strcmp(it->id,"year") || !strcmp(it->id,"season"))
        && a_temporal_reframed(tok, ntok)) return false;
    // A historical FACT verb ("in che anno MORÌ/NATO/ELETTO X") makes this a fact question about an
    // event, NOT the current year/date/season -> never an ambient state answer. Without this, "in che
    // anno morì Obama" answered "Siamo nel 2026" and "in che anno è nato Einstein" missed the HDC fact.
    static const char *const factv[] = { "nato","nata","nascita","morto","morta","mori","deceduto",
        "eletto","eletta","fondato","fondata","costruito","inventato","scritto","scoperto","regnato",
        "born","died","death","elected","founded","built","invented","wrote","discovered","reigned", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; factv[i]; i++) if (!strcmp(factv[i], tok[t])) return false;
    // <=2 (not <=3): a 3-word statement with one incidental ambient keyword ("ho fame oggi", "sono triste
    // oggi") must NOT fire the date/time readout. Real 3-word date/time questions carry a qword ("che ora
    // è") or 2+ keywords ("data di oggi"), both handled below.
    if (ntok <= 2 || best_score >= 2) return true;
    bool has_q = false;
    for (int t = 0; t < ntok; t++) {
        if (a_qword(tok[t])) has_q = true;
        for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
            if (a_match(it->kw[k], tok[t]) && t > 0 && a_qword(tok[t-1])) return true;  // "che ora","what year"
    }
    // Short question with an interrogative ("in che anno siamo"). MUST rest on an EXACT keyword, not a
    // fuzzy prefix collision: "annoia"~"anno" made "quando ANIMA si annoia" answer the current year.
    // Real inflections (giorno->giornata) still pass via the qword-adjacent a_match path above.
    if (!has_q || ntok > 4) return false;
    for (int t = 0; t < ntok; t++)
        for (int k = 0; k < A_MAX_KW && it->kw[k]; k++)
            if (!strcmp(it->kw[k], tok[t])) return true;
    return false;
}

// "spazio" is AMBIGUOUS: device disk space vs PHYSICAL space ("quanto spazio occupa il Pacifico/Sahara/la
// Russia" hit the SD-storage reading on dozens of geography cards). The storage reading fires only with a
// device-storage context (memoria/disco/sd present, or libero/ho/disponibile/rimasto), never on a physical-
// occupation question. memoria/disco/sd are unambiguous and always pass.
static bool a_storage_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    bool unamb = false, ctx = false, occupa = false;
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        // "disco" must be EXACT (+ plural): the fuzzy a_match made "Discord" -> "disco" -> SD-space reading
        // ("nome utente Discord di Cleopatra" answered "Spazio SD: …"). disk/storage/partizion stay fuzzy.
        if (a_match("memoria", w) || !strcmp(w,"disco") || !strcmp(w,"dischi") || a_match("scheda", w)
            || !strcmp(w, "sd") || !strcmp(w,"disk") || !strcmp(w,"disks") || a_match("storage", w)
            || a_match("partizion", w)) unamb = true;   // disco/disk EXACT: fuzzy matched "Discord" -> SD space
        if (!strcmp(w,"libero")||!strcmp(w,"liberi")||!strcmp(w,"disponibile")||!strcmp(w,"rimasto")||
            !strcmp(w,"rimasti")||!strcmp(w,"resta")||!strcmp(w,"occupato")||!strcmp(w,"ho")||!strcmp(w,"rimane")||
            !strcmp(w,"free")||!strcmp(w,"left")||!strcmp(w,"used")||!strcmp(w,"available")) ctx = true;
        if (!strcmp(w,"occupa")||!strcmp(w,"occupano")||!strcmp(w,"copre")||!strcmp(w,"coprono")||
            !strcmp(w,"misura")||!strcmp(w,"estende")||!strcmp(w,"occupies")||!strcmp(w,"covers")) occupa = true;
    }
    if (unamb) return true;                 // memoria/disco/sd/partizione named -> a real storage question
    if (occupa) return false;               // "quanto spazio OCCUPA il Pacifico" -> physical space, not disk
    if (ntok <= 2) {                        // "spazio libero", "quanta memoria" — but only on a REAL storage
        for (int t = 0; t < ntok; t++) {    // word, not a fuzzy false friend: "Discord"~"disco" drops to 1
            const char *w = tok[t];         // token after stopword removal and slipped through as ntok<=2.
            if (!strcmp(w,"spazio")||!strcmp(w,"space")||!strcmp(w,"disco")||!strcmp(w,"dischi")||
                !strcmp(w,"disk")||!strcmp(w,"memoria")||!strcmp(w,"sd")||!strcmp(w,"scheda")||
                !strcmp(w,"storage")||!strcmp(w,"capacita")||!strcmp(w,"gigabyte")||!strcmp(w,"archiviazione"))
                return true;
        }
        return false;
    }
    return ctx;                             // bare "spazio" in a longer query needs a device-storage context
}

// "versione" answers the DEVICE firmware version. It must NOT steal a knowledge question about some
// OTHER software's version ("la versione di python che uscirà nel tremila" -> unknowable/L1), which
// merely contains the word "versione". Fire only when the query is anchored to the device/self (hai/usi/
// sei/è, or firmware/sistema/os/nucleoos/anima), and never when it names another software with "di X".
static bool a_version_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    // NB no generic "this/questo": "in questo istante" must not anchor version (it also fuzzy-collides,
    // "persone"~"versione"). A short version question is allowed by length; a long one needs a real anchor.
    static const char *const self[] = { "hai","usi","sei","gira","giri","esegui","tua","tuo",
        "firmware","sistema","os","nucleoos","nucleo","anima","dispositivo","device","running","your", NULL };
    bool anchored = false, foreign = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; self[i]; i++) if (!strcmp(self[i], tok[t])) anchored = true;
        // "versione di <something>" with a following content noun -> it's about that something, not us.
        if ((!strcmp(tok[t],"di") || !strcmp(tok[t],"of")) && t + 1 < ntok) foreign = true;
    }
    if (foreign) return false;              // "versione di python …" -> about that software, not the device
    if (ntok <= 4) return true;             // short: "che versione", "what version is this"
    return anchored;                        // longer query needs an explicit device anchor
}

// uptime keys on "acceso/avvio/uptime". The command-vocab SPELLFIX rewrites "ultimo" (in "il mio ultimo
// giorno di vita") to "uptime", defeating an exact-match guard — so ALSO require the query to be SHORT:
// a genuine uptime question ("da quanto sei acceso", "uptime") is brief, a reframed sentence is not.
static bool a_uptime_ok(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (ntok > 5) return false;
    static const char *const kw[] = { "uptime","acceso","accesa","accesi","accese","avvio","avviato",
                                      "avviata","boot","booted", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; kw[i]; i++) if (!strcmp(kw[i], tok[t])) return true;
    return false;
}

// "aprilo" / "open it" / "apri quello": an open verb + a pronoun (or the explicit
// inflected form). Distinct from "apri spotify" (a named, unknown app -> not a follow-up).
static bool a_is_followup_open(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *fu[]    = { "aprilo", "aprila", "riaprilo", "riaprila", NULL };
    static const char *openv[] = { "apri", "aprire", "open", "mostra", "show", "riapri", NULL };
    static const char *pron[]  = { "lo", "la", "quello", "quella", "esso", "essa",
                                   "questo", "questa", "it", "that", "this", NULL };
    bool fuv = false, opn = false, prn = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; fu[i]; i++)    if (!strcmp(fu[i], tok[t]))    fuv = true;
        for (int i = 0; openv[i]; i++) if (a_match(openv[i], tok[t])) opn = true;
        for (int i = 0; pron[i]; i++)  if (!strcmp(pron[i], tok[t]))  prn = true;
    }
    return fuv || (opn && prn);
}

// "chiudilo" / "close it" — the close mirror of a_is_followup_open. The pronoun refers to the last app.
static bool a_is_followup_close(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *fu[]     = { "chiudilo", "chiudila", "spegnilo", "spegnila", NULL };
    static const char *closev[] = { "chiudi", "chiudere", "close", "esci", "termina", NULL };
    static const char *pron[]   = { "lo", "la", "quello", "quella", "esso", "essa",
                                    "questo", "questa", "it", "that", "this", NULL };
    bool fuv = false, cv = false, prn = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; fu[i]; i++)     if (!strcmp(fu[i], tok[t]))     fuv = true;
        for (int i = 0; closev[i]; i++) if (a_match(closev[i], tok[t])) cv = true;
        for (int i = 0; pron[i]; i++)   if (!strcmp(pron[i], tok[t]))   prn = true;
    }
    return fuv || (cv && prn);
}

// Drill-down follow-up: "dimmi di più" / "tell me more" / "fammi un esempio" — the user wants
// more on the LAST knowledge topic. Single tokens that are unambiguous, or a "piu"/"more"-style
// word paired with a request verb (so "voglio di più ___" works but a bare "more music" doesn't).
// A request to GENERATE code / a programming snippet — only the online model can write code, so the
// cascade routes these straight to Grok (with a code prompt) instead of a Wikipedia bio / L1 card / a
// "tell me more" drill-down. HIGH precision via WHOLE-TOKEN match (so "chi ha creato python" — a
// knowledge question — does NOT trigger): a strong code noun on its own, OR a language name together
// with a generation verb. "cos'è python" stays a knowledge query (lang but no generation verb).
static bool a_is_code_request(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0) return false;
    static const char *lang[] = { "python","javascript","typescript","golang","kotlin","react",
                                  "java","rust","php","swift","bash","sql","html","css","cpp", NULL };
    static const char *gen[]  = { "scrivi","scrivimi","scrivere","scriva","dammi","fammi","mostrami",
                                  "genera","generami","crea","creami","esempio","esempi","example",
                                  "examples","write","generate","snippet","voglio", NULL };
    static const char *code[] = { "codice","code","script","snippet","programma","algoritmo",
                                  "algorithm","pseudocodice", NULL };
    bool hasLang = false, hasGen = false;
    for (int t = 0; t < n; t++) {
        for (int i = 0; code[i]; i++) if (!strcmp(code[i], tok[t])) return true;   // code noun -> code request
        for (int i = 0; lang[i]; i++) if (!strcmp(lang[i], tok[t])) hasLang = true;
        for (int i = 0; gen[i];  i++) if (!strcmp(gen[i],  tok[t])) hasGen  = true;
    }
    return hasLang && hasGen;
}

static bool a_is_more_request(const char *input)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int n = a_tokenize(input, tok);
    if (n == 0) return false;
    if (a_is_code_request(input)) return false;   // "(dammi un) esempio di codice python" is a code request, not a drill-down
    static const char *solo[] = { "approfondisci", "approfondire", "dettagli", "dettaglio",
                                  "elaborate", "esempio", "example", "continua", NULL };
    static const char *more[] = { "piu", "more", NULL };                  // "di più", "more"
    static const char *verb[] = { "dimmi", "dammi", "raccontami", "dicci", "sai", "spiegami",
                                  "fammi", "tell", "give", "show", "explain", "voglio", NULL };
    bool m = false, v = false;
    for (int t = 0; t < n; t++) {
        for (int i = 0; solo[i]; i++) if (!strcmp(solo[i], tok[t])) return true;
        for (int i = 0; more[i]; i++) if (!strcmp(more[i], tok[t])) m = true;
        for (int i = 0; verb[i]; i++) if (a_match(verb[i], tok[t])) v = true;
    }
    return m && v;
}

static void mem_update(const anima_result_t *r)
{
    if (r->action == ANIMA_ACT_LAUNCH && r->arg[0]) {
        if (strcmp(s_mem.last_app, r->arg) != 0) s_session.dirty = true;
        snprintf(s_mem.last_app, sizeof(s_mem.last_app), "%s", r->arg); s_mem.last_kind = 'a';
    }
    // create_file is NOT remembered here (it isn't created yet, and may be auth-blocked or
    // refused-if-exists). The executor calls nucleo_anima_note_file() once the file is real.
}

// Record a file as the current context (called by the executor after a real create / on
// an already-existing file), so a follow-up "aprilo" opens the right thing.
void nucleo_anima_note_file(const char *path)
{
    if (path && path[0]) {
        snprintf(s_mem.last_file, sizeof(s_mem.last_file), "%s", path);
        s_mem.last_kind = 'f'; s_session.dirty = true;
    }
}

// Close the agentic loop: the executor tells us how the last action really went.
void nucleo_anima_observe(const char *intent, bool ok)
{
    // A blocked/failed create must not leave a dangling pending slot or stale "current file".
    if (!ok && intent && strcmp(intent, "create_file") == 0) {
        s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;
    }
}

// ---- session persistence (survives reboot; written only on change) ----------
static void session_save(void)
{
    if (!s_session.dirty) return;
    FILE *f = fopen(SESSION_PATH, "wb");
    if (!f) { s_session.dirty = false; return; }      // SD absent: don't retry every turn
    fprintf(f, "app=%s\nfile=%s\nkind=%c\ntopic=%s\n", s_mem.last_app, s_mem.last_file,
            s_mem.last_kind ? s_mem.last_kind : '-', s_mem.last_topic);
    fclose(f);
    s_session.dirty = false;
}

static void session_load(void)
{
    FILE *f = fopen(SESSION_PATH, "rb");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if      (!strncmp(line, "app=", 4))   snprintf(s_mem.last_app, sizeof(s_mem.last_app), "%.*s", (int)sizeof(s_mem.last_app) - 1, line + 4);
        else if (!strncmp(line, "file=", 5))  snprintf(s_mem.last_file, sizeof(s_mem.last_file), "%.*s", (int)sizeof(s_mem.last_file) - 1, line + 5);
        else if (!strncmp(line, "kind=", 5))  s_mem.last_kind = (line[5] && line[5] != '-') ? line[5] : 0;
        else if (!strncmp(line, "topic=", 6)) snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%.*s", (int)sizeof(s_mem.last_topic) - 1, line + 6);
    }
    fclose(f);
}

void nucleo_anima_reset_session(void)
{
    memset(&s_session, 0, sizeof(s_session));
    s_session.dirty = true;
    session_save();
}

// Derive the routing "domain" of a result (mirrors the executor's view; used by telemetry).
static const char *a_domain(const anima_result_t *r)
{
    if (!strcmp(r->intent, "clarify")) return "clarify";   // before FACT: an L1 clarify is tier FACT
    if (r->tier == ANIMA_TIER_FACT) return "knowledge";
    if (!strcmp(r->intent, "calc") || !strcmp(r->intent, "base") ||
        !strcmp(r->intent, "prime") || !strcmp(r->intent, "roman") ||
        !strcmp(r->intent, "geo") || !strcmp(r->intent, "phys")) return "calc";
    switch (r->action) {
        case ANIMA_ACT_TOOL:   return "tool";
        case ANIMA_ACT_SYSTEM: return "system";
        case ANIMA_ACT_LAUNCH: return "app";
        case ANIMA_ACT_ANSWER: return "faq";
        default:               return "none";
    }
}

// ---- routing telemetry: the offline-learning work-list (energy-aware) -------
// Append only the queries worth studying: knowledge hits (L1, already paid the SD read) and
// honest misses. Cheap L0 commands are skipped to keep the "L0 does zero SD I/O" principle.
// The file is bounded (rotated past TELEMETRY_CAP); a write failure is silently ignored.
static void telemetry_log(const char *q, const anima_result_t *r, const char *domain)
{
    if (r->tier == ANIMA_TIER_COMMAND) return;
    FILE *f = fopen(TELEMETRY_PATH, "ab");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > TELEMETRY_CAP) { fclose(f); f = fopen(TELEMETRY_PATH, "wb"); if (!f) return; }
    fprintf(f, "{\"t\":%u,\"q\":\"", (unsigned)s_session.turn);
    for (const char *p = q; *p && p < q + 80; p++) { if (*p == '"' || *p == '\\') fputc('\\', f); fputc(*p, f); }
    fprintf(f, "\",\"tier\":\"%s\",\"intent\":\"%s\",\"domain\":\"%s\",\"conf\":%d,\"budget\":%d}\n",
            r->tier == ANIMA_TIER_FACT ? "fact" : "none", r->intent, domain, r->confidence, r->budget);
    fclose(f);
}

// ============================================================================
// REASONING TRACE + CONTENT CHANNEL — the agent's visible "thought log" and the
// payload bus that lets a tool carry more than the 64-byte arg field. Both are
// tiny module-static buffers, reset per query: no heap, MCU-frugal (the trace is
// rendered ONCE into the result at the end, not threaded through every tier).
// The trace turns the single-pass cascade into a Claude-Code-style multi-step view
// both UIs render; the content channel is what makes "compose THEN act" possible.
// ============================================================================
static char s_trace[112];                  // steps taken this turn, " > " joined (ASCII: the device font has no middot)
static void trace_reset(void) { s_trace[0] = 0; }
static void trace_step(const char *step)
{
    if (!step || !*step) return;
    size_t n = strlen(s_trace);
    if (n) { for (const char *s = " > "; *s && n < sizeof(s_trace) - 1; s++) s_trace[n++] = *s; s_trace[n] = 0; }
    snprintf(s_trace + n, sizeof(s_trace) - n, "%s", step);
}

#define AG_CONTENT_MAX 200
static char s_tool_content[AG_CONTENT_MAX]; // composed payload for the next side-effect tool ("" = none)
static void content_reset(void) { s_tool_content[0] = 0; }
const char *nucleo_anima_tool_content(void) { return s_tool_content; }

// Overflow channel for a reply too long for result.reply[1024] — e.g. a multi-line CODE snippet from
// the online model. online_code stashes the FULL text here on the HEAP (not the fixed struct buffer,
// so a long answer never grows the stack), and the web handler serves it via cJSON (which handles long
// strings). Lives only between the query that sets it and the next (single-threaded httpd) -> 0 RAM idle.
static char *s_long_reply = NULL;
const char *nucleo_anima_long_reply(void) { return s_long_reply; }
void nucleo_anima_set_long_reply(const char *s) {
    free(s_long_reply); s_long_reply = NULL;
    if (s && s[0]) s_long_reply = strdup(s);
}

// ---- tool registry (typed function-calling) ---------------------------------
// Each tool owns a turn when its detector fires, filling the result with a real call, an
// ask-for-slot, or a refusal. New tools are one row — the cascade no longer hard-codes them.
typedef struct {
    const char *name;
    bool        side_effect;   // true -> the executor (httpd) runs it under auth
    int (*try_fn)(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r);
} a_tool_t;

// Route a filename to its NucleoOS data folder by extension (mirrors registry/file-
// associations.json: the extension's default app implies the folder). Returns the folder name,
// or NULL if the extension is unknown — the caller then ASKS which folder (never guesses).
static const char *a_folder_for_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return "Documents";     // no extension -> a text note by default
    char ext[12]; int i = 0;
    for (const char *p = dot + 1; *p && i < 11; p++) ext[i++] = (char)tolower((unsigned char)*p);
    ext[i] = 0;
    static const char *docs[]  = { "txt","md","log","json","csv","ini","cfg","yaml","yml","toml",
                                   "xml","html","htm","c","h","cpp","hpp","py","sh","js","css","todo", NULL };
    static const char *pics[]  = { "jpg","jpeg","png","bmp","gif","webp", NULL };
    static const char *music[] = { "mp3","wav","ogg","m4a","flac", NULL };
    static const char *video[] = { "mp4","webm","mov","mkv","avi", NULL };
    for (int j = 0; docs[j];  j++) if (!strcmp(ext, docs[j]))  return "Documents";
    for (int j = 0; pics[j];  j++) if (!strcmp(ext, pics[j]))  return "Pictures";
    for (int j = 0; music[j]; j++) if (!strcmp(ext, music[j])) return "Music";
    for (int j = 0; video[j]; j++) if (!strcmp(ext, video[j])) return "Videos";
    return NULL;
}

// Map a user's folder word (IT/EN) to a real data folder, or NULL.
static const char *a_folder_from_words(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    for (int t = 0; t < ntok; t++) {
        const char *w = tok[t];
        if (!strcmp(w,"documenti")||!strcmp(w,"documento")||!strcmp(w,"documents")||!strcmp(w,"doc")||!strcmp(w,"testo")) return "Documents";
        if (!strcmp(w,"immagini")||!strcmp(w,"immagine")||!strcmp(w,"foto")||!strcmp(w,"pictures")||!strcmp(w,"images")||!strcmp(w,"pics")) return "Pictures";
        if (!strcmp(w,"musica")||!strcmp(w,"music")||!strcmp(w,"audio")||!strcmp(w,"brani")) return "Music";
        if (!strcmp(w,"video")||!strcmp(w,"videos")||!strcmp(w,"filmati")) return "Videos";
    }
    return NULL;
}

// Build the create_file result for a known filename: route it to a folder, or (unknown
// extension) arm the AWAITING_SLOT(folder) state and ask. `folder` overrides routing (used
// when the user just told us the folder). Always produces a /data/<Folder>/<name> path.
static void a_emit_create(const char *name, const char *folder, bool en, anima_result_t *r)
{
    r->tier = ANIMA_TIER_COMMAND;
    snprintf(r->intent, sizeof(r->intent), "create_file");
    if (!folder) folder = a_folder_for_ext(name);
    if (!folder) {                                // unknown extension -> ask which folder
        r->action = ANIMA_ACT_ANSWER; r->awaiting = 1; r->confidence = 70;
        snprintf(r->state, sizeof(r->state), "slot");
        snprintf(r->reply, sizeof(r->reply),
                 en ? "Which folder should %s go in? (Documents, Pictures, Music, Videos)"
                    : "In quale cartella metto %s? (Documenti, Immagini, Musica, Video)", name);
        snprintf(s_session.pending_tool, sizeof(s_session.pending_tool), "create_file");
        snprintf(s_session.pending_slot, sizeof(s_session.pending_slot), "folder");
        snprintf(s_session.pending_arg,  sizeof(s_session.pending_arg),  "%s", name);
        return;
    }
    r->action = ANIMA_ACT_TOOL; r->confidence = 90;
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "/data/%s/%s", folder, name);
    snprintf(r->reply, sizeof(r->reply), en ? "Creating %s in %s." : "Creo %s in %s.", name, folder);
}

// Is an interrogative present? Used to keep how-to QUESTIONS ("come si crea un file?") out of the
// imperative create tool — they're knowledge, not an action. Exact match (these are short words).
static bool a_has_qword(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *q[] = { "come", "cosa", "perche", "quando", "dove", "quale",
                               "how", "what", "why", "when", "where", "which",
                               // capability questions ("posso/puoi creare un file?") are not commands
                               "posso", "puoi", "potresti", "puo", "can", "could", "may", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; q[i]; i++) if (!strcmp(q[i], tok[t])) return true;
    return false;
}

// Tool: create_file. Extracts the filename from the RAW input; if absent it arms the
// AWAITING_SLOT(filename) state and asks (never invents a name). Then routes by extension.
static int tool_create_file(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_is_create_file(tok, ntok)) return 0;
    char name[40];
    if (a_extract_filename(raw, name, sizeof(name))) {
        a_emit_create(name, NULL, en, r);
    } else {
        // No explicit filename AND the input is a question ("come si crea un file?") -> not a create
        // command but a how-to; let it fall through to L1 knowledge instead of asking for a name.
        if (a_has_qword(tok, ntok)) return 0;
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->awaiting = 1;
        snprintf(r->intent, sizeof(r->intent), "create_file");
        snprintf(r->state, sizeof(r->state), "slot");
        snprintf(r->reply, sizeof(r->reply), en ? "What should the file be called? E.g. \"create a file note.txt\"."
                                               : "Come vuoi chiamare il file? Es: \"crea un file note.txt\".");
        r->confidence = 70;
        snprintf(s_session.pending_tool, sizeof(s_session.pending_tool), "create_file");
        snprintf(s_session.pending_slot, sizeof(s_session.pending_slot), "filename");
    }
    return 1;
}

// Tool: calc. Owns the turn when the input is a real arithmetic expression.
static int tool_calc(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    double cv; int ck = a_try_calc(raw, &cv);
    if (!ck) return 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc");
    snprintf(r->state, sizeof(r->state), "tool");
    if (ck == 2) snprintf(r->reply, sizeof(r->reply), en ? "I can't divide by zero." : "Non posso dividere per zero.");
    else { char num[40]; a_fmt_round(cv, num, sizeof(num)); snprintf(r->reply, sizeof(r->reply), en ? "It's %s." : "Fa %s.", num); }   // calcolo base -> max 4 dec
    return 1;
}

// Tool: the unified MATH agent — ONE transversal skill the orchestrator calls for ANY
// calculation. Tries the exact solver (percent / Ohm's law / units / powers / roots / abs /
// modulo) then plain arithmetic as the fallback; returns 1 when it answered exactly (the
// number is computed, never guessed), 0 to let the retrieval cascade take over.
static int tool_math(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    // anima_reason runs FIRST: it owns the conversational/multi-step layer (chains, named registers,
    // self-description) which must orchestrate the single-shot solver, not be pre-empted by it. It reads
    // the previous turn's last-answer (set below), so the order — reason → solve → calc — also keeps the
    // register-read before the register-write within a turn.
    int ok = anima_reason(raw, en, r) || anima_solve(raw, en, r) || tool_calc(raw, tok, ntok, en, r);
    if (ok) { double v; if (a_reply_lastnum(r->reply, &v)) anima_reg_set_last(v); }   // remember for "chiamalo A" / "× 2"
    return ok ? 1 : 0;
}

// ---- agent loop: compose THEN act -------------------------------------------
// The leap from a one-shot Q&A to a deterministic micro-agent: a tool may run a few internal
// reasoning STEPS (compute / take literal text) that fill the content channel, VERIFY the payload,
// then propose ONE side-effect action carrying it. Each step is logged to the trace so both UIs show
// the agent thinking — Claude-Code-style — while staying 100% deterministic (no generation).

// Is `w` a naming word ("con nome X" = filename spec, NOT content)? Lowercased ASCII expected.
static bool a_is_naming(const char *w)
{
    static const char *naming[] = { "nome", "chiamato", "chiamata", "titolo", "title", "named", "called", NULL };
    for (int i = 0; naming[i]; i++) if (!strcmp(w, naming[i])) return true;
    return false;
}

// Content-clause modes — how the agent should turn the clause into a file body:
//   AG_LITERAL   copy the text verbatim   ("con scritto X", "che dice X", ":")
//   AG_AUTO      calc, else if it's a QUESTION answer it from the brain, else literal  (bare "con X")
//   AG_RETRIEVE  always answer the sub from the brain  ("con la risposta a X", "con la definizione di X")
enum { AG_LITERAL = 0, AG_AUTO = 1, AG_RETRIEVE = 2 };

// SAFETY: is this input worth escalating to the ONLINE/knowledge tiers (entity, Wikidata, teacher)?
// A bare DATE (24:04:2027), number, time, cell-ref, code, or wordless/symbol string is NOT a question:
// it must NEVER be sent to the cloud teacher (which would fabricate a "fact") nor learned. Heuristic:
// it must contain a real word — a run of >=3 letters (accented UTF-8 bytes count as letters). Pure
// digits/separators/symbols (dates, numbers, "A1:B10", "x7gq2", "??") have no such run -> not askable.
// The deterministic L0/L1/math tiers still run first; this only fences off the fabrication-prone tiers.
static bool a_is_askable(const char *q)
{
    while (*q == ' ' || *q == '\t') q++;
    int n = (int)strlen(q);
    while (n > 0 && (q[n-1] == ' ' || q[n-1] == '\t' || q[n-1] == '\n' || q[n-1] == '\r')) n--;
    if (n < 3) return false;
    int run = 0, max_run = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)q[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) { if (++run > max_run) max_run = run; }
        else run = 0;
    }
    return max_run >= 3;
}

// Does the sub-clause read as a QUESTION / lookup (so the agent should ANSWER it) rather than literal
// text to copy? Conservative by design (zero-false-write): the cue must be at the START (after at most
// one leading article) — "cos'e X", "la capitale di X", "chi e X" answer; literal text that merely
// CONTAINS a question word later ("la tua risposta a come...", "le cose da fare") stays literal.
static bool a_is_question(const char *sub)
{
    char tok[A_MAX_TOKENS][A_TOK_LEN]; int n = a_tokenize(sub, tok);
    if (n == 0) return false;
    static const char *art[] = { "la", "il", "lo", "i", "gli", "le", "un", "uno", "una", "l", "the", "a", "an", NULL };
    int s = 0;
    for (int i = 0; art[i]; i++) if (!strcmp(art[i], tok[0])) { s = 1; break; }   // skip one leading article
    static const char *cue[] = { "cos", "cosa", "chi", "quale", "quali", "quando", "dove", "come",
        "perche", "quanti", "quanto", "spiega", "significa", "significato", "definizione", "capitale",
        "what", "who", "which", "when", "where", "how", "why", "define", "meaning", "capital", NULL };
    for (int t = s; t < n && t <= s + 1; t++)         // only the first content token (+ one, for "capitale di X")
        for (int i = 0; cue[i]; i++) if (a_match(cue[i], tok[t])) return true;
    return false;
}

// Find a content clause in a create-file request and return the payload (a pointer INTO `raw`, so its
// case is preserved), or NULL. Sets *mode (LITERAL / AUTO / RETRIEVE). Explicit text connectors win
// ("con scritto X", "che dice X", ":"); "con la risposta/definizione di X" asks the brain; bare "con X"
// is content only when not a naming clause ("con nome X").
static const char *a_content_clause(const char *raw, int *mode)
{
    *mode = AG_LITERAL;
    const char *colon = strchr(raw, ':');                 // explicit "...: rest" wins outright (literal)
    if (colon && colon[1]) { const char *c = colon + 1; while (*c == ' ') c++; if (*c) return c; }

    const char *start[A_MAX_TOKENS]; char low[A_MAX_TOKENS][24]; int n = 0;
    for (const char *p = raw; *p && n < A_MAX_TOKENS; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start[n] = p; int k = 0;
        while (*p && *p != ' ' && *p != '\t') {
            unsigned char uc = (unsigned char)*p;
            if (uc == 0xC3 && p[1]) {                       // fold IT accents so "venerdì" matches WD "venerdi"
                unsigned char d = (unsigned char)p[1];
                char f = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':(d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':0;
                if (f) { if (k < 23) low[n][k++] = f; p += 2; continue; }
            }
            if (k < 23) low[n][k++] = (char)tolower(uc); p++;
        }
        low[n][k] = 0; n++;
    }
#define TOK_AT(j) (((j) < n) ? start[j] : NULL)
    for (int i = 0; i < n; i++) {
        const char *w = low[i], *n1 = (i + 1 < n) ? low[i + 1] : "", *n2 = (i + 2 < n) ? low[i + 2] : "";
        const char *n3 = (i + 3 < n) ? low[i + 3] : "";
        // RETRIEVE connectors (answer the sub from the offline brain): "con la risposta/definizione a/di X"
        if (!strcmp(w, "con") && !strcmp(n1, "la") && (!strcmp(n2, "risposta") || !strcmp(n2, "definizione")) &&
            (!strcmp(n3, "a") || !strcmp(n3, "di") || !strcmp(n3, "su"))) { *mode = AG_RETRIEVE; return TOK_AT(i + 4); }
        if (!strcmp(w, "con") && (!strcmp(n1, "risposta") || !strcmp(n1, "definizione")) &&
            (!strcmp(n2, "a") || !strcmp(n2, "di") || !strcmp(n2, "su"))) { *mode = AG_RETRIEVE; return TOK_AT(i + 3); }
        if (!strcmp(w, "rispondendo") && !strcmp(n1, "a")) { *mode = AG_RETRIEVE; return TOK_AT(i + 2); }
        if (!strcmp(w, "answering")) { *mode = AG_RETRIEVE; return TOK_AT(i + 1); }
        // 3-token LITERAL connectors (longest match wins at this position)
        if (!strcmp(w, "con")  && !strcmp(n1, "il")  && !strcmp(n2, "testo")) return TOK_AT(i + 3);
        if (!strcmp(w, "with") && !strcmp(n1, "the") && !strcmp(n2, "text"))  return TOK_AT(i + 3);
        // 2-token LITERAL connectors
        if (!strcmp(w, "con")  && (!strcmp(n1, "scritto") || !strcmp(n1, "testo") || !strcmp(n1, "dentro"))) return TOK_AT(i + 2);
        if (!strcmp(w, "che")  && (!strcmp(n1, "dice") || !strcmp(n1, "contiene") || !strcmp(n1, "recita"))) return TOK_AT(i + 2);
        if (!strcmp(w, "with") && (!strcmp(n1, "text") || !strcmp(n1, "content"))) return TOK_AT(i + 2);
        if (!strcmp(w, "that") && (!strcmp(n1, "says") || !strcmp(n1, "reads"))) return TOK_AT(i + 2);
        // 1-token LITERAL connectors
        if (!strcmp(w, "scritto") || !strcmp(w, "contenente") || !strcmp(w, "dicendo") ||
            !strcmp(w, "saying") || !strcmp(w, "containing"))
            return TOK_AT(i + 1);
        // bare connectors -> AUTO (calc / answer-if-question / literal)
        if (!strcmp(w, "con"))  { if (a_is_naming(n1)) continue; *mode = AG_AUTO; return TOK_AT(i + 1); }
        if (!strcmp(w, "with")) { *mode = AG_AUTO; return TOK_AT(i + 1); }
    }
    return NULL;
#undef TOK_AT
}

// Compose a tool payload from a sub-query — the agent's per-step skill dispatch:
//   1) a CALCULATION -> compute it          (math sub-skill, exact)
//   2) AUTO+question or RETRIEVE -> ANSWER it from the offline brain (L1, conf-gated -> zero fabrication)
//   3) otherwise -> literal text
// This is the "research then produce" step: ANIMA reads its own knowledge and writes the answer to a
// file. Logs each step to the visible trace. Pure offline, no generation.
#define AG_RECALL_MINCONF 78    // a retrieved fact must be confident before it's written to a FILE (reliability)
static bool ag_compose(const char *sub, int mode, bool en, char *out, size_t outsz)
{
    while (*sub == ' ') sub++;
    if (!*sub) return false;
    if (mode != AG_LITERAL) {                          // "con scritto X" stays verbatim; else compute/answer
        double v; int ck = a_try_calc(sub, &v);
        if (ck == 1) {
            char num[40]; a_fmt_num(v, num, sizeof(num));
            snprintf(out, outsz, "%s", num);
            char step[56]; snprintf(step, sizeof(step), en ? "compute %.24s=%s" : "calcolo %.24s=%s", sub, num);
            trace_step(step);
            return true;
        }
        // full math agent (percent / units / roots / powers / Ohm), not just basic arithmetic
        anima_result_t mr; memset(&mr, 0, sizeof(mr));
        if (anima_solve(sub, en, &mr) && mr.reply[0]) {
            snprintf(out, outsz, "%s", mr.reply);
            trace_step(en ? "solve" : "risolvo");
            return true;
        }
        if (mode == AG_RETRIEVE || (mode == AG_AUTO && a_is_question(sub))) {
            anima_result_t kr; memset(&kr, 0, sizeof(kr));
            // STRUCTURED deduction first (KGE/HDC, edge-grounded -> precise, zero fabrication): lets the
            // agent WRITE a *deduced* fact ("la capitale della Francia" -> Parigi) that L1 alone, which
            // only stores cards, does not hold. The reasoner's own coherence guards reject a wrong aim.
            bool ok = nucleo_anima_hdc_reason(sub, en ? "en" : "it", &kr) && kr.reply[0];
            // else High-confidence L1 retrieval: a permanent file must never hold a borderline guess.
            if (!ok) { memset(&kr, 0, sizeof(kr));
                ok = nucleo_anima_l1_query(sub, en, false, &kr) && kr.reply[0] && kr.confidence >= AG_RECALL_MINCONF; }
            if (!ok && !a_is_question(sub)) {                 // a bare term -> ask the question form ("cos'e X")
                char qf[120]; snprintf(qf, sizeof(qf), en ? "what is %.100s" : "cos'e %.100s", sub);
                memset(&kr, 0, sizeof(kr));
                ok = nucleo_anima_l1_query(qf, en, false, &kr) && kr.reply[0] && kr.confidence >= AG_RECALL_MINCONF;
            }
            if (ok) {
                snprintf(out, outsz, "%s", kr.reply);
                char step[48]; snprintf(step, sizeof(step), en ? "recall: %.20s" : "cerco: %.20s", sub);
                trace_step(step);
                return true;
            }
            trace_step(en ? "unsure -> literal" : "incerto -> letterale");   // never write a guessed fact
        }
    }
    snprintf(out, outsz, "%s", sub);
    trace_step(en ? "literal text" : "testo letterale");
    return true;
}

// Tool: compose-then-act note. Fires on a create-file request that ALSO carries a content clause —
// runs the internal plan (split filename | content, compose the payload, self-verify) and proposes a
// create_file action with the payload on the content channel. Returns 0 (no content clause) so the
// plain empty-file tool_create_file handles the legacy case. The flagship of the agent loop.
static int tool_note(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_is_create_file(tok, ntok)) return 0;
    int mode; const char *content = a_content_clause(raw, &mode);
    if (!content) return 0;
    trace_step(en ? "plan: compose+write" : "piano: componi+scrivi");

    char head[160]; size_t hl = (size_t)(content - raw);  // text before the connector -> filename scan
    if (hl >= sizeof(head)) hl = sizeof(head) - 1;
    memcpy(head, raw, hl); head[hl] = 0;
    char name[40];
    if (!a_extract_filename(head, name, sizeof(name)))
        snprintf(name, sizeof(name), en ? "note.txt" : "nota.txt");   // sensible agent default

    char body[AG_CONTENT_MAX];
    if (!ag_compose(content, mode, en, body, sizeof(body)) || !body[0]) {
        trace_step(en ? "abort: empty payload" : "annullo: payload vuoto");
        return 0;                                          // nothing to write -> fall through
    }
    snprintf(s_tool_content, sizeof(s_tool_content), "%s", body);
    trace_step(en ? "verify: ok" : "verifica: ok");

    a_emit_create(name, NULL, en, r);                      // routes by extension, sets ACT_TOOL + path
    if (r->action == ANIMA_ACT_TOOL) {
        char prev[48]; snprintf(prev, sizeof(prev), "%.40s", body);
        snprintf(r->reply, sizeof(r->reply), en ? "Creating %s with: %s" : "Creo %s con: %s", name, prev);
    }
    return 1;
}

// Tool: device settings (volume / brightness). Proposes a typed action the executor runs by calling
// the public setter — kept executor-side so nucleo_anima needs no nucleo_app/nucleo_audio dependency
// (no link cycle). Absolute "a 50" -> arg "50"; bare "alza/abbassa" -> arg "+10"/"-10" (relative).
static int tool_setting(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (a_action_is_statement(tok, ntok)) return 0;       // "ho già abbassato la luce" -> a statement, don't mutate
    int target = 0; bool explicit_tgt = false;            // explicit = a real device noun was named
    for (int t = 0; t < ntok; t++) {
        if (a_match("volume", tok[t]) || a_match("audio", tok[t]) || a_match("suono", tok[t])) { target = 1; explicit_tgt = true; }
        if (a_match("luminosita", tok[t]) || a_match("brightness", tok[t]) ||
            a_match("schermo", tok[t]) || a_match("luce", tok[t])) { target = 2; explicit_tgt = true; }
        // brightness IMPLIED (not named): "fai più CHIARO", "metti BUIO" — only acts WITH a command verb,
        // so a bare statement ("ho paura del buio") never fires a settings mutation.
        if (a_match("buio", tok[t]) || a_match("scuro", tok[t]) || a_match("chiaro", tok[t]) ||
            a_match("luminoso", tok[t]) || a_match("luminosa", tok[t]) || a_match("brighter", tok[t]) ||
            a_match("darker", tok[t]) || a_match("dim", tok[t]) || a_match("brighten", tok[t])) { if (!target) target = 2; }
    }
    if (!target) return 0;

    // DEFER to geometry/physics: a "volume"/"suono" that is actually a SHAPE volume or a sound-speed
    // problem ("volume del cubo lato 3", "densità massa 100 volume 50", "il suono viaggia a 340 m/s ...
    // secondi") is NOT a settings command -> let the math tier compute it. (exact match, no fuzzy.)
    {
        static const char *const geo[] = { "cubo","sfera","cilindro","cono","cerchio","quadrato","triangolo",
            "rettangolo","trapezio","piramide","lato","raggio","altezza","diametro","densita","massa","viaggia",
            "velocita","spazio","secondi","cube","sphere","cylinder","cone","circle","triangle","rectangle",
            "side","radius","height","diameter","density","mass","travels","speed","seconds", NULL };
        for (int t = 0; t < ntok; t++) for (int i = 0; geo[i]; i++) if (!strcmp(tok[t], geo[i])) return 0;
    }

    int verb_dir = 0, adj_dir = 0, quant_dir = 0; bool cmd = false;
    for (int t = 0; t < ntok; t++) {
        // imperative DIRECTION verbs (strongest: command + direction)
        if (a_match("alza", tok[t]) || a_match("aumenta", tok[t]) || a_match("raise", tok[t]) || a_match("increase", tok[t]) || a_match("brighten", tok[t])) { verb_dir = +1; cmd = true; }
        if (a_match("abbassa", tok[t]) || a_match("diminuisci", tok[t]) || a_match("riduci", tok[t]) || a_match("lower", tok[t]) || a_match("decrease", tok[t]) || a_match("dim", tok[t])) { verb_dir = -1; cmd = true; }
        // pure SET/CHANGE/MUTE verbs (command, no direction by itself)
        if (a_match("imposta", tok[t]) || a_match("metti", tok[t]) || a_match("set", tok[t]) ||
            a_match("porta", tok[t]) || a_match("regola", tok[t]) || a_match("cambia", tok[t]) ||
            a_match("setta", tok[t]) || a_match("settare", tok[t]) || a_match("settiamo", tok[t]) ||
            a_match("modifica", tok[t]) || a_match("aggiusta", tok[t]) || a_match("change", tok[t]) ||
            a_match("adjust", tok[t]) || a_match("turn", tok[t]) || a_match("fai", tok[t]) ||
            a_match("rendi", tok[t]) || a_match("make", tok[t]) ||
            a_match("muta", tok[t]) || a_match("silenzia", tok[t]) || a_match("azzera", tok[t]) || a_match("mute", tok[t]) || a_match("spegni", tok[t])) cmd = true;
        // comparative ADJECTIVE direction ("più ALTO/BASSO/CHIARO/SCURO") — beats a bare quantifier
        if (a_match("alto", tok[t]) || a_match("alta", tok[t]) || a_match("higher", tok[t]) || a_match("up", tok[t]) ||
            a_match("chiaro", tok[t]) || a_match("luminoso", tok[t]) || a_match("brighter", tok[t])) adj_dir = +1;
        if (a_match("basso", tok[t]) || a_match("bassa", tok[t]) || a_match("down", tok[t]) ||
            a_match("scuro", tok[t]) || a_match("buio", tok[t]) || a_match("darker", tok[t])) adj_dir = -1;
        // bare quantifier ("PIÙ volume" / "MENO luce") — weakest, only when no adjective gave a direction
        if (a_match("piu", tok[t]) || a_match("more", tok[t])) { if (!quant_dir) quant_dir = +1; }
        if (a_match("meno", tok[t]) || a_match("less", tok[t])) { if (!quant_dir) quant_dir = -1; }
    }
    int dir = verb_dir ? verb_dir : (adj_dir ? adj_dir : quant_dir);   // verb > adjective > quantifier

    int val = -1;                                          // first integer in the raw input, if any
    for (const char *p = raw; *p; p++) if (isdigit((unsigned char)*p)) { val = (int)strtol(p, NULL, 10); break; }
    if (val < 0) for (int t = 0; t < ntok; t++) {          // spelled amounts: "a zero" / "al massimo" / "a metà" / "muto"
        if (a_match("zero", tok[t]) || a_match("spento", tok[t]) || a_match("spegni", tok[t]) || a_match("muto", tok[t]) || a_match("muta", tok[t]) ||
            a_match("silenzia", tok[t]) || a_match("azzera", tok[t]) || a_match("mute", tok[t]) || a_match("off", tok[t])) { val = 0; break; }
        if (a_match("massimo", tok[t]) || a_match("massima", tok[t]) || a_match("max", tok[t]) || a_match("full", tok[t])) { val = 100; break; }
        if (a_match("minimo", tok[t]) || a_match("minima", tok[t]) || a_match("min", tok[t])) { val = 0; break; }
        if (a_match("meta", tok[t]) || a_match("half", tok[t])) { val = 50; break; }
    }

    // FIRE only on a real command: an imperative verb, OR a SHORT named-target phrase carrying a
    // direction/amount ("volume più alto", "volume al 50"). The verb-less path is capped at 4 tokens so a
    // long STATEMENT ("odio quando il volume è troppo alto") never fires a mutation. A bare adjective
    // ("...del buio") never mutates a setting either.
    if (!(cmd || (explicit_tgt && (dir != 0 || val >= 0) && ntok <= 4))) return 0;

    char argbuf[12];
    if (val >= 0) { if (val > 100) val = 100; snprintf(argbuf, sizeof(argbuf), "%d", val); }
    else if (dir) snprintf(argbuf, sizeof(argbuf), "%+d", dir * 10);
    else return 0;                                         // "imposta il volume" with no amount -> miss

    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_TOOL; r->confidence = 88;
    snprintf(r->intent, sizeof(r->intent), target == 1 ? "set_volume" : "set_brightness");
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "%s", argbuf);       // machine form: "70" | "+10" | "-10"
    const char *what = target == 1 ? "volume" : (en ? "brightness" : "luminosita");
    const char *art  = target == 1 ? "il" : "la";         // Italian gender: il volume / la luminosita
    if (en) {
        if (val >= 0) snprintf(r->reply, sizeof(r->reply), "Setting the %s to %d%%.", what, val);
        else          snprintf(r->reply, sizeof(r->reply), dir > 0 ? "Raising the %s." : "Lowering the %s.", what);
    } else {
        if (val >= 0) snprintf(r->reply, sizeof(r->reply), "Imposto %s %s al %d%%.", art, what, val);
        else          snprintf(r->reply, sizeof(r->reply), dir > 0 ? "Alzo %s %s." : "Abbasso %s %s.", art, what);
    }
    trace_step(en ? "tool: device setting" : "tool: impostazione");
    return 1;
}

// Tool: add_event (calendar reminder) — the agent SCHEDULES something. Fires on a reminder phrasing
// ("ricordami di X", "remind me to X") or a create+event-noun ("aggiungi un evento ..."). Parses the
// WHEN (day offset: oggi/domani/dopodomani/"tra N giorni"), an optional TIME ("alle HH[:MM]"), and the
// TEXT; packs them on the content channel as "off=<d>;time=<HH:MM|>;text=<...>" and proposes ONE
// ANIMA_ACT_TOOL the executor resolves against the RTC date and appends to the OS calendar.
static bool a_event_trigger(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    if (a_action_is_statement(tok, ntok)) return false;   // "ho creato un evento", "non aggiungere…" -> not a command
    static const char *remind[] = { "ricordami", "ricorda", "promemoria", "reminder", "remind", "dimenticare", "dimenticarmi", "forget", NULL };
    static const char *verbs[]  = { "crea", "creare", "aggiungi", "segna", "nuovo", "nuova", "add", "new", "set", "metti",
                                    "schedule", "pianifica", "programma", "fissa", "prenota", NULL };
    static const char *nouns[]  = { "evento", "eventi", "appuntamento", "appuntamenti", "impegno", "event", "appointment",
                                    "calendario", "calendar", "agenda", "meeting", "riunione", "incontro", NULL };
    // delete/cancel is NOT a create — "cancella il promemoria" must not fabricate a new reminder.
    static const char *del[]    = { "cancella", "elimina", "rimuovi", "togli", "delete", "remove", "cancel", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; del[i]; i++) if (a_match(del[i], tok[t])) return false;
    // "(me/te) LO ricordi?" / "LO ricordi?" = a RECALL question ("do you recall it"), NOT a reminder TASK.
    // An object clitic (lo/la/li/le/ne) right before ricorda/ricordi marks recall — "mi chiamo X, lo ricordi"
    // and "che anno è, me lo ricordi?" must NOT fabricate a calendar reminder. ("ricordami di …" is untouched.)
    for (int t = 1; t < ntok; t++)
        if ((a_match("ricorda", tok[t]) || !strcmp(tok[t], "ricordi")) &&
            (!strcmp(tok[t-1],"lo")||!strcmp(tok[t-1],"la")||!strcmp(tok[t-1],"li")||!strcmp(tok[t-1],"le")||!strcmp(tok[t-1],"ne")))
            return false;
    // interrogatives that make "ricordami <X>" a QUESTION, not a task. NB: NOT "che" — it is also a
    // conjunction ("ricordami che ho la riunione" is a real reminder). IT "quanto/quanti/significato"
    // were missing, so "ricordami quanto fa 2+2" fabricated a junk calendar entry (EN "what" worked).
    static const char *qwords[] = { "come", "cosa", "cos", "chi", "quando", "dove", "perche", "quale", "quali",
                                    "quanto", "quanti", "quanta", "quante", "significato", "significa", "spiega", "spiegami",
                                    "how", "what", "who", "when", "where", "why", "which",
                                    "posso", "puoi", "potresti", "puo", "sai", "riesci", "sapresti",
                                    "can", "could", "may", "able", NULL };
    bool rem = false, v = false, n = false, q = false; bool has_ore = false, has_sono = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; remind[i]; i++) if (a_match(remind[i], tok[t])) rem = true;
        for (int i = 0; verbs[i];  i++) if (a_match(verbs[i],  tok[t])) v = true;
        for (int i = 0; nouns[i];  i++) if (a_match(nouns[i],  tok[t])) n = true;
        for (int i = 0; qwords[i]; i++) if (!strcmp(qwords[i], tok[t])) q = true;   // "ricordami COME..." = a question
        if (!strcmp(tok[t], "ore")) has_ore = true;
        if (!strcmp(tok[t], "sono")) has_sono = true;
    }
    if (q) return false;                          // a how/what/when question is never a reminder to schedule
    if (has_ore && has_sono) return false;        // "ricordami che ore sono" — a time question, not a task
    return rem || (v && n);
}

static int tool_event(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!a_event_trigger(tok, ntok)) return 0;
    // A create-FILE command whose CONTENT merely contains a reminder word ("crea un file con il testo
    // ...un promemoria", "scrivi una nota che dice ricordati la riunione") is a FILE, not an event ->
    // fall through to tool_note (next in the registry). A genuine "ricordami di scrivere la nota domani"
    // has no content connector, so it still schedules.
    { int m; if (a_is_create_file(tok, ntok) && a_content_clause(raw, &m)) return 0; }
    // DELETE/CANCEL is not a create — scan the RAW string (the normalizer can corrupt "cancella"->"cartella",
    // dodging the tokenized guard). "cancella/elimina il promemoria" must abstain, not fabricate an event.
    { char rl[160]; size_t z = 0; for (; raw[z] && z + 1 < sizeof rl; z++) rl[z] = (char)tolower((unsigned char)raw[z]); rl[z] = 0;
      static const char *const drw[] = { "cancell", "elimin", "rimuov", "delete", "remove", "annulla", NULL };
      for (int i = 0; drw[i]; i++) if (strstr(rl, drw[i])) return 0; }

    const char *start[A_MAX_TOKENS]; char low[A_MAX_TOKENS][24]; int n = 0;
    for (const char *p = raw; *p && n < A_MAX_TOKENS; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start[n] = p; int k = 0;
        while (*p && *p != ' ' && *p != '\t') {
            unsigned char uc = (unsigned char)*p;
            if (uc == 0xC3 && p[1]) {                       // fold IT accents so "venerdì" matches WD "venerdi"
                unsigned char d = (unsigned char)p[1];
                char f = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':(d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':0;
                if (f) { if (k < 23) low[n][k++] = f; p += 2; continue; }
            }
            if (k < 23) low[n][k++] = (char)tolower(uc); p++;
        }
        low[n][k] = 0; n++;
    }

    bool drop[A_MAX_TOKENS]; for (int i = 0; i < n; i++) drop[i] = false;

    // WHEN: day offset (default today). Drop the when-words from the text. localtime gives today's
    // weekday so a named day ("venerdì") resolves to its NEXT occurrence.
    struct tm lt; { time_t now = time(NULL); lt = *localtime(&now); }
    static const char *const WD[7][3] = {   // index = tm_wday: 0=Sunday .. 6=Saturday
        {"domenica","sunday",NULL}, {"lunedi","monday",NULL}, {"martedi","tuesday",NULL},
        {"mercoledi","wednesday",NULL}, {"giovedi","thursday",NULL}, {"venerdi","friday",NULL}, {"sabato","saturday",NULL} };
    int off = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(low[i], "oggi") || !strcmp(low[i], "today")) { off = 0; drop[i] = true; }
        else if (!strcmp(low[i], "dopodomani")) { off = 2; drop[i] = true; }
        // EN "(the day) after tomorrow" -> +2 (must beat the bare 'tomorrow' below)
        else if (!strcmp(low[i], "tomorrow") && i >= 1 && !strcmp(low[i - 1], "after")) {
            off = 2; drop[i] = true; drop[i - 1] = true; if (i >= 2 && !strcmp(low[i - 2], "day")) drop[i - 2] = true;
        }
        else if (!strcmp(low[i], "domani") || !strcmp(low[i], "tomorrow")) { off = 1; drop[i] = true; }
        else if ((!strcmp(low[i], "tra") || !strcmp(low[i], "fra") || !strcmp(low[i], "in")) && i + 1 < n) {
            // "tra N giorni" / "tra una|due settimane" / "in a week" — number word or digit, days or weeks.
            int q = 0, ni = i + 1;
            if (isdigit((unsigned char)low[ni][0])) q = atoi(low[ni]);
            else if (!strcmp(low[ni],"una")||!strcmp(low[ni],"un")||!strcmp(low[ni],"a")||!strcmp(low[ni],"one")) q = 1;
            else if (!strcmp(low[ni],"due")||!strcmp(low[ni],"two")) q = 2;
            else if (!strcmp(low[ni],"tre")||!strcmp(low[ni],"three")) q = 3;
            if (q > 0) {
                bool wk = (ni + 1 < n) && (!strcmp(low[ni+1],"settimana")||!strcmp(low[ni+1],"settimane")||
                                           !strcmp(low[ni+1],"week")||!strcmp(low[ni+1],"weeks"));
                bool dy = (ni + 1 < n) && (!strcmp(low[ni+1],"giorni")||!strcmp(low[ni+1],"giorno")||
                                           !strcmp(low[ni+1],"days")||!strcmp(low[ni+1],"day"));
                int days = wk ? q * 7 : q;
                if (days > 0 && days <= 60) { off = days; drop[i] = true; drop[ni] = true; if (wk || dy) drop[ni + 1] = true; }
            }
        }
        else {   // a named weekday -> its NEXT occurrence (today's name means next week, +7)
            int w = -1;
            for (int d = 0; d < 7 && w < 0; d++) for (int s = 0; WD[d][s]; s++) if (!strcmp(low[i], WD[d][s])) { w = d; break; }
            if (w >= 0) { off = (w - lt.tm_wday + 7) % 7; if (off == 0) off = 7; drop[i] = true;
                if (i >= 1 && (!strcmp(low[i-1],"prossimo")||!strcmp(low[i-1],"prossima")||!strcmp(low[i-1],"next")||!strcmp(low[i-1],"questo"))) drop[i-1] = true; }
        }
    }

    // TIME: "alle/ore/at HH[:MM]" preferred, else a bare HH:MM or 12-hour "8pm"/"8am". A separate
    // "pomeriggio"/"sera"/"pm" token also shifts to PM.
    int hh = -1, mm = 0;
    for (int i = 0; i < n; i++)
        if ((!strcmp(low[i], "alle") || !strcmp(low[i], "ore") || !strcmp(low[i], "at")) && i + 1 < n &&
            isdigit((unsigned char)low[i + 1][0])) {
            hh = atoi(low[i + 1]); const char *c = strchr(low[i + 1], ':'); if (c) mm = atoi(c + 1);
            if (strstr(low[i + 1], "pm")) { if (hh >= 1 && hh < 12) hh += 12; } else if (strstr(low[i + 1], "am")) { if (hh == 12) hh = 0; }
            drop[i] = true; drop[i + 1] = true; break;
        }
    if (hh < 0)
        for (int i = 0; i < n; i++) {
            if (!isdigit((unsigned char)low[i][0])) continue;
            const char *c = strchr(low[i], ':');
            bool pm = strstr(low[i], "pm") != NULL, am = strstr(low[i], "am") != NULL;
            if (c) { hh = atoi(low[i]); mm = atoi(c + 1); drop[i] = true; if (pm && hh < 12) hh += 12; else if (am && hh == 12) hh = 0; break; }
            if (pm || am) { hh = atoi(low[i]); if (pm) { if (hh >= 1 && hh < 12) hh += 12; } else if (hh == 12) hh = 0; drop[i] = true; break; }
        }
    for (int i = 0; i < n; i++)
        if (!strcmp(low[i], "pomeriggio") || !strcmp(low[i], "sera") || !strcmp(low[i], "pm")) {
            if (hh >= 1 && hh < 12) hh += 12;
            drop[i] = true;
        }
    if (hh > 23 || mm > 59) { hh = -1; mm = 0; }

    // Drop the LEADING structural run (trigger / verb / event-noun / article / "di"/"che"/"to"/"that").
    static const char *lead[] = { "ricordami","ricorda","promemoria","reminder","remind","me","mi",
        "crea","creare","crei","aggiungi","segna","nuovo","nuova","add","new","set","metti",
        "schedule","pianifica","programma","fissa","prenota",                                      // scheduling verbs
        "evento","eventi","appuntamento","appuntamenti","impegno","event","appointment",
        "calendario","calendar","agenda","al","allo","alla","ai","nel","nella","negli","for",      // "aggiungi AL CALENDARIO X" -> X
        "non","farmi","fammi","dimenticare","dimenticarmi","forget","dont","let","scordare",        // "non farmi dimenticare X" -> X
        "chiamato","chiamata","intitolato","intitolata","titolo","dal","called","titled","named",   // "evento chiamato X" -> X
        "di","che","to","that","un","uno","una","il","lo","la","the","a","per","of", NULL };
    for (int i = 0; i < n; i++) {
        bool isLead = drop[i];
        for (int j = 0; !isLead && lead[j]; j++) if (!strcmp(low[i], lead[j])) isLead = true;
        if (isLead) drop[i] = true; else break;       // stop at the first real content word
    }

    // TEXT = the surviving words, in order (from the original raw, to keep their case).
    char text[AG_CONTENT_MAX]; int tl = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i]) continue;
        const char *s = start[i]; int len = 0; while (s[len] && s[len] != ' ' && s[len] != '\t') len++;
        if (tl && tl < (int)sizeof(text) - 1) text[tl++] = ' ';
        for (int k = 0; k < len && tl < (int)sizeof(text) - 1; k++) text[tl++] = s[k];
    }
    text[tl] = 0;
    if (!text[0]) {                                   // a timed reminder with no body ("promemoria per venerdì
        if (off > 0 || hh >= 0) snprintf(text, sizeof text, en ? "reminder" : "promemoria");  // alle 18") -> generic text
        else return 0;                                // truly nothing to schedule
    }

    // Pack the structured event on the content channel; the executor resolves off->date via the RTC.
    if (hh >= 0) snprintf(s_tool_content, sizeof(s_tool_content), "off=%d;time=%02d:%02d;text=%s", off, hh, mm, text);
    else         snprintf(s_tool_content, sizeof(s_tool_content), "off=%d;time=;text=%s", off, text);

    const char *when = off == 0 ? (en ? "today" : "oggi") : off == 1 ? (en ? "tomorrow" : "domani")
                     : off == 2 ? (en ? "in 2 days" : "dopodomani") : NULL;
    char whenbuf[24]; if (!when) { snprintf(whenbuf, sizeof(whenbuf), en ? "in %d days" : "tra %d giorni", off); when = whenbuf; }
    char timebuf[16] = ""; if (hh >= 0) snprintf(timebuf, sizeof(timebuf), en ? " at %02d:%02d" : " alle %02d:%02d", hh, mm);

    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_TOOL; r->confidence = 86;
    snprintf(r->intent, sizeof(r->intent), "add_event");
    snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->arg, sizeof(r->arg), "add_event");
    snprintf(r->reply, sizeof(r->reply), en ? "Reminder \"%s\" %s%s." : "Promemoria \"%s\" %s%s.", text, when, timebuf);
    trace_step(en ? "plan: schedule" : "piano: pianifica");
    char st[40]; snprintf(st, sizeof(st), en ? "when=%s%s" : "quando=%s%s", when, timebuf); trace_step(st);
    trace_step(en ? "verify: ok" : "verifica: ok");
    return 1;
}

// TEACH (offline durable learning): "ricorda che X è Y" / "remember that X is Y" stores a user fact that
// becomes recallable by paraphrase, fully offline (nucleo_anima_learn.c). The frame is TIGHT — an explicit
// teach lead PLUS a binding copula ("è"/"is"/"significa"/"means") — so a reminder ("ricordami DI comprare
// latte", no copula) falls through to tool_event, and a bare statement never triggers a write. The copula
// is matched on the RAW "è" (0xC3 0xA8), distinct from "e" (and), so "Marco e Luigi" isn't split as a fact.
static int tool_teach(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    char lo[256]; size_t z = 0;                       // lowercase ASCII, leave UTF-8 bytes (è) intact:
    for (; raw[z] && z + 1 < sizeof lo; z++) lo[z] = (char)tolower((unsigned char)raw[z]);
    lo[z] = 0;                                         // lo[] is byte-aligned with raw[] (tolower keeps length)

    static const char *const LEAD[] = {
        "ricorda che ", "ricordati che ", "impara che ", "imparare che ", "memorizza che ", "ricordare che ",
        "tieni a mente che ", "annota che ", "segnati che ", "sappi che ",
        "remember that ", "teach anima that ", "teach you that ", "teach that ", "learn that ",
        "note that ", "keep in mind that ", NULL };
    const char *rem = NULL;
    for (int i = 0; LEAD[i] && !rem; i++) { const char *p = strstr(lo, LEAD[i]); if (p) rem = p + strlen(LEAD[i]); }
    if (!rem) return 0;                                // no explicit teach frame -> not ours

    // Split subject | fact on the FIRST binding copula in the remainder. "è" via its UTF-8 bytes.
    static const char *const COP[] = { " \xC3\xA8 ", " sono ", " significa ", " vuol dire ", " is ", " are ", " means ", NULL };
    const char *cop = NULL; size_t clen = 0;
    for (int i = 0; COP[i]; i++) {
        const char *p = strstr(rem, COP[i]);
        if (p && (!cop || p < cop)) { cop = p; clen = strlen(COP[i]); }
    }
    if (!cop) return 0;                                // a teach lead with no copula -> abstain, don't guess

    // Extract subject/fact from the ORIGINAL raw at the byte-aligned offsets (preserves case + accents).
    size_t roff = (size_t)(rem - lo), coff = (size_t)(cop - lo);
    char subject[160], fact[360];
    size_t sl = coff - roff;       if (sl >= sizeof subject) sl = sizeof subject - 1;
    memcpy(subject, raw + roff, sl); subject[sl] = 0;
    snprintf(fact, sizeof fact, "%s", raw + coff + clen);
    { size_t a = 0; while (subject[a] == ' ') a++; if (a) memmove(subject, subject + a, strlen(subject + a) + 1);
      for (size_t b = strlen(subject); b && subject[b-1] == ' '; ) subject[--b] = 0; }

    int rc = nucleo_anima_learn_put(subject, fact, en);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->intent, sizeof r->intent, "teach");
    snprintf(r->state, sizeof r->state, "tool");
    if (rc == 1)
        snprintf(r->reply, sizeof r->reply, en ? "Got it — I'll remember: %s" : "Imparato — ricorderò: %s", fact);
    else if (rc == -1)
        snprintf(r->reply, sizeof r->reply, en ? "I won't store that — it's temporary information."
                                              : "Non lo memorizzo: è un'informazione temporanea.");
    else
        snprintf(r->reply, sizeof r->reply, en ? "I can't learn that right now." : "Non riesco a impararlo ora.");
    return 1;
}

// Thin adapter: the typed personal-profile tier ("mi chiamo X" / "come mi chiamo") reads only the RAW input.
static int tool_profile(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    return nucleo_anima_profile(raw, en, r);
}

// Translation tier with the RIGHT precedence: ONLINE-FIRST. In hybrid/online mode (online enabled +
// Wi-Fi) a translation request goes to the teacher LLM (Grok) — full sentences, real quality. The offline
// dictionary is the FLOOR, used only when offline, or if the teacher call fails (no key / heap / network).
// "The base translator is offline-only." A non-translation query never enters here (is_request is 0-FP).
static int tool_translate(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)tok; (void)ntok;
    if (nucleo_anima_translate_is_request(raw) && nucleo_anima_online_available()) {
        if (nucleo_anima_online_chat(raw, NULL, NULL, en, r)) return 1;   // Grok translated it
        // teacher unavailable/failed -> fall through to the grounded offline dictionary
    }
    return nucleo_anima_translate(raw, en, r);   // offline dictionary
}

// Image-generation request? Requires a GENERATION VERB *and* an IMAGE NOUN together, so it never steals the
// bare "disegna"/"draw" Paint-launch alias (verb only) and defers a note/file/event that merely mentions a
// photo. A question form ("come genero un'immagine?") falls through to knowledge. Detection only: image
// synthesis runs in Paint's Atelier on the browser GPU; ANIMA declines here and points there, so the
// on-device assistant never pretends it produced an image (zero hallucination by construction).
static bool a_is_image_gen(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok)
{
    static const char *const verb[] = { "genera","generare","generami","generarmi","crea","creare","creami","crearmi",
        "disegna","disegnami","disegnarmi","disegnare","dipingi","dipingimi","dipingermi","dipingere","produci",
        "raffigura","raffigurami","raffigurarmi",
        "generate","create","draw","paint","render","make","produce", NULL };
    static const char *const noun[] = { "immagine","immagini","foto","fotografia","fotografie","disegno","disegni",
        "illustrazione","illustrazioni","ritratto","ritratti","paesaggio","quadro","dipinto","figura","icona","logo",
        "image","images","picture","pictures","photo","photos","photograph","drawing","drawings","illustration",
        "portrait","artwork","painting","landscape", NULL };
    // Defer to the dedicated tools when the object is really a note / file / event, not an image.
    static const char *const other[] = { "nota","note","file","documento","document","promemoria","reminder",
        "evento","event","appuntamento","cartella","folder", NULL };
    bool v = false, n = false, o = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; verb[i];  i++) if (a_match(verb[i],  tok[t])) v = true;
        for (int i = 0; noun[i];  i++) if (a_match(noun[i],  tok[t])) n = true;
        for (int i = 0; other[i]; i++) if (a_match(other[i], tok[t])) o = true;
    }
    if (o) return false;                       // "crea una nota con una foto" -> the note tool owns it
    // Reject a how-to / definition / WHO / HOW-MANY question ("come genero un'immagine?", "cosa disegno?",
    // "chi ha disegnato il logo Nike", "posso ...?") but ALLOW a polite second-person REQUEST ("puoi
    // generarmi ...", "potresti disegnarmi ...", "can you make me an image") — those are genuine generation
    // asks to redirect, not how-tos. Exact match (like a_has_qword) so an inflected non-question word can't
    // accidentally veto a real request. ("che" is NOT here: it is the relative "voglio CHE tu generi ...".)
    static const char *const howto[] = { "come","how","cosa","what","perche","why","quando","when","dove",
        "where","quale","which","chi","who","quanti","quanto","quante","significa","significato","spiega",
        "spiegami","definizione","define","meaning","posso", NULL };
    for (int t = 0; t < ntok; t++) for (int i = 0; howto[i]; i++) if (!strcmp(howto[i], tok[t])) return false;
    // Direct REQUEST forms — "disegnami/dipingimi/raffigurami X" and "draw/paint/sketch me X" — are image-gen
    // even without the literal word "immagine": the -mi / "verb me" form is always a request TO the assistant,
    // never a knowledge statement or an app-open ("apri disegnami" / "chi disegnami" don't occur). This is the
    // safe way to catch "disegnami un gatto" / "draw me a dragon" without the false positives a bare
    // verb+object rule would create on "chi ha disegnato ..." or "crea una password".
    // "-mi" request forms + the inherently-pictorial verbs dipingi/dipingere/raffigura (you never "dipingi una
    // password" or "raffigura una conclusione") → image-gen with just an object, no need for the word "immagine".
    // (Bare "disegna"/"crea" are NOT here: "disegna la tua conclusione" / "crea un account" must stay safe.)
    static const char *const reqv[] = { "disegnami","disegnarmi","dipingimi","dipingermi","dipingi","dipingere",
        "raffigurami","raffigurarmi","raffigura", NULL };
    bool req = false, me = false, dps = false;
    for (int t = 0; t < ntok; t++) {
        for (int i = 0; reqv[i]; i++) if (!strcmp(reqv[i], tok[t])) req = true;
        if (!strcmp(tok[t], "me")) me = true;
        if (a_match("draw", tok[t]) || a_match("paint", tok[t]) || a_match("sketch", tok[t])) dps = true;
    }
    if (req && ntok >= 2) return true;
    if (dps && me && ntok >= 3) return true;
    return v && n;
}

// Tool: image-generation gate. Grounded decline + redirect to Paint's Atelier (text only, never a side
// effect: the web app turns this into an "Open Paint" button, the native app shows the text). Runs FIRST in
// TOOLS[] so "genera/disegna un'immagine di X" can never be mis-scored as open_app(paint) by the intent table.
static int tool_image_gen(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    (void)raw;
    if (!a_is_image_gen(tok, ntok)) return 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
    snprintf(r->intent, sizeof(r->intent), "image_gen");
    snprintf(r->state,  sizeof(r->state),  "idle");
    snprintf(r->reply,  sizeof(r->reply), en
        ? "Images are generated in the Atelier studio inside the Paint app - it runs on your browser's GPU, not here in chat. Open Paint, type what you want (or sketch it), press Generate, and the picture is saved to your Cardputer."
        : "Le immagini si generano nello studio Atelier dell'app Paint: gira sulla GPU del browser, non qui in chat. Apri Paint, scrivi cosa vuoi (o fai uno schizzo), premi Genera e l'immagine viene salvata sul Cardputer.");
    return 1;
}


// Order matters: IMAGE-GEN first (a grounded decline + redirect, so "genera/disegna un'immagine di X" can't be
// mis-scored as open_app(paint) by the intent table below), then PROFILE (first-person self-facts
// "mi chiamo X"/"come mi chiamo", deterministic), then
// TEACH (its tight frame ignores everything that isn't an explicit "ricorda che X è Y"), then schedule
// (reminder/event) and compose-then-act note (they read the RAW input), then the plain empty-file create,
// the device-settings tool, the offline IT<->EN translator, and finally the unified math agent.
static const a_tool_t TOOLS[] = {
    { "image_gen",      false, tool_image_gen },
    { "profile",        true,  tool_profile },
    { "teach",          true,  tool_teach },
    { "add_event",      true,  tool_event },
    { "create_file",    true,  tool_note },
    { "create_file",    true,  tool_create_file },
    { "set_brightness", true,  tool_setting },
    { "translate",      false, tool_translate },
    { "math",           false, tool_math },
};

static int tools_dispatch(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    for (size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); i++)
        if (TOOLS[i].try_fn(raw, tok, ntok, en, r)) return 1;
    return 0;
}

// FSM AWAITING_SLOT: a previous turn asked for a missing argument. Try to satisfy it from
// this input. Returns 1 if consumed. Bails out (clearing the slot) if the input is clearly
// a different command, so the user is never trapped in a half-finished tool call.
static int try_pending_slot(const char *raw, char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, bool en, anima_result_t *r)
{
    if (!s_session.pending_tool[0]) return 0;
    if (strcmp(s_session.pending_tool, "create_file") == 0) {
        if (a_is_followup_open(tok, ntok)) { s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; return 0; }

        // AWAITING_SLOT(folder): the filename is stashed; the user is telling us where to put it.
        if (strcmp(s_session.pending_slot, "folder") == 0) {
            const char *folder = a_folder_from_words(tok, ntok);
            if (folder) {
                char name[48]; snprintf(name, sizeof(name), "%s", s_session.pending_arg);
                s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; s_session.pending_arg[0] = 0;
                a_emit_create(name, folder, en, r);
                return 1;
            }
            s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0; s_session.pending_arg[0] = 0;  // bail
            return 0;
        }

        // AWAITING_SLOT(filename): read a name, then route it (which may in turn ask for a folder).
        char name[40];
        bool got = a_extract_filename(raw, name, sizeof(name));
        if (!got && ntok == 1) {                       // a single bare word -> use it as the name
            int o = 0;
            for (const char *q = tok[0]; *q && o < (int)sizeof(name) - 5; q++)
                if (isalnum((unsigned char)*q) || *q == '_' || *q == '-') name[o++] = *q;
            name[o] = 0;
            if (o > 0) { if (!strchr(name, '.')) memcpy(name + o, ".txt", 5); got = true; }
        }
        if (got) {
            s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;
            a_emit_create(name, NULL, en, r);
            return 1;
        }
        s_session.pending_tool[0] = 0; s_session.pending_slot[0] = 0;   // nothing usable -> drop
    }
    return 0;
}

// Collect up to `cap` DISTINCT app ids named in the query (for the open_app clarify path).
static int a_resolve_apps(char tok[A_MAX_TOKENS][A_TOK_LEN], int ntok, const char *out[], int cap)
{
    int n = 0;
    for (size_t i = 0; i < sizeof(APP_ALIAS) / sizeof(APP_ALIAS[0]) && n < cap; i++) {
        bool hit = false;
        for (int j = 0; j < A_MAX_ALIAS && APP_ALIAS[i].alias[j] && !hit; j++)
            for (int t = 0; t < ntok; t++)
                if (a_match(APP_ALIAS[i].alias[j], tok[t])) { hit = true; break; }
        if (hit) {
            bool dup = false; for (int k = 0; k < n; k++) if (out[k] == APP_ALIAS[i].id) dup = true;
            if (!dup) out[n++] = APP_ALIAS[i].id;
        }
    }
    return n;
}

// ---- public API -------------------------------------------------------------

static bool s_ready;

esp_err_t nucleo_anima_init(const char *lang)
{
    // Phase 0: only the embedded Italian command table. Future: load /sd/data/anima/
    // commands.<lang>.json to override/extend, and the L1 pack for retrieval.
    if (lang && strcmp(lang, "it") != 0)
        ESP_LOGW(TAG, "lang '%s' not bundled yet; using 'it'", lang);
    s_ready = true;
    ESP_LOGI(TAG, "L0 ready (%d intents)", (int)(sizeof(INTENTS) / sizeof(INTENTS[0])));
    nucleo_anima_l1_init();    // best-effort: semantic tier if the SD packs are present
    session_load();            // restore conversational context from a previous boot (best-effort)
    units_load();              // restore user-defined units learned in a previous session
    return ESP_OK;
}

// L0: cheap keyword/intent tier (returns tier NONE if not confident). `en` picks the reply
// language; ANIMA understands both regardless (the keyword tables hold IT+EN triggers).
static anima_result_t l0_query(const char *input, bool en)
{
    anima_result_t r = { 0 };
    r.tier = ANIMA_TIER_NONE;
    r.action = ANIMA_ACT_NONE;
    snprintf(r.state, sizeof(r.state), "idle");
    if (!s_ready || !input) return r;

    char tok[A_MAX_TOKENS][A_TOK_LEN];
    int ntok = a_tokenize(input, tok);
    if (ntok == 0) return r;

    // CLOSE/EXIT a named app — must NEVER be confused with OPEN. Without this, "chiudi il calendario"
    // matched the launch card and OPENED the very app (inverse action). Emit a close action; if the
    // executor lacks close it is an inert no-op — never the opposite open. Skipped on a how-to question.
    {
        static const char *const closev[] = { "chiudi","chiudere","esci","uscire","termina","terminare",
                                               "spegni","ferma","stoppa","close","exit","quit","stop", NULL };
        static const char *const setw[]   = { "volume","audio","suono","luminosita","schermo","luce","brightness", NULL };
        bool wantclose = false, isq = false, is_set = false;
        for (int t = 0; t < ntok; t++) {
            if (a_qword(tok[t])) isq = true;
            for (int i = 0; closev[i]; i++) if (a_match(closev[i], tok[t])) wantclose = true;
            for (int i = 0; setw[i];   i++) if (a_match(setw[i],   tok[t])) is_set = true;   // "spegni l'audio" -> mute, not close
        }
        if (wantclose && !isq && !is_set) {
            const char *app = a_resolve_app(tok, ntok);
            if (app) {
                r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 88;
                snprintf(r.intent, sizeof r.intent, "close_app");
                snprintf(r.arg, sizeof r.arg, "%s", app);
                snprintf(r.state, sizeof r.state, "tool");
                snprintf(r.reply, sizeof r.reply, en ? "Closing %s." : "Chiudo %s.", app);
                return r;
            }
        }
    }

    // FSM AWAITING_SLOT: finish a tool call whose argument we asked for last turn.
    if (try_pending_slot(input, tok, ntok, en, &r)) return r;

    // Tool registry (function calling): create_file, calc, ... Checked before intents because
    // tools read the RAW input (filenames, expressions) the normalizer would otherwise mangle.
    if (tools_dispatch(input, tok, ntok, en, &r)) return r;

    // Computed-from-state, executor-filled: today's calendar agenda, and the dynamic capabilities
    // answer. Checked before the intent table so "cosa devo fare oggi" beats the date intent ("oggi").
    if (a_is_agenda(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 85;
        snprintf(r.intent, sizeof(r.intent), "agenda");
        snprintf(r.arg, sizeof(r.arg), "agenda");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_capabilities(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 80;
        snprintf(r.intent, sizeof(r.intent), "capabilities");
        snprintf(r.arg, sizeof(r.arg), "capabilities");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_network(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 82;
        snprintf(r.intent, sizeof(r.intent), "network");
        snprintf(r.arg, sizeof(r.arg), "network");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }
    if (a_is_ram(tok, ntok)) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_SYSTEM; r.confidence = 82;
        snprintf(r.intent, sizeof(r.intent), "ram");
        snprintf(r.arg, sizeof(r.arg), "ram");
        snprintf(r.reply, sizeof(r.reply), "{value}");
        snprintf(r.state, sizeof(r.state), "tool");
        return r;
    }

    // Follow-up: "aprilo" / "open it" -> reopen the most recent file/app from utility memory.
    // "chiudilo" / "close it" -> close the last-opened app (the close mirror of the "aprilo" follow-up).
    if (a_is_followup_close(tok, ntok) && s_mem.last_app[0]) {
        r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 82; r.from_memory = 1;
        snprintf(r.intent, sizeof(r.intent), "close_app");
        snprintf(r.state, sizeof(r.state), "followup");
        snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_app);
        snprintf(r.reply, sizeof(r.reply), en ? "Closing %s." : "Chiudo %s.", s_mem.last_app);
        return r;
    }

    if (a_is_followup_open(tok, ntok)) {
        bool have_app = s_mem.last_app[0], have_file = s_mem.last_file[0];
        bool use_file = have_file && (s_mem.last_kind == 'f' || !have_app);   // recency, else fall back
        if (use_file) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_TOOL; r.confidence = 80; r.from_memory = 1;
            snprintf(r.intent, sizeof(r.intent), "open_file");
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_file);
            const char *bn = strrchr(s_mem.last_file, '/'); bn = bn ? bn + 1 : s_mem.last_file;
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", bn);
            return r;
        }
        if (have_app) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_LAUNCH; r.confidence = 80; r.from_memory = 1;
            snprintf(r.intent, sizeof(r.intent), "open_app");
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.arg, sizeof(r.arg), "%s", s_mem.last_app);
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", s_mem.last_app);
            return r;
        }
        // nothing remembered yet -> fall through to normal handling
    }

    // FSM CLARIFY resolution: if last turn we offered two apps, an ordinal ("il primo") or the
    // app's own name now picks one — without re-asking. The signal we'd normally throw away.
    if (s_session.clarify_opt[0][0]) {
        int pick = -1;
        for (int t = 0; t < ntok; t++) {
            if (!strcmp(tok[t], "primo") || !strcmp(tok[t], "prima") || !strcmp(tok[t], "first") ||
                !strcmp(tok[t], "uno")   || !strcmp(tok[t], "one"))   pick = 0;
            if (!strcmp(tok[t], "secondo") || !strcmp(tok[t], "seconda") || !strcmp(tok[t], "second") ||
                !strcmp(tok[t], "due")     || !strcmp(tok[t], "two"))     pick = 1;
        }
        if (pick < 0) {
            const char *a = a_resolve_app(tok, ntok);
            for (int i = 0; i < 2; i++) if (a && !strcmp(a, s_session.clarify_opt[i])) pick = i;
        }
        if (pick >= 0) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_LAUNCH; r.confidence = 85; r.from_memory = 1;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.intent, sizeof(r.intent), "open_app");
            snprintf(r.arg, sizeof(r.arg), "%s", s_session.clarify_opt[pick]);
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", r.arg);
            s_session.clarify_opt[0][0] = 0; s_session.clarify_opt[1][0] = 0;
            return r;
        }
        s_session.clarify_opt[0][0] = 0; s_session.clarify_opt[1][0] = 0;   // not a resolution -> drop
    }

    // Pick the best and runner-up intents.
    const a_intent_t *best = NULL;
    int best_score = 0, second_score = 0;
    for (size_t i = 0; i < sizeof(INTENTS) / sizeof(INTENTS[0]); i++) {
        int s = a_score(&INTENTS[i], tok, ntok);
        // whoami keys on bare "chi"/"who" -> guard it to genuine self-questions so "chi è <entity>"
        // falls through to the knowledge/online tier instead of replying "Sono ANIMA".
        if (s > 0 && !strcmp(INTENTS[i].id, "whoami") && !a_whoami_self(tok, ntok)) s = 0;
        // Ambient state intents must be framed, else an incidental "oggi"/"time"/"year"/"giorno"
        // answers the date/clock/year from device state ("favorite band of all time" -> the time).
        if (s > 0 && !l0_legacy() &&
            (!strcmp(INTENTS[i].id,"date") || !strcmp(INTENTS[i].id,"time") ||
             !strcmp(INTENTS[i].id,"year") || !strcmp(INTENTS[i].id,"season") ||
             !strcmp(INTENTS[i].id,"battery")) &&
            !a_ambient_ok(&INTENTS[i], tok, ntok, s)) s = 0;
        // storage has its own guard: "spazio" is ambiguous (disk vs physical space) -> see a_storage_ok.
        if (s > 0 && !l0_legacy() && !strcmp(INTENTS[i].id,"storage") && !a_storage_ok(tok, ntok)) s = 0;
        // version must be about the DEVICE firmware, not "la versione di python" (knowledge/unknowable).
        if (s > 0 && !l0_legacy() && !strcmp(INTENTS[i].id,"version") && !a_version_ok(tok, ntok)) s = 0;
        // uptime keys must hit EXACTLY (fuzzy "ultimo"~"uptime" answered "Acceso da …").
        if (s > 0 && !strcmp(INTENTS[i].id,"uptime") && !a_uptime_ok(tok, ntok)) s = 0;
        if (s > best_score) { second_score = best_score; best_score = s; best = &INTENTS[i]; }
        else if (s > second_score) { second_score = s; }
    }
    if (!best || best_score == 0) return r;

    // Confidence: grows with matched keywords, penalized when a runner-up ties (ambiguous).
    int conf = 45 + 20 * best_score;
    if (best_score == second_score) conf -= 25;
    if (conf > 100) conf = 100;

    // open_app needs a resolvable app, else it isn't really a command. When the query names
    // TWO different apps it's genuinely ambiguous -> ask instead of guessing (uncertainty
    // policy). The two candidates are remembered so an ordinal/name resolves it next turn.
    const char *app = NULL;
    if (best->action == ANIMA_ACT_LAUNCH && best->arg == NULL) {
        const char *apps[2]; int na = a_resolve_apps(tok, ntok, apps, 2);
        if (na == 0) return r;              // "apri" with no known app -> let higher tiers try
        if (na >= 2) {
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.awaiting = 1; r.confidence = 60;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.intent, sizeof(r.intent), "clarify");
            snprintf(r.reply, sizeof(r.reply), en ? "Which one — %s or %s?" : "Quale dei due apro, %s o %s?", apps[0], apps[1]);
            snprintf(s_session.clarify_opt[0], sizeof(s_session.clarify_opt[0]), "%s", apps[0]);
            snprintf(s_session.clarify_opt[1], sizeof(s_session.clarify_opt[1]), "%s", apps[1]);
            return r;
        }
        if (a_desire_only_launch(tok, ntok)) return r;   // "vorrei sapere perché…" → knowledge, not a launch
        if (a_question_not_launch(tok, ntok)) return r;  // "what team did X play for" → not a launch
        if (a_launch_is_degenerate(tok, ntok)) return r; // bare "play"/"player" → ambiguous noun, not "open X"
        app = apps[0];
        conf += 10;
        if (conf > 100) conf = 100;
    }

    if (conf < 55) return r;                 // gate: not confident enough for L0

    // Build the result.
    r.tier   = ANIMA_TIER_COMMAND;
    r.action = best->action;
    r.confidence = conf;
    snprintf(r.intent, sizeof(r.intent), "%s", best->id);

    const char *rep = en ? (best->reply_en ? best->reply_en : best->reply_it) : best->reply_it;
    switch (best->action) {
        case ANIMA_ACT_LAUNCH:
            snprintf(r.arg, sizeof(r.arg), "%s", app ? app : (best->arg ? best->arg : ""));
            snprintf(r.reply, sizeof(r.reply), en ? "Opening %s." : "Apro %s.", r.arg);
            break;
        case ANIMA_ACT_SYSTEM:
            snprintf(r.arg, sizeof(r.arg), "%s", best->arg ? best->arg : "");
            snprintf(r.reply, sizeof(r.reply), "%s", rep ? rep : "");
            break;
        case ANIMA_ACT_ANSWER:
            snprintf(r.reply, sizeof(r.reply), "%s", rep ? rep : "");
            break;
        default: break;
    }
    return r;
}

// Bilingual typo tolerance (spellfix) + foreign-script output cleanup live in anima_text.c
// (pure text utilities, extracted for scalability). Declared in anima_internal.h.


// Strip a conversational knowledge lead-in to the bare TOPIC ("cosa sai di X"->"X", "do you know
// X"->"X") so L1 matches a card's title / "cos'è X" asks regardless of how the user opened the
// question. ONE rule for every card, current and future — scalable, no per-card phrasing bloat.
// Defined after a_norm_phrase (below); forward-declared here. 0=none, 1=formal (L0-first), 2=conversational (L1-first).
void a_norm_phrase(const char *raw, char *out, size_t cap);
static int a_topic_strip(const char *q, char *out, size_t outsz);

// Conversational KNOWLEDGE->SKILL bridge: after telling the user what a topic IS, proactively offer the
// matching skill ANIMA can DO with it + invite a follow-up. Turns retrieval into a dialogue ("so cos'è la
// fisica E so calcolartela"). Curated map (skills are few & known) -> scalable, no per-card data. Appends
// to the knowledge reply only when it fits and isn't already present.
// Shared KNOWLEDGE<->SKILL map: a topic whose concept ANIMA both KNOWS (a card) and can DO (a solver).
// `bare` = the keyword the user types as a standalone topic ("fisica") -> used for ambiguity detection.
// `offer` = appended after a knowledge answer. `ask` = the clarify question when the topic is bare/ambiguous.
typedef struct { const char *bare[5]; const char *kw[8]; const char *offer_it, *offer_en, *ask_it, *ask_en; } a_skill_t;
static const a_skill_t SKILLS[] = {
    {{"fisica","fisico","physics",NULL},{"fisica","fisico","physics","cinematica","dinamica",NULL},
     " Inoltre ho una skill di fisica: calcolo forza, energia, velocità e accelerazione — dammi i dati.",
     " I also have a physics skill: I compute force, energy, speed and acceleration — give me the values.",
     "Posso dirti cos'è la fisica, oppure risolverti un problema (forza, energia, velocità). Quale ti serve?",
     "I can tell you what physics is, or solve a physics problem (force, energy, speed). Which do you need?"},
    {{"matematica","mathematics","aritmetica",NULL},{"matematica","mathematics","aritmetica","equazione","equazioni",NULL},
     " Inoltre posso fare calcoli e risolvere equazioni: scrivimi l'operazione.",
     " I can also do calculations and solve equations: just type the expression.",
     "Vuoi sapere cos'è la matematica, o devo fare un calcolo? Scrivimi pure l'operazione.",
     "Do you want to know what mathematics is, or should I compute something? Just type the expression."},
    {{"geometria","geometry",NULL},{"geometria","geometry","poligono","triangolo",NULL},
     " Inoltre ho una skill di geometria: aree, perimetri e volumi — dammi le misure.",
     " I also have a geometry skill: areas, perimeters and volumes — give me the measures.",
     "Posso spiegarti cos'è la geometria, oppure calcolare aree/perimetri/volumi. Cosa preferisci?",
     "I can explain what geometry is, or compute areas/perimeters/volumes. Which would you like?"},
    {{"vettore","vettori","vector","vectors"},{"vettore","vettori","vector","vectors",NULL},
     " Inoltre calcolo somma, modulo e prodotto di vettori — dammi le componenti.",
     " I can also compute vector sum, magnitude and product — give me the components.",
     "Vuoi la definizione di vettore, o devo calcolare (somma, modulo, prodotto)? Dimmi pure.",
     "Do you want the definition of vector, or should I compute (sum, magnitude, product)? Tell me."},
    {{"elettronica","electronics",NULL},{"elettronica","electronics","resistenza","tensione","corrente","ohm",NULL},
     " Inoltre applico la legge di Ohm (V=R·I): dammi due valori e ricavo il terzo.",
     " I also apply Ohm's law (V=R·I): give me two values and I'll find the third.",
     "Posso dirti cos'è l'elettronica, oppure applicare la legge di Ohm (V=R·I). Quale ti serve?",
     "I can tell you what electronics is, or apply Ohm's law (V=R·I). Which do you need?"},
    {{"statistica","statistics",NULL},{"statistica","statistics","statistico",NULL},
     " Inoltre calcolo media, mediana e deviazione standard — dammi i numeri.",
     " I also compute mean, median and standard deviation — give me the numbers.",
     "Vuoi sapere cos'è la statistica, o devo calcolare (media, mediana, deviazione)? Dammi i numeri.",
     "Do you want to know what statistics is, or should I compute (mean, median, deviation)? Give me the numbers."},
    {{"meteorologia",NULL},{"meteorologia","meteo","weather","clima",NULL},
     " Inoltre ti do il meteo attuale di una città — dimmi quale.",
     " I also give you the current weather for a city — tell me which.", NULL, NULL},   // no clarify: bare "meteo" -> wants weather
};

// After a knowledge answer, offer the matching skill + invite a follow-up. Turns retrieval into dialogue
// ("so cos'è la fisica E so calcolartela"). Appends only when it fits (clips a long fact to make room).
static void a_offer_skill(anima_result_t *r, const char *topic, bool en)
{
    if (!r || !r->reply[0]) return;
    char nq[160]; a_norm_phrase(topic, nq, sizeof nq);
    for (size_t i = 0; i < sizeof(SKILLS)/sizeof(SKILLS[0]); i++) {
        bool hit = false;
        for (int k = 0; SKILLS[i].kw[k]; k++) { char pat[24]; snprintf(pat, sizeof pat, " %s ", SKILLS[i].kw[k]); if (strstr(nq, pat)) { hit = true; break; } }
        if (!hit) continue;
        const char *add = en ? SKILLS[i].offer_en : SKILLS[i].offer_it;
        if (strstr(r->reply, en ? "I also" : "Inoltre")) return;   // already offered
        size_t need = strlen(add), cap = sizeof(r->reply) - 1;
        if (need + 8 >= cap) return;
        size_t room = cap - need;                                  // keep room for the offer; clip the long fact
        if (strlen(r->reply) > room) { size_t cut = room; while (cut > 16 && r->reply[cut] != ' ' && r->reply[cut] != '.') cut--; r->reply[cut] = 0; }
        memcpy(r->reply + strlen(r->reply), add, need + 1);
        return;
    }
}

// Detect a PALPABLE knowledge<->skill ambiguity: the query is essentially just a bare topic that ANIMA
// both knows AND can compute, with no disambiguating cue (no opener -> handled by caller via kind==0; no
// digits/compute -> those go straight to the solver). Returns the SKILLS index (and the clarify question)
// or -1. Narrow on purpose: only fires on the bare word, so "storia della fisica" stays knowledge.
static int a_skill_clarify(const char *q, bool en, char *topic_out, size_t tcap, const char **question)
{
    char nq[120]; a_norm_phrase(q, nq, sizeof nq);
    for (const char *p = q; *p; p++) if (isdigit((unsigned char)*p)) return -1;   // has data -> it's a compute, not ambiguous
    const char *body = nq; while (*body == ' ') body++;                            // strip ONE leading article
    static const char *const art[] = { "il ","lo ","la ","l ","un ","uno ","una ","the ","a ","an ", NULL };
    for (int i = 0; art[i]; i++) { size_t L = strlen(art[i]); if (strncmp(body, art[i], L) == 0) { body += L; break; } }
    char bare[64]; snprintf(bare, sizeof bare, "%s", body);
    size_t bl = strlen(bare); while (bl && bare[bl-1] == ' ') bare[--bl] = 0;      // trim
    for (size_t i = 0; i < sizeof(SKILLS)/sizeof(SKILLS[0]); i++) {
        if (!(en ? SKILLS[i].ask_en : SKILLS[i].ask_it)) continue;                 // skill opts out of clarify
        for (int k = 0; SKILLS[i].bare[k]; k++)
            if (!strcmp(bare, SKILLS[i].bare[k])) {
                snprintf(topic_out, tcap, "%s", SKILLS[i].bare[0]);
                *question = en ? SKILLS[i].ask_en : SKILLS[i].ask_it;
                return (int)i;
            }
    }
    return -1;
}


static int a_norm_ntok(const char *norm);                                   // fwd (defined below)
static bool a_has_phrase(const char *norm, const char *const *phrases);      // fwd (defined below)

// Is `q` a context-needing follow-up FRAGMENT — a short question with no subject of its own ("e cosa
// ha fatto?", "e quando è morto?", "perché?", "e lui?")? Used (MISS-gated) to retry with the last topic
// injected = lightweight coreference without NER.
static bool a_is_followup_q(const char *q)
{
    char nq[120]; a_norm_phrase(q, nq, sizeof nq);
    int nt = a_norm_ntok(nq);
    if (nt < 1 || nt > 7) return false;                    // a fragment, not a whole self-contained question
    // A follow-up has a question cue AND no subject of its own: EVERY token must be a function/cue word
    // (no content noun). So "e cosa ha fatto" -> yes; "cos'è quasar" (has the subject "quasar") -> no.
    static const char *const cue[] = { "cosa","cos","cose","quando","dove","perche","come","chi","quale",
        "quali","quanti","quanto","quanta","lui","lei","esso","essa","suo","sua","loro","ne",
        "what","when","where","why","who","which","how","he","she","it","his","her","they", NULL };
    if (!a_has_phrase(nq, cue)) return false;
    static const char *const fn[] = {  // function words allowed in a subject-less fragment
        "e","ed","ma","poi","allora","cosa","cos","cose","che","ha","hanno","ho","hai","fatto","fece","fa",
        "fanno","era","erano","stato","stata","quando","dove","perche","come","chi","quale","quali","quanti",
        "quanto","quanta","lui","lei","esso","essa","loro","suo","sua","il","lo","la","i","gli","le","di","del",
        "a","da","in","su","per","con","morto","morta","nato","nata","vissuto","serve","servono","significa",
        "significano","vuol","dire","mi","ne","si","cosi","oggi","ora","adesso",
        "what","whats","did","does","has","have","had","he","she","it","they","them","his","her","their","when",
        "where","why","who","which","how","is","are","was","were","do","the","a","an","of","to","about","mean",
        "means","born","die","died","made","make","for","then","and","so","now", NULL };
    char tok[A_MAX_TOKENS][A_TOK_LEN]; int n = a_tokenize(q, tok);
    for (int t = 0; t < n; t++) {
        bool isfn = false;
        for (int i = 0; fn[i] && !isfn; i++) if (!strcmp(fn[i], tok[t])) isfn = true;
        if (!isfn) return false;                           // a content word -> the query has its own subject
    }
    return true;
}

// Run the L0 -> L1 answer cascade on `q` (NO clarify band here — the band runs once, after the
// typo rescue, so a typo'd launch is corrected before a fuzzy L1 clarify can pre-empt it).
// Returns 1 if it produced an answer (fills *r and updates memory); 0 on a miss (*r = L0 NONE).
// ---- Conversational FOCUS shift (structured coreference for the deductive tier) ----------------
// The KGE reasoner DECLARES the (subject, relation) it anchored in r.subject / r.relation. We stash that
// as the conversational focus, so a bare follow-up can re-aim the SAME reasoner deterministically: swap
// the entity ("quando e nato einstein" -> "e newton?") keeping the relation, or swap the relation
// ("e dove si trova?") keeping the subject. NO text subtraction: we rebuild a FULL fact question from a
// relation template and re-run nucleo_anima_hdc_reason, so its lexical/role/coherence guards still apply
// and a wrong re-aim simply REFUSES (zero fabrication). Miss-gated: only after the normal cascade missed.
static const char *foc_template(const char *rel) {
    if (!rel || !rel[0]) return NULL;
    if (!strcmp(rel, "born"))       return "quando e nato %s";
    if (!strcmp(rel, "died"))       return "quando e morto %s";
    if (!strcmp(rel, "capital"))    return "capitale di %s";
    if (!strcmp(rel, "located_in")) return "dove si trova %s";
    if (!strcmp(rel, "author"))     return "chi ha scritto %s";
    return NULL;
}
static void foc_remember(const anima_result_t *r) {
    if (!r->subject[0] || !r->relation[0]) return;
    snprintf(s_session.foc_subject,  sizeof s_session.foc_subject,  "%s", r->subject);
    snprintf(s_session.foc_relation, sizeof s_session.foc_relation, "%s", r->relation);
    s_session.foc_turn = s_session.turn;
}

static int try_cascade(const char *q, bool en, anima_result_t *r)
{
    char topic[160];
    int kind = a_topic_strip(q, topic, sizeof topic);  // 0 none, 1 formal, 2 conversational

    // Helper outcome on a hit: offer the related skill, update memory, return 1.
    #define A_CASCADE_HIT(used) do { a_offer_skill(r, (used), en); \
        snprintf(r->state, sizeof(r->state), "idle"); mem_update(r); \
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", (used)); \
        s_session.dirty = true; return 1; } while (0)

    // CONVERSATIONAL lead-in ("cosa sai di X", "do you know X") is BY DEFINITION a knowledge question —
    // run L1 BEFORE L0 so a chatty opener can't be hijacked by a skill (e.g. "do you know…"->whoami).
    // Try the full phrase first (a card may carry it), then the bare topic. Lead-ins never name a command.
    if (kind == 2) {
        if (nucleo_anima_l1_query(q, en, false, r))     A_CASCADE_HIT(topic);
        if (nucleo_anima_l1_query(topic, en, false, r)) A_CASCADE_HIT(topic);
    }
    *r = l0_query(q, en);
    if (r->tier != ANIMA_TIER_NONE) { mem_update(r); return 1; }
    // PALPABLE knowledge<->skill ambiguity: a bare topic ANIMA both knows and can compute, with no cue
    // (no opener -> kind==0; no digits/compute verb -> handled inside a_skill_clarify). Ask instead of
    // guessing — exactly once. A clear request (opener, or numbers/"calcola") never reaches here.
    if (kind == 0) {
        char ct[40]; const char *cq = NULL;
        if (a_skill_clarify(q, en, ct, sizeof ct, &cq) >= 0) {
            memset(r, 0, sizeof *r);
            r->tier = ANIMA_TIER_FACT; r->action = ANIMA_ACT_ANSWER; r->confidence = 80; r->awaiting = 1;
            snprintf(r->intent, sizeof r->intent, "clarify");
            snprintf(r->state, sizeof r->state, "clarify");
            snprintf(r->reply, sizeof r->reply, "%s", cq);
            snprintf(s_session.skill_clarify, sizeof s_session.skill_clarify, "%s", ct);
            s_session.dirty = true;
            return 1;
        }
    }
    // L1: FULL query first (preserves exact "cos'è X" / "what is X" ask matches), then the stripped
    // topic as a fallback (handles "cos'è LA fisica" / openers the full phrase wouldn't hit).
    if (nucleo_anima_l1_query(q, en, false, r)) {
        if (kind && r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
        else A_CASCADE_HIT(kind ? topic : q);
    }
    if (kind && nucleo_anima_l1_query(topic, en, false, r)) {
        if (r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
        else A_CASCADE_HIT(topic);
    }
    // CANONICAL retry: cards are indexed on question-phrasings ("cos'è X" / "what is X"), so a bare topic
    // stripped from a chatty opener ("raccontami di suono" -> "suono") may not match while the canonical
    // form does. Reconstruct and retry (still gated -> no fabrication). Skip if topic already opens with the
    // cue, OR if the query NAMES a Proper Noun: the lowercased reconstruction would lose its case and bypass
    // the named-entity coverage guard (re-admitting "il linguaggio Floonk" -> the generic card).
    bool names_proper = false;
    { bool first = true;
      for (const char *p = q; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        if (!*p) break;
        char f = *p; int len = 0;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) { p++; len++; }
        if (!first && len >= 4 && f >= 'A' && f <= 'Z') { names_proper = true; break; }
        first = false;
      } }
    if (kind && !names_proper && topic[0] && strncmp(topic, en ? "what" : "cos", en ? 4 : 3) != 0) {
        char canon[180]; snprintf(canon, sizeof canon, en ? "what is %s" : "cos e %s", topic);
        if (nucleo_anima_l1_query(canon, en, false, r)) {
            if (r->action != ANIMA_ACT_ANSWER) memset(r, 0, sizeof *r);
            else A_CASCADE_HIT(topic);
        }
    }
    #undef A_CASCADE_HIT
    return 0;
}

// Online-only mode (ANIMA app "Online: Solo" setting): when set, the query skips the whole offline
// cascade and answers ONLY via the cloud teacher (Grok). Off by default; implies the network master
// switch is on (the app sets both).
static bool s_online_only;
void nucleo_anima_set_online_only(bool on) { s_online_only = on; }
bool nucleo_anima_online_only_enabled(void) { return s_online_only; }

// ============================================================================
// DIALOGUE ACTS — the conversational glue that makes ANIMA feel like a coherent
// agent across turns, not a one-shot Q&A box. These are meta-commands that act on
// the CONVERSATION STATE (working memory), NOT on the knowledge base — so they need
// no encoder, no card, no network: pure deterministic patterns over s_session, free
// of the retrieval gate's ceiling. The MCU-honest analog of an agent's turn-taking.
// ============================================================================

// Normalize `raw` to " token token … " (lowercase ASCII, accents folded, single spaces, sentinel
// spaces at both ends) so a multi-word phrase matches with a simple strstr(" phrase ").
void a_norm_phrase(const char *raw, char *out, size_t cap)
{
    int o = 0; if (cap < 2) { if (cap) out[0] = 0; return; }
    out[o++] = ' '; bool sp = true;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < (int)cap - 2; p++) {
        unsigned char c = *p; char ch = 0;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            if      (d>=0xA0&&d<=0xA2) ch='a'; else if (d>=0xA8&&d<=0xAA) ch='e';
            else if (d>=0xAC&&d<=0xAE) ch='i'; else if (d>=0xB2&&d<=0xB4) ch='o';
            else if (d>=0xB9&&d<=0xBB) ch='u';
        } else if (isalnum(c)) ch = (char)tolower(c);
        if (ch) { out[o++] = ch; sp = false; }
        else if (!sp) { out[o++] = ' '; sp = true; }
    }
    if (!sp && o < (int)cap - 1) out[o++] = ' ';
    out[o] = 0;
}
static bool a_has_phrase(const char *norm, const char *const *phrases)
{
    char pat[48];
    for (int i = 0; phrases[i]; i++) { snprintf(pat, sizeof(pat), " %s ", phrases[i]); if (strstr(norm, pat)) return true; }
    return false;
}
static int a_norm_ntok(const char *norm) { int n = 0; for (const char *p = norm; *p; p++) if (*p == ' ') n++; return n > 0 ? n - 1 : 0; }

// Strip a knowledge lead-in to the bare topic. Returns: 0 = no lead-in (keep original); 1 = FORMAL
// opener ("cos'è X" / "what is X") — strip for L1 but keep L0 FIRST (so "what is the time/weather"
// stays a live command); 2 = CONVERSATIONAL opener ("cosa sai di X" / "do you know X") — never a
// command, so the caller runs L1 on the topic FIRST. Never strips semantic words ("come"/"perche").
// An optional leading article is dropped after the opener.
static int a_topic_strip(const char *q, char *out, size_t outsz)
{
    char nq[160];
    a_norm_phrase(q, nq, sizeof nq);                   // " cosa sai di fisica "
    static const char *const lead_conv[] = {           // conversational -> L1-first
        "cosa sai di","cosa sai su","cosa sai sulla","cosa sai sul","che cosa sai di","cosa sapresti dirmi su",
        // di/su + ARTICLE contractions: "cosa sai DELLA fotosintesi" must route like "cosa sai DI X", not
        // carry "della" into the embedding as noise (which abstained on shard topics). Mirrors parlami del/della.
        "cosa sai della","cosa sai del","cosa sai dello","cosa sai dei","cosa sai degli","cosa sai delle","cosa sai dell",
        "che cosa sai della","che cosa sai del","cosa sai sullo","cosa sai sui","cosa sai sugli","cosa sai sulle",
        // "cosa sai DIRMI su/sulla X": l'infisso 'dirmi' rompe gli open"cosa sai X" sopra -> li rispecchio.
        "cosa sai dirmi di","cosa sai dirmi su","cosa sai dirmi sul","cosa sai dirmi sulla","cosa sai dirmi sullo",
        "cosa sai dirmi della","cosa sai dirmi del","cosa sai dirmi dello","cosa sai dirmi dei","cosa sai dirmi delle",
        "che cosa sai dirmi su","che cosa sai dirmi sulla","che cosa sai dirmi della","cosa mi sai dire di","cosa mi sai dire su",
        "dimmi di","dimmi qualcosa di","dimmi qualcosa su","dimmi cosa e","dimmi cos e","dimmi che cos e",
        "dimmi tutto su","dimmi tutto di","mi dici cos e","mi puoi dire cos e","parlami di","parlami del","parlami della",
        "parlami un po di","raccontami di","raccontami qualcosa di","mi parli di","mi spieghi","mi spieghi meglio",
        "mi sai spiegare","mi sapresti spiegare","mi spiegheresti","puoi spiegarmi","potresti spiegarmi","sapresti spiegarmi",
        "spiegami meglio","fammi capire","aiutami a capire","vorrei capire","voglio capire","ho sempre voluto capire",
        "non ho mai capito cosa e","non ho mai capito cosa fosse","non ho mai capito","mi sai dire","sai dirmi","sai cosa e",
        "conosci","hai presente","hai mai sentito parlare di","hai mai sentito di","ne sai qualcosa su",
        "ho sentito parlare di","ho sentito di","ho letto di","ho letto che","ho visto","mi hanno parlato di","sai qualcosa di",
        "what do you know about","what can you tell me about","what can you tell me","do you know about",
        "do you know","tell me about","tell me more about","tell me everything about","tell me what",
        "can you explain","could you explain","can you tell me","help me understand","walk me through",
        "give me a rundown of","give me an overview of","give me the gist of","i never understood","i have never understood",
        "i ve never understood","i always wanted to understand","i want to understand",
        "have you heard of","have you heard about","are you familiar with","ever heard of",
        "i heard about","i heard of","i read about",
        // describe-recall: more conversational openers (workflow-found misses, gate-verified 0-fabrication)
        "cosa sarebbe","cosa sarebbero","raccontami cos e","raccontami che cos e","puoi descrivermi","potresti descrivermi",
        "sai descrivermi","descrivimi un po","spiegatemi","spiegatemelo",
        "break down","break it down","can you break down","give me a breakdown of","what s the deal with","fill me in on", NULL };
    static const char *const lead_formal[] = {         // formal definition -> strip but L0-first
        "cos e","cose","che cos e","che cosa e","cosa e","cosa sono","cosa sia","quali sono","cosa significa",
        "che significa","che vuol dire","significato di","definizione di","definisci","definiscimi","descrivimi","descrivi",
        "spiegami","spiega","explain","describe","define","meaning of","how does","how do","what does",
        "what is the","what is a","what is an","what is","what are","what s","whats","who is","who was","who are",
        "descrivermi",
        // IT mechanism questions -> EN parity (kind=1 = L0-first, so live commands still win, then L1 gets the topic)
        "come funziona","come funzionano","come funziona il","come funziona la","come funziona lo","a cosa serve",
        "a cosa servono","a che serve","come si usa","come si fa", NULL };
    int kind = 0; const char *rest = NULL;
    for (int i = 0; lead_conv[i]; i++) {
        char pat[40]; snprintf(pat, sizeof pat, " %s ", lead_conv[i]);
        if (strncmp(nq, pat, strlen(pat)) == 0) { rest = nq + strlen(pat); kind = 2; break; }
    }
    if (!rest) for (int i = 0; lead_formal[i]; i++) {
        char pat[40]; snprintf(pat, sizeof pat, " %s ", lead_formal[i]);
        if (strncmp(nq, pat, strlen(pat)) == 0) { rest = nq + strlen(pat); kind = 1; break; }
    }
    if (!rest) return 0;
    static const char *const art[] = { "il ","lo ","la ","i ","gli ","le ","un ","uno ","una ","l ",
        "del ","dello ","della ","dei ","degli ","delle ","di ","the ","a ","an ", NULL };
    for (int i = 0; art[i]; i++) { size_t L = strlen(art[i]); if (strncmp(rest, art[i], L) == 0) { rest += L; break; } }
    while (*rest == ' ') rest++;
    if (!*rest) return 0;
    snprintf(out, outsz, "%s", rest);
    // A SECONDARY cue after the topic ("...kubernetes cos e?" from "kubernetes, cos'è?") -> keep only the
    // topic before it. (a_norm_phrase already folded the comma/apostrophe to spaces.) Cut at the earliest.
    static const char *const tailcut[] = { " cos ", " cos e", " cosa ", " che cos", " significa", " spiegami", " spiega ", NULL };
    size_t best = (size_t)-1;
    for (int i = 0; tailcut[i]; i++) { char *c = strstr(out, tailcut[i]); if (c) { size_t pos = (size_t)(c - out); if (pos > 0 && pos < best) best = pos; } }
    if (best != (size_t)-1) out[best] = 0;
    size_t n = strlen(out);                // trim trailing sentinel space / punctuation
    while (n && (out[n-1] == ' ' || out[n-1] == '?' || out[n-1] == '!' || out[n-1] == '.')) out[--n] = 0;
    // Strip TRAILING conversational noise so only the bare topic is left for retrieval
    // ("explain X to me" -> "X"; "how does X work" -> "X"; "X di preciso" -> "X"). Looped: peel several.
    static const char *const tailnoise[] = { " to me", " for me", " to a beginner", " please", " per favore",
        " di preciso", " esattamente", " in simple terms", " in parole semplici", " for dummies",
        " like i m five", " simply", " meglio", " un po", " work", " do", " mean",
        " actually", " really", " precisely", " exactly", NULL };   // adverb-interrupted "what does X actually mean" -> X
    for (bool cut = true; cut; ) {
        cut = false; n = strlen(out);
        for (int i = 0; tailnoise[i]; i++) {
            size_t tl = strlen(tailnoise[i]);
            if (n >= tl && strcmp(out + n - tl, tailnoise[i]) == 0) { out[n - tl] = 0; cut = true; break; }
        }
        n = strlen(out);
        while (n && (out[n-1] == ' ' || out[n-1] == '?' || out[n-1] == '!' || out[n-1] == '.')) out[--n] = 0;
    }
    return out[0] ? kind : 0;
}

// Epistemic self-report: ANIMA explains HOW certain it is about its last answer, read off the tier
// and confidence it ALREADY computed. The honest metacognition layer — the assistant knows how it
// knows (exact compute vs curated card vs deduction vs online vs a low-confidence rescue).
static void a_self_report(bool en, anima_result_t *r)
{
    const char *m; const char *it = s_session.last.intent; anima_tier_t t = s_session.last.tier; int c = s_session.last.conf;
    bool exact = !strcmp(it,"calc")||!strcmp(it,"percent")||!strcmp(it,"convert")||!strcmp(it,"ohm")||!strcmp(it,"base");
    bool live  = !strcmp(it,"battery")||!strcmp(it,"time")||!strcmp(it,"storage")||!strcmp(it,"ram")||!strcmp(it,"network")||
                 !strcmp(it,"date")||!strcmp(it,"uptime")||!strcmp(it,"version")||!strcmp(it,"year")||!strcmp(it,"season");
    if (!s_session.last.reply[0] || t == ANIMA_TIER_NONE)
        m = en ? "Honestly, I didn't know that one — I wouldn't rely on it." : "A dire il vero non lo sapevo: non mi fiderei.";
    else if (exact)                  m = en ? "Yes — it's an exact calculation, I can't get it wrong." : "Si: e un calcolo esatto, non posso sbagliarlo.";
    else if (!strcmp(it,"combinator")) m = en ? "Yes — I worked it out by combining facts I know." : "Si: l'ho calcolata componendo fatti che conosco.";
    else if (!strcmp(it,"hdc"))       m = en ? "Logically yes — I deduced it from facts I hold." : "Logicamente si: l'ho dedotta dai fatti che conosco.";
    else if (t == ANIMA_TIER_REMOTE) m = en ? "Yes — I checked it against an online source." : "Si: l'ho verificata da una fonte online.";
    else if (live)                   m = en ? "Yes — I read that straight from the device." : "Si: e un dato che leggo direttamente dal dispositivo.";
    else if (t == ANIMA_TIER_FACT && c >= 85) m = en ? "Yes — it's something I know with good confidence." : "Si: e una cosa che so con buona certezza.";
    else if (t == ANIMA_TIER_FACT)   m = en ? "Fairly — it's the most likely answer, but not 100%." : "Abbastanza: e la risposta piu probabile, ma non al 100%.";
    else                             m = en ? "Yes, I'm confident." : "Si, ne sono sicuro.";
    snprintf(r->reply, sizeof(r->reply), "%s", m);
}

// The dialogue-act tier. Returns 1 (and fills *r) when the input is a conversational meta-command
// resolved from working memory; 0 to let the normal cascade handle it. Runs early, before retrieval.
static int a_dialogue_act(const char *q, bool en, anima_result_t *r)
{
    char nz[160]; a_norm_phrase(q, nz, sizeof(nz));
    int nt = a_norm_ntok(nz);
    if (nt == 0) return 0;
    memset(r, 0, sizeof(*r));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->state, sizeof(r->state), "followup");

    // 1) CONFIDENCE CHALLENGE — "sei sicuro?", "davvero?" -> epistemic self-report.
    static const char *const sure[] = { "sei sicuro","sei sicura","sei certo","sei certa","ne sei sicuro",
        "sicuro","sicura","davvero","are you sure","you sure","really","for real", NULL };
    if (a_has_phrase(nz, sure) && (nt <= 4 || strstr(nz," sei sicuro ") || strstr(nz," sei certo "))) {
        snprintf(r->intent, sizeof(r->intent), "sure"); a_self_report(en, r); return 1;
    }

    // 2) CLARIFICATION — "spiegati meglio", "non ho capito" -> re-answer the last topic with its DETAIL
    //    (more verbose). Short queries only, so "non ho capito X" still gets routed to ask about X.
    static const char *const clar[] = { "spiegati meglio","spiega meglio","spiegamelo meglio","non ho capito",
        "non ho capito bene","non capisco","in che senso","puoi spiegare","puoi essere piu chiaro","cosa intendi",
        "explain better","i dont understand","dont understand","what do you mean","be clearer","explain again", NULL };
    if (nt <= 5 && a_has_phrase(nz, clar)) {
        if (s_mem.last_topic[0] && nucleo_anima_l1_query(s_mem.last_topic, en, true, r)) { snprintf(r->state, sizeof(r->state), "followup"); return 1; }
        snprintf(r->intent, sizeof(r->intent), "explain");
        snprintf(r->reply, sizeof(r->reply), en ? "Tell me which part and I'll try again." : "Dimmi quale parte e riprovo a spiegarla.");
        return 1;
    }

    // 3) RECAP — "ricapitola", "di cosa parlavamo" -> extractive summary of the working-memory ring.
    static const char *const recap[] = { "ricapitola","facciamo il punto","di cosa parlavamo","cosa stavamo dicendo",
        "riassumi","riepilogo","recap","what were we talking about","summarize","sum up", NULL };
    // recap summarizes the CONVERSATION, not an external document. "riassumi il documento che ho in mente"
    // / "summarize the document i have in mind" references a doc ANIMA cannot read -> don't recap the chat.
    static const char *const extdoc[] = { " documento "," document "," file "," libro "," book "," articolo ",
        " article "," testo che "," pagina "," pdf "," lettera ", NULL };
    bool ext = false; for (int i = 0; extdoc[i]; i++) if (strstr(nz, extdoc[i])) ext = true;
    if (!ext && a_has_phrase(nz, recap)) {
        snprintf(r->intent, sizeof(r->intent), "recap");
        char buf[256]; int o = 0, shown = 0;
        o += snprintf(buf+o, sizeof(buf)-o, en ? "So far: " : "Finora: ");
        for (int k = 0; k < s_session.ring_len && shown < 3; k++) {
            int i = (s_session.ring_head - 1 - k + ANIMA_RING*2) % ANIMA_RING;
            if (!s_session.ring[i].input[0]) continue;
            o += snprintf(buf+o, sizeof(buf)-o, "%s\"%s\"", shown?", ":"", s_session.ring[i].input); shown++;
        }
        if (!shown && s_mem.last_topic[0]) snprintf(buf, sizeof(buf), en ? "We were on: \"%s\"." : "Eravamo su: \"%s\".", s_mem.last_topic);
        else if (!shown) snprintf(buf, sizeof(buf), en ? "We just started." : "Abbiamo appena iniziato.");
        else snprintf(buf+o, sizeof(buf)-o, ".");
        snprintf(r->reply, sizeof(r->reply), "%s", buf);
        return 1;
    }

    // 4) THANKS — social glue.
    static const char *const thanks[] = { "grazie","grazie mille","ti ringrazio","thanks","thank you","cheers", NULL };
    if (nt <= 3 && a_has_phrase(nz, thanks)) {
        snprintf(r->intent, sizeof(r->intent), "thanks");
        snprintf(r->reply, sizeof(r->reply), en ? "You're welcome!" : "Prego, quando vuoi!");
        return 1;
    }

    // 5) DENIAL — "no", "sbagliato", "non è giusto" -> acknowledge + invite a rephrase (no fact asserted).
    static const char *const deny[] = { "sbagliato","errato","non e giusto","non e corretto","non e quello",
        "non e vero","thats wrong","that is wrong","not right","incorrect","nope", NULL };
    if (a_has_phrase(nz, deny) || (nt <= 2 && strstr(nz, " no "))) {
        snprintf(r->intent, sizeof(r->intent), "deny");
        snprintf(r->reply, sizeof(r->reply), en ? "Sorry — try rephrasing and I'll have another go." : "Scusa: prova a riformulare e ci riprovo.");
        s_mem.last_topic[0] = 0;        // don't let a rejected topic linger for the next "tell me more"
        return 1;
    }
    return 0;
}

// An OPEN-ENDED "describe / explain" request benefits from a fuller, multi-span answer (MOSAICO/L2);
// a crisp factoid reads better terse. Built on the SAME curated, accent-folded machinery the cascade
// already uses — a_topic_strip()'s lead-in families ("parlami di / cos'è / tell me about / what is /
// explain …") plus word-boundary a_has_phrase() over a normalized query — NOT a raw substring list, so
// it can't misfire mid-word and isn't fragile to accents/encoding. MOSAICO still fires only on an L1
// knowledge answer (the intent guard), so this is a "should I enrich?" quality signal, not a safety gate.
#define L1_STITCH_GATE 80      // only enrich a genuinely confident L1 anchor (conf = bestcos*100)
static bool a_is_describe(const char *q)
{
    char topic[160];
    if (a_topic_strip(q, topic, sizeof topic) != 0) return true;   // cos'è / parlami di / what is / tell me about / explain …
    // Explanation/identity cues a_topic_strip doesn't strip, matched word-boundary (a_norm_phrase pads
    // with spaces and folds accents, so " perche " never matches inside "perchéx" and "chi e" never
    // matches inside "chiede"). Curated + locked by eval_describe.jsonl.
    char nq[160]; a_norm_phrase(q, nq, sizeof nq);
    static const char *const cues[] = {
        // definition / identity — also catches EMBEDDED and apostrophe-CONTRACTED forms that
        // a_topic_strip's prefix anchor misses ("what's"->"what s", "cos'è"->"cos e", mid-sentence).
        "cos e", "che cos", "cosa e", "cosa sono", "cosa sia", "quali sono", "quale e",
        "chi e", "chi era", "chi sono", "descrivi", "descrivimi", "spiegami", "spiega",
        "what is", "what s", "what are", "who is", "who was", "who are", "who s", "describe", "explain",
        // explanation / purpose / mechanism
        "come funziona", "come funzionano", "a cosa serve", "a cosa servono", "a che serve", "perche",
        "how does", "how do", "what does", "why is", "why does", "what for", NULL };
    return a_has_phrase(nq, cues);
}

// Cascade (docs/anima.md §2): L0 keyword tier first (cheap, exact); on a miss fall through
// to the L1 semantic tier (distilled encoder + RAG index, if the SD packs are present).
// Understands IT+EN regardless of `lang`; replies in `lang` ("en" -> English, else Italian).
// DIAG breadcrumb: the ANIMA tier active when the device last reset. Survives a warm reboot (RTC
// memory, not zeroed at boot); read + cleared at boot (main.c) and surfaced at WARN in /api/logs, so a
// crash is locatable over WiFi without the serial console. 0=clean, 0xB0=query entry, 0xC0=L1,
// 0xC1=L1 index load (SD), 0xD0=online/learned recall.
RTC_NOINIT_ATTR uint32_t g_anima_stage;
RTC_NOINIT_ATTR uint8_t  g_anima_phase;   // DIAG: cascade phase (tier funcs never touch it -> never stale)

// Cumulative query telemetry (see nucleo_anima.h). Bumped once in the done: epilogue below — the single
// point every cascade path converges on — so it costs a few u32 stores per query and nothing at rest.
static anima_diag_t s_diag;
void nucleo_anima_diag(anima_diag_t *out) { if (out) *out = s_diag; }
static void diag_count(const anima_result_t *r)
{
    s_diag.queries++;
    switch (r->tier) {
        case ANIMA_TIER_COMMAND: s_diag.t_command++; break;
        case ANIMA_TIER_FACT:    s_diag.t_fact++;    break;
        case ANIMA_TIER_STITCH:  s_diag.t_stitch++;  break;
        case ANIMA_TIER_REMOTE:  s_diag.t_remote++;  break;
        default:                 s_diag.t_none++;    break;   // NONE = honest miss / abstain
    }
    s_diag.last_conf = r->confidence;
    snprintf(s_diag.last_intent, sizeof(s_diag.last_intent), "%s", r->intent);
}

anima_result_t nucleo_anima_query(const char *input, const char *lang)
{
    g_anima_stage = 0xB0;                 // DIAG: entered the ANIMA query
    g_anima_phase = 0x01;                 // DIAG: cascade entry
    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');
    // RAM policy: stand the offline L1/HDC brain down for this turn ONLY in ONLINE-ONLY mode, where the
    // cloud LLM is the sole brain and L1's index would be dead weight competing for the TLS handshake's
    // heap. In HYBRID, L1 SERVES (it's half of "L1 + intelligent wiki search") so we must NOT stand it
    // down just because a key exists. AUTO honors this; a user FORCE_ON/OFF from the apps overrides it.
    nucleo_anima_l1_set_online_brain(nucleo_anima_online_available() && nucleo_anima_teacher_configured()
                                     && nucleo_anima_online_only_enabled());
    s_session.turn++;
    trace_reset();        // fresh thought-log for this turn
    content_reset();      // no composed payload until a tool produces one
    nucleo_anima_set_long_reply(NULL);   // drop any previous turn's long (code) overflow reply

    // Action memory: "ripeti" / "di nuovo" replays the last actionable turn from the ring.
    const char *q = input;
    bool replayed = false;
    if (a_is_repeat(input)) {
        const char *prev = ring_last_input();
        if (prev) { q = prev; replayed = true; }      // else fall through -> honest miss
    }
    anima_result_t r;

    // Snapshot the conversation transcript ONCE for every online-teacher call this turn (the ring is
    // only appended at the epilogue, so this stays valid throughout). Oldest->newest; nctx may be 0.
    anima_turn_t ctx[ANIMA_CHAT]; int nctx = chat_context(ctx, ANIMA_CHAT);

    // Resolve a pending L1 knowledge clarify ("intendi 1) … o 2) …?"): an ordinal picks one of
    // the two offered cards. Not a pick -> drop the clarify and handle the input normally.
    if (s_session.clarify_l1) {
        int pick = a_pick_ordinal(input);
        if (pick >= 0 && nucleo_anima_l1_read(s_session.clarify_ans[pick], en, &r)) {
            s_session.clarify_l1 = false; r.from_memory = 1;
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", input);   // enable "tell me more"
            s_session.dirty = true;
            goto done;
        }
        s_session.clarify_l1 = false;
    }

    // Resolve a pending KNOWLEDGE<->SKILL clarify ("vuoi sapere cos'è X o calcolarlo?"). Map the reply
    // to one side. Anything else — the actual problem with numbers, or a new subject — drops the clarify
    // and is handled by the normal cascade (so "massa 2 accelerazione 3" goes straight to the solver).
    if (s_session.skill_clarify[0]) {
        char topic[40]; snprintf(topic, sizeof topic, "%s", s_session.skill_clarify);
        s_session.skill_clarify[0] = 0;
        char nz[120]; a_norm_phrase(input, nz, sizeof nz);
        bool digit = false; for (const char *p = input; *p; p++) if (isdigit((unsigned char)*p)) digit = true;
        static const char *const w_know[] = { "cos","cosa","cose","sapere","sappi","definizione","significato",
            "spiega","spiegami","spiegamela","teoria","uno","primo","prima","what","first","know","explain","definition", NULL };
        static const char *const w_do[]   = { "calcola","calcolare","calcolo","calcolarla","risolvi","risolvere",
            "problema","esercizio","due","secondo","seconda","calculate","solve","compute","problem","second", NULL };
            // NB: not "skill" — it collides with "che skill hai" (a capabilities question, not a clarify pick).
        bool wantKnow = a_has_phrase(nz, w_know), wantDo = a_has_phrase(nz, w_do);
        if (wantKnow && !wantDo) {
            if (nucleo_anima_l1_query(topic, en, false, &r)) {
                a_offer_skill(&r, topic, en);
                snprintf(r.state, sizeof(r.state), "clarify");
                snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", topic);
                s_session.dirty = true; goto done;
            }
        } else if (wantDo && !digit) {                      // wants the skill but gave no data yet -> prompt for it
            memset(&r, 0, sizeof(r));
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = 80; r.awaiting = 1;
            snprintf(r.intent, sizeof(r.intent), "clarify");
            snprintf(r.state, sizeof(r.state), "clarify");
            snprintf(r.reply, sizeof(r.reply), en ? "Great — give me the data and I'll compute it."
                                                  : "Perfetto, dammi i dati e lo calcolo.");
            goto done;
        }
        // not a clear pick -> fall through: the normal cascade handles the new input (data, calc, or topic).
    }

    // Drill-down: "dimmi di più" / "tell me more" -> re-query the LAST knowledge topic for its
    // detail text. Makes the assistant feel conversational without any generation (two-level
    // retrieval: the same card carries a short reply and a longer detail).
    if (a_is_more_request(q) && s_mem.last_topic[0]) {
        if (nucleo_anima_l1_query(s_mem.last_topic, en, true, &r)) {
            snprintf(r.state, sizeof(r.state), "followup");
        } else {
            memset(&r, 0, sizeof(r));
            r.tier = ANIMA_TIER_COMMAND; r.action = ANIMA_ACT_ANSWER; r.confidence = 60;
            snprintf(r.state, sizeof(r.state), "followup");
            snprintf(r.intent, sizeof(r.intent), "more");
            snprintf(r.reply, sizeof(r.reply), en ? "That's all I have on that." : "Su questo non ho altri dettagli.");
        }
        goto done;
    }

    // Dialogue acts: conversational meta-commands ("sei sicuro?", "spiegati meglio", "ricapitola",
    // "grazie", "no") resolved from working memory — the agent-like turn-taking glue. Skipped while a
    // tool slot or app clarify is pending, so a "no" there resolves the FSM, not the dialogue layer.
    if (!s_session.pending_tool[0] && !s_session.clarify_opt[0][0] && a_dialogue_act(q, en, &r)) goto done;

    // Online-only mode: bypass the offline cascade entirely and answer ONLY via PURE Grok — a direct
    // chat passthrough (no JSON classification, no Wikipedia truth gate, no learning). Honest miss if
    // unconfigured/offline — we deliberately do NOT fall back to the offline tiers (that's hybrid "On").
    if (s_online_only) {
        bool avail = nucleo_anima_online_available();
        // CODE even in online-only: take the dedicated code path (code prompt, bigger token budget,
        // verbatim long_reply) — the prose teacher's clip_reply sentence-truncates on '.' and so
        // mangles a snippet mid-statement (cuts "pygame.display." in half).
        if (avail && a_is_code_request(q) && nucleo_anima_online_code(q, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
        // WITH the multi-turn transcript so a pure-online follow-up ("divo 3?", "e lui?") resolves
        // against the running conversation — the old one-shot passthrough dropped all context, which
        // is why online mode "forgot" the prior turn. Still pure (no truth gate, no learning).
        if (avail && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
        // INTELLIGENT FALLBACK (user: ANIMA "sempre funzionante con fallback automatici"): the cloud is
        // unreachable or gave no answer — on this PSRAM-less chip the mbedTLS handshake often can't get its
        // ~24 KB contiguous block and OOMs (measured live: oom.count 0->12). Instead of a dead "no answer",
        // drop to the OFFLINE cascade so the assistant still helps. L1 was stood down for the cloud's RAM
        // (s_l1_online_brain, set at the top of this fn) -> RE-ARM it so the offline brain can actually
        // answer (its index lazy-loads from SD; the cloud attempt is over, the heap is free again). Mirrors
        // the recorder discipline: each step frees/uses RAM in turn, never overloading the chip. try_cascade
        // is offline-PURE (L0 commands + L1 knowledge, zero TLS) so it can neither re-OOM nor block. We take
        // ONLY a real textual ANSWER (knowledge/system/math) — never auto-fire an offline command/tool that
        // the user typed expecting the cloud — and LABEL it so an offline reply is never mistaken for the
        // online model (the on-device twin of the web app's "Ripiego offline" rung).
        nucleo_anima_l1_set_online_brain(false);
        if (try_cascade(q, en, &r) && r.action == ANIMA_ACT_ANSWER && r.reply[0]) {
            char body[sizeof r.reply];
            snprintf(body, sizeof body, "%s", r.reply);
            snprintf(r.reply, sizeof r.reply, "(offline) %s", body);
            snprintf(r.intent, sizeof r.intent, "online_only_offline");
            s_session.dirty = true;
            goto done;
        }
        // Even the offline cascade had nothing -> honest, non-empty dead-stop (unchanged strings).
        memset(&r, 0, sizeof(r));
        r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE;
        snprintf(r.intent, sizeof(r.intent), "online_only");
        snprintf(r.reply, sizeof(r.reply),
                 avail ? (en ? "Online-only mode, but the online model gave no answer."
                             : "Modalita solo online: il modello online non ha risposto.")
                       : (en ? "Online-only mode: no internet connection."
                             : "Modalita solo online: nessuna connessione a internet."));
        goto done;
    }

    // SAFETY GATE: only a genuine question reaches the online/knowledge tiers below. A bare date /
    // number / cell-ref / code / wordless string is data, not a question -> it must never be answered
    // with a fabricated "fact" nor learned. (Deterministic L0/L1/math already ran; this fences off the
    // online entity/Wikidata/teacher tiers, which are the only ones that can hallucinate.)
    const bool askable = a_is_askable(q);

    // POLICY: the cloud LLM (chat completions: chat_ctx / code / teacher) is allowed ONLY in ONLINE-ONLY
    // mode. HYBRID answers knowledge with L1 + INTELLIGENT WIKI SEARCH (Wikidata facts + Wikipedia bios,
    // all `!online_llm` paths below) — deterministic, grounded, far lighter on this PSRAM-less heap than a
    // chat completion carrying the whole transcript. (Online-only itself is handled+returned far above; so
    // online_llm is effectively false through this hybrid section, which is exactly the intent.)
    const bool online_llm = nucleo_anima_online_available() && nucleo_anima_teacher_configured()
                            && nucleo_anima_online_only_enabled();

    // Classify once up-front: the F_* feature flags drive the live/weather routing below AND the
    // later spellfix gate. (Pure function of q; q is stable from here on.)
    anima_plan_t plan; anima_cortex_plan(q, en, &plan);

    // LIVE-data priority (weather/news): these MUST beat the L0 FAQ/date intents, which would
    // otherwise grab the "che/fa/domani" words in a weather phrasing ("che tempo fara domani a
    // brescia" was being answered as capabilities). A forecast REQUEST is F_WEATHER/F_NEWS but NOT a
    // definition (F_DEFWORD: "cos'è il clima" still answers from L1). Free the L1 index first — the TLS
    // handshake needs a large contiguous heap block on this PSRAM-less chip.
    // ...but NOT a calculation: "20 gradi in fahrenheit" / "tempo di caduta" carry weather words yet are
    // math. A digit or a math operator means compute, not forecast -> let the math/L1 tiers handle it.
    bool has_digit = false; for (const char *d = q; *d; d++) if (*d >= '0' && *d <= '9') { has_digit = true; break; }
    // ...and NOT a create-file command: a weather word inside the note CONTENT ("crea una nota con
    // scritto domani piove") must not let the weather tier swallow the create_file action.
    bool is_create_cmd = (plan.feat & F_CREATEVB) && (plan.feat & F_FILENOUN);
    // ...and NOT geometry: "quanti GRADI ha un triangolo" carries the weather word "gradi" but is a
    // math/geometry question (degrees, not temperature) -> let it fall through to L1/math.
    bool is_geo = strstr(q, "triangol") || strstr(q, "angol") || strstr(q, "quadrat") || strstr(q, "cerchio") ||
                  strstr(q, "poligon") || strstr(q, "esagon") || strstr(q, "pentagon") || strstr(q, "rettangol");
    // ...and NOT an image-generation command: "draw an image of a SNOWY mountain" / "genera una foto di
    // PIOGGIA" carry a weather word but are a request to PAINT a picture, not a forecast -> let the
    // image_gen decline tool (in l0_query) own it, exactly as is_create_cmd protects create_file.
    bool is_image_gen = false;
    { char gtok[A_MAX_TOKENS][A_TOK_LEN]; int gnt = a_tokenize(q, gtok); is_image_gen = a_is_image_gen(gtok, gnt); }
    // An explicit TRANSLATE request ("traduci sole in inglese", "come si dice pioggia") carries a weather
    // word as its OBJECT, not its subject — the offline dictionary must own it, never the forecast. Veto.
    bool is_translate = nucleo_anima_translate_is_request(q);
    bool wx_req = (plan.feat & (F_WEATHER | F_NEWS)) && !(plan.feat & (F_DEFWORD | F_MATHOP)) && !has_digit && !is_create_cmd && !is_geo && !is_image_gen && !is_translate;
    if (askable && (wx_req || nucleo_anima_online_is_live(q, en))) {
        if (nucleo_anima_online_available()) nucleo_anima_l1_unload();
        if (nucleo_anima_online_live(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }
        // A weather/news REQUEST with no live data (offline / unreachable) -> honest miss. NEVER fall
        // through to L1, which would answer "che tempo fa a Roma" with the Rome-history card or "c'è il
        // sole a Bari?" with the Sun-is-a-star card. The forecast just needs internet.
        memset(&r, 0, sizeof r);
        r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE; r.confidence = 0;
        snprintf(r.intent, sizeof r.intent, (plan.feat & F_NEWS) ? "news" : "weather");
        goto done;
    }

    // CODE generation -> the online model directly. "scrivimi / dammi un esempio di codice python"
    // wants a real, professional snippet — which only the LLM produces, never a Wikipedia bio or an L1
    // card. Routed here (before the entity/L1 tiers) with a code prompt + a larger reply budget so the
    // fenced ```block isn't truncated. Online+key only; offline/no-key falls through (we never fabricate
    // code). Frees L1 first (the TLS handshake needs the contiguous heap on this PSRAM-less chip).
    if (online_llm && a_is_code_request(q)) {   // LLM-only: code is generated, never wiki-searched
        nucleo_anima_l1_unload();
        if (nucleo_anima_online_code(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }
    }

    // SPECIFIC entity questions ("cosa ha fatto X", "per cosa è famoso X", "what did X do") that the
    // frozen bio can't answer: let Grok answer them, grounded by its knowledge. Online + key only —
    // chat_ctx returns 0 without a key, so offline/no-key falls through to the entity bio (best
    // effort). Free L1 first (TLS handshake needs the contiguous heap).
    if (online_llm && askable && nucleo_anima_online_is_about(q, en)) {   // LLM-only: hybrid answers "cosa ha fatto X" from the Wikipedia bio below
        nucleo_anima_l1_unload();
        // WITH conversation context: "e cosa ha fatto?" (is_about, subject-less) resolves against the
        // previous turn; "cosa ha fatto einstein" (named) works too (context is harmless).
        if (nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
            if (a_is_followup_q(q)) snprintf(r.state, sizeof(r.state), "followup");
            mem_update(&r); s_session.dirty = true; goto done;
        }
    }

    // Structured-FACT priority (hybrid): "quando è morto X" / "capitale di X" must get the PRECISE
    // Wikidata fact, not the generic entity BIO that L1 returns for any question about X (otherwise
    // every Stalin question yields the same bio). Online-only and gated to actual fact-questions, so a
    // non-fact query never pays a net round-trip; offline -> 0 here and the L1 bio (whose prose holds
    // the fact) answers. Free L1 first — the TLS claim fetch needs the contiguous heap.
    if (askable && nucleo_anima_online_available() && nucleo_anima_online_is_fact(q, en)) {
        nucleo_anima_l1_unload();
        if (nucleo_anima_online_fact(q, en, &r)) {
            mem_update(&r);
            snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
            s_session.dirty = true;
            goto done;
        }
    }

    // CONVERSATIONAL FOCUS SHIFT (structured coreference): a bare CONTINUATION of the current thread.
    // SUBJECT-shift — a leading connector + a new entity, no question word ("e newton?", "e tokyo?") —
    // keeps the relation and swaps the entity; RELATION-shift — a subject-less question fragment — keeps
    // the entity. We re-aim the focus from a relation TEMPLATE (no text subtraction) and re-run the KGE
    // reasoner. This runs BEFORE the generic cascade on purpose, so the conversational relation wins over
    // an unrelated card ("e tokyo?" after "dove si trova lione" must answer location, not Tokyo's capital).
    // The reasoner's own lexical/role/coherence guards reject a wrong re-aim, so on a refuse we simply fall
    // through to the normal cascade (which reloads L1 on demand). Only fires with a fresh focus -> a cold
    // query (no prior fact in the thread) can never be hijacked, so single-shot routing cannot regress.
    if ((s_session.foc_subject[0] || s_session.foc_relation[0]) &&
        (s_session.turn - s_session.foc_turn) <= 8) {
        char ftok[A_MAX_TOKENS][A_TOK_LEN]; int fnt = a_tokenize(q, ftok);
        static const char *const conn[] = { "e","ed","poi","allora","anche","invece","ma","quindi","pure",
                                             "and","then","also","plus", NULL };
        bool lead_conn = false;
        if (fnt >= 1) for (int i = 0; conn[i]; i++) if (!strcmp(conn[i], ftok[0])) { lead_conn = true; break; }
        bool qword = a_has_qword(ftok, fnt);
        char shifted[176]; shifted[0] = 0;
        if (lead_conn && !qword && s_session.foc_relation[0] && fnt >= 2 && fnt <= 4) {
            // SUBJECT-shift: skip leading connectors, take the rest as the new entity, reuse the relation.
            int s0 = 0;
            while (s0 < fnt) { bool c = false; for (int i = 0; conn[i]; i++) if (!strcmp(conn[i], ftok[s0])) c = true; if (!c) break; s0++; }
            char ent[64]; int o = 0; ent[0] = 0;
            for (int t = s0; t < fnt && o + 1 < (int)sizeof ent; t++) o += snprintf(ent + o, sizeof ent - o, "%s%s", o ? " " : "", ftok[t]);
            const char *tmpl = foc_template(s_session.foc_relation);
            if (tmpl && (fnt - s0) >= 1 && strlen(ent) >= 2) snprintf(shifted, sizeof shifted, tmpl, ent);
        } else if (qword && s_session.foc_subject[0] && a_is_followup_q(q)) {
            // RELATION-shift: the subject-less question fragment carries the new relation; reuse the subject.
            snprintf(shifted, sizeof shifted, "%s %s", q, s_session.foc_subject);
        }
        if (shifted[0]) {
            nucleo_anima_l1_unload();                       // the reasoner builds its own KG of HVs; it needs the heap
            if (nucleo_anima_hdc_reason(shifted, en ? "en" : "it", &r)) {
                foc_remember(&r);                           // chain: the re-aimed turn becomes the new focus
                snprintf(r.state, sizeof r.state, "followup");
                mem_update(&r);
                snprintf(s_mem.last_topic, sizeof s_mem.last_topic, "%s", shifted);
                s_session.dirty = true;
                goto done;
            }
        }
    }
    
    // (Hook L0 dynamic-skill .lua su SD rimosso: interprete Lua ~90 KB flash, scaffold inerte
    //  — nessuno script spedito, anima_say scriveva solo sul log. Riattivabile se si integra davvero.)

    // STRUCTURED deduction precedes fuzzy L1 for fact-questions ("quando e nato Dante", "capitale del
    // Kenya", "in che continente e X"): nucleo_anima_hdc_reason fires ONLY on a recognized fact pattern
    // whose entity resolves in the learned graph (edge-grounded forward/inverse, honest-coherent
    // transitive) — otherwise it returns false and L0/L1 run unchanged. This stops a conversational card
    // (e.g. self.age matching "quando e nato <person>") from shadowing a precise stored fact.
    // ...UNLESS the input is a tool COMMAND: "crea una nota con la capitale della Francia" must CREATE
    // the note (composing the deduced fact INTO it), not be hijacked into a bare "Parigi" answer. So skip
    // the standalone reasoner for a create-file command; try_cascade below runs the compose-then-act tool,
    // whose ag_compose consults this very reasoner for the sub-clause -> the deduced fact lands in the file.
    {
        char ctok[A_MAX_TOKENS][A_TOK_LEN]; int cnt = a_tokenize(q, ctok);
        if (!a_is_create_file(ctok, cnt)) {
            // TYPED FACET first: a precise "che lavoro faceva X / X è uomo o donna" beats a fuzzy L1 bio
            // and the KGE (categorical facets don't live in the holographic graph). Abstains on a miss.
            if (nucleo_anima_facet(q, en, &r)) goto done;
            // NSPCG generative tier: a proof-carrying GENERATION for where/why/bridge questions ("dove si
            // trova X", "in che continente e X", "perche X e in Y", "come e collegato X a Y") — structured +
            // grounded, so it precedes the fuzzy L1, and it produces a NEW sentence the corpus never stored
            // with a self-verified proof (refuses if no grounded chain). It needs the contiguous heap for its
            // KG build, so reclaim L1's index FIRST — but ONLY when the query is actually NSPCG-shaped, so a
            // normal fact-question keeps its index for try_cascade below (a pcg refuse reloads it from SD).
            if (nucleo_anima_pcg_detect(q, en ? "en" : "it")) {
                nucleo_anima_l1_unload();
                if (nucleo_anima_pcg_generate(q, en ? "en" : "it", &r)) goto done;
            }
            if (nucleo_anima_hdc_reason(q, en ? "en" : "it", &r)) goto done;
        }
    }

    g_anima_phase = 0x03;                  // DIAG: try_cascade (L0/L1)
    if (try_cascade(q, en, &r)) {
        // Self-improving knowledge: an L1 fact hit that was LEARNED without Grok gets re-vetted now that
        // a key is configured (confirm -> permanent, veto -> dropped). No-op for baked cards / no key /
        // offline. s_mem.last_topic is the bare topic L1 matched on.
        if (r.tier == ANIMA_TIER_FACT && nucleo_anima_online_available())
            nucleo_anima_online_upgrade(s_mem.last_topic, en);
        // MOSAICO (L2 span-stitch): a confident L1 answer to a DESCRIBE/EXPLAIN question is enriched with
        // more grounded spans from the SAME index (its detail + a coherent runner-up) — verbatim span-copy
        // (cannot hallucinate). Crisp factoids and rescue-band guesses are left untouched. The index is
        // still loaded here (the unload only happens on the miss path below), so s_band is this query's.
        if (r.tier == ANIMA_TIER_FACT && r.action == ANIMA_ACT_ANSWER && !strcmp(r.intent, "l1")
            && r.confidence >= L1_STITCH_GATE && a_is_describe(q)
#ifdef ANIMA_HOST
            && !getenv("ANIMA_NO_STITCH")          // A/B toggle for the fluency-grounded gate
#endif
            && nucleo_anima_l1_stitch(q, en, &r))
            trace_step(en ? "stitch: fuse spans" : "stitch: fondo frammenti");
        goto done;
    }

    // Miss -> typo rescue: correct command words and retry ONCE. Because this runs only after a
    // miss, queries that already work are never altered (routing can't regress by construction).
    // CORTEX plan, built lazily HERE (not before try_cascade) so the ~70-90% of queries L0/L1
    // answer never pay for it — true to the cascade's "don't light up what you don't need". Its job
    // here: suppress the command-vocab rescue on fact/live questions, where "correcting" a valid
    // content word toward the command vocab corrupts them ("domani" -> "comandi"). `plan` was already
    // computed up-front (before the LIVE block).
    g_anima_phase = 0x04;                  // DIAG: spellfix rescue
    if (plan.allow_spellfix) {
        char fixed[160];
        if (a_spellfix(q, fixed, sizeof(fixed)) && try_cascade(fixed, en, &r)) {
            snprintf(r.corrected, sizeof(r.corrected), "%.*s", (int)sizeof(r.corrected) - 1, fixed);   // "ho inteso: …"
            goto done;
        }
    }

    // NEURO-SYMBOLIC COMBINATOR tier (FIRST reasoning tier): a compositional question whose answer is
    // COMPUTED by composing >=2 learned facts and exists as NO single stored triple — "chi e nato prima
    // A o B" (compare), "quanti anni tra la nascita di A e B" (subtract), "X era europeo" (nationality->
    // continent), "A e B erano connazionali" (equality). Runs BEFORE the KGE deductive detector below so a
    // composition is never mis-parsed as a simple "X di Y" fact lookup (mirrors the sim's ordering).
    // Pure integer/string composition over the learned triples -> cannot fabricate. ANTI-HIJACK: if the
    // question IS compositional but a required fact is missing, it returns TRUE with an honest "non ho i
    // dati" miss — STOPPING the cascade so no later entity/recall tier answers a comparison with a random
    // bio. It returns false ONLY for a non-compositional query (then the cascade continues normally).
    g_anima_phase = 0x05;                  // DIAG: combinator tier
    if (nucleo_anima_combinator(q, en ? "en" : "it", &r)) {
        if (r.confidence > 0) { mem_update(&r); snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q); }
        s_session.dirty = true;
        goto done;
    }

    // OFFLINE DEDUCTIVE tier (HDC/permutation-KGE): a fact L0/L1 didn't have, but that is logically
    // ENTAILED by the learned triples — "qual e la capitale della Francia" (inverse), "in che continente
    // e Lione" (transitive), "quando e nato X" (forward). Deduced on-device by composing relation-
    // rotations, gated by resonance coherence (refuses rather than fabricating). Runs BEFORE the online
    // tiers so a verifiable offline deduction beats a network round-trip; on a refuse it falls through.
    //
    // Free L1's ~18 KB index (+ row cache) FIRST: the reasoner builds its own KG of entity hypervectors
    // and needs the contiguous heap. L0/L1 already missed here, so the index isn't needed for the rest of
    // this query; the next query reloads it from SD. Without this the kg_build malloc fails on the PSRAM-
    // less chip (~25 KB free with L1 resident < the build peak) and the deductive tier silently never fires.
    g_anima_phase = 0x06;                  // DIAG: L1 unload (pre-HDC)
    nucleo_anima_l1_unload();
    g_anima_phase = 0x07;                  // DIAG: HDC deductive (kg_load_subgraph + kg_build malloc)
    if (nucleo_anima_hdc_reason(q, en ? "en" : "it", &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // HARD SAFETY GUARD: never let a non-question (bare date/number/code/garbage) reach the online
    // fetch + teacher tiers — those are the only ones that can FABRICATE a "fact" and learn it. The
    // deterministic + offline-deductive tiers above already had their chance; a miss here is honest.
    if (!askable) { memset(&r, 0, sizeof(r)); r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE; goto done; }

    // Free the L1 index BEFORE the online fetch tiers. On this PSRAM-less chip the mbedTLS handshake
    // needs a large CONTIGUOUS heap block that the loaded ~18 KB index fragments away — without this
    // the Wikipedia/Wikidata fetch silently TIMES OUT and the user gets "non lo so" (then a garbage L1
    // "did you mean"). L0/L1 already missed, so the index isn't needed for the rest of this query; the
    // next query reloads it from SD on demand. (The teacher tier already does this same reclaim.)
    g_anima_phase = 0x08;                  // DIAG: post-askable, online/recall tiers
    if (nucleo_anima_online_available()) nucleo_anima_l1_unload();

    // Trusted-data-first ("dati certi → Grok"): the structured web tiers (Wikidata facts, then the
    // Wikipedia bio) run BEFORE the cloud teacher so a verifiable, learnable fact always wins over the
    // LLM's prose. This is safe now that the Wikipedia hang is fixed at the root — it was an httpd-task
    // stack overflow during the long cert-chain TLS handshake (config.stack_size now 16 KB), NOT a slow
    // or unreachable host — so the structured fetch completes in ~1s instead of stalling ~60s. Grok
    // stays the last-resort fallback for open-ended misses (the teacher tier at the end of the cascade).

    // POLICY (user): when ONLINE and a cloud LLM is configured, the online knowledge tiers go through the
    // LLM ALONE — no Wikipedia opensearch/summary, no Wikidata, no bare-noun lookup, no teacher truth-gate
    // fetch. Rationale is twofold: (1) coherence — one brain answers, not a patchwork; (2) RAM — each of
    // those structured tiers is a SEPARATE outbound TLS handshake (GET), and on this PSRAM-less chip every
    // handshake fragments the scarce heap, so doing 2-3 of them per "chi è X" was what left no contiguous
    // block for the next query ("online works once then stops"). Routing entity questions straight to the
    // chat LLM means ONE handshake per turn. In HYBRID (online_llm==false, defined above) Wikipedia/Wikidata
    // IS the path — one structured GET, remembered for offline. Offline cache/recall (network-free) run regardless.

    // Wikidata precise facts (born/died/capital/author): deterministic, no key. Before the Wikipedia
    // bio so "capitale di X" / "quando è nato X" gives the fact, not a summary. Skipped when the LLM owns
    // online (policy above); keyless devices still use it.
    if (!online_llm && nucleo_anima_online_fact(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Online knowledge tier (docs/anima-online.md): a "chi è / cos'è X" question L0+L1 didn't know.
    // With an LLM configured -> answer it via the chat model (one TLS POST), WITH conversation context.
    // Without a key -> the keyless Wikipedia path (learned-card cache, else a structured summary fetch
    // that is REMEMBERED for offline next time). Runs before the band so a real answer beats a fuzzy guess.
    {
        char entity[64], slug[64];
        if (nucleo_anima_online_entity(q, en, entity, sizeof(entity), slug, sizeof(slug))) {
            if (online_llm) {
                if (nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
                    mem_update(&r);
                    snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
                    s_session.dirty = true;
                    goto done;
                }
            } else if (nucleo_anima_online_answer(entity, slug, en, &r)) {
                mem_update(&r);
                snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);   // enable "tell me more"
                s_session.dirty = true;
                goto done;
            }
        }
    }

    // Live tier (docs/anima-online.md): time/place-bound questions — weather, exchange rate, news.
    // Answered FRESH and NEVER learned: caching volatile data would assert stale facts as timeless
    // (volatility law §6). Runs before the band so a real live answer beats a fuzzy "did you mean".
    if (nucleo_anima_online_live(q, en, &r)) { mem_update(&r); s_session.dirty = true; goto done; }

    // USER-TAUGHT facts (offline, network-free): a paraphrase of something the USER taught with "ricorda
    // che / remember that". Embedded by the SAME shared encoder, gated by the same dual-channel discipline
    // as L1. Checked before the Wikipedia recall so a personal fact wins, and (unlike the online tier) it
    // also runs in the host harness, so the teach->recall loop is verifiable without a device. No network.
    g_anima_phase = 0x09;                  // DIAG: learn_recall (offline taught)
    if (nucleo_anima_learn_recall(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Semantic recall over LEARNED cards (docs/anima-online.md §5.2): a paraphrase of something the
    // device already learned, matched offline by the shared encoder. No network; conservative gate.
    if (nucleo_anima_online_recall(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);   // enable "tell me more"
        s_session.dirty = true;
        goto done;
    }

    // CONTEXTUAL FOLLOW-UP (coreference): a subject-less fragment — "e cosa ha fatto?", "e perché?",
    // "e lui?". Handle it HERE, before the clarify band / truth-gated teacher (which would misread a
    // fragment). Online -> Grok with the REAL last turn as context (resolves pronouns/ellipsis). Offline
    // -> the last topic's card (the bio): it still knows what you're talking about.
    if (a_is_followup_q(q)) {
        if (online_llm && nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {   // LLM-only; hybrid resolves the follow-up against the last topic's L1 card below
            snprintf(r.state, sizeof(r.state), "followup"); mem_update(&r); s_session.dirty = true; goto done;
        }
        if (s_mem.last_topic[0] && nucleo_anima_l1_query(s_mem.last_topic, en, false, &r)) {
            snprintf(r.state, sizeof(r.state), "followup"); mem_update(&r); s_session.dirty = true; goto done;
        }
    }

    // Still a miss -> dialogic clarify band (KNOWLEDGE only), on the last query's top-2 candidates.
    // Runs AFTER the typo rescue so a corrected launch/command isn't pre-empted by a fuzzy clarify.
    {
        long a1, a2;
        if (nucleo_anima_l1_band(en, &r, &a1, &a2)) {
            s_session.clarify_l1 = true; s_session.clarify_ans[0] = a1; s_session.clarify_ans[1] = a2;
            goto done;
        }
    }

    // Bare-noun entity fallback ("batman?", "einstein"): a short command-less noun L0/L1/clarify all
    // missed. STRICT Wikipedia lookup (free, no key) so junk stays an honest miss. Skipped when the LLM
    // owns online (it'll answer below); keyless devices still use it.
    if (!online_llm && nucleo_anima_online_entity_bare(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }

    // Last resort: the cloud teacher (LLM-backed: self-classifies via chat completions, verifies against
    // Wikipedia, learns). It IS an LLM call, so it runs ONLY when the LLM is allowed (online-only). In
    // HYBRID the Wikipedia/Wikidata tiers above already learned what was verifiable, with no LLM.
    if (online_llm && nucleo_anima_online_teacher(q, en, &r)) {
        mem_update(&r);
        snprintf(s_mem.last_topic, sizeof(s_mem.last_topic), "%s", q);
        s_session.dirty = true;
        goto done;
    }
    // GROK — "save-the-day" LLM fallback. ONLY when the LLM is allowed (online-only). HYBRID has no LLM:
    // a remaining miss after L1 + wiki is an honest "non lo so".
    if (r.tier == ANIMA_TIER_NONE && online_llm &&
        nucleo_anima_online_chat_ctx(q, ctx, nctx, en, &r)) {
        mem_update(&r); s_session.dirty = true; goto done;
    }
    // honest "non lo so": r already holds the NONE result from try_cascade.

done: {
        g_anima_stage = 0; g_anima_phase = 0;  // DIAG: query returned cleanly (no crash this turn)
        a_strip_foreign(r.reply);              // universal: clean foreign-script clutter even from old learned cards
        if (replayed && r.action != ANIMA_ACT_NONE) { r.from_memory = 1; snprintf(r.state, sizeof(r.state), "followup"); }
        const char *domain = a_domain(&r);
        // Visible reasoning trace. A multi-step agent turn (compose-then-act) joins its steps with
        // " > " — the marker both UIs use to render a Claude-Code-style ⎿ plan. A single-tier answer
        // gets a one-line summary joined with " | " instead, so it's never mistaken for a plan.
        if (s_trace[0]) snprintf(r.trace, sizeof(r.trace), "%s", s_trace);
        else {
            const char *tn = r.tier == ANIMA_TIER_COMMAND ? "L0" : r.tier == ANIMA_TIER_FACT ? "L1" :
                             r.tier == ANIMA_TIER_REMOTE  ? "web" : r.tier == ANIMA_TIER_NONE ? "-" : "L2";
            if (r.budget > 0) snprintf(r.trace, sizeof(r.trace), "%s %s | %dcl | %d%%", tn, domain, r.budget, r.confidence);
            else              snprintf(r.trace, sizeof(r.trace), "%s %s | %d%%", tn, domain, r.confidence);
        }
        // Remember the input only for turns that did something, so it can be replayed; a miss or
        // an unanswered clarify is not a replay target.
        bool actionable = r.action != ANIMA_ACT_NONE && !r.awaiting && strcmp(domain, "clarify") != 0;
        ring_push(actionable ? q : "", r.intent, domain, r.arg);
        // Remember this turn for a follow-up "sei sicuro?" — including a MISS (so "sei sicuro?" after
        // an honest "non lo so" reports the miss, not a stale earlier answer). A dialogue-act turn must
        // NOT overwrite it, so a meta-question always refers to the real previous substantive turn.
        if (strcmp(r.intent,"sure") && strcmp(r.intent,"recap") &&
            strcmp(r.intent,"thanks") && strcmp(r.intent,"deny") && strcmp(r.intent,"explain")) {
            snprintf(s_session.last.reply, sizeof(s_session.last.reply), "%s", r.reply);
            s_session.last.tier = r.tier; s_session.last.conf = r.confidence;
            snprintf(s_session.last.intent, sizeof(s_session.last.intent), "%s", r.intent);
            // Append to the online-context transcript too, so the cloud teacher sees a real multi-turn
            // dialogue next time (a no-op for empty answers — a miss carries nothing to replay).
            if (r.tier != ANIMA_TIER_NONE) chat_push(q, r.reply);
        }
        // Capture the conversational FOCUS from the QUERY's structure, whichever tier answered (a capital
        // fact often comes from an L1 card, not the reasoner). A bare follow-up ("e newton?") is not itself
        // a fact question -> detect returns false -> the focus set by the shift's foc_remember stands.
        if (r.tier != ANIMA_TIER_NONE && r.action == ANIMA_ACT_ANSWER && !r.awaiting) {
            char drel[24], dsubj[48];
            if (nucleo_anima_hdc_detect(q, drel, sizeof drel, dsubj, sizeof dsubj)) {
                snprintf(s_session.foc_subject,  sizeof s_session.foc_subject,  "%s", dsubj);
                snprintf(s_session.foc_relation, sizeof s_session.foc_relation, "%s", drel);
                s_session.foc_turn = s_session.turn;
            }
        }
        telemetry_log(q, &r, domain);          // offline-learning work-list (misses + L1 only)
        session_save();                        // persist context if it changed
        diag_count(&r);                        // cumulative tier/abstain telemetry for /api/diag (cheap)
        return r;
    }
}

// ---- cross-substrate grounded verification (ANIMA Forge) -------------------------------------
// Normalized substring test: lowercase ASCII, drop punctuation, fold accented bytes to spaces, then
// substring-match. Enough to tell "Parigi" present in "La capitale è Parigi." from a wrong "Lione".
static bool vf_norm_has(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    char h[640], n[160]; int hi = 0, ni = 0;
    for (const char *p = hay; *p && hi < (int)sizeof(h) - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) h[hi++] = (char)c; else h[hi++] = ' ';
    }
    h[hi] = 0;
    for (const char *p = needle; *p && ni < (int)sizeof(n) - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) n[ni++] = (char)c; else n[ni++] = ' ';
    }
    n[ni] = 0;
    char *ns = n; while (*ns == ' ') ns++;
    int nl = (int)strlen(ns); while (nl > 0 && ns[nl - 1] == ' ') ns[--nl] = 0;
    if (!nl) return false;
    return strstr(h, ns) != NULL;
}

anima_verify_t nucleo_anima_verify_claim(const char *kind, const char *key, const char *asserted,
                                         const char *lang, char *evidence, int evcap)
{
    if (evidence && evcap > 0) evidence[0] = 0;
    if (!kind || !key) return ANIMA_VERIFY_UNKNOWN;
    bool en = (lang && (lang[0] == 'e' || lang[0] == 'E'));

    // NUMERIC: re-derive the expression on the device's exact math engine and compare.
    if (!strcmp(kind, "numeric")) {
        double v;
        if (a_try_calc(key, &v) != 1) return ANIMA_VERIFY_UNKNOWN;     // not computable -> abstain
        char got[40]; a_fmt_num(v, got, sizeof got);
        if (evidence) snprintf(evidence, (size_t)evcap, "%s", got);
        double a;
        if (!asserted || sscanf(asserted, "%lf", &a) != 1) return ANIMA_VERIFY_UNKNOWN;
        double d = v - a; if (d < 0) d = -d;
        double scale = (v < 0 ? -v : v); if (scale < 1) scale = 1;
        return (d <= 1e-9 * scale) ? ANIMA_VERIFY_CONFIRMED : ANIMA_VERIFY_CONTRADICTED;
    }

    // FACT: ask the grounded brain the question; abstain if it has no confident answer (-> caller
    // WARNs), confirm if its answer CONTAINS the asserted value, else contradict (LENS-style veto).
    if (!strcmp(kind, "fact")) {
        anima_result_t r; memset(&r, 0, sizeof r);
        bool ok = nucleo_anima_hdc_reason(key, en ? "en" : "it", &r) && r.reply[0];
        if (!ok) { memset(&r, 0, sizeof r); ok = nucleo_anima_l1_query(key, en, false, &r) && r.reply[0] && r.confidence >= 78; }
        if (!ok) return ANIMA_VERIFY_UNKNOWN;
        if (evidence) snprintf(evidence, (size_t)evcap, "%s", r.reply);
        if (!asserted || !asserted[0]) return ANIMA_VERIFY_UNKNOWN;
        return vf_norm_has(r.reply, asserted) ? ANIMA_VERIFY_CONFIRMED : ANIMA_VERIFY_CONTRADICTED;
    }
    return ANIMA_VERIFY_UNKNOWN;
}
