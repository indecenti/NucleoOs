#!/usr/bin/env node
// TEACH-CHECK — the offline learning loop, end-to-end on the REAL cascade (anima.exe).
// Proves the intelligence delta of nucleo_anima_learn.c: a fact the user TEACHES becomes recallable by
// PARAPHRASE, offline, while same-shape-different-subject queries and volatile statements ABSTAIN.
//
// Stateful (unlike skill-probe): it drives ONE exe process whose SD store persists between turns, so a
// teach turn writes /data/anima/user.{tsv,vec} and a later recall turn reads it back. It wipes the store
// before AND after so no residue leaks into the other gates (halluc-stress etc. must see an empty store).
//
//   node tools/anima-host/teach-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const sd = join(here, 'sd', 'data', 'anima');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

const wipe = () => { for (const f of ['user.tsv', 'user.vec', 'user.tsv.tmp', 'user.vec.tmp']) { try { rmSync(join(sd, f)); } catch { /* absent is fine */ } } };

// One ordered script: teach a few facts, then recall by paraphrase (IT+EN), then prove the guards abstain.
// want: {intent} exact intent · {reply:/re/} reply must match · {abstain:true} must NOT recall ·
//       {notIntent} intent must differ (reminder must not be hijacked by teach).
const steps = [
  // --- teach (write) ---
  { q: 'ricorda che la sala riunioni è al terzo piano', lang: 'it', want: { intent: 'teach' } },
  { q: 'ricorda che il compleanno di Luca è il 12 marzo', lang: 'it', want: { intent: 'teach' } },
  { q: 'remember that the spare key is under the red flowerpot', lang: 'en', want: { intent: 'teach' } },
  // --- recall by paraphrase, fully offline (the intelligence delta) ---
  { q: 'dove si trova la sala riunioni', lang: 'it', want: { intent: 'recall', reply: /terzo piano/i } },
  { q: 'a che piano è la sala delle riunioni', lang: 'it', want: { intent: 'recall', reply: /terzo piano/i } },
  { q: 'quando è il compleanno di luca', lang: 'it', want: { intent: 'recall', reply: /12 marzo/i } },
  { q: 'where is the spare key', lang: 'en', want: { intent: 'recall', reply: /red flowerpot/i } },
  // --- adversarial: same shape, different subject / different thing -> MUST abstain (no misattribution) ---
  { q: 'quando è il compleanno di marco', lang: 'it', want: { abstain: true } },
  { q: 'dove si trova la sala professori', lang: 'it', want: { abstain: true } },
  { q: 'dove si trova la mensa', lang: 'it', want: { abstain: true } },
  { q: 'where is the car key', lang: 'en', want: { abstain: true } },
  // --- volatility law: a "today/now" statement is REFUSED by the is_volatile guard, never frozen as a fact ---
  { q: 'ricorda che oggi è il mio giorno fortunato', lang: 'it', want: { intent: 'teach', reply: /temporanea|temporary/i } },
  // --- a reminder ("ricordami DI ...") must NOT be hijacked by teach (no copula -> falls through) ---
  { q: 'ricordami di chiamare luca domani', lang: 'it', want: { notIntent: 'teach' } },
];

wipe();
let lang = 'it';
const lines = [];
for (const s of steps) {
  lines.push('/reset');
  const want = s.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(s.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const parse = (b) => ({
  tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});

const show = process.argv.includes('--show');
const fails = [];
for (let i = 0; i < steps.length; i++) {
  const s = steps[i], p = parse(blocks[i] || ''), w = s.want;
  let ok = true, why = '';
  if (w.intent && p.intent !== w.intent) { ok = false; why = `intent ${p.intent || '(none)'} != ${w.intent}`; }
  if (w.notIntent && p.intent === w.notIntent) { ok = false; why = `intent must not be ${w.notIntent}`; }
  if (w.abstain && p.intent === 'recall') { ok = false; why = `recalled "${p.reply.slice(0, 30)}" — should abstain`; }
  if (w.reply && !w.reply.test(p.reply)) { ok = false; why = `reply "${p.reply.slice(0, 40)}" !~ ${w.reply}`; }
  if (!ok) fails.push([s.q, why]);
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL  '} "${s.q}" -> ${p.intent || '(none)'}/${p.tier} ${p.reply ? `"${p.reply.slice(0, 36)}"` : ''}`);
}
wipe();

const pass = steps.length - fails.length;
console.log(`[teach-check] offline learn loop — ${pass}/${steps.length} pass (${(100 * pass / steps.length).toFixed(1)}%)`);
if (fails.length) {
  console.log('FAILURES:');
  for (const f of fails) console.log(`  ✗ "${f[0]}" — ${f[1]}`);
  process.exit(1);
}
console.log('✓ teach → offline paraphrase recall, abstention, volatility-refusal, reminder-safety all hold');
