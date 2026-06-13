// AKB5 gate — the coverage the 45 flat gates miss. The host harness runs FLAT by default (the calibrated
// fixture), so the SHARDED (AKB5) path is uncertified — which let a recall regression hide. This drives
// the SAME native cascade (anima.exe) twice per query — flat (no env) and AKB5 (ANIMA_AKB5=1) — over a
// battery of KGE deductions, AKB5-recall facts and adversarial abstentions, and flags any divergence:
//   * REGRESSION: flat answers but AKB5 abstains (or differs) on a query that must answer.
//   * FABRICATION: AKB5 answers an adversarial/OOD query that must abstain (HARD: zero).
// Usage: node tools/anima-host/akb5-check.mjs [--show]      (exit 0 = AKB5 clean vs flat)
import { execFileSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const show = process.argv.includes('--show');

function ask(q, lang, akb5) {
  const env = { ...process.env, L1_PFM: '64' };
  if (akb5) env.ANIMA_AKB5 = '1'; else delete env.ANIMA_AKB5;
  const input = (lang === 'en' ? '/en\n' : '/it\n') + '/reset\n' + q + '\n';
  let out = '';
  try { out = execFileSync(exe, [], { input, encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'], env }); }
  catch (e) { out = (e.stdout || '').toString(); }
  const m = out.match(/^ {3}reply: (.*)$/m);
  let r = m ? m[1] : ''; if (r === '(vuoto)') r = '';
  return r.trim();
}

// must ANSWER in both modes (KGE deductions + AKB5-recall facts)
const ANSWER = [
  ['it', 'quando è nato einstein'],
  ['it', 'in che continente è la francia'],
  ['it', 'chi ha scritto la divina commedia'],
  ['it', 'quando è nato leonardo da vinci'],
  ['it', 'capitale del giappone'],
  ['it', 'capitale della francia'],
  ['en', 'when was einstein born'],
];
// must ABSTAIN in both modes (adversarial / OOD)
const ABSTAIN = [
  ['it', 'capitale di Marte'],
  ['it', 'quando è nato il fiume Po'],
  ['it', 'asdkfj qwerty zzz'],
  ['it', 'chi è il presidente dei gatti'],
];

let regress = 0, fab = 0;
console.log('— must ANSWER (flat & AKB5) —');
for (const [l, q] of ANSWER) {
  const f = ask(q, l, false), a = ask(q, l, true);
  const bad = (f && !a);                       // flat answers, AKB5 abstains -> regression
  if (bad) regress++;
  if (show || bad) console.log(`  ${bad ? 'REGRESS' : 'ok    '} [${l}] ${q}\n          flat: ${JSON.stringify(f.slice(0, 50))}\n          akb5: ${JSON.stringify(a.slice(0, 50))}`);
}
console.log('— must ABSTAIN (flat & AKB5) —');
for (const [l, q] of ABSTAIN) {
  const a = ask(q, l, true);
  const bad = !!a;                             // AKB5 produced an answer -> fabrication
  if (bad) fab++;
  if (show || bad) console.log(`  ${bad ? 'FABRIC ' : 'ok    '} [${l}] ${q} -> akb5: ${JSON.stringify(a.slice(0, 50))}`);
}
console.log(`\n=== AKB5 gate: ${regress} regressions, ${fab} fabrications (both must be 0) ===`);
process.exit(regress === 0 && fab === 0 ? 0 : 1);
