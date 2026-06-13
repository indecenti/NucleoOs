#!/usr/bin/env node
// REALISTIC NL REQUESTS — drives the FULLY-PROGRAMMATIC offline cascade (anima.exe, linked with
// anima_online_stub.c → NO LLM, NO internet) over a corpus of real user requests, many carrying a small
// QWERTY fast-typing typo. Three kinds, each with the right deterministic assertion:
//   kind:"skill"     → must fire the expected deterministic intent (calc/convert/base/roman/scale/open_app/
//                      create_file/set_volume/agenda/profile/translate/date/…). expect = intent (pipe = alt).
//   kind:"knowledge" → must stay in the KNOWLEDGE lane (l1/mosaico/facet/hdc/combinator) or HONESTLY abstain
//                      — never fire a compute/action skill, never leak a {template}. (The small offline
//                      corpus may not know it; an honest abstain is the correct programmatic outcome.)
//   kind:"halluc"    → must abstain (kept in eval_halluc3_* / halluc-suite; handled here too if present).
// Deterministic + re-runnable: fixed JSONL, no RNG, no clock-coupled cases; parsed from the exe's own stdout.
//
//   node tools/anima-host/realistic.mjs [file.jsonl ...] [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

const FILES = process.argv.slice(2).filter(a => a.endsWith('.jsonl'));
if (!FILES.length) FILES.push(join(repo, 'tools', 'anima', 'eval_realistic_it.jsonl'), join(repo, 'tools', 'anima', 'eval_realistic_en.jsonl'));

const items = [];
for (const f of FILES) { if (!existsSync(f)) continue;
  for (const l of readFileSync(f, 'utf8').split(/\r?\n/)) { if (!l.trim() || l.startsWith('//')) continue;
    try { const o = JSON.parse(l); if (o.q) items.push(o); } catch {} } }

// knowledge-lane intents (a fact question may legitimately resolve to any of these) + abstain.
const KNOW = new Set(['l1', 'mosaico', 'facet', 'hdc', 'combinator', 'whoami', '']);
// every COMPUTE/ACTION skill intent — for kind:"robust", firing the WRONG one of these (a misroute) is the
// only failure; the right one OR a safe non-skill outcome (knowledge/abstain) both pass.
const SKILL_ALL = new Set(['calc', 'geo', 'phys', 'ohm', 'convert', 'percent', 'base', 'roman', 'prime', 'scale',
  'spreadsheet', 'open_app', 'close_app', 'create_file', 'set_volume', 'set_brightness', 'agenda', 'add_event',
  'profile', 'teach', 'translate', 'date', 'time', 'year', 'season', 'battery', 'version', 'weather', 'storage', 'network']);
const isLeak = (s) => /\{[a-z_]+\}/i.test(s);
const DECLINE = /\b(non (lo )?so|non ho|non posso|non riesco|non trovo|non conosco|non.* disponibile|nessun|non.* certezza|chiarire|non calcolo|i (don't|do not|can't|cannot)|i'm not|no information|not available|unable|don't have)\b/i;
const isAbstain = (p) => !p.reply || p.reply === '(vuoto)' || p.tier === 'none' || p.intent === 'clarify' || (DECLINE.test(p.reply) && !isLeak(p.reply));

let lang = 'it'; const lines = [];
for (const it of items) { lines.push('/reset'); const w = it.lang === 'en' ? 'en' : 'it'; if (w !== lang) { lines.push('/' + w); lang = w; } lines.push(it.q); }
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 128 * 1024 * 1024 });
const rows = r.stdout.toString('utf8').split(/^Q: /m).slice(1).map((b) => ({
  tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
}));
const intentEq = (p) => (p.intent === 'mosaico' ? 'l1' : p.intent);

const show = process.argv.includes('--show');
const fails = [], byKind = {};
let typos = 0;
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = rows[i] || { tier: 'none', intent: '', reply: '' };
  if (it.typo) typos++;
  byKind[it.kind] = byKind[it.kind] || { n: 0, fail: 0 };
  byKind[it.kind].n++;
  let ok;
  // for a skill, the INTENT firing is the assertion; a "{value}" sentinel from a system-readout intent
  // (agenda/time/battery/version/year…) is filled by the executor on the device, so it is not a defect here.
  if (it.kind === 'skill') ok = String(it.expect || '').split('|').map(s => s.trim()).includes(intentEq(p));
  else if (it.kind === 'knowledge') ok = !isLeak(p.reply) && (KNOW.has(intentEq(p)) || isAbstain(p));
  // ROBUST = stress cases (strange phrasing, deliberate logical typos, worded numbers): ANIMA MAY handle
  // them (fire the ideal skill) OR safely degrade (knowledge/abstain). The ONLY failure is fabricating — a
  // leaked template OR a misroute to the WRONG compute/action skill. Stays valid if ANIMA later improves.
  else if (it.kind === 'robust') ok = !isLeak(p.reply)
    && (String(it.expect || '').split('|').map(s => s.trim()).includes(intentEq(p)) || !SKILL_ALL.has(intentEq(p)));
  else /* halluc */ ok = isAbstain(p) && !isLeak(p.reply);
  if (!ok) { byKind[it.kind].fail++; fails.push([it.kind, it.q, it.expect || '-', `${p.intent || '(none)'}/${p.tier}`, p.reply.slice(0, 50)]); }
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL '} [${it.kind}${it.typo ? '/typo' : ''}] "${it.q}" -> ${p.intent || '(abstain)'}/${p.tier}`);
}

const pass = items.length - fails.length;
console.log(`\n[realistic] ${items.length} programmatic requests (${typos} with QWERTY typos) — ${pass}/${items.length} pass`);
for (const [k, s] of Object.entries(byKind)) console.log(`  ${k.padEnd(10)} ${s.n - s.fail}/${s.n}`);
if (fails.length) { console.log(`FAILURES (${fails.length}):`);
  for (const f of fails.slice(0, 50)) console.log(`  ✗ [${f[0]}] "${f[1]}" expect=${f[2]} got=${f[3]}  reply="${f[4]}"`); }
console.log(fails.length ? `\n[realistic] FAIL — ${fails.length} mismatch` : `\n[realistic] ✓ every programmatic request routed correctly (skills fire, knowledge stays grounded/abstains, no fabrication).`);
process.exit(fails.length ? 1 : 0);
