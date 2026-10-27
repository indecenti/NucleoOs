# NFV — Cardputer video tools

The Cardputer (ESP32-S3, **no PSRAM**) can't decode H.264/H.265, so videos are pre-converted into
`.nfv` clips that the native Video app plays. There is ONE format: **NFV v4** — whole-frame baseline
JPEGs and IMA ADPCM sound interleaved in one file. Older `.nfv` (v1/v2 with a sibling `.mp3`, v3
tile-delta) no longer play: the device lists them as "old" and `migrate.py` carries a v1/v2 clip over.

## Ways to make a clip

| Where | How | Best for |
|---|---|---|
| Browser | **Video Studio** app (ffmpeg.wasm, nothing to install) — writes straight to the SD in resumable parts | a few clips, from any PC |
| PC, GUI | `tools\nfv\run_studio.ps1` (installs deps, opens `studio_gui.py`; also in NucleoSuite) | drag & drop, queues |
| PC, CLI | `python tools/nfv/nfv4.py movie.mkv` | whole films, batches |

```bash
python tools/nfv/nfv4.py movie.mkv                     # 320x240: external 2.8" panel (scaled 3/4 on the Cardputer alone)
python tools/nfv/nfv4.py movie.mkv --target cardputer  # 240x135 for the Cardputer's own screen only
python tools/nfv/nfv4.py movie.mkv --ss 180 --duration 150 --quality 80 --fps 15
node tools/nfv/push-resume.mjs --host http://<device> --pin <pin> "movie.nfv"   # resumable upload to /data/Videos
python tools/nfv/migrate.py "Old clip.nfv"             # v1/v2 (+ sibling .mp3) -> "Old clip (v4).nfv", no source needed
```

All three write the same bytes: `nfv.test.mjs` rebuilds a `nfv4.py` clip with the browser writer and
requires it byte for byte. Sound is normalised to the same loudness everywhere (two-pass EBU R128
`loudnorm`, -14 LUFS, true peak -1.5 dB, linear gain), so a quiet film plays as loud as a loud one;
`--no-normalize` keeps the source level.

## Why v4 looks like this

| Measured on v3 | v4 answer |
|---|---|
| Two readers on one SD card (MP3 task + video loop) deadlocked on the bus the card shares with the external panel | **One reader**: video frames and audio blocks interleaved in one file, read front to back by the play loop |
| Per-tile decode (TJpgDec) ~3.5 ms per 64x64 tile; the picture visibly built up tile by tile | **Whole-frame JPEG** decoded by Espressif's **esp_new_jpeg** (SIMD on the S3): ~9.5 ms per 320x240 frame, in 16-row strips, ~10 KB RAM |
| Helix MP3: ~20 KB RAM and a task of its own | **IMA ADPCM** 16 kHz mono (8 KB/s), a few integer ops per sample, written to the I2S (whose queue is the clock) |
| The SPI bus (SD + panel) is the bottleneck: a full 320x240 frame is ~31 ms at 40 MHz | **Strip mask**: each frame says which 16-row strips changed; only those are sent (every strip refreshed at least every 2 s) |

Measured on the Cardputer ADV + 320x240 ILI9341, Wallace & Gromit (stop-motion, 15 fps, ~11 KB/frame):
**~14-15 fps shown** in calm scenes; per frame ~27 ms SD read, ~9.5 ms decode, ~3.7 ms per strip sent.
What did NOT work, and why (so nobody retries it): a reader task on the other core — the SD driver and the
panel then fight for the one SPI bus transaction by transaction (push went from ~30 to ~560 ms per frame); an
unbuffered `FILE` (every small read becomes a card transfer: ~60 KB/s instead of ~375 KB/s).

Format spec: the docstring of [`nfv4.py`](nfv4.py), which also holds the reference reader (`read_v4`,
`ima_decode_block`). Players: `play_nfv4()` in `firmware/components/nucleo_app/app_video.cpp` (device),
[`apps/video-player/www/nfv4.js`](../../apps/video-player/www/nfv4.js) (browser: canvas + Web Audio; the
device simulator's Video app uses it too).

## Validate without a device

```bash
node tools/nfv/nfv.test.mjs        # browser writer == nfv4.py byte for byte; layout, audio lead, index, masks
```

## Files

- `nfv4.py` — the v4 encoder (ffmpeg → JPEG frames + strip masks, loudness-normalised ADPCM) + reference reader + CLI.
- `studio_gui.py` / `run_studio.ps1` — the drag & drop GUI over `nfv4.py` (NucleoSuite's "NFV Video Converter").
- `migrate.py` — old v1/v2 clip → v4, from its own frames and `.mp3`.
- `push-resume.mjs` — resumable upload (512 KB parts over `/api/fs/write?append=1&at=N`); `push-file.mjs` — single-file streamer.
- `nfv.test.mjs` — host test of the browser writer/reader.
- `fetch-ffmpeg-wasm.mjs` — vendors ffmpeg.wasm into the Video Studio app.
- `push-wg-to-sd.ps1` — copies the pinned Wallace & Gromit shorts from `deploy/sd` to an SD in a card reader.
