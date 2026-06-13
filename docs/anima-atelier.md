# ANIMA Atelier — generazione immagini on-device, sketch-conditioned

Un OS tascabile su ESP32 che fa da **distributore di pesi** per un modello di diffusione che gira sulla
**GPU del browser del client** (WebGPU). Disegni uno schizzo nell'app Paint del Cardputer → uno **Stable
Diffusion XS con ControlNet sketch** lo trasforma in immagine sul telefono/PC → l'immagine torna salvata
sulla SD del Cardputer. Niente cloud, niente PSRAM, identità di ANIMA intatta (la generazione è isolata in
Paint; la chat non genera). Stessa filosofia opt-in del tier WebLLM e della rig Vosk a chunk da 7 MB.

## Modello (verificato)
- **`IDKiro/sdxs-512-dreamshaper-sketch`** — ControlNet **schizzo→immagine** 512×512, licenza **openrail++**
  (ri-ospitabile). Export ONNX: **`github.com/lsb/sdxs-controlnet-sketch`** (~430 MB).
- 4 componenti ONNX: CLIP ViT-L/14 (text encoder), ControlNet, UNet, VAE-decoder + `tokenizer.json`.
- **Runtime = ONNX Runtime Web su WebGPU** (fallback WASM) — NON il runtime MLC/TVM di WebLLM.
- **Denoising a un passo** ⇒ veloce (~0.5 s su GPU desktop) e **deterministico per seed** ("quasi
  deterministico"): stesso prompt + stesso seed ⇒ stessa immagine.

## Architettura (chi possiede cosa)
```
ANIMA web app                          Paint
 ├─ Model Manager (/modelli)            ├─ Atelier (modal ✨), 3 modi:
 │   catalogo + download 1-alla-volta    │   Testo→Immagine · Schizzo→Immagine · Da immagine
 │   resumibile (SHA/shard + indice SD)   ├─ motore: SDXS reale (se provvisto+WebGPU) | MockEngine
 │   forge/model-fetch + model-io         │            (anteprima procedurale deterministica)
 │                                        ├─ salva in /data/Pictures/atelier-<seed>.png
 └─ chat: "genera un'immagine" →          └─ isolamento tastiera: il prompt non attiva b/l/r/e
     decline + card "Apri Paint"
firmware nucleo_anima.c
 └─ tool_image_gen: decline grounded + redirect a Paint (verbo∧nome-immagine, 0 collisioni)
```

### Isolamento della skill (il requisito #1)
- **Firmware** (`nucleo_anima.c`): `a_is_image_gen` (verbo di generazione **∧** nome-immagine, meno
  nota/file/evento, meno domanda) + `tool_image_gen` PRIMO in `TOOLS[]` → "genera/disegna un'immagine di X"
  riceve un decline grounded che nomina Paint, e non può essere scambiato per `open_app(paint)`. Aggiunto
  `is_image_gen` al guard meteo `wx_req` (così "draw an image of a SNOWY mountain" non finisce al meteo).
- **Simulatore** (`tools/serve-shell.mjs`): stesso decline, `amatch` fedele a `a_match` (gap≤2).
- **Web app** (`apps/anima/www/index.html`): su `intent==='image_gen'` mostra la card "Apri Paint".
- **Gate**: `tools/anima/eval_paint_decline.jsonl` (39) → `skill-isolation (paint)`. 39/39, 0 collisioni con
  open_app/create_file/translate/how-to/meteo.

## Download resumibile (forge)
- `forge/model-fetch.js` — downloader reale: **un download alla volta** (lock globale del tab), shard = pezzi
  da 7 MB (granularità di resume), **SHA-256 sui byte reali** prima di fidarsi/persistere, **resume
  cross-reload** (cache localStorage ∩ indice su SD), pausa per lo scheduler MCU. Riusa il core PURO di
  `download.js` (planRanges/reduce/verifyManifest/adaptiveWindow) e `scheduler.js`.
- `forge/model-io.js` — adattatori: CDN-Range, WebCrypto-SHA, sink SD (POST raw, chunked), telemetria
  `/api/status`, `makeResumeStore` (localStorage + indice su SD; lo shard è "fatto" solo se concordano).
- `forge/sd-model-loader.js` — ricompone i `.NNN` su SD in un `ArrayBuffer` per ORT (generalizza la rig Vosk:
  retry/backoff, parte mancante = errore duro, SHA per-parte opzionale).
- `registry/model-catalog.json` — il catalogo che il Model Manager legge.

## Motore (Paint)
- `diffusion/diffusion-engine.js` — `makeDiffusionEngine` (pipeline ONNX **config-driven**: nomi tensori dal
  manifest, sessioni iniettate) **e** `makeMockEngine` (procedurale deterministico, sketch→immagine, niente
  pesi/WebGPU). Stessa interfaccia → SDXS reale è un drop-in.
- `diffusion/clip-tokenizer.js` — BPE CLIP ViT-L/14 (vocab+merges dal `tokenizer.json`).
- `diffusion/webgpu-probe.js` — sceglie WebGPU/WASM e stima la VRAM (onesto, niente blocco a 3.5 GB).

## Determinismo
Seed fisso ⇒ immagine identica (one-step + RNG mulberry32 + latente seedato). Pulsante 🎲 per il seed
casuale. Il MockEngine è anch'esso deterministico (hash del prompt + seed), così l'UX è riproducibile e
testabile fin da subito.

## Test host (deterministici, nel gate via `--test tools/**/*.test.mjs`)
- `forge-model-fetch.test.mjs` (11) · `forge-sd-model-loader.test.mjs` · `forge-diffusion-engine.test.mjs` (6)
- `skill-isolation (paint)` gate (39) via `skill-probe` su `eval_paint_decline.jsonl`.

## Limiti onesti / pendente su hardware
- WebGPU e i 430 MB di pesi NON sono eseguibili in CI: la **logica** è gated host-side (tokenizer, pipeline,
  determinismo-seed, download/resume, save flow); la **generazione reale su GPU** la verifichi tu sul device.
- Vendora `onnxruntime-web` in `apps/paint/www/vendor/onnxruntime-web/`; provvedi i pesi con `tools/sdxs/`.
- Build firmware per il device: il C è compilato/validato host (gcc, via il gate); conferma con
  `flash.ps1 -BuildOnly` (xtensa).
- Deploy: `deploy.ps1` rigenera i `.gz` di apps/anima e apps/paint. Lo shell non è toccato → niente bump
  `sw.js`.
