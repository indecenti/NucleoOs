#!/usr/bin/env node
// REC8 — DEVICE-DIM gate runner. The normal gate measures the D=256 HOST fixture; the real device is
// D=192 (deploy/sd-safe) with a coarser encoder (H=16384), so a fix can pass the gate yet miss on the
// device (e.g. "chi è nixon" recovers at D=256 but not D=192). This points the harness ./sd at the device
// tree via ANIMA_SD_ROOT (host_main.c honours it) so the SAME anima.exe measures REAL device recall on the
// host — no hardware round-trip. Runs akb5-content (or any harness check passed as args) at D=192.
// Usage: node tools/anima-host/dev192.mjs [akb5-content args]   |   npm run anima:dev192
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, 'd192root');
const link = join(root, 'sd');
if (!existsSync(link)) {
  console.error('[dev192] missing junction ' + link + ' -> deploy/sd-safe. Create it once (Windows):');
  console.error('  cmd /c mklink /J "' + link + '" "' + join(here, '..', '..', 'deploy', 'sd-safe') + '"');
  process.exit(2);
}
const target = process.argv[2] && process.argv[2].endsWith('.mjs')
  ? process.argv.slice(2)
  : [join(here, 'akb5-content.mjs'), ...process.argv.slice(2)];
const r = spawnSync('node', target, { stdio: 'inherit', env: { ...process.env, ANIMA_SD_ROOT: root } });
process.exit(r.status ?? 1);
