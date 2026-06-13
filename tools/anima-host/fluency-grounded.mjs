#!/usr/bin/env node
// ANIMA fluency-grounded gate — proves the L2 MOSAICO span-stitch realizer (docs/anima.md §"L2 …
// Deferred behind a measurement gate") is a WIN, not a regression: it must make describe/explain
// answers FULLER (more grounded text surfaced) WITHOUT inventing a single word, and WITHOUT ever
// answering a describe-shaped question about a non-existent entity.
//
// It drives the REAL compiled cascade (anima.exe) and measures three things at once:
//   1) UPLIFT     — for each describe query MOSAICO enriched, the stitched reply (default) is longer
//                   than the same query with stitch disabled (ANIMA_NO_STITCH=1). A/B on one binary.
//   2) GROUNDED   — every stitched reply is reconstructable, left-to-right, purely from VERBATIM card
//                   fields (reply/detail of the corpus) plus fixed connective glue. 0 invented spans.
//   3) SAFETY     — describe-shaped questions about fabricated entities ("parlami di Zorblax") must
//                   NOT be answered (MOSAICO only ever enriches an answer L1 already gave).
//
//   node tools/anima-host/fluency-grounded.mjs            # the gate
//   node tools/anima-host/fluency-grounded.mjs --verbose  # also print every stitched answer
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
const kdir = join(repo, 'tools', 'anima', 'knowledge');
const verbose = process.argv.includes('--verbose');
const C = { g: '\x1b[32m', r: '\x1b[31m', d: '\x1b[2m', x: '\x1b[0m' };

if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

// ---- normalization shared by groundedness + describe detection (accent-blind, keeps punctuation) --
const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
const asciiFold = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
// MUST mirror a_is_describe() in nucleo_anima.c (the cascade decides where MOSAICO may fire).
const DESCRIBE_MK = ['parlami', 'raccontami', 'descriv', 'spiega', "cos'", 'cosa ', 'che cos', 'chi ',
  'dimmi di', 'dimmi tutto', 'approfond', 'tell me about', 'what is', "what's", 'what are',
  'who is', 'who was', 'explain', 'describe'];
const isDescribe = (q) => { const s = asciiFold(q); return DESCRIBE_MK.some((m) => s.includes(m)); };

// ---- load the corpus: card fields (for groundedness) + describe-query candidates (for the eval) ----
const fieldSet = new Set();          // every verbatim reply/detail field, normalized
const candidates = [];               // {q, lang, id}
let cardCount = 0, detailCards = 0;
for (const f of readdirSync(kdir).filter((n) => n.endsWith('.jsonl')).sort()) {
  for (const line of readFileSync(join(kdir, f), 'utf8').split(/\r?\n/)) {
    const t = line.trim();
    if (!t || t.startsWith('//')) continue;
    let c; try { c = JSON.parse(t); } catch { continue; }
    cardCount++;
    const action = c.action || 'answer';
    for (const lang of ['it', 'en']) {
      for (const k of ['reply', 'detail']) {
        const v = c[k] && c[k][lang];
        if (typeof v === 'string' && v.trim().length >= 4) fieldSet.add(norm(v));
      }
    }
    if (action !== 'answer') continue;
    const hasDetail = (c.detail && (c.detail.it || c.detail.en));
    if (!hasDetail) continue;
    detailCards++;
    for (const lang of ['it', 'en']) {
      const asks = (c.ask && Array.isArray(c.ask[lang])) ? c.ask[lang] : [];
      for (const a of asks) if (typeof a === 'string' && isDescribe(a)) candidates.push({ q: a, lang, id: c.id });
    }
  }
}
// stable, deduped, balanced sample (cap for speed — l1-parity drives ~130, we stay similar)
const seen = new Set();
const uniq = candidates.filter((c) => { const k = c.lang + '|' + norm(c.q); if (seen.has(k)) return false; seen.add(k); return true; })
  .sort((a, b) => (a.id + a.q).localeCompare(b.id + b.q));
const CAP = 140;
const evalSet = uniq.slice(0, CAP);

// fixed SAFETY probes: describe-shaped questions about fabricated entities — must never be answered.
const fakes = [
  { q: 'parlami di Zorblax di Marte', lang: 'it' }, { q: "cos'è il flarbonio quantico", lang: 'it' },
  { q: 'chi è Aldric Venmoor', lang: 'it' }, { q: 'spiegami il gloptrone a tre fasi', lang: 'it' },
  { q: "cos'è la quaxite siberiana", lang: 'it' },
  { q: 'what is a glorptron', lang: 'en' }, { q: 'tell me about Zandar Quux', lang: 'en' },
  { q: 'who is Marnixel Vorbo', lang: 'en' }, { q: 'explain the florbnium isotope', lang: 'en' },
];

// ---- REPL driver (mirrors l1-parity.mjs) -------------------------------------------------------
function drive(items, extraEnv = {}) {
  let lang = 'it';
  const lines = [];
  for (const it of items) {
    lines.push('/reset');
    const want = it.lang === 'en' ? 'en' : 'it';
    if (want !== lang) { lines.push('/' + want); lang = want; }
    lines.push(it.q);
  }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024, env: { ...process.env, ...extraEnv } });
  const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
  return blocks.map((b) => {
    const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
    const intent = (b.match(/intent=(\S*)/) || [])[1] || '';
    let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/) || [])[1] || '';
    reply = reply.replace(/\s+/g, ' ').trim();
    if (reply === '(vuoto)' || reply === '(empty)') reply = '';
    return { tier, intent, reply };
  });
}

// ---- groundedness: consume the reply left-to-right with verbatim card fields + connective glue ----
const FIELDS = [...fieldSet].filter((f) => f.length >= 6).sort((a, b) => b.length - a.length);
const stripLead = (s) => s.replace(/^[\s.;:,!?]+/, '').replace(/^(?:inoltre|also)\b,?\s*/, '').replace(/^[\s.;:,!?]+/, '');
function grounded(reply) {
  let rem = stripLead(norm(reply));
  let guard = 0;
  while (rem.length && guard++ < 60) {
    let best = '';
    for (const f of FIELDS) { if (f.length > best.length && rem.startsWith(f)) best = f; }
    if (!best) return { ok: false, leftover: rem.slice(0, 60) };
    rem = stripLead(rem.slice(best.length));
  }
  return { ok: rem.length === 0, leftover: rem.slice(0, 60) };
}

// ---- run ---------------------------------------------------------------------------------------
console.log(`[fluency-grounded] corpus ${cardCount} cards (${detailCards} with detail), ${FIELDS.length} grounding fields; driving ${evalSet.length} describe candidates x2 ...`);
const on = drive(evalSet);                              // MOSAICO on (default)
const keptIdx = [];
for (let i = 0; i < evalSet.length; i++) if (on[i] && on[i].tier === 'L2/stitch') keptIdx.push(i);
const kept = keptIdx.map((i) => evalSet[i]);
const off = drive(kept, { ANIMA_NO_STITCH: '1' });     // baseline: same queries, stitch disabled

// GROUNDEDNESS is judged on MOSAICO's CONTRIBUTION only: the part it ADDED (ON − OFF). The baseline
// (OFF) may legitimately carry non-card text from other tiers (e.g. an interactive skill offer like
// "I can also compute vector sum — give me the components"); that's not MOSAICO's to account for. The
// claim under test is precise: MOSAICO appends only VERBATIM card spans. So ON must start with OFF
// (purely additive) and the appended delta must reconstruct from corpus fields + connective glue.
let upliftFails = 0, groundFails = 0, additiveFails = 0; const uplifts = [];
for (let j = 0; j < kept.length; j++) {
  const i = keptIdx[j];
  const onR = on[i].reply, offR = (off[j] && off[j].reply) || '';
  const onLen = onR.length, offLen = offR.length;
  const up = offLen > 0 ? (onLen - offLen) / offLen : (onLen > 0 ? 1 : 0);
  uplifts.push(up);
  if (!(onLen > offLen)) { upliftFails++; console.log(`  ${C.r}[UPLIFT]${C.x} "${kept[j].q}" on=${onLen} off=${offLen}`); }
  const additive = offR && onR.startsWith(offR);
  if (!additive && offR) { additiveFails++; console.log(`  ${C.r}[NON-ADDITIVE]${C.x} "${kept[j].q}" — MOSAICO did not purely append`); }
  const judged = additive ? onR.slice(offR.length) : onR;   // delta if additive, else the whole reply
  const g = grounded(judged);
  if (!g.ok) { groundFails++; console.log(`  ${C.r}[UNGROUNDED]${C.x} "${kept[j].q}"\n      leftover: …${g.leftover}…\n      added: ${judged.slice(0, 140)}`); }
  if (verbose) console.log(`  ${kept[j].lang} | ${kept[j].q}  ->  +${Math.round(up * 100)}%  ${g.ok ? '✓grounded' : '✗'}\n      ${onR}`);
}

// safety: fabricated entities must NOT be answered
const safe = drive(fakes);
let safetyFails = 0;
for (let i = 0; i < fakes.length; i++) {
  const p = safe[i] || { tier: 'none', reply: '' };
  const answered = (p.tier === 'L1/fact' || p.tier === 'L2/stitch' || p.tier === 'L3/remote') && p.reply;
  if (answered) { safetyFails++; console.log(`  ${C.r}[FABRICATED]${C.x} "${fakes[i].q}" -> ${p.tier} "${p.reply.slice(0, 60)}"`); }
}

const avgUp = uplifts.length ? Math.round((uplifts.reduce((a, b) => a + b, 0) / uplifts.length) * 100) : 0;
const minUp = uplifts.length ? Math.round(Math.min(...uplifts) * 100) : 0;
const MIN_KEPT = 15;
const okKept = kept.length >= MIN_KEPT;
const okAll = okKept && upliftFails === 0 && groundFails === 0 && additiveFails === 0 && safetyFails === 0;

console.log(`[fluency-grounded] stitched ${kept.length}/${evalSet.length} | uplift avg +${avgUp}% (min +${minUp}%) | additive ${kept.length - additiveFails}/${kept.length} | grounded ${kept.length - groundFails}/${kept.length} | safety ${fakes.length - safetyFails}/${fakes.length}`);
if (!okKept) console.log(`  ${C.r}too few stitched (${kept.length} < ${MIN_KEPT}) — MOSAICO may be disabled/broken${C.x}`);
console.log(okAll ? `${C.g}[fluency-grounded] ✓ MOSAICO is fuller AND adds only verbatim grounded spans AND is safe${C.x}`
                  : `${C.r}[fluency-grounded] FAIL — uplift:${upliftFails} non-additive:${additiveFails} ungrounded:${groundFails} fabricated:${safetyFails}${C.x}`);
process.exit(okAll ? 0 : 1);
