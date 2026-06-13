# NucleoOS — sistema operativo web-native, a sciame ed energy-aware per Cardputer

Questo documento definisce un piano di esecuzione realistico per costruire un vero sistema operativo embedded — **NucleoOS** — accessibile via web, utile nella pratica e sostenibile sul M5Stack Cardputer. Il piano è diviso in due parti tenute volutamente separate: un **core v1 consegnabile** (realistico, senza fronzoli) e una **roadmap di innovazioni a fasi** che rende il progetto qualcosa che in questa classe di dispositivi non esiste ancora.

## Realtà hardware del Cardputer (verificare la propria variante)

| Risorsa | Specifica | Implicazione di progetto |
|---|---|---|
| SoC | ESP32-S3 dual-core @ 240 MHz (modulo M5StampS3, ESP32-S3FN8) | Abbondante per task + rete + UI minima |
| Flash | 8 MB | Firmware + 2 slot OTA + NVS: spazio adeguato |
| **RAM** | **~512 KB SRAM interna, NESSUNA PSRAM sul modulo standard** | **Vincolo dominante.** Stack Wi-Fi (~50-100 KB) e framebuffer display competono per la SRAM. Niente framework pesanti, niente buffer generosi. |
| Storage di massa | **microSD 64 GB (hot-swap)** | Spazio abbondante per journal, app, backup, cache. **Nota: 64 GB nasce exFAT** → abilitare exFAT in ESP-IDF o riformattare in FAT32 |
| Display | TFT 240×135 (ST7789) | UI locale minima: pairing, stato, recovery, QR |
| Input | Tastiera fisica 56 tasti | Comando locale reale, non solo demo |
| Connettività | Wi-Fi 2.4 GHz + **ESP-NOW** (peer-to-peer) + **BLE 5.0** | ESP-NOW per la mesh senza router; **BLE è l'unico canale wireless raggiungibile anche dal browser Android (Web Bluetooth)** |
| I/O extra | **IR TX**, microfono PDM (SPM1423), speaker I2S (NS4168), Grove | Capability native per automazioni e voce |
| Alimentazione | Batteria ~120 mAh, USB-C | **Energia scarsa: in Wi-Fi attivo < 1h.** L'energia è una risorsa di sistema. |

> ⚠️ **Due note importanti.**
> 1. **PSRAM:** il modulo standard del Cardputer **non ha PSRAM**. Progettiamo per il caso peggiore "solo 512 KB SRAM" (scelta sicura che funziona su ogni variante); se la tua revisione include PSRAM, viene usata come cache opportunistica per sessioni/asset senza cambiare l'architettura.
> 2. **Bluetooth:** l'ESP32-S3 ha **solo BLE 5.0, non Bluetooth Classic** (niente BR/EDR, niente A2DP/SPP). Non è un limite: BLE è esattamente ciò che serve, perché è raggiungibile via **Web Bluetooth** anche dal browser su Android.

## Obiettivo del progetto

L'obiettivo non è imitare Linux o Windows in miniatura, ma creare un **appliance OS** embedded che gestisca servizi, app bundle, storage, automazioni, giochi e una workstation remota via browser. La UI web non è una simulazione dell'OS: il sistema reale gira sul dispositivo, mentre il browser è il terminale operativo avanzato per controllo, installazione, file management e monitoraggio live.

### Risultato atteso della v1

Entro la prima versione utile il sistema deve permettere:

- boot affidabile e recovery di emergenza;
- accesso ai file su microSD dal dispositivo e dal browser;
- installazione e rimozione di app bundle con manifest;
- dashboard web con stato live, log ed eventi;
- aggiornamento firmware OTA con rollback;
- una prima app davvero utile (automazioni / file workflow);
- **accesso via browser anche senza Wi-Fi**, tramite **BLE / WebUSB** (anche da Android) e Web Serial su PC (vedi innovazioni Fase 1);
- interfaccia **keyboard-and-mouse-first**, usabile in modalità desktop Android.

## Principi architetturali

Il progetto è guidato da pochi principi non negoziabili:

1. **ESP-IDF come base unica** per core, networking, tasking e update.
2. **Sistema in flash, contenuti su SD**: il firmware resta in flash; app, dati, log, asset vivono su microSD.
3. **UI remota via HTTP + WebSocket** per stato live e comandi bidirezionali — con **sync a delta**, mai a stato pieno (vincolo RAM).
4. **App model a capability**: ogni app dichiara permessi, eventi, mount point, route web e costo energetico.
5. **Recovery first**: ogni aggiornamento deve poter fallire senza brick del device.
6. **Vertical slice prima della complessità**: ogni fase produce qualcosa di provabile e usabile.
7. **L'energia è una risorsa di sistema**, non un dettaglio: ogni servizio dichiara e rispetta un budget.
8. **Tutto è un evento**: il cambiamento di stato passa per un event bus append-only → osservabilità, replay, undo.

## Architettura proposta

### Layer del sistema

| Layer | Funzione | Implementazione iniziale |
|---|---|---|
| Boot & recovery | Avvio, validazione firmware, rollback | Bootloader ESP-IDF + partizioni OTA |
| Core services | Wi-Fi, auth, **event bus event-sourced**, log, storage, **power manager**, scheduler | C/C++ su ESP-IDF |
| App runtime | Lifecycle app, manifest, mount, permessi, capability | Registry JSON + service launcher |
| Storage | Config, app bundle, dati, log, asset, **event journal** | microSD + aree di sistema |
| Transport | Accesso amministrativo multi-canale | HTTP+WS (Wi-Fi), **BLE GATT**, **WebUSB**, **Web Serial** — stesso protocollo a eventi |
| Local UI | Pairing (QR), setup, recovery, notifiche minime | UI nativa su display 240×135 |
| Web workstation | Dashboard, file manager, installer, logs, admin | HTML/CSS/JS leggero servito dal device (PWA) |

## Accesso e interfacce: PWA-first, multi-trasporto, desktop-ready

### Decisione: una PWA, non un'app nativa (con companion Android opzionale)

L'interfaccia primaria è una **PWA** (Progressive Web App) servita dal device, non un'app Android nativa. Motivo: una sola codebase copre PC e Android, supporta nativamente mouse e tastiera, è installabile, funziona in **modalità desktop Android** (finestra ridimensionabile, cursore, multi-pane) e raggiunge il Cardputer su più trasporti — Wi-Fi *e* senza rete. Una **app Android nativa resta opzionale**, come *companion sottile* solo per ciò che il browser non può fare in background (bridge USB/BLE persistente, gateway ESP-NOW always-on).

### Trasporto adattivo (la PWA sceglie il migliore disponibile)

Tutti i canali parlano **lo stesso protocollo a eventi delta**. La PWA negozia in ordine:

| Priorità | Canale | Disponibile su | Uso |
|---|---|---|---|
| 1 | **Wi-Fi (HTTP + WebSocket)** | ovunque c'è rete | pieno regime, multi-sessione |
| 2 | **BLE GATT (Web Bluetooth)** | Chrome desktop **e Android** | wireless **senza router**, copre Android |
| 3 | **WebUSB** | Chrome desktop **e Android** | via cavo USB-C, senza rete |
| 4 | **Web Serial** | Chrome desktop | fallback via cavo su PC |

> Punto chiave per Android: **Web Serial non esiste su Android**, ma **WebUSB e Web Bluetooth sì**. Per questo BLE è di prima classe nel firmware: è il canale wireless universale, raggiungibile dal telefono senza alcuna rete.

### Mouse, tastiera e modalità desktop Android

La web shell è progettata **keyboard-and-mouse-first**, così in modalità desktop (Android DeX-style o monitor esterno, e ovviamente su PC) si comporta come una vera console di amministrazione:

- **scorciatoie da tastiera** per le azioni frequenti (navigazione file, comandi, log filter);
- **drag & drop** dei file dal sistema operativo verso la SD del Cardputer (upload);
- **menu contestuali** (tasto destro) e **multi-pane** ridimensionabile;
- selezione multipla, copia/incolla, navigazione completa **senza touch**;
- la tastierina 56 tasti del Cardputer resta l'input **locale**; mouse e tastiera del dispositivo Android/PC sono l'input **remoto** ricco.

Bonus abilitato dall'hardware: tramite **BLE HID** il Cardputer può, come capability dedicata, comportarsi anche da *tastiera/telecomando Bluetooth* verso il telefono — utile per automazioni e scenari "presenter".

### Modello app (manifest esteso)

Ogni app è un bundle con questa struttura minima:

```text
/apps/<app-id>/
  manifest.json
  manifest.sig        # firma Ed25519 del manifest+contenuti
  www/
  data/
  assets/
  rules/              # bytecode automazioni (opzionale)
```

Manifest iniziale, con le nuove capability di NucleoOS:

```json
{
  "id": "automation-studio",
  "name": "Automation Studio",
  "version": "0.1.0",
  "entry_service": "automation_main",
  "web_route": "/apps/automation-studio/",
  "permissions": ["storage.app", "system.events", "device.keyboard", "device.ir"],
  "mounts": {
    "app": "/apps/automation-studio/data"
  },
  "subscribes": ["system.boot", "wifi.status", "storage.changed", "ir.received"],
  "publishes": ["automation.rule_fired"],
  "power": {
    "budget_class": "low",
    "wants_wakeup": ["timer", "keyboard", "ir"]
  },
  "mesh": {
    "exposes": ["automation.rule_fired"],
    "consumes": []
  }
}
```

Il manifest dichiara ora anche il **profilo energetico** (`power`) — usato dal power manager per decidere wakeup e sleep — e l'eventuale partecipazione alla **mesh** (`mesh`). Questo abilita un vero registro applicazioni con isolamento logico, installazione, aggiornamento, debugging *e* gestione dell'energia.

## Partizionamento e storage

ESP-IDF richiede almeno due partizioni OTA (`ota_0`, `ota_1`) e una partizione `otadata` per aggiornamenti con rollback: la prima scelta seria è definire una tabella custom invece di layout casuali.

### Strategia di memoria (vincolata: 512 KB SRAM, no PSRAM)

- **Flash:** bootloader, tabella partizioni, `nvs`, `otadata`, `phy_init`, `factory` minima, `ota_0`, `ota_1`.
- **SRAM (~512 KB) — disciplina rigida:**
  - allocazione **prevalentemente statica** per i servizi core (no frammentazione heap);
  - **un solo** framebuffer parziale per il display (partial refresh LVGL/diretto), non full-frame;
  - **niente sync a stato pieno** sul WebSocket: solo eventi delta, serializzati compatti;
  - massimo 1-2 sessioni web concorrenti garantite; le altre best-effort;
  - buffer audio/log circolari di dimensione fissa.
- **microSD:** `/system`, `/apps`, `/data/shared`, `/logs`, `/journal` (event sourcing), `/www-cache`, `/backups`.

### Layout SD consigliato

```text
/system/
  config/
  registry/
  sessions/
  keys/            # chiave device + chiavi fidate per bundle firmati
  logs/
/journal/          # event log append-only (sorgente di verità replicabile)
/apps/
  automation-studio/
  file-commander/
  game-runtime/
/data/
  shared/
  imports/
  exports/
/backups/
/www/
  shell/           # PWA installabile, funziona offline
```

## Backlog di prodotto: cosa fare prima (CORE v1)

### Deliverable 1 — Foundation Boot

Obiettivo: far partire il nucleo e montare la SD in modo affidabile.

Checklist:
- repository con struttura `firmware/`, `web/`, `docs/`;
- build ESP-IDF pulita su Cardputer;
- partizioni custom OTA;
- mount SD e test lettura/scrittura;
- logger seriale e logger persistente su file;
- schermata locale di boot/recovery.

Criterio: il device boota, mostra stato, monta la SD e genera log persistenti.

### Deliverable 2 — Core Services

Obiettivo: rendere il sistema un vero runtime, non solo un firmware.

Checklist:
- service manager con stati `registered`, `starting`, `running`, `failed`, `stopped`;
- **event bus centrale event-sourced** (ogni evento appeso al journal su SD);
- gestione configurazione JSON;
- health check dei servizi;
- API interne per log, notifiche e storage;
- **power manager minimo**: conteggio consumo per stato, soglie di sleep.

Criterio: un servizio demo si avvia, emette eventi, viene riavviato in caso di errore, e gli eventi sono ricostruibili dal journal.

### Deliverable 3 — Web Workstation

Obiettivo: l'accesso browser come interfaccia primaria di amministrazione.

Checklist:
- server HTTP base;
- endpoint REST di stato;
- WebSocket per eventi **delta** live;
- dashboard con info di sistema;
- viewer log in tempo reale;
- pagina diagnostica Wi-Fi/storage/**batteria**;
- shell servita come **PWA** (cache offline).

Criterio: collegandosi via browser si vedono stato, log e cambiamenti senza refresh completi.

### Deliverable 4 — File Commander

Obiettivo: uso reale quotidiano del dispositivo da subito.

Checklist:
- listing directory SD;
- upload / download file via browser;
- rename/delete/create dir;
- anteprima testo;
- notifiche locale + web sugli eventi file.

Criterio: il Cardputer diventa un appliance tascabile per spostare, leggere e organizzare file.

### Deliverable 5 — App Registry

Obiettivo: passare da utility singole a piattaforma modulare.

Checklist:
- parser manifest;
- scansione `/apps`;
- install/uninstall bundle;
- start/stop app;
- pagina web "Installed apps";
- gestione errori di avvio.

Criterio: almeno due app nel registry, lanciabili e rimovibili senza riflash.

### Deliverable 6 — Prima app di valore: Automation Studio

La prima app deve dimostrare che il sistema è utile, sfruttando eventi, storage, web UI e interazione locale.

Funzioni iniziali:
- regole evento → azione (anche su IR ricevuto e timer);
- notifiche locali e web;
- scrittura su file/log;
- reazione a tasti, timer, stato Wi-Fi, file system;
- stato live via browser.

Criterio: l'utente definisce una regola dal browser e il device la esegue senza riflash.

### Deliverable 7 — OTA serio

Obiettivo: progetto distribuibile e aggiornabile.

Checklist:
- endpoint update;
- download firmware;
- validazione immagine (**firma**);
- switch partizione OTA;
- boot health check;
- rollback automatico in caso di crash o mancata conferma.

Criterio: aggiornamento riuscito e rollback testato su firmware volutamente difettoso.

## Roadmap di innovazione (cosa rende NucleoOS "mai visto")

Queste capacità arrivano **dopo** il core, a fasi, per non tradire la consegnabilità. Ognuna riusa l'event bus e il modello a capability già esistenti.

### Fase 1 — Accesso zero-rete multi-canale (BLE + WebUSB + Web Serial)
La workstation web funziona **senza Wi-Fi**, con lo stesso protocollo a eventi su più trasporti, così da coprire **sia PC che Android**:
- **BLE GATT (Web Bluetooth):** wireless senza router, raggiungibile dal browser su Chrome desktop **e Android** — il canale universale;
- **WebUSB:** via cavo USB-C, funziona su Chrome desktop **e Android**;
- **Web Serial:** fallback via cavo sul solo Chrome desktop.
Risultato: apri la PWA, ti colleghi (cavo o BLE) e amministri il device anche dove non c'è rete, dallo stesso telefono Android. Pochissimi OS embedded offrono un canale di amministrazione completo via browser senza IP, e ancor meno coprono Android wireless.

### Fase 2 — Sicurezza reale e pairing dal display
- chiave device generata on-board, conservata in `/system/keys`;
- bundle app **firmati Ed25519**, verificati all'install;
- pairing browser↔device via **QR mostrato sul display 240×135** (trust-on-first-use), niente password digitate sulla tastierina.

### Fase 3 — Automazioni come micro-VM sandbox
Invece di un interprete generale (vietato dal piano), una **VM a stack minima** che esegue bytecode compilato da un editor visuale a blocchi nel browser. Deterministica, minuscola (poche KB), sandboxata per capability: una regola può toccare solo ciò che l'app dichiara. Le regole vivono in `/apps/<id>/rules/`.

### Fase 4 — Voce e IR come capability native
- **Wakeword on-device** (ESP-SR / WakeNet): comando vocale locale senza cloud, esposto come evento `voice.command`;
- **IR universale**: TX/RX come capability `device.ir`, integrato nelle automazioni (es. "alla parola chiave, spegni la TV").

### Fase 5 — OS a sciame (ESP-NOW mesh) ⭐ DIMOSTRATORE FLAGSHIP
**Questa è l'innovazione scelta come elemento "mai visto" del progetto.** Più Cardputer formano un **cluster ad-hoc senza router** via ESP-NOW (latenza <5 ms, bassissimo consumo, no IP):
- **clipboard e file condivisi** tra device vicini;
- **app distribuite**: un'app dichiara `mesh.exposes/consumes` e l'event bus si estende *trasparentemente* oltre il singolo device (gli eventi remoti arrivano come eventi locali);
- **giochi multiplayer** e **mesh di sensori** tascabili;
- discovery dei peer e gossip degli eventi costruiti **sopra il bus event-sourced già esistente** — nessun sottosistema separato.

Percorso minimo per il dimostratore (vedi Mese 3):
1. discovery peer ESP-NOW + handshake firmato (riusa le chiavi della Fase 2);
2. ponte event bus ↔ ESP-NOW: pubblica/sottoscrivi eventi marcati `mesh`;
3. demo end-to-end: **clipboard condivisa** tra due Cardputer (copio su uno, incollo sull'altro) e un evento `automation.rule_fired` che scatta su un device e notifica l'altro.

Un appliance OS tascabile che si federa in sciame, senza infrastruttura e con bus eventi unificato, è l'elemento davvero originale del progetto.

### Fase 6 — Time-travel e undo di sistema
Poiché tutto è event-sourced nel `/journal`, si abilitano:
- **replay deterministico** per debug ("riproduci l'ora prima del crash");
- **undo** di operazioni di sistema (ripristina lo stato a un evento precedente);
- **sync a delta** efficiente verso il browser anche con 512 KB di RAM (si inviano solo gli eventi nuovi).

## Piano di esecuzione in 30 giorni (core)

### Settimana 1 — Boot e fondamenta
Init progetto ESP-IDF; partizioni; mount SD; display+tastiera; logging stabile.
Output: firmware che boota sempre, test hardware base, struttura repo congelata.

### Settimana 2 — Core runtime
Service manager; **event bus event-sourced**; config system; primi endpoint REST; power manager minimo; diagnostica locale.
Output: ≥2 servizi di sistema attivi (`storage`, `events`) visibili da log e ricostruibili dal journal.

### Settimana 3 — Web shell iniziale
Dashboard HTML minimale (PWA) **keyboard-and-mouse-first**; WebSocket eventi delta; viewer log; pagina storage+batteria; primi comandi remoti; **prototipo BLE GATT** (trasporto senza rete, valido anche su Android).
Output: browser collegato al Cardputer con vista live, via Wi-Fi e via BLE; PWA usabile in modalità desktop con mouse e tastiera.

### Settimana 4 — File Commander + Registry
File manager funzionante; scansione bundle; start/stop moduli; prima installazione da SD o upload web.
Output: un sistema già utile, non solo dimostrativo.

## Piano di esecuzione in 90 giorni

### Mese 1 — Base utile
Boot, SD (exFAT/FAT32), dashboard PWA keyboard-and-mouse-first, file manager, registry, accesso senza rete via **BLE/WebUSB**.

### Mese 2 — Sistema credibile
Auth + pairing QR, bundle firmati, permessi, automazioni (micro-VM), UX web desktop-ready, salvataggio config, gestione errori, power manager completo.

### Mese 3 — Sistema distribuibile e originale
OTA + rollback, bundle versionati, backup, test, profiling. **Dimostratore flagship abilitato end-to-end: ESP-NOW mesh** — clipboard ed eventi condivisi tra due Cardputer, con event bus federato (vedi Fase 5). Documentazione per terzi.

## Scelte tecniche raccomandate

| Area | Scelta | Motivo |
|---|---|---|
| Core firmware | ESP-IDF C/C++ | controllo fine di task, rete, update |
| UI web | HTML + CSS + JS/TS leggero, **PWA keyboard-and-mouse-first** | una codebase per PC+Android, modalità desktop, niente app nativa obbligatoria |
| Messaging | REST + WebSocket (**delta**) su Wi-Fi; **BLE GATT / WebUSB / Web Serial** senza rete | stesso protocollo su ogni trasporto; BLE/WebUSB coprono Android |
| Config | JSON | semplice da ispezionare e modificare |
| App packaging | bundle con manifest **+ firma Ed25519** | scalabile, leggibile, fidato |
| Automazioni | micro-VM a bytecode sandbox | sicura e minuscola, no interprete generale |
| Persistenza | microSD + **event journal** | capacità + replay/undo, separazione da flash |
| Mesh | **ESP-NOW** | peer-to-peer senza router, bassa latenza |
| Recovery | dual-slot OTA | aggiornamenti robusti |

## Cosa NON fare

Per mantenere il piano consegnabile, evitare subito:

- desktop environment complesso con finestre multitasking;
- interprete generale Python come fondazione del sistema (la micro-VM lo sostituisce in sicurezza);
- emulatori come asse principale del progetto;
- marketplace online complesso prima di registry e bundle locali;
- permessi sofisticati prima di manifest semplici;
- mesh, voce e time-travel **prima** che il core v1 (boot→registry→OTA) sia stabile;
- **un'app Android nativa come interfaccia primaria**: la PWA copre PC e Android con una sola codebase; l'app nativa resta un companion opzionale solo per bridge USB/BLE in background;
- qualunque ipotesi di "abbondanza di RAM": progettare sempre per 512 KB.

## Rischi e contromisure

| Rischio | Impatto | Contromisura |
|---|---|---|
| **RAM insufficiente (no PSRAM)** | **Alta** | allocazione statica, sync a delta, no framework pesanti, 1 framebuffer parziale |
| Batteria che si esaurisce in <1h | Alta | power manager, sleep aggressivo, budget energetico per servizio, wakeup mirati |
| Crescita incontrollata del firmware | Alta | asset/app/UI su SD |
| OTA instabile | Alta | testare subito partizioni e rollback |
| UI web troppo pesante | Media | dashboard minimale, PWA, niente framework pesanti |
| Registry troppo astratto | Media | bundle semplici e una prima app reale |
| Innovazioni che destabilizzano il core | Media | fasi separate; ogni innovazione riusa event bus e capability esistenti |
| Progetto troppo teorico | Alta | consegnare File Commander entro 30 giorni |

## Definizione di successo

Il progetto è riuscito quando soddisfa insieme:

- si avvia sempre e si aggiorna senza brick;
- espone una dashboard web realmente usabile con mouse e tastiera, **anche senza Wi-Fi (BLE/WebUSB, incluso Android)**;
- gestisce file e app bundle firmati su SD;
- offre almeno una funzione quotidianamente utile;
- rispetta un budget energetico misurabile;
- permette di aggiungere moduli senza riscrivere il nucleo;
- abilita almeno **una** capacità originale (mesh, voce/IR o time-travel) come dimostratore.

## Primo sprint consigliato

Molto concreto:

1. inizializzare progetto ESP-IDF Cardputer;
2. definire partizioni e recovery minima;
3. montare SD;
4. leggere tastiera e mostrare stato a schermo;
5. implementare log persistente **su journal append-only**;
6. creare HTTP server base;
7. esporre `/api/status` (incluso stato batteria);
8. servire una pagina web statica (PWA) con stato device, navigabile con mouse e tastiera;
9. prototipo di canale **BLE GATT** che riusa lo stesso `/api/status` (trasporto senza rete, testabile anche da Chrome Android).

Se questo sprint viene completato, il progetto passa da idea a piattaforma reale.

## Conclusione operativa

NucleoOS è reale e consegnabile se trattato come una piattaforma embedded con una verticale utile già nella v1 — **file manager + dashboard PWA + app registry + automazioni + OTA** — costruita sotto due vincoli onesti: **512 KB di RAM** e **120 mAh di batteria**. L'interfaccia è una **PWA keyboard-and-mouse-first**, una sola codebase che copre PC e Android (anche in modalità desktop), raggiungibile via Wi-Fi e — **senza rete** — via **BLE/WebUSB**, così non serve un'app nativa come scelta obbligata. Su questa base solida si innesta ciò che lo rende unico: kernel **event-sourced** con undo e replay, app **firmate** come cartucce su SD, automazioni in **micro-VM** sandbox, **voce/IR** native e — l'elemento mai visto in questa classe, scelto come dimostratore flagship — un **OS a sciame ESP-NOW** che federa più Cardputer senza infrastruttura. Innovazione vera, ma poggiata su un nucleo che funziona.

## Riferimenti hardware

- M5Stack Cardputer (modulo M5StampS3 / ESP32-S3FN8): datasheet e schemi ufficiali M5Stack — **verificare presenza PSRAM sulla propria revisione**. Bluetooth = **BLE 5.0 only** (no Classic).
- Espressif ESP-IDF: partizioni OTA, `esp_http_server`, ESP-NOW, BLE/NimBLE, ESP-SR/WakeNet, FATFS (exFAT/FAT32 per SD 64 GB).
- W3C **Web Bluetooth / WebUSB / Web Serial** API (Chromium): trasporti browser senza rete. Nota: Web Serial non è disponibile su Android; BLE e WebUSB sì.
