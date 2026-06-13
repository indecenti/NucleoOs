# PROMPT OPERATIVO — Modalità Esclusiva Dichiarativa + Hardening App Pesanti (NucleoOS firmware)

## 1. Obiettivo

Rendere la **modalità esclusiva** (`nucleo_exclusive`) **coerente e dichiarativa**, far partire le **app pesanti** con risorse dedicate seguendo il pattern già usato da SSH/Video/Recorder, **correggere i leak e le incongruenze** rilevati dall'audit, e **ottimizzare** i punti OOM-sensibili. Lavora SOLO su `G:\Nucleo\firmware\components\nucleo_app\` (più `G:\Nucleo\firmware\components\nucleo_wifiatk\nucleo_wifiatk.c` e `G:\Nucleo\firmware\components\nucleo_evilportal\nucleo_evilportal.c` per gli engine). Mantieni lo stile del codebase: commenti brevi e solo se utili, licenza MIT, nessun asset bakeabile.

**NON flashare e NON deployare nulla.** L'utente flasha a mano via seriale. Tu ti fermi a build + gate host verdi.

## 2. Contesto (vincoli hardware da rispettare sempre)

- M5Stack Cardputer, ESP32-S3, **NO PSRAM**. Heap libero a runtime con OS completo ~18KB. Collo di bottiglia = **largest free block / frammentazione**, NON CPU.
- Due assi di reclaim **ortogonali e additivi**:
  - **Canvas 32KB** (`nucleo_app_release_buffers()` / `nucleo_app_set_direct_draw()`): per decoder/framebuffer/worker. NON toccato da `nucleo_exclusive`.
  - **Esclusiva ~70KB** (`nucleo_exclusive_enter(flags)`): sospende sottosistemi pesanti. NON tocca mai il canvas.
- Maschera canonica app-di-rete (~70KB, Wi-Fi STA resta su): `NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY | NX_VOICE`.
- **REGOLA FERREA: `NX_WIFI` / `NX_DEEP_OFFLINE` VIETATI per qualsiasi app che usa la rete** (SSH, streaming, online-LLM, upload TLS) e per le app offensive che POSSIEDONO la radio (Evil Portal, WiFiAtk, Beacon — staccare il Wi-Fi le ucciderebbe). `NX_WIFI` solo per app standalone deep-offline (es. TTS on-device): nessuna di quelle in scope qui.
- `nucleo_exclusive_enter` è idempotente (2° enter = no-op che NON aggiunge flag); `nucleo_exclusive_exit` è no-op se inattivo e safe in re-entrancy. Riempie sempre `out` (può essere NULL) anche se no-op.
- `nucleo_exclusive_enter` **NON** chiama `nucleo_setup_suspend()` né `esp_wifi_stop()` salvo `NX_WIFI`; `nucleo_exclusive_exit` chiama `nucleo_setup_apply_network()` SOLO se `NX_WIFI` era settato. Conseguenza critica per le app radio (vedi §6).

## 3. Modifiche al framework (approccio dichiarativo)

File: `include/nucleo_app.h`, `nucleo_app.cpp`, `include/nucleo_exclusive.h`.

### 3.1 Costante condivisa
In `include/nucleo_exclusive.h` aggiungi una costante per eliminare il drift dell'ordine dei flag:
```c
/* canonical reclaim mask for network apps (Wi-Fi STA stays up) */
#define NX_NET_APP (NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY | NX_VOICE)
```

### 3.2 Campo dichiarativo nel def
In `nucleo_app.h` (struct `nucleo_app_def_t`, righe 12-24) aggiungi **in coda dopo `on_exit`** un campo:
```c
uint32_t exclusive_flags; /* 0 = none; framework enters on open, exits on close */
```
È retro-compatibile con l'init posizionale delle ~28 register (chi non lo specifica resta 0). **Verifica che NESSUNA register esistente usi un init posizionale che metta un valore dopo `on_exit`** (grep su `nucleo_app_register` / aggregati `{...}`); se qualcuna lo fa, va corretta.

### 3.3 Gestione centrale + safety-net garantito
- In `open_app_def()` (nucleo_app.cpp:220-237), **subito dopo** `nucleo_anima_l1_unload_if_idle()` (riga 228) e prima del `def->on_enter()`:
  ```c
  if (def->exclusive_flags) nucleo_exclusive_enter(def->exclusive_flags, NULL);
  ```
- In `close_app()` (nucleo_app.cpp:301-315), **dentro `on_exit()` prima del re-acquire canvas** (cioè dopo `def->on_exit()` ma prima del retry `nucleo_screen_acquire`), aggiungi un **safety-net incondizionato** valido per TUTTE le app (dichiarative e imperative):
  ```c
  if (nucleo_exclusive_active()) nucleo_exclusive_exit(); /* never leave services suspended */
  ```
  Questo centralizza la rete di sicurezza che oggi è duplicata e non uniforme. Mantieni l'ordine **canvas-dopo-rete** già presente in `close_app` (exit esclusiva → poi retry acquire canvas).

### 3.4 Modello di adozione (NON forzare il dichiarativo dove serve granularità)
- **Dichiarativo (`exclusive_flags = NX_NET_APP`)**: SOLO per app pesanti **per tutta la loro vita in foreground** → **SSH**. L'app diventa esclusiva all'apertura, esce alla chiusura, e il safety-net del framework garantisce il restore su back/crash/disconnessione.
- **Imperativo (per-azione, `exclusive_flags = 0`)**: app che vogliono esclusiva solo durante l'azione pesante (play/record/arm) → **Video, Recorder, Music, Evil Portal, WiFiAtk, Beacon**. Restano con enter/exit manuale dentro l'azione, MA ora possono **affidarsi al safety-net centrale di `close_app`** (§3.3) per il ripristino su uscita anticipata.

## 4. Tabella app → flag

| App | File | exclusive_flags (dichiarativo) | Enter imperativo (dove) | Note |
|---|---|---|---|---|
| **SSH** | `app_ssh.cpp` | `NX_NET_APP` (dichiarativo) | rimuovere l'enter manuale (riga 288) → diventa framework-managed; tenere SOLO il gate soglia (vedi §5) eventualmente con un re-check | mai NX_WIFI |
| **Video** | `app_video.cpp` | 0 | manuale in `play_from` (riga 1579) `NX_NET_APP` | resta per-play; safety-net ora centrale |
| **Recorder** | `app_recorder.cpp` | 0 | manuale in `ai_task` (riga 419) `NX_NET_APP`, condizionato a `nucleo_anima_online_available()` | passare `&inf`, gate soglia, gestire join (vedi §5) |
| **Music** | `app_player.cpp` | 0 | manuale: estendere il release-canvas a TUTTI i play path + `NX_NET_APP` opzionale | priorità: canvas su `play_q`, vedi §5 |
| **Evil Portal** | engine `nucleo_evilportal.c` | 0 | manuale in `start_impl` (~r700) `NX_NET_APP` **+ mantenere `nucleo_setup_suspend()`** | exit in `nucleo_evilportal_stop()` + `nucleo_setup_apply_network()`. Mai NX_WIFI |
| **WiFiAtk** | engine `nucleo_wifiatk.c` | 0 | manuale in `deauth_start` (~r533) `NX_NET_APP` **in AGGIUNTA** a `setup_suspend` | exit in `deauth_stop` + `apply_network`. Mai NX_WIFI |
| **Beacon** | engine `nucleo_wifiatk.c` (beacon_start ~r691) | 0 | analogo a WiFiAtk: `NX_NET_APP` + `setup_suspend` accanto | exit in `beacon_stop` + `apply_network` |
| ANIMA | `app_anima.cpp` | **0 — NON usare** | nessuno | NX_ANIMA_L1/NX_VOICE/NX_HTTPD ucciderebbero le feature; reclama su canvas+L1-policy |
| Radio | `app_radio.cpp` | **0 — NON usare** | nessuno | reclama su canvas (già corretto); L1 già scaricato all'enter |
| Photos, Files, Clock, Calendar, Notepad, Notify, Sysmon, Torch, USB, USBKbd, USBHID-kbd, Voice trainer, Voice Lab, Wi-Fi mgr, app_ui | vari | **0 — light, NON usare** | nessuno | confermato light dall'audit |

**Regola anti-radio (critica):** Evil Portal / WiFiAtk / Beacon usano `NX_NET_APP` **senza** NX_WIFI; poiché `nucleo_exclusive` con maschera senza NX_WIFI **non** chiama `setup_suspend`/`apply_network`, questi engine **devono mantenere** `nucleo_setup_suspend()` accanto all'enter e `nucleo_setup_apply_network()` accanto all'exit. L'esclusiva si aggiunge SOLO per recuperare VOICE + ANIMA_L1 (oggi mancanti), non sostituisce il teardown rete.

## 5. Regole / error-path (pattern obbligatorio per ogni enter imperativo)

1. **PRE-CHECK** prerequisiti (host/utente/online disponibile) PRIMA dell'enter, così non si sospendono servizi per poi abortire.
2. **ENTER** con `&inf` (mai `nullptr`): `nucleo_exclusive_info_t inf; nucleo_exclusive_enter(NX_NET_APP, &inf);` + 1 log conciso `free/largest`.
3. **ALLOC** dei buffer pesanti DOPO l'enter.
4. **GATE SOGLIA reclaim-aware** sul blocco contiguo: se alloc fallita **OR** `inf.largest_after < soglia` → free parziali, `nucleo_exclusive_exit()`, status "RAM insufficiente", torna a stato chiuso. Usa la soglia di SSH come riferimento (`34*1024` per scrollback; per gli altri scegli la soglia coerente col buffer richiesto). Applica questo gate a **Recorder, Evil Portal, WiFiAtk, Beacon** che oggi NON lo hanno.
5. **EXIT su OGNI cammino d'errore intermedio** (alloc fallita, `xTaskCreate != pdPASS`, uscita anticipata).
6. **EXIT a fine lavoro**: ripristina/riacquisisci il canvas MENTRE la RAM è abbondante, POI `nucleo_exclusive_exit()` (rete per ultima) — pattern di `app_video.cpp:1602-1604` (`return_to_list()` poi exit).
7. **SAFETY-NET**: ora garantito centralmente da `close_app` (§3.3). Rimuovi i safety-net duplicati per-app (SSH r507, Video r1675) SOLO dopo aver confermato il centrale; in alternativa lasciali (idempotenti, innocui) finché la migrazione non è verificata.

**INVARIANTE:** per ogni enter deve esistere un exit raggiungibile su ogni cammino (successo, errore, uscita anticipata, back, crash). Per i task **detached** (Recorder `ai_task`), il safety-net centrale NON basta da solo perché può riavviare httpd/mDNS mentre il worker è ancora in TLS: serve **tracciare la TaskHandle + stop-flag + join** in `on_exit` (modello SSH on_exit r499-508: segnala quit, attende, `vTaskDelete`, POI exit), NON un `if(active) exit()` nudo.

## 6. Bug e incongruenze da correggere (priorità)

### Alta priorità (missing-exit / leak servizi)
- **Recorder** `app_recorder.cpp`: `exit_app` (r1071-1076) NON ha safety-net esclusiva. Aggiungi tracciamento handle `ai_task` + stop-flag + join in `exit_app`, poi `nucleo_exclusive_exit()`. Passa `&inf` all'enter (r419) + gate soglia. Stringere il single-owner su `s_job` (CAS/guard) per chiudere la finestra double-enter (start_job r506 vs ask_all r520).
- **SSH** `app_ssh.cpp`: la sessione che muore (`ssh_task` done: r276 → ST_CLOSED) e il ramo `on_key` ST_CLOSED (r358) NON chiamano `nucleo_exclusive_exit()` → servizi sospesi finché l'app non si chiude. Con SSH ora **dichiarativo** (§3.4), l'esclusiva è legata alla vita dell'app: va bene che resti attiva nel form "Disconnesso" SE è una scelta voluta; **MA** se si vuole liberare la RAM tra una sessione e l'altra, chiama exit sul ritorno a ST_FORM. Decidi UNA semantica e rendila coerente; documenta in commento breve. Il safety-net centrale copre comunque la chiusura app.
- **WiFiAtk** `app_wifiatk.cpp`: `leave()` (r366-370) è vuoto → flood resta armato + httpd/mDNS/setup sospesi a tempo indefinito, **e il launcher NON ha banner per wifiatk** (controlla solo `nucleo_evilportal_running()`). Fix: fermare il flood in `leave()` (chiamare `nucleo_wifiatk_deauth_stop`) salvo modalità background ESPLICITA; se si tiene il background, aggiungere un indicatore di sistema persistente (banner launcher analogo a Evil Portal in `launcher_render.cpp:238/311`).
- **Beacon** `app_beacon.cpp`: stesso problema (`leave()` vuoto r163-166, spam persiste). Aggiungere banner/indicatore o conferma all'uscita. Inoltre **footgun auto-riarmo**: `s_consented` (r34/85) è latch per-boot mai resettato → riaprendo l'app dopo stop esterno ri-trasmette senza consenso. Reset `s_consented` in `leave()` per renderlo per-sessione.

### Media priorità (OOM-risk / robustezza)
- **Music** `app_player.cpp`: i play path lato browser (autoplay descend r989, skip ]/[ r998-999, e i path `play_q` che NON passano da `now_playing`) avviano il decoder Helix con il canvas 32KB **ancora allocato** → OOM/audio muto. Fix primario: **centralizzare il release-canvas in `play_q()`** prima di `nucleo_audio_play` così TUTTI i path lo coprono (oggi solo `now_playing` r1277). Inoltre `play_q` ingoia `nucleo_audio_play != ESP_OK` (r341) silenziosamente → aggiungi pre-check `heap_caps_get_largest_free_block` + feedback utente su drop. (`now_playing` modal loop r1295-1353 e race su `started`/`s_play_counted`: spostare in PState o resettare su `play_q` — media, opzionale in questo giro.)
- **music_db** `app_player_db.cpp`: `music_db_search` (r197) fa `malloc(~64KB contiguo)` → fallisce quasi sempre a runtime, percepito come "non trova brani". Sostituire con `realloc` a chunk (16/32) o cap dinamico/streaming. `music_db_get_unique` (r261) NON ha null-check su `malloc` → crash potenziale: aggiungere `if(!res){*out_list=NULL; fclose(f); return 0;}`. `music_db_set_fav` (r170) ignora il parametro `fav` e togglA sempre → correggere a set assoluto. `update_jsonl_record` `line[512]` (r139): righe JSON lunghe corrompono il record → ampliare buffer o gestire righe lunghe.
- **Evil Portal** engine: `start_impl` (r700-712) non recupera L1/voice prima di avviare httpd captive 8KB + dns_task + template fino a ~64KB transitori. Adottare `NX_NET_APP` mantenendo `setup_suspend`/`apply_network` (§4) + gate `inf.largest_after`. Error path `start_server()` fail (r712-717): ordine subottimale (socket UDP/53 legato ~1s) — riordinare attesa task prima del restart rete.
- **WiFiAtk** engine: teardown manuale incompleto (r533-535: solo setup+httpd+discovery, NON voice/L1). Adottare `NX_NET_APP` in aggiunta (§4) + gate soglia (oggi `deauth_start` ritorna sempre ESP_OK).
- **IR** `app_ir.cpp`: picco RAM evitabile — `load_catalog` (r69-71) tiene buf ≤96KB + DOM cJSON coesistenti. Aggiungere `nucleo_app_release_buffers()` in `enter()` prima del load e/o abbassare il cap. Fix parziale-alloc silenzioso (s_rem ok / s_tvp fail, r72-74).

### Bassa priorità (cosmetico / coerenza)
- **ANIMA** `app_anima.cpp`: allineare i commenti stale "16 KB worker" → **30 KB** (r1430, r1485, r1137; r1880/1913 già corretti). Preset seeded scartato quando worker assente (r1471-1474): eseguire inline-offline o push meta. Unificare cap lettura calendario (r2401/2454 = 200000) al cap writer (32768).
- **Clock** `app_clock.cpp`: `desc` cita "stopwatch" inesistente (r42); `enter()` (r8) definito ma NON collegato (r43 passa `nullptr` come on_enter). O agganciare `enter` o correggere desc + rimuovere dead code. `strftime` con `tm` potenzialmente NULL (r23): proteggere come r22.
- **Sysmon** `app_sysmon.cpp`: gauge Free RAM su fondo-scala fisso 512KB (r50) → mostrare `largest_free_block` reale; batteria hardcoded 100% (r44-46); semantica gauge non uniforme (free vs used); desc cita "network" assente.
- **Notepad** `app_notepad.cpp`: `lines[40]` con indicizzazione `l % 40` (r156-172) corrompe il render oltre 40 righe → clamp/paginazione.
- **Notify** `app_notify.cpp`: `malloc` s_items non null-checked (r204) — degrada pulito ma aggiungere hint; tasto 'c' cancella il journal direttamente (r189-192) invece che via API del produttore `nucleo_notify` → race ownership.
- **Photos** `app_photos.cpp`: hint non aggiornato nel ramo scorciatoia 1-9 (r79-87) → fattorizzare `enter_view()` condiviso (hint+release_buffers+direct_draw).
- **Voice trainer** `app_voice.cpp`: gap sicurezza — non chiama `nucleo_voice_set_test_mode` → un GO fuori dal flow di arm può dispatchare un comando reale. Aggiungere `set_test_mode(true)` in `enter()` / `false` in `exit_app()`.
- **USB** `app_usbkbd.cpp`/`app_usb.cpp`: return di start ignorato → fallimento silenzioso "Waiting for PC..." (leggere `esp_err_t`); spazio non documentato come trigger reboot (app_usb r25).
- **app_ui.cpp**: stato morto `s_last` (r13/43) rimovibile; doc drift header (marquee/smooth-scroll inesistenti) — allineare commento.

## 7. Vincoli

- **MIT**, niente dipendenze da codice AGPL (Bruce è solo riferimento). Commenti brevi, solo se aggiungono valore.
- **NON** includere mai `NX_WIFI`/`NX_DEEP_OFFLINE` in nessuna app in scope.
- **NON** rompere l'init posizionale delle register esistenti (campo nuovo SOLO in coda).
- **NON** rigenerare l'indice ANIMA L1, non toccare la cascata/gate ANIMA.
- **NON** modificare il comportamento di Esc (app-owned nello shell web; firmware invariato per scelta utente).
- **NON** flashare, **NON** lanciare `flash.ps1`/`sd-sync.ps1`/deploy/OTA. Ti fermi a build + gate.
- Modifiche al canvas e all'esclusiva restano **assi ortogonali**: non fonderli.

## 8. Checklist di verifica (prima di considerare finito)

1. [ ] `nucleo_app.h`: campo `exclusive_flags` in coda alla struct, init posizionali esistenti non rotti (grep verifica).
2. [ ] `nucleo_exclusive.h`: costante `NX_NET_APP` definita; tutte le app-di-rete la usano (no ordine flag divergente).
3. [ ] `open_app_def`/`close_app`: enter dichiarativo + safety-net `if(active) exit()` centralizzato, ordine canvas-dopo-rete preservato.
4. [ ] SSH dichiarativo (`exclusive_flags=NX_NET_APP`), semantica disconnessione decisa e coerente.
5. [ ] Recorder: handle tracciata + join in `exit_app` + `&inf` + gate soglia + single-owner `s_job`.
6. [ ] Evil Portal / WiFiAtk / Beacon: `NX_NET_APP` **accanto** a `setup_suspend`/`apply_network` (NON al posto), gate soglia, leave/stop coerente, indicatore di stato per le offensive in background.
7. [ ] Music: release-canvas centralizzato in `play_q` su TUTTI i path + feedback su drop.
8. [ ] music_db: `search` chunked, `get_unique` null-check, `set_fav` set assoluto, `line[]` ampliato.
9. [ ] Ogni enter imperativo ha exit raggiungibile su ogni cammino (successo/errore/back/crash) — riletto path per path.
10. [ ] Build firmware verde (compilazione completa del componente `nucleo_app` + `nucleo_wifiatk` + `nucleo_evilportal`).
11. [ ] Gate host verdi dove applicabili (es. `npm run anima:gate` / `route-check` / `skill-check` se la modifica li tocca — qui probabilmente NON toccati, ma esegui i gate di build/host previsti da `flash.ps1` come pre-flight senza flashare).
12. [ ] Nessun warning nuovo introdotto; commenti brevi/MIT; nessun asset SD aggiunto.
13. [ ] Riepilogo finale: elenco file modificati (path assoluti), riga/funzione per ogni fix, e nota esplicita "NON flashato — flash manuale a carico dell'utente".

Procedi nell'ordine: prima §3 (framework), poi §4-§5 sulle app esclusive (SSH dichiarativo, poi Recorder/Video/Music/engine offensivi), poi §6 alta→bassa priorità, infine §8 checklist. Cita sempre file e righe nei commit/log di lavoro.