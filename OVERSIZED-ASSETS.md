# Oversized assets (>100 MiB)

GitHub rejects individual files larger than **100 MiB** on the free plan. Some binary
assets of NucleoOS exceed that limit, so they are versioned **in pieces** (parts of ≤90 MiB,
plain git blobs — no Git LFS, no cost) under [`oversized-assets/parts/`](oversized-assets/parts/),
and are **rebuilt on the fly** with a script.

## How to rebuild them

From the repo root (Node required):

```bash
node oversized-assets/rejoin.mjs            # rebuilds all of them
node oversized-assets/rejoin.mjs teacher-npy tts-it-clips   # only some
```

The script concatenates the parts into the original path, creating the folders, and **verifies
integrity with SHA-256**. The rebuilt files are listed in `.git/info/exclude` (they are not
versioned): they stay local.

## Included assets

| id | Rebuilt file | Size | Parts | What it is |
|----|------------------|------|-------|-----------|
| `qwen-coder-gguf` | `deploy/sd-safe/apps/anima/www/forge/models/Qwen2.5-Coder-0.5B-Instruct-GGUF/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf` | 469 MiB | 6 | **Qwen2.5-Coder 0.5B Instruct** model (GGUF q4_k_m) for ANIMA Forge (wllama/llama.cpp path) |
| `teacher-npy` | `tools/anima/.cache/teacher_200000_192.npy` | 146 MiB | 2 | NumPy "teacher" embedding cache (200k×192) of the ANIMA encoder pipeline |
| `tts-it-clips` | `deploy/sd-safe/data/tts/it/clips.pcm` | 416 MiB | 5 | Clip bank of the **Italian concatenative TTS** (`nucleo_tts`) |
| `tts-en-clips` | `deploy/sd-safe/data/tts/en/clips.pcm` | 386 MiB | 5 | Clip bank of the **English concatenative TTS** (`nucleo_tts`) |

The reference SHA-256 checksums are in [`oversized-assets/manifest.json`](oversized-assets/manifest.json).

## Regenerating the parts (for anyone updating the assets)

If you replace an original file, regenerate the parts and the manifest with:

```bash
node oversized-assets/make-parts.mjs
```

## Not included

- Converted `.nfv` videos (`tools/nfv/out/`) are never committed: they are build output of
  the converter in `tools/nfv/`, and third-party films are copyrighted.
