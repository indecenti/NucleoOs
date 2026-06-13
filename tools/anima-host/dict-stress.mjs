#!/usr/bin/env node
// DICT-STRESS — proves the BULK offline bilingual dictionary (FreeDict, ~60k entries per direction, binary-
// searched on SD) works end-to-end on the real exe, in BOTH languages, with zero hallucination:
//   (A) RECALL — sample real headwords from each sorted TSV and require "traduci/translate" returns the
//       dictionary's own value (grounded ground-truth, auto-derived — no hand list to drift).
//   (B) JUNK — invented non-words must ABSTAIN ("Non ho X nel dizionario"), never invent a translation.
//   (C) CROSS-SKILL — weather words as a translation OBJECT ("traduci sole in inglese") must TRANSLATE,
//       not fire the forecast. HARD gate: recall >= floor AND 0 junk-answered AND 0 weather-misroute.
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const SD = join(here, 'sd', 'data', 'anima');
const verbose = process.argv.includes('--verbose');
if (!existsSync(exe)) { console.error('anima.exe missing — build first.'); process.exit(2); }

const norm = (s) => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9 ]/g, ' ').replace(/\s+/g, ' ').trim();
// sample every STRIDE-th single-word entry from a sorted TSV -> {key, firstVal}
function sample(file, stride) {
  const out = [];
  if (!existsSync(file)) return out;
  const lines = readFileSync(file, 'utf8').split('\n');
  for (let i = 0; i < lines.length; i += stride) {
    const l = lines[i]; const t = l.indexOf('\t'); if (t < 0) continue;
    const key = l.slice(0, t), val = l.slice(t + 1).split(',')[0].trim();
    if (!key || key.includes(' ') || !val || val.includes(' ')) continue;   // single-word, clean
    if (key.length < 3 || /[0-9]/.test(key)) continue;
    out.push({ key, val });
  }
  return out;
}
// --deep (or DICT_DEEP=1): dense sampling + explicit edge keys (first/last/short/long/multi-word) to
// stress the binary search across the whole file — a mismatch means a NEIGHBOUR's value was returned
// (a wrong-entry hallucination), the worst dictionary failure mode.
const deep = process.argv.includes('--deep') || process.env.DICT_DEEP;
const STR_IT = deep ? 211 : 1511, STR_EN = deep ? 197 : 1499, CAP = deep ? 400 : 40;
function edges(file) {                                  // first/last + short/long — CLEAN single-word content
  if (!existsSync(file)) return [];                     // keys only. Multi-word idioms ("a caso") and numeric
  const rows = readFileSync(file, 'utf8').split('\n').filter((l) => l.indexOf('\t') > 0);  // keys ("100 metres")
  const mk = (l) => { const t = l.indexOf('\t'); const k = l.slice(0, t), v = l.slice(t + 1).split(',')[0].trim(); return { key: k, val: v }; };
  // are deliberately not recall targets: the translator trims leading function words and declines numbers.
  const single = rows.filter((l) => { const k = l.slice(0, l.indexOf('\t')); return k && !k.includes(' ') && !/[0-9]/.test(k) && k.length >= 3; });
  const byLen = [...single].sort((a, b) => a.length - b.length);
  const pick = [single[0], single[single.length - 1], ...byLen.slice(0, 4), ...byLen.slice(-4)];
  return pick.filter(Boolean).map(mk).filter((e) => e.key && e.val);
}
const itEn = [...sample(join(SD, 'dict-it-en.tsv'), STR_IT).slice(0, CAP), ...edges(join(SD, 'dict-it-en.tsv'))];
const enIt = [...sample(join(SD, 'dict-en-it.tsv'), STR_EN).slice(0, CAP), ...edges(join(SD, 'dict-en-it.tsv'))];
const JUNK = ['xqzwk', 'blorptano', 'gskdfh', 'zzxqwy', 'flunzible', 'qwertzu'];
const WX   = ['sole', 'pioggia', 'tempo', 'vento', 'neve', 'nuvola'];
// HOMOGRAPHS valid in BOTH languages: an explicit direction must NOT confidently emit one reading
// ("translate male to english" -> "evil"); it must flag both (the adversarial workflow's vector).
const HOMO = ['male', 'estate', 'fame', 'camera', 'come', 'sole'];

const cases = [];
for (const e of itEn) cases.push({ kind: 'recall', q: `traduci ${e.key} in inglese`, want: e.val });
for (const e of enIt) cases.push({ kind: 'recall', q: `traduci ${e.key} in italiano`, want: e.val });
for (const w of JUNK) cases.push({ kind: 'junk', q: `traduci ${w} in inglese` });
for (const w of WX)   cases.push({ kind: 'weather', q: `traduci ${w} in inglese` });
// Omografo SENZA direzione = sorgente ambigua -> deve mostrare entrambi i sensi (mai un singolo). CON
// direzione esplicita ("in inglese") la sorgente NON e' ambigua -> traduzione direzionale (vedi recall).
for (const w of HOMO) cases.push({ kind: 'homo', q: `traduci ${w}` });

const lines = [];
for (const c of cases) { lines.push('/reset'); lines.push(c.q); }
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024, env: { ...process.env } });
const byQ = new Map();
for (const b of r.stdout.toString('utf8').split(/^Q: /m).slice(1)) {
  const q = norm(b.split('\n')[0]);
  const intent = (b.match(/intent=(\S*)/) || [])[1] || '';
  let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || '';
  reply = reply.replace(/\s+/g, ' ').trim(); if (reply === '(vuoto)' || reply === '(empty)') reply = '';
  byQ.set(q, { intent, reply, answered: reply !== '' });
}

// a clean translation cell never contains a lone junk letter (" E") or any non-Latin script (Cyrillic/CJK).
const dirty = (s) => /[^\x00-\x7fÀ-ÖØ-öø-ÿ]/.test(s) || /(^|[,:;]\s*)[A-Z](\s*[,.;]|$)/.test(s);
let recallOk = 0, recallN = 0, junkBad = 0, wxBad = 0, homoBad = 0, dirtyBad = 0;
const fails = [];
for (const c of cases) {
  const b = byQ.get(norm(c.q)) || { answered: false, reply: '', intent: '' };
  if (b.answered && b.intent === 'translate' && dirty(b.reply)) { dirtyBad++; fails.push({ k: 'DIRTY-DATA', q: c.q, got: b.reply.slice(0, 50) }); }
  if (c.kind === 'recall') {
    recallN++;
    if (b.answered && b.intent === 'translate' && norm(b.reply).includes(norm(c.want))) recallOk++;
    else if (verbose) fails.push({ k: 'miss', q: c.q, want: c.want, got: b.reply.slice(0, 40) });
  } else if (c.kind === 'junk') {
    if (b.intent === 'translate' && /in (inglese|italiano|english|italian):/i.test(b.reply)) { junkBad++; fails.push({ k: 'JUNK-ANSWERED', q: c.q, got: b.reply.slice(0, 40) }); }
  } else if (c.kind === 'weather') {
    if (b.intent !== 'translate') { wxBad++; fails.push({ k: 'WEATHER-MISROUTE', q: c.q, got: (b.intent || 'none') + ':' + b.reply.slice(0, 30) }); }
  } else { // homograph: must flag BOTH languages, never a single confident reading
    if (!/entrambe|both languages/i.test(b.reply)) { homoBad++; fails.push({ k: 'HOMOGRAPH-CONFIDENT', q: c.q, got: b.reply.slice(0, 45) }); }
  }
}
const floor = Math.ceil(recallN * 0.85);
console.log(`[dict-stress] recall ${recallOk}/${recallN} (floor ${floor})  |  junk ${junkBad}  |  weather ${wxBad}  |  homograph-confident ${homoBad}  |  dirty-data ${dirtyBad}`);
if (fails.length && verbose) for (const f of fails) console.log('  ' + JSON.stringify(f));
const ok = recallOk >= floor && junkBad === 0 && wxBad === 0 && homoBad === 0 && dirtyBad === 0;
console.log(ok ? '[dict-stress] ✓ bulk bilingual dictionary: grounded recall + 0 junk + 0 weather + 0 homograph-confident + 0 dirty'
              : `[dict-stress] FAIL — recall ${recallOk}/${recallN} junk ${junkBad} weather ${wxBad} homo ${homoBad} dirty ${dirtyBad}`);
process.exit(ok ? 0 : 1);
