# NucleoOS — User Guide

The manual for people who have a **M5Stack Cardputer** (original or **Cardputer ADV**) and want
to install and use NucleoOS. It covers getting the OS onto the device and living with it day to
day. For the feature tour see the [README](../README.md); for how the system works internally see
the [docs index](README.md).

> **One idea to hold onto.** NucleoOS is *two* interfaces sharing one device: the **native UI** on
> the Cardputer's 240×135 screen, and **NucleoOS Web** — a desktop-class console your browser
> renders, served by the device over your Wi-Fi. Heavy work happens in the browser; the tiny
> microcontroller stays light.

---

## 1. What you need

- A **M5Stack Cardputer** (original or ADV). One firmware image runs on both — it auto-detects the
  board at boot.
- A **USB-C** cable.
- A **microSD** card, formatted **FAT32** (see [step 2](#step-2--prepare-the-microsd-card)).
- A **desktop Chrome or Edge** browser to flash. (Web Serial is not available in Firefox, Safari,
  or on phones.)
- Wi-Fi you can join, or nothing at all — the device can create its own network instead.

---

## 2. Install

Installing NucleoOS is **two separate things**: flashing the *firmware* to the device, and putting
the *SD payload* (all the apps and data) on the card. The `-sd.zip` is **not** firmware — it never
goes into a flasher.

### Step 1 — Flash the firmware (in your browser)

1. On a desktop, open the web flasher: **<https://indecenti.github.io/NucleoOs/>** in **Chrome** or
   **Edge**.
2. Plug the Cardputer in with USB-C.
3. Click **Connect & install NucleoOS**, pick the Cardputer's serial port, and confirm. It writes
   the latest firmware for you.
4. When it finishes, **press the Cardputer's reset button** (or unplug/replug USB) to start it.

If something goes wrong, you can usually just flash again from the same page. Installing a custom
firmware is at your own risk and may affect the manufacturer's warranty — see the
[legal notes](../README.md#legal--responsible-use). (Prefer a cable? See [`releasing.md`](releasing.md)
for the serial-flash path.)

### Step 2 — Prepare the microSD card

NucleoOS serves all its apps and data **from the card**, so this step is required.

1. Download the SD payload: **`nucleoos-latest-sd.zip`** from the
   [latest release](https://github.com/indecenti/NucleoOs/releases/latest).
2. Format a microSD as **FAT32**. (A large card may come as exFAT; NucleoOS can read exFAT if the
   firmware was built with it, but **FAT32 is the safe choice** — reformat if unsure. See
   [`storage.md`](storage.md).)
3. **Extract the zip to the card's root.** You should end up with these folders at the top level:
   `/apps` `/www` `/system` `/data`.
4. Insert the card into the Cardputer **before powering it on**.

> **Reminder:** the `-sd.zip` goes on the microSD, never into the flasher. The flasher installs
> only the firmware image.

---

## 3. First boot

On first power-up the Cardputer runs a short on-screen wizard, driven by its own keyboard. It runs
only once (until you reset the device). See [`setup-wizard.md`](setup-wizard.md).

1. **Language** — pick your UI language.
2. **Storage** — it shows the detected SD (type, size, free space).
3. **Network** — choose one:
   - **Join a Wi-Fi network** — pick your SSID from the scan, type the password on the keyboard.
   - **Create an Access Point** — the device starts its own network instead.
4. **Device name** — defaults to `nucleo-01` (this becomes the hostname, e.g. `nucleo-01.local`).
5. **Done** — the screen shows the **address to open from your PC or phone**.

**If you joined Wi-Fi:** the screen shows an **IP address**. Open it in a browser on the **same
Wi-Fi**.

**If you skipped Wi-Fi (or chose Access Point):** the Cardputer starts its own network named
**`NucleoOS-xxxx`** with a password shown on screen. Connect a phone or PC to that network, then
open the address displayed.

---

## 4. Open the web desktop & pair

NucleoOS Web is the full workstation — file manager, spreadsheet, Paint, games, monitor, settings.

1. On a device on the **same network**, open the **IP address** (or `http://<device-name>.local`)
   the Cardputer showed you.
2. The first time, the page asks you to **pair**. This proves you can physically see the device.
3. On the Cardputer, find the **6-digit PIN**. Two easy ways:
   - Press **TAB** to open the **Control Center**, move to the **Web client** shortcut — the bottom
     line shows the **IP and the pairing PIN**; or
   - open the **Connection** app and choose **Pair**.
4. Type that PIN into the browser. You're in — the session is remembered (an `HttpOnly` cookie), so
   you won't re-enter it on that browser after reboots.

Notes on pairing (details in [`security.md`](security.md)):

- The PIN is **stable across reboots** — the same 6 digits show every time; only *new* browsers
  need it.
- After 5 wrong PINs from one device, that device is locked out with an escalating backoff.
- To sign other browsers out (e.g. a lost laptop), use the revoke control in the OS
  (`Settings`/`Connection`), which drops the other sessions.

---

## 5. Using the Cardputer itself

The native UI is **device-first** and single-app-foreground: one app fills the screen at a time;
closing it returns you to the launcher. Full reference: [`device-ui.md`](device-ui.md).

### Screen layout

A thin **status bar** on top (time · battery · Wi-Fi), the **app area** in the middle, and a
**hint bar** at the bottom that always names the keys you can press right now.

### Launcher & keys

The home screen is a **carousel** of apps and categories (ANIMA always leads). Key map:

| Key | What it does |
|---|---|
| `;` / `.` | Move up / down (previous / next) |
| `,` / `/` | Move left / right (where an app uses it) |
| `Enter` | Open a category / launch the focused app / confirm |
| `Esc` (top-left key) | Back one level / close the app → launcher; also clears a search |
| `Del` | Backspace when typing |
| **letters / digits** | **Spotlight search** — type to filter (see below) |
| `TAB` | Open the **Control Center** |

- **Spotlight search.** Just start typing at Home to filter **every** app (inside a category it
  narrows that category). Apps match by name **in any installed language** — "wetter", "meteo" and
  "weather" all find Weather. Results are ranked so the likeliest app lands under the focus.
  Spotlight also reaches into **Settings** (a final "N settings" result opens Settings pre-searched).
- **Pin an app** to Home with `*` (up to 6). Pins sit right after ANIMA.
- **Recent** — one Home tile lists the last 5 apps you opened.

> Digits are **search characters** in the launcher (an old 1–9 quick-launch was removed). Inside an
> app's scrolling list, `1`–`9` jump to that row instead — handy in Files, Calendar, Player, etc.

### Control Center (TAB)

TAB raises a one-screen quick panel from almost anywhere:

- **Toggle tiles** (keys `1`–`4`): **Mute · Torch · Sleep · Hotspot**.
- **Brightness** and **Volume** sliders — `←`/`→` adjust in place; `Enter` mutes volume.
- Shortcuts row: **Settings · Web client · USB keyboard · USB drive · Restart**. The **Web client**
  shortcut is where you read the **IP + pairing PIN**.
- Disruptive actions (Hotspot, Restart) **arm on the first Enter and fire on the second**, so a
  stray keypress can't trigger them.
- `Esc` or `TAB` closes it. Brightness/volume are saved when it closes.

### Settings on the device

The native **Settings** app is a smartwatch-style list of sections (network, theme, volume,
brightness, language, updates, and more), each row previewing its live value. A **theme** switch
recolors the whole UI live.

---

## 6. ANIMA basics

ANIMA is NucleoOS's assistant, and it works **offline** — no cloud, no account. It runs both on the
firmware and, as the same C compiled to WebAssembly, inside NucleoOS Web. It never invents facts it
can't support (details: [`anima.md`](anima.md), ground truth in [`anima-cortex.md`](anima-cortex.md)).

What you can do:

- **Ask it to do things** — "open the calculator", "create a file", set a reminder, tell the time
  or battery, do math and unit conversions.
- **Ask it questions** — it answers from its on-device knowledge; when it doesn't know, it says so
  honestly rather than guessing.
- **It can speak** — a bilingual (IT/EN) offline voice reads suitable answers aloud (time, launch
  confirmations, status) and says "read it on screen" for anything it shouldn't voice. See
  [`tts.md`](tts.md).
- **On the Cardputer**, open **ANIMA** from Home. In **NucleoOS Web**, use the ANIMA app or summon
  the OS-wide copilot with **`Ctrl`/`⌘ + Space`**.
- **When online**, ANIMA can reach structured web knowledge and, if you configure an API key,
  a cloud model — but the offline brain stays the default. See [`anima-online.md`](anima-online.md).

Optional: on-device **voice control** (hold **Fn/G0** and speak) recognizes trained keywords —
train them in the Voice Manager app. See [`voice.md`](voice.md).

---

## 7. Languages

The OS UI ships in **five languages: Italian, English, German, Spanish, French** (it · en · de · es
· fr). Change it in **Settings ▸ language** on the device, or in the web Settings. The choice is
system-wide — the shell, apps, launcher search and voice all follow it. (ANIMA's offline knowledge
engine understands Italian and English.) See [`i18n.md`](i18n.md).

---

## 8. Updating

Installed devices learn about new releases **by themselves**, and the device never contacts the
internet to do it — your **browser** checks GitHub. See [`update-check.md`](update-check.md).

- **From the web desktop:** the Notification Center shows one notice when a newer release exists.
  Open **Settings ▸ Updates** to download, verify (SHA-256), and install it. This is the easy path.
- **On the device:** if the browser has recorded a newer release, the Cardputer shows an **update
  dialog at boot** with an on-device installer (no PC needed).
- **Rollback on failure.** Updates use two firmware banks with **A/B rollback**: an update that fails its
  integrity check is never booted, and an image that boots but doesn't come up healthy is rolled
  back automatically to the previous one.
- Prefer to be told by GitHub? **Watch ▸ Custom ▸ Releases** on the repo.

The apps/desktop (the SD "web layer") and the firmware version independently — most day-to-day
improvements are web-layer only.

---

## 9. Troubleshooting

Only issues documented in the repo are listed here.

- **A phone won't join `NucleoOS-xxxx`** ("authentication required" / "incorrect password" /
  "couldn't authenticate"). This was a real field bug; the Wi-Fi supervisor now keeps the hotspot's
  air quiet so a phone's automatic retries can succeed — **just try again and give it a moment**.
  Background: [`wifi-supervisor.md`](wifi-supervisor.md).
- **The web page asks to pair but you can't find the PIN.** On the Cardputer press **TAB** →
  **Web client** (bottom line shows IP + PIN), or open **Connection ▸ Pair**. See
  [`security.md`](security.md).
- **The update page says "release just published, try again in a few minutes."** The download host
  hadn't finished publishing yet; wait and retry — this guard exists on purpose so you never flash a
  mismatched image ([`update-check.md`](update-check.md)).
- **Paint's / Forge's local GPU model won't turn on.** The optional in-browser GPU model needs a
  *secure context* (WebGPU) and a capable GPU; on a plain `http://<device-ip>` page the button
  becomes "How to turn it on" and explains the cause. ANIMA keeps answering meanwhile. See the
  `v142` entry in [`shell-cache-log.md`](shell-cache-log.md).
- **The SD card isn't recognized / apps are missing.** Confirm the card is **FAT32** and that the
  payload was extracted to the **root** so `/apps` `/www` `/system` `/data` sit at the top level
  ([Install](#step-2--prepare-the-microsd-card), [`storage.md`](storage.md)).
- **Which firmware is on the device?** The version shows in the boot banner and in the web
  `Settings`; the same string comes from `/api/status`. See [`versioning.md`](versioning.md).

---

## 10. Responsible use of the security tools

NucleoOS includes wireless and wired network security-testing tools. **They are for authorized
testing, research, and education only** — use them only on networks and devices you own or have
explicit written permission to test. Active transmission is restricted or illegal in many
countries, and you alone are responsible for complying with the law where you are.

Read the full terms before using any of these tools:
**[Legal & responsible use](../README.md#legal--responsible-use)**.

---

## 11. Where to go next

- **[docs index](README.md)** — every spec, grouped by topic.
- **Everyday device use:** [`device-ui.md`](device-ui.md) · languages: [`i18n.md`](i18n.md) ·
  voice: [`voice.md`](voice.md) / [`tts.md`](tts.md).
- **The assistant:** [`anima.md`](anima.md) → [`anima-cortex.md`](anima-cortex.md) (what really
  runs) · online behavior: [`anima-online.md`](anima-online.md).
- **Pairing & privacy:** [`security.md`](security.md) · updates: [`update-check.md`](update-check.md).
- **Building your own apps:** [`app-manifest.md`](app-manifest.md) ·
  [`app-runtimes.md`](app-runtimes.md) · [`registry.md`](registry.md).
- **Project overview & feature tour:** the [README](../README.md).
