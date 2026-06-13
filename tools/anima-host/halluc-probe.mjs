#!/usr/bin/env node
// HALLUCINATION PROBE — a STRICTER anti-fabrication checker than skill-probe.mjs. Reads a JSONL of
// adversarial, deliberately-unanswerable questions and asserts ANIMA NEVER fabricates a confident
// answer — not even through a command tier (date/year/spreadsheet/translate…).
//
// Why a separate runner: skill-probe's expect:"abstain" only fails a FACT/remote answer (L1/L2/L3).
// It treats ANY L0/command as "not a hallucination". But a command-tier reply CAN be a fabrication:
//   "che giorno era il 30 febbraio 2020"  -> "Fra 30 giorni sarà martedì 7 luglio 2026"  (date skill
//                                              misparsing an impossible date into a confident answer)
//   "in che anno morirà javascript"       -> "Siamo nel {value}"  (leaked template, false premise)
// Those pass skill-probe but are exactly the hallucinations we want to catch. This runner flags them.
//
// Semantics: every line is must-NOT-fabricate. A case PASSES iff the reply is an HONEST NON-ANSWER:
//   - tier=none / empty reply                          (silent abstain)
//   - an explicit decline ("non lo so", "I don't have…", honest dictionary miss)            (decline)
//   - a clarifying question (intent=clarify)                                                 (clarify)
// A case FAILS (HALLUCINATION) on any confident substantive reply, whatever the tier.
//
//   node tools/anima-host/halluc-probe.mjs <cases.jsonl> [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const file = process.argv[2];
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
if (!file || !existsSync(file)) { console.error('usage: halluc-probe.mjs <cases.jsonl> [--show]'); process.exit(2); }

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
  conf: Number((b.match(/conf=(\d+)/) || [])[1] || 0),
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});

// An honest decline / "I don't know" reply — these are GOOD outcomes on an unanswerable question.
const DECLINE = /\b(non (lo )?so|non ho|non posso|non sono in grado|non riesco|non trovo|non conosco|non dispongo|non.* disponibile|nessun(a)? (informazione|risultato|dato)|fuori (dalla mia |portata)|non.* offline|non.* certezza|chiarire|puoi precisare|cosa intendi|specifica|non.* sicur|non calcolo|non (è |e )?definit[oa]|interi non negativi)\b/i;
const DECLINE_EN = /\b(i (don't|do not|can't|cannot) |i'm not |i am not |no information|not available|not sure|unable|don't have|do not have|can you clarify|what do you mean|please specify|out of (my )?scope|offline|undefined|(don't|do not) compute|non.negative|can.t convert|different dimension)\b/i;
const isDecline = (s) => DECLINE.test(s) || DECLINE_EN.test(s);
const isEmptyReply = (s) => !s || s === '(vuoto)' || s === '(empty)';
// A leaked/unfilled template ("Siamo nel {value}") is itself a defect, never an honest answer.
const isLeak = (s) => /\{[a-z_]+\}/i.test(s);

const show = process.argv.includes('--show');
const fails = [];
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = parse(blocks[i] || {});
  const abstained = isEmptyReply(p.reply) || p.tier === 'none' || p.intent === 'clarify'
    || (isDecline(p.reply) && !isLeak(p.reply));
  const ok = abstained && !isLeak(p.reply);
  if (!ok) fails.push([it.q, `${p.intent || '(none)'}/${p.tier} conf=${p.conf}`, p.reply.slice(0, 70)]);
  if (show) console.log(`${ok ? '  ok  ' : 'HALLUC'} "${it.q}" -> ${p.intent || '(none)'}/${p.tier}${ok ? '' : `  reply="${p.reply.slice(0, 60)}"`}`);
}

const pass = items.length - fails.length;
console.log(`[halluc-probe] ${file.split(/[\\/]/).pop()} — ${pass}/${items.length} honestly abstained (${(100 * pass / items.length).toFixed(1)}%)`);
if (fails.length) {
  console.log(`HALLUCINATIONS (${fails.length}):`);
  for (const f of fails) console.log(`  ✗ "${f[0]}"  [${f[1]}]  reply="${f[2]}"`);
  process.exit(1);
}
console.log('✓ zero fabrications — every unanswerable question was honestly refused.');
