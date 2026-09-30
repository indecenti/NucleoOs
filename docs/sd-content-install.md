# SD content self-install

Status: design approved 2026-09-30. **Phase 1 (pipeline, §2) is implemented**: `sd_deploy.py release`,
`tools/package-release.mjs`, `release.yml`, `pages.yml`, gated by `tools/sd-payload.test.mjs`. The device
side (§3–§6) is not built yet.

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

## 3. Device flow

- **Detect** (boot, NVS/SD only, no network): read `/system/content.json` (`{tag, packs, complete}`),
  written only at the end of a successful install. Missing, incomplete, or `tag` ≠ running firmware
  triplet → "SD content missing / out of date".
- **Offer**: boot dialog in the same slot/order as the update dialog (`nucleo_app.cpp:~1593`):
  `Download now (~N MB) / Later / Don't ask again`, plus a permanent Settings ▸ SD row
  ("Download / repair SD content") and a line + link on the web rescue page.
- **Run** (armed in NVS, then Solo boot — exactly the updater's pattern):
  1. gates: SD mounted & FAT, Wi-Fi STA IP, arbiter `ARB_FG`, heap largest-block gate, Pages
     `version.json` matches, manifest for **our** tag exists;
  2. fetch the manifest to `/system/content/manifest.<tag>.txt` (`.part` + rename);
  3. plan (pure C policy): for each line → skip if on-disk size+SHA match, else download; free-space check
     on the bytes still needed + margin;
  4. download over **one** persistent TLS connection: stream → SHA-256 + write `<path>.part` → verify →
     remove target → rename; write/remove the `.gz` twin consistently; progress per file and bytes;
  5. `apps.json`: merge (§4) — never overwrite;
  6. write `/system/content.json` last, then `esp_restart()` (flushes FAT, reloads ANIMA).
- **Resume**: any interruption leaves only complete files + `.part` leftovers; the next run re-plans
  from the manifest (verified files are skipped), deletes stale `.part`s. No journal needed.
- **Update**: a newer firmware (self-OTA or via Launcher) makes `tag` mismatch → the same flow
  downloads only changed files.

## 4. Safety rules (enforced on the device, host-tested; defense in depth over the build-time filter)

- **Never delete** user files, never prune a directory, never mirror. The only removals: our own `.part`
  files, and a device `.gz` whose raw sibling we just wrote but the manifest has no twin for (the
  anti-shadowing rule `push-ota.mjs` already uses).
- **Allow-list of writable roots**: `www/shell/`, `apps/<id>/` for ids the manifest ships (never
  `apps/theme.cfg`, never `apps/<id>/data/`), `system/registry/`, `system/ir/`, `data/anima/`
  **brain files only** (`anima-*`, `dict-*`, `commands*`, `akb5/`), `data/anima/learned/facets.{it,en}.jsonl`,
  `wallpapers/`, `evilportal/`. Anything else in a manifest → the line is refused and the run fails
  loudly (a bad manifest must not write anywhere).
- **Deny-list** mirrors `sd_deploy.py DEVICE_STATE` / `push-ota isDeviceState`: `system/config`,
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
| No SD / not FAT32 | not offered; Settings row explains (FAT32 required) |
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

## 6. Test plan

Host (no device), all in CI gate + ANIMA gate:
- **Policy C gate** (`content_policy.c`, pure): manifest header/line parser (truncation, bad hex, bad
  size, unknown mode), path hygiene + allow/deny lists (table of every row in the ownership map), plan
  decisions (skip/write/create-only/merge/.gz-twin), version matching, free-space math.
- **apps.json merge gate**: host-compiled with the IDF cJSON (as ui-host does) — agent apps kept,
  `enabled`/`permissions` kept, malformed device file → skip, system id shadows agent id.
- **End-to-end fault-injection harness**: the engine behind a small I/O seam (http fetch + fs ops),
  compiled on the PC against a local HTTP fixture and a temp dir as "SD". Scenarios: fresh card, foreign
  Launcher card, card with user data in every mixed dir, interruption after every Nth chunk, corrupted
  byte, disk-full at N, dropped keep-alive, stale `.gz` twin. Assert: final tree == manifest for payload
  paths, every user/state file byte-identical, no stray `.part`, rerun is a no-op.
- **Pipeline test**: assembled tree passes `COMPLETENESS` + `check-gz`, contains zero `DEVICE_STATE`
  paths, manifest matches the tree, zip has no `sd/` prefix, size budget per pack.
- **UI**: `ui:shots` scenes for the dialog, progress, done and each failure screen (5 languages × themes).

Device (manual, release checklist): both boards; fresh FAT32 card; Launcher-shared card; card with
user data; Wi-Fi drop mid-run (AP off); SD nearly full; power-off mid-run → resume; measured throughput
and total time for `core`; web console works after reboot; rerun downloads nothing.

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
