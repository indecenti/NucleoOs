# Changelog

All notable user-facing changes to NucleoOS. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Versions match
[`firmware/version/VERSION`](firmware/version/VERSION) and the pushed git tag (`vX.Y.Z`).

## [Unreleased]

## [0.7.4] — 2026-10-24

### Added
- **Community app**: a new icon on the web desktop that opens the NucleoOS Discord, copies the invite, shows it as a
  QR code to scan with your phone (handy when the computer has no Internet), and explains the helper bot's `/ask`
  and `/guide` commands. ANIMA opens it too ("open discord", "apri community").
- **Discord community with a help bot** (`tools/discord-bot/`): the bot answers `/ask` and `/guide` from the docs
  and says when it doesn't know; moderation runs on Discord's own AutoMod. Join: https://discord.gg/84tyTPtWus

### Security
- **The Wi-Fi network's name is shown only to paired browsers.** `/api/status` stays public so the desktop
  can find the Cardputer, but it no longer tells an unpaired visitor which network the device joined.
- **The web desktop can't be framed by other sites, and links out don't reveal the device's address.** Pages
  are sent with `X-Frame-Options: SAMEORIGIN` and `Referrer-Policy: same-origin`; apps keep opening in the
  desktop's windows as before.

## [0.7.3] — 2026-10-22

### Added
- **The first-boot "Later" step shows a QR to copy the SD content from a computer.** When you skip the SD
  download, the Cardputer offers two ways to add the files later: over Wi-Fi from Settings ▸ Device, or from a
  PC — scan the QR to open a page that links the exact zip for your firmware and walks the copy step by step,
  in five languages. Before, it only named the zip, with no link and no instructions.

### Changed
- **On a Cardputer installed with M5Launcher, the update dialog points to the way that works.** It now says to
  put the release's firmware file on the SD card and install it from the Launcher's SD menu, instead of a
  Launcher OTA entry that does not exist until NucleoOS is published there.

## [0.7.2] — 2026-10-12

### Added
- **Update the SD card from the browser.** Settings ▸ Updates now shows whether the card's web OS, apps and
  ANIMA knowledge match the firmware, and brings them up to date in one go — only the changed files, each one
  verified; your data, keys and settings are never touched. After a firmware update it tells you when the card
  is behind, and the Cardputer itself reminds you once per version (useful after an update from M5Launcher).

### Changed
- **A firmware update counts as done only once the new firmware has run.** The Cardputer confirms it after its
  first seconds of healthy running; if it crashes before that, it goes back to the previous version by itself.
  Before, a firmware that crashed a little later in its start-up could stay stuck until a USB reflash. The
  Updates tab now says "updated" only after that confirmation, and names a rollback when one happens.
- **The update dialog on the Cardputer says where to update** — the address to open in a browser and the
  menu to use, or M5Launcher's OTA list — instead of an "Update now" that could not work on this chip.
- **An ignored security update is offered again after a week** (other ignored updates stay ignored).
- **One AI decision for the whole OS.** The Game Center's AI opponent, Paint's prompt enhancer, the Recorder's
  and Dictation's summaries, IR Remote's and Nearby's spoken commands now use the AI you set up once: a model on
  your PC (Ollama, LM Studio…) with no key, otherwise your cloud key. They used to need a cloud key, and each
  picked a provider its own way.

### Fixed
- **Private mode keeps everything on this computer in every app** — games, Paint (no online image generation),
  the Recorder and Dictation (no audio sent for transcription), the Spreadsheet (ANIMA stays offline), IR Remote
  and Nearby. Before, these apps used the cloud whatever the mode.
- **Paint's image providers explain themselves in Spanish, French and German** (they showed Italian).
- **Settings shows a newly added xAI or Groq key at once** instead of offering to add it again for half a minute.
- **A Cardputer installed with M5Launcher keeps its paired browsers across restarts.** Its pairing sessions
  live in a small memory area shared with the Launcher; once it filled up, every new pairing was forgotten at
  the next restart — opening the web desktop for the first time asked for the PIN a second time. They are now
  stored in a third of the space, and if room still runs out the oldest sessions give way, never the one in use.
- **The firmware updater refuses a file that isn't NucleoOS** (for example the full install image picked by
  mistake), and updates itself only on its own standard memory layout — never over another installed app.
- **"Up to date" is no longer shown when a newer version is already known** to the Cardputer.
- **"Check now" with too little free memory says so** and points to the browser, instead of "offline?";
  accented letters on the Cardputer's update screen no longer come out garbled.
- **The SD download explains a missing version** ("retry later or update the firmware") instead of
  "HTTP 404", and four releases' content now stay online instead of three.
- **Settings no longer says the device never contacts the internet**: it contacts only the official release
  site, to check versions and to download the updates you start.
- **ANIMA answers about people's jobs again on cards updated from a Windows PC.** Its fact files could be
  copied with the wrong line endings, and the Cardputer then refused to use them.
- **The desktop no longer gets stuck in English after a hiccup.** When the Cardputer was busy at the moment the
  desktop loaded its language, the whole session fell back to English until a reload. The language is now
  retried, and the desktop switches to it by itself as soon as it arrives.

## [0.7.1] — 2026-10-11

The first published release since 0.6.0: it also carries everything listed under 0.7.0 in
[CHANGELOG.md](https://github.com/indecenti/NucleoOs/blob/main/CHANGELOG.md), whose release build never completed.

### Added
- **Costellazioni**, the flagship space game — 6DOF combat in procedural star systems, a galaxy to
  cross (map, jumps, docking, beacons to relight) and worlds to land on (seamless descent, eight
  biomes, discoveries), with 66 paintings and an original four-track score. The native Cardputer
  version was reworked too.
- **Giardino**, a new merge game. Every other native game (Pong, Snake Duel, Tank Duel, Flipper,
  Reattore, Scorribanda, Poker, Slot, Yahtzee, Dadi, Cardler, Orde) was fixed, retuned and redrawn
  with the console look, in the five OS languages, with sound packs that never stall play.
- **Settings ▸ AI / Models ▸ Models on your PC** — one panel for the AI servers on your computer
  (Ollama, LM Studio, llama.cpp, Jan): what is running, a model per task (chat, code, agent), PC or
  cloud first, add a server by address, and the exact fix when a server refuses NucleoOS. The AI
  profiles and the fallback chain now show the PC model as a step, and ANIMA's engines panel links here.
- **ANIMA** answers unit conversions and calendar questions exactly in five languages, and the agent
  shell runs workspace scripts with `node FILE.js`.

### Changed
- **The web OS loads about six times faster from the Cardputer** — every file the device served
  waited 1.2 s for memory it could never get in the web profile. Measured on the original unit over a
  weak Wi-Fi link: a fresh browser now shows the desktop in about 5 s instead of over 30 s, and the
  very first visit (which restarts the device into web mode) in 22 s instead of 48 s.
- **Agenti follows the OS-wide AI choices** — Private mode keeps it off the cloud, and "PC first"
  makes it use the computer's model first, as ANIMA does.

### Fixed
- **Choosing the "Auto" AI profile no longer switches ANIMA to Private** when only the browser brain
  was ready; "Local only" now really keeps everything on this computer.
- **Firmware updates over a slow Wi-Fi link no longer abort at 90 %.**
- **ANIMA's agent puts new files where you asked** — a model that wrote the workspace path without its
  leading slash created them in a nested copy of the workspace, then reported them missing.
- **The Recorder says "Loading…" while it reads the card**, instead of "No recordings yet" before it knows.
- **Voice transcription with a Groq key works in Dictation and in the agent** — both called a Groq address that
  does not exist; the Recorder now also finds the Groq key saved in Settings, not only the one typed in AI Chat.
- Costellazioni music reaches the Cardputer (large files ship in slices); the launcher draws its
  icons from a table (30 KB less flash) and the game front-end gives back 4.9 KB of RAM.
- The AI key manager in Settings speaks Spanish, French and German instead of falling back to English.

## [0.7.0] — 2026-10-09

### Added
- **The assistant's agent keeps its plan in view** — the open checklist rides on every tool result,
  files can be read from any line (exact offset and total shown), and when the step budget runs out
  the agent ends with its own summary instead of stopping mid-task.
- **Browser tests in CI** — every app in 5 languages, data safety, security and resilience run in a
  real headless Chrome against the device simulator on every push and pull request.

### Changed
- **Settings in your language** — the AI profile panel and the Security & sessions section are now
  fully translated into Spanish, French and German.
- **An app window that loses a file while loading reloads itself once** — the device's small web
  server can drop a connection under load; the window now recovers instead of staying broken.

### Fixed
- **Your data is never overwritten by a failed read** — Terminal, Notepad, Tasks, Spreadsheet, SSH,
  Settings (AI keys), Radio, IR Remote, Clock, Games and Arcade saves no longer replace the real file
  with an empty one when the card is busy or offline. Terminal `touch` no longer empties a file.
- **File Commander and Recycle Bin never delete a file they failed to copy** — move, trash, restore
  and paste delete the original last and roll back on failure.
- **Security** — apps start untrusted until the registry vouches for them; desktop links inherit
  their target's sandbox; AI error messages never show an API key; names, events and help text from
  files or other devices are shown as text, never run as markup. On the device, ESP-NOW games and
  Nearby accept frames only from the joined peer, cap and sanitise received files, and refuse
  commands longer than the confirmation dialog shows.
- **Updates** only reports "complete" when the device really restarted, and refuses files that can
  never be a firmware image before sending a byte.
- **Around 50 further app fixes** found by new behaviour tests (QR, Media/Video/Photo players, System
  Monitor, Voice Manager, Recorder, Dictation, Help, Mail, Authenticator, Passkeys, Wi-Fi…) and by
  testing all 46 apps on a real Cardputer: windows stay on screen, labels follow the OS language,
  controls follow the theme, the wallpaper gallery works again.

## [0.6.0] — 2026-10-04

### Added
- **The screen turns off while an app is open, too** — the idle screen-off now also applies inside
  open apps, not only on the launcher, so a forgotten app no longer drains the battery.
- **Smarter SD-content install** — the installer notices content you copied to the card by hand and
  offers only what is actually missing.

### Changed
- **More free memory** — apps and the MP3 decoder release their RAM when idle (~31.8 KB more heap),
  USB networking's 19 KB buffer is only allocated while USB-web is in use, and the assistant's search
  and learning scratch memory exists only while it is working.
- **A smaller firmware image** — the assistant's people table is read from the SD instead of flash
  (−38.5 KB), keeping the image inside the M5Launcher slot; a build gate now keeps it there.
- **Control Center** — scrolls in large type, keeps the focused card centred and eases smoothly.

### Fixed
- **SD-content download** — one keep-alive connection with windowed range requests, so large files
  finish reliably on a busy network.
- **Control Center** — no stale rows left on screen while scrolling.

## [0.5.0] — 2026-10-01

### Added
- **The device downloads its own SD content** — on a blank or out-of-date card, NucleoOS fetches the
  web desktop and the assistant's files for its firmware straight from the release (GitHub Pages),
  verifies every file by SHA-256 and writes it to the card by itself, without ever touching your data.
  Offered as a skippable step at the end of the first-run wizard, and it resumes if interrupted. The
  first install pulls the ~50 MB *core* (a complete web OS + assistant); emulator and PC/phone-app
  packs stay optional. See `docs/sd-content-install.md`.
- **Runs under M5Launcher** — the same firmware image can be installed by M5Launcher (its OTA list
  or SD installer). NucleoOS detects it and runs as a guest: updates come from the Launcher (its own
  updater is switched off so it can never overwrite another installed app), and Settings ▸ Device
  gains *Back to M5Launcher*. Stand-alone installs are unchanged. See `docs/m5launcher.md`.
- **Erase SD card / Wipe everything** — two new rows under Settings ▸ Reset. *Erase SD card* formats
  the whole card (Wi-Fi, PIN and sessions kept); *Wipe everything* does a factory reset and formats
  the card — a truly blank device, then the first-run wizard. The format runs at the next boot, before
  anything opens a file on the card, and resumes if a power cut interrupts it.

### Fixed
- **Wi-Fi joins reliably from the device** — picking a network in the first-run wizard (or the Settings
  Wi-Fi app) now connects in pure station mode, so the 4-way handshake can't fail on a channel clash
  with the setup hotspot (this chip has one radio) — the long-standing "connecting… then it asks for
  the password again". A wrong password re-opens the field with what you typed and says why; the join
  logic is now a single, centralized path. The regulatory region follows the UI language, so channels
  12–13 work in the EU after a reset.
- **A readable, on-rails first-run wizard** — five languages (not two), a password field at a legible
  size with a one-tap Shift (Aa), a reveal toggle and the last character shown briefly, the real
  signal list from Settings, and an end screen ("All set") with the address and pairing PIN. Every path
  reaches the end; the step can't be left half-finished.
- **The release SD zip is current again** — it is now built from the sources (it was a months-old
  snapshot: an old web shell and 15 web apps missing), carries no development state files, ships only
  the knowledge shards the assistant actually reads (~72 MB less), and extracts straight to the card
  root (it used to wrap everything in an `sd/` folder). Each release also publishes a per-file
  manifest of that content, the base for the device downloading it by itself.

## [0.4.0] — 2026-09-24

### Added
- **Native Game Gear emulation** — a from-scratch VDP/PSG/mapper machine around a vendored,
  permissively-licensed Z80 core (MAME's Z80, used by every small SMS/GG project, is
  "freeware, non-commercial" and revocable — unusable here). SMS/zip ROMs, EEPROM saves,
  shares the Game Boy emulator's front-end. Passes zexall/zexdoc in full.
- **Native Settings, rebuilt** — one root list with live value previews and a suggestion chip
  for things needing attention (clock never set, no network, battery low + bright screen),
  sectioned pages (Wi-Fi · Hotspot · Bluetooth · Display · Sound · ANIMA · Language ·
  Date/time · Device · Reset) that adjust in place like a watch crown, type-to-search across
  every setting, confirm cards before destructive actions, and a real date/time editor.
- **The browser IS the firmware AI engine** — ANIMA's offline (WASM) and on-device engines now
  share one implementation, so answers behave identically in the browser and on the Cardputer.
- **One learned memory, recalled everywhere** — a fact you teach ANIMA is recalled the same way
  on-device, from the host harness, and from the browser.
- **Streaming replies + a real Stop** in the ANIMA web app, one context per client, reader mode,
  opening apps straight from an answer, and honest "I don't know" instead of a guess.
- **Complete native Calendar** — month grid, add/edit/delete, large touch-friendly type.
- **Alarm v2** — silent mode with a bounded trigger and auto re-arm, an always-dark screen,
  an SD event log, and ambient WAV recording while armed.
- **Update system overhaul** — automatic rollback safety, a browser→SD bridge so the native
  boot dialog can show "update available" without the device ever calling GitHub over TLS,
  and the native install now runs in a fresh-heap boot so a big OTA can't wedge the device.
- **All-channel Wi-Fi scan** for reliable joins on networks the old scan missed.

### Changed
- Model choice across every AI surface (chat, ANIMA, Atelier) is resolved live instead of
  pinned — a retired model is swapped automatically, with plain-language errors when a
  provider can't be reached.
- ANIMA's reasoning got sharper: reminders parsed the way people actually phrase them, better
  weather/app-name/device-action recognition, fewer wrong follow-up answers, four specific
  wrong answers fixed after a 2026-09 accuracy audit.
- Every native app's chrome (icons, accent colors) now reads from one shared `THEME_*` role
  table, so a theme change recolors the whole native shell live, launcher included.
- `/api/fs/list` now streams instead of buffering, so very large SD folders no longer answer
  with a 503 out-of-memory.
- Faster, lighter ANIMA boot: ~5 KB less static RAM used per boot, one fewer large stack copy.
- Game Boy emulator: a more accurate core, Pokémon-size battery-backed save RAM, crash-safe
  save states, and a measured-speed counter.

### Fixed
- The ANIMA web app's "Context" inspector modal had a see-through background (an undefined
  CSS variable), so the chat underneath showed through it; the AI-key manager in Settings was
  mounted flush against its card with no padding. Both fixed.
- The local dev/CI device simulator (`tools/serve-shell.mjs`) could spin into an unbounded
  mkdir+list loop on boot, starving every other request behind "device busy" — the mkdir
  handler now only announces a change when it actually created something, matching real
  firmware behaviour.
- `Esc` in the Game Boy emulator rebooted the whole console instead of leaving the game.
- Several native-app papercuts: TAB not reaching all fields in Notes/SSH/Beacon, data loss in
  Notes, broken SSH output, Calendar back-key handling, Wi-Fi setup and ADV-keyboard edge
  cases, a remaining flicker on the Wi-Fi tools and OTA screens.
- Hardened the web API: auth, the online-proxy against SSRF, OTA, filesystem and WebSocket
  endpoints against a batch of real failure modes found in a security pass.
- ANIMA web app: confirmation before a destructive agent action, no silent model download,
  and it no longer claims a device action happened when it didn't.
- Reproducible icon bundle build (normalized embedded-SVG line endings).

### Also in this release
- The release pipeline itself: gated, provenance-signed (build attestations), and
  self-verifying — this is the first release built by it.

---

*Earlier history: `git log v0.3.0..v0.2.11` and further back for the full commit trail — this
file starts tracking from 0.4.0 onward.*
