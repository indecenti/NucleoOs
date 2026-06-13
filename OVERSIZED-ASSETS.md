# Asset oversize (>100 MiB)

GitHub rifiuta i singoli file oltre **100 MiB** sul piano gratuito. Alcuni asset binari
di NucleoOS superano quel limite, quindi sono versionati **a pezzi** (parti da ≤90 MiB,
blob git normali — niente Git LFS, niente costi) sotto [`oversized-assets/parts/`](oversized-assets/parts/),
e si **ricostruiscono al volo** con uno script.

## Come ricostruirli

Dalla root del repo (serve Node):

```bash
node oversized-assets/rejoin.mjs            # ricostruisce tutti
node oversized-assets/rejoin.mjs teacher-npy tts-it-clips   # solo alcuni
```

Lo script concatena le parti nel percorso originale, creando le cartelle, e **verifica
l'integrità con SHA-256**. I file ricostruiti sono in `.git/info/exclude` (non rientrano
nel versionamento): restano locali.

## Asset inclusi

| id | File ricostruito | Peso | Parti | Cos'è |
|----|------------------|------|-------|-------|
| `qwen-coder-gguf` | `deploy/sd-safe/apps/anima/www/forge/models/Qwen2.5-Coder-0.5B-Instruct-GGUF/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf` | 469 MiB | 6 | Modello **Qwen2.5-Coder 0.5B Instruct** (GGUF q4_k_m) per ANIMA Forge (path wllama/llama.cpp) |
| `teacher-npy` | `tools/anima/.cache/teacher_200000_192.npy` | 146 MiB | 2 | Cache NumPy embedding "teacher" (200k×192) della pipeline encoder ANIMA |
| `tts-it-clips` | `deploy/sd-safe/data/tts/it/clips.pcm` | 416 MiB | 5 | Banco clip del **TTS concatenativo italiano** (`nucleo_tts`) |
| `tts-en-clips` | `deploy/sd-safe/data/tts/en/clips.pcm` | 386 MiB | 5 | Banco clip del **TTS concatenativo inglese** (`nucleo_tts`) |

I checksum SHA-256 di riferimento sono in [`oversized-assets/manifest.json`](oversized-assets/manifest.json).

## Rigenerare le parti (per chi aggiorna gli asset)

Se sostituisci un file originale, rigenera parti + manifest con:

```bash
node oversized-assets/make-parts.mjs
```

## Non incluso

- `tools/nfv/out/Screamers - Urla dalla Spazio (1995).nfv` (307 MiB) — video `.nfv` di test:
  escluso di proposito (non è sorgente; riconvertibile dal filmato originale con il
  convertitore in `tools/nfv/`).
