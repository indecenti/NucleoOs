# NucleoOS: a real operating system for a $30 pocket computer

*Ready-to-post article (Hackster, dev.to, M5Stack forum, GitHub Discussions…). Images: `build/m5burner/cover.png`,
`docs/screenshots/webos.gif`, `docs/screenshots/anima.png`, `docs/screenshots/costellazioni-combat.jpg`;
video: `build/promo/nucleoos-promo-16x9.mp4`.*

---

**NucleoOS turns the M5Stack Cardputer (original and ADV) into a small but real computer**: an ESP32-S3 with
~512 KB of RAM and no PSRAM, a 240×135 screen and a keyboard the size of a credit card.

## One device, two interfaces

- **On the Cardputer:** a native launcher with 50+ apps and games, big readable fonts, type-to-search in five
  languages, a Control Center, and themes.
- **In your browser:** open `nucleo-01.local` and the device serves a **full desktop**: windows, snapping,
  Alt-Tab, notifications, and 40+ apps (files, spreadsheet, Paint, terminal, SSH, media, mail). The browser does
  the heavy lifting, so the tiny microcontroller stays light.

## What makes it different

- **ANIMA, an assistant that works offline.** A retrieval engine written in C: no cloud, no account. It opens
  apps, sets timers, does maths and conversions, answers questions, and when it doesn't know, it says so instead of
  making things up. The same engine runs in the browser (WebAssembly), an offline voice reads short answers in
  Italian and English, and it can also use a model on your own PC (Ollama, LM Studio, llama.cpp, Jan). Private
  mode keeps everything off the Internet.
- **Built for 512 KB.** Apps borrow memory only while they run and give it back on exit, a "solo" boot mode gives
  heavy apps a clean heap, and the native UI is tested on a PC by rendering the real C code into images, so changes
  are checked without flashing.
- **Installs and updates itself.** One firmware image for both boards (detected at boot). On first start it joins
  Wi-Fi and downloads its own SD content, verifying every file. Updates use two slots with automatic rollback. It
  also runs as a guest under M5Launcher without touching your other firmwares.
- **A pocket game console.** Native Game Boy and Game Gear emulators written for the project (bring your own
  ROMs), Costellazioni (a 6DOF space game with procedural star systems), multiplayer over ESP-NOW between
  Cardputers, and a peer-to-peer Game Center in the browser.
- **Voice and media.** MP3 recorder with live transcription, keyword voice commands trained on the device, radio,
  music and video player.
- **A security lab, for authorized testing only.** Clean-room Wi-Fi, BLE, IR, BadUSB and wired Ethernet tools,
  for devices and networks you own or may test.

## Try it

- Install from the browser: https://indecenti.github.io/NucleoOs/ (Chrome or Edge), or from M5Burner / M5Launcher
- Code and docs: https://github.com/indecenti/NucleoOs
- Website: https://nucleoos.indexhub.it/cardputer/
- Community and help bot on Discord: https://discord.gg/84tyTPtWus

Free for noncommercial use; commercial use needs a license.
