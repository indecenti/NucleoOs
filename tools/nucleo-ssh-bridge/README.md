# nucleo-ssh-bridge

The small self-hosted companion that lets the NucleoOS web **SSH** app reach **real** remote SSH hosts.

## Why it exists
Browsers cannot open raw TCP sockets (SSH is TCP/22), and the Cardputer (ESP32-S3, no PSRAM, ~18 KB
heap) cannot do SSH crypto. So this tiny process — running on **your** machine — terminates the
WebSocket from the browser and opens the actual SSH connection with [`ssh2`](https://github.com/mscdex/ssh2).
The Cardputer only serves the app's static files; the live SSH traffic never touches it.

**No third-party / cloud service.** Everything runs on your LAN. Credentials pass
browser → bridge → host and are held only for the life of the session (never written to disk).

## Run it (on your PC / Raspberry Pi / NAS / the target host)

```sh
cd tools/nucleo-ssh-bridge
npm install
node index.js            # prints a TOKEN — copy it into the NucleoOS SSH app
# or: NUCLEO_SSH_TOKEN=mysecret NUCLEO_SSH_PORT=8022 node index.js
```

Then in NucleoOS open **SSH**, set the bridge to `ws://<this-machine>:8022` (default `ws://localhost:8022`
when the bridge runs on the same PC as the browser), paste the **token**, add a host, connect.

### Package as a single executable (optional, zero-Node for end users)
```sh
npx pkg . --targets node18-win-x64,node18-linux-x64,node18-macos-x64
```
Ship the resulting binary; double-click / run it. (mDNS auto-discovery works if `bonjour-service` is installed.)

## Security model (read this)
- **Token-gated:** the first WS message must carry the token, or the connection is dropped.
- **LAN, unencrypted by default:** `ws://` is plaintext. On a trusted LAN that's typical; for
  untrusted networks put it behind a TLS reverse-proxy and use `wss://`, or tunnel over a VPN.
- **Host keys:** v1 is TOFU-lite — it **accepts** the host key but reports its SHA-256 fingerprint
  to the app so you can eyeball it. Strict pinning/known_hosts is a planned improvement.
- Prefer **key-based auth**; avoid storing passwords in the app's saved profiles.
- Bind to `127.0.0.1` (`NUCLEO_SSH_BIND=127.0.0.1`) if the browser runs on the same machine, so the
  bridge isn't reachable from the rest of the LAN.

## Protocol
One WebSocket == one SSH session. Control = JSON text frames; shell I/O = binary frames.
See the header comment in `index.js`. The `exec` message (run one command, capture stdout/stderr/exit)
is the **agent seam**: a future NucleoOS agent can drive a host non-interactively through the same bridge.
