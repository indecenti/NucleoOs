# NucleoOS

A web-native appliance OS for the **M5Stack Cardputer** (ESP32-S3 / M5StampS3).
The real system runs on the device; the browser is the rich operator console.
Everything in this repository is **English only**.

## Hard constraints (design for the worst case)

- **RAM:** ~512 KB SRAM, **no PSRAM** on the standard module.
- **Bluetooth:** BLE 5.0 only (no Classic).
- **Battery:** ~120 mAh — energy is a first-class system resource.
- **Storage:** 64 GB microSD (exFAT or FAT32).

## Repository layout

```
docs/        Engineering specs (one focused file each)
schemas/     JSON Schemas — the shared contract for firmware, PC and Android clients
registry/    The live OS registry: installed apps, settings, file associations
apps/        One folder per app, each with its own manifest.json
firmware/    ESP-IDF firmware: boot, SD, setup wizard, registry, HTTP, mDNS, event bus, WebSocket, file API
web/shell/   The desktop shell PWA (icons, taskbar, Start, pinnable apps, windows)
windows-app/ NucleoConnect: .NET WebView2 connector with LAN device discovery
tools/       Dev tooling (validator, shell preview server)
```

## Docs index

| File | Topic |
|---|---|
| [docs/architecture.md](docs/architecture.md) | System layers and principles |
| [docs/memory-budget.md](docs/memory-budget.md) | How 512 KB is divided — the riskiest bet, validated |
| [docs/anima-native.md](docs/anima-native.md) | **Native ANIMA app baseline:** lifecycle, RAM reclaim, the online TLS anti-reboot contract, per-board (ADV), invariants |
| [docs/partition-table.md](docs/partition-table.md) | Concrete 8 MB flash OTA layout |
| [docs/storage.md](docs/storage.md) | SD filesystem: mount, first-boot provisioning, capacity detection |
| [docs/setup-wizard.md](docs/setup-wizard.md) | On-device first-run wizard (Wi-Fi/AP, device name), persistent |
| [docs/device-ui.md](docs/device-ui.md) | Device-first on-device UI: Wear OS-style launcher, single-app foreground, UX tricks |
| [docs/event-protocol.md](docs/event-protocol.md) | Delta event protocol shared by Wi-Fi / BLE / WebUSB |
| [docs/app-manifest.md](docs/app-manifest.md) | App bundle manifest schema |
| [docs/app-runtimes.md](docs/app-runtimes.md) | Multi-runtime app model (web/vm/service/elf) + positioning vs other ESP32 OSes |
| [docs/roadmap.md](docs/roadmap.md) | What's missing to be a real/modern OS: core gaps, apps, innovations, sequence |
| [docs/media.md](docs/media.md) | Playing MP3/MP4 on one ESP: client-decode strategy + on-device limits |
| [docs/registry.md](docs/registry.md) | Registry structure and file associations |
| [docs/releasing.md](docs/releasing.md) | Releasing & OTA: firmware vs web layer, channels, rollback safety, checklist |
| [docs/versioning.md](docs/versioning.md) | Firmware versioning: single source of truth, auto build counter, where the version shows (API/serial/mDNS) |
| [docs/security.md](docs/security.md) | Device pairing & session auth: PIN on screen, cookie sessions, what's gated |
| [docs/debugging.md](docs/debugging.md) | **Default dev loop:** run firmware logic on the PC (ANIMA host harness); JTAG/QEMU/Wokwi for the rest |

## Registry

| File | Holds |
|---|---|
| [registry/apps.json](registry/apps.json) | Installed apps index |
| [registry/file-associations.json](registry/file-associations.json) | Extension → default app (e.g. `txt`→Notepad, `jpg`→Photo Viewer) |
| [registry/settings.json](registry/settings.json) | System settings |

## Apps (v1 set)

File Commander · Notepad · Photo Viewer · Media Player · Automation Studio ·
IR Remote · Swarm · Settings · Log Viewer — see [apps/](apps/).

## Validation

The registry and every manifest are validated against `schemas/` by a zero-dependency
Node script. Run it whenever you add or change an app:

```
npm run validate        # or: node tools/validate.mjs
```

It checks schema conformance plus cross-references (every association points to an
installed app, every app declares the extensions it is the default handler for).

## Debugging (default: run firmware logic on the PC)

Don't iterate by flashing. The default loop compiles the **real** ANIMA firmware C and runs
it on the PC — no device, no Wi‑Fi, no cable:

```
npm run anima -- "che cos'è nucleoos"   # one‑shot
npm run anima                           # interactive REPL
```

Flash only to *confirm*. For device state without Wi‑Fi (JTAG/GDB), the full real firmware
(QEMU), or display + internet (Wokwi), see **[docs/debugging.md](docs/debugging.md)**.

> The original Italian planning document is kept as `piano-cardputer-os.md`. It can be
> translated/retired on request; all OS artifacts going forward are English.
