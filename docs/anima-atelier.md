# ANIMA Atelier — on-device, sketch-conditioned image generation

A pocket OS on an ESP32 that acts as the **weight distributor** for a diffusion model that runs on the
**client browser's GPU** (WebGPU). You draw a sketch in the Cardputer's Paint app → a **Stable
Diffusion XS with a sketch ControlNet** turns it into an image on the phone/PC → the image comes back saved
to the Cardputer's SD. No cloud, no PSRAM, ANIMA's identity intact (generation is isolated in
Paint; the chat does not generate). The same opt-in philosophy as the WebLLM tier and the Vosk rig with 7 MB chunks.

## Model (verified)
- **`IDKiro/sdxs-512-dreamshaper-sketch`** — a 512×512 **sketch→image** ControlNet, **openrail++** licence
  (re-hostable). ONNX export: **`github.com/lsb/sdxs-controlnet-sketch`** (~430 MB).
- 4 ONNX components: CLIP ViT-L/14 (text encoder), ControlNet, UNet, VAE decoder + `tokenizer.json`.
- **Runtime = ONNX Runtime Web on WebGPU** (WASM fallback) — NOT WebLLM's MLC/TVM runtime.
- **One-step denoising** ⇒ fast (~0.5 s on a desktop GPU) and **deterministic per seed** ("near
  deterministic"): same prompt + same seed ⇒ same image.

## Architecture (who owns what)
```
ANIMA web app                           Paint
 ├─ Model Manager (/modelli)            ├─ Atelier (modal ✨), 3 modes:
 │   catalog + one-at-a-time download   │   Text→Image · Sketch→Image · From image
 │   resumable (SHA/shard + SD index)   ├─ engine: real SDXS (if provided+WebGPU) | MockEngine
 │   forge/model-fetch + model-io       │          (deterministic procedural preview)
 │                                      ├─ saves to /data/Pictures/atelier-<seed>.png
 └─ chat: "genera un'immagine" →        └─ keyboard isolation: the prompt does not trigger b/l/r/e
     decline + "Apri Paint" card
firmware nucleo_anima.c
 └─ tool_image_gen: grounded decline + redirect to Paint (verb∧image-noun, 0 collisions)
```

### Skill isolation (requirement #1)
- **Firmware** (`nucleo_anima.c`): `a_is_image_gen` (a generation verb **∧** an image noun, minus
  note/file/event, minus a question) + `tool_image_gen` FIRST in `TOOLS[]` → "genera/disegna un'immagine di X"
  ("generate/draw an image of X") gets a grounded decline that names Paint, and cannot be mistaken for
  `open_app(paint)`. Added `is_image_gen` to the weather guard `wx_req` (so "draw an image of a SNOWY
  mountain" does not end up at the weather).
- **Simulator** (`tools/serve-shell.mjs`): the same decline, `amatch` faithful to `a_match` (gap≤2).
- **Web app** (`apps/anima/www/index.html`): on `intent==='image_gen'` it shows the "Apri Paint" ("Open Paint") card.
- **Gate**: `tools/anima/eval_paint_decline.jsonl` (39) → `skill-isolation (paint)`. 39/39, 0 collisions with
  open_app/create_file/translate/how-to/weather.

## Resumable download (forge)
- `forge/model-fetch.js` — the real downloader: **one download at a time** (a tab-global lock), shards = 7 MB
  pieces (the resume granularity), **SHA-256 over the real bytes** before trusting/persisting, **cross-reload
  resume** (localStorage cache ∩ the index on SD), pauses for the MCU scheduler. Reuses the PURE core of
  `download.js` (planRanges/reduce/verifyManifest/adaptiveWindow) and `scheduler.js`.
- `forge/model-io.js` — adapters: CDN-Range, WebCrypto-SHA, SD sink (raw POST, chunked), `/api/status`
  telemetry, `makeResumeStore` (localStorage + the index on SD; a shard is "done" only if the two agree).
- `forge/sd-model-loader.js` — reassembles the `.NNN` parts on SD into an `ArrayBuffer` for ORT (generalizes the Vosk rig:
  retry/backoff, a missing part = a hard error, optional per-part SHA).
- `registry/model-catalog.json` — the catalog the Model Manager reads.

## Engine (Paint)
- `diffusion/diffusion-engine.js` — `makeDiffusionEngine` (a **config-driven** ONNX pipeline: tensor names from the
  manifest, injected sessions) **and** `makeMockEngine` (deterministic procedural, sketch→image, no
  weights/WebGPU). The same interface → the real SDXS is a drop-in.
- `diffusion/clip-tokenizer.js` — CLIP ViT-L/14 BPE (vocab+merges from `tokenizer.json`).
- `diffusion/webgpu-probe.js` — picks WebGPU/WASM and estimates the VRAM (honest, no hard block at 3.5 GB).

## Determinism
A fixed seed ⇒ an identical image (one-step + mulberry32 RNG + seeded latent). A 🎲 button for a random
seed. The MockEngine is deterministic too (hash of the prompt + seed), so the UX is reproducible and
testable from day one.

## Host tests (deterministic, in the gate via `--test tools/**/*.test.mjs`)
- `forge-model-fetch.test.mjs` (11) · `forge-sd-model-loader.test.mjs` · `forge-diffusion-engine.test.mjs` (6)
- the `skill-isolation (paint)` gate (39) via `skill-probe` on `eval_paint_decline.jsonl`.

## Honest limits / pending on hardware
- WebGPU and the 430 MB of weights CANNOT run in CI: the **logic** is gated host-side (tokenizer, pipeline,
  seed determinism, download/resume, save flow); the **real GPU generation** is yours to verify on the device.
- Vendor `onnxruntime-web` into `apps/paint/www/vendor/onnxruntime-web/`; provide the weights with `tools/sdxs/`.
- Firmware build for the device: the C is compiled/validated on the host (gcc, via the gate); confirm with
  `flash.ps1 -BuildOnly` (xtensa).
- Deploy: `deploy.ps1` regenerates the `.gz` files of apps/anima and apps/paint. The shell is not touched → no
  `sw.js` bump.
