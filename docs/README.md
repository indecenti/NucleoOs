# NucleoOS documentation

The engineering knowledge base for **NucleoOS** — a web-native OS for the M5Stack Cardputer
(ESP32-S3 / M5StampS3, ~512 KB SRAM, no PSRAM). Each file is one focused spec; `docs/` is the
source of truth for how the system works.

- **New here?** Read the **[User Guide](user-guide.md)** — the end-user manual for setting up and
  using a Cardputer. Then the project **[README](../README.md)** for the feature tour.
- **Building on NucleoOS?** Jump to [Building apps & protocols](#building-apps--protocols).
- **Working on the assistant?** Start at [ANIMA — assistant](#anima--the-assistant); the ground
  truth is [`anima-cortex.md`](anima-cortex.md), not the aspirational [`anima.md`](anima.md).

> **Conventions.** Everything in this repo is **English**; Italian appears only as *data* (sample
> utterances ANIMA understands, voice vocabulary), usually with an English gloss. Docs that describe
> unbuilt or superseded designs are marked *(design / plan)* or *(historical)* so you can tell a spec
> of record from a wish list.

---

## Start here

| Doc | What you get |
|---|---|
| [user-guide.md](user-guide.md) | **End-user manual** — install, first boot, pairing, using the device and the web desktop, updating, troubleshooting. |
| [setup-wizard.md](setup-wizard.md) | The on-device first-run wizard (language, Wi-Fi / Access Point, device name), where it persists, and where to find the pairing PIN afterwards. |
| [security.md](security.md) | Device pairing (6-digit PIN) and session auth: what is public vs paired-only, tokens, brute-force guard. |
| [update-check.md](update-check.md) | How installed devices discover a new release and install it — browser notifier, Settings ▸ Updates, and the native boot dialog, all without the device touching the internet. |

## Using NucleoOS

| Doc | What you get |
|---|---|
| [device-ui.md](device-ui.md) | The on-device UI: launcher carousel, key map, Spotlight search, pins, Recent, and the Control Center (TAB). |
| [app-categories.md](app-categories.md) | How the native launcher groups apps by *use*, its capacity limits, and the category reorganization. *(inventory tables are a snapshot — `npm run launcher:test` has the live tree)* |
| [media.md](media.md) | Media playback model — the device serves bytes, the browser decodes; plus the optional on-device audio/video paths. |
| [voice.md](voice.md) | The on-device push-to-talk keyword recognizer (AVCEB: MFCC · CMN · DTW), trained by the Voice Manager app. |
| [tts.md](tts.md) | Offline concatenative bilingual (IT/EN) voice — what ANIMA speaks aloud and why. |
| [i18n.md](i18n.md) | The internationalization engine — five UI languages (it · en · es · fr · de), one language signal, per-app catalogs, the parity gate; ANIMA itself stays IT/EN. |
| [keydeck.md](keydeck.md) | KeyDeck — turning the Cardputer into a Wi-Fi keyboard + monitor for the sibling NucleoV2 (ESP32-P4). |

## ANIMA — the assistant

ANIMA is NucleoOS's offline natural-language assistant (retrieval + reasoning in C, no runtime LLM).

| Doc | What you get |
|---|---|
| [anima.md](anima.md) | The engine overview and where each part runs. *(aspirational design — for what actually runs, read `anima-cortex.md`)* |
| [anima-cortex.md](anima-cortex.md) | **Ground truth**: the cascade the firmware actually runs today, and what is built vs deferred. |
| [anima-native.md](anima-native.md) | The native on-device C app (`app_anima.cpp`) — lifecycle, RAM budget, invariants. The definitive start for native ANIMA work. |
| [anima-online.md](anima-online.md) | How ANIMA behaves with Wi-Fi up — structured knowledge + a cloud teacher that leaves the device permanently smarter offline. |
| [anima-agent.md](anima-agent.md) | Micro-agent design: intent routing, tool use with strict schemas, multi-domain RAG within 512 KB. *(design)* |
| [anima-roadmap.md](anima-roadmap.md) | The ordered, verifiable build plan for ANIMA — north star, invariants, phased gates. *(roadmap)* |
| [anima-local-first-plan.md](anima-local-first-plan.md) | A local-first agent plan — keep the device minimal, run everything heavy in the browser / on the user's PC. *(plan; proposed, awaiting approval)* |
| [anima-memory.md](anima-memory.md) | The SRAM / flash (XIP) / SD memory hierarchy that lets the brain hold far more than RAM. *(design)* |
| [anima-knowledge-graph.md](anima-knowledge-graph.md) | Typed entities, faceted relations and indexed retrieval (Wikidata-derived). *(durable plan; SD index not yet built)* |
| [anima-knowledge-scale.md](anima-knowledge-scale.md) | Scaling the offline knowledge brain to 50–60 GB of certain, bilingual content. *(durable plan)* |
| [anima-knowledge-ollama.md](anima-knowledge-ollama.md) | Growing recall with a local ollama model as a *question-teacher* (proposes phrasings, never answers). |
| [anima-web-knowledge.md](anima-web-knowledge.md) | Client-side single-entity web lookup (Wikipedia/Wikidata, zero-LLM, verbatim). *(the older mass-download path was removed 2026-06-21)* |
| [anima-context-engine.md](anima-context-engine.md) | How the *online* chat budgets and injection-protects its context, and the Claude/Groq multi-agent runtime. |
| [anima-code.md](anima-code.md) | ANIMA Code — the on-device, multi-substrate app-building agent (Agenti pipeline + Forge engines). *(design of record; parts built)* |
| [anima-forge.md](anima-forge.md) | ANIMA Forge — four cooperating intelligence substrates, GPU/WASM local models, the verifier. *(design; deterministic spine built)* |
| [anima-atelier.md](anima-atelier.md) | Sketch-conditioned on-device image generation (SDXS + ControlNet on the browser's WebGPU). *(design)* |
| [ai-models.md](ai-models.md) | How every cloud-LLM surface picks a model live from the key's own `/models` and turns failures into one actionable sentence. |
| [ai-roadmap.md](ai-roadmap.md) | The AI strategy record — a 2026-08 design review's ranked proposals, with the judge's reasoning. *(roadmap)* |

## Building apps & protocols

| Doc | What you get |
|---|---|
| [app-manifest.md](app-manifest.md) | The `manifest.json` schema every app bundle declares. |
| [app-runtimes.md](app-runtimes.md) | The four runtime tiers — `web` · `vm` · `service` · `elf` — and why `web` dominates on a no-PSRAM board. |
| [registry.md](registry.md) | The OS registry: installed apps, file associations, settings, and how file-open resolves. |
| [event-protocol.md](event-protocol.md) | The one event bus over every transport — event envelope, monotonic `seq`, delta sync. |
| [notify-protocol.md](notify-protocol.md) | The single notification backbone (bus topic `notify.post`) feeding the web center + device. |
| [native-ui-kit.md](native-ui-kit.md) | The style contract every native app must follow (theme roles, primitives) so the OS looks like one product. |
| [native-emulation.md](native-emulation.md) | What the device can emulate itself in firmware C — Game Boy and Game Gear ship; the rest is analysis. |
| [game-mini-vs.md](game-mini-vs.md) | A worked native-game build plan (a horde survivor) and its hard RAM/perf contract. *(build plan)* |

## System internals

| Doc | What you get |
|---|---|
| [architecture.md](architecture.md) | The system principles and layer model (boot, core services, app runtime, transport, UIs). |
| [memory-budget.md](memory-budget.md) | The riskiest bet: fitting Wi-Fi/BLE/HTTP/display/bus into ~512 KB via mutually-exclusive transport profiles. |
| [partition-table.md](partition-table.md) | The 8 MB flash layout — dual OTA banks (`ota_0`/`ota_1`), no factory, coredump + config partitions. |
| [m5launcher.md](m5launcher.md) | Running as a guest of M5Launcher — the Launcher boot model, what guest mode changes (no self-OTA, Back to M5Launcher), and how to publish on M5Burner so it appears in the Launcher's OTA list. |
| [sd-content-install.md](sd-content-install.md) | The release SD payload (built from sources, per-file manifest, hosted on Pages) and the design of the device downloading its SD content by itself — ownership rules, failure handling, test plan. |
| [storage.md](storage.md) | The microSD filesystem — mount model (FAT32/exFAT over SPI3), capacity, first-boot provisioning. |
| [wifi-supervisor.md](wifi-supervisor.md) | When the background Wi-Fi supervisor may touch the radio, and the hotspot-join contract (fixes issue #3). |
| [swarm-architecture.md](swarm-architecture.md) | Decision record: why multi-Cardputer cooperation is *not* master/slave; the accepted model. *(ADR)* |
| [take-journal.md](take-journal.md) | Treating a long recording as an append-only stream + index so a crash never yields an unopenable file. |

## Developing & releasing

| Doc | What you get |
|---|---|
| [debugging.md](debugging.md) | The default host-first dev loop — compile the real firmware C and run it on the PC; flash only to confirm. |
| [testing.md](testing.md) | The verification system — one test registry feeding the gate, CLI and GUI; ANIMA's NL suites. |
| [releasing.md](releasing.md) | The two independent release layers (firmware vs web/SD) and the OTA / SD-sync channels for each. |
| [versioning.md](versioning.md) | The single-source-of-truth firmware version string, auto-incremented and reported identically everywhere. |

## History & maintenance notes

Point-in-time records — useful for context and decisions already made, not current how-to.

| Doc | What you get |
|---|---|
| [original-plan.md](original-plan.md) | The original execution plan NucleoOS started from — v1 core vs. phased innovation roadmap. *(historical)* |
| [maintenance-2026-06.md](maintenance-2026-06.md) | June 2026 hardening pass — dead-code removal, RAM/fragmentation and concurrency fixes. *(historical)* |
| [maintenance-2026-08.md](maintenance-2026-08.md) | August 2026 pass — making the test harness portable off-Windows and hunting real correctness bugs. *(historical)* |
| [maintenance-2026-09.md](maintenance-2026-09.md) | September 2026 web-OS consolidation — the browser E2E suite and the fixes it found. *(historical)* |
| [shell-cache-log.md](shell-cache-log.md) | The service-worker cache-version changelog for `web/shell/sw.js` (why each roll happened). |
| [roadmap.md](roadmap.md) | The honest overall state — what a real OS needs, what exists vs is still designed. *(roadmap)* |
| [repo-history-purge.md](repo-history-purge.md) | A one-time, deliberately-manual runbook to strip non-distributable blobs from git history. *(runbook)* |

---

## Not-a-`.md`: assets referenced by the docs & README

- **[`screenshots/`](screenshots/)** — the screenshots and demo GIFs used by the top-level README and
  promo posts (web desktop, native UI, per-app captures). All captured from the real shell/apps in
  the device simulator.
- **[`promo/`](promo/)** — [`announcements.md`](promo/announcements.md): ready-to-post launch copy
  (Hacker News, etc.) for getting NucleoOS in front of people.

---

### A note on an inconsistency you may hit

- **`anima.md` vs `anima-cortex.md`.** `anima.md` is the aspirational L0→L3 design;
  `anima-cortex.md` documents the cascade the firmware really runs. When they disagree, trust
  `anima-cortex.md`.
