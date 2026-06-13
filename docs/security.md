# Security: device pairing & session auth

NucleoOS requires **pairing** before it serves any user data. This closes the long-standing
gap where `/api/fs`, `/api/ota`, `/api/rec` and `/ws` were reachable by anyone on the LAN
despite `settings.security.require_pairing` being `true`.

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
| `/api/fs/*` (list/read/write/delete/mkdir) | `/` and all static assets (shell + app UIs) |
| `/api/ota` (firmware flash) | `/api/status` (discovery, e.g. NucleoConnect LAN scan) |
| `/api/rec/*` (microphone) | `/api/apps`, `/api/associations` (app catalog) |
| `/ws` (live event stream) | `/api/pair`, `/api/auth/status` (pairing itself) |

Static assets stay public so the shell can load and render the pairing overlay *before* the
browser is authenticated. The app catalog is public (it is not user data); user files are not.

## Tokens & the PIN

- **PIN**: 6 digits, regenerated on every boot (`esp_random`). Only *new* pairings need it.
- **Tokens**: persisted in the power-loss-safe config store (`/cfg/config/auth.json`,
  LittleFS — not the removable SD), bounded to the last 8 sessions. Because they persist,
  a paired browser keeps working across reboots without re-entering the PIN. The oldest
  session is evicted when the 9th client pairs ("unpair the rest" by clearing this file).
- **Brute-force guard**: after 5 wrong PINs the device refuses pairing for 30 s
  (`429 Too Many Requests`). A 6-digit space + lockout makes guessing impractical on a LAN.

## Firmware

`components/nucleo_auth` owns the PIN, the token store, the cookie check and the
`/api/pair` + `/api/auth/status` handlers. Protected handlers gate themselves with one line:

```c
static esp_err_t write_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);   // 401s and returns when the request has no valid session cookie
    …
}
```

`nucleo_auth_init()` runs in `app_main()` after the SD/`/cfg` mounts and before
`nucleo_httpd_start()`.

## Honest limitations / future work

- **HTTP, not HTTPS.** On a LAN AP this is acceptable, but the cookie travels in clear text;
  a hostile peer on the same network could sniff it. TLS on an ESP32-S3 AP is heavy and is a
  separate decision.
- **No per-app permissions.** A paired session can do anything the API allows. App sandboxing
  / capability scopes are future work.
- **App bundle signatures** (`settings.security.verify_bundle_signatures`, Ed25519) are still
  not enforced — see the roadmap. Pairing protects the *transport*; signing protects the
  *content* of installed apps. They are complementary.

## Simulator

`tools/serve-shell.mjs` mirrors all of the above (cookie sessions, PIN, lockout, the same
protected/public split) so the flow is verifiable on a PC. One deliberate difference: the
simulator exposes `GET /api/_dev/pin` to stand in for "read the code off the screen" during
automated testing. **The real firmware never serves the PIN over HTTP** — it only draws it on
the Cardputer display.
