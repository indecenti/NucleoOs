# Roadmap — what makes NucleoOS a real, modern, useful OS

Honest state: a strong **skeleton** exists (boot, SD + provisioning, registry, desktop
shell, mDNS discovery, setup wizard, Windows connector, multi-runtime app model). Most
**subsystems are designed but not implemented**, and most **apps are manifests without
working code**. The priority is to make the core *alive* before adding breadth.

## 1. Core subsystems a real OS needs

| Subsystem | State | Priority | Why it matters |
|---|---|---|---|
| **Event bus + WebSocket (delta)** | designed | ⭐⭐⭐ | The nervous system: live state, no polling. Unlocks the shell + every app |
| **File API `/api/fs/*`** | missing | ⭐⭐⭐ | Makes File Commander & Notepad actually work; backs `view`/`edit` intents |
| **Security: auth + pairing + bundle signing** | designed | ⭐⭐⭐ | Today the AP/HTTP is open. A real OS needs identity + permission enforcement |
| **Time: RTC + NTP** | missing | ⭐⭐ | Logs, journal, alarms, scheduler all need real time (no battery RTC → NTP + drift) |
| **Settings actually applied/persisted** | partial | ⭐⭐ | `settings.json` exists but isn't wired to runtime (brightness, sleep, theme) |
| **Notifications (local + web)** | missing | ⭐⭐ | System-wide notices, app alerts, swarm messages |
| **Journal / logging (event-sourced)** | designed | ⭐⭐ | Observability + replay/undo; backs Log Viewer |
| **Service supervision** | partial | ⭐⭐ | Start/stop/restart/health for apps & services |
| **Power management** | designed | ⭐⭐ | 120 mAh: battery gauge, sleep, per-service budget |
| **OTA update + rollback** | designed | ⭐ | Distributable & safe upgrades (partitions ready) |
| **Backup / restore** | designed | ⭐ | `/backups`: snapshot config + app data |

## 2. Apps — have vs. missing (fit the Cardputer hardware)

Have (mostly manifests; Notepad has a UI): File Commander, Notepad, Photo Viewer,
Media Player, Automation Studio, IR Remote, Swarm, Settings, Log Viewer.

Missing, genuinely useful, hardware-appropriate:

| App | Uses | Value |
|---|---|---|
| **Clock / Alarms / Timer** | RTC, speaker | Everyday utility |
| **Calculator** | — | Everyday utility |
| **Terminal / Console** | keyboard, serial | Power users; CLI parity with other OSes |
| **System Monitor** | core | RAM/CPU/tasks/battery/storage at a glance |
| **App Store / Installer** | net, registry | Install signed apps without reflash (modern) |
| **Wi-Fi & BLE scanner** | radios | Diagnostics (defensive/educational) |
| **Sensor Dashboard** | Grove / I2C | Read/plot attached sensors |
| **QR tool** | display | Show pairing/share codes (no camera on device) |
| **Clipboard manager** | swarm | Shared clipboard across devices |
| **Notes / To-do** | storage | Lightweight productivity |
| **Unit/Currency converter** | net (optional) | Handy |
| **Games (swarm multiplayer)** | ESP-NOW | Showcases the unique swarm feature |

## 3. Modern & innovative features (our edge)

| Feature | Notes |
|---|---|
| **AI assistant (client-delegated)** | The device asks the connected PC/phone to run the LLM call — "Commodore trick": heavy compute on the client, the ESP stays tiny. Can answer about device state and trigger automations |
| **Command palette / universal search** | Spotlight-style launcher in the shell (apps, files, settings, actions) |
| **Swarm continuity** | Handoff/clipboard/files between nearby devices (ESP-NOW) — designed |
| **Voice control** | On-device wakeword (no cloud) → `voice.command` events — designed |
| **Time-travel / undo** | Replay system state from the journal — designed |
| **Notification mirroring** | Device notices surface on the paired PC/phone |
| **Lock screen + PIN** | Optional access control, unlock via paired client |
| **Theming** | Shell themes/design tokens; per-user sessions |
| **Visual automation editor** | Block editor compiling to the micro-VM (`runtime: vm`) |

## 4. Recommended sequence

1. **Live core** — Event bus + WebSocket + `/api/fs`. Then Notepad & File Commander truly
   work, and the shell goes live. *Biggest single jump in "feels like an OS".*
2. **Security** — pairing + auth + signed bundles (before any app store).
3. **Time + Settings wiring + Notifications** — the everyday-OS glue.
4. **First useful new apps** — Clock, Calculator, System Monitor, Terminal.
5. **Innovative layer** — Command palette, AI assistant (client-delegated), Swarm clipboard.
6. **Distribution** — OTA + App Store + backup.

The guiding rule stays: heavy work on the client, the ESP core tiny — and lean into what
competitors can't do (swarm, multi-transport offline, event-sourcing, energy-awareness).
