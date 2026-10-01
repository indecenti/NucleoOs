# SD content self-install

Status: design approved 2026-09-30. **Implemented**: the pipeline (§2: `sd_deploy.py release`,
`tools/package-release.mjs`, `release.yml`, `pages.yml`, gated by `tools/sd-payload.test.mjs`) and the
device side (§3, 2026-10-02: `firmware/components/nucleo_sdcontent/`, the wizard step, Settings ▸ Device ▸
SD content), host-gated end to end (§6). Not yet: the web rescue page button, the optional packs on the
device (only `core` is installed), the apps.json *merge* (the registry is written only when absent).

Goal: a Cardputer that received **only the firmware** (M5Launcher OTA, M5Burner, web flasher) can fetch
its SD content ("payload": web shell, web apps, registry, ANIMA data, wallpapers) **by itself over
Wi-Fi**, verified, resumable, and without ever touching user data. The existing manual path (release
`-sd.zip`, dev `deploy.ps1` / `sd-sync.ps1` / `push-ota --sync` / `sd_deploy.py`) keeps working
unchanged; this adds one more consumer of the same payload definition.

## 1. Findings that shape the design

- **The release payload is stale today.** `tools/package-release.mjs` copies `deploy/sd-safe`, a
  hand-maintained snapshot nothing regenerates (www/shell 27 files vs 90 in `web/shell`; 34 of 49
  apps; registry 48 files differ). It also ships runtime state (`data/anima/session.txt`,
  `telemetry.ndjson`) and zips under a top-level `sd/` folder (`release.yml:92`), so "extract to the
  card root" yields `/sd/apps`… and a device that sees no payload.
- **The canonical, cross-platform assembler already exists:** `tools/nucleo-sd-deploy/sd_deploy.py`
  (`SOURCE_MAP` = payload, `DEVICE_STATE` = preserved, `COMPLETENESS`, `update` = write-only-if-different,
  never delete; stdlib Python, runs on the Linux CI runner). `deploy.ps1` is Windows-only.
- **Ownership:** no top-level SD tree is pure payload. Five mixed places need per-file rules:
  `/system/registry/apps.json` (the Agent app upserts user apps, `created_by:"agent"`), `/apps/`
  (agent-created apps, `/apps/theme.cfg`, `/apps/<id>/data/`), `/data/anima/` (brain files next to the
  API-key vault `teacher.json` and learned caches), `/data/anima/learned/` (read-only `facets.*.jsonl`
  seeds next to runtime caches), `/data/tts/` (banks in `it/`,`en/`, runtime cache in the root).
  The firmware's `nucleo_fsprotect.h` guards delete/move only — **overwrite safety is entirely on the
  installer**.
- **Device building blocks all exist, never combined yet:** Solo boot with Wi-Fi STA up and the canvas
  released (~31 KB largest block, enough for one TLS session — `app_updates.cpp`), one persistent
  `esp_http_client` handle reused for many requests (`nucleo_anima_online.c:1501-1594`, the transcriber's
  `tx_conn_t`: drain every body, never close between requests, re-dial once on a dropped keep-alive),
  streaming SHA-256 (`nucleo_update.c`), FAT atomic write (`.part` → remove → rename, orphan promotion —
  `nucleo_gg.c:1337-1357`), free-space gate + chunk-malloc fallback (`nucleo_fsapi.c:268-284`), line-by-line
  manifest parsing on a stack buffer (`nucleo_fsfactory.h`), direct-draw progress UI and 5-language `UT()`
  literals, FAT flush on `esp_restart` (`nucleo_storage_sync` shutdown hook).
- **Constraints:** `SHA256SUMS` is read into a 2 KB buffer by the firmware updater — per-file lines must
  NOT go there. The ANIMA index is read once at boot → an install ends with a reboot. `.gz` twins are
  served first → a file and its twin are always written (or the stale twin removed) together.

## 2. What gets built and published (pipeline, no device change)

1. **Assemble from sources in CI**: `sd_deploy.py assemble` (the SOURCE_MAP) → staging tree, then filter:
   the existing `HEAVY` rules, every `DEVICE_STATE` path (so `session.txt`/`telemetry.ndjson` can never
   ship again), and `COMPLETENESS` must pass. `deploy/sd-safe` stops being a release input (it stays a
   dev asset source for what sd_deploy.py already pulls from it).
2. **Packs**: each file is tagged with a pack. `core` (required: shell, web apps without heavy vendors,
   registry, ANIMA base, facets seeds, IR presets, wallpapers) and optional packs (e.g. `arcade`
   emulators, `anima-plus` extended knowledge shards). TTS voice banks (~400 MB each) are **out of scope**
   here (see §7).
3. **Manifest** `sd-manifest.txt` — one line per file (sorted by path), SHA256SUMS-style so the device
   parses it with `fgets` on a stack buffer and no cJSON:
   ```
   #nucleoos-sd 1 <tag> <total_files> <total_bytes>
   #pack <name> <files> <bytes>                      (one per pack)
   <sha256-lowercase> <size> <pack> <mode> <sd/relative/path>
   ```
   `mode` = `w` (write if different), `c` (create only if absent — seeds), `m` (merge — `apps.json`).
   Paths are validated at build time against the same allow-list the device enforces (§4).
4. **Hosting**: GitHub Pages (same host the firmware updater already trusts, CDN, no per-IP API limit),
   per-file under `/sd/<x.y.z>/…` plus `/sd/<x.y.z>/sd-manifest.txt` and `/sd/index.txt` (hosted
   versions). Each release attaches its `-sd.zip` and `-sd-manifest.txt`; `pages.yml` re-extracts the
   last three releases that carry a manifest and verifies every file against it before deploying, so a
   device a couple of versions behind still finds content matching its firmware.
5. **The zip keeps existing**, built from the same staging tree (so it is finally current) and zipped
   **without** the `sd/` prefix. FLASH.md is rewritten: stand-alone, M5Launcher, and "the device can
   download its SD content itself".
6. **The zip carries its own manifest** at `system/content/manifest.txt` (a copy of `sd-manifest.txt`,
   not listed in itself; added by `package-release.mjs`). A card filled **by hand** from the zip is then
   recognised by the device, with its release (§3.1).

## 3. Device side (as built)

Code: `firmware/components/nucleo_sdcontent/` — `content_policy.c` (pure: manifest parser, path allow-list,
plan), `sdc_engine.c` (pure C + stdio: status, fetch, verify, install, resume; the host gate compiles the
very same file), `sdc_sha256.c`, `nucleo_sdcontent.c` (device glue: HTTPS transport, NVS, 5-language
messages). UI: `nucleo_ui_modal.cpp` (install screens), `app_wifi.cpp` (wizard step, Settings row).

### 3.1 What the card holds — read from the files, never assumed

`sdc_content_status()` (no network) classifies the card, in this order:

| Status | When | Offered? |
|---|---|---|
| `COMPLETE` | `system/content.json` says complete for this firmware (installed + verified by the device) | no |
| `PARTIAL` | `content.json` for this firmware, not complete (a run started and was cut) | **resume** |
| `MANUAL` | the web OS is on the card (`www/shell/index.html[.gz]`) and `system/content/manifest.txt` names this release — a hand copy of the zip | no |
| `OUTDATED` | the web OS is there, from another release (by the manifest beside it, else by `content.json`) | **update** |
| `UNKNOWN` | the web OS is there, nothing names its release (a dev `sd-sync`, an old zip) | no |
| `SKIPPED` | nothing there, the user chose "Later" for this firmware | no |
| `MISSING` | nothing there | **download** |

"Offered" (`sdc_content_needed`) is what the first-boot wizard acts on: only MISSING, PARTIAL and OUTDATED.
A hand copy is never nagged about, and a stale "Later" never hides real files. Settings ▸ Device ▸ **SD
content** shows every state and runs the same install (download / resume / update / **verify and
repair**) for all of them. The engine marks a run as started (`content.json` complete:false) as soon as it
accepts the manifest, so an interrupted run is a resume, never mistaken for a hand copy.

### 3.2 Updating is incremental

Every run walks the manifest and, per file, compares size + SHA-256 of what is on the card: a match is
skipped (hashed, not downloaded), anything else is fetched. An **update** from an older release, a
**resume** after a cut, and a **verify** of a hand copy are therefore the same run: only missing or
changed files cross the network (S17 in the e2e gate: one changed file → exactly one download). Files
the new release no longer ships stay (harmless, §7).

### 3.3 The install boot

The download is **armed** (NVS `sdc/arm`) and the device restarts into a dedicated minimal boot
(`main.c`): only the SD, the UI language and a **STA-only** Wi-Fi link come up — no httpd, mDNS, ANIMA,
apps, splash or registry. The arm flag is cleared first, so a crash can never loop.

- **RAM.** The 32 KB UI canvas and TinyUSB's 19 KB NCM buffer (`nucleo_usbnet_reclaim`) are held while
  Wi-Fi associates and handed to the heap right before the TLS handshake
  (`nucleo_sdcontent_on_net_ready`), so the handshake gets intact blocks; a heap gate (22 KB largest
  block) refuses to dial a starved heap. The progress screen draws **direct** to the panel (no
  back-buffer). The heap at each stage is persisted (`/api/status` `sdc_heap`, e.g.
  `boot 41/31 | link 30/18 | tls 79/32 | min 9`, free/largest KB) because this boot has no httpd.
- **TLS records.** This chip's mbedTLS receives records of at most 8 KB
  (`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192`, kept low so ANIMA online survives in the full OS). GitHub
  Pages sends long bodies in 16 KB records (measured), so a plain GET dies right after HTTP 200. Every
  request is a **6 KB Range window** (a response that small never produces a record over the cap), all on
  **one keep-alive connection** (re-dialled only when the server closes it).
- **Screens** (5 languages, all themes, `ui:shots` `wizard.install-*`): "Connecting to the network..."
  (walking bar) → "Downloading and verifying": big percentage **by bytes**, MB done / total, files, time
  left (from this run's network rate, shown after 10 s), the file in flight, hint "do not switch off: keep
  it plugged in". Incremental repaint at ≤ 4 Hz: only changed fields, fixed-width opaque glyphs — no
  flicker. Then the verdict: **Installed** (files, MB, "Web OS and ANIMA are ready", restarts after 6 s)
  or **Not finished** (the localized reason, "Files already done are kept", "Retry: Settings > Device",
  restarts after 2 min or ENTER). The failure reason also goes to NVS (`/api/status` `sdc_diag`).

### 3.4 Entry points

- **First-boot wizard**, after a successful join (setup-wizard.md step 4): the page names the case —
  download (~50 MB) / update (only the changed files) / resume — with [action] [Later]. **Later** explains
  the two ways to get the files afterwards: Settings ▸ Device, or **by hand**: the release's SD zip
  unzipped at the card's root (recognised on its own, §3.1).
- **Settings ▸ Device ▸ SD content**: the status as a value (OK, v0.4.2, incomplete, missing, present)
  with what ENTER does; a confirm, then the install boot. Needs a Wi-Fi link (the row says so).
- Web rescue page: not yet (§ status).

### 3.5 Copying by hand

1. Download `nucleoos-<version>-sd.zip` from the GitHub release matching the firmware.
2. Unzip its **contents** at the root of the FAT32 card (`www/`, `apps/`, `data/`, `system/`, … at the
   top; the zip has no `sd/` folder). Copying over an existing card is fine: it holds no user state —
   except `data/anima/learned/facets.*.jsonl`, seeds the device may have edited; skip those if asked.
3. Boot: the device reads `system/content/manifest.txt` and shows the card as MANUAL (this release) or
   OUTDATED (an older zip → it offers the update). Settings ▸ Device ▸ SD content ▸ verify checks every
   file against the manifest and repairs what is off.

## 4. Safety rules (enforced on the device, host-tested; defense in depth over the build-time filter)

- **Never delete** user files, never prune a directory, never mirror. The only removals: our own `.part`
  files, and a device `.gz` whose raw sibling we just wrote but the manifest has no twin for (the
  anti-shadowing rule `push-ota.mjs` already uses).
- **Allow-list of writable roots**: `www/shell/`, `apps/<id>/` for ids the manifest ships (never
  `apps/theme.cfg`, never `apps/<id>/data/`), `system/registry/`, `system/ir/`, `data/anima/`
  **brain files only** (`anima-*`, `dict-*`, `commands*`, `akb5/`), `data/anima/learned/facets.{it,en}.jsonl`,
  `wallpapers/`, `evilportal/`. Anything else in a manifest → the line is refused and the run fails
  loudly (a bad manifest must not write anywhere).
- **Deny-list** = the shared device/user-state table `tools/lib/sd-policy.json` (every SD tool reads it;
  the device installer will be held to the same `tools/lib/sd-policy-vectors.json`), e.g. `system/config`,
  `system/keys`, `system/sessions`, logs, `data/anima/teacher.json`, learned caches, `data/tts/` root, …
- **Path hygiene**: relative, no `..`, no backslash, no leading `/`, ≤ 200 chars (+`.part` fits the 256
  buffers), printable ASCII.
- **apps.json merge** (mirrors `planRegistryUpdate`, `apps/agent/www/app-publish.js:366-384`): start
  from the release `installed[]`; carry every device entry with `created_by:"agent"` whose id is not a
  system id; keep the user's `enabled` and granted `permissions` on bundled apps. If the device file is
  unreadable or malformed → **skip the merge**, keep the device file, store the release copy as
  `apps.json.release`, and report it (the OS keeps working with the old list).
- **Shared cards (M5Launcher)**: write only inside the roots above; create no new top-level folder
  beyond what provisioning already makes.

## 5. Failure handling (every path leaves a working device)

| Failure | Behaviour |
|---|---|
| No SD / not FAT32 | not offered; the progress run fails with "No SD card" / "SD not writable"; Settings row says "No SD card" |
| No Wi-Fi | dialog points to Settings ▸ Wi-Fi; nothing armed |
| Pages unreachable / 404 manifest | "content for vX not available yet" — retry later; zip link shown |
| Pages lagging the release | `version.json` guard (same as the updater): retry in minutes |
| Firmware older than the 3 hosted versions | "update the firmware first" (self-OTA, or Launcher OTA when hosted) |
| TLS OOM / handshake fails | heap gate before dialing; one re-dial; then clean fail, nothing half-written |
| Keep-alive dropped mid-run | re-dial once and continue at the current file |
| Hash mismatch | retry that file once; then fail the run, file never installed |
| SD full (pre-check or ENOSPC) | stop; files already installed are complete; report MB needed |
| Power loss / Esc / crash | complete files stay; `.part` cleaned next run; resume skips verified files |
| apps.json unreadable | merge skipped, device registry kept, reported |
| Stuck transfer | per-file stall timeout + whole-run ceiling; WDT fed every chunk |

Esc during the run warm-reboots to the full OS (like the updater's Solo boot) — always safe.

## 6. Tests

Host (no device), in the ANIMA gate and CI:
- `npm run sdcontent:test` (59 checks): `content_policy.c` — header/line parser, path hygiene, the write
  allow-list against every row of the ownership map, plan decisions, version matching.
- `npm run sdcontent:e2e` (153 checks): the **real** `sdc_engine.c` against a fake Pages server that
  enforces the device's limit (no response over one window) and injects faults, a temp dir as the SD.
  Fresh card, rerun no-op, resume after every kind of cut (mid-file link loss, corrupt byte, 404,
  manifest 404), hostile manifest (writes nothing), tag mismatch, user state byte-identical, no `.part`
  left, progress by bytes monotonic and moving inside a big file, the status table of §3.1 including a
  hand copy (MANUAL), a newer firmware over it (OUTDATED → only the changed file downloads), a dev tree
  (UNKNOWN), an interrupted run (PARTIAL), "Later" (SKIPPED); FIPS SHA-256 vectors and the real release
  manifest parsed and hash-compared.
- `npm run ui:shots`: the wizard step (missing / update / resume, focus moved, Later page), Settings ▸
  Device ▸ SD content (missing / OK / hand copy / old / confirm), the install screens — 5 languages × 4
  themes, direct and buffered paths.
- `tools/sd-payload.test.mjs`: the release tree vs its manifest, allow-list, no device state.

Device (manual, release checklist): both boards; fresh FAT32 card; Launcher-shared card; card with user
data; Wi-Fi drop mid-run; power-off mid-run → resume; a hand copy is not re-offered; an old zip offers the
update and fetches only the difference; `/api/status` `sdc_heap` / `sdc_diag` after each run.

## 6b. Under M5Launcher

The Launcher installs only the firmware (its OTA list or SD installer); the SD payload is ours, exactly
as for a web-flasher install — which is why this self-install exists. Specifics:
- The card is **shared** with the Launcher: the installer writes only the allow-listed roots (§4) and
  never formats or prunes; "Erase SD card" / "Wipe everything" in Settings ▸ Reset say "M5Launcher too"
  on a hosted device.
- Under the Launcher the USB port is switched away from the serial console, so the install boot is
  diagnosed over the network afterwards (`sdc_diag`, `sdc_heap` in NVS → `/api/status`).
- A Launcher OTA of a newer NucleoOS makes the card OUTDATED: the next first boot (or Settings ▸ Device)
  offers the update, which fetches only the changed files.
- The install boot is an ordinary `esp_restart` — the Launcher's bootloader keeps NucleoOS selected
  (only power-on / deep-sleep wake show the Launcher), so arm → install → full OS never leaves NucleoOS.

## 7. Out of scope (for now)

- TTS voice banks (~400 MB each, already split in ≤90 MiB Release parts by `oversized-assets/`): later
  as an optional long download from Release assets; meanwhile the UI must say "voice not installed"
  instead of staying silent.
- Deleting files the release no longer ships (orphans are harmless; `.factory`/registry decide what shows).
- Signing the manifest beyond HTTPS + SHA-256 (same trust level as the firmware updater today).

## 8. Decisions (2026-09-30)

1. **Lean `core` pack** (~51 MB, 1261 files) + optional `arcade` (emulator cores, ~20 MB) and `downloads`
   (phone APK / Windows exe, ~7 MB). The Arcade app must say "emulator pack not installed" when its cores
   are missing (phase 3).
2. **AKB5 = the shipped 47-shard manifest** (`deploy/sd-safe/data/anima/anima-it-akb5.bin`, what devices
   run today via `deploy/sd`) and exactly the shards it routes to. The 15 extra `tools/sd-sim` shards are
   unreferenced by it and are not shipped.
3. **Hosting: GitHub Pages, last 3 releases**, per file under `sd/<x.y.z>/`.
4. **TTS voice banks out of scope** for now; the UI must say "voice not installed" instead of silence.
5. **Entry points**: the first-run wizard (a step after the Wi-Fi join, when online with an SD card and no
   content), the boot dialog, Settings ▸ SD, and a line on the web rescue page.
