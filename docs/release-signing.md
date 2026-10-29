# Release signing: firmware and SD content

Everything the device takes from the release site is signed offline with a NucleoOS release key, and the
device refuses what does not verify. Before this, the firmware updater and the SD-content installer trusted
HTTPS plus a `SHA256SUMS` / manifest fetched **from the same site**: that proves a clean transfer, not who
built the file, so anyone able to publish on the site (a stolen GitHub token, a compromised CI run) could
ship code to every device. The SD content matters as much as the firmware: it carries the web OS, which runs
same-origin with the device API.

| What | Signed object | Where the signature travels | Checked by |
|---|---|---|---|
| Firmware (native updater, browser updater, `ota.ps1`, rescue page) | the app image `nucleoos.bin` | a 128-byte block **appended** to `nucleoos-*-ota.bin` | `ota_post` (`nucleo_httpd.c`), `install_task` (`nucleo_update.c`) |
| SD content (web OS, bundled apps, ANIMA brain) | `sd-manifest.txt` (pins every file's SHA-256) | `sd-manifest.sig` beside it (`sd/<x.y.z>/` on Pages) | `sdc_engine_run` step 3b, before any payload byte |
| Store apps | catalog + packages | store's own scheme (`docs/native-apps.md`) | `nucleo_store` (unchanged) |

## Format (`firmware/components/nucleo_fwsig`)

Block (128 bytes): `"nucleoos-sig-v1\n"`, key id (16 hex = first 8 bytes of SHA-256 of the uncompressed P-256
public key), DER length (u16 LE), ECDSA P-256/SHA-256 DER signature, zero padding. Signed message:

```
<domain>\n<version>\n<sha256 hex of the signed bytes>\n<their size>\n
```

Domains `nucleoos-cardputer-ota-v1` (version = the image's app-descriptor version, read back from the slot)
and `nucleoos-cardputer-sd-v1` (version = the manifest header's tag), so a signature for one can never pass
as the other, nor for another version. The device hashes while it streams and holds the last 128 bytes back,
so nothing is buffered; the image is never marked bootable unless the block verifies.

## Updating stays possible for every device

- **Old firmware** (without the check) writes the appended block into the OTA slot past the image, where
  nothing reads it, and never asks for `sd-manifest.sig`: signed releases install on it exactly as before.
  `SHA256SUMS` lists the signed file, so their checksum check still passes.
- **New firmware** needs the signature; a release published by CI but not signed yet fails cleanly ("Release
  not signed yet: retry later") and the current firmware keeps running. USB (web flasher) always works.
- Both keys are trusted by every firmware: if the primary is lost or leaks, a release signed with the
  **backup** key reaches every device and can carry a new primary.

## Keys

| | private (never in the repo or CI) | public (compiled in) |
|---|---|---|
| primary | `%USERPROFILE%\.nucleo\cardputer-ota-signing-key.pem` | `nucleo_fwsig/keys/primary.pub.pem` |
| backup | `%USERPROFILE%\.nucleo\cardputer-ota-signing-backup-key.pem` — move it **offline** | `nucleo_fwsig/keys/backup.pub.pem` |

`fwsig_keys.c` is generated (`python tools/fw_sign.py keys`). Lock-out guard: `fw_sign.py` signs only an image
that embeds the primary public key, so a build that would stop trusting it can never ship signed.

Copies: `python tools/keys_backup.py create <dir> <dir> ...` writes one AES-256-GCM bundle (scrypt from a
passphrase typed at the terminal) of every key in `~/.nucleo` to each folder and re-opens each copy to verify
it; `verify` / `restore` read it back. Keep copies on at least two separate offline media plus one off-site,
and the passphrase apart from them.

## Release flow

1. Tag as usual: CI builds and publishes the release (no keys in CI).
2. On the PC with the key: `python tools/fw_sign.py release vX.Y.Z` — downloads the OTA images and the SD
   manifest, checks they are the files CI published (`SHA256SUMS`), appends the firmware block, writes
   `-sd-manifest.sig`, rewrites `SHA256SUMS`, uploads them back and re-runs Pages. `--dry-run` to look first.
3. Local builds: `tools/ota.ps1` signs `nucleoos.bin` into `nucleoos-signed.bin` before uploading.

Gate: `npm run fwsig:test` (the real `fwsig.c` against mbedTLS: chunking, tampering, wrong version/domain,
stranger key, forged key id, missing block; key table matches the public PEMs). The SD-content engine's
signature step is covered by `npm run sdcontent:e2e` (unsigned release, manifest changed after signing,
missing hook: nothing is written).

## Not covered (hardware)

ESP32-S3 secure boot and flash encryption are off: someone holding the device can flash anything over USB and
read the flash. Turning them on burns eFuses irreversibly, needs a USB flash per unit, and would break the
M5Launcher and web-flasher installs, so it is a per-device choice for owners who want it, not an update.
