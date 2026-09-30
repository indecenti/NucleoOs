# Maintenance review — September 2026 (web OS consolidation)

Goal of this pass: make what exists genuinely useful, stable and tested — no new apps unless
fundamental — while keeping the Cardputer's load as low as possible. Method: a real browser test
harness first (so every fix is proven), then fix what it and a per-app code audit found.

## New verification: browser E2E suite (`npm run web:e2e`, `tools/web-e2e/`)

Headless Chrome/Edge driven over the DevTools protocol (zero npm dependencies) against the device
simulator. The shell is loaded from a NON-loopback hostname, so the page is **not a secure context** —
exactly like the Cardputer on `http://192.168.x.x` (on localhost the service worker and
`navigator.locks` run and hide the device's failure modes). Suites:

| File | What it proves |
|---|---|
| `shell-smoke.e2e.mjs` | boot + every app opened from Start in it/en/es/fr/de: zero exceptions, faults, asset 404s, unknown device routes; the UI and app names are really in that language; cold-boot device-load budget |
| `security.e2e.mjs` | hostile manifest name/icon, `javascript:`/`data:` `.lnk`, sandboxed frame reading the clipboard / driving the OS — all inert, legit paths still work |
| `session-restore.e2e.mjs` | restore loads the top window first and the rest ONE AT A TIME; maximised stays maximised; no focus stealing |
| `resilience.e2e.mjs` | refused write (500) announced + kept + saved on recovery; `/api/apps` 503 at boot; device dropping off the network (banner < 10 s, recovery) |
| `desktop-ux.e2e.mjs` | no dead icons, labels never split mid-letter, many windows never scroll the page or open off-screen |
| `data-safety.e2e.mjs` | calendar: device-written reminders deletable; a failed read never overwrites the SD; ANIMA's live additions survive the next edit |
| `anima-local-ai.e2e.mjs` | on plain http ANIMA explains how to unlock WebGPU with the shared panel (5 languages); `E2E_GPU=1`: Qwen3-1.7B installs on the real GPU and answers it/de with today's date |
| `ollama.e2e.mjs` | (skipped without Ollama) real Ollama: task-based pick + a tool call booking tomorrow; the ANIMA app in Private answered by the PC's server (it/de), 0 inference via the Cardputer; from the device origin the CORS refusal leads to the exact `OLLAMA_ORIGINS` fix |

Simulator (`tools/serve-shell.mjs`) gained: `PORT=0`, `NUCLEO_SD_ROOT` (tests run on a temp copy),
fault injection (`/api/_sim/fault|offline|ws-drop|apps|stats`), a faithful `esp_http_server` socket
model (`SIM_DEVICE_SOCKETS=4`: single task, lru_purge semantics), persisted `/api/lang`, and every
firmware route (`sim-endpoint-coverage.test.mjs` now checks ALL of `web-api-spec.json`).

## Fixed & verified

- **Security (shell trust boundary):** app records sanitised at load (`appbroker.js sanitizeApp`); taskbar
  name escaped; URL shortcuts accept only http(s) (new tab) or `/apps/…`; the message router accepts only
  same-origin, non-sandboxed senders; status/bus broadcasts skip sandboxed frames.
- **Session restore storm:** N restored windows no longer means N simultaneous app loads.
- **Config writes:** `saveConfig` checks the HTTP status, coalesces per path, retries 5xx, keeps a refused
  change and saves it on recovery, tells the user once (5 languages). Echo race that reverted a second
  quick desktop change fixed.
- **Device link:** "Cardputer unreachable" banner with Retry, fast probe instead of 30 s of retries, tray and
  apps (`link.state`) updated, pending saves flushed on recovery.
- **Desktop:** icons of disabled/uninstalled apps hidden (not deleted); labels hyphenate (catalog soft
  hyphens + shrink-to-fit); page can't scroll; cascade wraps; `applyGeom` idempotent (maximised restore).
- **i18n:** app names in 5 languages through ONE system — `appName()` + `app_<id>` in `shell.<lang>.json` (a7d8d1d;
  a parallel `core.*.json app.<id>` layer was merged into it, and the Italian names that were English manifest
  names — "Settings", "Calculator", "Notepad" — are Italian now), live switch;
  tray / remote / link strings; search still matches the manifest name.
- **Calendar:** typed reads, read-modify-write, stable ids for ANIMA reminders, `calendar.changed`, OS locale.
- **Gates on Windows:** `validate`, `gen-api-spec --check`, `gen-core-reports` EOL-normalised (CRLF checkout
  is not drift).

- **ANIMA gate reproducible on any machine:** the host fixture (L1 index + encoder + AKB5 + facets + HDC
  triples) is built by `npm run anima:fixture` with a deterministic k-means and an EOL-independent corpus hash;
  its bytes are fingerprinted (`index_sha`) and verified by `check_pack`. On this machine the NL suites went
  from "cannot run" to green (skill-routing 81/81, 462/462, false-positives 103/103, typed-nl 0 fabrications).
- **ANIMA phase 1 (local-first plan):** `/capabilities.js` = one diagnosis + exact per-browser fix (ANIMA,
  Settings, Agent); installer blocks plain-http up-front and never loops on unknown errors (5 languages); Qwen3
  catalog with real sizes, adapter-class recommendation, automatic q4f32 without shader-f16, CDN-first WebLLM;
  every prompt carries date/time and the OS language (es/fr/de got Italian before); Settings GPU rung reads the
  real install state. Verified on the real RTX 5070 (Qwen3-1.7B installs, answers it/de correctly).
- **ANIMA phase 2:** `/ai-engines.js` — Ollama / LM Studio / llama.cpp / Jan detection with real per-model
  capabilities, CORS-vs-down diagnosis, native Ollama chat + tool calls, OpenAI-compatible SSE, memory-aware
  fallback ladder. Verified against the real Ollama (gemma4:12b, correct tool call, 0 device API calls).
  **Wired into ANIMA's ladder** as the "local server" rung: first when there is no cloud key, in Private (only a
  server on this computer — a LAN box is still the network) or with `prefer:'local'`; otherwise the cloud's
  fallback. Gets the full ANIMA prompt sized for an 8k window (`contextkit` profile `local`); the engine map says
  "Local server · Ollama · <model>" and which model was skipped for memory. Detection is cached (60 s when a
  server is up, 5 min when none is — a refused localhost port costs ~1 s on Windows) and dropped on any error.
  The copilot (Ctrl+Space) uses the same rung, and its prompt now tells es/fr/de models which language to use.
- **Model choice adapts to THIS computer:** measured on the 8 GB laptop RTX 5070, gemma4:12b (7.56 GB file, 8.9 GB
  loaded) ran a third on the CPU at 2.1 tok/s — a minute per answer — while qwen3.5:9b gave 20–37 tok/s; the
  22.6 GB coder did not start at all (CUDA OOM). `pickModel` now starts from an honest prior (≈7 GB usable VRAM,
  file size compared with no slack) and then trusts what it measured: tokens/s of every answer plus Ollama's
  `/api/ps` VRAM share, kept per browser (`ai.local.perf`, 24 entries). Result: ~3 s answers instead of ~60 s.
- **"Ollama is running but refuses NucleoOS" wizard** (`/local-ai-help.js`, core `lai_*` keys, 5 languages): the
  documented fix for the user's OS (Windows `setx` + restart from Start, macOS `launchctl setenv` + reboot note,
  Linux systemd drop-in, a hand-started `ollama serve`) and server (LM Studio `--cors`), this page's origin in every
  command, copy buttons, a DHCP warning for IP origins, and "check again" that re-probes. ANIMA shows it once per
  session under an answer when a server answers 403.
- **E2E runner in three phases** (`npm run web:e2e` → `tools/web-e2e/run.mjs`): shell suites three at a time, shell-smoke (5 browsers) alone, then the
  real-model suites (Ollama, WebGPU) alone — next to five headless browsers they measured contention (GPU OOM for
  every model, 4-minute turns), not NucleoOS.
- **Every prompt carries the full clock:** the cloud path and the web-search prompt passed a bare ISO date (no
  weekday, no time) — now `nowText()` in the OS language; the web-search prompt is 5-language too.
- **ANIMA C cascade — polite want-to-know wrappers:** "vorrei sapere / mi piacerebbe sapere / sono curioso di
  sapere / I'd like to know / I was wondering …" are peeled before the opener is read (`a_know_wrapper`), live
  readings still win ("vorrei sapere che ore sono" → TIME, a launch never rests on such a phrasing), English
  indirect questions ("do you know what X is") reduce to X, and the proper-noun guard no longer mistakes
  "piacerebbe"/"curioso" for a name. Gate: 0 fabrications across every hallucination suite.

## Repository hygiene & professionalization (2026-09-30)

A pass over everything *around* the code — what a first-time reader or a license reviewer sees.

- **Untracked build output & scratch:** stopped tracking 214 regenerable/personal files — host-gate
  `.exe`s under `build/` and `tools/*-host/build/`, `*.pyc`, 81 build/flash logs, `_scratch/`, the
  Windows app's `dist/`, and deploy-time device-state backups. Files stay on disk; `.gitignore` was
  rewritten in English and grouped by purpose so the tree only holds sources.
- **English-only enforced:** the Italian root plan `piano-cardputer-os.md` became
  [`docs/original-plan.md`](original-plan.md) (indexed under History); `OVERSIZED-ASSETS.md`,
  `tools/nucleo-suite/README.md`, the `oversized-assets/` tooling, and the TTS/oversize comments in
  `deploy.ps1` / `sd-sync.ps1` / `sd_deploy.py` were all translated.
- **No more vendored third-party trees:** `reference/bruce` (AGPL-3.0, ~63 MB) and `reference/esp-claw`
  (Apache-2.0, ~22 MB) were full upstream copies checked into a PolyForm-Noncommercial repo. Replaced
  by [`reference/README.md`](../reference/README.md) (upstreams, licenses, clone commands); firmware
  header comments now cite `upstream pr3y/Bruce`. Nothing built against them.
- **Copyrighted media removed:** the *Wallace & Gromit* / *Screamers* `.nfv`/`.mp3` test clips under
  `tools/nfv/out/` (and the SD `data/Videos/` copies) are no longer tracked; `*.nfv` is ignored.
- **Oversized assets fetched on demand, not split into git:** the 1.4 GB of 90 MiB parts is gone from
  the working tree. The Qwen GGUF (streamed from Hugging Face by the browser) and the regenerable
  `teacher_*.npy` cache were dropped entirely; the two ~400 MiB TTS clip banks move to a GitHub Release.
  [`oversized-assets/rejoin.mjs`](../oversized-assets/rejoin.mjs) now resolves each asset *on disk →
  local parts → Release/CDN download*, SHA-256-verifying every result (`npm run assets`).
- **Consistency:** `package.json` version aligned to the released `0.4.0`; stale memory path dropped
  from `CLAUDE.md`.
- **History rewrite (separate step):** the copyrighted media, the vendored `reference/` trees and the
  dropped GGUF/teacher parts still exist in git history; a `git filter-branch` + force-push removes
  them and shrinks the ~3 GB `.git`. The TTS parts are purged in a second pass once the Release exists.

## Backlog found by the audit (to fix, highest risk first)

### Data loss
- [ ] **File Commander trash**: deletes the original even when the trash copy failed (no `r.ok`), throws on a
  fresh SD (`trash.json` missing → `{}`), and `/system/.trash` is delete-protected by the firmware
  (`nucleo_fsprotect.h`) so the bin can never be emptied (Recycle Bin purge/restore get 403 and still say
  "done"). Fix: `/api/fs/move`, check `r.ok`, trash outside `/system` (or firmware exemption).
- [x] **Failed read treated as empty, next save overwrites the SD**: calendar, authenticator, **tasks, contacts,
  qr** fixed — typed reads (404/"{}" = none yet; 5xx/offline/401 = unknown; unparsable = left untouched), no edit
  on an unknown list, read-modify-write so changes made elsewhere (another browser, the native QR app, ANIMA) are
  merged, deletes by id not by position, and no false "saved": tasks retries a refused write with backoff and
  warns before closing with unsaved changes (it said "Saved locally" while saving nothing). All in 5 languages,
  E2E `data-safety.e2e.mjs` (21 cases with settings).
- [x] settings: a failed read of settings.json showed the defaults and the next save (even a theme tap) wrote
  defaults + that change over the real file (Wi-Fi, language, device name). Typed read; saving blocked until the
  real file is read; re-read → merge → write kept (E2E `data-safety.e2e.mjs`, settings case).
- [ ] Folder delete calls `rmdir` on non-empty folders and ignores the failure.

### Broken flows
- [ ] Opening a file: shell ignores manifest `handles`; File Commander as fallback lists the FILE as a folder;
  no "Open with…". Apps ignore `?path=`: contacts (.vcf, no import at all), ir-remote (.ir), log-viewer (.evt),
  tasks (.todo), archive-manager (.zip — stuck on "waiting"), video-studio. Photos go to Paint, not Photos.
  Arcade ROM extensions not associated.
- [ ] Archive Manager: zips without folder entries fail to extract (firmware write doesn't create parents).
- [ ] Video Player: avi/nfv routed to `<video>` that cannot decode them; no error listener (silent black).
- [ ] Browser app: Back/Forward/Reload open a new tab each time; `/api/proxy` hides the upstream status (a
  remote 404 is saved as a "successful" download).
- [ ] Voice Manager delete → 403 under protected `/system/voice`; Dictation Groq URL misses `/openai`.
- [ ] Updates app can report "update complete" when nothing was flashed; duplicate of Settings ▸ Updates.
- [x] Calculator: `-(2+3)` = 1, `2*-(1+1)` = 0 (unary minus before a bracket) — fixed, 12 cases pinned.
- [ ] Settings: Wi-Fi/BLE/Swarm/IPv6/device-name/time-zone toggles save but nothing reads them.
- [ ] Mail: offline shown as "no account"; STARTTLS unsupported but port is free text; Outlook preset 465.
- [ ] miei-fatti: es/fr/de teaching phrases the firmware doesn't understand; "Edit" deletes immediately.
- [ ] Authenticator: HOTP/SHA512 silently produce wrong codes.
- [ ] Clipboard over http (`navigator.clipboard` absent): unit-converter shows "Copied" falsely, help copy
  throws, authenticator paste fails → use the shell clipboard bridge.

### ANIMA knowledge on the device
- [ ] **The device SD payload lacks the HDC triple store**: `deploy/sd/data/anima/learned` has facets but no
  `mind.it/en.jsonl` (born/died/capital/continent triples), and `deploy/sd-safe` has no `learned/` at all. On the
  real Cardputer "quando è nato Einstein" likely abstains offline. Fix: `python tools/anima/extract_triples.py --apply`
  then an SD sync — **only with explicit approval** (it changes what ships).

### Device load
- [x] **Every boot crawled the SD** for the file-search index (`fsindex.js`: breadth-first `/api/fs/list` of `/data`,
  up to 600 folders, during the session restore) and **every save under /data re-crawled it** 1.5 s later. Now the
  browser's kept index answers at once, a change only marks it stale, and the crawl runs on the first search (or at
  once only while a search is open). Cold boot on the simulator's small SD: 10 folder listings → 1 (the Desktop); on
  a real SD the crawl was up to 600. The shell's own cold boot is now 38–42 device requests in all five languages
  (`shell-smoke` measures it with no saved windows — with restored apps the count depended on timing); pinned by
  the `requests: 60` / `fsLists: 2` budgets and `tools/shell-fsindex.test.mjs`.
- [x] Catalogs fetched once per session: app windows read core/shell i18n from the shell's in-memory Map (same
  origin), and concurrent `I18N.init()` calls share one request (Settings fetched core.en/core.<lang> twice, and a
  boot with four restored windows six times). Settings reads `teacher.json` once (shared `readTeacher()`, in-flight
  dedup) instead of twice at open.
- [ ] Firmware serves JS/CSS/HTML/JSON with `Cache-Control: no-cache` → every boot/app open revalidates
  every file. Needs a firmware change (versioned immutable assets). **Flash only with explicit approval.**
- [ ] The idle `/ws` is the LRU victim under `lru_purge` (only client frames refresh it) — firmware can bump
  its LRU counter when serving other sessions.
- [ ] Polling not paused when hidden/minimised: games host signalling (700 ms), lobby (4 s), ir-remote sweep
  (350 ms), settings poller, log-viewer health (12 s), system-monitor.
- [ ] Help: 77 sequential `/api/fs/read` at start → one cached index.
- [ ] Heavy libraries fetched from the SD BEFORE the CDN: `web-llm.js` 6.1 MB (anima, spreadsheet, agent),
  `vosk.js` 5.8 MB, paint's 23.6 MB ONNX runtime (outside `dlgate`) → CDN-first, SD fallback.

### Big apps
- [x] Claude ids: defaults/tiers are Sonnet 5.5 / Opus 5.5 / Haiku 4.5 everywhere (shell, agent, ANIMA stream,
  games, paint, firmware teacher default), and a config still holding a FORMER factory default (4.6 / 4.8)
  follows its family to the newest served model; a model the user picked is still honoured (`ai-models.test.mjs`).
- [ ] agent: "no key" shown when the device is merely unreachable/unpaired; ~12 Italian status strings.
- [ ] code-runner: Ctrl+S saves twice; "Fetch JSON" example always fails; `os.hw` granted without permission.
- [ ] paint: saving .bmp/.gif writes PNG bytes; tour checks a wrong model path; it/en-only NL commands.
- [ ] spreadsheet: formatting lost on save (CSV only); `;`-separated CSV opens as one column; every AI
  reply labelled "Grok"; `=ANIMA()` hardcodes `lang=it`.
- [ ] anima: three inline i18n layers besides the catalogs. (Web-mode prompt: 5 languages now.)
- [x] ANIMA wasm brain rebuilt after the C cascade change (Emscripten installed at `C:\emsdk`,
  `apps/anima/local/build.ps1`): wasm parity 22/22 identical, **all 115 ANIMA gates green**.

### i18n (hardcoded strings)
- [ ] terminal (~207 literals), games (~60), settings AI extras (it/en only), paint inline dictionary,
  photo-viewer, archive-manager, radio, voice-manager (Italian curriculum), dictation (it/en only), ssh
  (Italian errors), qr, clock, system-monitor ("g" for days), wm.js window buttons.

### Wizards needed
Pairing (PIN location, lockout, QR), Wi-Fi join, AI engines (cloud / local server / in-browser / device),
mail account with test send, SSH bridge, DOS import, Arcade ROM setup, OTA via Settings, voice setup.
