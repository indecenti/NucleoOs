// Icon-set drift gate. The firmware launcher icon set (ui_icon in
// firmware/components/nucleo_app/launcher_render.cpp) is a hand-kept 1:1 port of the browser set
// (web/device/icons.js). They must draw the SAME id -> glyph mapping, or an app shows a vector icon
// on the device but a bald letter in the web sim (or vice versa). This gate parses the id list out
// of each file and fails on any id that exists in one but not the other. Run: npm run icons:gate
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const C  = join(root, 'firmware/components/nucleo_app/launcher_render.cpp');
const JS = join(root, 'web/device/icons.js');

// Known drift frozen as debt: these ids draw a real vector glyph on the DEVICE (ui_icon) but the
// browser set (icons.js) still falls back to a bald letter for them. Listed here so the gate is a
// ratchet — it tolerates this existing backlog but FAILS on any NEW divergence. To clear an entry,
// port its glyph into web/device/icons.js and delete it from this set. (Not done here: this pass is
// firmware-only, and a few are security-app icons that are out of scope.)
const ALLOW = new Set([
  'ble', 'payloads', 'sniffer',   // security-app glyphs: device-only, web port deferred (out of scope)
]);

function idsFromC(src) {
  // ui_icon dispatches on: !strcmp(id, "xxx")
  const ids = new Set();
  for (const m of src.matchAll(/strcmp\(id,\s*"([^"]+)"\)/g)) ids.add(m[1]);
  return ids;
}
function idsFromJS(src) {
  // drawIcon dispatches on: case 'xxx':  (single-quoted string case labels)
  const ids = new Set();
  for (const m of src.matchAll(/case\s+'([^']+)'\s*:/g)) ids.add(m[1]);
  return ids;
}

const c  = idsFromC(readFileSync(C, 'utf8'));
const js = idsFromJS(readFileSync(JS, 'utf8'));

const onlyC  = [...c].filter(id => !js.has(id) && !ALLOW.has(id)).sort();
const onlyJS = [...js].filter(id => !c.has(id) && !ALLOW.has(id)).sort();

if (onlyC.length || onlyJS.length) {
  console.error('ICON DRIFT — the firmware and web icon sets disagree:');
  if (onlyC.length)  console.error(`  in C (launcher_render.cpp) but NOT in web/device/icons.js: ${onlyC.join(', ')}`);
  if (onlyJS.length) console.error(`  in web/device/icons.js but NOT in C (launcher_render.cpp): ${onlyJS.join(', ')}`);
  console.error('Add the missing glyph to the other file (or, if intentional, allow-list it in tools/check-icons.mjs).');
  process.exit(1);
}
console.log(`icons:gate OK — ${c.size} ids match across firmware and web.`);
