# KeyDeck — the Cardputer as a Wi-Fi keyboard (and system monitor) for NucleoV2

KeyDeck turns the Cardputer into a **physical keyboard for NucleoOS Anima** — the sibling
NucleoOS that runs on the ESP32-P4 board (`D:\NucleoV2`, 7" touch panel). Everything typed
on the Cardputer lands in the P4's focused text field through its IME; in return the P4
streams live telemetry (free PSRAM, per-core CPU load, free SRAM, uptime) that the app
renders on the 240×135 screen. Both devices only need to be on the same LAN.

- Native app: `firmware/components/nucleo_app/app_keydeck.cpp` (launcher: **KeyDeck**, Connect)
- Receiver service on the P4: `D:\NucleoV2\components\nv_keydeck\` (+ `nv_ime_inject_*` in nv_ui)
- UX intentionally mirrors the **USB Keyboard** app (`app_usbkbd.cpp`): same "Digita"
  passthrough modal, same chips/echo. USB Keyboard types into a PC over the cable;
  KeyDeck types into NucleoV2 over Wi-Fi.

## Using it

1. Both devices on the same Wi-Fi. On the P4 the service is always on once Wi-Fi connects.
2. Open **KeyDeck** (Connect). It browses mDNS for `_keydeck._tcp`; no hit → falls back to
   the last saved IP (NVS `keydeck/ip`). Manual entry: **Server** tab → `M`.
3. **Monitor** tab shows free PSRAM (headline + bar), CPU0/CPU1 load bars, free SRAM,
   uptime. Values dim when telemetry is stale (>5 s).
4. `ENTER` opens the typing modal: every key is forwarded live. Arrows/Tab/Enter work;
   `Fn+Canc` = forward Delete, `Fn+`` ` = Esc to the P4, plain `` ` `` exits. If nothing is
   focused on the P4 the app shows *"Tocca un campo di testo sul P4!"* (server `ERR nofocus`).

## Protocol v1 (shared contract — keep in sync with `nv_keydeck.h`)

Plain TCP, **port 5588**, server = the P4, one client at a time (latest wins). Lines are
UTF-8, LF-terminated (CR stripped), ≤160 bytes. Discovery: the P4 advertises mDNS
`_keydeck._tcp` (hostname `nucleov2`, TXT `v=1`).

| client → server | server → client |
|---|---|
| `HELLO v1 <client-name> [PIN=<pin>]` | `WELCOME v1 <os> <host>` |
| `TXT <text…to EOL>` (literal insert) | `STAT ps_free=<B> ps_total=<B> sram_free=<B> cpu0=<pct> cpu1=<pct> up=<s>` every ~1 s |
| `KEY <NAME> [mods]` | `PONG` |
| `PING` (every ~2 s when idle) | `ERR nofocus` / `ERR badkey` / `ERR badcmd` / `ERR badpin` |

`KEY` names: `ENTER ESC BACKSPACE DELETE TAB LEFT RIGHT UP DOWN`. `mods` (decimal bitmask
1=shift 2=ctrl 4=alt 8=gui) is reserved — v1 receivers ignore it. `cpu0/cpu1` are `-1`
when the P4 build lacks `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`. The server drops a
client silent for >8 s; the client auto-reconnects with a ~2.5 s backoff.

**PIN pairing — wired in, DISABLED by default.** Out of the box the server is open
(LAN-only, same trust domain as the OTA updater): anyone on the LAN could type into a
focused field; nothing is readable back except the telemetry. To lock it, set the
`keydeck_pin` config key on the P4 (numeric string, NVS via nv_config): the server then
requires `HELLO … PIN=<pin>` before honoring anything, answers `ERR badpin` and drops
otherwise (no toast, no STAT for unauthenticated clients). On the Cardputer: **Server**
tab → `P` to store the PIN (NVS `keydeck/pin`, empty clears it); on `ERR badpin` the app
stops auto-retrying and points at the editor. The pin is re-read per connection, so
changing it applies to the next client without a reboot.

## Design notes

- **RAM posture (Cardputer):** no TLS, no exclusive mode. One socket, static buffers,
  networking pumped from `on_tick` (5 Hz) and the typing modal loop (~50 Hz) on the app
  task — no extra FreeRTOS task. mDNS is only used for the one-shot async browse
  (`mdns_query_async_new`), with `nucleo_discovery_resume()` first in case a security app
  stopped the responder.
- **P4 side:** `nv_keydeck` task owns the socket; every UI action goes through
  `lvgl_port_lock()` + `nv_ime_inject_text/key()`, which reuse the on-screen keyboard's
  own semantics (return-key actions, next-field Tab, one-shot shift, key tick sound).
- **Future transports (planned, in order):** USB — Cardputer as standard HID keyboard into
  the P4's OTG-HS Type-C port (reuses `nucleo_usbhid`; telemetry stays on Wi-Fi); BLE —
  only if RAM allows: NimBLE needs ~60 KB heap, the Cardputer runtime has ~18 KB free, so
  it would require an exclusive-mode posture or a Solo-style boot.
