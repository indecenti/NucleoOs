#!/usr/bin/env node
// WHICH-PERSON gate — a shared surname must be asked about, never guessed at.
//
// The defect this pins down was not a hallucination, which is what makes it worth a gate of its own.
// Ten people in the corpus are called Trump, four Kennedy, four King. Asked "chi è Trump", retrieval
// answered with whichever namesake scored highest and stated it as fact: Frederick Christ Trump Sr.
// at 82% confidence, and Ethel Kennedy at 80% for "chi è Kennedy". Every card was real; every gate
// stayed green; the answer was still a coin flip wearing the costume of a verified one.
//
// Cosine cannot separate these cases — "chi è Kennedy" beats its runner-up by 0.186, looking
// confident while being the wrong Kennedy — so the decision is made from an exact corpus fact
// instead (anima_person_ambig.h). What this gate checks:
//
//   1. ASKS WHEN AMBIGUOUS   — a bare shared surname returns a question naming real people.
//   2. ANSWERS WHEN NOT      — a full name, and a surname only one person carries, still answer.
//                              A clarify that fires too often is a worse assistant, not a safer one.
//   3. OFFERS ONLY REAL CARDS— every name offered must be answerable on its own. This is the
//                              zero-hallucination property: a question may not invent a person.
//   4. RESOLVES              — an ordinal picks the person, and picking still yields THAT person.
//   5. NEVER ASSERTS         — the clarify turn must be a pending question (awaiting), not a fact.
//
//   node tools/anima-host/person-clarify-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

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
      tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
      intent: (b.match(/intent=(\S*)/) || [])[1] || '',
      awaiting: /\[attende follow-up\]/.test(b) || /awaiting/.test(b),
      reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
    };
  });
}

const fails = [];
const fail = (what, detail) => fails.push(`${what}: ${detail}`);
const isClarify = (r) => r.intent === 'clarify';

// ---- 1. asks when the surname is genuinely shared ---------------------------------------------
// Each expects a question that NAMES the person the user most likely meant, so the fix is not merely
// "stopped answering" — it has to stay useful.
const AMBIGUOUS = [
  { q: 'chi è Trump', lang: 'it', must: 'Donald Trump' },
  { q: 'who is Trump', lang: 'en', must: 'Donald Trump' },
  { q: 'chi è Kennedy', lang: 'it', must: 'John F. Kennedy' },
  { q: 'who was Kennedy', lang: 'en', must: 'John F. Kennedy' },
  { q: 'chi è King', lang: 'it', must: 'Martin Luther King' },
  { q: 'chi è Williams', lang: 'it', must: 'Serena Williams' },
  { q: 'chi è Washington', lang: 'it', must: 'George Washington' },
  { q: 'cosa sai di Jackson', lang: 'it', must: 'Michael Jackson' },
];
const amb = drive(AMBIGUOUS);
for (const r of amb) {
  if (!isClarify(r)) { fail('does not ask', `"${r.q}" -> ${r.tier} "${r.reply.slice(0, 70)}"`); continue; }
  if (!r.awaiting) fail('asks but does not wait', `"${r.q}"`);
  if (!r.reply.includes(r.must)) fail('asks without offering the likely person', `"${r.q}" omits ${r.must}: ${r.reply}`);
}

// ---- 2. answers when the query is not ambiguous ------------------------------------------------
// The generational suffix is the whole of what separates some of these, so they are the sharp cases.
const UNAMBIGUOUS = [
  { q: 'chi è Donald Trump', lang: 'it' },
  { q: 'who is Donald Trump', lang: 'en' },
  { q: 'chi è Donald Trump Jr', lang: 'it' },
  { q: 'chi è Robert Kennedy', lang: 'it' },
  { q: 'chi è Stephen King', lang: 'it' },
  { q: 'chi è Michael Jordan', lang: 'it' },
  { q: 'chi è Michael B Jordan', lang: 'it' },
  { q: 'chi è Serena Williams', lang: 'it' },
  { q: 'chi è einstein', lang: 'it' },          // only one Einstein exists: never a question
  { q: 'chi è George Washington', lang: 'it' },
];
for (const r of drive(UNAMBIGUOUS)) {
  if (isClarify(r)) fail('asks needlessly', `"${r.q}" -> ${r.reply}`);
  else if (!r.reply || r.tier === 'none') fail('stopped answering', `"${r.q}" -> ${r.tier}`);
}

// ---- 3. every offered name is a real, answerable card ------------------------------------------
// A clarify is only honest if each option can be chosen and answered. Parse the options back out of
// the questions from step 1 and ask each one as its own query.
const offered = [];
for (const r of amb) {
  if (!isClarify(r)) continue;
  // Cut the trailing "(N more carry that name)" / "?" first, then split on the "1) 2) 3)" markers —
  // parsing by marker rather than by spacing, so the check does not break on a reworded question.
  const body = r.reply.replace(/\s*\([^)]*\)\s*$/, '').replace(/\?\s*$/, '');
  for (const name of body.split(/\s*\d\)\s*/).slice(1)) {
    const clean = name.trim();
    if (clean) offered.push({ q: `chi è ${clean}`, lang: 'it', from: r.q, name: clean });
  }
}
if (offered.length < 8) fail('parsed too few options', `${offered.length} — the clarify format changed`);
for (const r of drive(offered)) {
  if (r.tier === 'none' || !r.reply || /^Non lo so/.test(r.reply))
    fail('offers a person it cannot answer', `"${r.from}" offered "${r.name}" -> ${r.tier} "${r.reply}"`);
  if (isClarify(r)) fail('offers an option that is itself ambiguous', `"${r.from}" offered "${r.name}"`);
}

// ---- 4. the pick resolves to the person that was offered ---------------------------------------
// The Guinness cases are the sharp ones. "chi è Arthur Guinness" narrows five Guinnesses down to the
// two Arthurs, who sit at positions 2 and 3 of the surname group — so an implementation that derived
// the pick from its ORDINAL would answer "Guinness family" for option 1. It also asks about a name
// that is itself shared, which must resolve rather than re-ask the same question forever.
const PICKS = [
  { ask: 'chi è Trump', pick: '1', want: /Donald John Trump/i, notwant: /Trump Jr/i },
  { ask: 'chi è Trump', pick: '3', want: /Trump Jr/i },
  { ask: 'chi è Kennedy', pick: '1', want: /John Fitzgerald Kennedy/i },
  { ask: 'chi è Washington', pick: '2', want: /George Washington/i },
  { ask: 'chi è Arthur Guinness', pick: '2', want: /Ardilaun/i, notwant: /famiglia irlandese|Guinness family/i },
];
for (const p of PICKS) {
  const [, res] = drive([{ q: p.ask, lang: 'it' }, { q: p.pick, lang: 'it', reset: false }]);
  if (isClarify(res)) { fail('pick did not resolve', `"${p.ask}" + "${p.pick}" still asking`); continue; }
  if (!p.want.test(res.reply)) fail('pick resolved to the wrong person', `"${p.ask}" + "${p.pick}" -> ${res.reply.slice(0, 90)}`);
  if (p.notwant && p.notwant.test(res.reply)) fail('pick bled into a namesake', `"${p.ask}" + "${p.pick}" -> ${res.reply.slice(0, 90)}`);
}

// Answering with the NAME instead of a number must work too — a clarify the user answers in prose is
// still an answered clarify, and dropping it into a dead end would be worse than never asking.
{
  const [, res] = drive([{ q: 'chi è Kennedy', lang: 'it' }, { q: 'John F. Kennedy', lang: 'it', reset: false }]);
  if (!/John Fitzgerald Kennedy/i.test(res.reply)) fail('name answer to a clarify not honoured', res.reply.slice(0, 90));
}

// A pick must never hand back the same question. The chosen name can itself be shared, so resolving
// it by re-asking has to suppress the clarify for that turn or the dialogue loops.
for (const ask of ['chi è Arthur Guinness', 'chi è Trump', 'chi è Kennedy']) {
  const [, res] = drive([{ q: ask, lang: 'it' }, { q: '1', lang: 'it', reset: false }]);
  if (isClarify(res)) fail('pick loops back into the same question', `"${ask}" + "1" -> ${res.reply.slice(0, 80)}`);
}

// The "N more carry that name" tail counts CANDIDATES, not the surname group: after narrowing five
// Guinnesses to two Arthurs, promising three more would point at people who cannot be the answer.
{
  const [r] = drive([{ q: 'chi è Arthur Guinness', lang: 'it' }]);
  if (/altre|more carr|more carries/i.test(r.reply)) fail('counts non-candidates in the tail', r.reply.slice(0, 90));
}

// ---- 5. the clarify is a question, never a stated fact -----------------------------------------
// A pending clarify carries no confidence: asserting 82% next to "which one do you mean" is exactly
// the confusion the whole change exists to remove.
for (const r of amb) {
  if (!isClarify(r)) continue;
  if (/^(Frederick|Eric|Ethel|Melania|Barron|Tiffany)/.test(r.reply))
    fail('clarify leads with an asserted namesake', r.reply.slice(0, 90));
}

// ---- report ------------------------------------------------------------------------------------
if (show) for (const r of amb) console.log(`  ${r.q}\n    ${r.reply}`);
if (fails.length) {
  console.error('WHICH-PERSON FAILED');
  for (const f of fails) console.error('  ✗ ' + f);
  process.exit(1);
}
console.log(`✓ which-person: ${AMBIGUOUS.length} ambiguous asked, ${UNAMBIGUOUS.length} unambiguous answered, ` +
            `${offered.length} offered names all answerable, ${PICKS.length + 1} picks resolved`);
