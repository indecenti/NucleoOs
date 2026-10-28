# App store (Cardputer)

Apps and games move out of the firmware into a signed store, installed on the SD card. Store repo and
format: [indecenti/nucleoos-cardputer-store](https://github.com/indecenti/nucleoos-cardputer-store)
(`docs/format.md` there is the contract the firmware client implements). Served from
`https://indecenti.github.io/nucleoos-cardputer-store/`.

Why: the image is over the M5Launcher app slot (docs/m5launcher.md "Size budget"), and the native games
alone are ~480 KB of it (Constellations 74, Tanks 66, Tank Duel 43, Brawler 41, Game Boy 31, ...).

## Upgrade guarantees (non-negotiable)

Updating NucleoOS must never cost a user an app, a save or a working device. Every change on this
path is checked against these rules before it ships:

1. **Two releases, never one.** Release N ships the store client (and installs nothing by itself).
   Only release N+1, after N has been on devices, removes an app from the firmware - and only an app
   whose store package exists, is signed, and has been installed and run on both boards (original + ADV).
2. **Nothing disappears.** An app that left the firmware keeps its launcher tile. Until it is
   installed, the tile opens its store page with one "Install" button; once installed it opens the app.
   On the first boot after the update, if Wi-Fi works, the apps the user had used (recents, pins) are
   reinstalled automatically, with progress shown; otherwise one notice explains it, once.
3. **User data is never touched.** Saves, settings and records keep their paths. Installing, updating or
   removing a store app only ever writes `/sd/apps/<id>/`.
4. **No network required.** The SD content zip of each release also carries the store packages, so a
   user who copies the card by hand gets the same apps offline.
5. **No partition change over the air.** The flash layout stays as it is for every OTA. Native
   flash-executed games (`kind: native`) need an "apps" partition; they ship only with a migration that is
   safe without a cable and leaves a bootable device at every step, or not at all.
6. **Rolling back is harmless.** An older firmware ignores `/sd/apps/` packages it does not know and
   still has its own built-in apps.
7. **Under M5Launcher** the image must fit the Launcher slot (`npm run image:check`); the store is how it
   gets back under it, never a reason to waive the gate.
8. **Verified on hardware.** Each release on this path is tested as an upgrade from the previous public
   release on both Cardputers, with existing saves on the card, before it is published.

## Firmware client

`components/nucleo_store` (engine, host-tested by `npm run store:test`) and the Store app
(`nucleo_app/app_store.cpp`). Two stores, chosen on the Store's first screen and never mixed: **Native C**
(channel `native`: apps that run on the Cardputer, [native-apps.md](native-apps.md)) and **Web app** (channel
`web`: apps served to a browser, and content packs). Each channel is its own signed catalog, with its channel
name inside the signed text, its own serial and its own state under `/sd/apps/.store/<channel>/`.

Streams `index.txt`/`index.sig` to the SD and verifies them chunk by chunk (mbedTLS ECDSA P-256, keys
compiled in: main + backup), keeps the highest accepted `serial`, refuses expired catalogs when the
clock is set, then reads the catalog one line at a time. Installs into `/sd/apps/.tmp/<id>/`, checks
every file against the signed `package.sig`, then swaps the folder in and keeps the signed package as
`.package.sig` (a native app is re-verified against it before every load). RAM: a few KB of buffers, no
JSON tree.

**Store art.** Each channel also publishes `art.bin` + `art.sig` (format in the store's `docs/format.md`): the
apps' colour icons, painted for the store, 48 x 48 RGB565. After a good catalog the engine fetches them only when
`art.sig` changed and keeps them only when the signature verifies; a missing pack, a failed download or a bad
signature leave the previous art (or none) and never fail the refresh. The UI draws an icon a row at a time
straight from the SD card, box-filtered to the size it needs (no decoder, no image in RAM: ~0.5 KB of stack), and
falls back to the category tile with the app's initial. Older firmware and the web shells never ask for the pack.

**The Store's two looks.** On the Cardputer alone: two-line rows (icon, name, size and category, a state chip) under
the section tabs, the app page with its icon. With the external panel the panel is the store: the apps as a touch
list beside the selected app's card (icon, facts, description, the Install / Update / Open button), Categories as a
grid of coloured tiles, and the Cardputer shows the selected app in full (name, state, facts, description).

**Status (2026-10-27):** client, two channels and native loader written and host-tested; the loader runs
Constellations from the SD card on both boards (docs/native-apps.md, "Verification"). Not yet in a release. Native apps need the `coredump` partition as their flash scratch region:
unavailable under M5Launcher, where the Store shows the native store as such.
