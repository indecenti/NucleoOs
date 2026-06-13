Perfetto, ho tutti i dati. Compongo il rapporto finale in italiano, denso e fattuale.

## Mapping web/shell â€” NucleoOS

### 1. Scopi file JS/CSS principali

- **shell.js (139KB)**: Controller OS â€” boot, app launcher, WM orchestration, clipboard, keyboard shortcuts, file associations, drag-drop desktop, taskbar/Start, status polling (/api/status ogni 15s, /api/apps su cambio SD), WebSocket live events, session restore, installer scrim coordination.
- **wm.js (20KB)**: Window manager minimale â€” open/focus/drag/minimize/maximize/close app iframe, snap-layout Windows11, Z-stacking, persisted geometry per app, pin-to-top (per modal), click-to-front su iframe.
- **copilot.js (18KB)**: ANIMA come servizio OS â€” floating AI bar (Ctrl+Space), chat con /api/anima on-device, traduce azioni JSON in effetti shell (WM.open, openFile, showToast), modalitÃ  offline/ibrida/online, sincro lingue con ANIMA app via localStorage.
- **ai.js (17KB)**: Client cloud multip-provider â€” Anthropic/Groq/xAI/Gemini (browser-direct o relay via /api/llm), registry provider + capability matrix, tier mapper (max/mid/fast), Gemini plan/tier sniffing, key vault legge /data/anima/teacher.json.
- **nucleo-i18n.js (18KB)**: Runtime i18n centralizzato â€” una lingua OS-wide, cataloghi per ns (core/shell/app-id), lazy-load .json da /i18n/, fall-back itâ†’key, storage event live sync cross-iframe, apply() per [data-i18n].
- **notify.js (16KB)**: Notification Center web â€” accetta emit() in-process o eventos WebSocket (topic notify.post, legacy calendar.reminder), earcon firma per source (anima/calendar/ota/recorder/voice/app/system), chord per livello (info/success/warn/critical), DND + quiet hours, storico localStorage (cap 50).
- **micgate.js (13KB)**: Web Locks OS-wide mic â€” esclusivitÃ  /dev/I2S PDM, fallback promise-chain, BroadcastChannel cross-tab, stream + record-to-SD, re-anchor su 5min cap, withMicLock(label, fn, {wait?}), backup /api/rec/status per recorder nativo.
- **dlgate.js (3.6KB)**: Web Locks OS-wide download â€” serializza multi-MB pull (brain/voice/weights), label broadcast, FIFO QUEUE (default) o ifAvailable skip, fallback per tab senza Locks.
- **fsindex.js (7.3KB)**: Search index RAM â€” crawl BFS /data (max depth 6, 600 dir cap), persist localStorage, refresh debounced (1.5s) su fs.changed, categorizza estensioni (image/audio/video/doc/code/game).
- **busy.js (1.8KB)**: Busy indicator controller â€” debounce 600ms su system.busy event, show() immediato, hide() deferred â†’ evita flicker burst short jobs.
- **onboarding.js (17KB)**: First-boot wizard â€” linguaâ†’benvenutoâ†’installa PWAâ†’provider pickerâ†’key setup (live test, browser-direct), persiste nucleo.onboarded, repeat-proof via /api/anima/caps.
- **shortcuts.js (3.4KB)**: Pure keyboard resolution â€” doc (Ctrl+S/N/O), edit (Ctrl+C/X/V/A/Z/Y, F2, Del, F5), Escape priority (taskSwitcherâ†’dialogâ†’copilotâ†’actionCenterâ†’startâ†’contextMenuâ†’app).
- **sw.js (8.1KB)**: Service Worker offline â€” cache shell v75 (asset precache tollerante 404), Forge model cache (offline model weights), gatedFetch gate (MAX_INFLIGHT 3, esclusiva per /api/fs/write), immagini /api/fs/read cache-first.
- **style.css (43KB) + copilot.css (7.4KB) + notify.css (4.4KB) + onboarding.css (4.3KB)**: Layout desktop/WM, tray, copilot dialog, notification toast, wizard card.

### 2. sw.js: cache + concurrency

- **Versione**: `const CACHE = 'nucleo-shell-v75'` (riga 3, v75 = /data/Desktop mirroring + WM click-to-front fix + protected-target).
- **Strategia**: Cache-first (asset sw ASSETS); network-first /api/fs/read immagini (riga 106); network /api/fs/write (riga 122, mai cache); live /api/chat, /api/logs, /api/llm (riga 127, no gate, no cache).
- **MAX_INFLIGHT = 3** (riga 19): Semaforo shared-read / exclusive-write, timeout 15s per write (riga 49), queue FIFO head-only (riga 25), retry GETs su transient OOM (riga 40).
- **Precache ASSETS** (riga 4): index.html, *.{js,css}, manifest, i18n/*.json, wallpaper.png, tollerante 404 per install resiliente.

### 3. shell.js: sottosistemi + polling

**Struttura 139KB**: boot â†’ pairing â†’ apps load â†’ UI state (pins/wallpaper/desktop) â†’ window geometry restore â†’ WM init â†’ clipboard load â†’ keyboard shortcuts â†’ 6 lazy-imports (copilot, notify, FsIndex), WebSocket subscribe.

**Polling**:
- **tickClock()** ogni 10s (riga 546): HH:MM + data locale (no network).
- **refreshStatus()** ogni 15s (riga 547): /api/status GET â†’ storage free, network info (SSID/IP/AP), repaint tray + action-center.
- **fs.changed event** (riga 600): WebSocket â†’ refreshStatus() + FsIndex rebuild (debounce 1.5s).
- **system.busy event** (riga 627): WebSocket â†’ busyCtl.onEvent() debounce 600ms.
- **Manual**: /api/apps GET su "primo boot con niente" o "SD fs.changed" (riga 559 refreshApps).

**Endpoint device**:
- **/api/auth/status**: pairing check (timeout 4s, riga 441).
- **/api/fs/read**: SESSION, clipboard, UI state, app icon (riga 146, 176, 363, 41).
- **/api/fs/write**: session debounce 500ms, clipboard debounce 400ms, UI state debounce 400ms (riga 140, 183, 382).
- **/api/apps**: lista app + abilitati (riga 561).
- **/api/status**: storage/network/remote (riga 2370).
- **WebSocket /ws**: subscribe + sync (riga 589-630), eventi all'app window via postMessage (riga 628).

### 4. Cross-iframe postMessage

**Shell â†’ App** (riga 628, 199):
- **Event broadcast** (system.busy, fs.changed, storage.*, etc.): ogni window app riceve `ev` con `{t, d}` (riga 628).
- **Shortcut forward**: active app riceve `{type:'os-shortcut', action, ...extra}` su Ctrl+S/N/O/etc (riga 199).

**App â†’ Shell** (riga 683, wireMessages):
- **set-theme**: dark/light tema globale.
- **clipboard-write**/**clipboard-read**: app â†’ OS clipboard (clip.items boundato cap 20).
- **launch-app**: app id â†’ WM.open().
- **install-block-state**: installer chiede full-screen scrim, shell pinza window sopra (riga 639-680).
- **os-file-dialog-result**: File Commander â†’ file dialog callback via requestId (riga 2509).
- **fc-drag-payload**: File Commander drag â†’ desktop postMessage {items:[{path,name,isDir}]}, shell drop handler (Ctrl=copy, Shift=move, Alt=shortcut) (riga 1155).
- **set-language**: apps e i18n â†’ window.postMessage({type:'set-language', lang}) â†’ shell scrive /system/config/settings.json (riga 238-239).

### 5. AI provider registration + firmware talk

**ai.js PROVIDERS**:
- `anthropic`: sk-ant-*, /v1/messages, x-api-key + anthropic-version, browser-direct.
- `openai` (Groq): gsk_* o sk-*, /openai/v1/chat/completions, Bearer, browser-direct.
- `xai`: xai-*, /v1/chat/completions Bearer, browser-direct (supporta image generation).
- `google` (Gemini): AIza*, /v1beta/openai (CORS-blocked) â†’ relay via **/api/llm proxy** (firmware dials server-side).

**copilot.js** riga 14-23: legge /data/anima/teacher.json (readTeacher via AI), chiama **/api/anima** GET (device ANIMA engine, on-device), riceve JSON contract `{action,tool,arg,content,reply,intent}`, esegue in-process via WM/openFile/showToast.

**Gemini tier sniff** (riga 103): Pro-model test via /api/llm relay â†’ infer paid/free (riga 75-86).

### 6. Web Locks: micgate.js / dlgate.js

**micgate.js**: 
- Lock name `'nucleo-mic'`, mode `exclusive`.
- **withMicLock(label, fn, {wait?, skipValue?})**: default ifAvailable fail-fast, {wait:true} FIFO queue.
- BroadcastChannel `'nucleo-mic'` broadcast holder label cross-tab.
- **streamMic()**: tiene lock per intera sessione live PCM (16-bit, 16kHz, gain Ã—2.5), riassembla odd-byte tails, reconnect su 5min cap.
- Fallback non-Locks: promise-chain per tab, busy flag (riga 60-66).

**dlgate.js**:
- Lock name `'nucleo-dl'`, mode `exclusive`.
- **withDownloadLock(label, fn, {ifAvailable?})**: default QUEUE (FIFO), {ifAvailable:true} fail-fast.
- BroadcastChannel `'nucleo-dl'` broadcast active label cross-tab.
- Fallback: promise-chain per tab.

### 7. Asset .gz: shadowing + caching

**Gemelli .gz** (sw.js):
- `web/shell/`: ai.js, busy.js, copilot.js/css, dlgate.js, fsindex.js, index.html, manifest, micgate.js, notify.js/css, nucleo-i18n.js, onboarding.js/css, shell.js, shortcuts.js, style.css (no entry per questi, .gz Ã¨ pre-built serve HTTP Content-Encoding).
- **Gotcha**: sw.js NON nomina .gz in ASSETS (riga 4) â†’ HTTP serverserve .gz trasparente (Content-Encoding header), ma SW cache.match(request) matcha il path senza estensione. Se il file Ã¨ state aggiunto/rimosso, c'Ã¨ rischio di stale-serve. Mitigation: ogni v75 â†’ v76 incide CACHE string.
- **Rischio shadowing**: nessuno se HTTP/2 server manda Content-Encoding:gzip automatico; pericoloso SOLO se un asset Ã¨ renamed e lo .gz vecchio rimane su disk (HTTP 200 + stale gzip â‰  atteso).

**Nota**: i18n/*.json.gz aggiunti ma NO .gz entry nel SW ASSETS â†’ served via gatedFetch network con retry.

---

**Verifiche non eseguite** (READ-ONLY):
- Build actual shell.js byte count (wc confirm 139850, âœ“).
- Find ogni `fetch(` endpoint ed esatto path (sample key endpoints sopra, exhaustive grep possibile).
- Tracciare FULL app launch sequence WM.open(route) (80+ righe, non sintetizzabile).

Rapporto completo.
