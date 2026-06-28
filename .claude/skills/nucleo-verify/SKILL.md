---
name: nucleo-verify
description: How to verify a NucleoOS change actually works before claiming done or releasing — the full preflight matrix (registry/manifest validation, i18n, gzip parity, API spec drift, ANIMA gate, app tests) plus running the device simulator for web changes. Use when finishing a change, before a release, or when asked to verify/test that something works.
---

# Verifying a NucleoOS change

Pick the checks that match what you touched; run the whole matrix before a release.
All of these run on the PC — none touch the device. They're pre-approved (no prompt).

## Preflight matrix
| Touched | Run |
|---|---|
| registry / a manifest.json | `npm run validate` (schema + cross-refs) |
| any UI string / i18n catalog | `npm run i18n:gate` |
| a web asset (`*.js/.html/.css`) | `npm run gz:check` — the `.gz` must match its source (device serves `.gz` first) |
| a firmware HTTP route | `npm run gen:api:check` (Swagger drift gate) |
| ANIMA logic / corpus / NLU / KG | `npm run anima:gate` (the 12-gate canonical pre-flight) |
| broad / unsure | `npm run test:all` (the full catalog incl. app tests) |

Slice the catalog when iterating: `node tools/run-tests.mjs --grep <x>` / `--cat <c>` / `--list`.
The ANIMA gate is hermetic (clears volatile SD state per test); a green `anima:gate` is the bar
before any flash (`flash.ps1`/`release.ps1` enforce it).

## Verify a web app / shell change in the simulator
The repo ships a zero-dependency device simulator — use it, don't ask the user to check by hand.
```
node tools/serve-shell.mjs      # http://localhost:5599
```
It mirrors the firmware: `/api/status|apps|associations`, `/api/fs/*` on `tools/sd-sim`, `/ws`
event deltas, and serves `/apps/<id>/<file>` → `apps/<id>/www/<file>`, `/<asset>` → `web/shell/`.
It also runs the real ANIMA host cores, so "apri <app>" and offline answers work like the device.

Then drive it with the preview tools: start/point at `localhost:5599`, snapshot for content,
check console/network for errors, click/fill to exercise it, screenshot the result as proof.
Remember the cross-import gotcha: import another app as `/apps/<id>/<file>` **without** `/www/`.

## What stays gated (don't auto-run)
`npm run push-ota`, `npm run test:lab` (device-load), and every `tools/*.ps1`
(flash/ota/release/sd-sync) touch the physical device — run only when the user asks.
