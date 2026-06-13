// nucleo_tts — voce OFFLINE on-device per concatenazione di clip pre-renderizzate.
//
// PERCHE' COSI': il Cardputer (ESP32-S3, NIENTE PSRAM, ~512KB SRAM) non puo' sintetizzare
// fonemi in tempo reale — PicoTTS vuole ~1.1MB di RAM anche con risorse mmap (serve PSRAM),
// eSpeak ~120KB al limite assoluto. MA ANIMA offline NON e' un LLM generativo: e' una cascata
// di retrieval su un corpus FINITO, noto a build-time. Quindi pre-vocalizziamo offline (sul PC,
// con un TTS di qualita') il corpus + le parole a classe chiusa (numeri/date/unita'/connettivi),
// e a runtime CONCATENIAMO le clip. Voce naturale, RAM ~zero, offline e standalone puro. E' il
// gemello vocale di MOSAICO: "grounded by construction".
//
// FLUSSO: testo -> nucleo_tts_plan() (questo modulo, puro+testabile) -> sequenza di token CLIP/PAUSE
// -> nucleo_tts_say() assembla i PCM delle clip in UN solo WAV temporaneo e lo suona via nucleo_audio
// (il path audio battle-tested non viene toccato). Le clip vivono su SD (/sd/data/tts/it/<slug>.wav),
// NON nel flash (pref: niente asset bakeabili nell'immagine).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- planner (puro, host-compilabile: niente ESP/SD) -----------------------------------------

typedef enum {
    TTS_TOK_CLIP = 0,   // suona la clip identificata da `slug` (parola/frase/numero)
    TTS_TOK_PAUSE,      // inserisci `ms` di silenzio (punteggiatura/prosodia)
    TTS_TOK_UNKNOWN,    // parola NON coperta da nessuna clip -> l'enunciato non e' pronunciabile
                        // pulito: nucleo_tts_say() ripiega sulla frase "leggila sullo schermo".
} tts_tok_kind_t;

typedef struct {
    tts_tok_kind_t kind;
    char slug[48];      // CLIP: slug della clip -> file /sd/data/tts/<lang>/<slug>.wav
    int  ms;            // PAUSE: durata silenzio in ms
    bool fallback;      // CLIP: 1 se prodotto da fallback (es. spelling lettera-per-lettera)
} tts_token_t;

// Callback: vero se lo slug ha una clip renderizzata. Permette al planner di degradare con grazia
// (spelling) su parole sconosciute. `ud` e' opaco (es. la lingua). I numeri/connettivi del
// "pacchetto obbligatorio" il planner li emette comunque (build_voice.py li genera sempre).
typedef bool (*tts_has_clip_fn)(const char *slug, void *ud);

// Pianifica un'enunciazione in una sequenza di token. Normalizza (lowercase, fold accenti), espande
// numeri/decimali/segno nei cardinali della lingua (lang "en" -> inglese, altrimenti italiano),
// prova un match a FRASE (greedy, MOSAICO-style) PRIMA del match a parola — cosi' le frasi comuni
// hanno precedenza — e per le parole ignote ripiega sullo spelling. Ritorna i token scritti (<= max).
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max);

// Slug dell'intero `text` (fold accenti + minuscolo + parole unite da '_', troncato a cap-1) — IDENTICO
// a slugify() di build_voice.py. Serve al lookup "risposta fissa intera -> clip unica" (identita',
// "non lo so", pairing...): cosi' suonano con prosodia naturale, oltre il limite di 6 parole del planner.
void nucleo_tts_full_slug(const char *text, char *out, int cap);

// Guardia di contenuto (OFFLINE): vero solo se `text` e' SENSATO da pronunciare a voce. Falso se e'
// troppo lungo (> max_chars, 0 = nessun limite) o se "sa di codice"/markup (backtick, graffe, tag,
// operatori, parole-chiave, alta densita' di caratteri tecnici). I call-site, quando e' falso, suonano
// "leggila sullo schermo" invece di leggere porzioni di codice o cose incomprensibili. Pura/testabile.
bool nucleo_tts_text_speakable(const char *text, int max_chars);

// Compone l'ORARIO (h 0..23, m 0..59) in una frase ESATTA AL MINUTO fatta SOLO di clip del pacchetto
// (numeri 0..99 + "sono le"/"e"/"in punto"/"e un quarto"/"e mezza"/"meno un quarto"/"mezzogiorno"/
// "mezzanotte" per l'IT; "it is"/"o'clock"/"noon"/"midnight" per l'EN). 24h. NIENTE ":"/zero-padding
// (che il planner leggerebbe come pausa/"zero"). Lo stesso testo va a SCHERMO e a VOCE, quindi resta
// leggibile. lang "en" -> inglese, altrimenti italiano. Pura/testabile (vedi tts-time-ctest.c).
void nucleo_tts_speak_time(char *out, int n, int h, int m, const char *lang);

// "Parlabilizza" i simboli matematici delle risposte del solver: '=' -> "uguale a"/"equals",
// '%' -> "per cento"/"percent", '^' -> "elevato"/"to the power of". Senza, '=' farebbe scattare la
// guardia "sa di codice" (tutto in "leggila") e '%'/'^' verrebbero scartati (si perde il senso). Gli
// altri simboli (/ · ² ³ ( ) π) restano: se presenti, l'enunciato cade in fallback/"leggila" (ohm,
// geometria). nucleo_tts_say()/say_or() la applicano gia' internamente. Pura/testabile (tts-plan-ctest).
void nucleo_tts_mathspeak(const char *in, char *out, int n, const char *lang);

// Estrae la PRIMA frase (gist) di `in` in `out` (max n). INNOVAZIONE "voce a mosaico": le risposte
// descrittive/di conoscenza sono lunghe -> invece di "leggila" si pronuncia il gist breve (la prima
// frase, di solito la definizione). nucleo_tts_say() poi la dice se coperta dal pool, altrimenti legge.
// Pura/testabile (tts-plan-ctest). Non termina su decimali ("3.14") ne' su sigle ("Dr.").
void nucleo_tts_first_sentence(const char *in, char *out, int n);

// Estrae dalla risposta del traduttore (`"<src>" in <lingua>: <target>.`) la PAROLA tradotta in `word`
// e la sua LINGUA ("en"/"it") in `lang`; ritorna true sulla forma principale. Cosi' la voce pronuncia
// la traduzione con l'indice GIUSTO ("come si dice cane in inglese" -> "dog" in inglese) invece di
// "leggila" (il target e' nell'altra lingua, non coperto dall'indice mono-lingua). Pura/testabile.
bool nucleo_tts_translate_word(const char *reply, char *word, int wn, char *lang, int ln);

// Estrae il RISULTATO numerico dopo l'ultimo "= " di una risposta-formula (geometria/fisica) in `out`,
// se e' un numero PULITO (no unita'/simboli). Cosi' la voce dice "Il risultato e' 78.5398" invece di
// "leggila" sulla formula simbolo-densa. Ritorna true se trovato e pulito. Pura/testabile.
bool nucleo_tts_eq_result(const char *reply, char *out, int n);

// True se `text` ha tipografia matematica densa (· ² ³ √ π Δ ½) = una formula (geometria/fisica). Pura.
bool nucleo_tts_has_mathtypo(const char *text);

// ---- velocita' di lettura (zero-RAM: rate dell'header WAV in uscita) --------------------------
// La voce si accelera/rallenta cambiando il SAMPLE-RATE dichiarato nell'header del WAV assemblato:
// il DAC scandisce gli STESSI campioni piu' in fretta (clip + pause si accorciano insieme) — nessun
// buffer, nessun sample toccato, costo RAM/CPU ZERO. Tape-speed: a >100% sale leggermente il pitch
// (accettabile per "un po' piu' veloce"). La velocita' e' un intero PERCENTUALE (100 = naturale).
#define TTS_SPEED_MIN 70     // 0.70x (piu' lento)
#define TTS_SPEED_MAX 160    // 1.60x (piu' veloce)
#define TTS_SPEED_DEF 110    // default: un filo piu' veloce del naturale (l'utente lo trovava "lentino")
#define TTS_SPEED_STEP 5     // passo consigliato per gli slider/▲▼

// Clampa il percentuale a [TTS_SPEED_MIN, TTS_SPEED_MAX]. Pura/testabile.
int nucleo_tts_speed_clamp(int pct);
// rate I2S in uscita = base * pct/100, col pct clampato e il risultato tenuto in [8000,48000] Hz
// (limiti sani per l'I2S; play_wav rifiuta fuori range). Pura/testabile (vedi tts-plan-ctest.c).
uint32_t nucleo_tts_speed_rate(uint32_t base_rate, int pct);

// ---- anti-click: smussatura dei bordi clip (qualita' della concatenazione) -------------------
// Le clip del pacchetto sono trim-silence + loudnorm -> iniziano/finiscono a un livello d'ampiezza
// NON nullo: incollandole, ai punti di giunzione c'e' un salto = un "tic"/click udibile. La cura
// classica della sintesi concatenativa e' un breve fade lineare ai due bordi di OGNI clip, cosi' la
// giunzione passa per ~0 ed e' liscia. ~16 campioni @24kHz ≈ 0.7ms: azzera il click senza intaccare
// la nitidezza dei suoni. Costo: scala in loco i pochi campioni di bordo del buffer di streaming gia'
// esistente (nessuna RAM, nessun resample, nessun campione "vero" toccato a parte i bordi).
#define TTS_DECLICK_SAMPLES 16

// Applica il fade ai bordi della clip lavorando su UN chunk dello streaming: `buf`/`nbytes` e' il pezzo
// corrente (PCM mono 16-bit LE), `chunk_off` la sua posizione in BYTE dall'inizio della clip, `clip_len`
// i byte totali della clip, `fade` la lunghezza del fade in CAMPIONI. Smussa i primi e gli ultimi `fade`
// campioni della clip ovunque cadano nel chunk -> indipendente dalla frammentazione in chunk. Allineata
// al campione (gestisce chunk a cavallo del bordo di fade); no-op su clip troppo corte. Pura/testabile
// (alignment-safe via byte LE, niente cast a int16*). Vedi tts-plan-ctest.c.
void nucleo_tts_declick_chunk(unsigned char *buf, int nbytes, uint32_t chunk_off, uint32_t clip_len, int fade);

// ---- servizio firmware (solo on-device; vedi nucleo_tts.c) -----------------------------------

// Imposta la lingua di default e verifica che il pacchetto voce sia su SD (stat di n0.wav in
// /sd/data/tts/<lang>/). Idempotente. Niente da caricare in RAM: l'esistenza delle clip si controlla
// a runtime con stat. Ritorna true se la voce per `lang` e' installata.
bool nucleo_tts_init(const char *lang);

// Pronuncia `text` in `lang` (NULL/"" -> lingua di default): pianifica, e SE l'enunciato e'
// interamente coperto da clip (nessuna parola ignota) assembla i PCM in /sd/data/tts/_say.wav e
// suona. ALTRIMENTI (parola non coperta -> leggerebbe male) NON prova: suona la frase canonica
// "leggila sullo schermo" (clip "read_it"). Cosi' la voce non sbaglia MAI: o dice la cosa giusta,
// o invita a leggere. Interrompe l'audio in corso. Non blocca. False se la voce non e' installata.
bool nucleo_tts_say(const char *text, const char *lang);

// Come nucleo_tts_say(), ma se `text` NON e' interamente pronunciabile (contenuto/parola scoperta)
// pronuncia `fallback` (es. "Fatto") invece di "leggila sullo schermo". Per le CONFERME di operazioni
// (aggiungi promemoria, crea file...) dove conta l'ESITO, non il testo esatto a contenuto variabile.
bool nucleo_tts_say_or(const char *text, const char *fallback, const char *lang);

// Suona direttamente la frase "leggila sullo schermo" (clip "read_it"). I call-site la usano per le
// risposte che NON vanno dette a voce (conoscenza, calcolatrice): l'utente sa che c'e' da leggere.
bool nucleo_tts_read_hint(const char *lang);

// True se la voce nella lingua di default e' installata su SD.
bool nucleo_tts_available(void);

// Interruttore "parla quando interrogato dal Cardputer" (default ON). Persistito su SD
// (/sd/data/tts/speak.cfg). Quando OFF, nucleo_tts_say() e' un no-op che ritorna false — cosi' i
// call-site on-device (app ANIMA, voce PTT) non parlano. Lo settano sia il Settings nativo che il
// web (/api/tts). NON tocca il path web del browser (quello usa speechSynthesis lato client).
void nucleo_tts_set_enabled(bool on);
bool nucleo_tts_enabled(void);

// Velocita' di lettura on-device, percentuale (100 = naturale; default TTS_SPEED_DEF). Persistita su
// SD (/sd/data/tts/speed.cfg). Il valore e' clampato a [TTS_SPEED_MIN,TTS_SPEED_MAX]. set() invalida la
// cache WAV per-slug (il rate e' "cotto" nei file): cosi' il nuovo passo vale subito anche sui fissi.
// La impostano TUTTE le superfici (shell web + app Settings + ANIMA web/nativo) via un'unica sorgente.
void nucleo_tts_set_speed(int pct);
int  nucleo_tts_speed(void);

// Ferma la pronuncia in corso (delega a nucleo_audio_stop).
void nucleo_tts_stop(void);

#ifdef __cplusplus
}
#endif
