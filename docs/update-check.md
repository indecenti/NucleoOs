# Release update check & in-shell updater

How installed devices learn that a new NucleoOS release exists, how the firmware and the SD card's
content get updated, and why an update can't leave a device unusable.

Two surfaces share one trust model: the **browser** (shell notifier + Settings ▸ Updates, which installs)
and the **native OS** (a boot dialog that says where to update, plus reminders). Both compare the running
firmware version against the latest release tag using the SAME semver-triplet rule. The device contacts
only the official release site (`indecenti.github.io`), and only to check versions or to download what
the user started. The web path is in [the section below](#architecture-in-one-paragraph); the SD content in
[SD card content](#sd-card-content); the native path in [Native OS path](#native-os-path).

## Architecture in one paragraph

The **browser** asks `api.github.com` for the latest release (one conditional request per day per
browser; an `ETag`/304 answer is free against the unauthenticated rate limit). It compares the tag
with the firmware version the shell already holds from its `/api/status` poll (`version` =
`PROJECT_VER`, e.g. `0.2.11+17.g1a2b3c4`). If the release is newer, the Notification Center shows
one notification per tag whose click opens **Settings ▸ Updates**; that tab can then download the
image, verify it, and stream it to `POST /api/ota`. The device's only involvement is the final
same-origin POST — the same browser-direct rule the LLM API keys follow.

## Trust & safety chain (Settings ▸ Updates, "Update firmware")

1. **Tag** — `api.github.com/repos/indecenti/NucleoOs/releases/latest` (shared cache in
   `localStorage`, key `nucleo.update.cache`, one coherent state for shell + Settings).
2. **Host** — downloads come from the Pages site (`indecenti.github.io/NucleoOs/`), not from
   Release assets: GitHub Release assets send **no CORS headers** (browser fetch blocked — the web
   flasher hit the same wall, see `.github/workflows/pages.yml`), Pages serves with CORS.
3. **Deploy-lag guard** — Pages `version.json` must carry the same semver triplet as the API tag,
   or the updater refuses ("release just published, try again in a few minutes"). Never flash
   yesterday's image under a fresh tag.
4. **Integrity** — `SHA256SUMS` (same Pages deploy) must list `nucleoos-latest-ota.bin`; the
   download is hashed in pure JS (`web/shell/sha256.js` — `crypto.subtle` needs a secure context
   and `http://<lan-ip>` is not one) and must match. First byte must be the ESP image magic
   (`0xE9`), so a cached error page can never reach the flash.
5. **The right image** — `POST /api/ota` writes the **OTA slot**, so it gets the app-only image
   (`nucleoos-latest-ota.bin` = `firmware/build/nucleoos.bin`). The merged 0x0 image
   (`nucleoos-latest.bin`) is for full flashing only — sending it to `/api/ota` would write a
   bootloader into the app slot.
6. **Device side** — `/api/ota` is pairing-gated, single-flight (arbiter, `503 + Retry-After` when
   busy) and manages its own RAM posture. After `esp_ota_end()` it requires the image's app descriptor
   to name the project `nucleoos` before making it bootable — a foreign firmware or the merged 0x0 image
   also starts with `0xE9`. Self-OTA is allowed only on NucleoOS's own two-bank table (`ota_0`/`ota_1`,
   `guest_layout_is_own`), never under M5Launcher or another boot manager. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
7. **Verdict** — after the POST the updater polls `/api/status` until the reported version matches the
   tag AND `ota.state` is no longer `pending` (the device confirms a new image only once its OS has run,
   see below) — so "updated" is never announced while a power cut could still roll it back. An image that
   ran and came back on the old version is reported as **rolled back**, by name. The Updates app (manual
   file install) applies the same rule, judging only answers from AFTER the restart and comparing the
   running slot with the one before.

## Cadence, privacy, opt-out

- Passive check: `web/shell/update-check.js`, lazy-loaded ~15 s after the desktop is up (never in
  competition with session restore or the SD crawl). TTL 24 h; long-lived tabs re-evaluate every 6 h.
- The browser contacts `api.github.com` (the check) and `indecenti.github.io` (only when the user
  installs). Nothing identifying is sent. The device itself contacts only `indecenti.github.io`: its own
  version check (heap permitting, see below) and the SD-content download the user starts.
- The browser bridges what it learned to the card (`/system/config/update.json`) **once per tag**
  (`localStorage` `nucleo.update.bridged`), not on every 6 h re-check — no needless flash wear.
- Every passive failure (offline, rate limit, GitHub down) is **silent**. Errors only surface on
  the explicit "Check now" in Settings.
- Opt-out: Settings ▸ Updates ▸ "Automatic check" (per browser — the check runs in the browser;
  `localStorage` key `nucleo.update.enabled`).
- One notification per tag per browser (`nucleo.update.notified`), coalesced by stable id.

## Automatic rollback (fail-safe by construction)

An update can't leave the device unusable — two independent layers guarantee it:

1. **Integrity, before the image is ever booted.** The install verifies SHA-256 + the ESP image magic,
   `esp_ota_end()` validates the image's own checksum, and the app descriptor must name `nucleoos`. A
   corrupt, incomplete or foreign image therefore **never becomes the boot partition**
   (`esp_ota_set_boot_partition` runs only after validation) — the device keeps running the old firmware.
2. **Rollback, if a valid-looking image runs badly.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a
   freshly-flashed image boots as `PENDING_VERIFY` and is *confirmed*
   (`esp_ota_mark_app_valid_cancel_rollback`) only once the OS has proved itself — **deferred confirm**
   (rules in `ota_confirm_fsm.h`, host-gated by `npm run otaconfirm:test` across simulated reboots; glue in
   `main.c`): httpd must be serving, and then the app loop (`nucleo_app_run`, every boot profile) must run for
   20 s from its first turn (`nucleo_ota_health_tick`). A failed confirm (flash busy) is retried every 5 s,
   never dropped. Before the confirm:
   - **Crash / panic / watchdog** anywhere — ANIMA init, the voice, the app registry, the UI — reboots a
     still-pending image and the bootloader rolls back to the previous one. (It used to confirm right after
     httpd started, so an image crashing later in the boot looped for good: only a USB reflash helped.)
   - **A deliberate restart inside the window** (the web-profile handoff, a Solo launch, the setup wizard)
     confirms first through a shutdown handler — but only once the loop has run, so an `esp_restart()` taken
     on a failure path before the UI comes up still rolls back. Panics and the watchdog never run it. Such an
     EARLY confirm proved little, so it leaves a **trial marker** (RTC memory, bound to that image's ELF
     SHA-256 so it never judges another image): a later boot that ends in a panic or a watchdog reset while it
     is set calls `esp_ota_mark_app_invalid_rollback_and_reboot()` and goes back to the previous firmware (the
     next boot's `system.boot` event carries `"ota":"trial-rollback"`); the marker clears once a boot's loop has
     itself run 20 s. A brown-out, a power cut or a crash in the SD-content install boot (no app loop, a
     download says nothing about the firmware) is no verdict. The confirm itself calls the idempotent
     `esp_ota_mark_app_valid_cancel_rollback()` directly, so a transient otadata read error can't disarm it.
   - **Restarts the device would regret are refused** until the image has settled (its loop has run):
     `POST /api/sdcontent/arm` and `POST /api/reboot` answer `409 {"error":"starting"}` + `Retry-After` (the
     Settings tab retries the arm by itself and explains a refused reboot; `/api/reboot` stops refusing after a
     minute of uptime, so a boot wedged before its loop stays restartable — and rolls back), and `POST /api/ota` answers `409 {"error":"pending"}` while the running
     image still waits for its own confirm (the bootloader would refuse a new one anyway); the web updaters
     wait for that before sending the image.
   - **Degraded boot** (full OS, httpd never came up) on a still-pending image: `main.c` rolls back
     **immediately** (`esp_ota_mark_app_invalid_rollback_and_reboot()`). The previous image was confirmed,
     so this cannot loop. Excluded: Solo boots and the first-boot setup (httpd down on purpose — the setup
     boot uses the same deferred confirm), and a guest of M5Launcher (no OTA data of its own).

Net effect: a bad update **self-heals** — corrupt or foreign images are refused before boot, and an image
that boots but doesn't hold up is replaced automatically with the one that did. A power cut in the first
~20 s after an update also rolls back — which is why the web updaters wait for the confirm.

## SD card content

The web OS, the apps and ANIMA's knowledge live on the SD card, and each release ships its own
(`docs/sd-content-install.md`). The firmware only serves what is on the card, so the two must move together:

- `/api/status .sdc` reports `{status, card, fw, needed, armed}` (`nucleo_sdc_status_t`: no SD, missing,
  partial, outdated, skipped, complete, manual, unknown), cached ~30 s; `.sdc_diag` is why the last
  install failed.
- **Settings ▸ Updates ▸ SD card content** shows the card's release against the firmware's, in plain words,
  with Install / Update / Resume / Verify and repair. Two clicks (the device restarts and is away for
  minutes) → `POST /api/sdcontent/arm` → the device's lean install boot fetches **only the changed files**
  from `indecenti.github.io/NucleoOs/sd/<x.y.z>/` and verifies each against the release manifest, then
  restarts. The tab shows the elapsed time, then a verdict that stays readable: the new version, or the
  reason the device persisted. After a firmware update it points at the card when the card is behind.
  A hotspot-only device (no internet) is told so and can't start it. Works under M5Launcher too.
- **On the device**: once per firmware version, ~10 s into the app loop (not in the tight boot prologue), a
  card that is behind gets a notification (native banner + web Notification Center; in the web profile only
  once a browser is connected, so the device stays silent there) saying where to fix it — so a firmware updated from M5Launcher never leaves
  the card behind in silence (`nucleo_sdcontent_remind_due`: NVS first, the card only when not yet reminded).
- `/api/sdcontent/arm` writes the flag first and answers only what is true (500 if NVS refused), then restarts.
  On the device's failure screen a long reason wraps on two lines instead of running off the panel.

## Native OS path

For users who live in the native launcher. **The device does not install firmware by itself**, and the
dialog says so plainly. Two hard limits stand in the way on this chip: the TLS fetch needs a ~42 KB
contiguous block the heap never has once the OS is up (even the lean Solo boot tops out near 32 KB), and the
release site sends 16 KB TLS records to an 8 KB receive buffer (the SD-content installer gets around that
with 6 KB Range windows; an SD-staged firmware installer is the way forward).

**What the device knows.** The newest release it has learned comes from two sources and **the newer one
wins** (`best_known_latest`): the card's `/system/config/update.json`, bridged by any browser that ran the
check, and the device's own `version.json` check. That check is attempted only when the heap can fit the
handshake (`UPD_TLS_MIN_BLOCK`) — decided *before* spawning its task, so no RAM is held just to be refused.
A newer, non-dismissed release becomes a notification (`src:"ota"`, WARN for a security release); the
**boot dialog needs no network** — `nucleo_update_dialog_pending()` (pure NVS + `upd_should_show_at`).

**Boot dialog (3 choices):**
- **How to update** → a card with the exact way: on a stand-alone unit `http://<device IP>` (the hotspot's
  `192.168.4.1` in AP mode) ▸ Settings ▸ Updates (SHA-256 verified, automatic rollback); under M5Launcher,
  Launcher ▸ OTA ▸ NucleoOS; on a partition layout that is not ours (`ota.host: "other"`), over USB.
- **Next boot** (also Esc) → nothing persisted; the dialog returns on the next boot.
- **Ignore this version** → the tag (and when) is written to NVS (`dismiss`, `dismt`); that tag never
  prompts again — unless it is a **security** release, which comes back after 7 days
  (`UPD_SECURITY_REMIND_S`): reminded, never nagged every boot. A newer tag always prompts.

**Manual entry.** The Updates app lives in the System category: "Check now" runs the device's check, and a
heap too tight for it says so ("Low RAM: check from the browser") instead of blaming the network.

**Files:** `firmware/components/nucleo_app/nucleo_update.c` (engine: check worker + the Solo install
engine kept for a future SD-staged installer), `update_policy.c` / `update_policy.h` (pure decisions — the
C twin of `update-core.js`), `app_updates.cpp` (the dialog, five languages, ASCII for the Font0 panel),
wired into the run loop and builtin registry in `nucleo_app.cpp`. Rendered on the PC by `npm run ui:shots`
(`updates.*` scenes).

## Release assets involved

| Asset | Produced by | Purpose |
|---|---|---|
| `nucleoos-<ver>.bin` / `nucleoos-latest.bin` | `release.yml` (merge_bin) | Full 0x0 image — web flasher / esptool |
| `nucleoos-<ver>-ota.bin` / `nucleoos-latest-ota.bin` | `release.yml` | App-only image — in-shell `POST /api/ota` |
| `SHA256SUMS` | `release.yml` | Integrity for everything above |
| Pages: `nucleoos-latest-ota.bin`, `SHA256SUMS`, `version.json` | `pages.yml` (on release publish) | CORS-reachable copies + deploy-lag guard |

Releases that predate these assets simply hide the one-click path (the updater says "use the web
flasher") — `pages.yml` tolerates their absence.

## Files

- `web/shell/update-core.js` — pure logic (parsing, compare, cadence, notify decision, conditional
  fetch, SHA256SUMS parsing). Shared by both surfaces; unit-tested.
- `web/shell/sha256.js` — dependency-free SHA-256 (FIPS 180-4), streaming, hex out.
- `web/shell/update-check.js` — passive notifier (shell); emits `src:'ota'` with action
  `app:settings@updates` (the notify router deep-links a Settings tab via a one-shot
  `nucleo.settings.tab` hint + a `settings.tab` postMessage).
- `apps/settings/www/fwupdate.js` — the Updates tab driver (check → download → verify → flash →
  confirm verdict) and its SD card content section. Freezes the Settings poller during a flash or an SD
  install.
- `apps/updates/www/index.html` — manual install of a `.bin` file (offline / hotspot units): restart,
  confirm and rollback verdicts.
- `.github/workflows/release.yml`, `.github/workflows/pages.yml` — asset production/republish.

## Tests

**Web:** `node --test nucleo-webos tests/suite/update-core.test.mjs` (in `npm run test:unit`): version parsing/compare
edge cases (dirty `*`, `+build.g<hash>`, `nogit`, 4-part tags), cadence, notify decision, conditional
fetch with fake responses (ETag, 304, 403/500/offline), SHA256SUMS parsing, and SHA-256 against the
FIPS vectors plus `node:crypto` across all padding regimes.

**Native:** `npm run update:test` (also in the ANIMA gate as `update-policy (release)`) compiles the
REAL `update_policy.c` with MinGW and proves the same triplet parse/compare, `version.json` +
`SHA256SUMS` parsing (strict shape, refuses truncation and an HTML error page), the show/dismiss/
dev-build-ahead dialog logic, the security reminder over time (`upd_should_show_at`: an ignored security
release returns after 7 days; bad clocks never re-raise it), and the 24 h throttle that can never wedge
(never-checked and clock-went-backwards are both "due"). 68 assertions, pure C, no device.

**Guards:** `npm run guest:test` — the self-OTA whitelist (`guest_layout_is_own`): our two-bank table only;
the live ADV layout, a test-less boot manager and a factory app are all refused. `npm run otaconfirm:test` —
the deferred confirm and the trial across simulated reboots (23 cases: crash before/after the loop, the
handoff, a failed confirm, brown-out, a crash in the install boot, settle). Both also run in CI (`gate.yml`).

**Browser E2E:** `tools/web-e2e/sdcontent-update.e2e.mjs` (the SD content section: language, two-step
confirm, success and failure verdicts, hotspot, older firmware) and `tools/web-e2e/updates.e2e.mjs` (manual
install: complete only once confirmed; a rolled-back image is named as such). The simulator plays the
restart, the deferred confirm and a rollback (`/api/_sim/ota`, `/api/_sim/sdc`).
