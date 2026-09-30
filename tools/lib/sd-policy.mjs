// sd-policy — the device/user-state classifier every SD tool uses. The DATA lives in tools/lib/sd-policy.json
// (read here at load time); this file only implements its pattern semantics. See the JSON's _doc.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export const POLICY_PATH = fileURLToPath(new URL('./sd-policy.json', import.meta.url));
const POLICY = JSON.parse(readFileSync(POLICY_PATH, 'utf8'));

// '**/' -> zero or more whole segments, '**' -> anything, '*' -> within one segment; case-insensitive.
export function globToRegExp(glob) {
  let re = '';
  for (let i = 0; i < glob.length; i++) {
    const c = glob[i];
    if (c === '*' && glob[i + 1] === '*') {
      if (glob[i + 2] === '/') { re += '(?:.*/)?'; i += 2; } else { re += '.*'; i += 1; }
    } else if (c === '*') re += '[^/]*';
    else re += c.replace(/[.+?^${}()|[\]\\]/g, '\\$&');
  }
  return new RegExp(`^${re}$`, 'i');
}

const STATE = POLICY.state.map(globToRegExp);
const ANIMA_SHIP = POLICY.animaShip.map(globToRegExp);

export function isDeviceState(rel) {
  const p = String(rel || '').replace(/\\/g, '/').replace(/^\/+/, '');
  if (STATE.some((r) => r.test(p))) return true;
  if (/^data\/anima\//i.test(p)) return !ANIMA_SHIP.some((r) => r.test(p));
  return false;
}
