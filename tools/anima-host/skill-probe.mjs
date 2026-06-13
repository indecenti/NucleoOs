#!/usr/bin/env node
// SKILL PROBE — generic NL routing checker over the REAL pipeline (anima.exe). Reads a JSONL of
// { q, lang?, expect } cases and asserts the agent loop routes each phrase as expected:
//   expect = "<intent>"          pass if routed intent === that (or is in a "a|b|c" alternative list)
//   expect = "abstain"           pass if honest miss (tier none / empty reply) — for WRONG/unanswerable asks
//   expect = "answer"            pass if it answered with any tier (fact/command), non-empty reply
//   expect = "tier:L0/command"   pass if the tier matches (prefix ok)
// Prints per-file pass rate + every failure (phrase, expected, got). Exit != 0 on any failure.
//
//   node tools/anima-host/skill-probe.mjs <cases.jsonl> [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const file = process.argv[2];
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
if (!file || !existsSync(file)) { console.error('usage: skill-probe.mjs <cases.jsonl> [--show]'); process.exit(2); }

const items = readFileSync(file, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

let lang = 'it';
const lines = [];
for (const it of items) {
  lines.push('/reset');
  const want = it.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(it.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const parse = (b) => ({
  tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});

// A MOSAICO (L2/stitch) answer IS an L1 knowledge answer, enriched with more grounded spans — so it
// counts as answered/fact for routing, and its intent "mosaico" satisfies an expected "l1".
const answered = p => (p.tier === 'L1/fact' || p.tier === 'L2/stitch' || p.tier === 'L3/remote' || p.tier.startsWith('L0')) && p.reply;
const factAnswered = p => (p.tier === 'L1/fact' || p.tier === 'L2/stitch' || p.tier === 'L3/remote') && p.reply;
const intentEq = p => (p.intent === 'mosaico' ? 'l1' : p.intent);

const show = process.argv.includes('--show');
const fails = [];
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = parse(blocks[i] || '');
  const exp = String(it.expect || '');
  let ok;
  if (exp === 'abstain') ok = !factAnswered(p) && p.intent !== 'capabilities' && p.intent !== 'agenda';
  else if (exp === 'answer') ok = answered(p);
  else if (exp.startsWith('tier:')) ok = p.tier.startsWith(exp.slice(5));
  else ok = exp.split('|').map(s => s.trim()).includes(intentEq(p));
  if (!ok) fails.push([it.q, exp, `${p.intent || '(none)'} / ${p.tier}`, p.reply.slice(0, 44)]);
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL  '} "${it.q}" exp=${exp} got=${p.intent || '(none)'}/${p.tier}`);
}

const pass = items.length - fails.length;
console.log(`[skill-probe] ${file.split(/[\\/]/).pop()} — ${pass}/${items.length} pass (${(100*pass/items.length).toFixed(1)}%)`);
if (fails.length) {
  console.log('FAILURES:');
  for (const f of fails) console.log(`  ✗ "${f[0]}" exp=${f[1]} got=${f[2]} ${f[3] ? `reply="${f[3]}"` : ''}`);
  process.exit(1);
}
console.log('✓ all routed as expected');
