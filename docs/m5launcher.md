# M5Launcher compatibility (guest mode)

NucleoOS ships **one binary** that works in two worlds:

| | Stand-alone (default) | Installed by M5Launcher |
|---|---|---|
| Installed with | web flasher, esptool, M5Burner "Burn" (full 0x0 image) | [M5Launcher](https://github.com/bmorcelli/Launcher) OTA list, SD card or GitHub link |
| Bootloader + partition table | ours (`firmware/partitions.csv`) | Launcher's (Launcher in a `test` slot, apps in `ota_N` slots) |
| Firmware updates | self-OTA (Updates app, Settings ▸ Updates, `POST /api/ota`) with A/B rollback | **from the Launcher** (its OTA list); self-OTA is refused |
| Getting back to another firmware | reflash | Settings ▸ Device ▸ **Back to M5Launcher**, or power-cycle + ENTER |

Nothing is configured: the mode is detected at runtime from the live partition table
(`firmware/components/nucleo_guest/`). Our own table never contains an app partition of subtype
`test`, so a stand-alone install can never be mistaken for a hosted one — that invariant is gated
on the real `partitions.csv` by `npm run guest:test`.

## How M5Launcher boots (what guest mode relies on)

Verified against the Launcher sources (bmorcelli/Launcher, Sept 2026) and its bootloader patch
(bmorcelli/myLibBuilder, branch `launcher_keyboot`,
`patches/esp-idf/components/bootloader/subproject/main/bootloader_start.c`):

- The Launcher lives in an app partition of subtype **`test`** at `0x10000`. Installed firmwares
  get `ota_0`, `ota_1`, … slots behind it, sized to the image, plus the data partitions they declare.
- Its **bootloader** boots the Launcher on a **power-on reset** and on a **deep-sleep wake**; every
  other reset (`esp_restart`, panic, watchdog, brownout) boots the app selected in `otadata`. So our
  warm reboots (Solo, FIDO mode, USB drive, reset) stay inside NucleoOS, exactly as stand-alone.
  - Launcher setting `DDLB` (NVS `launcher/DDLB`, u8): deep-sleep wake no longer boots the Launcher.
  - Launcher setting `LauncherOnKey` (NVS `launcher/LauncherOnKey`, i32 > 0): ONLY a held GPIO boots
    the Launcher — power-on and deep sleep both go straight to the app.
  - The patch sets `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=n`, so a deep-sleep wake really
    re-runs partition selection (it is not short-circuited back to the sleeping app).
- On power-on the Launcher shows its splash for `1 s + boot timer`; **ENTER** opens its menu, any other
  key or the timeout boots the selected app with a **software reset** (`esp_restart`). Consequence:
  under the Launcher every cold boot of NucleoOS reports `ESP_RST_SW`. All our RTC one-shot flags
  (Solo, return cursor, BLE/Wi-Fi keep-once, FIDO, recorder reopen) are magic-guarded, so the
  garbage RTC contents of a real power-on can't fire them.
- Installing a merged image (Launcher `src/sd_functions.cpp`, `updateFromSD`) reads the table
  embedded at `0x8000`: the **first** app partition of subtype factory/ota_0/test is the firmware
  (ours: `ota_0` at `0x20000`), and every SPIFFS/LittleFS/FAT data partition is recreated **with its
  label**. Empty ones (ours: `cfg`) are created at the Launcher default size (0x70000 = 448 KB on
  8 MB flash). The LauncherHub OTA path (`api.launcherhub.net`) pre-computes the same plan server-side.

## What changes in guest mode

- **No self-OTA.** `esp_ota_get_next_update_partition()` under the Launcher returns *another installed
  app's* slot (or our own running one). Writing it would destroy that app, so the native installer
  (`nucleo_update.c`), `POST /api/ota` (→ `409 {"error":"hosted","host":"m5launcher"}`) and the web
  updater (Settings ▸ Updates, which checks `ota.self_update` first) all refuse and point to the
  Launcher. The daily release check still runs and still notifies — only the install path moves.
- **No rollback.** The Launcher's bootloader writes `otadata` with state `UNDEFINED`, so our
  `PENDING_VERIFY` → confirm/rollback flow is a no-op (it already is for any non-pending image).
  `/api/status` reports `ota.rollback_enabled:false`, `ota.self_update:false`, `ota.host:"m5launcher"`.
- **Back to M5Launcher** (Settings ▸ Device, shown only when hosted): a 100 ms timer deep sleep, which
  the Launcher bootloader turns into a Launcher boot. The user then presses ENTER on the splash
  (otherwise the Launcher boots NucleoOS again after its timer — we deliberately never write the
  Launcher's own settings to change that). If `LauncherOnKey` or `DDLB` make a software hand-off
  impossible, the row says what to do instead (hold the key / switch off and on).
- **Shared NVS.** The `nvs` partition belongs to the Launcher too (its settings + app list). NucleoOS
  only *reads* the `launcher` namespace. The boot-time NVS recovery (erase on `NO_FREE_PAGES` /
  `NEW_VERSION_FOUND`) still runs — it is the only way back to a usable NVS — but logs loudly that it
  also resets the Launcher's settings. The Launcher resets `nvs.net80211` itself on install, so Wi-Fi
  is re-joined by our own `/cfg` network list.
- **Smaller `cfg`.** 448 KB instead of 640 KB; it holds small JSON config, pins/recents and the music
  DB, well within that.

Not handled by the Launcher (same as a web-flasher install): the **SD payload** (shell + web apps +
ANIMA data). NucleoOS fetches it **itself** over Wi-Fi — the first-boot wizard offers it right after the
Wi-Fi join, Settings ▸ Device ▸ SD content any time — or it can be copied by hand from the release's
`nucleoos-*-sd.zip` (recognised by the manifest it carries). Details, including what is specific to a
Launcher-shared card: [`sd-content-install.md`](sd-content-install.md) §3 and §6b. The native OS runs
without it.

**Size budget.** The Launcher sizes each app slot to the image it installs (ours: `nucle1`/`nucle2`,
0x310000 = 3,211,264 B on the ADV with the current Launcher). An update larger than the slot the Launcher
created cannot be installed in place, so the image is kept under it: after the TinyUSB buffer reclaim
(memory-budget.md) the image is 3,201,088 B (2026-10-02), ~10 KB of headroom. Check
`build/nucleoos.bin` against the slot before publishing.

## Publishing so it shows up in the Launcher's OTA list

The Launcher's online list is LauncherHub, which mirrors the **M5Burner** community catalog
(category `cardputer` covers the original and the ADV). There is no pull request to open: a firmware
published on M5Burner appears in the Launcher automatically. (The Launcher's "starred" shortlist is
curated by its author.)

1. Cut a release as usual (`docs/releasing.md` §3). Use the release's **merged image**
   `nucleoos-<version>.bin` (0x0: bootloader + our partition table + app). It serves both paths:
   M5Burner "Burn" flashes it whole (stand-alone mode); the Launcher extracts the app + `cfg` from it
   (guest mode).
2. In the M5Burner desktop app, sign in with the project's M5Stack account → *Share* / upload a
   firmware: device **Cardputer**, name **NucleoOS**, version = the release tag, the merged `.bin`,
   description (below) and screenshots from `docs/screenshots/`.
3. After the catalog refreshes, verify with
   `curl "https://api.launcherhub.net/firmwares?category=cardputer&q=nucleoos"` and install it from a
   Launcher once (Launcher ▸ OTA ▸ search "NucleoOS").

Suggested description: *NucleoOS — an appliance OS for the Cardputer (original + ADV, one binary):
native apps, offline assistant, web console. Works stand-alone or under M5Launcher (updates via
Launcher OTA). License: PolyForm Noncommercial 1.0.0 — commercial use needs a license (COMMERCIAL.md).
SD content: see the GitHub release.*

The upload itself is a manual, account-bound step (M5Burner has no public upload API) — it is never
automated from this repo.

## Testing

- Host: `npm run guest:test` — the real `guest_policy.c` against the real `partitions.csv` plus a
  Launcher-written table (also in the ANIMA gate and CI `gate.yml`).
- No serial console under the Launcher (it hands the USB port to the app, which switches it): diagnose a
  hosted device over the network — `/api/status` (`sdc_diag`, `sdc_heap`, `ota.host`) and `/api/logs`.
- Device (manual, when releasing): install the merged image from the Launcher's SD installer →
  NucleoOS boots, Settings ▸ Device shows *Back to M5Launcher*, `/api/status` has
  `ota.host:"m5launcher"`, the Updates app points to the Launcher, *Back to M5Launcher* + ENTER lands
  in the Launcher menu, and a power-cycle without keys lands back in NucleoOS. Then flash the same
  image stand-alone and confirm the row is gone and `ota.self_update:true`.
