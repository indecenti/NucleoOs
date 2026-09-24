# Changelog

All notable user-facing changes to NucleoOS. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Versions match
[`firmware/version/VERSION`](firmware/version/VERSION) and the pushed git tag (`vX.Y.Z`).

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
