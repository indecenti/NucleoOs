# Reference material

Third-party projects NucleoOS studied for interoperability and design comparison.

These are **not** part of NucleoOS and are **not** built or shipped. To keep this repository
free of vendored third-party trees (and free of license mixing — see the licenses below), the
full source is **not** checked in. Clone the upstreams here when you need to read them; this
directory is otherwise empty on a fresh checkout and ignored by git.

| Project | Upstream | License | Why it is referenced |
|---|---|---|---|
| Bruce | https://github.com/pr3y/Bruce | AGPL-3.0 | ESP-NOW file-share wire format. NucleoOS's "Bruce mode" (`firmware/components/nucleo_link/`) is wire-compatible with Bruce's `EspConnection::Message`; the evolved Nucleo↔Nucleo link adds ACK/retransmission/resume on top. |
| ESP-Claw | https://github.com/esp-claw/esp-claw | Apache-2.0 | Comparison point for the ESP32 agent/UI approach. |

## Fetching a reference locally

```bash
git clone https://github.com/pr3y/Bruce            reference/bruce
git clone https://github.com/esp-claw/esp-claw     reference/esp-claw
```

> **License note.** Bruce is AGPL-3.0; NucleoOS ships under PolyForm Noncommercial (see the
> repository `LICENSE`). NucleoOS does **not** incorporate Bruce source — it only implements
> a compatible wire format documented from the public protocol. Keep any locally cloned Bruce
> tree out of NucleoOS builds and out of any commercial distribution.
