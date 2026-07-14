// package-release.mjs — assemble the PUBLIC release payload into dist/.
//
// Produces, from a clean checkout (no user data, no ROMs — those are gitignored):
//   dist/sd/                 the distributable SD-card tree (deploy/sd-safe minus the heavy OPTIONAL
//                            browser-LLM models + oversized voice clips) — the workflow zips this.
//   dist/manifest.json       an ESP Web Tools manifest for the single merged image (browser flashing).
//   dist/FLASH.md            end-user install instructions (esptool + web flasher + SD setup).
//   dist/RELEASE_NOTES.md    a short notes stub the release can prepend to auto-generated notes.
//
// The merged firmware image itself (nucleoos-<ver>.bin, flashable at 0x0) is produced by the release
// workflow with `esptool merge_bin` after `idf.py build` — this script only references it by name so it
// runs with plain Node (no ESP-IDF needed) and is unit-testable locally.
//
//   node tools/package-release.mjs
import { readFileSync, writeFileSync, mkdirSync, rmSync, cpSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const ver   = readFileSync(join(root, 'firmware/version/VERSION'), 'utf8').trim();   // e.g. 0.2.0
const build = readFileSync(join(root, 'firmware/version/BUILD'),   'utf8').trim();   // e.g. 122
const full  = `${ver}.${build}`;
const tag   = `v${full}`;
const binName = `nucleoos-${full}.bin`;
const sdZip   = `nucleoos-${full}-sd.zip`;

const dist = join(root, 'dist');
rmSync(dist, { recursive: true, force: true });
mkdirSync(dist, { recursive: true });

// --- clean SD tree: deploy/sd-safe is the distributable card (game/ROM entries are .factory manifests
// only, never binaries). Drop the heavy OPTIONAL ML assets a first install doesn't need — they're all
// installable from inside their app: the image-diffusion model (~530 MB, Paint), the speech models
// (~70 MB, ANIMA voice), the WebLLM model (ANIMA Forge), and the big runtime wasm (ONNX, ffmpeg, wllama).
// This keeps the SD payload lean (~60 MB) while the OS, every app's code, and the offline text knowledge
// base ship. Users add the models in-app on demand. ---
const HEAVY = ['/models/', '/vendor/onnxruntime-web/', '/vendor/ffmpeg/', '/vendor/wllama/', '/forge/vendor/'];
const norm = (s) => s.replace(/\\/g, '/');
const skip = (s) => { const n = norm(s);
  return HEAVY.some((h) => n.includes(h)) || /\.(pcm|gguf|npy|onnx)$/i.test(n); };
cpSync(join(root, 'deploy/sd-safe'), join(dist, 'sd'), { recursive: true, filter: (s) => !skip(s) });

// --- ESP Web Tools manifest: one merged image written at offset 0 (bootloader+parttable+ota+app) ---
writeFileSync(join(dist, 'manifest.json'), JSON.stringify({
  name: 'NucleoOS',
  version: full,
  new_install_prompt_erase: true,
  builds: [{ chipFamily: 'ESP32-S3', parts: [{ path: binName, offset: 0 }] }],
}, null, 2) + '\n');

// --- end-user install guide ---
writeFileSync(join(dist, 'FLASH.md'), `# NucleoOS ${tag} — Install Guide

NucleoOS is a web-native appliance OS for the **M5Stack Cardputer** (ESP32-S3). A single
universal firmware auto-detects your board — the **original** Cardputer and the **ADV** both
run the same image. The device shows a desktop UI on its own screen and, over Wi-Fi, a full
operator console in any browser.

**You need:** a Cardputer, a USB-C cable, and a **microSD card** (formatted **FAT32**).

---

## Step 1 — Flash the firmware ( \`${binName}\` )

Pick **one** of the two methods.

### A) Web flasher (easiest — no software to install)
1. Use **Chrome** or **Edge** on a desktop (Web Serial isn't supported on Firefox/Safari/mobile).
2. Open an ESP Web Tools flasher page (e.g. <https://web.esphome.io> → **Prepare for first use** /
   **Connect**) — or any page that loads the included \`manifest.json\`.
3. Plug the Cardputer into USB-C, click **Connect**, choose its serial port, then **Install**.
   \`${binName}\` is a full merged image (bootloader + partition table + app) written at offset 0 — that's all it needs.

### B) esptool (command line)
Requires Python: \`pip install esptool\`. Then, with the Cardputer connected:
\`\`\`
esptool --chip esp32s3 --baud 921600 write_flash 0x0 ${binName}
\`\`\`
(If flashing fails, hold the reset/boot combo or lower the baud to 115200.)

Re-flashing is always safe: it writes the app slot, and the previous firmware survives in the OTA
backup slot, so you can't brick the device this way.

---

## Step 2 — Prepare the microSD card

NucleoOS serves all of its apps and data from the microSD.
1. Format the card as **FAT32**.
2. Extract **\`${sdZip}\`** to the **root** of the card. You should end up with these folders on the card:
   \`/apps\`, \`/www\`, \`/system\`, \`/data\`.
3. Insert the card into the Cardputer **before powering on**.

---

## Step 3 — First boot

Power on. You'll see the animated boot splash, then the desktop. The screen shows the device's
**IP address** — open it in a browser **on the same Wi-Fi** for the full desktop, apps and settings.

---

## Adding the optional AI models (not bundled — see below why)

To keep this download small (~30 MB) and license-clean, the **large machine-learning models are not
included**. You almost never need to copy them manually: **open the app once while online and it
downloads its model from a public CDN and caches it in your browser**, after which it runs offline.
The models on the SD are only needed for a device that is *never* online, or to skip that one-time
browser download.

| Feature | App | Get it the easy way (browser) | Put it permanently on the SD |
|---------|-----|-------------------------------|------------------------------|
| **Sketch → image** (diffusion) | Paint / Atelier | Open Paint → **Atelier**, click the download button (~530 MB, from GitHub) — cached in the browser. | Copy the model into \`apps/paint/www/models/sdxs-512-dreamshaper-sketch/\` on the card (from the repo: \`node tools/sdxs/provision-lsb.mjs\`). |
| **Offline chat LLM** (WebLLM) | ANIMA → Forge | Open ANIMA → **Forge** → **Install model** — it pulls the weights from a CDN into the browser cache. | Let Forge install from the *device* SD instead of the CDN: place the model under \`apps/anima/www/forge/models/\` (see the app's model list). |
| **Voice dictation** (Vosk) | ANIMA (mic) | Just use the mic once online — the small Vosk model auto-downloads and caches. | Copy the model files into \`apps/anima/www/vosk/models/\` (\`vosk-model-small-it-0.4.tar.gz\`, \`…-en-us-0.15.tar.gz\`, or their split \`.000/.001…\` parts). |
| **On-device spoken voice** (TTS) | system voice | — (this one is device-side, no browser fallback) | Rebuild the clip banks from the repo (\`node oversized-assets/rejoin.mjs tts-it-clips tts-en-clips\`) and copy them to \`data/tts/it/clips.pcm\` and \`data/tts/en/clips.pcm\`. See \`OVERSIZED-ASSETS.md\`. |

**Copying files onto the SD** — two ways:
- **Card reader:** put the SD in your computer and drop the files into the paths above.
- **Over Wi-Fi (no card removal):** in the OS's **File Commander** app, or via the file API
  \`POST http://<device-ip>/api/fs/write?path=<path>\` (pair first with the on-screen PIN).

### Games (retro launchers)
The **GameFront** launcher and the **DOS** app are included, but ship with **no game files** for copyright
reasons. Add your own legally-obtained ROMs to \`data/ROMs/\` (Game Boy → \`gb\`, Game Gear → \`gg\`, …) and
DOS bundles to \`data/DOS/\`.

---

## Troubleshooting
- **No apps / blank desktop:** the microSD isn't inserted, isn't FAT32, or the zip wasn't extracted to the card root.
- **Web flasher won't connect:** use Chrome/Edge on desktop; try a different USB-C cable/port; close other serial monitors.
- **A model won't download:** check the device (or your browser) has internet the first time; downloads are one-at-a-time.
`);

writeFileSync(join(dist, 'RELEASE_NOTES.md'), `## NucleoOS ${tag}

An installable build of NucleoOS — a web-native appliance OS — for the **M5Stack Cardputer**
(ESP32-S3). One universal firmware runs on both the **original** and the **ADV** board.

### Download
- **\`${binName}\`** — the firmware. Flash it at offset **0x0** with a browser web flasher (uses
  \`manifest.json\`) or \`esptool write_flash 0x0\`.
- **\`${sdZip}\`** — the microSD payload (desktop + apps + system + the offline knowledge base). Extract
  it to the root of a **FAT32** card and insert it before boot.

### Install
See **[FLASH.md](FLASH.md)** for step-by-step instructions, including **how to add the optional AI models**
(image diffusion, offline chat LLM, voice) — most just download themselves from inside the app on first
use, or can be copied onto the SD for fully-offline devices.

### What's included / not
The download is intentionally lean (~30 MB): the large ML models are **not** bundled (they install
in-app), and games ship **without ROMs** — add your own. No user data, keys, or other sensitive files
are included.
`);

console.log(`package-release: tag=${tag}  bin=${binName}  sdzip=${sdZip}`);
console.log(`  staged clean SD tree at dist/sd (excluded: ML models + heavy runtime wasm)`);
console.log(`  wrote dist/manifest.json, dist/FLASH.md, dist/RELEASE_NOTES.md`);
