# First-Run Setup Wizard (on-device OOBE)

On first boot NucleoOS runs a Windows-OOBE-style wizard **on the Cardputer's own
240×135 screen**, driven by the built-in keyboard. It is **persistent**: once finished it
never shows again (unless reset). This is what lets the user choose, on the device itself,
whether to join an existing Wi-Fi or create an Access Point for the client app.

## Flow

The wizard is **on rails**: every path ends on step 4, and nothing else in the OS is reachable before.

```
1. Language      "Language / Lingua": English, Italiano, Espanol, Francais, Deutsch — applied at once,
                 so every later screen (and the whole OS) is in the chosen language. Esc is inert.
2. Welcome       "Welcome to NucleoOS. Next: pick your Wi-Fi. Esc = use the hotspot."  [Enter]
3. Network       Settings ▸ Nearby networks in onboarding mode: the real scan list (signal bars,
                 Scan again), the real password field (size 2, last character shown for 1.5 s,
                 Tab shows/hides all of it) and the real join.
                 Locked: Tab, LEFT, the other pages, forget/prefer are inert.
                 Exits: a join (native, or from a browser on the hotspot), or Esc -> "Skip Wi-Fi?
                 You will use its hotspot." [Yes] -> per-device hotspot.
                 A failed join stays here: "Failed: check the password".
4. All set       "All set!" + "Connected to <ssid>" (or hotspot name + password), "Open in a
                 browser" <ip> (or 192.168.4.1) and the pairing PIN.  [Enter] -> the launcher.
                 setup.json (complete: true) is written when step 3 ends; never shown again.
```

Code: `nucleo_setup_run()` (steps 1–2) in `firmware/components/nucleo_setup/nucleo_setup.c`, then
`app_wifi.cpp` (`OB_NETS` → `OB_DONE`, steps 3–4). Guarantees:

- **Off the UI task.** Ending step 3 (hotspot up + setup persisted to /cfg, NVS and the SD) runs on
  Settings' "wifi" worker task with a spinner, after any scan in flight — never on the launcher's 8 KB
  main task, where the store chain overflowed the stack and rebooted the device on "Skip Wi-Fi".
- **Never abandoned.** If Settings is closed before step 4 by any path, the launcher re-opens the
  network step (at most every 2 s) as long as `nucleo_setup_onboarding()` is true.
- **Resumable.** Power loss before step 3 ends leaves `complete: false`: the next boot starts again
  at step 1.
- **No dead screens.** The modals draw into the shared back-buffer, an 8-bpp sprite or the panel
  (never into a failed sprite), and a short back-buffer still gets its hint bar.
- Device name: `nucleo-XXXX` from the MAC (renamable in Settings ▸ Device).

Host-rendered: `ui:shots` scenes `wizard.*` (language, welcome, inputs) and
`settings.onboard*` (list, password, skip confirm, locked Tab/LEFT, All set: joined / hotspot).

**Pairing PIN — implemented, but not a wizard step.** The 6-digit pairing PIN
([`security.md`](security.md)) is minted once on first boot (`esp_random`, persisted, stable across
reboots; `settings.security.pin` can pin a fixed one) by `nucleo_auth`, not by the wizard, and the
wizard shows it on its last screen (All set). After setup, read it on the device from any of: the **Connection** app
(`info`; its PIN row shows the code once the device has joined a Wi-Fi network), **Settings ▸ Device ▸ PIN**, the Control Center's
web-client shortcut (`Web <ip>   PIN <pin>`), or the **QR Code** app's pairing source.

**QR code — partly there.** The QR Code app's pairing source shows a QR of the web OS URL
(`http://<ip>` or `http://192.168.4.1`) with the PIN as text in the footer; the PIN itself is not
encoded, so the browser still asks for it. A QR / PIN screen inside the wizard remains future work.

**Time sync** is not a wizard step either: SNTP starts automatically whenever the device gets an IP
on a Wi-Fi network.

## Screen layout (240×135)

A title line, up to ~6 body lines, and a hint line at the bottom. Menus highlight the
selected row; lists scroll when longer than the viewport.

```
+------------------------------------------+
| NucleoOS  ·  Network                     |  title
|                                          |
|  > Connect to a Wi-Fi network            |  menu (selected)
|    Skip - use an Access Point            |
|                                          |
|                                          |
| [;/.] move   [enter] ok   [`] back       |  hint
+------------------------------------------+
```

## Keyboard mapping (Cardputer)

| Key | Action |
|---|---|
| `;` | move up |
| `.` | move down |
| `enter` | select / confirm |
| `del` (backspace) | delete char (text entry) |
| `` ` `` | back / cancel |
| printable | type (SSID names, password, device name) |

## Persistence

Two documents — `setup.json` (wizard result + hotspot) and `networks.json` (every Wi-Fi joined) —
are written by `firmware/components/nucleo_setup/setup_store.c` to three **independent** tiers,
each best-effort so one failure never blocks the others. Loads read them in this order and the
first tier that answers wins, and a copy recovered from NVS or the SD is re-written to every tier.
The wizard therefore also runs on a device with no SD card inserted.

| Tier | Where | Holds |
|---|---|---|
| 1. `/cfg` LittleFS (internal flash, power-loss-safe) | `/cfg/config/setup.json`, `/cfg/config/networks.json` | full document |
| 2. NVS, namespace `nucleocfg` | keys `setup`, `networks` | full document |
| 3. SD mirror | `/sd/system/config/setup.json`, `/sd/system/config/networks.json` | document **without secrets** |

Full documents (tiers 1–2):

```json
{ "complete": true, "mode": "sta", "ssid": "HomeWiFi", "device_name": "nucleo-01",
  "ap_ssid": "NucleoOS-1A2B", "ap_pass": "<random 12 chars>", "ap_open": false }

{ "seq": 9, "nets": [ { "ssid": "HomeWiFi", "pass": "<wifi password>", "prio": 2, "seq": 7 } ] }
```

The SD mirror is the same document with every `ap_pass` / `pass` member removed.

- `complete: true` makes `nucleo_setup_is_complete()` return true → the wizard is skipped.
- **Hotspot password** (`ap_creds.c`): on first use the device mints a per-device SSID and a
  random 12-char WPA2 password. The user can set their own (8–63 chars; 1–7 is rejected because
  the driver would silently start an open AP) or clear it for an **open** hotspot. That choice is
  `ap_open: true` — its own flag, because an empty `ap_pass` alone also means "not minted yet",
  and firmware before this flag re-minted a password over the open choice on every AP restart.
  `ap_open` is not a secret, so it stays in the SD mirror; a stale `ap_open` next to a stored
  password is ignored (the password wins), and a document without the flag (older firmware)
  counts as not open.
- On every later boot, `nucleo_setup_apply_network()` reads the mode: for STA it brings the AP up at
  once (the device is reachable immediately) and lets the background Wi-Fi supervisor join the best
  saved network; otherwise it starts the AP.
- **No password is ever written to the card.** Wi-Fi and hotspot passwords live only on internal
  flash (`/cfg` + NVS; esp_wifi also keeps the current station credentials in its own NVS). The
  card is removable, unencrypted FAT and served to paired clients by `/api/fs/read`, so a copy
  there would hand every saved password to whoever pulls it. See `docs/security.md`.
- **Why the mirror still exists:** it is the only copy that survives a wipe of the internal
  flash (e.g. a reflash through a launcher that erases NVS, or a firmware without our `cfg`
  partition). After such a wipe, with the card still inserted, the device recovers
  `complete`, the device name, the mode, the hotspot SSID and every saved SSID + priority —
  **the wizard does not re-run**. What it cannot recover are the passwords:
  - the hotspot mints a fresh random WPA2 password, shown in Device Info and the Wi-Fi app
    (a hotspot the user had made open stays open — `ap_open` survives on the card);
  - each saved secured network appears as known but passwordless (`has_pass: false` in
    `GET /api/wifi/known`) — the native Wi-Fi app opens the password editor for it, the web Wi-Fi
    scanner asks for the password instead of offering "blank = use saved", a join without one is
    refused before the radio is touched (the current link is not dropped), and the auto-join
    supervisor skips it until a password is entered (no pointless retries).
- **Cards written by older firmware** (which mirrored the full documents) are scrubbed: on the
  first load the store sees a password member on the card and the loader re-saves, rewriting the
  mirror without it (tried once per boot). If the card is the *only* copy (legacy SD-only layout,
  or a flash wipe), the passwords are first migrated into `/cfg` + NVS, then stripped from the
  card. If the card can't be rewritten (full, write error) the dirty copy is deleted instead —
  but only once the passwords are safe on `/cfg` or NVS. An older firmware's orphan
  `<file>.json.tmp` holding a password is removed on load. The card probe streams the file
  through a 64-byte stack buffer: no heap copy on the boot path. FAT deletion does not overwrite
  the old sectors, so treat passwords stored on a card used with older firmware as exposed if
  that card may have left your hands.
- **Why not encrypt the SD copy instead:** a key kept in NVS dies in the very wipe that makes
  the mirror useful; a key derived from the MAC is public (the soft-AP broadcasts it as its
  BSSID); an eFuse key is irreversible per-device provisioning. None of them buys a working
  recovery path, so the mirror simply leaves the secrets out.
- Verified on the PC by `npm run setupstore:test` (part of the ANIMA gate): the real
  `setup_store.c` + `ap_creds.c` + ESP-IDF's cJSON: fan-out, read order and the reset contract
  below, every tier combination, legacy scrub, an allocation failure at every malloc of a save,
  and the hotspot lifecycle (an open hotspot stays open across AP restarts, reboots and flash-wipe
  recovery; a secured one is re-minted).

## Reset

The wizard runs again only when no tier holds a `setup.json` with `complete: true`. Because `/cfg` and
NVS are read before the SD mirror (and a copy recovered from them is written back to the SD), deleting
the SD copy alone — e.g. from File Commander — does **not** re-arm it, and neither does wiping the
internal flash alone: the SD mirror then restores `complete: true` (without any password).
Settings ▸ Reset does it properly; both rows need ENTER ×3 and then reboot:

- **Reset settings** ("Network, prefs, logs. Files kept") calls `nucleo_setup_factory_reset()`:
  `setup.json` and `networks.json` are erased from `/cfg`, NVS (`nucleocfg` keys `setup` / `networks`)
  and the SD mirror, and esp_wifi's own copy of the STA credentials is cleared (`esp_wifi_restore()`,
  which also stops Wi-Fi at once, then its `nvs.net80211` namespace is erased directly — the only path
  in a Wi-Fi-skipped boot, where the driver is not up). Saved networks, the hotspot SSID and password,
  the device name, the mode and `complete` are all gone: the wizard runs on the next boot and the
  hotspot gets a freshly minted password. It also clears every Settings pref — the SD `system/config`
  (`settings.json`: language, brightness, volume, voice; `anima_ui.json`; `screensaver.json`; the
  apps' own UI state), the theme (`/cfg/config/theme.json` + `/sd/apps/theme.cfg`), read-aloud
  (`/sd/data/tts/speak.cfg`), and the NVS-backed Web handoff and Bluetooth-at-boot toggles (back to
  their defaults) — plus `system/sessions` and the logs (`system/log`, `system/logs`,
  `net_trace.txt`, `boot_trace.txt`). User content stays: files, and the calendar events and alarms
  that live beside the config (`system/config/calendar.json`, `alarm.json`).
  **Pairing is kept**: the PIN and web sessions are not network config, a paired browser should not
  have to re-pair after a Wi-Fi fix, and Settings ▸ Device ▸ Web sessions revokes them on its own.
- **Factory reset** ("Also keys and ANIMA data") does all of the above, plus
  `nucleo_auth_factory_reset()` — the pairing PIN and every session, from `/cfg/config/auth.json` and
  the `nucleoauth` NVS namespace (every browser must pair again; the next boot mints a new PIN) — the
  SMTP accounts and their app passwords (`nucleo_mailcfg_erase_all()`, `mail` NVS) and the sent-mail
  log (`system/mail`), the Key deck server address + PIN (`nucleo_keydeck_forget()`, `keydeck` NVS),
  the rest of `/cfg/config` (launcher pins and recents), and on the SD `system/keys`, ANIMA's learned
  data and session files, `config`, `backups` and `journal`. User files, calendar and alarms stay here
  too — wipe the card for those.
- Both **seal** their stores before erasing: from that moment every save is refused until the reboot.
  Without it a Wi-Fi supervisor join or the hotspot minting its default password in the gap before
  `esp_restart()` would write the document straight back. esp_wifi is switched to RAM storage for the
  same reason. The reset functions return false when a tier survived (erase could not be verified,
  NVS down, or a racing save outlived the seal's 2 s wait). The row then does **not** reboot — a
  reboot would heal the survivor back and look reset — but shows "Reset incomplete: try again";
  every step is idempotent, so ENTER ×3 again retries. Details are in the log (`reset: … still present`).
- **Not erased by either**: FIDO passkeys (`fido` NVS) — wiping them locks the owner out of every
  account the key protects, so that stays a deliberate act in the Passkeys app (`POST /api/fido/reset`)
  — and the update-check state (`nucupd`, no secrets). A full flash erase clears everything.

## Architecture

The wizard logic (`nucleo_setup`) is hardware-independent and talks to a tiny UI interface
(`nucleo_ui`: message / menu / text input / key polling). The on-device implementation of
that interface uses **M5GFX / M5Unified** for the display, keyboard and fonts — the
standard, battle-tested path for Cardputer UIs — keeping risky low-level driver code out
of the OS core. See `firmware/components/nucleo_ui`.
