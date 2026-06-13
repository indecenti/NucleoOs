# NucleoMind

**Il nostro Ollama per Android.** Esegue piccoli LLM *on-device* sul telefono e li
serve in Wi‑Fi con un'API compatibile OpenAI, scoperta automaticamente via mDNS.
Pensato come terzo "cervello" di NucleoOS / ANIMA — il lobo che il Cardputer
prende in prestito quando il telefono è nei paraggi — ma **standalone e riusabile
da qualsiasi client**.

## Cosa fa

- **Foreground service** + wake lock: tiene vivo il server quando l'app è in background.
- **Server HTTP** (`0.0.0.0:8080`) con endpoint OpenAI-like:
  - `GET  /health` — stato e modello caricato
  - `GET  /metrics` — uptime, RAM, batteria, temperatura, tok/s, throttle
  - `GET  /v1/models` — modello corrente
  - `POST /v1/chat/completions` — chat, con `"stream": true` (SSE) o risposta unica
  - `POST /v1/distill` — **endpoint teacher**: dato un argomento, restituisce una
    card fattuale bilingue pronta per il flywheel di conoscenza di ANIMA
  - `POST /v1/ground` — **split-inference**: il client invia `question` +
    `evidence[]` già recuperata; il telefono sintetizza SOLO da quella
    (anti-allucinazione by-construction). Il grounding resta sul client.
- **mDNS / DNS-SD**: si annuncia come `_anima._tcp` col nome `NucleoMind`.
- **Catalogo modelli + download automatico**: lista curata di LLM 1–3B compatibili
  MediaPipe, con download ripristinabile (HTTP Range), uno alla volta, token HF
  facoltativo per i repo gated. Sempre disponibile anche "Da file".
- **Raccomandazione per il device**: legge la RAM totale e suggerisce il modello
  più grande che ci sta.
- **Guard "respirazione del sistema"**: se la batteria è bassa (e non in carica) o
  il telefono scotta, il server resta vivo ma declina il lavoro pesante (503) e
  riprende da solo quando le condizioni migliorano.
- **QR di pairing** dell'URL del server, per connettere un client al volo.
- **Banco di prova** in-app per chattare col modello senza uscire dall'app.
- **Motore di fallback "echo"**: l'app parte e risponde anche **senza modello**.
- **Motore reale**: MediaPipe LLM Inference (Gemma & co., file `.task`/`.bin`).

### Novità v2.0
- **Impostazioni persistenti**: porta, parametri di generazione (temperatura,
  top-K, max token), system prompt — sopravvivono al riavvio.
- **Avvio automatico**: all'apertura dell'app e/o al boot del telefono; **ricarica
  l'ultimo modello** da sola. (Boot su Android 14+ può essere limitato dal sistema.)
- **Sicurezza**: API key opzionale (`Authorization: Bearer` o `X-API-Key`) sugli
  endpoint generativi; vuota = aperto in LAN.
- **`/v1/ground`** (split-inference) e **`/v1/distill`** (teacher).
- **Storico richieste** (endpoint, latenza, token) nel Monitor.

### UI
Cinque schede: **Server** (stato, QR, endpoint, log), **Modelli** (catalogo,
download, installati), **Monitor** (RAM, batteria, termico, uptime, tok/s,
storico), **Prova** (playground locale), **Opzioni** (impostazioni).

### Esempio grounded (split-inference)
```bash
curl http://IP:8080/v1/ground -H 'Content-Type: application/json' -d '{
  "question": "Quando è nato Newton?",
  "evidence": ["Isaac Newton nacque il 25 dicembre 1642 a Woolsthorpe."],
  "lang": "it"
}'
# → {"answer":"...","grounded":true,"evidence_count":1}
```

## Build

Richiede Android Studio (Ladybug+) o JDK 17 + Android SDK 35.

```bash
# da Android Studio: apri la cartella nucleomind/ e fai "Sync".
# da CLI (dopo aver generato il wrapper jar con `gradle wrapper`):
./gradlew :app:assembleDebug
# APK in app/build/outputs/apk/debug/app-debug.apk
```

> Il `gradle-wrapper.jar` non è incluso: Android Studio lo rigenera al primo
> sync, oppure esegui `gradle wrapper` una volta.

## Uso

1. Apri l'app, concedi la notifica.
2. **Carica modello** → scegli un `.task`/`.bin` MediaPipe (es. Gemma 2B/3 1B int4).
   Viene copiato nello storage privato e caricato. Senza modello resta in echo.
3. **Avvia server**. L'app mostra `http://IP:porta`.
4. I client si connettono a quell'IP, o scoprono `_anima._tcp` via mDNS.

### Prova rapida

```bash
curl http://IP:8080/health
curl http://IP:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Ciao"}]}'
```

## Note tecniche

- `minSdk 26`, `targetSdk 35`. Foreground service tipo `dataSync`.
- La versione di `com.google.mediapipe:tasks-genai` (0.10.24) è sensibile: se
  cambia l'API del runtime, l'unico file da toccare è
  `llm/MediaPipeEngine.kt` — il resto è agnostico dal motore.
- Modello di rete: telefono e client sulla **stessa rete** (Wi‑Fi o hotspot del
  telefono). In hotspot il client deve agganciarsi alla rete del telefono.

## Integrazione col Cardputer (prossimo passo)

Lato firmware basta riusare il client del tier *online* di ANIMA puntandolo a
`http://<ip-telefono>:8080/v1/chat/completions` — è la stessa shape. Il valore
aggiunto è la **probe mDNS** all'avvio per popolare il tier "LAN" da solo.
