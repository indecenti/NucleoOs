# Testing — the NucleoOS / ANIMA verification system

> Tests **are an integral part of the system**, and the most important part. They must **always all be
> valid and green**. The heart is the verification of **natural language** (ANIMA): that skills never step
> on knowledge's toes, that answers are never fabricated, that real requests work.

All tests are **centralized in a single source of truth**:

```
tools/test-registry.json      ← the catalog: EVERY test, with category, command, description, NL flag
tools/gen-test-registry.mjs   ← generates it (from gate.mjs + every *.test.mjs + the non-gate runners)
```

**Every** view reads from that one file: the gate, the CLI and the GUI. Regenerate the catalog when
you add/move a test: `npm run test:registry`.

## Three ways to run them

| View | Command | What it is for |
|---|---|---|
| **GUI** (cockpit + monitor, **one button**) | `npm run test:lab` (Python+Tkinter) | A 3-tab observatory. **Panoramica** (Overview): ANIMA's health at a glance (hallucinations=0, NL cases covered, routing drift=0, green/total, per-category health, ▲▼ vs the previous run). **Test**: **▶ Esegui tutti** (Run all) launches with ONE click ALL the tests that make sense offline (no web model — forge uses MockEngine); also NL-only/category/selection/failed. **Andamento** (Trend): sparklines of the metrics across all runs (is ANIMA getting better or worse?). |
| **CLI** (catalog) | `npm run test:anima` · `test:all` · `test:nl` · `node tools/run-tests.mjs --cat <id>` · `--grep <x>` · `--list` | The same list from the terminal/CI, with a green/red summary. |

> **Offline ANIMA only (the deterministic cascade, WITHOUT models).** `npm run test:anima` — or the
> **🧠 ANIMA offline** button in the GUI — runs *only* the 70 tests of the M1 programmatic cascade: the 8
> NL categories + cascade infrastructure + knowledge graph. It deliberately **excludes** `forge-webllm` (the
> WebLLM/M4 substrate — even mocked, it is about *models*), the app tests and the build lint. It is the
> "ANIMA running offline without models, and only that" subset (the `anima` flag in the registry).
| **Gate** (canonical) | `npm run anima:gate` | The mandatory pre-flight: red = flash/release aborted. ~43 ANIMA gates + the `*.test.mjs` suite. It is the canonical judge of "all green" (wired into `flash.ps1`/`release.ps1`). |

GUI, CLI and gate read the **same single source** (`test-registry.json`): zero duplicated lists. Both the GUI
and the CLI clean the SD state before every test (hermetic) and recompile the exe once at the start of a batch.

> **New tests included forever, automatically.** The catalog is regenerated from `gate.mjs` (every gate) and from
> a **scan of every `*.test.mjs`** in the repo. So a new test added as a `*.test.mjs` or
> wired into `gate.mjs` shows up in the cockpit at the next `npm run test:registry`, with no manual touch. Only
> a standalone runner (neither a `*.test.mjs` nor a gate) has to be added with one line in `extras` of
> `tools/gen-test-registry.mjs`. The app packaging lint (`tools/validate.mjs`) is deliberately **excluded**
> (it checks the app manifests, not ANIMA): run it separately with `npm run validate`.

## Categories

The categories put **natural language first** (8 NL categories 🗣 + 8 system/app ones). The GUI and the
registry show the category labels in Italian; the registry id is given in brackets.

| Category | NL | What it guarantees |
|---|:--:|---|
| **NL · Anti-hallucination** (`nl-hallucination`) | 🗣 | Adversarial traps: any confident answer to an unanswerable request = failure. Safety. (halluc-stress, halluc-battery, metamorph, nl-stress, reliability, false-positives, cross-topic, halluc-probe IT/EN, ood-check) |
| **NL · Skill routing** (`nl-skill-routing`) | 🗣 | The right request → the right tool; a skill **never steps on** knowledge and vice versa. (boundary, realistic, skill-routing 1/2, cross-skill, action-tier, image-gen, skill-isolation) |
| **NL · Knowledge & retrieval** (`nl-knowledge`) | 🗣 | L1/L2 retrieval, definitions, descriptions, entities: grounded or an honest abstention. (l1-parity, l1-recall, describe-stress, fluency-grounded, regress, typed-nl, entity-detect, clean-extract, akb5-content) |
| **NL · Reasoning** (`nl-reasoning`) | 🗣 | KGE deduction, HDC recall, combinatorial composition, typed facets. |
| **NL · Math & calculation** (`nl-math`) | 🗣 | Every math answer exact + parity with the JS twin. (math-check, calc-eval) |
| **NL · Memory & profile** (`nl-memory`) | 🗣 | User learning + typed personal profile: recall by paraphrase, zero misattributions. |
| **NL · Translation** (`nl-translate`) | 🗣 | Offline IT↔EN dictionary translator: grounded, honest decline, zero false positives. |
| **NL · Weather** (`nl-weather`) | 🗣 | Weather NLU: place/date extraction, offline/hybrid/online tier. |
| **Cascade · Infrastructure** (`cascade-infra`) | | Orchestrator, agentic loop, consistency of the index/encoder packs. |
| **Knowledge Graph & Ledger** (`knowledge-graph`) | | Taxonomy self-evolution (Wikidata) + an immutable, verifiable ledger. |
| **ANIMA Forge / WebLLM (M4)** (`forge-webllm`) | | In-browser agentic editor: action firewall, cross-substrate verification, download/engine, provenance (35 tests). |
| **App · Spreadsheet** (`app-spreadsheet`) | 🗣 | Excel-class formula engine + the copilot's NL→formula. |
| **App · Paint / Shell / Device** (`app-paint` · `app-shell` · `app-device`) | | Imaging, NL commands, browser, shortcuts, terminal, USB-HID, NFV. |
| **Build & lint** (`build-lint`) | | API spec anti-drift (Swagger ↔ firmware routes) + freshness of the served `.gz` files (`npm run gz:check`: no stale/orphan code shipped to the device). |

The **live** list (with exact counts and commands) is always: `npm run test:list`.

## Health monitor (not just tests)

The cockpit does not only give green/red: it reads **metrics** that every runner prints in its own output and
aggregates them into **ANIMA health indicators**. They are declared in the registry (`METRICS` + `health` in
`tools/gen-test-registry.mjs`), so they are **modular and scalable**: a new test that emits `cases`
(NL requests exercised) or `halluc` (fabrications/false positives) **feeds the dashboard on its own**.

Headline indicators:

| Indicator | Meaning | Target |
|---|---|---|
| **Hallucinations** | the sum of all fabrications/false positives on the adversarial traps | **0** |
| **NL cases covered** | how many natural-language requests ANIMA is verified to handle (the sum of all the NL suites) | the higher the better (today ~6500) |
| **Routing drift** | routing changes vs the golden snapshot | **0** |
| **Green tests** | passed / total (intentional skips excluded) | all |

Every complete run (GUI **▶ Esegui tutti** or `npm run test:all`) **records a snapshot** in
`tools/test-lab/history.jsonl`; the **Andamento** (Trend) tab and the CLI's `SALUTE ANIMA` (ANIMA health)
section use it for the trends. Adding a metric = one line in the generator's `METRICS` map.

## The natural-language part (the heart)

The NL tests run **only on the programmatic cascade** — the `anima.exe` compiled from the firmware, linked with
`anima_online_stub.c` (online unavailable): **no LLM/generative model in the tests**. A request
that would need the internet or a model **must abstain**, and we verify that it does.

Dedicated NL runners (all under `tools/anima-host/`), also runnable via `npm`:

| Runner | npm | What it proves |
|---|---|---|
| `halluc-probe.mjs` + `halluc-suite.mjs` | `anima:halluc` | **Strict** anti-fabrication: a confident reply via ANY tier (even `L0/command`, a leaked `{value}`) = a hallucination. 8 files / ~585 traps. |
| `metamorph.mjs` | `anima:meta` | **Metamorphic**: 485 seed sentences × 8 semantics-preserving transformations (typo/CAPS/accents/politeness/filler/code-switch/reordering) = ~2300 mutants; the abstention must be invariant. Honest cross-substrate (M1 full / M3 detection / M4 verifier). |
| `boundary.mjs` | `anima:boundary` | **Skill↔knowledge boundary** in 3 directions: a *definition* never computes; a *compute* always fires; *knowledge with a bait* does not fire a skill. 473 cases. |
| `realistic.mjs` | `anima:realistic` | ~310 **real requests** (many with QWERTY typos): under-tested skills fire, knowledge stays grounded or abstains; safe degradation on garbled input. |
| `skill-probe.mjs` | (various `eval_skills*`) | Curated cross-skill routing: the right request reaches the right tool. |
| `nl-stress.mjs` / `describe-stress.mjs` / `ood-check.mjs` | | NL stress at volume, descriptions, out-of-scope: 0 hallucinations. |

## Determinism and validity

- **No unseeded RNG, no wall-clock** in the tests (the `date` skill is excluded from the generators to
  avoid coupling to the clock). Same input → same outcome, on every rerun.
- **Clean SD state before EVERY gate** (`clearVolatile()` in `gate.mjs`): taught units, profile, learn
  and events persist on SD; cleaning them only at startup let one gate pollute the next.
- **Expectations are grounded in the real behaviour** of the exe, not guessed: for a new sentence you
  run the real exe and pin the outcome; for a hallucination the expectation is fixed (abstention) and if it does not
  abstain it is a bug to fix at the root.

## Adding a test

1. Write the runner or the `*.test.mjs` file, or add cases to an existing `eval_*.jsonl`.
2. If it is a new ANIMA runner that must become mandatory, add it to the `gates` array in
   `tools/anima-host/gate.mjs`.
3. Regenerate the catalog: `npm run test:registry` (it picks up every `*.test.mjs` automatically).
4. Verify it is green: `npm run anima:gate` (canonical) or the GUI `npm run test:lab`.

## Status

Catalog: **147 tests** in 17 categories (64 of them natural-language) — regenerated by `npm run test:registry`,
the count is always live with `npm run test:list`. All green with one click (`npm run test:lab` → **▶ Esegui
tutti**, or `npm run test:all`); the only skips bow out on their own without a network/SD/device setup
(`forge-download-manager`, the on-device arbiter load) — they are not failures. The canonical gate stays
green with `npm run anima:gate`.
