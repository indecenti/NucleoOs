#!/usr/bin/env node
// DEVICE AKB5 PROBE — runs the canonical AKB5-content cases (mirror of akb5-content.mjs) against the
// REAL device /api/anima (offline), to verify the on-device D=192 sharded knowledge matches the host
// D=256 gate. Light by design (~45 sequential offline queries, gently paced) — does NOT overload the
// single-task web server. Usage: node tools/anima-host/device-akb5-probe.mjs [host]   (default .166)
const host = process.argv[2] || '192.168.0.166';

// Mirror of tools/anima-host/akb5-content.mjs (keep in sync). want = keyword a correct answer must contain.
const RECALL = [
  { q: 'qual è la capitale della Norvegia', lang: 'it', want: 'oslo' },
  { q: 'capitale del Giappone', lang: 'it', want: 'tokyo' },
  { q: "qual e la capitale dell'Egitto", lang: 'it', want: 'cairo' },
  { q: 'capitale di Malta', lang: 'it', want: 'valletta' },
  { q: 'what is the capital of Brazil', lang: 'en', want: 'brasilia' },
  { q: 'capital of Kenya', lang: 'en', want: 'nairobi' },
  { q: 'capitale della Francia', lang: 'it', want: 'parigi' },
  { q: 'qual è la capitale del Canada', lang: 'it', want: 'ottawa' },
  { q: 'costante di Planck', lang: 'it', want: '6.626' },
  { q: 'quanto vale la costante di Planck', lang: 'it', want: '6.626' },
  { q: 'costante di Avogadro', lang: 'it', want: '6.022' },
  { q: "carica dell'elettrone", lang: 'it', want: '1.602' },
  { q: 'value of the speed of light', lang: 'en', want: '299792458' },
  { q: "cos'è il DNS", lang: 'it', want: 'dominio' },
  { q: 'what is a transistor', lang: 'en', want: 'transistor' },
  { q: "cos'è la fotosintesi", lang: 'it', want: 'piante' },
  { q: "cos'è un microcontrollore", lang: 'it', want: 'chip' },
  { q: 'what is photosynthesis', lang: 'en', want: 'light' },
];
const ABSTAIN = [
  { q: 'qual è la capitale di Marte', lang: 'it' },
  { q: 'qual è la capitale della Luna', lang: 'it' },
  { q: 'qual è la capitale del Sole', lang: 'it' },
  { q: 'qual è la capitale di Giove', lang: 'it' },
  { q: 'what is the capital of Mars', lang: 'en' },
  { q: 'what is the capital of the Moon', lang: 'en' },
  { q: 'quando ha twittato Cristoforo Colombo', lang: 'it' },
  { q: 'quanti followers ha il Medioevo', lang: 'it' },
  { q: "qual è l'indirizzo email di Napoleone", lang: 'it' },
  { q: 'qual è la costante di Planck di Marte', lang: 'it' },
  { q: 'qual è il pianeta più grande oltre Giove', lang: 'it' },
  { q: 'what is the biggest planet other than Jupiter', lang: 'en' },
  { q: 'che cosa è il protocollo Flixxon-9', lang: 'it' },
  { q: 'what is the half-life of isotope Carbon-Floonk-14', lang: 'en' },
  { q: 'qual è la radice cubica del cristallo Zorblax-27', lang: 'it' },
  { q: 'what is 50 percent of Floonkonium-200', lang: 'en' },
  { q: 'come si configura il modulo Frobnicator-X12', lang: 'it' },
  { q: 'qual è il server Discord di Napoleone', lang: 'it' },
  { q: 'what is the Discord username of Cleopatra', lang: 'en' },
  { q: 'qual è la valuta del Giappone', lang: 'it' },
  { q: 'qual è il lago più grande del deserto del Sahara', lang: 'it' },
];
// Cases observed live in the chat (diagnostic, non-gating) — phrasing variants + the suspicious abstentions.
const DIAG = [
  { q: 'chi è nixon', lang: 'it', note: 'real figure — abstained live' },
  { q: 'cosa sai della fotosintesi', lang: 'it', note: 'phrasing variant of cos\'è la fotosintesi (recall set)' },
  { q: 'cosa sai di napoleone', lang: 'it', note: 'worked live (knowledge 93%)' },
  { q: 'chi è george bush', lang: 'it', note: 'answered, garbled W/H.W.' },
  { q: 'chi è iron man', lang: 'it', note: 'fictional — should abstain' },
];

// PHRASING regression: "cosa sai DELLA/DEL X" on shard topics that ARE in the corpus. Before the
// a_topic_strip article-contraction fix these abstained (the lead-in leaked into the embedding); after,
// they must answer like "cos'è X". Topics drawn from the RECALL set (known present).
const PHRASING = [
  { q: 'cosa sai della fotosintesi', lang: 'it', want: 'piante' },
  { q: 'cosa sai del DNS', lang: 'it', want: 'dominio' },
  { q: 'cosa sai del transistor', lang: 'it', want: 'transistor' },
  { q: 'cosa sai della costante di Planck', lang: 'it', want: '6.626' },
  { q: 'cosa sai del microcontrollore', lang: 'it', want: 'chip' },
];

const norm = s => s.normalize('NFD').replace(/\p{Diacritic}/gu, '').toLowerCase().replace(/\s+/g, ' ').trim();
const isRefusal = s => /non lo so|non ho (informazioni|dettagli|altri)|posso aprire app|not a place|not a person|non (è|e) (un|una) (luogo|persona|posto)|did you mean|intendi\b|i (don'?t|do not) (have|know)/i.test(s);
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function ask(q, lang) {
  const u = `http://${host}/api/anima?q=${encodeURIComponent(q)}&lang=${lang}&mode=off`;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const r = await fetch(u, { signal: AbortSignal.timeout(20000) });
      const j = await r.json();
      if (j.busy) { await sleep(400); continue; }   // spine busy -> retry
      return { tier: j.tier || 'none', reply: (j.reply || '').replace(/\s+/g, ' ').trim() };
    } catch (e) { await sleep(400); }
  }
  return { tier: 'none', reply: '(no response)' };
}

console.log(`[device ${host}] AKB5 content probe — ${RECALL.length} recall + ${ABSTAIN.length} false-premise (offline)\n`);
let recallOk = 0, recallMiss = 0, halluc = 0; const fails = [];
for (const it of RECALL) {
  const { tier, reply } = await ask(it.q, it.lang); await sleep(120);
  const answered = reply !== '' && !isRefusal(reply);   // device answers some correct cases at tier=none (compose/MOSAICO) — judge by CONTENT, not the tier label
  if (answered && norm(reply).includes(norm(it.want))) recallOk++;
  else { recallMiss++; fails.push({ k: answered ? 'WRONG' : 'MISS', q: it.q, want: it.want, tier, reply: reply.slice(0, 70) }); }
}
for (const it of ABSTAIN) {
  const { tier, reply } = await ask(it.q, it.lang); await sleep(120);
  const answered = reply !== '' && !isRefusal(reply);   // device answers some correct cases at tier=none (compose/MOSAICO) — judge by CONTENT, not the tier label
  if (answered) { halluc++; fails.push({ k: 'HALLUC', q: it.q, tier, reply: reply.slice(0, 70) }); }
}
const floor = Math.ceil(RECALL.length * 0.8);
console.log(`RESULT  recall ${recallOk}/${RECALL.length} (host-gate floor ${floor})  |  false-premise abstained ${ABSTAIN.length - halluc}/${ABSTAIN.length}  |  HALLUCINATIONS ${halluc}`);
// Phrasing-fix regression (cosa sai della/del X)
let phrOk = 0; const phrFails = [];
for (const it of PHRASING) {
  const { tier, reply } = await ask(it.q, it.lang); await sleep(120);
  const answered = reply !== '' && !isRefusal(reply);
  if (answered && norm(reply).includes(norm(it.want))) phrOk++;
  else phrFails.push({ q: it.q, want: it.want, tier, reply: reply.slice(0, 70) });
}
console.log(`\nPHRASING (cosa sai della/del X)  ${phrOk}/${PHRASING.length} answered correctly`);
for (const f of phrFails) console.log('  MISS ' + JSON.stringify(f));

console.log('\n-- user-observed diagnostics (non-gating) --');
for (const it of DIAG) { const { tier, reply } = await ask(it.q, it.lang); await sleep(120); console.log(`  "${it.q}" -> tier=${tier} | ${reply.slice(0, 90)}   [${it.note}]`); }
if (fails.length) { console.log('\n-- misses / hallucinations --'); for (const f of fails) console.log('  ' + JSON.stringify(f)); }
console.log(`\n${halluc === 0 && recallOk >= floor ? '✓ device matches host gate (0 halluc, recall above floor)' : '✗ device DIVERGES from host gate — see details above'}`);
