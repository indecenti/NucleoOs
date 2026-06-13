# Media playback on a single ESP

The honest engineering question: how do you play MP3 and MP4 on one ESP32-S3 with no PSRAM?
NucleoOS answers it the way that actually works well — and beats on-device-only OSes.

## Principle: the device serves bytes, the client decodes

For `web`-runtime media apps, the device only **streams the file** over `/api/fs/read`
(with the correct MIME type). The **browser** (PC or phone) does the decoding with native
`<audio>` / `<video>` elements. Result:

- **MP3, AAC, WAV, MP4 (H.264), WebM** all play perfectly — the browser has hardware-
  accelerated codecs.
- **ESP CPU/RAM cost ≈ zero** — it just reads the SD and pushes bytes. No PSRAM needed.
- One code path for PC and Android; no on-device codec to maintain.

This is why our Media Player and Video Player are `runtime: web`. It is strictly better than
decoding on-device for the common case, and it sidesteps the PSRAM limit that constrains
ELF/emulator-based OSes.

## On-device playback (optional, for standalone use)

When no client is connected and you want sound/video on the device itself:

| Media | On-device approach | Feasible? |
|---|---|---|
| **Audio (MP3)** | Helix MP3 decoder → I2S (NS4168 speaker) | ✅ Realistic on S3 (~tens of KB) |
| **Audio (WAV/PCM)** | Stream PCM → I2S directly | ✅ Trivial |
| **Video (H.264/MP4)** | Real-time decode on screen | ❌ No hardware decoder, no PSRAM |
| **Video (lightweight)** | MJPEG / GIF / raw RGB565 frames → ST7789 | ✅ Low res/fps only |

So: on-device **audio** is a realistic `service`-runtime add-on; on-device **video** is
limited to lightweight frame formats, while full MP4 always plays on the client. This is
documented honestly rather than pretending the Cardputer can decode H.264.

## Sample content & media library

First-boot provisioning creates standard media folders:

```
/data/Pictures   (images, wallpapers)
/data/Music      (audio)
/data/Videos     (video)
```

The repo ships sample content under `tools/sd-sim/data/` for the device simulator:
a `wallpaper.png`, a `test-tone.mp3`, and a `test-clip.mp4`.

## Apps

- **Photo Viewer** — browse `/data/Pictures`, view images, **set as wallpaper** (the shell
  applies it to the desktop and remembers the choice).
- **Media Player** — browse `/data/Music`, play audio via `<audio>`.
- **Video Player** — browse `/data/Videos`, play video via `<video>`.
- **Voice Recorder** — records the built-in PDM microphone (SPM1423) on-device to a WAV
  file in `/data/Recordings` via I2S RX (`nucleo_recorder`, `/api/rec/{start,stop,status,stream}`).
  The ESP only writes uncompressed 16 kHz/16-bit PCM (~32 KB/s, no codec); the app then
  **encodes MP3 in the browser** (lamejs) on download — the same "device serves bytes,
  client decodes/encodes" principle, so the device never runs an audio encoder.
- **ANIMA live dictation (on-device mic)** — `/api/rec/stream` streams the raw PCM live; the
  ANIMA web chat feeds it straight into Vosk WASM **in the browser** for speech-to-text. Same
  principle again (device serves bytes, client recognizes), so dictation needs no cloud and
  no browser-mic permission. Selectable in ANIMA ▸ Settings ▸ General ▸ Microphone.

## Recording on-device (microphone)

The Cardputer has a digital **PDM microphone** wired to the PDM clock (GPIO43) and data
(GPIO46) lines. `nucleo_recorder` opens an I2S PDM RX channel and streams 16-bit PCM
straight into a WAV file, patching the RIFF/data sizes when the recording stops. A cheap
peak meter is published as `rec.level` events for the live UI. This is `service`-class work
that costs ~tens of KB of RAM and no PSRAM — recording works standalone even with no client
connected. MP3 is intentionally **not** produced on-device (encoding is far heavier than
decoding); the browser does it instead.

For **live dictation**, `/api/rec/stream` opens the same PDM mic but, instead of writing a
file, sends the raw 16 kHz mono int16 PCM to the client over a chunked HTTP response until
the client disconnects (`mic_open`/`mic_close` are shared with the recorder). The browser
decodes each chunk to float and feeds Vosk — so transcription happens client-side with no
cloud. The mic is exclusive: streaming and recording-to-SD are mutually exclusive (guarded
by `s_streaming`/`s_recording`, returning HTTP 409 on conflict). Because the ESP-IDF web
server is single-task, an active stream holds the server for the dictation session and exits
the instant the client disconnects (or after a 5-minute safety cap).

## Limitations / next steps

- HTTP **Range requests** aren't served yet, so in-browser **seeking** of large media may
  be limited; playback from the start works. Adding `Range` support is a small follow-up.
