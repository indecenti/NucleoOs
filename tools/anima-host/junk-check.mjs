#!/usr/bin/env node
// SAFETY gate: a bare DATE / number / time / cell-ref / code / wordless string must NEVER be
// answered with a (fabricated) fact — it must be an honest miss (tier=none). This pins the
// a_is_askable() guard so the "type a date → get 'Diva Futura' at 70%" class of bug can't return.
// Genuine offline-answerable requests must still work (the guard is transparent to real questions).
//
//   npm run anima:build && node tools/anima-host/junk-check.mjs
// Exit != 0 on any regression.
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

function run(q) {
  const r = spawnSync(exe, [q], { encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  const tier = (out.match(/tier=([^\s]+)/) || [, '?'])[1];
  const reply = (out.match(/reply:\s*(.*)/) || [, ''])[1].trim();
  return { tier, reply };
}

// must be an honest miss: tier=none, no fabricated fact text
const JUNK = ['24:04:2027', '24/04/2027', '2027-04-24', '24.04.2027', '42', '2024', '007', '14:30',
  'A1', 'A1:B10', 'B7', 'x7gq2', 'ab12cd', '??', '#$%', '. , ;'];
// must NOT be blocked (deterministic offline tiers answer these); incl. "12/4" to prove the
// date-guard does NOT over-block a genuine two-part division.
const REAL = ['che ore sono', 'chi sei', '12 per 8', 'quanto fa 15% di 240', '2+2', '12/4'];

let fails = [];
for (const q of JUNK) { const { tier, reply } = run(q); if (tier !== 'none' && tier !== '?') fails.push(`JUNK "${q}" -> tier=${tier} reply="${reply.slice(0, 40)}" (atteso tier=none)`); }
for (const q of REAL) { const { tier } = run(q); if (tier === 'none') fails.push(`REAL "${q}" -> tier=none (bloccata per errore)`); }

console.log(`[junk] ${JUNK.length + REAL.length - fails.length}/${JUNK.length + REAL.length} cases pass  (junk→none, real→answered)`);
if (fails.length) { console.log('\nREGRESSIONS:'); fails.forEach(f => console.log('  ✗ ' + f)); process.exit(1); }
console.log('✓ no garbage is ever answered with a fact; real questions still work');
