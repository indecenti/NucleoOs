Ora delivero il resoconto finale in italiano, denso e fattuale:

## MAPPATURA HARNESS HOST E GATE DI TEST â€” NucleoOS

### 1. package.json (root) â€” npm script principali

**Gate canonico + test offline:**
- `anima:gate` â†’ node tools/anima-host/gate.mjs â€” orchestratore centralizzato (65 gate + suite unit)
- `test` / `test:anima` / `test:all` / `test:nl` â†’ node tools/run-tests.mjs â€” CLI su test-registry.json (stesso catalogo della GUI)

**Test specifici ANIMA offline:**
- `ir:test`, `hw:test`, `skill:test` â€” intenti/hardware/skill routing via anima-host/
- `anima:halluc`, `anima:meta`, `anima:boundary`, `anima:realistic` â€” anti-allucinazione
- `anima:route`, `anima:reason`, `anima:local:*` â€” routing snapshot / ragionamento / local engine
- `anima:webindex`, `anima:packs`, `anima:prep-models/libs` â€” build knowledge/modelli offline

**Test app-specifici:**
- `spread:gate`, `games:gate` â€” spreadsheet e giochi
- `browser:check`, `logviewer:test`, `test:registry` â€” browser-compat / log viewer / rigenera catalogo

**Strumenti diagnostici / build:**
- `gen:api`, `gen:api:check` â€” spec Swagger/API
- `i18n:gate` â€” verifica traduzioni
- `anima:entity`, `anima:enrich` â€” entity extraction / Wikidata enrichment
- `push-ota`, `test:lab` (Python+Tkinter cockpit)

---

### 2. tools/ â€” sottodirectory e scopo (1-riga)

| Sottodirectory | Scopo |
|---|---|
| **anima-host** | Harness host MinGW: compila firmware C reale (anima.exe), REPL e batch query senza device/Wi-Fi/flash. ~105 test .mjs gate. |
| **anima** | Factory offline: compila knowledge (JSONL cards) â†’ encoder/index binari (AKB2 prefilter, holographic vettori), strumenti Python/Node per RAG. |
| **test-lab** | Cockpit GUI Tkinter: tre schede (Panoramica salute ANIMA, Esegui test, Andamento metriche), legge test-registry.json. |
| **arb-host** | Arbiter contention: host-compila firmware arbiter (mutex device TLS/SD/heap), test Win32 thread sotto contesa. |
| **eventbus-host** | Event protocol mock: test dell'architettura delta-sourced event bus, protocol validation. |
| **notify-host** | Test notification protocol: local + web push. |
| **voice-host** | Test cascata voice/TTS (offline clip planning + index SD). |
| **spread-copilot** | Copilot NLâ†”formula spreadsheet: NLU â†’ Excel combinator, test NL stressing. |
| **spread-nlu** | NLU meteo/calcolo: regressione su corpus meteo/skill routing. |
| **nucleo-tts** | TTS offline concatenativo IT/EN: planner clip (numero/date/decimali), index binario. |
| **nucleo-sd-deploy** | Python tool: configura SD card (partition, models, teacher.json, secrets). |
| **nucleo-ssh-bridge** | SSH bridge: proxy device-host su Wi-Fi/BLE/USB. |
| **sd-sim** | Simulatore SD: mock FATFS per test offline (no device). |
| **nfv** | NFV audio (v3 tag): embedding ID3, preview extraction. |
| **npx_gen** | Generatore snippet app-specific (config injection). |
| **sdxs** | (non documentato) |
| **experiments** | (non documentato) |

**File root tools/:**
- `validate.mjs` â€” verifica manifest app/registry assoc (API spec, icon paths)
- `gen-test-registry.mjs` â€” rigenera test-registry.json da gate.mjs + scan *.test.mjs
- `run-tests.mjs` â€” CLI runner catalogo: --nl / --anima / --cat <id> / --grep
- `gzip-assets.mjs` â€” pre-comprime .js/.css/.html per webfs firmware (3-4x heap)
- `push-ota.mjs` â€” push web-layer a device via /api/fs/write

---

### 3. anima:gate â€” struttura e coverage

**65 gate + suite unit (tools/anima-host/gate.mjs:62-369)**

Organizzati in 8 categorie NL + infrastruttura:

**NL Â· Anti-allucinazione (10 gate):** halluc-stress, halluc-battery, metamorph, nl-stress, reliability, false-positives (Ã—2), cross-topic, halluc-probe (IT/EN)

**NL Â· Routing skill (9 gate):** boundary, realistic, skill-routing (Ã—2), false-positives-2, cross-skill, action-tier, skill-isolation, image-gen-stress

**NL Â· Conoscenza & retrieval (10 gate):** l1-parity, l1-recall, describe-stress, fluency-grounded, regress, typed-nl, entity-detect, clean-extract, akb5-content, packed dict

**NL Â· Ragionamento & composizione (7 gate):** kge-eval, hdc-eval, combinator-eval, pcg-eval, typed-facets, evolution-check, contradiction-check

**NL Â· Matematica (2 gate):** math-check, math-dialog (Ã—2 IT/EN)

**NL Â· Memoria & profilo (3 gate):** teach-loop, profile, dict-sync

**NL Â· Traduzione (2 gate):** translate, dict-stress

**NL Â· Meteo:** (annegato in skill routing)

**Cascata Â· Infrastruttura (6 gate):** pack-coherence, route-check, agent-check, skill-check (app-skill), agent-helpers, arbiter

**Knowledge & Ledger (2 gate):** ledger-attack, kge-eval (auto-evolution Wikidata)

**ANIMA Forge/WebLLM (35 gate in suite offline-installer):** download ctrl / install flow / engine prereq / suspend-resume / verifica shards

**TTS + Device + App (3 gate):** tts-check, hw-manifest, unit-tests (*.test.mjs)

**Runner:** tools/anima-host/gate.mjs, invocato da `npm run anima:gate`, canonico pre-flight per flash.ps1/release.ps1 (exit 0 = tutti i gate passano).

**Hermetic:** clearVolatile() resetta SD prima di OGNI gate (units.txt, user.tsv, profile.tsv, events.tsv) â†’ determinismo, no cross-pollution.

---

### 4. Script PowerShell root (deploy/flash/release/gzip/sync guard)

| Script | Cosa fa | Guard / pre-flight |
|---|---|---|
| **flash.ps1** | Compila firmware C (idf build), genera nucleoos.bin, flasha via serial (espadool) a device. `-BuildOnly` salta il flash. | Richiede `npm run anima:gate` verde (exit 0). Controlla spazio bin vs free space flash. `-SkipGate` bypassa gate (non consigliato). |
| **release.ps1** | Compila firmware + web-layer: anima.exe, gzip-assets, push OTA a device via /api/ota. | Richiede gate verde. Comprime shell.js, apps. Bumpa versione sw.js (cache buster). |
| **deploy.ps1** | Sincronizza deploy/sd/{www,apps,system} â†’ device (via Invoke-WebRequest /api/fs/write + mkdir). | Pre-flight: SD card presente, no /MIR (specchiamento). |
| **sd-sync.ps1** | Speculare deploy/sd â†’ H:\ (SD fisica) via Robocopy /MIR. | `-Target` specifica destinazione. Guard: verifica H: esiste (USB reader). |
| **gzip-assets.mjs** | Pre-comprime HTML/JS/CSS/JSON nei tree www/ e apps/ â†’ .gz siblings. | Smart: salta se .gz >= raw (no win). |

**Artefatti non referenziati â€” uso-e-getta candidati pulizia:**

- **fw-build-*.log** (60+ file): log build firmware vecchi (watchfix, radioheap, twin, wifi3, ecc.) â†’ **non usati da script, greppabili = cleanup OK**
- **bounce_test*.wav** (Ã—3): audio test codec â†’ **non citati, cleanup OK**
- **TEMP*.txt** (Ã—5: TEMPcomb, TEMPev, TEMPev2, TEMPhdc, TEMPkge): debug tmp â†’ **not grep'd, cleanup OK**
- **scratch_*.mjs** (3 file), **tmp_fix.js**: stubs test temporanei â†’ **not ref'd, cleanup OK**
- **deploy_fix.ps1, deploy_fixes.ps1**: vecchi script fix (hanno equivalenti in deploy_asr.ps1, deploy_fixes.ps1 coexist) â†’ **verificare dipendenze**

**Grep verifica:** nessun .mjs/.ps1/.py attivo referenzia questi nomi â†’ **candidati rimozione sicura**.

---

### 5. docs/ â€” indice 1-riga (4289 righe totali, 33 file)

| File | Contenuto |
|---|---|
| **anima.md** | Architettura ANIMA: layer L0 intenti â†’ L1 retrieval â†’ HDC â†’ facet â†’ online; RAG, zero generazione. |
| **anima-agent.md** | Agenti browser (ANIMA Code / Claude): tool validation, code execution, jailbreak defense. |
| **anima-atelier.md** | Image generation API (Paint): Diffusion engine, prompt enhancement, safety. |
| **anima-context-engine.md** | Context engine: multi-turn, named registers, temporal resolution. |
| **anima-cortex.md** | Cortex: vector/HDC/combinatoria layer, reasoning. |
| **anima-forge.md** | Forge offline installer: download mgmt, engine loader, SD model persistence. |
| **anima-knowledge-graph.md** | Knowledge graph: typed facets (isa/occupation/died), auto-evolution Wikidata, ledger immutabile. |
| **anima-knowledge-ollama.md** | Integration Ollama per local-run LLM models. |
| **anima-knowledge-scale.md** | Scaling: AKB5 sharded router, encyclopedia range (10k+ cards). |
| **anima-memory.md** | Memory budget: ~18KB heap runtime, sd-fragmentation, SD fat32 limits. |
| **anima-online.md** | Online tier: Wikipedia certain-source lookup, TTS streaming, LLM fallback. |
| **anima-roadmap.md** | Roadmap ANIMA: v2 ledger immutable, facet typati, scaling con AKB5. |
| **anima-web-knowledge.md** | Web knowledge pipeline: import Wikidata, entity linking, stored offline. |
| **app-manifest.md** | Format manifest app: route, permission, mount, energy. |
| **app-runtimes.md** | Runtime app: JS (Node-like), WASM (Emscripten), py2wasm. |
| **architecture.md** | System layers: boot, services core, app runtime, SD storage, transport (Wi-Fi/BLE/USB), shell PWA. |
| **debugging.md** | Debug harness: anima-host MinGW, JTAG, QEMU, Wokwi, GDB. |
| **device-ui.md** | UI device: 240Ã—135, pairing QR, notifications, minimal console. |
| **event-protocol.md** | Event bus: append-only delta-sourced, ESP-NOW swarm federation. |
| **i18n.md** | Internazionalizzazione: IT/EN card, dictionaries, facet localization. |
| **media.md** | Media: codec audio (opus), opus + PCM, FFmpeg pipeline. |
| **memory-budget.md** | Budget RAM: 18KB heap + 2KB stack, SD fragmentation, OOM recovery. |
| **notify-protocol.md** | Notification protocol: system/app push, local + web. |
| **partition-table.md** | Partition: factory, ota_0/ota_1, app, nvs, spiffs, fat32. |
| **registry.md** | Registry: app manifest, route handler, permission schema. |
| **releasing.md** | Release workflow: firmware OTA (rollback-safe) + web-layer SD sync, versioning. |
| **roadmap.md** | Roadmap system: file API, auth/pairing, RTC/NTP, notifications, event journal, OTA. |
| **security.md** | Security: bundle signing, pairing, capability enforcement, code injection defense. |
| **setup-wizard.md** | Setup: Wi-Fi, pairing PIN, account link, time. |
| **storage.md** | Storage: microSD layout, fatfs config, fragmentation mitigation. |
| **testing.md** | Testing (dettagliato): 99 test in 16 categorie, determinismo, GUI+CLI+gate, metrics aggregation. |
| **tts.md** | TTS offline concatenativo: IT/EN clip planner, index binario packed. |
| **voice.md** | Voice: mic + ADC, codec opus, Vosk ASR (online), TTS (offline). |

---

**Sintesi harness & gate:**

- **Harness host:** `anima.exe` (anima-host/build/) â€” vera cascata firmware C no flash, REPL, batch query
- **Gate canonico:** 65 test harness + 35 forge + unit suite, orchestrati da gate.mjs, pre-flight per flash/release
- **Test registry:** unica fonte (`test-registry.json`), autogenera da gate + scan *.test.mjs, legge GUI/CLI/gate
- **Determinismo:** clearVolatile() pre-gate, no RNG, stato SD pulito, snapshot golden route-check
- **Artefatti usa-e-getta:** fw-build-*.log, bounce_test.wav, TEMP*.txt, scratch_*, tmp_fix.js â†’ candidati cleanup (no ref grep)
