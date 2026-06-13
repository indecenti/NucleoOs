# Voce offline (TTS concatenativo)

Sintesi vocale **offline** sul Cardputer (ESP32-S3, **niente PSRAM**, ~512 KB SRAM). Niente sintesi
fonemica a bordo (PicoTTS vuole ~1.1 MB, serve PSRAM; eSpeak è al limite e robotico): si
**concatenano clip pre-vocalizzate**. È il gemello vocale di MOSAICO: *grounded by construction*.

## Idea
ANIMA offline **non genera** testo, **recupera** da un insieme finito → il suo parlato è
**pre-vocalizzabile**. Sul PC (una volta) si generano le clip con un TTS di qualità; a runtime il
device **incolla** i pezzi giusti. Voce naturale, RAM ~zero, completamente offline.

## Cosa pronuncia (policy)
Il device parla SOLO le risposte **on-device** sensate da dire a voce:
- ✅ comandi/L0, stato (ore/batteria/data/spazio), conferme di lancio ("Apro musica"), risposte fisse.
- ❌ **conoscenza** (tier FACT/STITCH/REMOTE) e **calcolatrice** (intent `calc`) → suona invece la
  frase "**leggila sullo schermo**" (clip `read_it`).
- ❌ testo **troppo lungo** (>140 char) o che **sa di codice** (backtick/graffe/tag/operatori/keyword
  o densità >12% di caratteri tecnici) → "leggila".
- ❌ parola **non coperta** da clip → "leggila" (mai spelling/lettura sbagliata).
- L'**online** è un discorso separato (il browser usa `speechSynthesis`; il device non vocalizza
  l'online).

## Architettura firmware (`firmware/components/nucleo_tts`)
- `nucleo_tts_plan.c` (PURO, testabile): testo IT/EN → token `CLIP`/`PAUSE`/`UNKNOWN`. Espande numeri
  in cardinali per lingua (IT con elisione: *ventitré*, *millenovecento…*; EN *twenty-three*), match a
  **frase greedy** (le frasi comuni vincono sulle parole), `nucleo_tts_text_speakable()` (guardia
  contenuto) e `nucleo_tts_full_slug()` (slug dell'intero testo, == `slugify` della pipeline).
  - `nucleo_tts_speak_time(h, m, lang)` (PURO): compone l'ORARIO in una frase **parlabile esatta al
    minuto** fatta solo di clip del pacchetto — IT *"Sono le 9 e 30" / "…in punto" / "…e un quarto" /
    "…e mezza" / "…meno un quarto" / "Mezzogiorno" / "Mezzanotte"*; EN *"It is 9 30" / "…o'clock" /
    "noon" / "midnight"*. **Mai** `HH:MM` (il `:` diventerebbe una pausa e `:00` un "zero"). Lo stesso
    testo va a schermo e a voce. I call-site (`app_anima.cpp`, `anima_get` in httpd, simulatore
    `serve-shell.mjs`) lo usano con un guard RTC (`now > 2023-01-01`, altrimenti "orologio non impostato").
- `nucleo_tts_index.c` (PURO): retrieval **RAM/CPU/IO-light**. NON 1 file per clip in una cartella FAT
  (lo `stat` sarebbe O(N)): **UN `index.bin`** (slug→offset,len, ORDINATO) + **UN `clips.pcm`** (PCM
  concatenato). Trovare una clip = **ricerca binaria** via `fseek` (~log₂N letture da 56 B), RAM ~zero.
- `nucleo_tts.c`: apre l'indice della lingua, prova il lookup **risposta-fissa-intera → clip** (per le
  frasi oltre le 6 parole del match-frase), altrimenti pianifica e renderizza (CLIP letti dal blob,
  PAUSE = silenzio) in `/sd/data/tts/_say.wav` @rate dell'indice → `nucleo_audio_play()`. Se l'enunciato
  ha un token `UNKNOWN` → suona `read_it`. Interruttore "parla" persistito (`speak.cfg`, default ON).
  - **Anti-click (qualità della concatenazione):** le clip sono *trim-silence* + *loudnorm* → iniziano/
    finiscono a livello d'ampiezza **non nullo**: incollandole crude, ai punti di giunzione c'è un salto =
    un "tic" udibile. `nucleo_tts_declick_chunk()` (PURO, testato) applica un breve **fade lineare** (~16
    campioni @24kHz ≈ 0.7ms) ai due bordi di OGNI clip durante lo streaming di `copy_clip` → le giunzioni
    clip|clip e clip|silenzio passano per ~0 e sono lisce. **Zero RAM** (scala in loco i pochi campioni di
    bordo del buffer già esistente, niente resample, l'IO/FD di `render` resta intatto). Un marker di
    formato (`fmt.ver` / `TTS_RENDER_FMT`) invalida una volta la cache WAV per-slug all'avvio, così anche
    le frasi fisse (`read_it`/identità/"non lo so") si ri-rendono col nuovo trattamento.

Call-site on-device: `app_anima.cpp` (`speak_result`) e `nucleo_voice.c` (PTT, solo senza client web).
Toggle: Settings nativo (riga "Voce") + Settings web (tab IA) via `/api/tts`.

## Formato file (su SD: `/sd/data/tts/<lang>/`)
- `index.bin`: `"NTI1"` | u32 rate | u32 count | count record ORDINATI `{ char slug[48]; u32 off; u32 len }`.
- `clips.pcm`: PCM mono 16-bit @rate, tutte le clip concatenate (niente header per-clip).
- `speak.cfg` / `_say.wav`: scritti a runtime dal firmware.
Sul device vanno **solo 2 file per lingua** (FAT-friendly), non decine di migliaia.

## RAM sul device (verificata — audit avversariale)
Il design è **RAM-light per costruzione**: l'indice NON è in RAM (ricerca binaria via `fseek`), il blob
è in **streaming** (buffer fisso da 1KB), nessuna struttura grande residente → il TTS aggiunge **~0**
al budget idle (il punto critico del Cardputer, no-PSRAM).
- **Stack**: picco ~6.6 KB di una `nucleo_tts_say()` (`norm[1024]` + `u[96]≈5.4KB` nel planner). Gli
  stack dei task che la chiamano: ANIMA worker **30 KB**, voice task **16 KB** → 59–78% libero.
  La ricorsione di `emit_int_it/en` è **limitata a 3 livelli** (~72 B), con fallback cifra-per-cifra
  oltre il milione → nessun rischio di overflow.
- **Heap**: unica alloc dinamica = il token array `malloc(96 × ~56 = 5.4 KB)`, liberato subito dopo.
  Avviene SOLO nel path **offline-comando** (conoscenza/calcolatrice vanno a `read_hint`, senza malloc)
  → heap sano (~31 KB largest block) → margine ampio; se fallisse, `say()` ritorna false (niente crash).
- **File**: al picco 3 handle (index + blob + _say.wav), tutti chiusi su OGNI return (`render` gestisce
  blob/out; il chiamante chiude sempre l'indice prestato) → nessun leak.
- **.bss**: trascurabile (pochi byte di stato). I buffer sono dimensionati dalla guardia 140-char.

## Pipeline (PC, `tools/nucleo-tts/`)
- **Motore:** `gen_edge.py` — **Edge-TTS** (voci neurali Microsoft, gratis, online solo durante la gen;
  IT=Elsa / EN=Aria), **24 kHz**, **trim del silenzio** ai bordi + loudnorm. Async/concorrente/resumable.
  Alternativa **locale/offline**: `build_voice.py` (engine piper/espeak/pico) — fallback se serve gen
  100% offline.
- **Vocabolario (modulo condiviso `build_voice.py`):** quattro strati, deduplicati: pacchetto
  obbligatorio (cardinali IT/EN generati in codice + `read_it`); `lexicon.<lang>.txt` (curato, frasi/
  parole reali di ANIMA); `lexicon.wf.<lang>.txt` (autorato dal workflow `tts-coverage`: ~470 voci
  IT/EN *grounded* sul parlato reale, verificate avversarialmente); `freq.<lang>.txt` (le ~3000 parole
  più frequenti da OpenSubtitles → coniugazioni per uso). `slugify` **folda gli accenti come il
  firmware** (lo slug DEVE combaciare) e `sanitize_text` toglie ciò che il TTS leggerebbe male.
- **Lessico mirato:** il set è puntato al **parlato REALE di ANIMA** (estratto dal firmware), non al
  dizionario intero — così il blob resta piccolo (~decine/centinaia di MB) e il `fseek` è immediato.
- **Pack:** `build_index.py` impacchetta i `.wav` in `index.bin`+`clips.pcm` (ordine byte == `strcmp` del
  device). `gen_enrich.ps1` / `gen_all.ps1` orchestrano gen→pack→(copia su H: con `-CopyToSd`).

## Test
- `npm run anima:tts` (anche dentro `npm run anima:gate`): compila la C reale del firmware con MinGW e
  verifica il **planner** (IT/EN: numeri/date/decimali, frasi, UNKNOWN, guardia codice/lunghezza,
  full_slug, mathspeak, eq_result, traduttore, **confini decompose**, **anti-click** incl. equivalenza
  per-chunk) e l'**indice** (ricerca binaria). 87/87 + 11/11. Nessun hardware.
- **ANTI-REGRESSIONE PARTE DELLA BUILD:** il ctest del planner non è solo un comando a parte — il
  `CMakeLists.txt` di `nucleo_tts` lo compila+esegue **dentro `idf.py build`** (via `find_program(gcc)` +
  `cmake -E env PATH=<mingw bin>` perché gcc carica le sue DLL da lì). Se la logica testo→clip regredisce,
  il **firmware NON si costruisce** (il gate è una dipendenza di build del `COMPONENT_LIB`, gira prima del
  link). Stamp + `DEPENDS` → ri-scatta SOLO quando cambiano `nucleo_tts_plan.c` / il ctest / l'header
  (build incrementali di altri componenti = costo zero). **Skip grazioso** se manca gcc host (CI/macchine
  minimali restano verdi; `anima:tts` resta la rete completa con indice/replies/time).
- `npm run anima:tts-replies`: prova END-TO-END che le **risposte reali di ANIMA si scatenino** —
  carica l'indice generato (`deploy/sd-safe/data/tts/<lang>/index.bin`) e per ogni risposta
  (`tools/anima-host/test-replies.<lang>.txt`) replica la decisione di `nucleo_tts_say`: WHOLE-clip /
  SPEAK(lista clip) / READ_IT, stampando le **parole non coperte**. Le risposte calc/codice/lunghe
  vanno (giustamente) in READ_IT; le parlabili devono avere 0 scoperte. SKIP pulito se l'indice non
  è ancora generato.

## Stato / da fare
**RILASCIATO sul ferro** (FLASHATO COM3, ~1.76 MB; pool voce su SD H: IT 12504 / EN 12507 clip, 0
risposte scoperte, 0/1440 minuti scoperti). Live: planner composizionale (compone parole non incise dai
sotto-pezzi), mathspeak (`= % ^ +`), `eq_result`/`has_mathtypo` (geometria/fisica dicono il risultato),
fix homografo traduttore, arrotondamento, WDT pets + FATFS timeout 5s, cache WAV per-slug, **build-gate
anti-regressione**. Scelte di stabilità consapevoli: voce SINCRONA (l'async esauriva FD/heap), niente
refactor FD di `render` senza copertura host. **Da verificare sul ferro**: latenza reale di assemblaggio
dal blob su SD e qualità sullo speakerino. Vedi memoria `nucleo-tts-offline`.

**Non ancora flashato:** **anti-click** sui bordi clip (`nucleo_tts_declick_chunk`, gate 87/87, IO/FD di
`render` intatto, RAM zero). Da confermare **all'orecchio** dopo il flash: le frasi concatenate devono
suonare più lisce ai punti di giunzione (niente "tic"). Reversibile (basta non chiamare il declick in
`copy_clip`). Copertura testuale deterministica già satura (replies 0 scoperte, time 0/1440) → per
*leggere ancora più cose* servirebbe ampliare il pool clip (regen) o lo spelling acronimi (clip lettere).
