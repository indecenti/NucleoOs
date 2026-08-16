# Maintenance review — August 2026

A deep analysis + fix pass focused on making the test harness runnable off-Windows, hardening the
gate against a fresh clone, and hunting real correctness defects. Method: bring the whole
verification matrix up on a **Linux/i386 host** (no ESP-IDF, no PowerShell, no MinGW), run every
gate/suite that can run there, and treat each red as a hypothesis — a real bug, a stale artifact, or
a missing fixture — proven either way before acting. All fixes below are verified on host.

## Environment note

The harness was previously **Windows-only** end-to-end (PowerShell + MinGW + `windows.h`). This pass
made the two gate-critical harnesses portable, so the real firmware C now compiles and runs on Linux
(and therefore CI). Two artifacts still require the author's toolchain to regenerate — see
*Needs the author's machine* below.

## Fixed & verified (host)

- **Portable ANIMA host harness.** `tools/anima-host/build.sh` (POSIX mirror of `build.ps1`);
  `esp_timer_host.c` (clock_gettime) and `host_main.c` (native UTF-8 argv + `/proc/self/exe` chdir)
  guarded by `#ifdef _WIN32` — Windows path byte-for-byte unchanged. `anima.mjs` now selects the
  per-OS build script and **reports spawn errors instead of dying silently** (it used to exit 1 with
  no output when `pwsh` was absent). The real firmware cascade compiles clean and answers IT/EN.
- **Portable arbiter host test.** `tools/arb-host/build.sh` + `arb_host_compat.h` (a thread/mutex/
  atomic/time shim; Win32 expansion identical) make `nucleo_arb.c` — the PSRAM-less device's
  heavy-work arbiter — run under pthreads. `arb-check.mjs` picks the per-OS script. Proof passes 5/5
  (mutual exclusion across 12 threads with zero overlap, FG-preempts-BG, never-block, heap-floor).
- **Real firmware bug: gallon→liter precision.** `anima_solve.c` used the truncated US-gallon factor
  `0.00378541` m³. Besides a ~0.5 ppm error on every conversion, "5 galloni" landed on an exact
  rounding half (18.92705) and printed `18.927` instead of `18.9271`. Replaced with the exact
  definition `0.003785411784`. `math-check` 472/473 → **473/473**.
- **Fresh-clone robustness (gates crashed instead of running).** Several checks read host-only
  fixtures that a fresh clone / CI never builds and blew up (ENOENT / exit 2 / confusing length
  mismatch) rather than running or skipping:
  - `forge-delivery` + `forge-model-manifest`: keyed "staged" off `manifest.json` alone, then failed
    against absent weights / 133-byte LFS pointer stubs. New shared guard `forge-staged.mjs` requires
    every declared file present at its declared size; otherwise the test skips cleanly.
  - `ledger-attack` (VKL security gate) and `evolution` (VKL) and `typed-facets` (KG): now fall back
    to the committed device tree (`tools/sd-sim` == `deploy/sd`) when the host fixture is absent, so
    the security/KG gates **actually run** on a fresh clone. All three pass.
  - `gen_dicts.py --check`: a *missing* dict in the untracked host build-artifact tree was reported
    as "STALE (seed edited)"; now it's correctly "not built (skipped)". The real guard — a shipped
    dict that *differs* from the seed — still fails everywhere.
- **`games:gate` broken by an unstubbed import.** `nucleo-game.js` / `llm-brain.js` import the
  OS-absolute `/nucleo-i18n.js`, which Node can't resolve; `test-foundation.mjs` and `test-brain.mjs`
  didn't shim it → `ERR_MODULE_NOT_FOUND`. Added a passthrough i18n stub + import rewrite. All game
  suites pass (multiplayer 47, forza4 40, tris 30, pong 35, brain 12).

## Wi-Fi: known-network priority (activated) + native management

The multi-network store (`nucleo_setup` — up to 16 saved nets, NVS + SD, auto-pick best-in-range) and
the host-tested supervisor (`wifi_policy.c`, 160 assertions) are solid. The gap: the per-network
**manual priority** field drove `connect_best_known()` ("priority first, then RSSI") but **nothing ever
set it** — no HTTP route, no UI — so it was always 0 and auto-connect was effectively RSSI-only.

- **Activated priority over HTTP** (verified): new `POST /api/wifi/priority {ssid,priority}` (pin/unpin,
  clamped 0..9) and `POST /api/wifi/reconnect` (rejoin best known now). Both auth-gated, mirror the
  existing `wifi_forget` handler. Documented in `registry/api-docs.json`; `npm run gen:api:check` green
  (75/75 routes). This lets the user prefer, e.g., home over a closer open hotspot the device once joined.
- **Native Settings app** (`app_wifi.cpp`): in the Wi-Fi list, **`P` pins/unpins** the selected saved
  network (toggles priority 0↔5 via `nucleo_setup_net_set_priority`); preferred nets show a green pip +
  "pref" label; hint updated. Reuses the existing scan-list + the DEL=forget pattern. **Compiles in the
  firmware build here**; flash to see it on the TFT.

## Multi-agent review — confirmed device bugs (fixed)

An adversarially-verified review (per-component reviewers → skeptic verifiers; 7 confirmed of 14
candidates) found real defects in external-input parsers. All fixed and **compiled clean in the full
firmware build here** (see Verification status); flash to confirm runtime.

- **`nucleo_link_espnow.c:232` (high, OOB read).** Bruce file-transfer receive did
  `fwrite(m->data, 1, m->dataSize, ...)` with `dataSize` taken verbatim off the ESP-NOW frame; `m->data`
  is only `BRUCE_DATA_SIZE` (150 B) inside the fixed rx packet. A hostile peer sending a large `dataSize`
  drove fwrite past `data[]` (OOB read of packet/stack, huge bogus write). Fixed: clamp to `BRUCE_DATA_SIZE`.
- **`fido_ctap2.c:169` & `:242` (high, OOB read).** CTAP2 makeCredential/getAssertion copy `rp.id`/`rpId`
  into `char rpid[128]` via tinycbor, which does **not** NUL-terminate when the text exactly fills the
  buffer (len == cap). `strlen(rpid)`/`sha256`/UI callbacks then over-read the stack. Fixed: reject
  `rpidl >= sizeof rpid` and NUL-terminate.
- **`nucleo_ws.c:189` (high, NULL deref → reboot/DoS).** WebSocket `{"op":"subscribe","since"}` with a
  `since` token but no `:` made `strchr(p,':')` return NULL, then `strtoul(NULL+1,…)` dereferenced 0x1 →
  LoadProhibited reboot from any authenticated client. Fixed: NULL-check the colon.
- **`nucleo_eth.c:426` (concurrency, UAF + double fclose).** `nucleo_eth_stop()` fclosed `s_pcap` after a
  bounded 1 s join, but the pcap worker already owns that close on exit — a slow SD write outlasting the
  join gave a use-after-free + double free. Fixed: worker is sole owner; stop() no longer closes it.
- **`nucleo_voice.c:302` (OOB read).** A 32-byte template header field with no NUL was passed to
  `snprintf("%s", hdr)`. Fixed: `hdr[31]=0` after the read.
- **`nucleo_weather.c:191` (logic).** Geocoded `place` written into a hand-rolled JSON cache unescaped; a
  quote/backslash produced malformed JSON that `cache_load`'s `cJSON_Parse` then rejected. Fixed: build
  with cJSON (auto-escapes).

One review group (`wifiatk`/`evilportal`, 802.11 attack tooling) was blocked by the model's cyber
safeguards and not covered by the automated pass; `evilportal`'s template path was hand-reviewed earlier
(clean). `nucleo_wifiatk`'s inbound path (promiscuous RX → `hs_add`/EAPOL parse) was hand-reviewed
afterwards: `len` is bounded to `HS_FRAME_MAX` before the frame memcpy, `eapol_off()` guards every header
read (`len < eo+7`), and the EAPOL message id is 0..4 so `1u<<msg` is safe — clean.

## Multi-agent review — confirmed app/game bugs (fixed)

A second pass over `nucleo_app` (52k lines, 12 groups, adversarially verified; 10 confirmed of 14). Most
are the P2P-decode class — a peer-controlled count/index used without a bound — the same shape as the
`app_tankduel` fix. All fixed and **compiled clean in the firmware build here**.

- **`app_snake.cpp:554` (high, OOB write).** `apply_state()` trusted wire `s1_len`/`s2_len` (a spoofable
  peer can send up to 87 vs the `MAX_SEG`=60 arrays); the copy loops overran `bx/by[]` and clobbered the
  snake's `len` field → a ~2-billion draw index. Fixed: reject `n1|n2 > NET_SEG`. Also clamped the wire
  power-up fields that index `PU_COL/PU_SYM[PU_COUNT]`.
- **`app_tanks.cpp:1314` (high, OOB read).** `TK_AIM` set the tank weapon from an unclamped peer byte,
  then every frame indexed `WEAPS[]`/`ammo[]` with it (wild `const char*` → snprintf fault). Fixed: clamp
  `>= NWEAP` like the sibling `TK_FIRE`/`TK_RESULT` handlers.
- **`app_ssh.cpp:114` (high, DoS).** A TAB from the SSH server on a line at column ≥104 made the tab-stop
  loop (`while (s_curlen < t) sb_putc`) spin forever — `sb_putc` caps at `SB_W-1` so the target was never
  reached — hanging the SSH task **while holding the scrollback mutex**, freezing the UI. Fixed: clamp the
  tab target to `SB_W-1`.
- **`app_brawler.cpp` / `brawler_net.cpp:270` (medium, NULL deref → DoS).** The guest stored a peer enemy
  `kind` unchecked; the draw path deref'd `brawler_enemy(kind)` (NULL when out of range). Fixed: drop the
  slot when `brawler_enemy()` returns NULL instead of storing an invalid kind.
- **`app_notepad.cpp:498` (OOB read).** The editor line cache is `lines[40]`; content filling exactly 40
  wrapped rows made the render loop print `lines[40]`. Fixed: cap `total` to 40.
- **`app_player_db.cpp:209` (NULL deref).** `strdup()` result used without a NULL check. Fixed.
- **`app_video.cpp:679,925` (buffer overflow).** `snprintf(dot, 5, ".mp3")` could write past `apath[256]`
  when the extension dot sat near the buffer end. Fixed: bound to the remaining space.
- **`app_anima.cpp:2293` / `app_evilportal.cpp:707` (low logic).** Digit-shortcut range spilled past `'9'`
  into punctuation; captive-portal page-picker selection was reset by `go()` (dead store). Both fixed.

## Web/shell review — confirmed bugs (fixed & verified here)

A third pass over the browser layer (~35k lines JS: the shell PWA + apps). Unlike the firmware, these
**are** verifiable on this host — every fix below is syntax-checked and passes `browser:check`, the forge
tests, and the full unit suite. 10 confirmed (self-verified against the source when the workflow's verify
phase was cut short to wrap up).

- **`web/shell/shell.js` ×3 (high/medium, broken contract).** The device↔web language sync, the web→device
  change, and the boot restore from `settings.json` all hard-coded `it||en`, silently **dropping es/fr/de**
  — even though all 5 languages are 100% translated and the firmware stores/broadcasts the code verbatim.
  Fixed: validate against `NucleoI18N.LANGS` (and let `setLang()`'s own normalize gate the WS path).
- **`apps/anima/www/forge/model-store.js` (high, corruption).** The SD (Cardputer) shard downloader
  re-planned Range windows from byte 0 whenever the window shrank (throttle / device 503), so the old loop
  index then pointed at a different byte range — chunks overlapped and the assembled shard was corrupt.
  Fixed: an **offset-based** walk, so a window change only affects the next window.
- **`apps/agent/www/runtime.js` (high, crash).** In `device_status`, `const t = new Date(...)` shadowed the
  injected translator `t()` with a TDZ that crashed the `!r.os` error path. Fixed: renamed to `dt`.
- **`apps/voice-manager/www/app.js` (medium, stored XSS).** Trained command names and the device-supplied
  matched word were interpolated into `innerHTML` through `esc()`, which only escapes JS-string quotes, not
  HTML. Fixed: added a real HTML-escaper (`escH`) for every text-into-innerHTML site; kept `esc()` for the
  `onclick='…'` JS-string context. Also fixed the 'fair' badge color `#fd20` (4-digit #RGBA = transparent).
- **`apps/paint/www/imaging.js` (high, logic).** Magic-wand seed X was run through the 0–255 *colour* clamp,
  picking the wrong flood-fill start on any canvas wider than 256 px. Fixed: clamp X to `[0,w-1]`.
- **`apps/paint/www/diffusion/clip-tokenizer.js` (high, parse/loop).** BPE pairs were keyed by bare
  concatenation and recovered with `best.split('')` (single code units), corrupting any multi-char merge and
  spinning. Fixed: a NUL-separated key, split back exactly.
- **`web/shell/notify.js` (low).** Opening the notification center ran `add('open') && remove('open')` on the
  Start menu — `add` returns undefined, so it *opened* the Start menu instead of closing it. Fixed.

## Verification status (on this host)

ESP-IDF v5.3.2 **is** installed here (`~/esp/esp-idf`, xtensa-esp32s3 toolchain) — so the device firmware
was actually compiled, not just statically checked. What was verified:
- **Full firmware build**: `idf.py build` → **exit 0, 0 errors**, `nucleoos.bin` generated (0x2b3f50 B,
  23% partition free). Every modified device component compiled and linked with the real xtensa-esp32s3
  cross-compiler (nucleo_app, nucleo_httpd, nucleo_ws, nucleo_link, nucleo_fido, nucleo_eth, nucleo_voice,
  nucleo_weather, nucleo_anima) — including the native Wi-Fi app and the two new HTTP endpoints.
- **Runtime-verified the FIDO fix** (not just compiled): `tools/anima-host/fido-ctest.c` host-compiles
  the real `fido_ctap2.c`. Added two regression tests that send a makeCredential/getAssertion with an
  `rp.id`/`rpId` of exactly 128 bytes and assert a clean `0x14` (MISSING_PARAMETER) — i.e. the guard
  fires and there is no `rpid[]` stack over-read. `fido:test` **54/0** (was 52). The tests fail against
  the un-fixed code, so they lock the fix in.
- **Host harnesses of touched/adjacent components**: `link:test`, `eth:test`, `weather:test` all green —
  no regression. `math-check` 473/473 (the gallon fix), arbiter concurrency proof 5/5.
- **Full unit suite**: `node --test tools/**/*.test.mjs` → **771 pass, 0 fail, 2 skip** — no regressions.
- **Web/JS fixes**: syntax-checked, `browser:check` green, forge/model-store tests green.
- **Static analysis**: cppcheck over every modified file → no new warning and no syntax error.
- **api-spec**: `gen:api:check` green, 75/75 routes (the two new Wi-Fi routes included).

The only step this machine can't do is **flash + run on the physical Cardputer** (no device attached) — the
runtime confirmation. Everything up to and including a clean, complete firmware image is verified here.

## Needs the author's machine (real, but toolchain-bound)

- **Stale L1 index (device).** `check_pack.mjs` fails: `deploy/sd`, `deploy/sd-safe`, `tools/sd-sim`
  ship an index built from corpus `03e7eb18…` while the current corpus is `5dd83925…`. The knowledge
  text was edited without rebuilding the derived index, so the device recalls stale knowledge and new
  corpus facts are unreachable by L1. Fix: `npm run anima:packs` (needs scikit-learn — no i386 wheel,
  scipy source build is a no-go on this 1.8 GB machine).
- **Stale browser-local engine.** `apps/anima/www/local/anima-local.wasm` still ships the homograph
  translation bug already fixed in C (`nucleo_anima_translate.c:264` — the code even carries the
  "Era il bug…" comment): `traduci cane in inglese` dumps both directions in the browser twin while
  the firmware correctly answers `in inglese: dog, hound, trigger`. Caught by `parity.mjs` (21/22).
  Fix: rebuild the WASM (`apps/anima/local/build.ps1`, needs emscripten).
- **No CI runs the gate.** `.github/workflows` only packages a release; nothing runs `anima:gate` /
  `check_pack` / the test suites. The two staleness defects above would have been caught by CI. Now
  that the harness is Linux-portable, a CI job is feasible. Also still Windows-only (not gate-blocking
  today): `imu-host`, `voice-host`, `eventbus-host`, `nucleo-suite`, and the two `*build.ps1` above.

## Method notes for reruns on this host

- Node lives at `/opt/node22/bin` (not on PATH). The C host harness needs its fixture seeded:
  copy `models/anima-it-encoder.bin` + `anima-it-index.bin` and `tools/sd-sim/data/anima/learned/`
  into `tools/anima-host/sd/data/anima/`, and `python3 tools/anima/gen_dicts.py` to write the host
  dicts. With that in place, the NL gate reds are **fixture flips at 192d**, not firmware bugs
  (proven: `translate` 21/67 → 67/67; `capability` 3 fail → 0). The remaining reds are the two
  stale artifacts above, which need the author's toolchain.
