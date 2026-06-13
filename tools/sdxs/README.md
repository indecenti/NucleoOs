# tools/sdxs — provisioning di Stable Diffusion XS per l'Atelier di Paint

Genera l'immagine **sul dispositivo del client** (GPU del browser, WebGPU) usando il Cardputer come
distributore dei pesi. I pesi **non** stanno in git: li prepari tu sul PC e li sincronizzi su SD.

## Flusso completo

### 1. Ottieni gli ONNX (~430 MB)
Servono 5 file: `text_encoder.onnx`, `controlnet.onnx`, `unet.onnx`, `vae_decoder.onnx`, `tokenizer.json`.

Opzione A — esporta con lo script upstream (richiede Python + torch + optimum):
```
git clone https://github.com/lsb/sdxs-controlnet-sketch
# segui il README del repo per esportare l'ONNX di IDKiro/sdxs-512-dreamshaper-sketch (openrail++)
```
Opzione B — usa ONNX già esportati (se li hai/li scarichi) e mettili in una cartella.

> Verifica i nomi dei tensori del tuo export. Se differiscono dai default, aggiusta il blocco `io`
> nel `manifest.json` generato (o `DEFAULT_IO` in `provision.mjs`). Controllo rapido:
> `python -c "import onnx;print([i.name for i in onnx.load('unet.onnx').graph.input])"`

### 2. Chunk + manifest (Node, deterministico)
```
node tools/sdxs/provision.mjs --in <cartella-onnx> [--out <dir>] [--rev 2025-03]
```
Divide ogni componente in pezzi da **7 MiB** (`file.onnx.000`, `.001`, …), calcola lo **SHA-256** di ogni
pezzo, copia `tokenizer.json`, ed emette `manifest.json` (componenti + parti + `io` + shard).
Default `--out`: `deploy/sd-safe/apps/paint/www/models/sdxs-512-dreamshaper-sketch`.

### 3. Vendora onnxruntime-web (una volta)
Il motore reale usa ONNX Runtime Web su WebGPU. Vendoralo nell'app Paint (offline-first):
```
npm i onnxruntime-web
# copia il dist ESM in: apps/paint/www/vendor/onnxruntime-web/
#   ort.webgpu.min.mjs  +  i .wasm/.mjs richiesti
```
Finché non è presente (o WebGPU manca), l'Atelier usa l'**anteprima procedurale deterministica** e lo dice.

### 4. Sincronizza su SD e prova
```
tools/sd-sync.ps1            # copia deploy/sd-safe → SD (non cancella lo stato utente)
```
Apri **Paint → ✨ Atelier** su un browser con WebGPU (Chrome/Edge desktop). Scrivi un prompt o fai uno
schizzo sulla tela, premi **Genera**: l'immagine viene disegnata e salvata in `/data/Pictures/`.

## Download dal dispositivo (opzionale)
Se ospiti gli shard su un CDN, imposta `cdn.baseUrl` per il modello in `registry/model-catalog.json`:
allora il **Model Manager** in ANIMA (`/modelli`) può scaricarli direttamente, **un download alla volta**,
**resumibile** (verifica SHA per shard + indice su SD). Senza CDN, il provisioning da PC è la via.

## Test (host, deterministici)
- `node --test tools/anima-host/forge-model-fetch.test.mjs` — lock singolo, resume, finestre, SHA, scheduler.
- `node --test tools/anima-host/forge-sd-model-loader.test.mjs` — ricomposizione chunk.
- `node --test tools/anima-host/forge-diffusion-engine.test.mjs` — tokenizer, sonda, MockEngine, pipeline ONNX.
