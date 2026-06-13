# Maintenance review — June 2026

A targeted hardening pass: remove dead code, cut wasted device resources, fix RAM/fragmentation risks
and concurrency races — without changing observable behaviour or breaking gates. Method: parallel
read-only diagnosis (66 findings, each adversarially re-verified) → fixes applied in coherent batches,
each followed by the full `npm run anima:gate` + a firmware build. **Released to hardware** (firmware
flashed over COM3, web synced to the SD via `deploy.ps1` + `sd-sync.ps1`).

## What shipped, by area

- **Hygiene / dead code.** Removed ~48 MB of stale logs/dumps/temp dirs from the repo root; deleted
  orphan web assets (root-app `icon.svg` duplicates, stale `www/assets/icon.svg`, `codes.json`,
  code-runner `examples/`); dropped dead firmware (`teacher_pure`, 9 GC'd UI functions,
  `nucleo_screen_buffer`, phantom decls); fixed the lying `app_gfx.h` comment (release frees 32 KB,
  it is NOT a no-op). Relocated the games gate out of `_scratch/` to `tools/games-host/`.
- **Concurrency (crash-grade).** Guarded `nucleo_anima_l1_unload` behind the spine lock for external
  callers (`nucleo_anima_l1_unload_if_idle`) — fixes a use-after-free that rebooted the device;
  epoch-orphaning of the ANIMA worker instead of `vTaskDelete` (no more lock/arb leak on app exit);
  `nucleo_recorder_stream_abort()` before `httpd_stop`; atomic single-owner mic claim; learning-mode
  state under a critical section; event-bus sink on a stack copy; `/api/anima/verify` spine-locked.
- **RAM peaks (the headline).** Radio refuses `https://` (the CA bundle made an un-gated ~40-50 KB
  TLS handshake reachable beside the decoder); the calendar add **fails closed** instead of silently
  rewriting the file with only the new event; transcribe/teacher reclaim L1 before bailing; PTT
  fail-fasts on a busy arbiter / tight heap before the ~38 KB spin-up; transcribe reads lazy-grow.
- **Reclaim.** HDC self-test off in production (`CONFIG_NUCLEO_ANIMA_HDC_SELFTEST` default off; it ran
  every boot AND was muted at WARN); recorder/cal-svc/audio `.bss` trimmed; IR RMT enabled only around
  a transmit (frees the APB pm-lock at rest); recall norms `double`→`int32`+`sqrtf` (no soft-float).
- **Web traffic on the device.** The shell rebroadcasts `/api/status` as a `status.snapshot` message
  so embedded apps stop self-polling; a new `os-visibility` WM primitive pauses hidden apps; the
  search index re-crawl is filtered to `/data`; Voice Manager no longer fights the shell for the
  single `/ws`; the Game Center lobby stops re-`mkdir`-ing (and the firmware only fires `fs.changed`
  when a dir was actually created); clipboard byte-capped; `teacher.json` memoized.
- **App caching.** A version-keyed SW cache (`APP_CACHE`) serves app assets cache-first so repeat
  opens hit zero device reads; the SW `activate` now **preserves** the Forge model cache (a version
  bump used to wipe downloaded model weights — GBs).
- **Event journal.** Ephemeral high-frequency topics (`rec.level`, `voice/state`, `voice/match`,
  `system.busy`) are live-only — no longer the SD write that jittered the realtime voice/recorder
  loops — and `events.ndjson` now rotates (O(1) `.0` rename) instead of growing forever.

## New anti-regression gate

`npm run gz:check` (`tools/check-gz.mjs`, also folded into `npm run validate`) verifies every served
`.gz` (web/shell + apps/*/www) is in sync with its source. This is the recurring **".gz shadowing"**
gotcha: the webfs serves `<file>.gz` with precedence, so a stale sibling (source edited, `.gz` not
regenerated) ships old code, and an orphan `.gz` (source deleted) ships a deleted file. Run
`node tools/gzip-assets.mjs <dirs>` to regenerate, then `npm run gz:check` to confirm. The games and DJ
gates are now single-`node` entry points (`tools/games-host/all.mjs`, `apps/dj/test/all.mjs`) and are
listed in the central test catalog (`npm run test:registry`).

## Deliberately NOT done (honest)

- **Unify the LLM provider resolver across apps (F7).** Zero device benefit (it is browser JS; the
  app cache already removed the transfer cost). The copies have already diverged in possibly-intentional
  ways (agent's `pickCfg` lacks the xAI step), one is host-test-coupled, and it is untestable without
  live keys → medium risk for hygiene alone. Skipped.
- **Move large `.bss` to lazy `malloc` (audio in/out, ANIMA app state, registry).** The adversarial
  verifier flagged this as the OOM-fragmentation anti-pattern the device already fights: deterministic
  reserved `.bss` is not the bottleneck — runtime contiguous allocations (L1/TLS/decoder) are. Deferred.
- **APK button in Start (F57).** Cosmetic, no device impact.

Full per-finding detail (with adversarial verdicts) is in the session working notes; the released set
covers batches 1–8 minus the items above.
