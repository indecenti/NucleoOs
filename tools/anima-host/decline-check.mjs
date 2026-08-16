#!/usr/bin/env node
// HONEST-DECLINE gate — an abstention must SAY so, in the user's language, without ever becoming an
// answer. Guards the engine-level refusal added at the cascade's convergence point.
//
// Before it, tier=NONE returned an EMPTY reply and each of the three runtimes coped on its own: the
// web shell substituted its own "dontknow" string, the native bubble had a fallback literal, and the
// voice path and the host CLI simply produced nothing. Same engine, four different behaviours, three
// of them invisible to every gate. The refusal now belongs to the engine, and this gate pins it.
//
// The properties checked, in the order they matter:
//   1. NEVER SILENT   — no query may come back with an empty reply. This is the actual bug.
//   2. STILL ABSTAINED— a filled-in refusal must NOT change the tier: tier=NONE stays NONE, so every
//                       hallucination gate (which keys on the tier, never on the wording) is unaffected.
//   3. RIGHT LANGUAGE — an Italian session declines in Italian, an English one in English.
//   4. NOT A LEAK     — the refusal is a fixed sentence, never an unfilled "{template}".
//   5. NOT OVERBROAD  — a real answer must never be replaced by the refusal, and a pending clarifying
//                       question must keep ITS text (it is a question, not an abstention).
//
//   node tools/anima-host/decline-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe  = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

const DECLINE_IT = 'Non lo so.';
const DECLINE_EN = "I don't know.";

// Drive a script of {lang, q} turns. `reset` starts a fresh session before the turn (default true) —
// set it false to chain turns, which the clarify case needs.
function drive(turns) {
  const lines = [];
  let lang = 'it';
  for (const t of turns) {
    if (t.reset !== false) lines.push('/reset');
    const want = t.lang === 'en' ? 'en' : 'it';
    if (want !== lang) { lines.push('/' + want); lang = want; }
    lines.push(t.q);
  }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
  const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
  return turns.map((t, i) => {
    const b = blocks[i] || '';
    return {
      ...t,
      tier:     (b.match(/tier=(\S+)/) || [])[1] || 'none',
      awaiting: /\[attende follow-up\]/.test(b),
      reply:    ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
    };
  });
}
const isEmpty = (s) => !s || s === '(vuoto)' || s === '(empty)';
const fails = [];
const fail = (what, detail) => fails.push(`${what}: ${detail}`);

// ---- 1-4. the invariant, over the real adversarial fixtures -----------------------------------
const fixtures = [
  ['tools/anima/eval_halluc_it.jsonl', 'it'],
  ['tools/anima/eval_halluc_en.jsonl', 'en'],
  ['tools/anima/eval_halluc2_it.jsonl', 'it'],
  ['tools/anima/eval_halluc2_en.jsonl', 'en'],
];
const turns = [];
for (const [rel, lang] of fixtures) {
  const p = join(here, '..', '..', rel);
  if (!existsSync(p)) continue;
  for (const line of readFileSync(p, 'utf8').split(/\r?\n/)) {
    if (!line.trim() || line.startsWith('//')) continue;
    const it = JSON.parse(line);
    turns.push({ q: it.q, lang: it.lang || lang });
  }
}
if (turns.length < 20) { console.error('adversarial fixtures missing — nothing to check'); process.exit(2); }

const res = drive(turns);
let silent = 0, declined = 0;
for (const r of res) {
  if (isEmpty(r.reply)) { silent++; if (silent <= 5) fail('SILENT', `"${r.q}" [${r.tier}] returned nothing`); continue; }
  if (/\{[a-z_]+\}/i.test(r.reply)) fail('LEAK', `"${r.q}" -> ${r.reply.slice(0, 60)}`);
  if (r.tier !== 'none') continue;
  declined++;
  const want = r.lang === 'en' ? DECLINE_EN : DECLINE_IT;
  const other = r.lang === 'en' ? DECLINE_IT : DECLINE_EN;
  if (r.reply === other) fail('WRONG LANGUAGE', `"${r.q}" (${r.lang}) -> "${r.reply}"`);
  if (show) console.log(`  [${r.lang}] "${r.q}" -> ${r.tier} "${r.reply.slice(0, 60)}"`);
  void want;
}
console.log(`[decline] ${res.length} adversarial turns — ${declined} abstained (tier=none), ${silent} silent`);

// ---- 2 + 3. the exact template on a guaranteed miss --------------------------------------------
// Deliberate gibberish: no tier can claim it, so tier=NONE is certain and the wording is pinned.
const gib = drive([
  { q: 'asdkfj qwerty zzz', lang: 'it' },
  { q: 'zzqq wubba lorem xyzzy', lang: 'it' },
  { q: 'asdkfj qwerty zzz', lang: 'en' },
  { q: 'zzqq wubba lorem xyzzy', lang: 'en' },
]);
for (const g of gib) {
  const want = g.lang === 'en' ? DECLINE_EN : DECLINE_IT;
  if (g.tier !== 'none') fail('TIER MOVED', `"${g.q}" (${g.lang}) answered at ${g.tier} — the refusal must not change routing`);
  else if (g.reply !== want) fail('TEMPLATE', `"${g.q}" (${g.lang}) -> "${g.reply}" (want "${want}")`);
}

// ---- 5. not overbroad --------------------------------------------------------------------------
// A real answer must survive untouched...
const good = drive([
  { q: "chi era einstein", lang: 'it' },
  { q: 'who was einstein', lang: 'en' },
  { q: 'apri il blocco note', lang: 'it' },
  { q: 'che ore sono', lang: 'it' },
]);
for (const g of good) {
  if (g.tier === 'none') fail('LOST ANSWER', `"${g.q}" no longer answers (tier=none)`);
  if (g.reply === DECLINE_IT || g.reply === DECLINE_EN) fail('OVERBROAD', `"${g.q}" was replaced by the refusal`);
}
// ...and a pending clarifying question is a QUESTION, not an abstention: it keeps its own text.
const clar = drive([{ q: 'crea un file', lang: 'it' }]);
for (const c of clar) {
  if (!c.awaiting) fail('SETUP', `"${c.q}" no longer awaits a follow-up — the clarify case needs a new example`);
  else if (c.reply === DECLINE_IT) fail('CLARIFY CLOBBERED', `"${c.q}" -> "${c.reply}" instead of its question`);
}

// ---- report ------------------------------------------------------------------------------------
if (fails.length) {
  console.log(`DECLINE DEFECTS (${fails.length}):`);
  for (const f of fails.slice(0, 20)) console.log(`  ✗ ${f}`);
  process.exit(1);
}
console.log('✓ every abstention speaks, in the right language, without becoming an answer.');
