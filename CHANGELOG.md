# Changelog

All notable user-facing changes to NucleoOS. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Versions match
[`firmware/version/VERSION`](firmware/version/VERSION) and the pushed git tag (`vX.Y.Z`).

## [Unreleased]

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
