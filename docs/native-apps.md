# Native apps from the store

Apps written in C/C++ for the Cardputer, installed from the store onto the SD card and run on the device
itself, with the same API and the same look as the apps built into the firmware. They are how the firmware
gets smaller (the games alone are ~480 KB of it) and how new apps ship without a firmware release.

Store side: [indecenti/nucleoos-cardputer-store](https://github.com/indecenti/nucleoos-cardputer-store),
channel `native` (`apps/native/<id>/`). Store format and the web channel: its `docs/format.md`.

## What a native app is

A `.nap` file: an ELF32 Xtensa shared object (position independent, `-fPIC`) linked with
`sdk/native/nap.ld` into exactly three load segments, and nothing else:

| Segment | Flags | Holds | At run time |
|---|---|---|---|
| text | R+X | code, PLT stubs, literal pools, the dynamic tables | flash scratch region, instruction bus |
| rodata | R | constants, strings, jump tables | the same flash pages, through the data bus |
| data | RW | GOT, init/fini arrays, `.data`, `.bss` | a heap block, allocated at load |

It imports firmware functions **by name** from the ABI export table and exports two symbols:

- `nucleo_native_info` — a `nap_info_t` (plain data, read before anything runs): magic `NAP1`, ABI level,
  flags, layout fingerprint, extra RAM it needs, and its store id;
- `nucleo_native_main` — called once after relocation; it calls the app's usual registration function, whose
  `nucleo_app_register` the firmware captures.

So porting an existing app is no rewrite: the same source file, the same `nucleo_app_def_t`, the same
`d.fillRect(...)`, compiled a second way. `sdk/native/include/nucleo_native.h` has the details.

## The ABI

`sdk/native/abi/v<N>.txt` lists what a module may import: libc/libm/libgcc helpers, the app framework
(`nucleo_app_*`), drawing (the LovyanGFX methods and fonts, `gui::`, `lg::`), keyboard, touch, IMU, audio,
files, `pnet` multiplayer, cJSON, time. Level 1 is everything the built-in games use, minus what is unsafe for
a module (below). `tools/native/gen-exports.mjs` turns it into `firmware/components/nucleo_app/nap_exports.S`,
a sorted `{name, address}` table the loader binary-searches.

The rules that keep an installed app working across OS updates:

1. **A level only grows.** A name, once listed, keeps its signature and meaning in every later firmware. New
   names mean a new level: `v2.txt` = `v1.txt` + additions (the generator refuses a level that drops or
   reroutes a name). A firmware exports every level up to its own; a module built for a higher level than
   the firmware has is refused at install (`ST_E_ABI`) and at load.
2. **A name the firmware does not have fails the firmware build** (the table is assembly: an unknown symbol
   is a link error), never an app on a user's device. Every name in v1 was checked this way.
3. **`name = target`** exports a firmware guard under the usual name: `nucleo_app_register` is
   `nucleo_native_capture_register`, which accepts a registration only during `nucleo_native_main`.
4. **The layout fingerprint.** Inline code in a module (LovyanGFX methods, struct fields) bakes in the shape
   of firmware types. `nucleo_native_layout()` hashes the LovyanGFX version and the size and key offsets of the
   shared types (`LGFXBase`, `LGFX_Sprite`, `nucleo_app_def_t`, `gui::Menu`...) — not the ABI level, which only
   adds names, so a new level never invalidates the modules already installed. Module and firmware compute it
   from the same header; a different value is refused at load. When a firmware update changes it, the store
   rebuilds every native app from its recipe and publishes them with the firmware.
   Level 2 (2026-10-27) = level 1 + the second-screen helpers the games use (`lg::tap`, `lg::field`,
   `lg::frame` with the touch flag, `nucleo_touch_add_pad`, `nucleo_touch_add_scroll`).

Not in v1, on purpose: the web server (`httpd_*`: a module's handlers would outlive it, and the web OS's
catch-all route is registered last, so a late endpoint is never reached), task creation, raw radio, OTA,
NVS, the registry, partitions. A module cannot outlive its own foreground life except through what the
framework already bounds (handlers it sets are cleared when the app closes).

## Building one

In the store repository:

```
python tools/native_build.py <id> --firmware ../NucleoOs           # writes apps/native/<id>/files/app.nap
python tools/native_build.py <id> --firmware ../NucleoOs --check   # rebuilds and compares, byte for byte
```

The recipe is the `build` object of the store manifest: repository, commit, sources, registration function.
The firmware checkout must be at that commit, with those sources and `sdk/native/` unmodified, and
configured (`idf.py reconfigure`): each source is compiled with **the firmware's own flags for that file**
(from `compile_commands.json`) plus `-fPIC -fvisibility=hidden -DNAP_MODULE`, then linked with `nap.ld`.
Every import is checked against the ABI level the manifest names. Prefix maps keep paths out of the
output, so the same commit and toolchain give the same bytes: anyone can check a published module against
its source. `--allow-dirty` builds from a modified checkout for testing; such a build records
`<commit>-dirty` and the store refuses to sign it.

## Running one

`firmware/components/nucleo_app/native_apps.cpp`, with the portable half in `nucleo_native/nap_loader.c`.
One native app is resident at a time.

1. **Signature again.** `/sd/apps/<id>/app.nap` is re-checked against the signed package it was installed
   from (`.package.sig`, kept beside it), with the keys compiled into the firmware: a card edited on a PC
   never runs code the store did not sign. The signed SHA-256 is kept for step 7.
2. **`nap_inspect()`**: ELF32 LE Xtensa `ET_DYN`; exactly the three segments, file offset = address; only
   the dynamic tags the SDK's link produces (no `DT_NEEDED`, `DT_INIT`, TLS...); every relocation of an
   allowed type (`RELATIVE`, `JMP_SLOT`, `GLOB_DAT`, `32`, `RTLD`), aligned, inside the module (no offset that
   can wrap around); every import in the export table; `nucleo_native_info` valid; ABI level, layout
   fingerprint and id as expected; sizes within the scratch region and the RAM budget. The relocation tables
   are then read once into RAM for the passes below.
3. **Map.** The scratch region is the 256 KB `coredump` partition (core dumps to flash are off in this
   firmware: the build fails if they are turned on). It is mapped with `esp_mmu_map` on the instruction
   bus; on the ESP32-S3 the MMU table is shared by both buses, so the same pages are also visible through the
   data bus at a fixed offset, which serves the module's constants. `.data`/`.bss` get a heap block.
4. **Dry pass.** The whole image is relocated in RAM sector by sector and hashed: every relocation target is
   checked here, before the flash is touched.
5. **Write** — unless the last sector of the region already says it holds exactly this image: same module, same
   firmware (its ELF SHA-256), same instruction-bus, data-bus **and RAM** addresses (Xtensa literal pools in the
   code hold absolute `.data` addresses) and same image hash. Otherwise only the sectors whose content differs
   are erased and written. On the device the `.data` block lands elsewhere at almost every launch (Wi-Fi
   allocates concurrently in the Solo boot), and that changes just the sectors whose literal pools point into
   `.data`: measured on Constellations, **2 of 21 sectors** per launch, ~0.6 s. At 100k erase cycles per
   sector that is tens of years of daily use. The header is erased before and written after, so an
   interrupted write can never vouch for a half-written image; a log line says what changed and how many
   sectors were rewritten.
6. **Check what the CPU will see.** Flash writes flush only the instruction-bus side of the cache, so the data
   cache is invalidated over the data-bus alias (and on unload, for the next module at the same address);
   then the whole image is hashed through the data bus and must equal the dry pass.
7. **Data and start**: `.data` relocated; the file is hashed again and must still be the signed one (not swapped
   while loading); constructor, destructor and entry pointers must point into the module's code; constructors
   run; `nucleo_native_main` registers the app. The launcher slot of the store app (`store_apps.cpp`) gets the
   module's callbacks; the store keeps the identity (id, name, category, icon, description), so nothing the
   launcher keeps ever points into a module. Long passes feed the task watchdog.

A native **game** gets the same fresh-heap Solo boot as a built-in game (the store apps are registered in
that boot too), so it loads on an unfragmented heap. A crash inside an app resets the device into the full
OS, as for every app: the Solo latch only survives a clean software reboot.

Unloading (another native app needs the region, or the store updates or removes this one) puts the slot's
placeholder back first, then runs destructors and releases the mapping and the RAM. Switching from one app to
another drops every hook the outgoing app set (poll, Back, TAB, push-to-talk, a latched GO hold) before the
incoming module may be loaded, so no hook can jump into code that is no longer mapped. The store never swaps
the files of a loaded module (`nucleo_native_forget` first), and refuses to unload the app in the foreground.

**On the second screen: the system bar.** A native app with `on_draw_large` that is in front on the external
panel gets a 30 px system bar at the top — a Back button (the same pill every built-in app has) and the app's
name; a tap anywhere on it is Back. The app's surface is the rest of the panel: `d.width()` x `d.height()` is
320 x 210 and its touch targets are in that surface's own coordinates (the firmware shifts the taps), so an app
needs no code for it. A module that draws its own Back opts out with `NAP_FLAG_NO_HUD` in
`NUCLEO_NATIVE_APP_EX(..., flags, ...)`: then it gets the whole 320 x 240. Built-in apps never get the bar —
they draw their Back themselves (`lg::header` / `lg::back`). Leaving the large surface (the Control Center,
an overlay) takes the bar away with it.

**Where native apps are unavailable:** a partition table without a 64 KB-aligned `coredump` partition — in
practice, under M5Launcher (it recreates only SPIFFS/FAT partitions). The Store then shows the native store
as unavailable, with the reason, and the web store as usual.

## Security model

A native module is machine code in the same address space as the firmware: there is no MMU sandbox on this
chip, so a module can do anything the firmware can. Trust therefore comes from where the code comes from,
and every step is checked:

- only the maintainer signs (offline key); the device installs only what verifies with the compiled-in keys;
- the published module names the commit it was built from and rebuilds byte for byte from it, so the
  source that was reviewed is the code that runs;
- the file is re-verified on the device before every load, and validated structurally before it is mapped;
- the ABI is the whole interface: no symbol lookup beyond the table, no way to reach unlisted functions by
  name, and the dangerous families are not listed.

## Games that ship from the store

| Game (id) | Module | In the firmware | Kconfig |
|---|---|---|---|
| Constellations (`stelle`) | 104 KB (82 KB code + constants, 2.9 KB RAM) | ~68 KB | `CONFIG_NUCLEO_STORE_CONSTELLATIONS` |
| Tanks (`tanks`) | 96 KB (75 KB, 2.1 KB RAM) | ~70 KB | `CONFIG_NUCLEO_STORE_TANKS` |
| Poker (`poker`) | 37 KB (29 KB, 0.7 KB RAM) | ~20 KB | `CONFIG_NUCLEO_STORE_POKER` |

With all three from the store (the default) the image is **3,461,120 B instead of 3,603,792 B: 142,672 B (139 KB)
recovered**
(208 KB free in the OTA slot instead of 66 KB). Every game except the Game Boy emulator already fits ABI v1
(measured: all their imports are in `abi/v1.txt`); the emulator needs 77 more functions, a v2 candidate.

Moving one more: add `CONFIG_NUCLEO_STORE_<GAME>` to the menu in `nucleo_app/Kconfig`, the same three-line guard
at the top of its source and around its registration in `nucleo_app.cpp`, then in the store a manifest with its
recipe and a launcher icon, `tools/native_build.py`, `store.py publish`.

## Leaving the firmware

Decision (2026-10-27): Constellations, Tanks and Poker leave the image in the same release that ships the
loader (`CONFIG_NUCLEO_STORE_*` default `y`), not one release later. Updating to it removes them from the
device until they are installed from the Store (Native C); their launcher id, Recents/pins and saves under
`/sd/data/` stay, so an install brings them back exactly where they were. The Store is the second entry on
Home, right after ANIMA, so the way back is one step away. Turning an option off builds that game in again.

## Verification

- `npm run native:test` — the export table is current; a real module (`tools/native-host/nap/fixture.c`,
  with constructors, destructors, data imports, a jump table) is built with the SDK and relocated by the real
  loader, and every relocated word is compared with an independent computation from `readelf`; 23 targeted
  corruptions are refused with the right reason; 4000 random ones never crash it (UB and bounds traps on);
  the same on every module in the store checkout beside this repository.
- `npm run store:test` — the store engine over a real two-channel store built and signed by the store's
  own tools: install, re-verification on the card, ABI and channel refusals, tampering.
- An independent review of the loader against the ESP-IDF 5.4 sources (MMU, cache, flash driver) found 13
  issues before the first hardware run; all fixed (RAM base in the cache key, data-cache invalidation, full
  read-back hash, offset overflow, hooks on app switch, re-hash of the file, call targets, watchdog).
- **Installed from the published store over HTTPS (2026-10-27)**, on the ADV: native catalog refreshed and
  verified, Constellations, Tanks and Poker updated 1.0.0 -> 1.1.0 ("update all"), every operation rc=0 with
  3.1-3.6 KB of the 7 KB store task stack left; the updated Constellations (ABI 2) then loaded and ran.
- **All three games verified on hardware (2026-10-27)**, on both boards, with a test firmware that has none of
  them built in: Tanks, Poker and Constellations opened one after another from the launcher, each replacing
  the previous module in the scratch region (`tanks loaded ... 75648 B`, `poker loaded ... 29116 B`,
  `stelle loaded ... 82644 B`), no panic on either board. Not exercised: playing them with keys (the Solo boot
  has no web server to inject keys; a person at the device), Tanks' two-player LAN mode.
- **First verified on hardware (2026-10-27)**, on both boards (original Cardputer and ADV), with a test firmware built
  with `CONFIG_NUCLEO_STORE_CONSTELLATIONS=y` (Constellations only from the store; image 67 KB smaller):
  `stelle` installed on the SD with its signed package, opened from the launcher search into its Solo boot,
  verified, mapped, written and executed (`stelle loaded: abi 1, 82644 B code+const, 2888 B RAM`, ~0.8 s),
  then relaunched after a reset: data block elsewhere, 2 sectors rewritten, loaded again; no panic in 30+ s
  of play on either board. Not yet exercised there: the install path over HTTPS (the published store is not
  pushed yet; the engine is host-tested), Poker (built in on this firmware), the store-side unload when
  switching between two native apps.
