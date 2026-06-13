#!/usr/bin/env node
// HALLUC SUITE — runs the anti-fabrication probe over every adversarial NL eval file and fails (exit 1)
// if ANY question was answered with a confident fabrication. These files were NOT in the release gate
// before, so a regression (e.g. a date/geo/system skill over-firing on a false premise) slipped through
// silently. This wires the whole battery into anima:gate as a HARD 0-hallucination guard.
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const probe = join(here, 'halluc-probe.mjs');
const FILES = ['eval_halluc_it', 'eval_halluc_en', 'eval_halluc2_it', 'eval_halluc2_en', 'eval_halluc_math_it', 'eval_halluc_math_en', 'eval_halluc3_it', 'eval_halluc3_en'];

let total = 0, fab = 0, bad = 0;
for (const f of FILES) {
  const r = spawnSync(process.execPath, [probe, join(here, '..', '..', 'tools', 'anima', f + '.jsonl')], { encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  const m = out.match(/(\d+)\/(\d+) honestly abstained/);
  if (m) { const pass = Number(m[1]), n = Number(m[2]); total += n; fab += (n - pass); }
  else { bad++; }
  if (r.status !== 0) process.exitCode = 1;
  process.stdout.write(`  ${f}: ${m ? `${m[1]}/${m[2]}` : 'ERROR'}${r.status ? '  <-- FABRICATIONS' : ''}\n`);
}
console.log(`[halluc-suite] ${FILES.length} files · ${total} adversarial traps · ${fab} fabrication(s) (HARD: must be 0)`);
if (fab === 0 && bad === 0) console.log('✓ zero hallucinations across the whole offline NL battery.');
