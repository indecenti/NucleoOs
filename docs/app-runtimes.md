# App Runtimes

NucleoOS runs apps **without reflashing**, like Tactility/B3OS — but with a multi-runtime
model chosen to fit the Cardputer's hardware (no PSRAM) instead of fighting it. Each app
declares a `runtime` in its manifest; the OS hosts all tiers under one registry.

## The four tiers

| `runtime` | Where it runs | Reflash? | RAM on device | Use for |
|---|---|---|---|---|
| `web` *(default)* | Browser / PC (served from SD) | No | ~0 (just file serving) | Most apps; cross-device UI (PC + Android) |
| `vm` | On-device sandboxed bytecode VM | No | Tiny (KB) | Automations, simple on-screen apps, logic |
| `service` | Native C/C++ compiled into firmware | Yes | Static | Core/built-in apps, drivers |
| `elf` | Dynamically loaded native ELF from SD | No | **Needs PSRAM** | Heavy native apps — **PSRAM boards only** |

## Why this beats a pure-ELF approach on the Cardputer

Espressif's `elf_loader` runs ELF from **PSRAM** on the ESP32-S3. The Cardputer's M5StampS3
has **no PSRAM**, so native ELF apps would be limited to scarce IRAM — impractical for real
apps. Our answer:

- **`web`** already delivers "install/run apps without reflashing" with **zero device RAM
  cost** — the heavy UI runs in the browser/PC. This is the dominant tier and our edge.
- **`vm`** gives real *on-device* dynamic apps that fit in KB and are sandboxed by
  capability — the no-PSRAM-friendly equivalent of dynamic native apps.
- **`elf`** stays in the model as an **opt-in tier gated on PSRAM**, so NucleoOS scales up
  on bigger boards (CYD, S3 + PSRAM) without pretending on the Cardputer.

## App lifecycle & states (Android-like)

Apps move through `registered → starting → running → stopped/failed`. The runtime drives
each app through lifecycle callbacks: `onCreate → onShow → onHide → onDestroy`, plus
`onResult` when a launched app returns.

An app may launch another (by id **or by intent**) and receive a result back, over the
event bus. Results carry a status and an optional key-value bundle:

```
app.launch   { launch_id: 7, target: "photo-viewer", args: { path: "/data/x.jpg" } }
app.launch   { launch_id: 8, intent: { action: "edit", uri: "/data/notes.txt" } }
app.result   { launch_id: 7, from: "photo-viewer", status: "ok", data: {...} }
```

`status` is one of `ok` | `cancelled` | `error`. Launching by intent lets the OS pick the
handler app from manifest `intents` + file associations, so apps need not hard-code each
other's ids.

## Compatibility checks (load + install time)

Before enabling an external app the runtime verifies `platforms` (the SoC must match) and
`min_os` (the running OS must be new enough), exactly like a phone refusing an incompatible
APK. An SD card carried over from an older build therefore can't load an app it shouldn't.

## Third-party SDK

Apps are bundles validated against `schemas/manifest.schema.json`. The OS contract is:
the manifest (capabilities, routes, runtime), the event protocol (`docs/event-protocol.md`),
and the file API. NucleoOS is **MIT-licensed** (see `LICENSE`) so third parties can build
and ship apps freely.

## Case study: DOS emulation (why it's a `web` app)

A recurring "wow" request is running classic DOS software. There are two ways to do it,
and only one fits the Cardputer:

- **Native emulator in firmware** — a from-scratch x86 emulator (e.g. Tiny386, Faux86)
  must allocate ~1 MB of contiguous *guest* RAM for DOS real mode. The Cardputer's
  M5StampS3 has **512 KB SRAM and no PSRAM**, so that image can't fit. Projects that boot
  DOS/Win9x on an ESP32-S3 all rely on **PSRAM**. → only viable as a **PSRAM-gated tier**
  (like `elf`), e.g. on the CardputerADV with a PSRAM mod. Not for the standard board.
- **Client-side WASM** *(chosen)* — **js-dos** (DOSBox compiled to WebAssembly) runs the
  emulation **in the client browser** (PC / Android), where there are megabytes of RAM.
  The device only **stores the disk image on SD and streams the bytes** via `/api/fs/read`.
  Device RAM cost ≈ file serving. This is the **`web`** tier and the "Commodore trick" in
  action — exactly like Video Player decoding in the browser.

The **DOS Box** app (`apps/dosbox`, `runtime: web`) implements the second path: it lists
`/data/DOS` for `.jsdos`/`.zip`/`.img`/`.ima` images and boots the selected one in js-dos.
js-dos is GPL/LGPL but is *served and executed on the client*, never linked into our
MIT firmware — no license contamination. The loader defaults to the js-dos CDN and can be
**vendored on the SD card** (`apps/dosbox/www/vendor/`) for fully offline use.

## Positioning vs other ESP32 OSes

| | Tactility | B3OS | Tiny386 | **NucleoOS** |
|---|---|---|---|---|
| Apps without reflash | ELF (PSRAM) | yes | n/a (emulator) | **web + vm (no PSRAM) / elf opt-in** |
| Cross-device (PC + Android) | no | partial | browser demo | **yes, one shell** |
| Offline transports (BLE/USB) | no | no | no | **yes** |
| Multi-device swarm (ESP-NOW) | no | no | no | **yes** |
| Event-sourced + replay/undo | no | no | no | **yes** |
| Energy-aware scheduling | no | no | no | **yes** |
| Runs without PSRAM | limited | yes | **no** | **yes (by design)** |
