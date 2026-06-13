#!/usr/bin/env node
// ANIMA METAMORPHIC NL STRESS — covers FAR more cases WITHOUT adding new phrases. It takes the ~485
// curated UNANSWERABLE seeds (eval_halluc_it/en + eval_halluc2_it/en) and applies SEMANTICS-PRESERVING
// transformations (typo / CAPS / dropped-accents / punctuation / politeness lead-in / filler / wrapper
// code-switch / commutative reorder), then asserts the METAMORPHIC LAW:
//
//     verdict(mutate(seed)) == verdict(seed) == ABSTAIN     for every seed and every safe operator
//
// A mutant that flips abstain -> a confident answer/launch/system reading, or leaks a {template}, is a
// hallucination the static suite missed — exactly the adversary's real input (typos, no accents, CAPS,
// "per favore puoi dirmi…"). It found, e.g., dropped-accent "morira"→spellfix "mostra"→launch code-runner,
// and typo "febbriao" defeating the exact-string month guard. Cross-substrate (HONEST scope):
//   M1 offline cascade   — FULLY exercised here over the real anima.exe (the host links anima_online_stub.c)
//   M3 online routing     — DETECTION invariance is host-proven via entity.mjs::extractEntity (a mutant must
//                           never route to a DIFFERENT answerable entity); the live Grok/Wikipedia answer is
//                           device/network-only and is NOT claimed green.
//   M4 WebLLM browser     — the M1 cross-substrate VERIFIER (anima.exe --verify) is host-proven to never
//                           CONFIRM a fabricated false-premise claim; real WebGPU generation is browser-only.
//   web-twin              — covered by the existing weather/translate twins + entity gate (not duplicated).
// Operators that survive the firmware normalizer (typo/accent/politeness/filler/codeswitch/reorder) genuinely
// exercise the matcher; CAPS/punct are byte-folded and mostly probe the normalizer — all are still run.
//
//   node tools/anima-host/metamorph.mjs            # the gate (exit 1 on any invariance break)
//   node tools/anima-host/metamorph.mjs --show     # list a sample of mutants + verdicts
//   node tools/anima-host/metamorph.mjs --verbose  # print every invariance break in full
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { extractEntity } from '../anima/entity.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe  = join(here, 'build', 'anima.exe');
const KDIR = join(repo, 'tools', 'anima', 'knowledge');
const SEED_FILES = ['eval_halluc_it', 'eval_halluc_en', 'eval_halluc2_it', 'eval_halluc2_en'];
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

// ---- deterministic RNG + primitives (mirrored from nl-stress.mjs; pure, no Math.random) -----------
let _s = 0x9e3779b9;
const rnd  = () => { _s = (_s * 1664525 + 1013904223) >>> 0; return _s / 0x100000000; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
// case-fold before extraction (the firmware lowercases before TRIG matching; entity.mjs is case-sensitive).
// null is always an allowed result (a mutation may erase the wrapper -> no lookup), so accent/punct that
// stop entity.mjs from matching are SAFE; only a change to a DIFFERENT non-null slug is a routing drift.
const slugOf = (s) => { try { const e = extractEntity(s.toLowerCase()); return e && e.slug ? e.slug : null; } catch { return null; } };

// ---- the anti-fabrication oracle (identical contract to halluc-probe.mjs) -------------------------
const DECLINE    = /\b(non (lo )?so|non ho|non posso|non sono in grado|non riesco|non trovo|non conosco|non dispongo|non.* disponibile|nessun(a)? (informazione|risultato|dato)|fuori (dalla mia |portata)|non.* offline|non.* certezza|chiarire|puoi precisare|cosa intendi|specifica|non.* sicur)\b/i;
const DECLINE_EN = /\b(i (don't|do not|can't|cannot) |i'm not |i am not |no information|not available|not sure|unable|don't have|do not have|can you clarify|what do you mean|please specify|out of (my )?scope|offline)\b/i;
const isDecline    = (s) => DECLINE.test(s) || DECLINE_EN.test(s);
const isEmptyReply = (s) => !s || s === '(vuoto)' || s === '(empty)';
const isLeak       = (s) => /\{[a-z_]+\}/i.test(s);
const isHonestNonAnswer = (p) => (isEmptyReply(p.reply) || p.tier === 'none' || p.intent === 'clarify'
                                 || (isDecline(p.reply) && !isLeak(p.reply))) && !isLeak(p.reply);

// A GROUNDED RELAY is a confident L1/L2 reply that is VERBATIM a curated knowledge card (a true fact about
// a concept the query names) — e.g. "sai dirmi quanto fa il PIL diviso 3" deflecting to the real PIL
// DEFINITION. That is the repo's sanctioned "deflect-to-grounded-fact": it relays curated truth, never
// invents a false fact, so it is NOT a fabrication. We load every card reply and detect this exactly,
// so the harness flags only genuine fabrications (wrong/invented facts, leaked templates, false launches).
const cards = [];
for (const f of readdirSync(KDIR)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(KDIR, f), 'utf8').split(/\r?\n/)) { if (!l.trim() || l.startsWith('//')) continue; try { cards.push(JSON.parse(l)); } catch {} } }
const cardReplies = [];
for (const c of cards) for (const lang of ['it', 'en']) { const r = c.reply?.[lang]; if (r && r.length > 12) cardReplies.push(norm(r)); }
const isGroundedRelay = (p) => { if (!(p.tier === 'L1/fact' || p.tier === 'L2/stitch') || !p.reply) return false;
  const n = norm(p.reply); return cardReplies.some(r => n.startsWith(r.slice(0, Math.min(r.length, 40)))); };

// ABSTAIN (the metamorphic-law target) = an honest non-answer OR a grounded verbatim card relay.
const abstained = (p) => isHonestNonAnswer(p) || isGroundedRelay(p);

// ---- semantics-preserving mutation operators ------------------------------------------------------
// WRAPPER tokens (interrogatives, articles, prepositions, scaffolding nouns) carry the question FRAME,
// not the unanswerable PAYLOAD. Typos are confined to these so a perturbation can only DAMAGE recognition
// (abstain harder), never spell the PAYLOAD into a different real entity.
const WRAPPER = new Set(['qual','quale','quali','quanta','quanto','quante','quanti','che','chi','cosa','come',
  'dove','quando','perche','qual è','capitale','citta','nome','numero','colore','anno','giorno','data','ora',
  'what','which','who','whom','whose','when','where','why','how','many','much','is','are','the','of','a','an',
  'in','to','di','del','della','dei','degli','delle','la','il','lo','i','gli','le','un','uno','una','e','è',
  'mi','dimmi','dammi','sai','puoi','tell','do','does','did','can','you','me','please','per','favore','capital']);
const QWERTY = { a:'sq', e:'wr', i:'ou', o:'ip', s:'ad', t:'ry', n:'mb', r:'et', l:'k', c:'xv', u:'yi', d:'sf', p:'ol', m:'n', b:'vn', g:'hf', h:'gj' };
const LEADIN_IT = ['sai dirmi ', 'mi puoi dire ', 'dimmi ', 'vorrei sapere ', 'per favore '];
const LEADIN_EN = ['can you tell me ', 'tell me ', 'do you know ', 'please '];
const FILLER_IT = ['tipo', 'cioe', 'insomma', 'ecco'];           // pure discourse markers (NO log/secondo/coseno homographs)
const FILLER_EN = ['like', 'you know', 'so'];
const SWAP = [[/^qual è la capitale di /i, 'what is the capital of '], [/^what is the capital of /i, 'qual è la capitale di '],
  [/^chi è /i, 'who is '], [/^who is /i, 'chi è '], [/^qual è /i, 'what is '], [/^what is /i, 'qual è '],
  [/^quanti /i, 'how many '], [/^how many /i, 'quanti '], [/^dove è nato /i, 'where was '], [/^where was /i, 'dove è nato ']];

function typoWrapper(q) {                          // 1 QWERTY edit confined to a WRAPPER token
  const w = q.split(' ');
  const cand = [...w.keys()].filter(i => WRAPPER.has(norm(w[i])) && /[a-z]/i.test(w[i]) && w[i].length >= 3);
  if (!cand.length) return null;
  const i = pick(cand); const cs = [...w[i]];
  const al = [...cs.keys()].filter(j => /[a-z]/i.test(cs[j])); if (!al.length) return null;
  const j = pick(al); const ch = cs[j].toLowerCase(); const k = rnd();
  if (k < 0.5 && QWERTY[ch]) cs[j] = pick([...QWERTY[ch]]);              // adjacent-key substitution
  else if (k < 0.75 && cs.length > 3) cs.splice(j, 1);                  // drop a char
  else cs.splice(j, 0, cs[j]);                                          // duplicate a char
  w[i] = cs.join(''); return w.join(' ');
}
const caps     = (q) => rnd() < 0.5 ? q.toUpperCase() : q.replace(/\b\w/g, c => c.toUpperCase());
const accent   = (q) => { const d = q.normalize('NFD').replace(/[̀-ͯ]/g, ''); return d === q ? addAccent(q) : d; };
function addAccent(q) { return q.replace(/\b(citta|universita|perche|cosi|piu|gia|puo|e)\b/gi,
  m => ({ citta:'città', universita:'università', perche:'perché', cosi:'così', piu:'più', gia:'già', puo:'può', e:'è' }[m.toLowerCase()] || m)); }
const punct    = (q) => { let r = q.replace(/ /g, () => rnd() < 0.5 ? '   ' : '  '); if (rnd() < 0.5) r = '  ' + r; return r + pick([' ?', '???', '!!!', '.', '...', '  ']); };
const politeness= (q, lang) => (lang === 'en' ? pick(LEADIN_EN) : pick(LEADIN_IT)) + q;
function filler(q, lang) { const w = q.split(' '); if (w.length < 3) return null; const at = 1 + Math.floor(rnd() * (w.length - 1)); w.splice(at, 0, lang === 'en' ? pick(FILLER_EN) : pick(FILLER_IT)); return w.join(' '); }
function codeswitch(q) { for (const [re, to] of SWAP) if (re.test(q)) return q.replace(re, to); return null; }
function reorder(q, lang) {                        // ONLY commutative: move a trailing courtesy, or swap "X o/or Y"
  const m = q.match(/^(.*?)(,?\s*(grazie|thanks|please|per favore))\s*$/i);
  if (m && m[1].trim().length > 6) return (lang === 'en' ? 'please ' : 'per favore ') + m[1].trim();
  const o = q.match(/^(.*?)\b([\wàèéìòù']+)\s+(o|or)\s+([\wàèéìòù']+)\s*\??$/i);
  if (o) return `${o[1]}${o[4]} ${o[3]} ${o[2]}`;
  return null;
}

const OPS = [
  { name: 'typo',       fn: (q, l) => typoWrapper(q),        survives: true,  guardSlug: true },
  { name: 'accent',     fn: (q, l) => accent(q),             survives: true,  guardSlug: false },
  { name: 'caps',       fn: (q, l) => caps(q),               survives: false, guardSlug: false },
  { name: 'punct',      fn: (q, l) => punct(q),              survives: false, guardSlug: false },
  { name: 'politeness', fn: (q, l) => politeness(q, l),      survives: true,  guardSlug: true },
  { name: 'filler',     fn: (q, l) => filler(q, l),          survives: true,  guardSlug: true },
  { name: 'codeswitch', fn: (q, l) => codeswitch(q),         survives: true,  guardSlug: true },
  { name: 'reorder',    fn: (q, l) => reorder(q, l),         survives: true,  guardSlug: true },
];

// ---- load seeds ----------------------------------------------------------------------------------
const seeds = [];
for (const f of SEED_FILES) {
  const p = join(repo, 'tools', 'anima', f + '.jsonl');
  if (!existsSync(p)) continue;
  for (const l of readFileSync(p, 'utf8').split(/\r?\n/)) {
    if (!l.trim() || l.startsWith('//')) continue;
    try { const o = JSON.parse(l); if (o.q) seeds.push({ q: o.q, lang: o.lang === 'en' ? 'en' : 'it', seedFile: f }); } catch {}
  }
}

// ---- build the mutant set (apply each operator once per seed; drop guard-failing / no-op mutants) --
const mutants = [];
const seen = new Set();
let dropped = 0;
for (const s of seeds) {
  const seedSlug = slugOf(s.q);
  for (const op of OPS) {
    let m; try { m = op.fn(s.q, s.lang); } catch { m = null; }
    if (!m || m === s.q) continue;                                    // operator was a no-op for this seed
    if (op.name === 'punct' && /\{|\}/.test(m)) continue;             // never synthesize a fake {template} leak
    if (op.guardSlug && slugOf(m) !== seedSlug) { dropped++; continue; } // meaning-changing: would shift the referent
    const key = op.name + ' ' + norm(m);
    if (seen.has(key)) continue; seen.add(key);
    mutants.push({ q: m, lang: s.lang, op: op.name, survives: op.survives, seed: s.q });
  }
}

// ---- run every mutant through the REAL offline cascade (one batched anima.exe stream) -------------
function runBatch(items) {
  let lang = 'it'; const lines = [];
  for (const it of items) { lines.push('/reset'); const w = it.lang === 'en' ? 'en' : 'it'; if (w !== lang) { lines.push('/' + w); lang = w; } lines.push(it.q); }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 128 * 1024 * 1024 });
  const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
  return blocks.map((b) => ({
    tier:   (b.match(/tier=(\S+)/) || [])[1] || 'none',
    intent: (b.match(/intent=(\S*)/) || [])[1] || '',
    conf:   Number((b.match(/conf=(\d+)/) || [])[1] || 0),
    reply:  ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
  }));
}

const rows = runBatch(mutants);
const breaks = [];
const byOp = {};
let relays = 0;
for (let i = 0; i < mutants.length; i++) {
  const m = mutants[i], p = rows[i] || { tier: 'none', intent: '', conf: 0, reply: '' };
  byOp[m.op] = byOp[m.op] || { n: 0, fail: 0 };
  byOp[m.op].n++;
  if (!isHonestNonAnswer(p) && isGroundedRelay(p)) relays++;          // deflected to a true curated card — not a fabrication
  if (!abstained(p)) { byOp[m.op].fail++; breaks.push({ ...m, got: `${p.intent || '(none)'}/${p.tier} conf=${p.conf}`, reply: p.reply.slice(0, 64) }); }
}

// ---- M3 detection-invariance (host-real surrogate for the online route gate) ----------------------
// A mutant must NEVER route to a DIFFERENT answerable entity than its seed; it may only lose the wrapper
// (-> null). This proves the network is never aimed at a new target by a perturbation. (guardSlug already
// discards slug-changing mutants at build time, so this asserts the surviving set holds the invariant.)
let m3bad = 0;
for (const m of mutants) { const ms = slugOf(m.q); if (ms !== null && ms !== slugOf(m.seed)) m3bad++; }   // null (wrapper erased) is allowed; only a DIFFERENT non-null slug is a drift

// ---- M4 cross-substrate VERIFIER monotonicity (host-real: "M1 judges M4") -------------------------
// If a WebLLM (M4) browser model FABRICATED an answer to one of these unanswerable questions, the M1
// device verifier (anima.exe --verify) must NEVER return 'confirmed' — at worst 'unknown' (uncheckable
// -> the caller WARNs) or 'contradicted'. We assert this on a sample of seeds AND their mutants, so the
// safety net is proven mutation-invariant. A 'confirmed' on a fabricated false-premise answer = a hole.
const FAKE = 'Bloop42';
function verifyVerdict(query, en) {
  const r = spawnSync(exe, (en ? ['--en'] : []).concat(['--verify', `fact|${query}|${FAKE}`]), { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 });
  const m = /VERDICT=(\w+)/.exec(r.stdout || ''); return m ? m[1] : 'unknown';
}
const vSample = [...seeds.filter((_, i) => i % 11 === 0), ...mutants.filter((_, i) => i % 53 === 0)].slice(0, 70);
let m4bad = 0; const m4fails = [];
for (const s of vSample) { const v = verifyVerdict(s.q, s.lang === 'en'); if (v === 'confirmed') { m4bad++; m4fails.push([s.q, v]); } }

// ---- DETERMINISM: a sample re-run must be byte-identical (anima.exe is a pure function) ------------
const sample = mutants.filter((_, i) => i % 37 === 0).slice(0, 60);
const det1 = runBatch(sample), det2 = runBatch(sample);
let nondet = 0;
for (let i = 0; i < sample.length; i++) { if (JSON.stringify(det1[i]) !== JSON.stringify(det2[i])) nondet++; }

// ---- report --------------------------------------------------------------------------------------
const show = process.argv.includes('--show'), verbose = process.argv.includes('--verbose');
if (show) for (const m of mutants.filter((_, i) => i % 50 === 0)) console.log(`  [${m.op}] "${m.q}"`);
console.log(`\n[metamorph] ${seeds.length} unanswerable seeds × ${OPS.length} safe operators -> ${mutants.length} derived mutants (${dropped} meaning-changing dropped)`);
for (const [op, s] of Object.entries(byOp)) console.log(`  ${op.padEnd(11)} ${s.n - s.fail}/${s.n} abstain${s.fail ? `   (${s.fail} INVARIANCE BREAK)` : ''}`);
console.log(`  grounded deflections (verbatim card relay, NOT a fabrication): ${relays}`);
console.log(`  M3 detection-invariance (entity.mjs): ${mutants.length - m3bad}/${mutants.length} stable slug   ${m3bad ? `(${m3bad} ROUTING DRIFT)` : '✓'}  | ${dropped} slug-changing mutants rejected at generation`);
console.log(`  M4 verifier monotonicity (anima.exe --verify, "M1 judges M4"): ${vSample.length - m4bad}/${vSample.length} never confirm a fabricated answer   ${m4bad ? `(${m4bad} CONFIRMED!)` : '✓'}`);
console.log(`  determinism (re-run sample): ${sample.length - nondet}/${sample.length} byte-stable   ${nondet ? `(${nondet} NON-DETERMINISTIC)` : '✓'}`);
console.log('  DEVICE/BROWSER-DEFERRED (not exercised here): M2 live heap-pressure force-offline · M3 Grok answer + live Wikipedia resolution · M4 on-GPU WebLLM generation');

if (breaks.length) {
  console.log(`\nINVARIANCE BREAKS — a mutation flipped abstain -> fabrication (${breaks.length}):`);
  for (const b of (verbose ? breaks : breaks.slice(0, 40)))
    console.log(`  ✗ [${b.op}] "${b.q}"\n        seed="${b.seed}"  got=${b.got}  reply="${b.reply}"`);
  if (!verbose && breaks.length > 40) console.log(`  … and ${breaks.length - 40} more (use --verbose)`);
}
const fail = breaks.length + m3bad + nondet + m4bad;
if (m4fails.length) { console.log('\nM4 VERIFIER HOLES (confirmed a fabricated answer):'); for (const [q, v] of m4fails) console.log(`  ✗ "${q}" -> ${v}`); }
console.log(fail ? `\n[metamorph] FAIL — ${breaks.length} invariance break(s), ${m3bad} routing drift, ${m4bad} verifier hole(s), ${nondet} nondeterministic`
                 : `\n[metamorph] ✓ ${mutants.length} mutants — abstain invariant under every safe perturbation; detection routing stable; M1-verifier never confirms a fabrication; deterministic.`);
process.exit(fail ? 1 : 0);
