# Architecture

## Principles

1. **ESP-IDF** is the single base (core, networking, tasking, update).
2. **System in flash, content on SD** — firmware in flash; apps, data, logs, assets on microSD.
3. **Delta sync, never full state** — the RAM budget forbids full snapshots.
4. **Capability-based apps** — each app declares permissions, events, mounts, routes, energy cost.
5. **Recovery first** — any update must fail without bricking.
6. **Vertical slice before complexity** — every phase ships something usable.
7. **Energy is a system resource** — every service declares and respects a budget.
8. **Everything is an event** — state changes flow through an append-only bus → observability, replay, undo.

## Layers

| Layer | Function | Implementation |
|---|---|---|
| Boot & recovery | Boot, firmware validation, rollback | ESP-IDF bootloader + OTA partitions |
| Core services | Wi-Fi, auth, event bus (event-sourced), log, storage, power manager, scheduler | C/C++ on ESP-IDF |
| App runtime | Lifecycle, manifest, mounts, permissions, capabilities | JSON registry + service launcher |
| Storage | Config, bundles, data, logs, assets, event journal | microSD + system areas |
| Transport | Multi-channel admin | HTTP+WS (Wi-Fi), BLE GATT, WebUSB, Web Serial |
| Local UI | Pairing (QR), setup, recovery, minimal notices | Native UI on 240×135 display |
| Web workstation | Dashboard, file manager, installer, logs, admin | Lightweight PWA, keyboard-and-mouse-first |

## Access model

The primary UI is a **PWA** (one codebase for PC and Android, works in desktop mode with
mouse and keyboard). A native Android app is **optional**, only as a thin companion for
background USB/BLE bridging. Transport is negotiated in order:
**Wi-Fi → BLE → WebUSB → Web Serial** (Web Serial is desktop-only; BLE and WebUSB cover Android).

## Flagship innovation

**ESP-NOW swarm**: multiple Cardputers federate without a router. The event bus extends
transparently across peers — shared clipboard, distributed apps, multiplayer, sensor mesh.
See the Swarm app and `docs/event-protocol.md`.
