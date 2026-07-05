# Ready-to-post announcements

Copy-paste these to get NucleoOS in front of people. Post when you have a spare hour to answer
comments (engagement in the first few hours drives GitHub Trending). Lead with the web-shell demo
GIF — it's the hook: a Windows-class desktop coming out of a $30 keyboard-sized ESP32.

Repo: https://github.com/indecenti/NucleoOs

---

## Hacker News — "Show HN"

**Title:**
`Show HN: NucleoOS – a real OS for the M5Stack Cardputer (512KB RAM, no PSRAM)`

**Body:**
> I built an operating system for the M5Stack Cardputer — an ESP32-S3 keyboard-sized device with
> no PSRAM and ~512 KB of RAM. It has an offline AI assistant (retrieval cascade + neuro-symbolic
> reasoning in C, no LLM, grounded so it never hallucinates a fact it doesn't have), a bilingual
> concatenative TTS voice, native multiplayer games over ESP-NOW, a clean-room Wi-Fi/BLE/Ethernet
> security lab, on-device speech transcription, and a full desktop-class shell that the device
> serves over Wi-Fi and your browser renders — file manager, spreadsheet, an AI image editor
> (WebGPU, fully offline), a game center over WebRTC.
>
> The interesting engineering problem was fitting all of this in 512 KB with zero PSRAM: exclusive
> app modes that reclaim RAM, heap-on-enter instead of `.bss`, a solo-boot mode that skips the
> HTTP server for a contiguous heap, canvas-free OTA. Runs on both the original Cardputer and the
> newer Cardputer ADV from one universal binary.
>
> Repo + demo GIF in the README. Happy to talk about the RAM tricks, the offline retrieval
> cascade, or the security lab.

Post at https://news.ycombinator.com/submit — link to the repo, put the note in the text field.

---

## Reddit — r/esp32 (also r/embedded, r/cyberDeck)

**Title:**
`I wrote a real OS for the Cardputer — offline AI, security lab, and a full browser desktop shell`

**Body:**
> ESP32-S3, no PSRAM, ~512 KB RAM — turned into a multi-app OS with two complete UIs: a
> smartwatch-style native launcher on the 240×135 screen, and a Windows-class desktop shell the
> device serves over Wi-Fi (file manager, spreadsheet, Paint with offline WebGPU image gen, a
> WebRTC game center). The AI assistant (ANIMA) runs fully offline in C — retrieval cascade,
> neuro-symbolic reasoning, no cloud — and is also compiled to WASM so it runs in your browser.
> There's also a clean-room Wi-Fi/BLE/Ethernet security toolkit and on-device speech
> transcription. One universal binary supports both the original Cardputer and the Cardputer ADV.
>
> Repo + demo GIFs: https://github.com/indecenti/NucleoOs
> Free for noncommercial use. Feedback welcome — especially from anyone who's fought RAM
> fragmentation on an S3.

---

## M5Stack community / forum

**Title:** `NucleoOS — a full OS for the Cardputer (+ Cardputer ADV), AI assistant, security lab, web shell`

> Sharing a project for the Cardputer family: NucleoOS turns the device into an actual OS rather
> than a menu of demos — native firmware apps (games, security tools, sensors, media, an offline
> AI assistant) plus a desktop-class web shell served straight off the device. One universal
> binary auto-detects original Cardputer vs Cardputer ADV. Repo + screenshots/GIFs:
> https://github.com/indecenti/NucleoOs

Post in the M5Stack forum (https://community.m5stack.com/) under Cardputer / show-and-tell.

---

## Hackster.io / Hackaday tip line

Short note + repo link + demo GIF:
> "NucleoOS: a real multi-app OS for the M5Stack Cardputer — offline AI assistant with no LLM
> (grounded, in C), a security lab, native ESP-NOW multiplayer games, and a full desktop-class web
> shell the device serves over Wi-Fi. Runs on both Cardputer and Cardputer ADV. <repo link>"

- Hackaday tip line: https://hackaday.com/submit-a-tip/
- Hackster.io: post as a project write-up with the GIF as the hero image.

---

## awesome-esp32 / awesome-embedded — pull request

Add a line under the relevant section:

```
- [NucleoOS](https://github.com/indecenti/NucleoOs) - A web-native OS for the M5Stack Cardputer (ESP32-S3, no PSRAM): offline AI assistant, native games, security lab, desktop-class browser shell.
```

Candidate lists: https://github.com/agucova/awesome-esp32 ·
https://github.com/nhivp/Awesome-Embedded

---

### Tips
- Lead every post with a **GIF**, not a wall of text — the web-shell desktop GIF is the strongest hook.
- Reply fast to the first comments; that window decides whether you hit Trending.
- Don't cross-post everything in the same 10 minutes; space HN / Reddit / forums over a day or two.
- Never buy stars/followers — GitHub flags it and it kills ranking.
- The M5Stack community forum is a *very* targeted audience for this specific device — likely your
  highest-signal channel before HN/Reddit.
