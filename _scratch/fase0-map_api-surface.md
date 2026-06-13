## SUPERFICIE API HTTP/WS NUCLEO FIRMWARE

### 1. ROUTE HTTP REGISTRATE (firmware/components/nucleo_httpd/nucleo_httpd.c:1528-1585)

| Metodo | Path | Handler | File:Riga | Auth | CORS |
|--------|------|---------|----------|------|------|
| GET | /api/status | status_get | nucleo_httpd.c:146 | NO | "*" |
| GET | /api/apps | apps_get | nucleo_httpd.c:989 | NO | NO |
| GET | /api/associations | assoc_get | nucleo_httpd.c:1016 | NO | NO |
| GET | /api/logs | logs_get | nucleo_httpd.c:301 | NO | NO |
| GET | /api/diag | diag_get | nucleo_httpd.c:342 | NO | NO |
| GET | /api/heap | heap_get | nucleo_httpd.c:253 | NO | NO |
| GET | /api/cpu | cpu_get | nucleo_httpd.c:114 | NO | NO |
| GET | /api/wifi/scan | wifi_scan_get | nucleo_httpd.c:276 | NO | NO |
| POST | /api/ota | ota_post | nucleo_httpd.c:450 | YES (NUCLEO_AUTH_GUARD) | NO |
| POST | /api/reboot | reboot_post | nucleo_httpd.c:494 | YES | NO |
| GET | /api/anima | anima_get | nucleo_httpd.c:643 | NO | NO |
| GET | /api/anima/verify | anima_verify_get | nucleo_httpd.c:958 | NO | NO |
| GET | /api/anima/caps | anima_caps_get | nucleo_httpd.c:1127 | NO | NO |
| POST | /api/anima/l1 | anima_l1_post | nucleo_httpd.c:1156 | NO | NO |
| GET/POST | /api/tts | tts_handler | nucleo_httpd.c:1189 | NO | NO |
| GET | /api/proxy | proxy_get | nucleo_httpd.c:1054 | NO | NO |
| GET/POST | /api/llm | llm_proxy | nucleo_httpd.c:1221 | NO | NO |
| GET | /api/transcribe | transcribe_get | nucleo_httpd.c:1350 | YES | "*" |
| POST | /api/voice/learn | voice_learn_post | nucleo_httpd.c:1406 | YES | NO |
| POST | /api/voice/always | voice_always_post | nucleo_httpd.c:1436 | YES | NO |
| POST | /api/time/set | time_set_post | nucleo_httpd.c:1471 | NO | "*" |
| GET/POST | /api/fs/* | nucleo_fsapi_register | nucleo_fsapi.c:419 | Alcuni endpoint | NO |
| POST | /api/rec/{start,stop,mkdir} | nucleo_recorder_register | nucleo_recorder.c:323 | NO | NO |
| GET | /api/rec/status | status_get | nucleo_recorder.c | NO | NO |
| GET | /api/rec/stream | stream_get | nucleo_recorder.c | NO | NO |
| GET/POST | /api/ir/* | nucleo_ir_register | nucleo_ir.c:355 | NO | NO |
| GET/POST | /api/gpio | nucleo_gpio_register | nucleo_gpio.c:80 | NO | NO |
| GET/POST | /api/display | nucleo_app_register_display | nucleo_httpd.c:1582 (external) | NO | NO |
| POST | /api/pair | nucleo_auth_register | nucleo_httpd.c:1574 | NO | NO |
| GET | /api/auth/status | nucleo_auth_register | nucleo_httpd.c:1574 | NO | NO |

### 2. API /api/fs/* (nucleo_fsapi.c:419-432)

**Route registrate:**
- GET /api/fs/list?path=... â†’ list_get | lettura directory
- GET /api/fs/read?path=... â†’ read_get | lettura file
- POST /api/fs/write?path=... â†’ write_post | scrittura file (streaming, max 640 MB)
- POST /api/fs/delete?path=... â†’ delete_post | eliminazione
- POST /api/fs/mkdir?path=... â†’ mkdir_post | creazione directory
- POST /api/fs/move?from=...&to=... â†’ move_post | spostamento/rename

**Protezione filesystem (nucleo_fsprotect.h:55-113):**
- Paths protetti: `/system` (escluso state: config, keys, sessions, log), `/apps`, `/www`, `/data/anima/{akb5,anima-*.bin,dict-*,commands*}`
- Allowed: OVERWRITE in-place via /api/fs/write; DELETE/MOVE bloccati per protected
- /data liberamente deletabile (media, recordings, downloads, config, backups)
- Protezione applicata in: nucleo_fsapi.c:114, 348, 393

### 3. WebSocket (nucleo_ws.c:155-162)

**Endpoint:** GET /ws (upgrade HTTP â†’ WS)
- **Single-client policy:** MAX_CLIENTS=1; last-wins eviction (nucleo_ws.c:10, 36-50)
- **Protocol:** `{"op":"subscribe","since":N}` â†’ sync risposta buffered delta + live on_event
- **on_event structure** (nucleo_ws.c:112-117): `{"t":"<topic>","seq":N,"d":<payload>}`
- **Event payload pool:** 3 slot fissi, WS_MSG_MAX=320 B; pool-full â†’ drop delta, client resync
- **Auth:** NUCLEO_AUTH_GUARD su handshake (nucleo_ws.c:122)

### 4. Sandbox os.* per JS app (nucleo-run.js + nucleo-hw.js)

**Primitive esposti via RPC al worker:**

| Namespace | Azione | Endpoint | Auth | GPIO allowlist |
|-----------|--------|----------|------|-----------------|
| os.fs | read, write, append, list, exists, mkdir, remove | /api/fs/* | NO | N/A |
| os.http | get, json | /api/proxy (per HTTPS) | NO | N/A |
| os.anima | query | /api/anima | NO | N/A |
| os.notify | toast | postMessage â†’ shell | NO | N/A |
| os.hw.ir | send, tvbgone, jammer | /api/ir/* | NO | N/A |
| os.hw.wifi | scan | /api/wifi/scan | NO | N/A |
| os.hw.gpio | read (pins 0,1,2), write (pins 1,2) | /api/gpio | NO | GPIO_READ=[0,1,2], GPIO_WRITE=[1,2] |
| os.hw.command | NLâ†’capability | offline grammar | NO | IR, GPIO, WiFi |

**Isolation:** Worker = no fetch/XMLHttpRequest/WebSocket ambientali; RPC-only via postMessage; timeout wall-clock enforce (nucleo-run.js:30-100, nucleo-hw.js:1-79)

### 5. /api/proxy & /api/llm (SSRF + streaming)

**GET /api/proxy?url=<encoded https/http URL>**
- Filtra: url_target_blocked() (nucleo_httpd.c:1068)
- Streaming: proxy_evt chunk-by-chunk
- TLS heap guard: l1_unload se tight, refuse 503 se still OOM (nucleo_httpd.c:1073-1081)
- ARB budget: try-only, mai block httpd (nucleo_httpd.c:1099)

**GET/POST /api/llm?url=<HTTPS OpenAI|Anthropic endpoint>**
- TLS-only (nucleo_httpd.c:1231)
- Headers passate: Authorization, x-api-key, anthropic-version, anthropic-dangerous-direct-browser-access
- POST body streaming (1 MB sanity cap, non RAM-bound) (nucleo_httpd.c:1254-1256)
- Same heap/ARB discipline come /api/proxy (nucleo_httpd.c:1258-1281)

### 6. /api/anima/* (parametri & semantica)

**GET /api/anima?q=<query>&lang=it|en&mode=off|on|only&reset=1**
- Parametri query: q, lang (default it), mode (0=default, 1=offline-only, 2=hybrid, 3=online-only), reset (1=session reset)
- Cascata offline L1 + online (Groq/Anthropic) con lock spine (nucleo_anima_try_lock) (nucleo_httpd.c:702-721)
- System values: {time,storage,date,year,season,agenda,capabilities,network,ram,version,uptime}
- Tools: create_file, add_event (pairing gate) (nucleo_httpd.c:840-881)
- Risposta: {query, tier, action, reply, tool, confidence, domain, intent, lang, budget, memory, state, awaiting, corrected, trace} (nucleo_httpd.c:892-927)

**GET /api/anima/verify?kind=numeric|fact&key=...&asserted=...&lang=it**
- Grounded verification: JSâ†’device brain via (nucleo_httpd.c:951-957)

**GET /api/anima/caps** â†’ {hasKey, online, enabled, provider, model} (no key exposed)

**POST /api/anima/l1** â†’ offline L1 brain policy direct (nucleo_httpd.c:1156)

### 7. DRIFT SOSPETTI (endpoint dichiarati web â‰  firmware)

**Endpoint firmware registrati ma ZERO uso web/apps:**
- /api/cpu, /api/heap, /api/diag, /api/logs (diagnostica, non UI consumer)
- /api/ota, /api/proxy, /api/llm (potenzialmente usabili da app custom/agent, non web shell)
- /api/anima/verify, /api/anima/l1 (Forge groundedness, non shell)
- /api/tts (endpoint lato firmware, ma nessun fetch lato web)
- /api/ir/{send,tvbgone,db,jammer} (registrate, usabili via os.hw.ir, non fetch diretto)
- /api/gpio (registrata, usabile via os.hw.gpio, non fetch diretto)
- /api/display (extern register, app-facing, non web shell)

**Endpoint web usati:**
- /api/voice/learn (apps/voice-manager, nucleo_httpd.c:1406)
- /api/rec/stream (micgate.js, nucleo_recorder.c)
- /api/rec/status, /api/rec/start, /api/rec/stop (registrati, usati)

**Nessun endpoint fetch() lato web orfano** (tutti su firmware registrati). Drift asimmetrico: firmware espone piÃ¹ di quanto web shell consumi; le app + agent + Forge usano il resto.

### File sorgente chiave

- HTTP route registration: `firmware/components/nucleo_httpd/nucleo_httpd.c:1527-1585`
- /api/fs protezione: `firmware/components/nucleo_board/include/nucleo_fsprotect.h:55-113`
- /api/fs endpoints: `firmware/components/nucleo_fsapi/nucleo_fsapi.c:419-432`
- /ws single-client: `firmware/components/nucleo_ws/nucleo_ws.c:10, 36-162`
- /api/anima cascade: `firmware/components/nucleo_httpd/nucleo_httpd.c:643-949`
- /api/proxy+llm streaming: `firmware/components/nucleo_httpd/nucleo_httpd.c:1054-1342`
- Sandbox hw manifest: `apps/code-runner/www/nucleo-hw.js:27-79`
- Sandbox runtime: `apps/code-runner/www/nucleo-run.js:30-100`
- Web shell fetch calls: `web/shell/shell.js, ai.js, copilot.js, micgate.js`
