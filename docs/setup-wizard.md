# First-Run Setup Wizard (on-device OOBE)

On first boot NucleoOS runs a Windows-OOBE-style wizard **on the Cardputer's own
240×135 screen**, driven by the built-in keyboard. It is **persistent**: once finished it
never shows again (unless reset). This is what lets the user choose, on the device itself,
whether to join an existing Wi-Fi or create an Access Point for the client app.

## Flow

```
1. Welcome            "NucleoOS — first-time setup"  [Enter to start]
2. Storage            shows detected SD: "exFAT · 64 GB · XX GB free"
3. Network mode       ▸ Join a Wi-Fi network
                      ▸ Create an Access Point
4a. (Join) Scan       pick SSID from a scrolled list
4b. (Join) Password   masked text entry via the keyboard
5a. (AP) Show AP       per-device SSID "NucleoOS-XXXX" + password (client connects here)
6. Device name        default "nucleo-01"  (becomes hostname / nucleo-01.local)
7. Done               "Connect from your PC/phone:" URL + nucleo-01.local
                      -> writes setup.json, never shown again
```

Optional steps reserved for later (kept out of v1 to stay light): NTP time sync (when on
Wi-Fi), and a pairing PIN / QR code on screen.

## Screen layout (240×135)

A title line, up to ~6 body lines, and a hint line at the bottom. Menus highlight the
selected row; lists scroll when longer than the viewport.

```
+------------------------------------------+
| NucleoOS  ·  Network                     |  title
|                                          |
|  > Join a Wi-Fi network                  |  menu (selected)
|    Create an Access Point                |
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

Saved to `/system/config/setup.json` on the SD card:

```json
{ "complete": true, "mode": "sta", "ssid": "HomeWiFi", "device_name": "nucleo-01" }
```

- `complete: true` makes `nucleo_setup_is_complete()` return true → the wizard is skipped.
- **Wi-Fi credentials are NOT stored in this file.** They are persisted by esp_wifi in NVS
  (`WIFI_STORAGE_FLASH`), so the password never sits in plaintext on the card.
- On every later boot, `nucleo_setup_apply_network()` reads the mode and brings up STA
  (credentials from NVS) or the AP automatically.

## Reset

Deleting `setup.json` (from File Commander or recovery) re-arms the wizard on next boot.

## Architecture

The wizard logic (`nucleo_setup`) is hardware-independent and talks to a tiny UI interface
(`nucleo_ui`: message / menu / text input / key polling). The on-device implementation of
that interface uses **M5GFX / M5Unified** for the display, keyboard and fonts — the
standard, battle-tested path for Cardputer UIs — keeping risky low-level driver code out
of the OS core. See `firmware/components/nucleo_ui`.
