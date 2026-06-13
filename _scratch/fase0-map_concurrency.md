Perfetto. Ora genero il report finale completo:

---

## MAPPA RISORSE A PROPRIETARIO-UNICO E MECCANISMI DI CONCORRENZA

### 1. NUCLEO_ARB (Arbiter TLS one-at-a-time)

**API e Acquisitori:**
- firmware/components/nucleo_arb/include/nucleo_arb.h:60-84 â€” nucleo_arb_init/acquire/release/should_yield/busy/snapshot
- firmware/components/nucleo_arb/nucleo_arb.c:105+ â€” acquirenti: 
  - firmware/components/nucleo_anima/nucleo_anima_online.c â€” "anima-get", "anima-post", "anima-anthropic", "transcribe"
  - firmware/components/nucleo_httpd/nucleo_httpd.c:460 â€” "ota" (OTA firmware)
  - firmware/components/nucleo_httpd/nucleo_httpd.c â€” "proxy" (server-side fetch), "llm" (cloud LLM)

**Comportamento se busy:**
- Try-only (timeout=0): Ritorna 0, caller degrada a 503 + offline gracefully
- firmware/components/nucleo_httpd/nucleo_httpd.c:461 â€” Risposta 503 con Retry-After: 2
- Httpd task NEVER blocca â€” fail-fast garantito

**Mechanismo lock:**
- firmware/components/nucleo_arb/nucleo_arb_plat_esp.c:15-30 â€” xSemaphoreTake/xSemaphoreGive (FreeRTOS mutex)
- firmware/components/nucleo_arb/nucleo_arb.c:44,92,106,114 â€” arb_plat_lock() guardia su s_holder/s_cls/s_job/s_yield
- Polling-based waiter (no condvar): firmware/components/nucleo_arb/nucleo_arb.c:84-85 â€” sleep 2ms retry loop
- FG preempts BG: s_yield flag firmware/components/nucleo_arb/nucleo_arb.c:73

---

### 2. MICGATE.JS + FIRMWARE MIC GATE

**Chi puÃ² prendere il mic (mutually exclusive):**
- Web apps via withMicLock('label', fn) â€” web/shell/micgate.js:51-67 â€” lock name "nucleo-mic"
- Native on-device Recorder app â€” holds s_streaming atomic (firmware/components/nucleo_recorder/nucleo_recorder.c:28)
- Native voice PTT â€” firmware/components/nucleo_voice/nucleo_voice.c (usa I2S, non esplicitamente guarded via atomic)
- Web dictation stream â€” /api/rec/stream â€” firmware/components/nucleo_recorder/nucleo_recorder.c:303 â€” atomic_exchange(&s_streaming, true)
- Web record-to-SD â€” /api/rec/start â€” firmware/components/nucleo_recorder/nucleo_recorder.c:163 â€” atomic_load(&s_recording)

**Guard mechanism:**
- **Web layer**: Web Locks (browser API) â€” navigator.locks.request(LOCK, {mode:'exclusive'}) â€” web/shell/micgate.js:56-57
- **Fallback** (no Web Locks): Promise chain (_chain) + _busy flag â€” web/shell/micgate.js:61-66
- **Firmware layer**: atomic_bool atomics:
  - firmware/components/nucleo_recorder/nucleo_recorder.c:27-28 â€” s_recording, s_streaming (atomic_bool, no mutex)
  - firmware/components/nucleo_recorder/nucleo_recorder.c:163 â€” check s_recording || s_task || atomic_exchange(&s_streaming, true)
  - 409 Conflict response on double-open â€” firmware/components/nucleo_recorder/nucleo_recorder.c:200-205, 303-305

**Fail mode:**
- Web app: micBusy() returns {busy:true, label} + onerror('busy', label) â€” web/shell/micgate.js:75-83
- /api/rec/status.recording: 'Registrazione' label â€” firmware/components/nucleo_recorder/nucleo_recorder.c:240+
- Native Recorder: 409 Conflict if web holds lock (but no web lock seen by firmware)
- Fast-fail check for native Recorder: web/shell/micgate.js:122-125 â€” fetch /api/rec/status.recording before retrying

---

### 3. DLGATE.JS + OTA DOWNLOAD GATE FIRMWARE

**Mechanismo:**
- **Web layer**: Web Locks â€” navigator.locks.request('nucleo-dl', {exclusive}) â€” web/shell/dlgate.js:42
- **Firmware layer**: nucleo_arb_acquire(ARB_FG, "ota", 0) â€” firmware/components/nucleo_httpd/nucleo_httpd.c:460
- OTA Ã¨ ARB_FG (foreground), try-only, 503 if arbitrer busy

**Chi lo rispetta:**
- OTA updater: firmware/components/nucleo_httpd/nucleo_httpd.c:450+ ota_post()
- ANIMA Forge downloads: web/shell/dlgate.js (apps/anima/www/local/engine.js:_gate = withDownloadLock)
- Rispetto: Caller avvia download dentro withDownloadLock(..., () => fetch/download)

**Fallback assenza Web Locks**: Promise chain + _busy flag â€” web/shell/dlgate.js:45-51

---

### 4. GPIO0 / TASTO GO: NUCLEO_VOICE_PTT SINGLE-OWNER

**Single owner**: 
- firmware/components/nucleo_voice/nucleo_voice.c:127 â€” s_ptt_on volatile bool (set by UI loop ONLY)
- firmware/components/nucleo_voice/nucleo_voice.h:30-33 â€” nucleo_voice_ptt(bool on) â€” driven by "main UI loop (the single owner of the button timing)"
- firmware/components/nucleo_voice/nucleo_voice.c:848-851 â€” nucleo_voice_ptt() -> s_ptt_on = true/false

**Chi altro tocca GPIO0:**
- firmware/components/nucleo_gpio/nucleo_gpio.c:19-23 â€” READ_OK allowlist include GPIO0 (BtnA/G0), WRITE forbidden
- Web /api/gpio?pin=0: read-only, allowed â€” firmware/components/nucleo_gpio/nucleo_gpio.c:72-77
- Voice engine legge s_ptt_on polling nel task â€” firmware/components/nucleo_voice/nucleo_voice.c:554

**Mutua esclusione PTT vs voice/audio:**
- PTT holds VH_PTT holder bit â€” firmware/components/nucleo_voice/nucleo_voice.c:98, 850-851
- Engine self-releases su PTT release â€” firmware/components/nucleo_voice/nucleo_voice.c:851
- Lazy create/destroy â€” firmware/components/nucleo_voice/nucleo_voice.c:699+ voice_recompute()

---

### 5. MUTEX/SEMAFORI FIRMWARE (xSemaphore, portMUX, atomic)

| Risorsa | File:Riga | Chi la usa | Tipo Lock |
|---------|-----------|-----------|----------|
| nucleo_arb token (s_holder) | nucleo_arb_plat_esp.c:15-30 | Tutti acquire/release | xSemaphoreMutex |
| voice s_life_mux | nucleo_voice.c:105 | voice_hold_set/voice_recompute | portMUX (spinlock) |
| voice s_live_mux | nucleo_voice.c:129 | s_live_sentence R/W | portMUX (spinlock) |
| voice s_intro_mux | nucleo_voice.c:134 | s_last_match/s_last_result introspection | portMUX (spinlock) |
| recorder s_recording | nucleo_recorder.c:27 | atomic_load/atomic_store | atomic_bool (no explicit lock) |
| recorder s_streaming | nucleo_recorder.c:28 | atomic_exchange | atomic_bool (no explicit lock) |

---

### 6. STATI CONDIVISI SENZA LOCK EVIDENTE (race condition candidati)

| Stato | File:Riga | Task writer | Task reader | Rischio |
|-------|-----------|-------------|-----------|---------|
| s_level (meter) | nucleo_recorder.c:32 | record_task (core?) | web /api/rec/status httpd | R/W unguarded |
| s_path (filename) | nucleo_recorder.c:30 | record_task (core?) | web /api/rec/status, publish_rec | R/W unguarded |
| s_bytes (count) | nucleo_recorder.c:31 | record_task (core?) | web /api/rec/status | R/W unguarded |
| s_task (TaskHandle) | nucleo_recorder.c:29 | record_task self-null | check in start() | Racy se checked durante task exit |
| s_is_listening | nucleo_voice.c:126 | voice_task (core 1) | web (httpd task core 0) | No explicit lock visible |
| s_learning_mode | nucleo_voice.c:110 | voice_task | web arm_learning_mode | No lock guarding |
| s_win_label | nucleo_voice.c:123 | voice_task | introspection poll | No lock guarding |

**Mitigation:**
- recorder.c: s_recording/s_streaming atomic_bool proteggono la "doppia apertura" di basso livello
- voice.c: s_intro_mux guarda solo s_last_match/s_last_result, non s_is_listening/s_learning_mode

---

### 7. WEB LOCKS (Navigator.locks)

**Tutti gli usi:**
1. **"nucleo-mic"** â€” web/shell/micgate.js:22
   - streamMic() â€” web/shell/micgate.js:55-57 (ifAvailable: try-only)
   - recordToSd() â€” web/shell/micgate.js:200 (ifAvailable: try-only)
   - Broadcast channel sync cross-tab â€” web/shell/micgate.js:27-43

2. **"nucleo-dl"** â€” web/shell/dlgate.js:13
   - withDownloadLock(label, fn, opts) â€” web/shell/dlgate.js:42
   - Default: WAIT (FIFO queue) â€” web/shell/dlgate.js:42
   - ifAvailable:true â€” skip on busy â€” web/shell/dlgate.js:38-40

**Navigator.locks.query() usage:**
- micBusy() â€” web/shell/micgate.js:77-78 (check if any tab holds 'nucleo-mic')
- isDownloading() â€” web/shell/dlgate.js:57-59 (check if any tab holds 'nucleo-dl')

---

### 8. WEBFOCUS / SCREEN-OFF / L1 STAND-DOWN: SEQUENZA E TRIGGER

**Screen brightness (client-side dimming, firmware-agnostic):**
- firmware/components/nucleo_httpd/nucleo_httpd.c â€” NO esplicito screen-off handler
- web/shell/shell.js:990-1003 â€” applyBrightness(v) â€” CSS dim overlay, localStorage persist
- Ordine: User slider â†’ applyBrightness() â†’ dimEl.style.opacity â†’ localStorage "nucleo.brightness"

**L1 ANIMA unload (heap reclaim):**
- firmware/components/nucleo_httpd/nucleo_httpd.c:1048 â€” nucleo_anima_l1_unload() before TLS if heap tight
- firmware/components/nucleo_exclusive.c:45 â€” NX_ANIMA_L1 flag: suspends on exclusive app entry
- Lazy reload on next /api/anima query

**Heavy-work arbiter busy banner:**
- firmware/components/nucleo_arb/nucleo_arb_plat_esp.c:47-55 â€” arb_plat_on_busy() publishes "system.busy" event
- web/shell/shell.js:621-628 â€” renderBusy() shows tray indicator on system.busy event
- Trigger: ARB transition 0â†’1 (acquire) or 1â†’0 (release)

**Per-app exclusive mode (stand-down):**
- firmware/components/nucleo_exclusive.c:36-65 â€” nucleo_exclusive_enter/exit
- Ordine teardown (enter): Discovery â†’ Voice suspend â†’ Recorder stop â†’ Httpd â†’ ANIMA L1 â†’ WiFi
- Ordine restore (exit): WiFi â†’ Voice unsuspend â†’ Discovery resume â†’ Httpd start

---

**NOTE CRITICHE:**

- **No PSRAM, ~18 KB heap**: La scarsitÃ  di memoria rende il nucleo_arb essenziale â€” OOM Ã¨ un crash device, non una degradazione
- **Voice engine lazy**: s_task NULL when idle, ricreato su demand; voice_recompute() governa lo stato
- **Recorder atomic_bool senza mutex**: s_recording/s_streaming atomici a livello hardware, ma s_level/s_path/s_bytes no
- **Voice introspection racy**: s_is_listening/s_learning_mode letti senza portMUX â€” candidati race (basso rischio perchÃ© solo pollati, non critici)
- **Web Locks fallback**: Promise chain fallback garantisce serializzazione anche senza browser Web Locks API
- **OTA gated by nucleo_arb**: Firmware e dlgate.js.withDownloadLock sono **due livelli** â€” firmware Ã¨ l'enforcement, web Ã¨ user-facing UI serialization
