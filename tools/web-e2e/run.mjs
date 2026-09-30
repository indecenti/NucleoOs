// `npm run web:e2e` — every browser E2E suite, in three phases so the suites measure NucleoOS, not the
// machine running them:
//   1. the shell/app suites, three at a time (each starts its own simulator + headless browser);
//   2. shell-smoke alone — it already runs one browser per language (five) side by side;
//   3. the suites that drive a REAL model on this PC's GPU (Ollama, and WebGPU with E2E_GPU=1), one at a time.
// Measured 2026-09-30 on a laptop with everything in parallel: a Notepad window took 16 s to load, a cold
// boot's request count swung 38–125, and the GPU ran out of memory for every model — all contention.
// Extra args are passed to node --test (e.g. --test-name-pattern).
import { spawnSync } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const SOLO = new Set(['shell-smoke.e2e.mjs']);
const HEAVY = new Set(['ollama.e2e.mjs', 'anima-local-ai.e2e.mjs']);
const all = readdirSync(here).filter((f) => f.endsWith('.e2e.mjs')).sort();
const path = (f) => join(here, f);
const phases = [
  { files: all.filter((f) => !SOLO.has(f) && !HEAVY.has(f)), conc: 3 },
  { files: all.filter((f) => SOLO.has(f)), conc: 1 },
  { files: all.filter((f) => HEAVY.has(f)), conc: 1 },
];
const extra = process.argv.slice(2);
let code = 0;
for (const { files, conc } of phases) {
  if (!files.length) continue;
  const r = spawnSync(process.execPath, ['--test', `--test-concurrency=${conc}`, ...extra, ...files.map(path)], { stdio: 'inherit' });
  code = code || (r.status ?? 1);
}
process.exit(code);
