# NFV — Cardputer video tools

The Cardputer (ESP32-S3, **no PSRAM**) can't decode H.264/H.265, so videos are pre-converted on
the PC into `.nfv` clips that the native Video app plays by drawing one JPEG at a time straight to
the panel. These tools do that conversion.

## Quick start — the GUI (recommended)

```powershell
tools\nfv\run_studio.ps1            # installs deps on first run, then opens the window
```

Drag a video onto the window (MP4 / AVI / MKV / MOV / WMV / FLV / WebM …), pick a profile, hit
**CONVERTI**. Then hit **▶ Anteprima** to play the result **exactly as the Cardputer renders it**
(240×135, same tile-delta decode, the on-device controller overlay, audio in sync) before you copy
it over — so you can judge quality/size without flashing. Copy the file(s) to the SD card under
`\data\Videos\` and open the Video app on the device.

### Preview player (▶ Anteprima)
Plays any `.nfv` (v2 or v3) with the **same decode engine the firmware uses**, so what you see is
what the device shows. Controls mirror the device: space = play/pause, ←/→ = ±5 s, ↑/↓ = volume,
`m` = mute. Toggles: **Overlay** (the on-device controller bar), **Liscio** (smooth vs real chunky
pixels), **Zoom** (2–5×), **Lum** (emulates the panel backlight). Audio uses `ffplay` (ships with
ffmpeg); without it the preview is silent.

Requirements: **ffmpeg** in `PATH`; `pip install pillow numpy` (v3 engine) and optionally
`pip install tkinterdnd2` (drag&drop — without it the window still works via the Browse button).

## Two formats

| | **NFV v3** (default) | **NFV v2** (classic) |
|---|---|---|
| Layout | ONE file; audio embedded | `.nfv` video + sibling `.mp3` |
| Coding | tile-delta — only the parts of each frame that change are stored | every frame is a full JPEG |
| Size | **3-5× smaller** on typical content | baseline |
| Quant | tuned for the 1.14" panel (high-freq detail dropped) | generic |
| Device RAM | one small tile buffer (~a few KB) | one full-frame buffer (up to ~32 KB) |
| Plays on | builds with the v3 player (this repo) | any NucleoOS build |

Why v3 is possible at all: the player draws direct to the ST7789, so the **panel's own GRAM keeps
the previous frame** — a partial (tile) update costs zero extra ESP RAM. The encoder precomputes
which tiles changed; the device just repaints those. Every tile shares one JPEG header (stored
once); each tile carries only its entropy scan, rebuilt on-device as `template + scan + FFD9` —
byte-identical to a normal JPEG, so the panel's baseline decoder accepts it unchanged. Full format
spec: the docstring at the top of [`nfv3.py`](nfv3.py).

### Audio sizing
The mono NS4168 speaker has no usable response above ~6 kHz, so v3 defaults to **16000 Hz / 24 kbps**
(v2 used 22050 Hz / 40 kbps). Same perceived sound on that speaker, ~40 % smaller audio.

### Motion fidelity (no "creep / bob")
A tile is repainted when its average changes **or** when enough pixels move locally (a face drifting
inside an otherwise-flat tile). A pure mean would freeze such a tile until the next keyframe, so the
content would slide behind a stale grid and then snap back every GOP — the "image goes up and down"
artifact. The localized-motion test plus a 2 s GOP keeps the decoded picture tracking the source:
measured ~2.5× lower error-vs-source than the mean-only metric. Diagnose any clip with
`python tools/nfv/diag_drift.py clip.nfv [N] [source.mp4]` — it reports player faithfulness, vertical
drift, and keyframe-snap, isolating an encoder issue from genuine content motion or a player bug.

## Command line (for scripting / batch)

```bash
# v3 (tile-delta, single file)
python tools/nfv/nfv3.py movie.mkv --quality 70 --denoise light --tile 48 --gpu
python tools/nfv/nfv3.py movie.mkv --ss 30 --duration 120 --ar 11025 --ab 16   # trim + tiny audio

# v2 (classic, compatible)
python tools/nfv/encode.py movie.mkv --profile balanced
```

`--gpu` uses the NVIDIA NVDEC decoder for H.264/HEVC sources (the MJPEG re-encode always runs on
CPU — it's cheap at 240×135).

## Validate without a device

```bash
python tools/nfv/test_nfv3.py       # qtable round-trip, byte-exact template surgery,
                                     # synthetic + real-content PSNR & compression vs v2
```

## Files

- `studio_gui.py` — the drag&drop GUI (v2 + v3) with the built-in Cardputer preview player.
- `nfv3.py` — v3 engine (encoder, container, self-validating decoder, stateful `V3Reader`/`V2Reader`
  that mirror the firmware playback engine) + CLI.
- `encode.py` — v2 CLI encoder.
- `test_nfv3.py` — host validation / benchmark.
- `reindex.py` — add a seek index to legacy v1 clips.
- `studio.mjs`, `push-*.mjs` — the older Node.js web studio and device-push helpers.
