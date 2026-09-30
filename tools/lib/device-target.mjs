// device-target — where a dev tool finds the Cardputer's address and pairing PIN. NEVER from the source:
// the PIN is a secret (the repo is public). Order: explicit value (CLI) > env NUCLEO_HOST / NUCLEO_PIN >
// tools/release.local.json {"host","pin"} (gitignored — tools/*.local.json). Missing values stay '' so the
// caller can fail with a clear message.
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export const LOCAL_CONFIG = fileURLToPath(new URL('../release.local.json', import.meta.url));

export function deviceTarget({ host, pin } = {}) {
  let local = {};
  if (existsSync(LOCAL_CONFIG)) {
    try { local = JSON.parse(readFileSync(LOCAL_CONFIG, 'utf8').replace(/^﻿/, '')) || {}; } catch { local = {}; }
  }
  const pick = (...v) => String(v.find((x) => x !== undefined && x !== null && x !== true && String(x) !== '') ?? '');
  return {
    host: pick(host, process.env.NUCLEO_HOST, local.host),
    pin: pick(pin, process.env.NUCLEO_PIN, local.pin),
  };
}

export const TARGET_HELP = 'pass --host/--pin, set NUCLEO_HOST/NUCLEO_PIN, or create tools/release.local.json {"host":"…","pin":"…"}';
