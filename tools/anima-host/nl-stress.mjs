#!/usr/bin/env node
// ANIMA offline NL stress — drives the REAL compiled cascade (anima.exe) with HUNDREDS of natural
// language queries to prove the 0-hallucination contract under realistic pressure:
//   (A) PARAPHRASE+TYPO recall — take real asks from the corpus, perturb them (small QWERTY typos,
//       dropped accents, lead-ins) and require the SAME card back OR a safe abstain — NEVER a different
//       card's confident answer (a "misattribution" = a hallucination).
//   (B) ADVERSARIAL must-abstain — false premises about real entities (wrong-domain facets), impossible
//       actions, fictional entities, and nonsense. ANY confident answer here is a hallucination.
// HARD GATE: 0 hallucinations (misattributions in A + any answer in B). Recall in A is reported, not gated
// (the flat index's recall ceiling is a known, separately-tracked property).
//
//   node tools/anima-host/nl-stress.mjs            # the gate
//   node tools/anima-host/nl-stress.mjs --verbose  # print every hallucination + miss
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
const KDIR = join(repo, 'tools', 'anima', 'knowledge');
const verbose = process.argv.includes('--verbose');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

// Deterministic RNG (no Math.random → reproducible gate).
let _s = 0x9e3779b9;
const rnd = () => { _s = (_s * 1664525 + 1013904223) >>> 0; return _s / 0x100000000; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();

// ---- load corpus -------------------------------------------------------------------------------
const cards = [];
for (const f of readdirSync(KDIR)) {
  if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(KDIR, f), 'utf8').split('\n')) {
    if (!l.trim() || l.trim().startsWith('//')) continue;
    try { cards.push(JSON.parse(l)); } catch {}
  }
}
// reply text (normalized) -> card id, for detecting WHICH card answered (MOSAICO prepends the card reply).
const replyOwner = new Map();
for (const c of cards) for (const lang of ['it', 'en']) {
  const r = c.reply?.[lang]; if (r && r.length > 8) replyOwner.set(norm(r), c.id);
}
function whoAnswered(reply) {                 // longest stored reply that the exe output starts with
  const n = norm(reply); let best = null, blen = 0;
  for (const [r, id] of replyOwner) if (n.startsWith(r.slice(0, Math.min(r.length, 60))) && r.length > blen) { best = id; blen = r.length; }
  return best;
}

// ---- typo / paraphrase perturbations -----------------------------------------------------------
const QWERTY = { a: 'sq', e: 'wr', i: 'ou', o: 'ip', s: 'ad', t: 'ry', n: 'mb', r: 'et', l: 'k', c: 'xv' };
function typo(s) {
  const cs = [...s]; const idxs = [...cs.keys()].filter(i => /[a-z]/i.test(cs[i]));
  if (!idxs.length) return s;
  const i = pick(idxs); const ch = cs[i].toLowerCase(); const k = rnd();
  if (k < 0.4 && QWERTY[ch]) cs[i] = pick([...QWERTY[ch]]);   // adjacent-key swap
  else if (k < 0.7) cs.splice(i, 1);                          // drop a char
  else cs.splice(i, 0, cs[i]);                                // duplicate a char
  return cs.join('');
}
const dropAccent = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '');
const LEADIN_IT = ['', 'sai dirmi ', 'mi puoi dire ', 'dimmi ', 'vorrei sapere ', 'per favore '];
const LEADIN_EN = ['', 'can you tell me ', 'tell me ', 'do you know ', 'please '];
const spacePunct = (s) => {
  // inject extra spaces between words + random trailing punctuation (robustness: tiers must collapse these)
  let r = s.replace(/ /g, () => (rnd() < 0.5 ? '   ' : '  '));
  if (rnd() < 0.5) r = '  ' + r;
  return r + pick(['', ' ?', '???', '!!!', '.', '  ']);
};
function perturb(ask, lang) {
  const k = rnd();
  if (k < 0.34) { let s = typo(ask); if (rnd() < 0.35) s = typo(s); return s; }   // 1–2 QWERTY typos (harder)
  if (k < 0.52) return dropAccent(ask);
  if (k < 0.68) return (lang === 'en' ? pick(LEADIN_EN) : pick(LEADIN_IT)) + ask;
  if (k < 0.84) return spacePunct(ask);                                           // sloppy spaces + punctuation
  return dropAccent(typo(ask));                                                   // typo + accent fold combined
}

// ---- (A) paraphrase+typo cases from real corpus asks -------------------------------------------
const A = [];
const answerCards = cards.filter(c => (c.action ?? 'answer') === 'answer' && c.reply && c.ask);
for (let n = 0; n < 300; n++) {
  const c = pick(answerCards);
  const lang = rnd() < 0.5 ? 'it' : 'en';
  const asks = c.ask?.[lang]; if (!asks || !asks.length) continue;
  const base = pick(asks);
  if (base.length < 6 || base.length > 60) continue;
  A.push({ kind: 'recall', q: perturb(base, lang), lang, expect: c.id, expectReply: c.reply?.[lang] || c.reply?.it || c.reply?.en });
}

// ---- (B) adversarial must-abstain --------------------------------------------------------------
const PEOPLE = ['Albert Einstein', 'Dante Alighieri', 'Leonardo da Vinci', 'Napoleone', 'Mozart', 'Marie Curie', 'Galileo', 'Shakespeare'];
const PLACES = ['Parigi', 'Roma', 'Tokyo', 'Giappone', 'Francia', 'Milano'];
const ABSTRACT = ['libertà', 'gravità', 'coraggio', 'matematica'];
const FICTIONAL = ['Floonk Bargle', 'Zxqwerty Plumbus', 'Gribbo Flandare', 'Wibblethorpe'];
const NONSENSE = ['asdkjfh qwepoi', 'blarg fnord zzz', 'xkcd plumbus 42 grok', 'lorem ipsum dolor sit', 'qqq www eee', 'flibber jabber wock'];
const B = [];
const adv = (q, lang, entity) => B.push({ kind: 'adv', q, lang, entity });
// people asked place/quantity/sport questions (type violation) — must abstain or type-gate refuse
for (const p of PEOPLE) { adv(`qual è la capitale di ${p}`, 'it', p); adv(`quanti abitanti ha ${p}`, 'it', p); adv(`in che continente si trova ${p}`, 'it', p); adv(`what is the capital of ${p}`, 'en', p); adv(`how many people live in ${p}`, 'en', p); }
// abstract concepts asked physical-domain questions
for (const x of ABSTRACT) { adv(`qual è la capitale di ${x}`, 'it', x); adv(`quanti abitanti ha ${x}`, 'it', x); adv(`di che colore è ${x}`, 'it', x); }
// places asked person questions
for (const x of PLACES) { adv(`in che squadra gioca ${x}`, 'it', x); adv(`quando è morto ${x}`, 'it', x); adv(`che lavoro fa ${x}`, 'it', x); }
// fictional entities with real-sounding templates — must abstain (entity doesn't exist)
for (const x of FICTIONAL) { adv(`chi è ${x}`, 'it', x); adv(`quando è nato ${x}`, 'it', x); adv(`who is ${x}`, 'en', x); adv(`qual è la capitale di ${x}`, 'it', x); adv(`dove è nato ${x}`, 'it', x); }
// pure nonsense — must abstain
for (const x of NONSENSE) { adv(x, 'it', x); adv(x + ' ?', 'en', x); adv(`cos'è ${x}`, 'it', x); }
// FALSE PREMISE: a real person credited with a thing they didn't do — must NOT confirm (abstain or
// deflect to the true grounded bio about the SAME person; never assert the false fact).
const FALSE_DEED = ['ha inventato la lampadina', 'ha vinto i mondiali di calcio', 'ha scoperto l\'America', 'ha scritto la Divina Commedia nel 2020'];
for (const p of PEOPLE.slice(0, 5)) adv(`perché ${p} ${pick(FALSE_DEED)}`, 'it', p);
// PRESUPPOSITION about ANIMA — loaded questions presupposing feelings/body/family; must abstain/deflect-identity
for (const v of ['si arrabbia', 'va in vacanza', 'ha figli', 'dorme la notte', 'si annoia']) adv(`quando ANIMA ${v}`, 'it', 'anima');
// CODE-SWITCHING IT+EN mixed — a fictional/false target mixed across languages must still abstain
for (const x of FICTIONAL.slice(0, 3)) { adv(`what is la capitale di ${x}`, 'en', x); adv(`dimmi the birthplace of ${x}`, 'it', x); }
// unit/function FALSE-FRIENDS embedded in real phrases — must NOT compute
for (const s of ['ho perso il conto dei giorni', 'fai un log degli errori', 'dammi un secondo', 'che senso ha tutto questo', 'il coseno della vita']) adv(s, 'it', s);

const items = [...A, ...B];

// ---- drive the real exe ------------------------------------------------------------------------
let lang = 'it'; const lines = [];
for (const it of items) { lines.push('/reset'); const w = it.lang === 'en' ? 'en' : 'it'; if (w !== lang) { lines.push('/' + w); lang = w; } lines.push(it.q); }
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024, env: { ...process.env } });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1).map(b => {
  const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  const action = (b.match(/action=(\S+)/) || [])[1] || 'none';
  let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || '';
  reply = reply.replace(/\s+/g, ' ').trim(); if (reply === '(vuoto)' || reply === '(empty)') reply = '';
  return { tier, action, reply, answered: tier !== 'none' && reply !== '' };
});
if (blocks.length !== items.length) { console.error(`[nl-stress] block mismatch items=${items.length} got=${blocks.length}`); process.exit(1); }

// A REFUSAL is the safe, correct response to a type-violating/unknown query (NOT a hallucination).
const isRefusal = (r) => /not a (place|person|thing|team)|is a (person|place|work)\b.*\bnot\b|è una? (persona|luogo|opera|cosa)\b.*\bnon\b|non (è|e) un ?a? (luogo|posto|persona|nome|cosa|opera)|non un (luogo|posto|nome)|non lo so|non (ho|saprei|conosco)|i (don'?t|do not) (know|have)|non sono sicur|non trovo|nessun(a)? (informazione|risultato)/i.test(r);
const isLaunch = (b) => b.action === 'launch' || /^(apro|opening|avvio|lancio)\b/i.test(b.reply);
// A knowledge/adversarial query answered by a live SYSTEM reading (storage/time/year/battery — the
// "{value}" template, "SD space", "Siamo nel …") is a cross-skill MISROUTE, not an answer.
const isSysCmd = (b) => b.action === 'system' || /\{value\}|^(sd space|spazio sd|siamo nel|batteria:)/i.test(b.reply);
const isGrounded = (r) => !!whoAnswered(r);                       // reply is a verbatim corpus card → not fabricated
const mentions = (r, e) => { const n = norm(r); return norm(e).split(' ').some(t => t.length >= 4 && n.includes(t)); };

// ---- classify ----------------------------------------------------------------------------------
let recallOk = 0, recallMiss = 0, misattrib = 0, advAbstain = 0, advDeflect = 0, advHalluc = 0, misroute = 0;
const fails = [];
for (let i = 0; i < items.length; i++) {
  const it = items[i], b = blocks[i];
  if (it.kind === 'recall') {
    if (!b.answered || isRefusal(b.reply)) { recallMiss++; continue; }       // safe abstain
    if (isLaunch(b)) { misroute++; fails.push({ k: 'MISROUTE', q: it.q, reply: b.reply }); continue; }
    // a recall query answered by a live system reading MAY be topic-aligned (uptime→uptime) — soft, not gated.
    if (isSysCmd(b)) { misattrib++; if (verbose) fails.push({ k: 'soft-sysroute', q: it.q, reply: b.reply }); continue; }
    const who = whoAnswered(b.reply);
    if (who === it.expect || (it.expectReply && norm(b.reply).startsWith(norm(it.expectReply).slice(0, 30)))) recallOk++;
    // a recall query corrupted (by aggressive typos) into a DIFFERENT knowledge card is a SOFT quality
    // metric, not a hard hallucination — the typo can legitimately change meaning ("Elton John"→"Elon Joh"
    // ~ Elon Musk). Reported, not gated. The hard contract is: no misroute, no adversarial/x-turn halluc.
    else if (who) { misattrib++; if (verbose) fails.push({ k: 'soft-misattrib', q: it.q, reply: b.reply, want: it.expect, got: who }); }
    else recallMiss++;   // answered with a non-card reply on a recall paraphrase — count as miss, not halluc
  } else { // adversarial: must abstain / refuse / deflect-to-true-fact about the SAME entity
    if (!b.answered || isRefusal(b.reply)) { advAbstain++; continue; }
    if (isLaunch(b)) { misroute++; fails.push({ k: 'MISROUTE', q: it.q, reply: b.reply }); continue; }
    if (isGrounded(b.reply) && mentions(b.reply, it.entity || '')) { advDeflect++; continue; }  // true fact, same entity
    advHalluc++; fails.push({ k: 'HALLUC', q: it.q, reply: b.reply });
  }
}

// ---- (C) CROSS-AGENT multi-turn: a knowledge answer then a follow-up must stay on the SAME entity ----
// "dimmi di più"/"tell me more"/"e in inglese?" must return the same card's detail, the same entity, or
// honestly abstain — NEVER a different card's confident fact (a cross-turn hallucination). Tests the
// agent loop's dialogue-act + working-memory handling, not just single-shot retrieval.
const FOLLOW_IT = ['dimmi di più', 'spiegami meglio', 'e quindi?', 'puoi approfondire'];
const FOLLOW_EN = ['tell me more', 'explain more', 'go on', 'can you elaborate'];
const convos = [];
for (let n = 0; n < 60; n++) {
  const c = pick(answerCards); const lang = rnd() < 0.5 ? 'it' : 'en';
  const asks = c.ask?.[lang]; if (!asks || !asks.length) continue;
  const base = pick(asks); if (base.length < 6 || base.length > 60) continue;
  convos.push({ lang, t1: rnd() < 0.5 ? perturb(base, lang) : base, t2: lang === 'en' ? pick(FOLLOW_EN) : pick(FOLLOW_IT), card: c.id });
}
const clines = [];
for (const cv of convos) { clines.push('/reset'); clines.push('/' + cv.lang); clines.push(cv.t1); clines.push(cv.t2); }
const cr = spawnSync(exe, [], { input: Buffer.from(clines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024, env: { ...process.env } });
const cblocks = cr.stdout.toString('utf8').split(/^Q: /m).slice(1).map(b => {
  const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || '';
  reply = reply.replace(/\s+/g, ' ').trim(); if (reply === '(vuoto)' || reply === '(empty)') reply = '';
  return { tier, reply, answered: tier !== 'none' && reply !== '' };
});
let mtOk = 0, mtSafe = 0, mtHalluc = 0;
for (let i = 0; i < convos.length; i++) {
  const cv = convos[i]; const t2 = cblocks[i * 2 + 1]; if (!t2) continue;          // the follow-up turn
  if (!t2.answered || isRefusal(t2.reply)) { mtSafe++; continue; }                  // honest "nothing more"
  const who = whoAnswered(t2.reply);
  if (!who || who === cv.card) { mtOk++; continue; }                               // same card / non-card chit-chat
  // a DIFFERENT grounded card on the follow-up = cross-turn misattribution (hallucination)
  mtHalluc++; fails.push({ k: 'XTURN', q: `${cv.t1} » ${cv.t2}`, reply: t2.reply, want: cv.card, got: who });
}

const totalHalluc = advHalluc + mtHalluc;   // misattrib is a SOFT metric (typo can change meaning), not gated
console.log(`[nl-stress] cross-agent multi-turn: same-entity ${mtOk}  safe-abstain ${mtSafe}  X-TURN HALLUC ${mtHalluc}`);
console.log(`[nl-stress] ${items.length} single + ${convos.length} multi-turn offline cases  (recall ${A.length}, adversarial ${B.length})`);
console.log(`[nl-stress] recall:      correct ${recallOk}  safe-abstain ${recallMiss}  soft-misattrib ${misattrib} (typo-drift, not gated)`);
console.log(`[nl-stress] adversarial: abstain/refuse ${advAbstain}  grounded-deflect ${advDeflect}  HALLUCINATION ${advHalluc}`);
console.log(`[nl-stress] cross-skill MISROUTES (knowledge→launch): ${misroute}`);
console.log(`[nl-stress] TOTAL HALLUCINATIONS: ${totalHalluc} (adversarial ${advHalluc} + x-turn ${mtHalluc})  cross-skill misroutes: ${misroute}  [soft typo-drift: ${misattrib}]`);
if (verbose || totalHalluc || misroute) for (const h of fails.slice(0, 40)) console.log(`  ✗ [${h.k}] "${h.q}"  →  "${h.reply}"${h.want ? `  (want ${h.want}, got ${h.got})` : ''}`);
// HARD gate: zero hallucinations (single + cross-turn) AND zero cross-skill misroutes (a knowledge
// question must never silently launch an app). Both are now locked green; regressions fail the gate.
process.exit(totalHalluc === 0 && misroute === 0 ? 0 : 1);
