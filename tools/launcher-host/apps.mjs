// The REAL native app registry, read straight from the firmware sources (no device, no build):
// every app definition in registration order, with its icon, accent colour and board gating.
// Shared by the launcher model gate (tools/launcher-host/check.mjs) and the UI screenshot harness
// (tools/ui-host/run.mjs), so both exercise exactly what ships.
import { readdirSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const ROOT = join(here, '..', '..');
const FW = join(ROOT, 'firmware', 'components');

function sources() {
  const out = [];
  (function walk(dir) {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) walk(p);
      else if (/\.(c|cpp)$/.test(e.name)) out.push(p);
    }
  })(FW);
  return out;
}

const stripComments = (s) => s.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

// Resolve a colour token: hex literal, a launcher_theme.h accent, or a #define in the same file.
function colorOf(tok, fileSrc, theme) {
  if (/^0x[0-9a-f]+$/i.test(tok)) return parseInt(tok, 16);
  if (theme[tok] !== undefined) return theme[tok];
  const m = new RegExp(`#define\\s+${tok}\\s+(0x[0-9A-Fa-f]+)`).exec(fileSrc)
         || new RegExp(`\\b${tok}\\s*=\\s*(0x[0-9A-Fa-f]+)`).exec(fileSrc);
  return m ? parseInt(m[1], 16) : 0x4D9F;
}

const DEF_RE = /nucleo_app_def_t\s+(\w+)\s*=\s*\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*'(\\?.)'\s*,\s*([A-Za-z0-9_]+)/g;

export function readRegistry() {
  const theme = {};
  for (const m of readFileSync(join(FW, 'nucleo_app', 'launcher_theme.h'), 'utf8').matchAll(/#define\s+(C_\w+)\s+(0x[0-9A-Fa-f]+)/g))
    theme[m[1]] = parseInt(m[2], 16);

  let callSites = 0;
  const byFunc = new Map();     // register function -> [defs] in textual order
  const loose = [];             // defs outside any nucleo_register_* body (e.g. nucleo_setup's device app)
  for (const f of sources()) {
    const raw = readFileSync(f, 'utf8');
    const src = stripComments(raw);
    callSites += (src.match(/\bnucleo_app_register\s*\(/g) || []).length;
    callSites -= (src.match(/\bvoid\s+nucleo_app_register\s*\(/g) || []).length;
    const funcs = [...src.matchAll(/void\s+(nucleo_(?:register_\w+|setup_register_apps))\s*\(\s*void\s*\)\s*\{/g)]
      .map((m) => ({ name: m[1], start: m.index }));
    for (const m of src.matchAll(DEF_RE)) {
      if (m[2] === 'stub') continue;                       // nucleo_app.cpp's internal placeholder
      const desc = [...m[5].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]).join('');
      const def = { id: m[2], name: m[3], cat: m[4], desc, icon: m[6].replace(/^\\/, ''), color: colorOf(m[7], raw, theme) };
      const owner = funcs.filter((fn) => fn.start < m.index).pop();
      if (owner) { if (!byFunc.has(owner.name)) byFunc.set(owner.name, []); byFunc.get(owner.name).push(def); }
      else loose.push(def);
    }
  }

  // Registration order = the call order in nucleo_app_register_builtins(), then nucleo_setup_register_apps().
  const app = stripComments(readFileSync(join(FW, 'nucleo_app', 'nucleo_app.cpp'), 'utf8'));
  const body = /void\s+nucleo_app_register_builtins\s*\(\s*void\s*\)\s*\{([\s\S]*?)\n\}/.exec(app)[1];
  const ordered = [];
  const seen = new Set();
  // ADV-gated calls sit inside `if (nucleo_ui_is_adv()) { ... }`.
  const advSpans = [...body.matchAll(/if\s*\(\s*nucleo_ui_is_adv\s*\(\s*\)\s*\)\s*\{([^}]*)\}/g)].map((m) => [m.index, m.index + m[0].length]);
  for (const m of body.matchAll(/\b(nucleo_register_\w+)\s*\(\s*\)/g)) {
    const advOnly = advSpans.some(([a, b]) => m.index >= a && m.index < b);
    for (const def of byFunc.get(m[1]) || []) {
      if (seen.has(def.id)) continue;
      seen.add(def.id);
      ordered.push({ ...def, advOnly });
    }
  }
  for (const def of [...(byFunc.get('nucleo_setup_register_apps') || []), ...loose])
    if (!seen.has(def.id)) { seen.add(def.id); ordered.push({ ...def, advOnly: false }); }
  // Anything defined but never reached from the boot path (dead code) is reported, not registered.
  const unreached = [...byFunc.values()].flat().filter((d) => !seen.has(d.id)).map((d) => d.id);
  return { apps: ordered, callSites, unreached };
}
