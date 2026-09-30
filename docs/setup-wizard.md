# First-Run Setup Wizard (on-device OOBE)

On first boot NucleoOS runs a Windows-OOBE-style wizard **on the Cardputer's own
240×135 screen**, driven by the built-in keyboard. It is **persistent**: once finished it
never shows again (unless reset). This is what lets the user choose, on the device itself,
whether to join an existing Wi-Fi or create an Access Point for the client app.

## Flow

```
1. Language           "Language / Lingua": English / Italiano — applied instantly, so every
                      later screen (and the whole OS) is already in the chosen language
2. Welcome            "Welcome to NucleoOS. Let's set up your device."  [Enter to begin]
3. Network            ▸ Connect to a Wi-Fi network
                      ▸ Skip - use an Access Point
3a. (Connect) Scan    pick SSID from a scrolled list
3b. (Connect) Password masked text entry via the keyboard -> "Connected!" + http://<ip>/
3c. (AP)              on Skip, a failed join, no networks found or back: per-device SSID
                      "NucleoOS-XXXX" + random password + http://192.168.4.1/ (client connects here)
4. Device name        default "nucleo-01"  (becomes hostname / nucleo-01.local)
5. Done               "Setup complete!" + Wi-Fi/AP line, "Open: <url>/", "Win app: <url>/downloads/"
                      -> writes setup.json (complete: true), never shown again
```

Code: `nucleo_setup_run()` in `firmware/components/nucleo_setup/nucleo_setup.c`. The language step
offers English and Italian only; Spanish, French and German are picked afterwards in Settings
(native or web) — see [`i18n.md`](i18n.md).

**Pairing PIN — implemented, but not a wizard step.** The 6-digit pairing PIN
([`security.md`](security.md)) is minted once on first boot (`esp_random`, persisted, stable across
reboots; `settings.security.pin` can pin a fixed one) by `nucleo_auth`, not by the wizard, and the
wizard's screens never show it. After setup, read it on the device from any of: the **Connection** app
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

Saved as `setup.json` to **three independent tiers**, so it survives any install (even a firmware
loaded without the custom partition table, and a device with no SD card): `/cfg/config/setup.json`
on the internal LittleFS (primary), an NVS copy, and an SD mirror at `/system/config/setup.json`.
They are read in that order — the first one that answers wins — and a copy recovered from NVS or the
SD is re-written to every tier. The wizard therefore also runs on a device with no SD card inserted.

```json
{ "complete": true, "mode": "sta", "ssid": "HomeWiFi", "device_name": "nucleo-01",
  "ap_ssid": "NucleoOS-XXXX", "ap_pass": "<per-device hotspot password>" }
```

- `complete: true` makes `nucleo_setup_is_complete()` return true → the wizard is skipped.
- **Joined Wi-Fi networks** (SSID + password + priority, up to 16) live in a sibling `networks.json`,
  persisted to the same three tiers (`/cfg/config/networks.json`, NVS, and the SD mirror
  `/system/config/networks.json`); esp_wifi also keeps the current credentials in NVS
  (`WIFI_STORAGE_FLASH`). Note that the SD mirrors of both files hold the Wi-Fi and hotspot
  passwords in plaintext.
- On every later boot, `nucleo_setup_apply_network()` reads the mode: for STA it brings the AP up at
  once (the device is reachable immediately) and lets the background Wi-Fi supervisor join the best
  saved network; otherwise it starts the AP.

The three-tier store is `firmware/components/nucleo_setup/setup_store.c` — plain C, host-tested
by `npm run setupstore:test` (fan-out, read order, and the reset contract below).

## Reset

The wizard runs again only when no tier holds a `setup.json` with `complete: true`. Because `/cfg` and
NVS are read before the SD mirror (and a copy recovered from them is written back to the SD), deleting
the SD copy alone — e.g. from File Commander — does **not** re-arm it. Settings ▸ Reset does it
properly; both rows need ENTER ×3 and then reboot:

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
