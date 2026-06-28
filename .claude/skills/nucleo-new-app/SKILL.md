---
name: nucleo-new-app
description: Scaffold a new NucleoOS web app correctly — manifest, www assets, registry entry, ANIMA launch aliases, i18n, icon — so it shows up, launches by name, and passes validation. Use when adding a new app to the OS, creating an app folder, or wiring an existing app folder into the registry.
---

# Adding a NucleoOS web app

An app is `apps/<id>/` with a `manifest.json` + a `www/` folder. To actually appear and be
launchable it must also be registered and aliased. Miss a step and it's invisible or unfindable.

## 1. App bundle — `apps/<id>/`
`manifest.json` (validated against `schemas/manifest.schema.json` — see an existing one like
`apps/radio/manifest.json` and `docs/app-manifest.md`):
```json
{ "id": "<id>", "name": "<Name>", "version": "1.0.0", "description": "...",
  "category": "media|tools|system|games|...", "runtime": "web",
  "entry_service": "none", "web_route": "/apps/<id>/", "icon": "/apps/<id>/icon.svg",
  "permissions": ["storage.shared"], "handles": { "role": "none", "extensions": [] },
  "intents": [], "subscribes": [], "publishes": [],
  "power": { "budget_class": "low", "wants_wakeup": [] }, "mesh": { "exposes": [], "consumes": [] } }
```
`www/` holds at least `index.html`, `icon.svg`, and (if it has UI text) `i18n.en.json` +
`i18n.it.json`. **The icon is served only from `www/`** (`/apps/<id>/icon.svg`); an app's icon
source is its manifest. To import code from another app use `/apps/<otherid>/<file>` —
**without** `/www/` (webfs + sim both map there; adding `/www/` 404s).

## 2. Register it — `registry/apps.json`
Add to `installed[]`: `{ "id":"<id>", "version":"1.0.0", "path":"/apps/<id>", "enabled":true,
"autostart":false, "permissions":[...] }` (permissions mirror the manifest). If it owns a file
type, add to `registry/file-associations.json`.

## 3. Make it launchable by voice/NL — `registry/app-aliases.json`
Add `"<id>": ["italian", "and", "english", "aliases"]` (lowercase, accents folded). This is the
**single source** for the "apri X" intent, shared by firmware + simulator. After editing, run the
generator so the firmware copy stays in lock-step:
```
python tools/anima/gen_aliases.py
```
It errors on exact alias collisions, warns on prefixes. Order = first-match priority.

## 4. i18n
Per-app strings go in `apps/<id>/www/i18n.{en,it}.json`; the shell/core catalogs live in
`web/shell/i18n/`. Both must stay in parity (`npm run i18n:gate`). See `docs/i18n.md`.

## 5. Verify, then build the .gz
```
npm run validate          # registry + manifest schema + cross-refs
npm run i18n:gate
node tools/serve-shell.mjs # http://localhost:5599 — open /apps/<id>/ and exercise it
npm run gz:check          # the device serves <file>.gz first — keep it in sync
```
Use the `nucleo-verify` skill for the full matrix and `nucleo-release` to ship.
Don't deploy/flash on your own initiative.
