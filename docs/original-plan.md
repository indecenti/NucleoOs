# NucleoOS — a web-native, swarm-based, energy-aware operating system for the Cardputer

_Historical document: the original execution plan NucleoOS started from. The current architecture lives in the other docs/ files; this is kept for context and may be out of date._

This document defines a realistic execution plan for building a real embedded operating system — **NucleoOS** — that is reachable via the web, practically useful, and sustainable on the M5Stack Cardputer. The plan is split into two deliberately separate parts: a **deliverable v1 core** (realistic, no frills) and a **phased innovation roadmap** that makes the project something that does not yet exist in this class of device.

## Cardputer hardware reality (verify your own variant)

| Resource | Specification | Design implication |
|---|---|---|
| SoC | ESP32-S3 dual-core @ 240 MHz (M5StampS3 module, ESP32-S3FN8) | Plenty for tasks + networking + minimal UI |
| Flash | 8 MB | Firmware + 2 OTA slots + NVS: adequate space |
| **RAM** | **~512 KB internal SRAM, NO PSRAM on the standard module** | **Dominant constraint.** The Wi-Fi stack (~50-100 KB) and the display framebuffer compete for SRAM. No heavy frameworks, no generous buffers. |
| Mass storage | **64 GB microSD (hot-swap)** | Ample space for journal, apps, backups, cache. **Note: a 64 GB card ships as exFAT** → enable exFAT in ESP-IDF or reformat to FAT32 |
| Display | 240×135 TFT (ST7789) | Minimal local UI: pairing, status, recovery, QR |
| Input | 56-key physical keyboard | Real local control, not just a demo |
| Connectivity | 2.4 GHz Wi-Fi + **ESP-NOW** (peer-to-peer) + **BLE 5.0** | ESP-NOW for the router-less mesh; **BLE is the only wireless channel that the Android browser can also reach (Web Bluetooth)** |
| Extra I/O | **IR TX**, PDM microphone (SPM1423), I2S speaker (NS4168), Grove | Native capabilities for automation and voice |
| Power | ~120 mAh battery, USB-C | **Scarce energy: under 1h with Wi-Fi active.** Energy is a system resource. |

> ⚠️ **Two important notes.**
> 1. **PSRAM:** the standard Cardputer module **has no PSRAM**. We design for the worst case, "only 512 KB SRAM" (a safe choice that works on every variant); if your revision includes PSRAM, it is used as an opportunistic cache for sessions/assets without changing the architecture.
> 2. **Bluetooth:** the ESP32-S3 has **only BLE 5.0, not Bluetooth Classic** (no BR/EDR, no A2DP/SPP). This is not a limitation: BLE is exactly what is needed, because it can be reached via **Web Bluetooth** from the browser on Android as well.

## Project goal

The goal is not to imitate Linux or Windows in miniature, but to create an embedded **appliance OS** that manages services, app bundles, storage, automations, games, and a remote workstation via the browser. The web UI is not a simulation of the OS: the real system runs on the device, while the browser is the advanced operating terminal for control, installation, file management, and live monitoring.

### Expected outcome of v1

By the first useful version the system must allow:

- reliable boot and emergency recovery;
- access to files on the microSD from the device and from the browser;
- installation and removal of app bundles with a manifest;
- a web dashboard with live status, logs, and events;
- OTA firmware update with rollback;
- a first truly useful app (automations / file workflows);
- **browser access even without Wi-Fi**, via **BLE / WebUSB** (including from Android) and Web Serial on PC (see Phase 1 innovations);
- a **keyboard-and-mouse-first** interface, usable in Android desktop mode.

## Architectural principles

The project is guided by a few non-negotiable principles:

1. **ESP-IDF as the single base** for core, networking, tasking, and update.
2. **System in flash, content on SD**: the firmware stays in flash; apps, data, logs, and assets live on the microSD.
3. **Remote UI over HTTP + WebSocket** for live state and bidirectional commands — with **delta sync**, never full-state (RAM constraint).
4. **Capability-based app model**: each app declares permissions, events, mount points, web routes, and energy cost.
5. **Recovery first**: every update must be able to fail without bricking the device.
6. **Vertical slice before complexity**: every phase produces something provable and usable.
7. **Energy is a system resource**, not a detail: every service declares and respects a budget.
8. **Everything is an event**: state changes go through an append-only event bus → observability, replay, undo.

## Proposed architecture

### System layers

| Layer | Function | Initial implementation |
|---|---|---|
| Boot & recovery | Startup, firmware validation, rollback | ESP-IDF bootloader + OTA partitions |
| Core services | Wi-Fi, auth, **event-sourced event bus**, log, storage, **power manager**, scheduler | C/C++ on ESP-IDF |
| App runtime | App lifecycle, manifest, mount, permissions, capabilities | JSON registry + service launcher |
| Storage | Config, app bundles, data, logs, assets, **event journal** | microSD + system areas |
| Transport | Multi-channel administrative access | HTTP+WS (Wi-Fi), **BLE GATT**, **WebUSB**, **Web Serial** — same event protocol |
| Local UI | Pairing (QR), setup, recovery, minimal notifications | Native UI on the 240×135 display |
| Web workstation | Dashboard, file manager, installer, logs, admin | Lightweight HTML/CSS/JS served by the device (PWA) |

## Access and interfaces: PWA-first, multi-transport, desktop-ready

### Decision: a PWA, not a native app (with an optional Android companion)

The primary interface is a **PWA** (Progressive Web App) served by the device, not a native Android app. Reason: a single codebase covers PC and Android, natively supports mouse and keyboard, is installable, works in **Android desktop mode** (resizable window, cursor, multi-pane), and reaches the Cardputer over multiple transports — Wi-Fi *and* without a network. A **native Android app remains optional**, as a *thin companion* only for what the browser cannot do in the background (persistent USB/BLE bridge, always-on ESP-NOW gateway).

### Adaptive transport (the PWA picks the best available)

All channels speak **the same delta event protocol**. The PWA negotiates in order:

| Priority | Channel | Available on | Use |
|---|---|---|---|
| 1 | **Wi-Fi (HTTP + WebSocket)** | anywhere there is a network | full rate, multi-session |
| 2 | **BLE GATT (Web Bluetooth)** | Chrome desktop **and Android** | wireless **without a router**, covers Android |
| 3 | **WebUSB** | Chrome desktop **and Android** | over USB-C cable, no network |
| 4 | **Web Serial** | Chrome desktop | cable fallback on PC |

> Key point for Android: **Web Serial does not exist on Android**, but **WebUSB and Web Bluetooth do**. That is why BLE is first-class in the firmware: it is the universal wireless channel, reachable from the phone without any network.

### Mouse, keyboard, and Android desktop mode

The web shell is designed **keyboard-and-mouse-first**, so that in desktop mode (Android DeX-style or an external monitor, and of course on PC) it behaves like a real administration console:

- **keyboard shortcuts** for frequent actions (file navigation, commands, log filter);
- **drag & drop** of files from the operating system to the Cardputer's SD (upload);
- **context menus** (right click) and resizable **multi-pane**;
- multiple selection, copy/paste, full navigation **without touch**;
- the Cardputer's 56-key keypad remains the **local** input; the mouse and keyboard of the Android/PC device are the rich **remote** input.

Bonus enabled by the hardware: via **BLE HID** the Cardputer can, as a dedicated capability, also behave as a *Bluetooth keyboard/remote* toward the phone — useful for automations and "presenter" scenarios.

### App model (extended manifest)

Each app is a bundle with this minimal structure:

```text
/apps/<app-id>/
  manifest.json
  manifest.sig        # Ed25519 signature of manifest+contents
  www/
  data/
  assets/
  rules/              # automation bytecode (optional)
```

Initial manifest, with NucleoOS's new capabilities:

```json
{
  "id": "automation-studio",
  "name": "Automation Studio",
  "version": "0.1.0",
  "entry_service": "automation_main",
  "web_route": "/apps/automation-studio/",
  "permissions": ["storage.app", "system.events", "device.keyboard", "device.ir"],
  "mounts": {
    "app": "/apps/automation-studio/data"
  },
  "subscribes": ["system.boot", "wifi.status", "storage.changed", "ir.received"],
  "publishes": ["automation.rule_fired"],
  "power": {
    "budget_class": "low",
    "wants_wakeup": ["timer", "keyboard", "ir"]
  },
  "mesh": {
    "exposes": ["automation.rule_fired"],
    "consumes": []
  }
}
```

The manifest now also declares the **energy profile** (`power`) — used by the power manager to decide wakeup and sleep — and any participation in the **mesh** (`mesh`). This enables a true application registry with logical isolation, installation, updating, debugging, *and* energy management.

## Partitioning and storage

ESP-IDF requires at least two OTA partitions (`ota_0`, `ota_1`) and an `otadata` partition for updates with rollback: the first serious choice is to define a custom table instead of ad-hoc layouts.

### Memory strategy (constrained: 512 KB SRAM, no PSRAM)

- **Flash:** bootloader, partition table, `nvs`, `otadata`, `phy_init`, minimal `factory`, `ota_0`, `ota_1`.
- **SRAM (~512 KB) — strict discipline:**
  - **mostly static** allocation for core services (no heap fragmentation);
  - **a single** partial framebuffer for the display (LVGL/direct partial refresh), not full-frame;
  - **no full-state sync** over the WebSocket: only delta events, compactly serialized;
  - a maximum of 1-2 guaranteed concurrent web sessions; the others best-effort;
  - fixed-size circular audio/log buffers.
- **microSD:** `/system`, `/apps`, `/data/shared`, `/logs`, `/journal` (event sourcing), `/www-cache`, `/backups`.

### Recommended SD layout

```text
/system/
  config/
  registry/
  sessions/
  keys/            # device key + trusted keys for signed bundles
  logs/
/journal/          # append-only event log (replicable source of truth)
/apps/
  automation-studio/
  file-commander/
  game-runtime/
/data/
  shared/
  imports/
  exports/
/backups/
/www/
  shell/           # installable PWA, works offline
```

## Product backlog: what to do first (CORE v1)

### Deliverable 1 — Foundation Boot

Goal: get the core running and mount the SD reliably.

Checklist:
- repository with `firmware/`, `web/`, `docs/` structure;
- clean ESP-IDF build on the Cardputer;
- custom OTA partitions;
- SD mount and read/write test;
- serial logger and persistent file logger;
- local boot/recovery screen.

Criterion: the device boots, shows status, mounts the SD, and generates persistent logs.

### Deliverable 2 — Core Services

Goal: make the system a real runtime, not just firmware.

Checklist:
- service manager with states `registered`, `starting`, `running`, `failed`, `stopped`;
- **central event-sourced event bus** (every event appended to the journal on SD);
- JSON configuration management;
- service health checks;
- internal APIs for logs, notifications, and storage;
- **minimal power manager**: per-state consumption accounting, sleep thresholds.

Criterion: a demo service starts, emits events, is restarted on error, and the events can be reconstructed from the journal.

### Deliverable 3 — Web Workstation

Goal: browser access as the primary administration interface.

Checklist:
- basic HTTP server;
- REST status endpoints;
- WebSocket for live **delta** events;
- dashboard with system info;
- real-time log viewer;
- Wi-Fi/storage/**battery** diagnostics page;
- shell served as a **PWA** (offline cache).

Criterion: connecting via browser shows status, logs, and changes without full refreshes.

### Deliverable 4 — File Commander

Goal: real day-to-day use of the device right away.

Checklist:
- SD directory listing;
- file upload / download via browser;
- rename/delete/create dir;
- text preview;
- local + web notifications on file events.

Criterion: the Cardputer becomes a pocket appliance for moving, reading, and organizing files.

### Deliverable 5 — App Registry

Goal: move from single utilities to a modular platform.

Checklist:
- manifest parser;
- `/apps` scan;
- bundle install/uninstall;
- app start/stop;
- "Installed apps" web page;
- startup error handling.

Criterion: at least two apps in the registry, launchable and removable without reflashing.

### Deliverable 6 — First app of value: Automation Studio

The first app must demonstrate that the system is useful, leveraging events, storage, the web UI, and local interaction.

Initial features:
- event → action rules (including on received IR and timers);
- local and web notifications;
- writing to file/log;
- reacting to keys, timers, Wi-Fi state, file system;
- live status via browser.

Criterion: the user defines a rule from the browser and the device executes it without reflashing.

### Deliverable 7 — Serious OTA

Goal: a distributable and updatable project.

Checklist:
- update endpoint;
- firmware download;
- image validation (**signature**);
- OTA partition switch;
- boot health check;
- automatic rollback on crash or missing confirmation.

Criterion: successful update and rollback tested on deliberately faulty firmware.

## Innovation roadmap (what makes NucleoOS "never seen before")

These capabilities arrive **after** the core, in phases, so as not to betray deliverability. Each one reuses the existing event bus and capability model.

### Phase 1 — Multi-channel zero-network access (BLE + WebUSB + Web Serial)
The web workstation works **without Wi-Fi**, with the same event protocol over multiple transports, so as to cover **both PC and Android**:
- **BLE GATT (Web Bluetooth):** wireless without a router, reachable from the browser on Chrome desktop **and Android** — the universal channel;
- **WebUSB:** over USB-C cable, works on Chrome desktop **and Android**;
- **Web Serial:** cable fallback on Chrome desktop only.
Result: you open the PWA, connect (cable or BLE) and administer the device even where there is no network, from the same Android phone. Very few embedded OSes offer a complete browser-based administration channel without an IP, and even fewer cover Android wirelessly.

### Phase 2 — Real security and pairing from the display
- device key generated on-board, stored in `/system/keys`;
- app bundles **signed with Ed25519**, verified at install;
- browser↔device pairing via a **QR code shown on the 240×135 display** (trust-on-first-use), no passwords typed on the keypad.

### Phase 3 — Automations as a sandboxed micro-VM
Instead of a general interpreter (ruled out by the plan), a **minimal stack VM** that executes bytecode compiled by a visual block editor in the browser. Deterministic, tiny (a few KB), sandboxed by capability: a rule can only touch what the app declares. Rules live in `/apps/<id>/rules/`.

### Phase 4 — Voice and IR as native capabilities
- **On-device wakeword** (ESP-SR / WakeNet): local voice command without the cloud, exposed as a `voice.command` event;
- **Universal IR**: TX/RX as the `device.ir` capability, integrated into automations (e.g. "on the keyword, turn off the TV").

### Phase 5 — Swarm OS (ESP-NOW mesh) ⭐ FLAGSHIP DEMONSTRATOR
**This is the innovation chosen as the project's "never seen before" element.** Multiple Cardputers form a **router-less ad-hoc cluster** via ESP-NOW (latency <5 ms, very low power, no IP):
- **shared clipboard and files** between nearby devices;
- **distributed apps**: an app declares `mesh.exposes/consumes` and the event bus extends *transparently* beyond a single device (remote events arrive as local events);
- pocket **multiplayer games** and **sensor meshes**;
- peer discovery and event gossip built **on top of the already existing event-sourced bus** — no separate subsystem.

Minimum path for the demonstrator (see Month 3):
1. ESP-NOW peer discovery + signed handshake (reuses the Phase 2 keys);
2. event bus ↔ ESP-NOW bridge: publish/subscribe events marked `mesh`;
3. end-to-end demo: **shared clipboard** between two Cardputers (copy on one, paste on the other) and an `automation.rule_fired` event that fires on one device and notifies the other.

A pocket appliance OS that federates into a swarm, with no infrastructure and a unified event bus, is the truly original element of the project.

### Phase 6 — System time-travel and undo
Since everything is event-sourced in `/journal`, this enables:
- **deterministic replay** for debugging ("replay the hour before the crash");
- **undo** of system operations (restore the state to a previous event);
- efficient **delta sync** to the browser even with 512 KB of RAM (only new events are sent).

## 30-day execution plan (core)

### Week 1 — Boot and foundations
ESP-IDF project init; partitions; SD mount; display+keyboard; stable logging.
Output: firmware that always boots, basic hardware tests, frozen repo structure.

### Week 2 — Core runtime
Service manager; **event-sourced event bus**; config system; first REST endpoints; minimal power manager; local diagnostics.
Output: ≥2 active system services (`storage`, `events`) visible from logs and reconstructible from the journal.

### Week 3 — Initial web shell
Minimal HTML dashboard (PWA) **keyboard-and-mouse-first**; delta-event WebSocket; log viewer; storage+battery page; first remote commands; **BLE GATT prototype** (network-less transport, valid on Android too).
Output: browser connected to the Cardputer with a live view, via Wi-Fi and via BLE; PWA usable in desktop mode with mouse and keyboard.

### Week 4 — File Commander + Registry
Working file manager; bundle scanning; module start/stop; first installation from SD or web upload.
Output: a system that is already useful, not just a demo.

## 90-day execution plan

### Month 1 — Useful base
Boot, SD (exFAT/FAT32), keyboard-and-mouse-first PWA dashboard, file manager, registry, network-less access via **BLE/WebUSB**.

### Month 2 — Credible system
Auth + QR pairing, signed bundles, permissions, automations (micro-VM), desktop-ready web UX, config saving, error handling, full power manager.

### Month 3 — Distributable and original system
OTA + rollback, versioned bundles, backup, tests, profiling. **Flagship demonstrator enabled end-to-end: ESP-NOW mesh** — clipboard and events shared between two Cardputers, with a federated event bus (see Phase 5). Documentation for third parties.

## Recommended technical choices

| Area | Choice | Reason |
|---|---|---|
| Core firmware | ESP-IDF C/C++ | fine control of tasks, networking, update |
| Web UI | HTML + CSS + lightweight JS/TS, **keyboard-and-mouse-first PWA** | one codebase for PC+Android, desktop mode, no mandatory native app |
| Messaging | REST + WebSocket (**delta**) over Wi-Fi; **BLE GATT / WebUSB / Web Serial** without a network | same protocol on every transport; BLE/WebUSB cover Android |
| Config | JSON | easy to inspect and edit |
| App packaging | bundle with manifest **+ Ed25519 signature** | scalable, readable, trusted |
| Automations | sandboxed bytecode micro-VM | safe and tiny, no general interpreter |
| Persistence | microSD + **event journal** | capacity + replay/undo, separation from flash |
| Mesh | **ESP-NOW** | router-less peer-to-peer, low latency |
| Recovery | dual-slot OTA | robust updates |

## What NOT to do

To keep the plan deliverable, avoid right away:

- a complex desktop environment with multitasking windows;
- a general Python interpreter as the foundation of the system (the micro-VM safely replaces it);
- emulators as the main axis of the project;
- a complex online marketplace before the registry and local bundles;
- sophisticated permissions before simple manifests;
- mesh, voice, and time-travel **before** the v1 core (boot→registry→OTA) is stable;
- **a native Android app as the primary interface**: the PWA covers PC and Android with a single codebase; the native app remains an optional companion only for background USB/BLE bridging;
- any assumption of "abundant RAM": always design for 512 KB.

## Risks and countermeasures

| Risk | Impact | Countermeasure |
|---|---|---|
| **Insufficient RAM (no PSRAM)** | **High** | static allocation, delta sync, no heavy frameworks, 1 partial framebuffer |
| Battery draining in <1h | High | power manager, aggressive sleep, per-service energy budget, targeted wakeups |
| Uncontrolled firmware growth | High | assets/apps/UI on SD |
| Unstable OTA | High | test partitions and rollback right away |
| Web UI too heavy | Medium | minimal dashboard, PWA, no heavy frameworks |
| Registry too abstract | Medium | simple bundles and a first real app |
| Innovations destabilizing the core | Medium | separate phases; each innovation reuses the existing event bus and capabilities |
| Project too theoretical | High | deliver File Commander within 30 days |

## Definition of success

The project has succeeded when it satisfies all of the following together:

- it always boots and updates without bricking;
- it exposes a web dashboard that is genuinely usable with mouse and keyboard, **even without Wi-Fi (BLE/WebUSB, including Android)**;
- it manages files and signed app bundles on SD;
- it offers at least one daily-useful function;
- it respects a measurable energy budget;
- it allows adding modules without rewriting the core;
- it enables at least **one** original capability (mesh, voice/IR, or time-travel) as a demonstrator.

## Recommended first sprint

Very concrete:

1. initialize the ESP-IDF Cardputer project;
2. define partitions and minimal recovery;
3. mount the SD;
4. read the keyboard and show status on screen;
5. implement persistent logging **on an append-only journal**;
6. create a basic HTTP server;
7. expose `/api/status` (including battery status);
8. serve a static web page (PWA) with device status, navigable with mouse and keyboard;
9. prototype of a **BLE GATT** channel reusing the same `/api/status` (network-less transport, testable from Chrome on Android too).

If this sprint is completed, the project moves from idea to real platform.

## Operational conclusion

NucleoOS is real and deliverable if treated as an embedded platform with a useful vertical already in v1 — **file manager + PWA dashboard + app registry + automations + OTA** — built under two honest constraints: **512 KB of RAM** and **120 mAh of battery**. The interface is a **keyboard-and-mouse-first PWA**, a single codebase covering PC and Android (including desktop mode), reachable over Wi-Fi and — **without a network** — over **BLE/WebUSB**, so a native app is not needed as a mandatory choice. On this solid base sits what makes it unique: an **event-sourced** kernel with undo and replay, **signed** apps like cartridges on SD, sandboxed **micro-VM** automations, native **voice/IR**, and — the never-before-seen element in this class, chosen as the flagship demonstrator — an **ESP-NOW swarm OS** that federates multiple Cardputers without infrastructure. Real innovation, but resting on a core that works.

## Hardware references

- M5Stack Cardputer (M5StampS3 module / ESP32-S3FN8): official M5Stack datasheet and schematics — **verify PSRAM presence on your own revision**. Bluetooth = **BLE 5.0 only** (no Classic).
- Espressif ESP-IDF: OTA partitions, `esp_http_server`, ESP-NOW, BLE/NimBLE, ESP-SR/WakeNet, FATFS (exFAT/FAT32 for a 64 GB SD).
- W3C **Web Bluetooth / WebUSB / Web Serial** APIs (Chromium): network-less browser transports. Note: Web Serial is not available on Android; BLE and WebUSB are.
