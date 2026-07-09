# NucleoOS — working notes for Claude

A web-native appliance OS for the **M5Stack Cardputer** (ESP32-S3 / M5StampS3).
The system runs on the device; the browser is the rich operator console.
**Everything in this repository is English only.** (Conversational replies follow the
user's own language preference — that's a personal setting, not a repo rule.)

## Hard constraints — design for the worst case
- **RAM:** ~512 KB SRAM, **no PSRAM**. Runtime heap is ~18 KB; the real bottleneck is
  fragmentation (httpd + L1 + workers), not CPU. Never load heavy data on the device —
  push it to the browser client. See `docs/memory-budget.md`.
- **Battery:** ~120 mAh — energy is a first-class resource.
- **Storage:** microSD (system in flash, content on SD).

## Repository layout
```
docs/            Engineering specs — one focused file each (the canonical knowledge base)
schemas/         JSON Schemas — shared contract for firmware / PC / Android
registry/        Live OS registry: installed apps, settings, file associations
apps/<id>/www/   One folder per app + manifest.json (web apps; .gz built alongside)
firmware/        ESP-IDF firmware (boot, SD, registry, HTTP, WS, ANIMA C core)
web/shell/       Desktop shell PWA
tools/           Dev tooling: validators, host harness, deploy/flash scripts
tools/anima-host/  Host gates — compile the REAL firmware C and run it on the PC
```
Start every orientation from **`README.md`** (full docs index) and the relevant
`docs/*.md`. `docs/` is the source of truth; prefer reading it over re-deriving.

## Default dev loop — host first, flash last
Don't iterate by flashing. The host harness compiles the real ANIMA C and runs it on the PC:
```
npm run anima -- "che cos'è nucleoos"   # one-shot     npm run anima   # REPL
npm run anima:gate                       # the 12-gate regression suite
npm run validate                         # registry + manifest schema check
```
Per-subsystem gates live in `tools/anima-host/` and as `npm run <x>:test`
(`ir`, `link`, `mesh`, `eth`, `hw`, `skill`, …). Flash only to *confirm*.

For web/shell work, run the zero-dependency device simulator and verify in it (don't ask the
user to check by hand):
```
node tools/serve-shell.mjs      # http://localhost:5599 — mirrors the device API + serves the apps
```
Verification matrix: `npm run validate` (registry/manifest), `i18n:gate`, `gz:check`,
`icons:gate` (firmware↔web launcher icon-set parity), `gen:api:check`, `anima:gate`,
`test:all`. See `docs/debugging.md` and the skills below.

**Library/API docs:** when touching a third-party API (ESP-IDF, Three.js, Vosk, js-dos, the
LLM providers, web platform APIs), use the **context7** MCP (`.mcp.json`) — `resolve-library-id`
then `get-library-docs` — to pull version-correct docs instead of guessing. Wrong/outdated API
usage is the most common bug class here.

## Build / release — via OTA + web API, not serial
- **Release (preferred, one command):** `tools/release.ps1` — runs the ANIMA gate, builds,
  assembles `deploy/sd`, syncs the SD over WiFi (delta, manifest-driven, never `/MIR`),
  then OTAs the firmware. `-FirmwareOnly` / `-SdOnly` / `-SkipBuild` to scope it.
- **Firmware only:** `tools/flash.ps1` (gate → build → flash). OTA: `tools/ota.ps1 -DeviceHost <ip> -Pin <pin>`.
- The ANIMA gate must be **green before any flash** (`flash.ps1`/`release.ps1` enforce it;
  `-SkipGate` overrides — don't).
- Run `.ps1` deploy scripts from the **PowerShell tool** as a child process, not via Bash.
- See `docs/releasing.md` and the `nucleo-release` skill.

### Rule: never deploy/flash/OTA on my own initiative
Build, gate and host-test freely. But **sd-sync / deploy / flash / OTA only when the user
explicitly asks.** Sending firmware or files to the device is hard to reverse.

## Native UI/UX priority (permanent)
Every native firmware UI must use **large readable fonts, exploit every pixel, and lean on
smartwatch tricks** (autocomplete, history, resume, 1–9 quick-select, preview) to fight the
tiny screen + keyboard. See `docs/device-ui.md`.

## Where knowledge goes (keep this tidy)
- **`docs/*.md`** — durable engineering specs and architecture (English, versioned). Default home for "how the system works".
- **`CLAUDE.md`** (this file) — durable conventions and rules I must follow every session.
- **`.claude/skills/`** — repeatable multi-step workflows: `nucleo-release` (ship to one device),
  `nucleo-release-dual` (ship to BOTH Cardputers, original + ADV — one universal binary, build
  once + fan out), `nucleo-anima-debug` (host-first dev loop), `nucleo-verify` (preflight matrix +
  simulator), `nucleo-new-app` (scaffold an app + register/alias/i18n it correctly).
  `.claude/settings.json` pre-approves the safe dev commands (gates/tests/validate/sim);
  device-touching commands stay gated. Two guard hooks (`.claude/hooks/`): a PreToolUse hook
  forces a confirmation prompt before any flash/OTA/release/sd-sync (deterministic
  never-auto-deploy), and a PostToolUse hook runs `npm run validate` after registry/manifest edits.
- **`.claude/agents/`** — custom subagents for recurring roles: `firmware-reviewer`,
  `anima-corpus-curator`, `board-bringup-verifier`. Delegate to them with the Agent tool.
- **Memory** (`~/.claude/projects/G--Nucleo/memory/`) — personal facts, preferences, and
  decision/status notes that aren't in the repo. One fact per file; keep `MEMORY.md`
  index entries to a tight one line. Don't grow memory files into mini-docs — that belongs in `docs/`.
