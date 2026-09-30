# Oversized assets

A few binaries are too large to belong in git (they would bloat every clone and the history
forever). They are kept **out of the repository** and fetched on demand, each from its canonical
home, then SHA-256-verified into place.

## Get them (one command)

From the repo root (Node 18+):

```bash
node oversized-assets/rejoin.mjs            # fetch every repo-hosted asset
node oversized-assets/rejoin.mjs tts-it-clips   # only some, by id
```

For each asset `rejoin.mjs` tries, in order: the file already on disk (skipped if its SHA-256
matches), local split parts under `oversized-assets/parts/` (offline fallback), then the declared
`source`. Everything is SHA-256-verified before it is written.

## Repo-hosted assets (GitHub Release)

Project-generated data with no public mirror. Hosted as plain files on a GitHub Release
(`releaseBase` in [`manifest.json`](oversized-assets/manifest.json)) and downloaded plug-and-play.

| id | Rebuilt file | Size | What it is |
|----|------------------|------|-----------|
| `tts-it-clips` | `deploy/sd-safe/data/tts/it/clips.pcm` | 416 MiB | Clip bank of the Italian concatenative TTS (`nucleo_tts`) |
| `tts-en-clips` | `deploy/sd-safe/data/tts/en/clips.pcm` | 386 MiB | Clip bank of the English concatenative TTS (`nucleo_tts`) |

On the device these are the spoken-voice packs: `tools/deploy.ps1` stages them onto the SD card,
and the on-device **Updates** app can pull them the same plug-and-play way the browser pulls
LLM/Vosk models. Split parts under `oversized-assets/parts/` remain as an offline fallback.

## External assets (never in the repo)

Fetched from their upstream, not hosted by this project at all.

| id | Where it comes from |
|----|---------------------|
| `qwen-coder-gguf` | The ANIMA Forge model. The browser streams it straight from Hugging Face (`apps/anima/www/forge/model-store.js`); a copy is never kept here. |
| `teacher-npy` | A NumPy encoder cache. Regenerated on demand: `python tools/anima/distill_aug.py`. |

## Updating a repo-hosted asset

If you replace a `clips.pcm`, regenerate the parts and refresh the manifest hashes, then upload
the new file to the release:

```bash
node oversized-assets/make-parts.mjs
```

Reference SHA-256 checksums live in [`manifest.json`](oversized-assets/manifest.json).

## Not committed

Converted `.nfv` videos (`tools/nfv/out/`) are build output of the converter in `tools/nfv/`;
third-party films are copyrighted and never committed.
