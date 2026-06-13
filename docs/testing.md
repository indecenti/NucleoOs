# Testing — il sistema di verifica di NucleoOS / ANIMA

> I test **sono parte integrante del sistema**, e la parte più importante. Devono essere **sempre tutti
> validi e verdi**. Il cuore è la verifica del **linguaggio naturale** (ANIMA): che le skill non pestino
> mai i piedi alla conoscenza, che non si fabbrichino mai risposte, che le richieste reali funzionino.

Tutti i test sono **centralizzati in un'unica fonte di verità**:

```
tools/test-registry.json      ← il catalogo: OGNI test, con categoria, comando, descrizione, flag NL
tools/gen-test-registry.mjs   ← lo genera (da gate.mjs + ogni *.test.mjs + i runner non-in-gate)
```

Da quell'unico file leggono **tutte** le viste: il gate, la CLI e la GUI. Rigenera il catalogo quando
aggiungi/sposti un test: `npm run test:registry`.

## Tre modi per eseguirli

| Vista | Comando | A cosa serve |
|---|---|---|
| **GUI** (cockpit + monitor, **un bottone**) | `npm run test:lab` (Python+Tkinter) | Osservatorio a 3 schede. **Panoramica**: la salute di ANIMA a colpo d'occhio (allucinazioni=0, casi NL coperti, drift routing=0, verdi/totali, salute per categoria, ▲▼ vs run precedente). **Test**: **▶ Esegui tutti** lancia con UN click TUTTI i test offline-sensati (nessun modello web — forge usa MockEngine); anche solo-NL/categoria/selezione/falliti. **Andamento**: sparkline delle metriche su tutte le esecuzioni (ANIMA migliora o peggiora?). |
| **CLI** (catalogo) | `npm run test:anima` · `test:all` · `test:nl` · `node tools/run-tests.mjs --cat <id>` · `--grep <x>` · `--list` | La stessa lista da terminale/CI, con riepilogo verde/rosso. |

> **Solo ANIMA offline (la cascata deterministica, SENZA modelli).** `npm run test:anima` — oppure il
> pulsante **🧠 ANIMA offline** nella GUI — esegue *solo* i 70 test della cascata programmatica M1: le 8
> categorie NL + infrastruttura cascata + knowledge-graph. **Esclude** apposta `forge-webllm` (il substrato
> WebLLM/M4 — anche se mockato riguarda i *modelli*), i test delle app e il build-lint. È il sottoinsieme
> "ANIMA che va offline senza modelli, e solo quella" (flag `anima` nel registro).
| **Gate** (canonico) | `npm run anima:gate` | Il pre-flight obbligatorio: rosso = abort di flash/release. ~43 gate ANIMA + la suite `*.test.mjs`. È il giudice canonico del "tutto verde" (cablato in `flash.ps1`/`release.ps1`). |

GUI, CLI e gate leggono la **stessa unica fonte** (`test-registry.json`): zero liste duplicate. Entrambi GUI
e CLI puliscono lo stato SD prima di ogni test (ermetico) e ricompilano l'exe una volta a inizio batch.

> **Nuovi test inclusi per sempre, in automatico.** Il catalogo si rigenera da `gate.mjs` (ogni gate) e da
> una **scansione di tutti i `*.test.mjs`** del repo. Quindi un nuovo test aggiunto come `*.test.mjs` o
> cablato in `gate.mjs` compare nel cockpit al prossimo `npm run test:registry`, senza tocchi manuali. Solo
> un runner standalone (né `*.test.mjs` né gate) va aggiunto con una riga in `extras` di
> `tools/gen-test-registry.mjs`. Il lint di packaging app (`tools/validate.mjs`) è volutamente **escluso**
> (verifica i manifest delle app, non ANIMA): si esegue a parte con `npm run validate`.

## Categorie

Le categorie mettono il **linguaggio naturale al primo posto** (8 categorie NL 🗣 + 8 di sistema/app):

| Categoria | NL | Cosa garantisce |
|---|:--:|---|
| **NL · Anti-allucinazione** | 🗣 | Trappole avversariali: ogni risposta confidente a una richiesta inananswerabile = fallimento. La sicurezza. (halluc-stress, halluc-battery, metamorph, nl-stress, reliability, false-positives, cross-topic, halluc-probe IT/EN, ood-check) |
| **NL · Routing skill** | 🗣 | La richiesta giusta → lo strumento giusto; la skill **non pesta mai** la conoscenza e viceversa. (boundary, realistic, skill-routing 1/2, cross-skill, action-tier, image-gen, skill-isolation) |
| **NL · Conoscenza & recupero** | 🗣 | Retrieval L1/L2, definizioni, descrizioni, entità: fondate o astensione onesta. (l1-parity, l1-recall, describe-stress, fluency-grounded, regress, typed-nl, entity-detect, clean-extract, akb5-content) |
| **NL · Ragionamento** | 🗣 | Deduzione KGE, recall HDC, composizione combinatoria, facet tipizzati. |
| **NL · Matematica & calcolo** | 🗣 | Ogni risposta matematica esatta + parità col gemello JS. (math-check, calc-eval) |
| **NL · Memoria & profilo** | 🗣 | Apprendimento utente + profilo personale tipato: recall per parafrasi, zero misattribuzioni. |
| **NL · Traduzione** | 🗣 | Traduttore offline IT↔EN via dizionario: fondato, decline onesto, zero falsi positivi. |
| **NL · Meteo** | 🗣 | NLU meteo: estrazione luogo/data, tier offline/ibrida/online. |
| **Cascata · Infrastruttura** | | Orchestratore, loop agentico, coerenza pacchetti indice/encoder. |
| **Knowledge Graph & Ledger** | | Auto-evoluzione tassonomia (Wikidata) + libro mastro verificabile immutabile. |
| **ANIMA Forge / WebLLM (M4)** | | Editor agentico browser: firewall azioni, verifica cross-substrato, download/engine, provenienza (35 test). |
| **App · Foglio di calcolo** | 🗣 | Motore formule Excel-class + NL→formula del copilot. |
| **App · Paint / Shell / Device** | | Imaging, comandi NL, browser, scorciatoie, terminale, USB-HID, NFV. |
| **Build & lint** | | Anti-drift dello spec API (Swagger ↔ route firmware) + freschezza dei `.gz` serviti (`npm run gz:check`: nessun codice vecchio/orfano spedito al device). |

L'elenco **vivo** (con conteggi e comandi esatti) è sempre: `npm run test:list`.

## Monitor di salute (non solo test)

Il cockpit non dà solo verde/rosso: legge **metriche** che ogni runner stampa nel proprio output e le
aggrega in **indicatori di salute di ANIMA**. Sono dichiarati nel registro (`METRICS` + `health` in
`tools/gen-test-registry.mjs`), quindi sono **modulari e scalabili**: un nuovo test che emette `cases`
(richieste NL esercitate) o `halluc` (fabbricazioni/falsi-positivi) **alza da solo** la dashboard.

Indicatori headline:

| Indicatore | Significato | Obiettivo |
|---|---|---|
| **Allucinazioni** | somma di tutte le fabbricazioni/falsi-positivi sulle trappole avversariali | **0** |
| **Casi NL coperti** | quante richieste in linguaggio naturale ANIMA è verificata a gestire (somma di tutte le suite NL) | più alto è, meglio è (oggi ~6500) |
| **Drift routing** | cambi di instradamento rispetto allo snapshot golden | **0** |
| **Test verdi** | passati / totali (esclusi gli skip intenzionali) | tutti |

Ogni esecuzione completa (GUI **▶ Esegui tutti** o `npm run test:all`) **registra uno snapshot** in
`tools/test-lab/history.jsonl`; la scheda **Andamento** e la sezione `SALUTE ANIMA` della CLI lo usano per i
trend. Aggiungere una metrica = una riga nella mappa `METRICS` del generatore.

## La parte in linguaggio naturale (il cuore)

I test NL girano **solo sulla cascata programmatica** — l'`anima.exe` compilato dal firmware, linkato con
`anima_online_stub.c` (online non disponibile): **nessun modello LLM/generativo nei test**. Una richiesta
che richiederebbe internet o un modello **deve astenersi**, e lo verifichiamo.

Runner NL dedicati (tutti su `tools/anima-host/`), eseguibili anche via `npm`:

| Runner | npm | Cosa prova |
|---|---|---|
| `halluc-probe.mjs` + `halluc-suite.mjs` | `anima:halluc` | Anti-fabbricazione **stretta**: una reply confidente via QUALSIASI tier (anche `L0/command`, un `{value}` trapelato) = allucinazione. 8 file / ~585 trappole. |
| `metamorph.mjs` | `anima:meta` | **Metamorfico**: 485 frasi seed × 8 trasformazioni semantica-preservante (typo/CAPS/accenti/cortesia/filler/code-switch/riordino) = ~2300 mutanti; l'astensione deve essere invariante. Cross-substrato onesto (M1 pieno / M3 detection / M4 verifier). |
| `boundary.mjs` | `anima:boundary` | **Confine skill↔conoscenza** in 3 direzioni: una *definizione* non calcola mai; un *compute* parte sempre; una *conoscenza con esca* non fa partire una skill. 473 casi. |
| `realistic.mjs` | `anima:realistic` | ~310 **richieste reali** (molte con typo QWERTY): skill sotto-testate firano, conoscenza resta fondata o astiene; degradazione sicura su input garbled. |
| `skill-probe.mjs` | (vari `eval_skills*`) | Routing curato cross-skill: la richiesta giusta raggiunge il tool giusto. |
| `nl-stress.mjs` / `describe-stress.mjs` / `ood-check.mjs` | | Stress NL su massa, descrizioni, out-of-scope: 0 allucinazioni. |

## Determinismo e validità

- **Niente RNG non-seedato, niente wall-clock** nei test (la skill `date` è esclusa dai generatori per
  evitare accoppiamento all'orologio). Stesso input → stesso esito, ad ogni riesecuzione.
- **Stato SD pulito prima di OGNI gate** (`clearVolatile()` in `gate.mjs`): unità insegnate, profilo, learn
  ed eventi persistono su SD; pulirli solo all'avvio lasciava un gate inquinare il successivo.
- **Le aspettative sono fondate sul comportamento reale** dell'exe, non indovinate: per una nuova frase si
  esegue il vero exe e si fissa l'esito; per un'allucinazione l'attesa è fissa (astensione) e se non
  astiene è un bug da correggere alla radice.

## Aggiungere un test

1. Scrivi il runner o il file `*.test.mjs`, oppure aggiungi casi a un `eval_*.jsonl` esistente.
2. Se è un nuovo runner ANIMA da rendere obbligatorio, aggiungilo all'array `gates` in
   `tools/anima-host/gate.mjs`.
3. Rigenera il catalogo: `npm run test:registry` (raccoglie automaticamente ogni `*.test.mjs`).
4. Verifica verde: `npm run anima:gate` (canonico) o la GUI `npm run test:lab`.

## Stato

Catalogo: **147 test** in 17 categorie (64 in linguaggio naturale) — rigenerato da `npm run test:registry`,
conteggio sempre vivo con `npm run test:list`. Tutti verdi con un click (`npm run test:lab` → **▶ Esegui
tutti**, oppure `npm run test:all`); gli unici skip si auto-salutano senza setup di rete/SD/device
(`forge-download-manager`, il carico arbiter su device), non sono fallimenti. Il gate canonico resta
verde con `npm run anima:gate`.
