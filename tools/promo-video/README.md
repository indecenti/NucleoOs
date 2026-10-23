# Promo video

A 30-second NucleoOS presentation video for Reddit (r/CardPuter, r/esp32) and Discord, cut to the
beat of an original soundtrack. Everything is scripted; outputs go to `build/promo/` (not committed).

| output | |
|---|---|
| `build/promo/nucleoos-promo-16x9.mp4` | 1920x1080, H.264 + AAC |
| `build/promo/nucleoos-promo-1x1.mp4`  | 1080x1080 square cut |
| `build/promo/contact-<fmt>.jpg`, `frame-<fmt>-<shot>.png` | one frame per shot, for review |

## Shots (bars of 4 beats, 128 BPM: one bar = 1.875 s)

1. hook: "A microcontroller that thinks it's a workstation" (2 bars)
2. native UI on the device, screens changing every two beats (2)
3. the web desktop the Cardputer serves at `nucleo-01.local` (3)
4. ANIMA, the offline assistant (2)
5. Costellazioni, then the Game Boy / Game Gear emulators (3)
6. "Installs itself": M5Launcher guest, Cardputer + ADV (2)
7. end card: logo, GitHub and Discord links (2 + 1.5 s hold)

## Inputs

- Native UI: `npm run ui:shots` renders (`build/ui-host/shots/*.en.classic.png`).
- Website checkout (`../nucleos_sites`, or `NUCLEOOS_SITE`): device cut-outs, web captures,
  fonts (Big Shoulders, Work Sans, Silkscreen), the atom icon.
- Soundtrack: ACE-Step 1.5 (local REST API) from `soundtrack.json` — instrumental electro, three
  seeds to choose from — then mastered for web (about -14 LUFS, true peak at most -2 dBTP).

## Build

```
npm run ui:shots                                   # if build/ui-host/shots is missing
python tools/promo-video/build.py --stills --audio x   # contact sheets only, no audio needed
python tools/promo-video/build.py --audio <mastered.wav> [--offset <s>]
```

Without `--offset` the first beat is found with librosa and the video starts on it.

## Privacy

The web captures' taskbar shows the capture machine's Wi-Fi name and clock, so every web capture is
cropped above it (`TASKBAR_CROP` in `build.py`). "Casa-Rossi" in the native renders is the UI host's
fake fixture. Check every shot's frame before publishing.
