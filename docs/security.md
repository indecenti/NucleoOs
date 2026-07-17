# Security: device pairing & session auth

NucleoOS requires **pairing** before it serves any user data, runs any state-changing
action, reaches paid cloud, or discloses device internals. This closes the long-standing
gap where `/api/fs`, `/api/ota`, `/api/rec` and `/ws` were reachable by anyone on the LAN
despite `settings.security.require_pairing` being `true`.

The pre-auth surface is deliberately minimal: **static assets + `/api/status` (discovery) +
the app catalog + the pairing endpoints, and nothing else.** Everything that changes state,
costs money/heap, or fingerprints the device is paired-only — including the diagnostics
(`/api/diag`, `/api/logs`, `/api/heap`, `/api/cpu`, `/proc/*`), the assistant (`/api/anima*`,
which spawns a 30 KB worker and can call a paid cloud model), the Wi-Fi scan, and the
device-clock / TTS write actions.

## Model

Pairing proves **physical proximity**: the device shows a 6-digit PIN on the Cardputer
screen (Connection app → `Pair`), and a browser proves it can see that screen by entering
the code — the same idea as pairing a Chromecast or a Bluetooth speaker.

```
browser                         device
  │  GET /api/auth/status   ───────▶  { required:true, paired:false }
  │  (overlay shown)
  │  POST /api/pair {pin}   ───────▶  PIN matches?
  │                          ◀─────── Set-Cookie: nucleo_session=…; HttpOnly
  │  every later request carries the cookie automatically (fetch + WS + iframes)
```

The session token is delivered as an **HttpOnly cookie**. The browser then attaches it to
every same-origin request on its own — including requests made from inside **app iframes**
and the **WebSocket handshake** — so the 18 apps need **zero changes** to become
authenticated. JavaScript can't read the cookie (HttpOnly), which limits XSS token theft.

## What is protected vs public

| Protected (needs a paired session) | Public (no auth) |
|---|---|
| `/api/fs/*` (list/read/write/delete/mkdir/move) | `/` and all static assets (shell + app UIs) |
| `/api/ota` (firmware flash), `/api/reboot` | `/api/status` (discovery, e.g. NucleoConnect LAN scan) |
| `/api/rec/*` (microphone), `/ws` (live events) | `/api/apps`, `/api/associations` (app catalog) |
| `/api/anima`, `/api/anima/verify`, `/api/anima/l1`, `/api/anima/caps` (assistant: paid cloud + 30 KB worker) | `/api/pair`, `/api/auth/status` (pairing itself) |
| `/api/proxy`, `/api/llm`, `/api/transcribe` (relays / paid cloud) | `/api/mail/presets` (static SMTP host/port list) |
| `/api/mail/accounts`, `/api/mail/send` (stored creds) | |
| `/api/wifi/scan`, `/api/wifi/known`, `/api/wifi/join`, `/api/wifi/forget` | |
| `/api/ir/*`, `/api/gpio/*`, `/api/link/*`, `/api/fido/*` (peripherals) | |
| `/api/diag`, `/api/logs`, `/api/heap`, `/api/cpu`, `/proc/*` (diagnostics) | |
| `/api/lang` **POST**, `/api/tts` **POST**, `/api/time/set` (writes) | `/api/lang` GET, `/api/tts` GET (read-only status) |
| `/api/voice/*`, `/api/anima/verify`, display/app control routes | |
| `/api/unpair` (revoke sessions) | |

Static assets stay public so the shell can load and render the pairing overlay *before* the
browser is authenticated. `/api/status` stays public for LAN discovery (NucleoConnect); the app
catalog is public (it is not user data). User files, actions, diagnostics and paid/heavy ops are
not. Dual GET/POST handlers (`/api/lang`, `/api/tts`) gate only the **POST** — the read-only GET
stays public, the write needs a session. The rule of thumb: **if it changes state, costs
money/heap, or reveals more than "a NucleoOS device exists here", it is paired-only.**

## Tokens & the PIN

- **PIN**: 6 digits, **stable across reboots** — minted once with `esp_random` on first boot,
  then persisted so the same code shows every boot (a fresh code each boot would force a re-pair
  after every restart). A fixed memorable PIN can be pinned in `settings.security.pin`. Only
  *new* pairings need it. Persisted to `/cfg/config/auth.json` with an NVS fallback tier (so it
  survives even a launcher install that lacks our `/cfg` partition) — never mirrored to the
  removable SD.
- **Tokens**: 48-hex (24 random bytes), delivered as an `HttpOnly; SameSite=Lax` cookie, stored
  alongside the PIN in the same power-loss-safe tier (not the SD), a bounded ring of the last
  sessions. Because they persist, a paired browser keeps working across reboots without
  re-entering the PIN; the oldest session is evicted when the ring fills. Compared
  **constant-time** so a network attacker can't time-probe them.
- **Idle-TTL (sliding)**: a session unused for **90 days** is expired server-side and dropped, so a
  cookie sniffed but never replayed dies on its own and abandoned browsers don't leave a live token
  forever. Actively-used sessions never expire — each request re-stamps "last seen" (at most once a
  day, to keep the hot auth path write-free). TTL is only enforced once the wall clock is really set
  (`time() > 2023-01-01`); a token minted on a cold clock starts its window on first clocked use.
  Each token's last-seen persists next to it (`{t,s}` in `auth.json`; legacy bare-string tokens load
  fine and start their window on next use). TTL bounds **idle** tokens; an actively-abused token is
  killed with revoke, below.
- **Revoke** (`POST /api/unpair`, paired-only): the leaked-cookie recovery. `{"scope":"others"}`
  (default) logs out every *other* client and keeps the caller in; `{"scope":"all"}` wipes every
  session so everyone re-pairs with the PIN. `GET /api/auth/status` reports the live `sessions` count
  so a UI can surface "N active sessions" + a revoke button. (Clearing `auth.json` on the SD-less
  config tier is the physical-access equivalent.)
- **Brute-force guard**: **per source-IP**, not global (a global counter let one hostile client
  lock out pairing for everyone — a trivial remote DoS). After 5 wrong PINs from an IP the device
  refuses that IP with an **exponential backoff** (30 s, 60 s, 120 s … capped ~16 min;
  `429 Too Many Requests`). A 6-digit space + escalating per-IP lockout makes guessing impractical
  on a LAN.

## Firmware

`components/nucleo_auth` owns the PIN, the token store (with per-token last-seen for the
idle-TTL), the cookie check, and the `/api/pair` + `/api/auth/status` + `/api/unpair` handlers.
Protected handlers gate themselves with one line:

```c
static esp_err_t write_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);   // 401s and returns when the request has no valid session cookie
    …
}
```

`nucleo_auth_init()` runs in `app_main()` after the SD/`/cfg` mounts and before
`nucleo_httpd_start()`.

## Honest limitations / future work

- **HTTP, not HTTPS — the dominant residual risk.** Everything is clear text on the LAN. Pairing
  gates *who* may act, but on a network you don't control a passive sniffer on the same L2 segment
  can capture the PIN in the `POST /api/pair` body *and* the `nucleo_session` cookie on every later
  request (replay ⇒ full access). `HttpOnly` blocks XSS token theft and `SameSite=Lax` blocks CSRF,
  but neither stops on-wire interception, and the cookie can't be `Secure` without TLS. **Operational
  mitigation until TLS exists: on an untrusted network, use the device's own WPA2 soft-AP (encrypted
  L2) instead of joining a shared LAN.** TLS on a PSRAM-less ESP32-S3 is heavy and is a separate
  decision.
- **Session TTL + revoke — done** (see *Tokens & the PIN*): a 90-day sliding idle-TTL auto-expires
  unused/leaked-but-unreplayed sessions, and `POST /api/unpair` logs out other/all sessions on
  demand. The **web UI** ships too: Settings → Device → *Security & sessions* shows the live
  `sessions` count with "Revoke other sessions" / "Revoke all" buttons (and Ctrl+K palette actions).
  Still open: a **native** "revoke all" menu item on the device screen — the highest-trust,
  physical-access recovery for when every web session may be compromised.
- **OTA accepts any `0xE9` image.** `/api/ota` is paired-only, but it does not verify a signature —
  a paired client can flash arbitrary firmware. Secure Boot v2 + signed OTA is the fix; it is a
  flash-config decision (irreversible eFuse) and is deliberately not enabled yet.
- **No per-app permissions.** A paired session can do anything the API allows. App sandboxing
  / capability scopes are future work.
- **App bundle signatures** (`settings.security.verify_bundle_signatures`, Ed25519) are still
  not enforced — see the roadmap. Pairing protects the *transport*; signing protects the
  *content* of installed apps. They are complementary.
- **`/api/status` still discloses the joined SSID + device internals** (it must stay public for
  discovery). On a shared LAN this is low-value (a peer already sees the AP), but trimming it to
  the bare discovery fields is a candidate hardening if the taskbar/status dependencies are moved
  to the gated `/api/diag`.

## Simulator

`tools/serve-shell.mjs` mirrors all of the above (cookie sessions, PIN, lockout, the same
protected/public split) so the flow is verifiable on a PC. One deliberate difference: the
simulator exposes `GET /api/_dev/pin` to stand in for "read the code off the screen" during
automated testing. **The real firmware never serves the PIN over HTTP** — it only draws it on
the Cardputer display.
