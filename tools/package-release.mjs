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
writeFileSync(join(dist, 'FLASH.md'), `# NucleoOS ${tag} — install

A web-native appliance OS for the **M5Stack Cardputer** (ESP32-S3). One universal
binary auto-detects the board (original **and** ADV) at runtime.

## 1. Flash the firmware (\`${binName}\`)

**Easiest — browser (Chrome/Edge, no install):** open the web flasher, plug the Cardputer
in via USB-C, pick the port, click Install. (Web flasher uses \`manifest.json\`.)

**Or with esptool** (Python; \`pip install esptool\`):

\`\`\`
esptool --chip esp32s3 --baud 921600 write_flash 0x0 ${binName}
\`\`\`

The image is a full merged flash (bootloader + partition table + app) written at 0x0 — nothing else needed.

## 2. Prepare the microSD

The OS serves its apps and data from a microSD. Format a card **FAT32** and extract
**\`${sdZip}\`** to its root (you should end up with \`/apps\`, \`/www\`, \`/system\`, \`/data\` on the card).
Insert it before boot.

## 3. First boot

Power on: you'll see the animated boot splash, then the desktop. Open a browser on the same
Wi-Fi and go to the device's IP (shown on screen) for the full operator console.

## Optional add-ons (not bundled, for legal/size reasons)

- **Games:** the launchers are included but ship with **no ROMs** — add your own to \`/data/ROMs\` and \`/data/DOS\`.
- **In-browser LLM (ANIMA Forge):** the WebLLM model (~280 MB) is not bundled — install it from inside the app.
- **On-device voice (TTS):** the concatenative clip banks are an oversized asset — see the repo's \`OVERSIZED-ASSETS.md\`.

## Recover / re-flash

Re-flashing is always safe (writes the app slot; the previous firmware survives in the OTA backup slot).
`);

writeFileSync(join(dist, 'RELEASE_NOTES.md'), `## NucleoOS ${tag}

Installable build for the M5Stack Cardputer (ESP32-S3), original + ADV (one universal binary).

- **\`${binName}\`** — merged firmware, flash at 0x0 (esptool) or via the browser web flasher (\`manifest.json\`).
- **\`${sdZip}\`** — the microSD payload (apps + desktop + system + offline knowledge base). Extract to a FAT32 card.

See **FLASH.md** for install steps. No ROMs / user data / secrets are included; games ship without ROMs (add your own).
`);

console.log(`package-release: tag=${tag}  bin=${binName}  sdzip=${sdZip}`);
console.log(`  staged clean SD tree at dist/sd (excluded: forge/models, *.pcm)`);
console.log(`  wrote dist/manifest.json, dist/FLASH.md, dist/RELEASE_NOTES.md`);
