# Firmware versioning

One version string identifies the running firmware, derived from a **single source of truth**,
auto-incremented on every build, and reported identically by the API and the serial console.
The goal: read the version from `/api/status` *or* from COM and they always agree, and the number
moves whenever you build — so you can tell at a glance which image is on a given Cardputer.

> Scope: this is the **firmware** version. The web layer (shell/apps on SD) versions separately —
> `web/shell/sw.js` cache tag + per-app `manifest.json` `version`. See [releasing.md](releasing.md)
> for why the two layers are independent.

## The version string

```
<semver>+<build>.g<git-short>[*]
   │        │        │        └─ '*' iff the tree had uncommitted source changes at build time
   │        │        └────────── short commit hash of HEAD
   │        └─────────────────── monotonic build counter, ++ on every scripted build
   └──────────────────────────── human-set release version (major.minor.patch)
```

Example: `0.2.0+7.g1a2b3c4` — semver `0.2.0`, 7th build since the last semver bump, commit `1a2b3c4`,
clean tree. A trailing `*` (`0.2.0+7.g1a2b3c4*`) means it was built from uncommitted code.

It fits the ESP-IDF app-descriptor cap of 32 chars (composition is truncated as a guard).

## Single source of truth

Two tiny files under `firmware/version/` hold the only hand-or-tool-managed numbers:

| File | Meaning | Who writes it |
|---|---|---|
| `firmware/version/VERSION` | semver, e.g. `0.2.0` | `version-bump.ps1 -Bump <part>` on a real release |
| `firmware/version/BUILD` | build counter, e.g. `7` | `version-bump.ps1` (default), auto, every build |

`firmware/version/version.cmake` reads those two files, adds the git short hash + dirty flag, and
sets **`PROJECT_VER`** *before* ESP-IDF's `project.cmake` (the only point where it's honoured —
see ESP-IDF "App version" docs). ESP-IDF bakes `PROJECT_VER` into the application descriptor.

Everything that reports a version reads it back from there via `esp_app_get_description()->version`
— so they **cannot disagree**:

| Surface | Where |
|---|---|
| `GET /api/status` → `version` (+ `built`) | `nucleo_httpd.c` `status_get` |
| `GET /proc/version` and `/proc/uname` | `nucleo_httpd.c` |
| `GET /api/diag` → `sys.fw` | `nucleo_httpd.c` `diag_get` |
| **serial boot banner** (`NucleoOS <ver> | proj … | built … | idf …`) | `main.c` `app_main` |
| mDNS `_nucleoos._tcp` TXT record `ver` | `nucleo_discovery.c` |
| ANIMA "che versione?" (web tool + native app) | `nucleo_httpd.c`, `app_anima.cpp` |
| SD `volume.json` `os_version` (at provisioning) | `nucleo_storage.c` |

## How the counter moves on every build

`tools/flash.ps1` runs `tools/version-bump.ps1` before building (unless `-NoBump`). That writes the
new `BUILD`. `version.cmake` lists `VERSION`/`BUILD` under `CMAKE_CONFIGURE_DEPENDS`, so a changed
counter forces a CMake reconfigure and the new number (and fresh git hash) land in the app
descriptor for that build. Since `release.ps1` builds through `flash.ps1`, releases bump too.

The build-counter churn is **excluded** from the dirty check (`git status … :(exclude)firmware/version`),
so bumping `BUILD` every build never makes the tree show as dirty — only real uncommitted source does.

## Workflows

**Just build / OTA (day to day).** Do nothing special — the counter auto-increments:
`0.2.0+5.g… → 0.2.0+6.g…`. After an OTA, `release.ps1` prints `was vX -> now vY (OTA confirmed)`
by re-reading `/api/status` through the reboot — proof the new image took.

**Cut a real release (semver bump).** Bump the semver first (resets the build counter to 0), then
release, then commit the `firmware/version/*` change so the number is monotonic in history:
```
powershell -ExecutionPolicy Bypass -File tools\version-bump.ps1 -Bump patch   # 0.2.0 -> 0.2.1, build 0
powershell -ExecutionPolicy Bypass -File tools\release.ps1
```
`-Bump minor` / `-Bump major`, or `-Set X.Y.Z` to set it explicitly.

**Dual release (both Cardputers).** Build once, ship the same bin to both so they stay on the SAME
version. OTA path: `release.ps1` for unit A, `release.ps1 -SkipBuild` for unit B (no rebuild, no
bump). Serial path: `flash.ps1 -Port COM3` then `flash.ps1 -Port COM4 -NoBump` (the `-NoBump` keeps
the second flash on the same version instead of bumping to a higher build number). Verify by
confirming both units report the same `/api/status.version`. See
[`.claude/skills/nucleo-release-dual`](../.claude/skills/nucleo-release-dual/SKILL.md).

## Reading the version on a device

```
# Over Wi-Fi
curl http://<ip>/api/status        # { "version": "0.2.0+7.g1a2b3c4", "built": "...", ... }
curl http://<ip>/proc/version      # NucleoOS version 0.2.0+7.g1a2b3c4 (esp32s3) SMP cores=2

# Over COM (no network) — the boot banner, first INFO line after reset:
#   NucleoOS 0.2.0+7.g1a2b3c4 | proj nucleoos | built <date> <time> | idf v5.4 | esp32s3
```

## Gotchas
- The version only changes for a build that **reconfigures** — that's why the bump goes through a
  `CONFIGURE_DEPENDS` file. A bare `idf.py build` with no `version-bump` run keeps the old number
  (intentional: throwaway dev builds don't churn the version).
- `firmware/version/BUILD` shows as modified after every build — that's expected; commit it with
  releases. It's the one tracked file the dirty check ignores.
- App-descriptor version caps at 32 chars. The composition stays well under it; keep semver sane.
