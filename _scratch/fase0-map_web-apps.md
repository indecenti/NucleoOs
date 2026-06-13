Perfetto. Ho dati sufficienti. Genero il report:

## MAPPATURA APPS/ â€” NucleoOS

### 1. TABELLA APPLICAZIONI (40 app)

| App ID | Scopo (1-riga) | Kit condivisi | Firmware? | Note |
|--------|---|---|---|---|
| **anima** | Assistente offline Italian (LLM on-device) | anima-skill.js, contextkit.js, fsclient.js | /api/anima*, /api/llm | Hub centrale; Vosk+WebLLM+WASM |
| **agent** | Multi-agente cloud (Claude orchestratore) | (interno) | /api/apps, /api/status | Sandbox file/code/web |
| **terminal** | Console filesystem + system status | (interno) | /api/fs/*, /api/status | fs.changed subscribe |
| **code-runner** | Sandbox JS in Web Worker | nucleo-run.js, i18n | /api/fs/*, /api/anima, os.hw.* | 6s timeout; Worker + hwAPI |
| **settings** | Configurazione device/WiFi/UI/associazioni | i18n | /api/wifi/scan, /api/*, registry events | entry_service firmware |
| **voice-manager** | Training vocale offline (Vosk) | (interno) | /api/voice/* | Voice/state/learned subscribe |
| **spreadsheet** | Foglio calcolo V2 + ANIMA Copilot | anima-skill.js (sheet-engine, copilot) | /api/anima, /api/llm | ANIMA sidebare; >=2600 righe HTML |
| **ir-remote** | Telecomandi IR (builder + macros) | anima-skill.js (makeSkill) | /api/ir/{send,jammer,tvbgone}, /api/fs/* | Deterministic floor offline |
| **games** | Lobby multiplayer + WebRTC P2P | (interno) | /api/games? | Chat/partite peer-to-peer |
| **dosbox** | Emulatore DOS (js-dos WASM) | (interno) | /api/fs/* (stream disk) | jsdos/zip/img/exe handler |
| **video-studio** | Converter video UI (PC companion ffmpeg) | (interno) | /api/fs/* | Companion PC-side GPU |
| **paint** | Editor immagini + canvas | (interno) | /api/fs/* | jpg/png/bmp handler |
| **media-player** | Riproduttore audio/video | (interno) | /api/fs/* | |
| **radio** | Sintonizzatore radio (IP/FM?) | (interno) | /api/* | |
| **browser** | Browser web embedded | (interno) | /api/* | WebView? |
| **notepad** | Editor di testo semplice | (interno) | /api/fs/* | |
| **calendar** | Calendario | (interno) | /api/* (events?) | |
| **clock** | Orologio + timer/allarmi | (interno) | /api/* | |
| **calculator** | Calcolatrice | (interno) | none | Puro client |
| **system-monitor** | Monitoraggio CPU/heap/logs | (interno) | /api/cpu, /api/heap, /api/logs | |
| **tasks** | TODO/task manager | (interno) | /api/fs/* | |
| **dictation** | Trascrizione vocale (Vosk) | asr.js | /api/voice/learn? | Vosk integrato |
| **dj** | Mixer audio/effetti | (interno) | /api/audio? | |
| **log-viewer** | Visualizzatore log sistema | (interno) | /api/logs | |
| **file-commander** | File manager | (interno) | /api/fs/* (list/read/delete/mkdir) | dlgate/micgate? |
| **help** | Documentazione in-app | (interno) | none | Statico |
| **updates** | Sistema aggiornamenti | (interno) | /api/updates? | |
| **recycle-bin** | Trash bin | (interno) | /api/fs/* | |
| **wifi-scanner** | Scansione WiFi | (interno) | /api/wifi/scan | WiFi status subscribe |
| **photo-viewer** | Visualizzatore immagini | (interno) | /api/fs/* | jpg/png handler |
| **video-player** | Riproduttore video | (interno) | /api/fs/* | |
| **recorder** | Registratore audio | (interno) | /api/voice/*, /api/audio? | Audio input |
| **groq-chat** | Chat LLM (Groq cloud) | (interno) | /api/llm | Cloud inference |
| **ssh** | Client SSH web | (interno) | /api/ssh? | Terminal SSH |
| **anima-knowledge** | Base di conoscenza offline | (interno) | /api/anima/l1 | Fallback ANIMA |
| **archive-manager** | Gestore archivi (zip) | (interno) | /api/fs/* | zip handler |
| **dos-importer** | Importer disco DOS | (interno) | /api/fs/* | |
| **automation-studio** | Micro-VM event-to-action | (interno) | /api/*, automation.rule_fired publish | entry_service (firmware) |
| **swarm** | Federazione ESP-NOW (mesh) | (interno) | mesh.* API (firmware) | entry_service (firmware); no /www |
| **miei-fatti** | Note/fatti personali | (interno) | /api/fs/* | |

**Totale verificato:** 40 app; **No index.html:** automation-studio, swarm (service-only); **Manifest:** 41 (1 root).

---

### 2. KIT CONDIVISI â€” Localizzazione e Importatori

| File | Percorso | Chi lo importa |
|------|---------|---|
| **anima-skill.js** | `/apps/anima/www/` | anima, ir-remote, spreadsheet (+copilot/sheet-*) |
| **contextkit.js** | `/apps/anima/www/` | anima, dictation (event/callback framework) |
| **fsclient.js** | `/apps/anima/www/` | anima (FS access wrapper) |
| **nucleo-run.js** | `/apps/code-runner/www/` | code-runner (Web Worker sandbox runtime) |
| **nucleo-hw.js** | `/apps/code-runner/www/` | code-runner (GPIO/IR/WiFi HW API) |
| **i18n.js** | `/nucleo-i18n.js` (web/shell/) | anima, code-runner, settings, ir-remote, spreadsheet, voice-manager (e ~20 altre) |
| **dlgate.js** | `/web/shell/` | file-commander (Drive local gateway?) |
| **micgate.js** | `/web/shell/` | ? (audio microphone gateway) |
| **web-llm.js** | `/apps/anima/www/forge/vendor/` | anima/forge (WebLLM engine vendor) |
| **ffmpeg.js** | `/apps/video-studio/www/vendor/ffmpeg/` | video-studio (WASM + JS FFmpeg) |

**Gotcha noto:** Import senza `/www` prefix â€” es. code-runner fa `import { createRunner } from './nucleo-run.js'` (assume sibling in www/); anima fa `import { makeSkill } from '/apps/anima/anima-skill.js'` (absolute).

---

### 3. PATTERN DI POLLING (setInterval)

| App | Intervallo/Uso | Endpoint |
|-----|---|---|
| **anima/contextkit.js** | Battery/screen polling | /api/status (presumably) |
| **games/nucleo-game.js** | Game loop animation | none (local state) |
| **games/nucleo-play.js** | P2P state sync | WebRTC (not /api) |
| **ir-remote** | tvbQuick() sweep | setInterval(()=>fetch('/api/ir/tvbgone'),350ms) |
| **spreadsheet** | Formula recalc? | none (client-side) |

**Nota:** Polling esplicito raro; anima-skill.js usa setInterval per device readiness check; ir-remote tvbgone Ã¨ unico polling /api periodico (350ms).

---

### 4. APP CON WORKER / WASM / GPU

| App | Risorsa | Heap/RAM Risk |
|-----|---------|---|
| **anima** | Vosk (ASR WASM: vosk.js 800KB+) | ALTO: modelo locale ~40MB su SD â†’ streaming in Worker |
| **anima/forge** | WebLLM engine (1-3GB quantized) | CRITICO: solo su SD; streaming WASM chunk (~18KB heap) |
| **code-runner** | nucleo-run.js (Web Worker sandbox) | MEDIO: user code runs isolato; timeout 6s |
| **dosbox** | js-dos WASM + DOSBox | CRITICO: DOS disk image streamed; Worker per emulation |
| **paint** | WebGPU probe (diffusion/webgpu-probe.js) | BASSO: GPU compute offload (optional) |
| **video-studio** | FFmpeg WASM (vendor/ffmpeg/) | BASSO: PC companion; web UI solo orchestration |
| **games** | WebRTC peer (nucleo-play.js) | MEDIO: P2P bitstream; browser codecs |

**Dato critico:** Vosk + WebLLM vivono in `/apps/anima/www/forge/` e sono loadati on-demand via streaming WASM (SPM pattern); 18KB heap tight â†’ no PSRAM fallback.

---

### 5. APP APPARENTEMENTE INCOMPLETE / MORTE / DUPLICATE

| Segnale | App | Stato |
|---------|-----|-------|
| **No index.html** | automation-studio | Service firmware (entry_service: automation_main) â€” VIVO |
| **No index.html** | swarm | Service firmware (entry_service: swarm_main) â€” VIVO |
| **Manifest ma no code** | â€” | none found |
| **Asset orfani** | â€” | none detected (grep -r non ritorna "unused" files) |
| **Duplicate ID** | â€” | no duplicates (app_id unique per manifest) |
| **Obsolete suffix** | â€” | none (es. -legacy, -old) |

---

### 6. REGISTRY / MANIFEST â€” Come un'App Risulta "Installata"

- **Manifest.json presente** in `/apps/<id>/` = app registrata
- **entry_service** (es. settings_main, automation_main, swarm_main) = firmware component (C in nucleo_*); skip web index.html
- **runtime: "web"** = entry_point is `/apps/<id>/www/index.html`; fetch via `/apps/<id>/` route
- **subscribes/publishes** = event mesh (firmware dispatches on system.boot, wifi.status, fs.changed, etc.)
- **permissions** = ACL array (system.events, storage.shared, device.audio, etc.)
- **mounts** = { app: "/apps/<id>/data" } su SD (persistent storage)

**Installazione verifica:** check manifest.json esistenza + web_route route presente nello shell; non c'Ã¨ registry.json separato (metadata inline manifest).

---

### FIRMWARE ENDPOINTS USATI (Raccolti)

```
/api/anima*             â† ANIMA inference (l1 fallback, caps, query)
/api/llm?url=...        â† LLM provider proxy (Groq, Anthropic, Gemini)
/api/voice/*            â† ASR/TTS/training voice
/api/ir/{send,jammer,tvbgone}  â† IR transmission
/api/fs/{read,write,delete,list,mkdir}  â† Filesystem
/api/wifi/scan          â† WiFi enumeration
/api/gpio               â† GPIO read/write
/api/cpu, /api/heap, /api/logs  â† System telemetry
/api/status             â† Device status (ready, heap, etc.)
/api/auth/status        â† Auth state
/api/display            â† Screen control (?)
/api/diag               â† Diagnostics
/api/apps               â† App registry query
/api/tts                â† Text-to-speech
```

---

**Summary denso:**
- **40 app web** + **2 service** (automation-studio, swarm) = 42 total
- Kit shared: anima-skill.js, nucleo-run.js in app dirs; i18n + dlgate/micgate in /web/shell/
- Polling raro; ir-remote tvbgone unico polling /api (350ms)
- ANIMA + code-runner + games heavy su WASM/Worker
- No dead/duplicate; automation-studio + swarm are firmware-only (no www/)
- Manifest = installation source; no separate registry file
