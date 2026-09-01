# Release update check & in-shell updater

How installed devices learn that a new NucleoOS release exists, and how the browser installs it —
without the device ever contacting the internet.

Two surfaces share one trust model: the **browser** (shell notifier + Settings ▸ Updates) and the
**native OS** (a boot dialog + on-device installer). Both compare the running firmware version
against the latest release tag using the SAME semver-triplet rule, and both verify SHA-256 before
flashing. The web path is in [the section below](#architecture-in-one-paragraph); the native path
is in [Native OS path](#native-os-path).

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
   busy) and manages its own RAM posture. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: an image that
   fails to boot rolls back automatically.
7. **Verdict** — after the POST the updater polls `/api/status` until the reported version matches
   the tag (success), times out honestly, or detects the old version well past the reboot window
   (rollback happened).

## Cadence, privacy, opt-out

- Passive check: `web/shell/update-check.js`, lazy-loaded ~15 s after the desktop is up (never in
  competition with session restore or the SD crawl). TTL 24 h; long-lived tabs re-evaluate every 6 h.
- Exactly one origin is contacted (`api.github.com`; plus `indecenti.github.io` only when the user
  installs). Nothing identifying is sent; the device is never in the loop.
- Every passive failure (offline, rate limit, GitHub down) is **silent**. Errors only surface on
  the explicit "Check now" in Settings.
- Opt-out: Settings ▸ Updates ▸ "Automatic check" (per browser — the check runs in the browser;
  `localStorage` key `nucleo.update.enabled`).
- One notification per tag per browser (`nucleo.update.notified`), coalesced by stable id.

## Automatic rollback (fail-safe by construction)

An update can never leave the device unusable — two independent layers guarantee it:

1. **Integrity, before the image is ever booted.** The install verifies SHA-256 + the ESP image
   magic, and `esp_ota_end()` validates the image's own checksum. A corrupt or incomplete download
   therefore **never becomes the boot partition** (`esp_ota_set_boot_partition` runs only after
   validation) — the device simply keeps running the old firmware. This is true for both the
   browser install (`POST /api/ota`) and any native path.
2. **Rollback, if a valid-looking image boots badly.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a
   freshly-flashed image boots as `PENDING_VERIFY` and is only *confirmed*
   (`esp_ota_mark_app_valid_cancel_rollback`) once the boot proves healthy — httpd actually serving
   (`boot_healthy` in `main.c`). Two outcomes if it doesn't:
   - **Crash / panic / watchdog:** the bootloader rolls back to the previous image on the next boot.
   - **Degraded boot** (full OS, httpd never came up) on a still-`PENDING_VERIFY` image: `main.c`
     rolls back **immediately** — `esp_ota_mark_app_invalid_rollback_and_reboot()` reboots straight
     into the previous, known-good firmware. No waiting for a manual reboot. The previous image was
     already confirmed valid, so this cannot loop; a Solo boot (httpd down by design) is excluded.

Net effect: a bad update **self-heals** — corrupt images are refused before boot, and an image that
boots but doesn't work is replaced automatically with the one that did.

## Native OS path

For users who live in the native launcher and may never open the browser shell, the device checks
and installs on its own — still without ever hitting the GitHub API from the device.

**Boot notification.** ~60 s after boot (never in Solo — no launcher, no services there) a one-shot
background task waits for the Wi-Fi STA link, then, at most once per 24 h (throttle persisted in
NVS), does a **~30-byte HTTPS GET of `version.json`** from the Pages site. That is the key economy:
the GitHub API answer is tens of KB the chip would have to buffer and parse; `version.json` is one
tag. The fetch is heap-gated and rides the arbiter (try-only) like every other TLS touch. A newer,
non-dismissed release becomes a system notification (`nucleo_notify_emit(src:"ota")` → native
banner/chime + `notify.post` to any connected browser). What it learns persists in NVS, so the
**boot dialog needs no network**: on the next boot `nucleo_update_dialog_pending()` (pure NVS +
`upd_should_show`) decides whether the run loop opens the Updates app before the launcher.

**Boot dialog (3 choices), as requested:**
- **Update now** → confirm → the app enters `NX_NET_APP` (~47 KB reclaimed, Wi-Fi stays up) and
  streams `nucleoos-latest-ota.bin` straight into the OTA slot with a live progress bar, SHA-256 +
  ESP-magic verified against `SHA256SUMS`. On success the device reboots itself; a bad image rolls
  back (bootloader rollback). Nothing to power-cycle by hand.
- **Next boot** (also Esc) → nothing persisted; the dialog returns on the next boot.
- **Ignore this version** → the tag is written to NVS `dismiss`; that exact tag never prompts again
  (a newer one still will).

**Manual entry.** The Updates app also lives in the System category: open it any time for "Check
now" and the same install flow.

**Why the device install mirrors the web install byte-for-byte:** same Pages host, same
`nucleoos-latest-ota.bin` (app-only, never the merged 0x0 image), same `SHA256SUMS`, same rollback.
Only the byte-mover differs (esp_http_client + esp_ota_write here; `POST /api/ota` there).

**Files:** `firmware/components/nucleo_app/nucleo_update.c` (engine: check + install workers),
`update_policy.c` / `update_policy.h` (pure decisions — the C twin of `update-core.js`),
`app_updates.cpp` (the dialog + installer UI, five languages), wired into the run loop and builtin
registry in `nucleo_app.cpp`.

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
  reboot verdict). Freezes the Settings poller during the flash.
- `.github/workflows/release.yml`, `.github/workflows/pages.yml` — asset production/republish.

## Tests

**Web:** `node --test tools/update-core.test.mjs` (in `npm run test:unit`): version parsing/compare
edge cases (dirty `*`, `+build.g<hash>`, `nogit`, 4-part tags), cadence, notify decision, conditional
fetch with fake responses (ETag, 304, 403/500/offline), SHA256SUMS parsing, and SHA-256 against the
FIPS vectors plus `node:crypto` across all padding regimes.

**Native:** `npm run update:test` (also in the ANIMA gate as `update-policy (release)`) compiles the
REAL `update_policy.c` with MinGW and proves the same triplet parse/compare, `version.json` +
`SHA256SUMS` parsing (strict shape, refuses truncation and an HTML error page), the show/dismiss/
dev-build-ahead dialog logic, and the 24 h throttle that can never wedge (never-checked and
clock-went-backwards are both "due"). 50 assertions, pure C, no device.
