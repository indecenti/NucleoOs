# Swarm Architecture — Decision Record (ADR)

**Status:** accepted (2026-06-22) · supersedes the unbuilt `swarm` manifest stub and the
"swarm extension" sketch in `docs/event-protocol.md`.

## The question

Make multiple Cardputers work together. The seductive first idea was **master/slave
capability-lease** ("ORGANS"): a master *commands* a peer to drop its duties, enter a dedicated
mode, run a job, and stream back the result. We adversarially validated it against the **real
firmware** before committing. It was rejected as the lead design.

## Why master/slave was rejected (verified, not opinion)

1. **The RAM premise is false.** A full ANIMA query (incl. the L1 cascade) does **not** run in the
   caller — `nucleo_httpd` spawns a transient **30 720-byte worker** (`nucleo_httpd.c:777,807`). On
   this no-PSRAM chip the largest free block is **~31 KB**, and the shipping code only carves that
   worker by **unloading the idle L1 index** (`nucleo_httpd.c:779-781`). A "resident organ" wants L1
   loaded *and* a 30 KB worker → structural contradiction. "The organ query is essentially free" is
   contradicted by the firmware's own reason for moving the cascade off-thread.
2. **The transport is open.** ESP-NOW peers are added on any PING with `p.encrypt=false`, frames
   validated by CRC32 only (`nucleo_link_espnow.c:79-80,179`). The claim "inherits Vicino's consent
   verbatim" is false: Vicino's consent is **a human pressing confirm**; a headless auto-granting
   organ deletes that gate. Any co-channel device that knows a `cap_id` could drive your Cardputer
   into exclusive mode (shedding httpd/mDNS → it goes dark to its own owner) and feed it poisoned
   "grounded" answers.
3. **Economics.** A Cardputer is a single-unit hobbyist device; the median owner has **one**. A
   design with no single-device mode is non-instantiable for most users.

## The decision

Build a **shared bulletin board**, not a boss. Two tracks, kept deliberately separate:

- **Track A — on-device swarm (the differentiator).** Extend the existing event bus over ESP-NOW
  so peers *advertise what they already are* and requesters *content-route* to them. No leases, no
  master election, no TTL/heartbeat, no "slave stuck in exclusive". Mode-entry is a **local,
  optional, per-transfer** decision the serving device owns — the foreground pattern Vicino already
  proves. Two layers:
  - **MESH** — the seam: foreign `mesh.*` events appear on every peer's local bus transparently
    (apps need zero swarm-aware code). Delivers presence + shared clipboard for ~64 B of RAM.
  - **CHORUS** — built *on top* of MESH: gossip a tiny self-describing **capability manifest**
    (≤208 B, not data); route a request to the peer that holds the right domain by its advertised
    `free_kb`. This is what enables the flagship: an offline ANIMA brain that is the **union of every
    device's SD**, larger than any single no-PSRAM device's ~31 KB index ceiling.
- **Track B — NucleoMesh (ship-now, parallel).** Promote `apps/games/www/nucleo-play.js`'s proven
  WebRTC-over-fs-rendezvous into an OS "rooms" primitive: multi-MB file/clipboard handoff +
  browser-compute fan-out. **Zero firmware, ~zero device RAM.** Shards across *browsers*, **not**
  device-side L1 brains — so it is **not** the flagship and is never presented as such.

## Confidence levels (read this before building)

- **Certain (build now):** the MESH seam + NucleoMesh. Host-testable, no flash, useful regardless.
- **Right direction, hardware-gated:** CHORUS routing + the distributed brain. Architecturally
  sound, but the *product* is **unproven** until a two-device soak (P5) confirms the serving device
  can carve the 30 KB worker with httpd shed, and until shard provisioning is frictionless. Do not
  pre-announce success.

## P0 seam — API contract (the only firmware-sensitive edit)

The bus today has a single `s_sink` (the WS broadcaster). P0 generalizes it minimally and
**additively** — the WS path must remain byte-for-byte equivalent.

```c
// nucleo_eventbus.h — added
// Sink signature gains `src`: NULL = locally-published, non-NULL = injected from peer <src>.
typedef void (*nucleo_event_sink_t)(uint32_t seq, const char *topic,
                                    const char *payload_json, const char *src);

// Register an additional sink (WS + mesh coexist). Bounded array, no malloc.
void nucleo_event_add_sink(nucleo_event_sink_t sink);

// Inject a foreign event from a peer onto the LOCAL bus (assigns a local seq so the
// browser can order/replay it). Delivers to sinks with src=<peer>. The mesh sink MUST
// NOT re-forward when src!=NULL (loop prevention). Dedup by (src,seq) lives in the mesh
// layer, BEFORE calling this — the bus stays dumb.
void nucleo_event_inject(const char *src, const char *topic, const char *payload_json);
```

Loop prevention is **src==self → forward; src==peer → deliver locally only** (no TTL needed).
Hard invariants, host-gated by `mesh:test`:
- **Zero heap allocation per publish/inject** (the WS static-pool fragmentation fix must not
  regress — see `nucleo_ws.c:64-74`).
- Mesh gossip is **rate-capped** and bounded; foreign events are deduped per-peer (small fixed
  table) and **never re-forwarded**.
- Only `mesh.*` / `chorus.*` topics are gossiped; everything else stays local.

## Phased plan

| Phase | What | Host-test? | Risk |
|---|---|---|---|
| **P0** | MESH seam: `add_sink` + `inject` + `src`; `mesh:test` asserts zero-malloc, no-WS-regression, dedup, loop-prevention | yes | low |
| **P1** | CHORUS directory core (SANS-I/O): 512 B manifest table, capability bitfield, content-router; `chorus:test` on a lossy 3-peer sim | yes | low |
| **P2** | swarm↔ANIMA glue: **ship query TEXT, not vectors** (organ re-encodes with its resident encoder — zero new buffers); **shard digest** (derived routing) + **answer fusion by confidence**. NO edits to the fragile L1 code. | yes | low |
| **P3** | ✅ Firmware engine `nucleo_swarm_espnow.c` (ESP-NOW + mesh/chorus/membership + mbedtls HMAC/PBKDF2) + native `app_swarm.cpp` "Sciame". **Build-verified** (`flash.ps1 -BuildOnly` green). First-test scope = presence + ping (OPEN). Still TODO: ESP-NOW PMK/LMK link encryption, passphrase wiring (settings/UI), L1 organ. | `idf.py` ✅ | med-high |
| **P4** | `swarm` app www (live organ map, provenance, manual override) + serving side as **load-on-accept** (enter exclusive, reload L1, recheck heap, decline if block < floor) | yes (preview) | medium |
| **P5** | **Two real devices** (only hardware step): measure largest-free-block ≥ 31 KB at query time, fragmentation watermark over sustained loops, owner-preempt. GO/NO-GO gate. | no | high |
| **B (//)** | NucleoMesh: lift `nucleo-play.js` into `os.mesh.*`, repurpose the `swarm` app as a Nearby Rooms lobby | yes | very low |

## Key optimization — text, not vectors (verified in firmware)

The first design shipped a precomputed 192-dim query *signature* to the organ. The L1 code review
killed that: the organ already has its encoder + index **resident**, and `l1_encode()` re-encodes
query text with **zero new heap** (static/stack buffers only; CPU is not the bottleneck on this chip).
So the wire carries only the **query text** out (tens of bytes, one frame) and a small
`(src, digest, confidence)` result back — never a vector. This removes the encoder from the *master*
entirely, removes vector serialization, and reuses the organ's resident encoder. Scores are
comparable across devices because they share the same encoder/index format (cosine → confidence%).

The organ answers by calling the **existing** `nucleo_anima_l1_query(text, en, want_detail, &out)`
([nucleo_anima_l1.c](../firmware/components/nucleo_anima/nucleo_anima_l1.c)) verbatim — the swarm adds
**no new ANIMA RAM and edits no L1 code**. The L1 path was audited for savings and found already
frugal (centroids streamed not resident, adaptive encoder cache, static buffers, `float` not
`double`); the only candidates save ~0 bytes and risk k-means/gate reproducibility, so L1 is left
untouched on purpose. The on-device sharded analog (AKB5) already exists and is gated.

## Joining a swarm (membership)

WPA-style, no camera needed. **Discovery is automatic** (MESH gossip on the agreed `channel`); what
needs a flow is *membership*:

1. The user sets a **swarm name** + **passphrase** on each device (native UI, or pushed from the web
   companion) — `network.swarm.name` / `network.swarm.passphrase`. Ships **disabled / empty** (an
   **OPEN**, not-running swarm) — never an open swarm by default.
2. `swarm_join()` derives a 32-byte **PSK** via an injected KDF — **PBKDF2-HMAC-SHA256 (≥100k iters,
   tuned to the Cardputer budget in the P3b binding)** on the device, a mock in the host gate. The
   salt is **`SW_SALT` + 0x1f + the swarm name** (SSID-like), so *same name + same passphrase → same
   swarm* and a global rainbow table can't attack the whole fleet. Passphrase policy: 8–63 chars
   (out of range → fail-safe OPEN / error).
3. **Admission** (`swarm_admit`): an inbound frame is accepted only if it is a mesh frame, the sender
   MAC passes the optional **trust-pin** (`peers[]`), and — for a closed swarm — its **HMAC verifies
   under the PSK** (constant-time). Knowing the passphrase = being a member. Untagged frames are
   rejected by a closed device (no silent downgrade).
4. Outbound frames are sealed with the HMAC tag (`swarm_seal_out`); link-layer ESP-NOW PMK/LMK
   encryption is layered on top by the device shim.

This is implemented and host-gated in `nucleo_swarm` + `nucleo_swarm_sec` (`npm run swarm:test`). The
KDF + HMAC bindings to mbedtls and the passphrase UI are the build-gated remainder (P3b/P4).

**Hardening deferred to P3b/P4** (surfaced by an adversarial security review of the join layer; the
pure layer is sound — these live in the unwritten device shim / config, not the host code):
- **PSK-only at rest.** Do NOT persist the plaintext passphrase: `/api/fs/read` can serve
  `settings.json` to an authenticated session (it blocks `..` but not absolute system paths), so the
  passphrase would leak and the PSK is derivable (salt+iters are public). The P3b shim must run the
  KDF and store only the derived 32-byte PSK; the `passphrase` field is write-only / transient.
- **PBKDF2 ≥100k iters**, timed against the Cardputer CPU/boot budget, pinned in the shim.
- **Replay across reboot / first-contact.** The MESH 32-window seq dedup loses state on reboot and a
  first-contact peer can be seq-floor-locked by a replayed high-seq frame. Fold a **boot-epoch
  counter** into the sealed frame (own task — it changes the mesh frame format). HMAC alone proves
  membership, not freshness.
- **Open-by-default avoided:** `swarm.enabled` ships `false` until a name+passphrase are set; the UI
  must show an explicit warning if a user chooses an OPEN swarm.

## Honest caveats (do not paper over)

- **Latency is batch, not chat:** 200–500 ms/query clean, 0.5–2 s under loss. Surface as "searching
  the desk". The local cascade or cloud is faster for any domain the device already holds — swarm
  only wins the narrow "offline AND domain-not-local" sliver.
- **Single channel for all ESP-NOW:** every member on the master's STA channel (or all parked on
  ch1). A forced channel move / deauth (NucleoOS ships Bruce deauth tooling) is a clean denial.
- **A serving device sheds httpd/mDNS while answering** → mandatory opt-in "organ mode" + instant
  local preempt (any keypress releases). Without it a 2-device household browns out mid-use.
- **Provisioning:** disjoint signed shards are required, or the union-win evaporates into duplicate
  domains. Needs a manifest/registry (domain/version/hash), not a self-declared, spoofable hint.
- **Host tests cannot surface the contiguous-block failure** that historically froze this device
  (boot freeze, centroid OOM, L1 stack overflow were all contiguous-block failures). The flagship
  GO genuinely depends on P5 and may still fail there.

## First two-device test (ready now — OPEN swarm)

All firmware is **build-verified** (`tools\flash.ps1 -BuildOnly` green; the swarm engine
`nucleo_swarm_espnow.c` + the native `app_swarm.cpp` link into `nucleoos.bin`). The first test runs
an **OPEN** swarm (no passphrase): presence + a ping/pong round-trip.

1. Flash both Cardputers (each on its own COM port — `flash.ps1` runs the host gate, builds, flashes):
   `powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM3`
2. On BOTH, open **Connect ▸ Sciame**.
3. Radio rendezvous: join both to the same Wi-Fi (ESP-NOW rides the AP channel) **or** leave both
   unassociated (they park on ch1). They must share a channel.
4. Within ~2–4 s each device lists the other under **Dispositivi vicini** (gossiped presence manifests).
5. Press **ENTER** on one → it broadcasts a ping; the other replies → the activity line shows
   **"pong da <id>"**. Round-trip confirmed.

Exercises end-to-end on real hardware: ESP-NOW init, the MESH gossip frame + per-origin dedup, the
CHORUS manifest directory, the membership admit path (OPEN), and the native UI.

**Not in this first test (next steps):** the CLOSED swarm (name+passphrase → PBKDF2 PSK + HMAC) is
built and host-tested but needs the passphrase wired (a settings read or the P4 UI); ESP-NOW link
PMK/LMK encryption; and the L1 "organ" (answering a peer's offline query) needs the load-on-accept
serving path. The first test deliberately proves the transport + presence first.

## Verified evidence

- Single-sink bus, stack-copy outside the lock: `nucleo_eventbus.c:32,43,112`.
- WS static-pool anti-fragmentation (the regression to protect): `nucleo_ws.c:64-74`.
- 30 KB transient worker for an ANIMA query: `nucleo_httpd.c:777,807`; carve-by-unloading-L1:
  `nucleo_httpd.c:779-781`.
- Open ESP-NOW transport: `nucleo_link_espnow.c:79-80,179`; human-confirm consent: `cmd_confirm_post`
  in `nucleo_link_api.c`.
- Existing reusable dedicated mode: `nucleo_exclusive.h` (flags + masks).
