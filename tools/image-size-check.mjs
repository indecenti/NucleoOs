#!/usr/bin/env node
// image-size-check.mjs — the firmware app image must fit the app slot M5Launcher created on devices
// that already run NucleoOS under it.
//
// The Launcher sizes each app slot to the image it installs and keeps that size: on the ADV it made
// nucle1/nucle2 = 0x310000 (3,211,264 B). A larger image cannot be written into that slot, so an update
// in place (Launcher OTA list, SD installer, tools/flash_slot) fails and the user would have to delete
// and reinstall. 2026-10-02 the image grew 27.8 KB past it unnoticed (+5-language strings); this gate
// makes the next one loud. Stand-alone installs (our own partition table, 0x380000 slots) have more
// room — the Launcher slot is the binding budget. Docs: docs/m5launcher.md "Size budget".
//
//   node tools/image-size-check.mjs [path/to/nucleoos.bin]     (default firmware/build/nucleoos.bin)
// Exit 1 when over the slot; a warning when under WARN_HEADROOM.
import { statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const LAUNCHER_SLOT = 0x310000;            // nucle1/nucle2 on the ADV (M5Launcher 2.9.x)
const WARN_HEADROOM = 8 * 1024;
const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const bin = process.argv[2] || join(ROOT, 'firmware', 'build', 'nucleoos.bin');

if (!existsSync(bin)) { console.error(`image-size: ${bin} not found — build the firmware first`); process.exit(2); }
const size = statSync(bin).size;
const free = LAUNCHER_SLOT - size;
const kb = (n) => (n / 1024).toFixed(1) + ' KB';
if (free < 0) {
  console.error(`FAIL  image ${size} B is ${kb(-free)} OVER the M5Launcher app slot (${LAUNCHER_SLOT} B): `
    + 'it cannot be updated in place on devices installed by M5Launcher. Recover flash (docs/memory-budget.md).');
  process.exit(1);
}
console.log(`${free < WARN_HEADROOM ? 'WARN' : 'OK  '}  image ${size} B, ${kb(free)} free in the M5Launcher slot (${LAUNCHER_SLOT} B)`);
