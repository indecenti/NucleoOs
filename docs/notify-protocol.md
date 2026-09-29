# NucleoOS — Notification protocol

One backbone for **all** of the system's notifications: one contract, N producers
(calendar, ANIMA, voice, recorder, OTA, system, apps), 2 surfaces (web center + device).
Same philosophy as the rest of the OS: **device-authoritative, the web renders the broadcast**
(see `firmware/components/nucleo_app/calendar_svc.cpp` and `docs/event-protocol.md`).

## Transport

Notifications travel on the existing event bus (`nucleo_eventbus`) with the topic **`notify.post`**.
The WebSocket sink forwards them to every client; the shell renders them in the Notification Center.
Bus constraint: **payload ≤ 208 bytes** (`NUCLEO_EVENT_PAYLOAD_MAX`) → the text fields
must be kept short; a long body is referenced via `id` to the journal on SD, not carried in the payload.

## Contract (`notify.post` payload, short keys to fit in the 208 B)

```json
{"id":"cal-0930","src":"calendar","lvl":"info","ic":"🔔",
 "ttl":"Riunione team","bd":"Sala A · 09:30","act":"app:calendar","snd":"info","ts":1718000000}
```

| field | req. | meaning |
|-------|:----:|-------------|
| `id`  | no  | **dedupe/coalescing** key: a notification whose id is already present replaces it and shows `×N` instead of piling up. Auto-generated if absent. |
| `src` | yes | source: `calendar` · `system` · `anima` · `voice` · `recorder` · `ota` · `app`. Decides the tag and the default icon. |
| `lvl` | yes | level: `info` · `success` · `warn` · `critical`. Decides the colour + the default sound. |
| `ic`  | no  | emoji/glyph; defaults per `src`/`lvl`. |
| `ttl` | yes | title (short). |
| `bd`  | no  | body (short). Long → referenced via `id`, not in the payload. |
| `act` | no  | action run on click, routed over the copilot contract: `app:<id>` opens an app · `file:<path>` opens a file · `anima:<query>` asks ANIMA · empty = none. |
| `snd` | no  | sound profile: `info`·`success`·`warn`·`critical`·`none`. Default = `lvl`. |
| `sticky` | no | `1` = stays until you close it (no toast auto-dismiss). |
| `ts`  | no  | epoch ms; automatic if absent. |

## Producers

Anyone publishes with **a single call**, zero new UI:

- **Firmware C:** `nucleo_notify_emit(...)` (in `nucleo_app`) — appends to the SD journal, publishes
  `notify.post` on the bus, and — if no web client is connected (`nucleo_ui_is_remote()` false) —
  plays the melody + raises the native surface. It is the single entry point: the calendar service calls it
  instead of its ad-hoc code.
- **Web/JS:** `Notify.emit({src,lvl,title,body,action,sound,...})` (in `web/shell/notify.js`),
  or by publishing `notify.post` on the bus from the firmware (it arrives via WebSocket).

Backward compatibility: the legacy topic `calendar.reminder` `{time,text}` is still adapted by the shell into a
`calendar`/`info` notification, until the service has been migrated to `notify.post`.

## Surfaces

1. **Web Notification Center** (`web/shell/notify.js`): a transient toast (Win11 style) + a **persistent
   history** in a flyout opened from the bell in the tray, with an unread badge, **Do Not
   Disturb**, quiet hours and "clear all". Sound = a Web Audio polyphonic chord (true
   polyphony in the browser). All event-driven: zero polling.
2. **Device** (firmware, later phase): a non-blocking peek at the top (Win11 corner style) +
   a native Notification Center with tabs (All / Calendar / System / ANIMA), history read from the
   SD journal only when it is opened. Sound = a polyphonic chord synthesized at runtime onto SD
   (additive mono, like `ensure_chime`), one timbre per level. Respects volume + DND + quiet
   hours.

## Cost discipline

- No new always-on task: the pump is the existing `cal-svc` task (prio 2).
- The web center is purely event-driven; the SD journal is read only when the center is opened
  (never on ANIMA's hot path).
- Coalescing by `id` + DND = unobtrusive by construction.
