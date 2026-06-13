# NucleoOS — Protocollo notifiche

Una sola spina dorsale per **tutte** le notifiche del sistema: un contratto, N produttori
(calendario, ANIMA, voce, recorder, OTA, sistema, app), 2 superfici (centro web + device).
Filosofia identica al resto dell'OS: **device-autoritativo, il web rende il broadcast**
(vedi `firmware/components/nucleo_app/calendar_svc.cpp` e `docs/event-protocol.md`).

## Trasporto

Le notifiche viaggiano sul bus eventi esistente (`nucleo_eventbus`) col topic **`notify.post`**.
Il sink WebSocket le inoltra a ogni client; lo shell le rende nel Centro Notifiche.
Vincolo del bus: **payload ≤ 208 byte** (`NUCLEO_EVENT_PAYLOAD_MAX`) → i campi testuali
vanno tenuti corti; un corpo lungo si referenzia via `id` al journal su SD, non nel payload.

## Contratto (payload `notify.post`, chiavi corte per stare nei 208 B)

```json
{"id":"cal-0930","src":"calendar","lvl":"info","ic":"🔔",
 "ttl":"Riunione team","bd":"Sala A · 09:30","act":"app:calendar","snd":"info","ts":1718000000}
```

| campo | obbl. | significato |
|-------|:----:|-------------|
| `id`  | no  | chiave di **dedupe/coalescing**: una notifica con id già presente la rimpiazza e mostra `×N` invece di accumularsi. Auto-generato se assente. |
| `src` | sì  | sorgente: `calendar` · `system` · `anima` · `voice` · `recorder` · `ota` · `app`. Decide il tag e l'icona di default. |
| `lvl` | sì  | livello: `info` · `success` · `warn` · `critical`. Decide colore + suono di default. |
| `ic`  | no  | emoji/glyph; default per `src`/`lvl`. |
| `ttl` | sì  | titolo (corto). |
| `bd`  | no  | corpo (corto). Lungo → referenziato via `id`, non nel payload. |
| `act` | no  | azione eseguita al click, instradata sul contratto del copilot: `app:<id>` apre un'app · `file:<path>` apre un file · `anima:<query>` chiede ad ANIMA · vuoto = nessuna. |
| `snd` | no  | profilo audio: `info`·`success`·`warn`·`critical`·`none`. Default = `lvl`. |
| `sticky` | no | `1` = resta finché non la chiudi (niente auto-dismiss del toast). |
| `ts`  | no  | epoch ms; auto se assente. |

## Produttori

Chiunque pubblica con **una sola chiamata**, zero UI nuova:

- **Firmware C:** `nucleo_notify_emit(...)` (in `nucleo_app`) — appende al journal SD, pubblica
  `notify.post` sul bus, e — se nessun client web è connesso (`nucleo_ui_is_remote()` falso) —
  suona la melodia + alza la superficie nativa. È il punto unico: il servizio calendario lo chiama
  al posto del suo codice ad-hoc.
- **Web/JS:** `Notify.emit({src,lvl,title,body,action,sound,...})` (in `web/shell/notify.js`),
  oppure pubblicando `notify.post` sul bus dal firmware (arriva via WebSocket).

Retro-compatibilità: il topic legacy `calendar.reminder` `{time,text}` viene ancora adattato a una
notifica `calendar`/`info` dallo shell, finché il servizio non è migrato a `notify.post`.

## Superfici

1. **Centro Notifiche web** (`web/shell/notify.js`): toast transitorio (stile Win11) + **storico
   persistente** in un flyout aperto dalla campanella in tray, con badge non-letti, **Non
   disturbare**, ore silenziose e "pulisci tutto". Suono = accordo polifonico Web Audio (vera
   polifonia nel browser). Tutto event-driven: zero polling.
2. **Device** (firmware, fase successiva): peek non-bloccante in alto (stile angolo Win11) +
   Centro Notifiche nativo con tab (Tutte / Calendario / Sistema / ANIMA), storico letto dal
   journal SD solo all'apertura. Suono = accordo polifonico sintetizzato a runtime su SD
   (additivo mono, come `ensure_chime`), un timbro per livello. Rispetta volume + DND + ore
   silenziose.

## Disciplina costo

- Nessun task nuovo sempre attivo: il pump è il task `cal-svc` esistente (prio 2).
- Centro web puramente event-driven; il journal SD si legge solo all'apertura del centro
  (mai sul path caldo di ANIMA).
- Coalescing per `id` + DND = poco invasivo per costruzione.
