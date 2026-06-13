// settings-merge.js — the device settings model + a clobber-safe deep merge.
//
// Extracted from the old inline merge (which replaced whole sub-trees and could clobber keys the
// user never touched). Pure + dependency-free so the Settings app, the scenes engine and the unit
// tests all share ONE definition of "what a settings document looks like" and "how to overlay it".
//
// Contract: merge(base, over) returns a NEW value = base overlaid with `over`, recursing only into
// plain objects. Arrays and primitives from `over` replace wholesale. Missing sub-trees in `over`
// are back-filled from `base`. Neither argument is mutated.

export const DEFAULTS = {
  schema: 1,
  device:  { name: 'nucleo-01', locale: 'it-IT', timezone: 'Europe/Rome' },
  network: {
    wifi:  { enabled: true },
    ble:   { enabled: true },
    swarm: { enabled: true },
    ipv6:  { enabled: false },
  },
  power: { profile: 'performance', sleep_timeout_s: 0, display_brightness: 100, volume: 70 },
  ui:    { theme: 'dark', accent: '#4ea1ff', fontSize: '14px', language: 'it', regionLocale: '' },
  voice: { alwaysOn: false },
  // AI profile: only the chosen preset INTENT is durable on SD. The concrete brain knobs
  // (anima.mode, L1 policy, exec, local model) are RE-DERIVED at load by preset-engine.js from
  // this intent + the live CapabilityModel — so the profile survives a browser wipe without
  // mirroring a dozen volatile keys onto the device (no extra MCU cost, no fifth source of truth).
  ai:    { preset: 'auto' },
};

const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);

// Deep clone of plain JSON-ish values (objects, arrays, primitives). Avoids structuredClone so the
// same code runs identically in the browser iframe and under `node --test`.
export function clone(v) {
  if (Array.isArray(v)) return v.map(clone);
  if (isObj(v)) { const o = {}; for (const k of Object.keys(v)) o[k] = clone(v[k]); return o; }
  return v;
}

export function merge(base, over) {
  // base is a primitive/array → over wins if defined, else a clone of base.
  if (!isObj(base)) return over === undefined ? clone(base) : clone(over);

  const out = {};
  for (const k of Object.keys(base)) out[k] = clone(base[k]);   // start from a full clone of base
  if (isObj(over)) {
    for (const k of Object.keys(over)) {
      out[k] = (isObj(out[k]) && isObj(over[k])) ? merge(out[k], over[k]) : clone(over[k]);
    }
  }
  return out;
}

// Shallow "what differs from DEFAULTS" — drives the "show only changed" filter. Returns dotted paths.
export function changedPaths(model, base = DEFAULTS, prefix = '') {
  const out = [];
  for (const k of Object.keys(base)) {
    const p = prefix ? prefix + '.' + k : k;
    const a = base[k], b = model ? model[k] : undefined;
    if (isObj(a) && isObj(b)) out.push(...changedPaths(b, a, p));
    else if (JSON.stringify(a) !== JSON.stringify(b)) out.push(p);
  }
  return out;
}
