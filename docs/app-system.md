# The app system: where it is going

How NucleoOS on the Cardputer gets, runs, updates and tests apps, and the plan to take it from "apps
compiled into the firmware" to a professional, scalable platform. Status markers: **done**, **next**,
**planned**. Store details: [`app-store.md`](app-store.md); runtime tiers today: [`app-runtimes.md`](app-runtimes.md).

## Principles (non-negotiable)

1. **Signed or nothing.** Every app a device installs is signed offline by the maintainer after review;
   no one else can publish. The device and the web shell both verify (ECDSA P-256; WebCrypto in the browser).
2. **No bad surprises on update.** The rules in `app-store.md` "Upgrade guarantees": two releases to move an
   app out of the firmware, tiles never vanish, user data untouched, no partition change over the air.
3. **Every app is a first-class citizen**: its own drawn icon, five languages, the Cardputer UI rules
   (`device-ui.md`, keyboard support), works offline.
4. **Tested on hardware**, on both boards (original + ADV), before it is signed — native apps above all.
5. **Format compatibility forever.** Catalog and package formats only ever append fields; readers ignore
   what they do not know; a breaking change gets a new magic string. Old firmware and old web shells keep
   working against the store.

## App kinds

| Kind | Runs | RAM on the device | Status |
|---|---|---|---|
| `web` | in the browser, served from SD | ~0 | **done** (store installs them) |
| `data` | content for other apps (books, maps, packs) | ~0 | **done** (format), UI next |
| `native` | native code executed from flash (XIP) | only the app's globals | **planned** (spike first) |
| built-in | compiled into the firmware | static | today's native apps; they leave as `native` matures |

## One store, every client — **next**

The same signed catalog (`indecenti/nucleoos-cardputer-store`) serves:
- the native **Store** app on the Cardputer (**done**: sections, details, install/update/remove, update all, QR);
- the **web shell** store page in the browser (**next**): fetch `index.txt`, verify `index.sig` with WebCrypto
  against the same compiled-in public keys, install through the device file API, which verifies again;
- any future NucleoOS web OS version: it reads the catalog by the format rules above.

Store improvements (**next**): a small RGB565 icon per app in the catalog folder so the device list shows
real icons; screenshots for the web page; an optional "featured" list; an update badge on the launcher at
boot; opt-in automatic updates; a beta channel (a second signed catalog).

## Native apps from the SD card — **working on both boards; not yet released**

Spec: [native-apps.md](native-apps.md). Installed on the SD card only; at launch the signed `.nap` module is
re-verified, relocated into the 256 KB `coredump` partition (core dumps are off) and executed from flash
through the MMU, so an app costs RAM only for its own variables. ABI level 1 = what the built-in games use
(195 functions); the layout fingerprint pins the shapes of the shared types; a native game gets the same Solo
boot as a built-in one. First apps: Constellations (`stelle`, 104 KB module, run on both boards) and Poker (37 KB). Next: the release that
ships the loader, then the one that drops them from the image.

## Testing — **next**

- **PC first** (exists): host harnesses compile the real code (`ui:shots`, `store:test`, `presence:test`,
  per-game tests). Every new app adds its scenes to `ui-host` (5 languages x 4 themes, layout checks).
- **On the device, automated:** `tools/device-smoke.mjs` opens every native app through the launcher with
  `/api/input`, presses a fixed key script, captures `/api/screen`, and fails on a reboot (uptime reset),
  a heap leak across open/close, or a screen identical to the launcher. Apps that turn the web server off
  (games in Solo boot, network apps in exclusive mode) are driven over the **USB serial test channel**: a
  `KEY <name>` / `SHOT` line protocol compiled in **only** when `CONFIG_NUCLEO_TEST_CHANNEL=y` (test builds;
  never in a release image — the release script refuses an image with it).
- **Release gate:** device smoke green on both boards, from a card with real user data, as an upgrade from
  the previous public release.

## Developer workflow — **next**

`python tools/store.py new <id> --kind web` scaffolds an app that already passes every rule (manifest,
icon placeholder that must be replaced, five catalogs); `store.py lint <id>` runs the rules alone;
a local preview server mirrors the device paths (`/nucleo-i18n.js`, `/apps/<id>/`).

## Order of work

1. Device-verify the Store client (catalog download, install, update, remove) on both boards.
2. Device smoke test (HTTP part), then the serial test channel and the Solo/exclusive apps.
3. Web shell store page (WebCrypto verification).
4. Native apps: run the loader on both boards (Constellations), then the release with the loader, then the
   release that drops Constellations from the image (docs/native-apps.md "Migration").
5. Recover flash for the M5Launcher slot (the image is over it today), helped by step 4.
