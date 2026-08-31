// Generate the EmulatorJS "core report" JSONs for the cores vendored under apps/arcade.
//
// Before downloading a core, EmulatorJS fetches `cores/reports/<core>.json` and uses its
// `buildStart` as the version key of its IndexedDB core cache. Upstream ships those files next to
// the cores; this bundle did not, so every launch logged "Could not fetch core report JSON! Core
// caching will be disabled!", fell back to a RANDOM key (`100 * Math.random()`), and re-pulled the
// ~1 MB core off the SD on every single run. That is precisely the traffic the firmware's low-heap
// circuit breaker answers with 503 (nucleo_webfs: files > 512 KB under a 32 KB largest-free-block).
//
// `buildStart` is derived from the core blob's own bytes: stable across runs, so the cache actually
// hits, and different the moment a core is replaced, so a stale cached core cannot survive an
// update. `options.defaultWebGL2` is true because this bundle ships only the WebGL2 build of each
// core — with it false, EmulatorJS asks for a "<core>-legacy-wasm.data" that is not on the card.
//
// Usage: node tools/gen-core-reports.mjs [--check]   (--check verifies, exits 1 on drift, no writes)
// Also wired into `npm run validate` so a swapped core cannot silently outlive its report.
import { readdirSync, readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CORES = join(ROOT, 'apps', 'arcade', 'www', 'emulatorjs', 'cores');
const REPORTS = join(CORES, 'reports');

// "<core>-wasm.data", "<core>-legacy-wasm.data" and "<core>-thread-wasm.data" all report under
// <core>: the report is per CORE, the suffix only picks which blob a given browser downloads.
// The alternation is longest-first ("thread-legacy" before "legacy"): a plain-first order would
// strip only "-legacy" off "-thread-legacy" and leave a bogus "<core>-thread" report.
export const coreName = (f) => f.replace(/-wasm\.data$/, '').replace(/-(thread-legacy|legacy|thread)$/, '');

// Self-check the variant folding — it is one regex whose ordering is easy to break. Runs on --check.
function selfCheck() {
  const cases = [
    ['fbalpha2012-wasm.data', 'fbalpha2012'],
    ['fbalpha2012-legacy-wasm.data', 'fbalpha2012'],
    ['fbalpha2012-thread-wasm.data', 'fbalpha2012'],
    ['fbalpha2012-thread-legacy-wasm.data', 'fbalpha2012'],
    ['snes9x-wasm.data', 'snes9x'],   // a name ending in a digit, not a variant
  ];
  const bad = cases.filter(([f, want]) => coreName(f) !== want).map(([f, want]) => `${f} -> ${coreName(f)} (want ${want})`);
  if (bad.length) { console.error('coreName self-check FAILED:\n  ' + bad.join('\n  ')); process.exit(1); }
}

export function buildReports() {
  const out = new Map();
  for (const f of readdirSync(CORES).sort()) {
    if (!f.endsWith('-wasm.data')) continue;
    const name = coreName(f);
    // 48 bits of the blob's SHA-256 — a safe integer, and EmulatorJS only ever compares it.
    const key = parseInt(createHash('sha256').update(readFileSync(join(CORES, f))).digest('hex').slice(0, 12), 16);
    const prev = out.get(name);
    // A core with several variants gets ONE report, so fold every variant into the key: replacing
    // any of them moves it (xor, so readdir order can never change the output).
    out.set(name, prev ? { ...prev, buildStart: prev.buildStart ^ key } : {
      _generated: 'node tools/gen-core-reports.mjs — do not edit by hand',
      core: name,
      buildStart: key,
      options: { defaultWebGL2: true },
    });
  }
  return out;
}

const text = (report) => JSON.stringify(report, null, 2) + '\n';
const read = (p) => (existsSync(p) ? readFileSync(p, 'utf8') : null);

// Returns the list of reports that do not match the cores on disk (empty === in sync).
export function checkCoreReports() {
  const drift = [];
  const reports = buildReports();
  if (!reports.size) return ['no core blobs found in apps/arcade/www/emulatorjs/cores'];
  for (const [name, report] of reports) {
    const current = read(join(REPORTS, `${name}.json`));
    if (current !== text(report)) drift.push(`${name}.json ${current === null ? 'missing' : 'out of date'}`);
  }
  // An orphan report would keep answering 200 for a core that is no longer on the card.
  for (const f of existsSync(REPORTS) ? readdirSync(REPORTS) : []) {
    if (f.endsWith('.json') && !reports.has(f.slice(0, -5))) drift.push(`${f} has no core blob — delete it`);
  }
  return drift;
}

function main() {
  const check = process.argv.includes('--check');
  selfCheck();
  const drift = checkCoreReports();
  if (check) {
    if (drift.length) {
      console.error('core reports out of sync (run: node tools/gen-core-reports.mjs):\n  ' + drift.join('\n  '));
      process.exit(1);
    }
    console.log(`core reports OK (${buildReports().size})`);
    return;
  }
  const reports = buildReports();
  mkdirSync(REPORTS, { recursive: true });
  for (const [name, report] of reports) {
    const path = join(REPORTS, `${name}.json`);
    if (read(path) !== text(report)) writeFileSync(path, text(report));
  }
  const orphans = drift.filter((d) => d.includes('no core blob'));
  if (orphans.length) console.warn('note:\n  ' + orphans.join('\n  '));
  console.log(`wrote ${reports.size} core reports to apps/arcade/www/emulatorjs/cores/reports/ (then run: node tools/gzip-assets.mjs apps/arcade)`);
}

if (process.argv[1] && process.argv[1].endsWith('gen-core-reports.mjs')) main();
