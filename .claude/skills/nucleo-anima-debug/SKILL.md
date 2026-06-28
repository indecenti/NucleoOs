---
name: nucleo-anima-debug
description: Debug and develop NucleoOS firmware/ANIMA logic on the PC without flashing — the host harness compiles the real C and runs it locally. Use when working on ANIMA (the offline NL assistant), retrieval cascade, NLU, knowledge graph, or any firmware logic, and when deciding how to verify a change. Lists the gate suites to run before claiming a fix.
---

# Host-first debugging for NucleoOS / ANIMA

**Default: do NOT iterate by flashing.** The host harness compiles the *real* ANIMA
firmware C (MinGW gcc) and runs it on the PC — no device, no WiFi, no cable. Flash only to
*confirm*. Don't jump to flash→WiFi for logic work; that's the slow path and a user pref.

```
npm run anima -- "che cos'è nucleoos"   # one-shot query against the real cascade
npm run anima                            # interactive REPL
npm run anima:build                      # force rebuild the host exe
npm run anima:sweep                      # batch over tools/anima-host/queries.txt
```

## Gate suites (run the relevant ones before claiming a fix)
- `npm run anima:gate` — **the 12-gate regression suite** (0-hallucination, skill-routing,
  …). This is the pre-flight; it must be green before any flash.
- `npm run anima:route` — dispatch/routing snapshot (catches cascade-order regressions).
- `npm run anima:reason` — math/reasoning layer.
- `npm run anima:halluc` / `anima:meta` / `anima:boundary` / `anima:realistic` —
  anti-hallucination stress (485-phrase suite + metamorphic harness).
- `npm run anima:typed` / `anima:typednl` — typed knowledge graph.
- `npm run anima:local` — the browser WASM cascade (policy + contract + capability + parity).
- Per-subsystem (non-ANIMA): `npm run <ir|link|mesh|eth|hw|skill|radio|logviewer>:test`,
  `npm run validate` (registry/manifests), `npm run i18n:gate`.

## Live device debug (when host isn't enough)
HTTP debug without flashing: `/api/logs` (no auth), `/api/diag` (consolidated). Pair with the
6-digit PIN shown on screen → session cookie → `/api/fs`. The IP/PIN drift per environment —
read them fresh; don't hardcode. `delete` is NOT recursive. See `docs/debugging.md`.

## Pitfalls
- Host dim is 256, device dim is 192 — vector dims differ between host and device.
- Don't rebuild the L1 index casually: k-means isn't reproducible and the reshuffle breaks
  gates (see memory `anima-l1-index-rebuild` / `anima-index-provenance-guard`).
- L1 query stack: httpd task needs ≥24 KB or it panics.
- Online tier is LLM-only by design (cloud Grok/Claude → browser WebLLM, never the offline
  cascade); the "Grok" teacher is actually configured as **Groq** (Llama) by default.
