# KryonOS integration plan

**Status:** items 1 & 3 implemented (host-tested; firmware halves flash-pending) · item 2 parked ·
**Opened:** 2026-09-30 · **Owner:** indecenti
**Fits:** the consolidation phase (harden/test what exists; no new apps).

How to resume: this file **is** the plan. Item 1 (rescue page) and item 3 (release `type`) are
implemented and green on every host gate; their firmware C halves still need one flash to confirm
on-device (see [§8 Implementation status](#8-implementation-status)). Item 2 stays parked.

---

## 1. Why this document exists

We evaluated **KryonOS** (github.com/Haris16-code/KryonOS, GPL-3.0, v2.0.0) — a GUI
JavaScript OS for ESP32 that also lists the M5Stack Cardputer as an (untested) target. The
source was read in full. This plan records the licensing decision, the few ideas worth
importing, and exactly how to import them on our hardware, so we don't re-derive it later.

The headline conclusions:

- KryonOS targets **PSRAM** boards; its JS apps get ~90 KB heap only with Wi-Fi **off**. Our
  Cardputer (M5StampS3) has **no PSRAM** and ~18 KB runtime heap with Wi-Fi **on**. Most of
  KryonOS (Duktape on-device, Kryon3D, FastMath, cloud AI, mesh) does not fit our model and
  is rejected — see [§5](#5-rejected-with-reasons).
- Three ideas are genuinely worth importing; one of them (the **firmware rescue page**) is
  high-value robustness.
- We take **ideas, never code** — see [§3](#3-the-clean-room-rule).

## 2. Licensing decision — no change

**Decision: NucleoOS stays on [PolyForm Noncommercial 1.0.0](../LICENSE). Do not change it.**

Reasoning (so we don't relitigate):

- The question arises only because KryonOS is GPL-3.0. But we import **ideas**, and ideas —
  architecture, API shapes, "put a rescue page in flash" — are not copyrightable. No license
  event is triggered by reimplementing a concept clean-room.
- This is already the project's established practice: `nucleo_eth` and `nucleo_wifiatk` are
  clean-room re-implementations that use **Bruce (AGPL-3.0)** only as a behaviour reference,
  with no copyleft source copied. KryonOS is treated identically.
- Switching to GPL would be **actively harmful**: GPL is copyleft and would force the whole
  firmware to GPL, killing the commercial dual-licensing that `COMMERCIAL.md` exists to
  enable. We would gain nothing, because the ideas are already free to reuse.
- Copyleft components we already ship (EmulatorJS GPL-3.0, ffmpeg.wasm LGPL/GPL, DOSBox
  GPL-2.0) run **in the browser** as separate vendored bundles, never linked into the
  firmware — no contamination. This is documented in `THIRD_PARTY.md` and unchanged.

## 3. The clean-room rule

When importing any of the items below:

1. Read the KryonOS `.cpp` as a **behaviour reference** only. Note *what* it does and *why*.
2. Close the file. Do **not** keep it open while writing our code. Do **not** paste any line.
3. Write from scratch against our own APIs (`nucleo_httpd`, `nucleo_webfs`, `nucleo_auth`,
   the event bus, `nucleo_update`).
4. Cite KryonOS as the idea's origin in the commit message, not in the code.

This is the same discipline the header comments in `nucleo_eth.h` / `nucleo_wifiatk.h`
already describe for Bruce.

---

## 4. Items to import

### Item 1 — Firmware rescue page (HIGH VALUE)

**KryonOS idea.** It serves a web file manager straight from flash (PROGMEM), needing zero
bytes on the SD/LittleFS. So even with a blank card the device is manageable over the web.

**Our gap (verified).** The whole web OS is served from the SD card by the static catch-all
`static_get` in
[nucleo_webfs.c:307](../firmware/components/nucleo_webfs/nucleo_webfs.c) →
`map_uri("/")` resolves to `/sd/www/shell/index.html`
([nucleo_webfs.c:86](../firmware/components/nucleo_webfs/nucleo_webfs.c)). If that file (and
its `.gz` sibling) is missing or the card is unreadable, `fopen` fails and we return a bare
`404 Not Found` ([nucleo_webfs.c:201](../firmware/components/nucleo_webfs/nucleo_webfs.c)).
Result: SD absent/corrupt/unprovisioned means `http://<device>/` is a dead 404, with no web
way to recover — the exact failure KryonOS designed around.

Note this is *only* the web surface. The on-device UI and the JSON API
(`/api/status`, `/api/fs/*`, `/api/ota`) already work without the shell files, because httpd
still starts (`boot_healthy`) even on a degraded SD. What is missing is a **human-usable
landing page** that drives those APIs when the shell itself is gone.

**Design.**

- Author a single tiny static page, `web/rescue/index.html`, self-contained (inline CSS+JS,
  no external assets, no framework). Functions, all against existing endpoints:
  - list `/sd/www/shell` and `/sd/apps` via `GET /api/fs/list`,
  - upload a rebuilt shell/app bundle via the existing `POST /api/fs` upload path,
  - re-flash firmware via `POST /api/ota`,
  - show `/api/status` (version, free heap, SD state).
- Gzip it at build time (`gz:check` parity, like every other asset). Target **≤ 4 KB gz**.
- **Embed it in the firmware binary**, not on the SD — that is the whole point. Use ESP-IDF
  `target_add_binary_data(... TEXT)` / `EMBED_TXTFILES` in
  `nucleo_webfs/CMakeLists.txt` (no embed exists in the tree yet; this is the first).
  4 KB in flash is free against the 3.5 MB app slot at ~55% use
  (see [partition-table.md](partition-table.md)).
- **Serve it only as a fallback.** In `static_get`, when the resolved path is the shell index
  (request for `/` or `/index.html`) **and** neither the raw nor `.gz` file opens, serve the
  embedded page with `200 text/html` instead of the 404. Every other 404 (favicon probes,
  missing wllama variants) keeps today's behaviour — do not turn the rescue page into a
  catch-all SPA fallback, that would mask real missing-asset bugs.
- **Gate it** behind `NUCLEO_AUTH_GUARD(req)`
  ([nucleo_auth.h:49](../firmware/components/nucleo_auth/include/nucleo_auth.h)) — the page
  can upload files and trigger OTA, so it must be paired-only, exactly like `/api/ota` and
  `/api/logs`. Unpaired -> the existing 401 path, which itself needs a minimal inline "enter
  the PIN shown on the device" prompt (the normal pairing UI lives in the shell that is, by
  definition, missing here). Keep that prompt inside the same embedded page.

**RAM.** Zero at rest — it is a `.rodata` blob, not a task, not an SD read. It is only touched
when a browser hits `/` while the shell is absent. Serving 4 KB from flash cannot trip the
low-heap circuit breaker (that guards >512 KB streams).

**Files touched.** `web/rescue/index.html` (new), `nucleo_webfs/CMakeLists.txt` (embed),
`nucleo_webfs.c` (fallback branch in `static_get`), maybe a 1-line note in
`nucleo_webfs.h`. Build glue for the `.gz` in the asset pipeline. Update
`docs/update-check.md` / a short section in `docs/storage.md` describing the recovery path.

**Testing.**
- Host/sim: extend `tools/serve-shell.mjs` or add a check that, with `/www/shell` renamed,
  `GET /` returns the rescue page (paired) / 401 (unpaired), and a normal load is unaffected.
- On device (only when the user asks to flash): boot with the shell folder renamed on the SD,
  confirm `http://<device>/` shows the rescue page, upload a shell zip, confirm normal OS
  returns.
- Add to the verify matrix: `gz:check` must see `rescue/index.html.gz`.

**Effort:** ~half a day. **Risk:** low (additive; the only edit to an existing hot path is one
`if` in `static_get` on the failure branch). **This is robustness, not a feature — squarely
in the consolidation mandate.**

### Item 2 — OTA confirm liveness window (LOW / OPTIONAL — reassess)

**Reassessment after reading the code:** our OTA rollback gate is **already stronger than
KryonOS's**, so this is not the fix I first thought.

- KryonOS confirms the image after a blind **10 s timer** regardless of whether anything
  works (`OTAManager::confirmBootSuccessful`).
- We confirm **only on `boot_healthy`** — i.e. `nucleo_httpd_start()` actually succeeded and
  the web OS can serve
  ([main.c:419](../firmware/main/main.c)) — and a **degraded full-OS boot forces an immediate
  rollback** to the last-known-good image right then
  ([main.c:427+](../firmware/main/main.c)), with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` as
  the backstop. That is a real liveness proof, not a timer.

**Residual gap (small).** We confirm at the *end of `app_main`*, so a crash that happens a few
seconds later in the app task loop (first WS client, first render) lands *after*
confirmation -> no auto-rollback until a manual reboot triggers it.

**Optional hardening (only if we ever see such a late crash).** Defer `ota_confirm_if_pending()`
until the app task has completed a few main-loop iterations *and* the first client interaction,
instead of firing at boot's end. **Do not add KryonOS's fixed timer:** if the user power-cycles
inside the window, an otherwise-good image would roll back — a worse failure than the one we'd
be guarding against. Given the boot is already heap-tight and intricate, **leave as-is unless a
real late-boot crash is observed.** Recorded here so we don't re-open it blindly.

**Effort:** ~1–2 h if ever done. **Priority: parked.**

### Item 3 — Release `type` field (`security` / `major` / `minor` / `patch`)

**KryonOS idea.** Its `update.json` tags each release with a `type`; a `security` release
raises the update prompt's prominence.

**Our state (verified).** `version.json` is only `{"tag":"vX.Y.Z"}`
(`.github/workflows/pages.yml:68`). Both consumers compare **only** the semver tag:
- web: `decideNotify(...)` in `web/shell/update-check.js`,
- native: `upd_should_show(...)` in
  [nucleo_update.c:309](../firmware/components/nucleo_app/nucleo_update.c), which always emits
  `NOTIFY_INFO`.

**Design.**
- Add an optional `"type"` (`security`|`major`|`minor`|`patch`, default `patch`) to
  `version.json` in `pages.yml`; source it from the release tag/notes.
- Web: `update-check.js` reads `cache.type`; a `security` type maps the Notification Center
  entry to `warn` (see [notify-protocol.md](notify-protocol.md) levels) and can bypass the
  "one notification per tag" dedupe suppression once.
- Native: `nucleo_update.c` reads the type and, for `security`, emits `NOTIFY_WARN` instead of
  `NOTIFY_INFO`, with an adjusted title. Keep all five languages
  (IT/EN/ES/FR/DE) via the existing `UT(...)` macro — i18n parity is mandatory.
- Backward compatible: a `version.json` with no `type` behaves exactly as today.

**Files touched.** `.github/workflows/pages.yml`, `web/shell/update-check.js`,
`nucleo_update.c`, `docs/update-check.md`, `docs/versioning.md`. Host tests for `decideNotify`
extended for the type mapping.

**Effort:** ~2–3 h. **Priority: low but cheap; do it alongside the next release notes work.**

---

## 5. Rejected (with reasons)

- **Duktape / on-device JS apps (our `vm` tier).** KryonOS's own numbers confirm the wall:
  ~90 KB per app with Wi-Fi off. We have ~18 KB with Wi-Fi on. Standard Duktape keeps its
  built-ins in RAM and will not fit. Our `vm` tier stays a schema placeholder
  ([app-runtimes.md](app-runtimes.md)) until/unless a few-KB bytecode VM is built. Not now.
- **Kryon3D / FastMath.** A 240×135×16-bit framebuffer is 64.8 KB — larger than our entire
  heap. 3D compositing is a PSRAM feature.
- **KryonCloud / KryonBeam / boot telemetry.** Centralised cloud, and a boot-time POST of MAC
  + heap + RSSI to their server. This is the opposite of our local-first, device-contacts-
  nobody stance (see [update-check.md](update-check.md), the ANIMA local-first plan). Hard no.
- **Remote Help Center** (articles fetched from GitHub at runtime). We keep help on-device
  (`registry/manual/*.info`, five languages).
- **Notifications that snapshot the screen background** (~80 KB of sprites in KryonOS). Our
  device lesson is the opposite: redraw the dirty region, don't save it. Our
  [notify-protocol.md](notify-protocol.md) (event-bus, dedupe, DND) is already the better model.
- **`author`-string app identity.** KryonOS trusts a manifest `author` string. We already have
  a validated registry + `trustedSender` + sandboxing in the shell; nothing to take.
- **Insecure OTA transport.** KryonOS uses `client.setInsecure()` + an MD5 from the same
  channel (MITM-flashable). Ours is cert-bundle TLS + SHA-256 + `0xE9` magic-byte guard
  ([nucleo_update.c](../firmware/components/nucleo_app/nucleo_update.c),
  [update-check.md](update-check.md)). Keep ours.

## 6. Order of work

1. **Item 1 — rescue page.** Highest value, lowest risk, pure robustness. Prototype the page
   against the simulator (`node tools/serve-shell.mjs`) first, then the embed + `static_get`
   fallback, then flash-verify (only on the user's request).
2. **Item 3 — release `type`.** Bundle it with the next release-notes/versioning change.
3. **Item 2 — OTA liveness.** Parked; revisit only if a real late-boot crash appears.

Before any device flash: green ANIMA gate + the full verify matrix (`npm run validate`,
`i18n:gate`, `gz:check`, `icons:gate`, `gen:api:check`, `anima:gate`, `test:all`). Never
flash/OTA/sd-sync without the user explicitly asking.

## 7. Already done (license wording)

Corrected stale "MIT" claims that contradicted `LICENSE` (PolyForm Noncommercial 1.0.0),
found during this evaluation:

- [app-runtimes.md](app-runtimes.md) "Third-party SDK" — "NucleoOS is MIT-licensed" → PolyForm,
  with the clarification that an app built on the SDK is the author's own work.
- [app-runtimes.md](app-runtimes.md) DOS case study — "our MIT firmware" → "our firmware".
- `nucleo_eth.h` header — "Clean-room, MIT" → "Clean-room (no copyleft source copied)".
- `nucleo_wifiatk.h` header — "NucleoOS stays MIT" → "NucleoOS keeps its own license".

These are wording fixes only; no behaviour change.

## 8. Implementation status

**Item 1 — rescue page: implemented (2026-09-30).**
- `firmware/components/nucleo_webfs/rescue.html` — single self-contained source (inline CSS+JS,
  ~11 KB), shared by the firmware (EMBED) and the simulator; drives `/api/status`, `/api/fs/*`
  (list/write/delete), `/api/ota` and `/api/pair`.
- `nucleo_webfs.c` + `CMakeLists.txt` — `EMBED_TXTFILES "rescue.html"`, served from `static_get`
  when the shell entry page (`/` or `/index.html`) can't be opened; every other missing asset keeps
  the honest 404. Design change vs §4: the page is public like the shell HTML (the `/api/*` auth gate
  is what protects uploads/flash), embedded **raw** not gz (simpler, still trivial in flash).
- `tools/serve-shell.mjs` — serves the same file at `/rescue` and mirrors the shell-missing fallback.
- Verified: the page paired, listed `/www/shell`, uploaded+deleted a file, and posted `/api/ota`
  against the simulator (browser + curl), no console errors; `/` still serves the real shell.
- **Flash-pending:** the `nucleo_webfs.c` C is not host-compiled (needs a device build/flash).

**Item 3 — release `type`: implemented (2026-09-30).**
- Pure cores (host-tested): `update_policy.c/.h` `upd_extract_type()` (gate `npm run update:test`,
  58/58) and `update-core.js` `parseReleaseType()` + `updateLevel()` (`tools/update-core.test.mjs`,
  green). A `security` release → `warn`/`NOTIFY_WARN`; else `info`/`NOTIFY_INFO`.
- Wiring: `update-check.js` (web notification level + icon + `type` in the SD bridge write) and
  `nucleo_update.c` (native notify level + security title). New i18n key `up_title_sec` in all five
  shell locales. `.github/workflows/pages.yml` derives `type` from the release-notes marker
  (`[security]` / `[major]` / `[minor]` / `[patch]`, default patch) into `version.json`.
- Green gates: `update:test`, `update-core` test, `gz:check` (595 pairs), `i18n:gate`, `validate`,
  `gen:api:check` (76/76).
- **Flash-pending:** the `nucleo_update.c` wiring is not host-compiled (the pure core it calls is).

Deviation noted: §4 item 3 proposed the `type` in `version.json` only; the browser actually derives
it from the release notes it already fetches, so no extra fetch — CI copies the same marker into
`version.json` for the native path. One authoring convention (`[security]` in the notes), both surfaces.
