#!/usr/bin/env node
// LIVE test against the device (default http://192.168.0.166): ask about 60 lesser-known figures —
// obscure Roman emperors + old/niche Italian athletes — in VARIED IT/EN natural language through the
// HYBRID online tier. For each we check THREE things, not one:
//   reached   = the online tier answered (not an abstention / "I need internet")
//   correct   = the answer is about the RIGHT person (a distinctive token of the name appears)
//   mismatch  = reached but WRONG person (a substantive answer about someone else) — the dangerous case
// Then it RE-QUERIES OFFLINE to verify ANIMA persisted (learned) what it fetched. Retries transient
// heap-fallbacks (our guard degrades, never crashes), samples /api/heap, checks the device stays alive.
//   node tools/anima-host/live-figures-test.mjs
const HOST = process.env.ANIMA_HOST || 'http://192.168.0.166';

const FIGURES = [
  {it:'Didio Giuliano', en:'Didius Julianus', kind:'emperor'},
  {it:'Macrino', en:'Macrinus', kind:'emperor'},
  {it:'Diadumeniano', en:'Diadumenianus', kind:'emperor'},
  {it:'Pupieno', en:'Pupienus', kind:'emperor'},
  {it:'Balbino', en:'Balbinus', kind:'emperor'},
  {it:'Gordiano I', en:'Gordian I', kind:'emperor'},
  {it:'Gordiano II', en:'Gordian II', kind:'emperor'},
  {it:'Treboniano Gallo', en:'Trebonianus Gallus', kind:'emperor'},
  {it:'Emiliano imperatore', en:'Aemilianus', kind:'emperor'},
  {it:'Volusiano', en:'Volusianus', kind:'emperor'},
  {it:'Ostiliano', en:'Hostilian', kind:'emperor'},
  {it:'Floriano imperatore', en:'Florianus', kind:'emperor'},
  {it:'Marco Claudio Tacito', en:'Marcus Claudius Tacitus', kind:'emperor'},
  {it:'Caro imperatore', en:'Carus', kind:'emperor'},
  {it:'Numeriano', en:'Numerian', kind:'emperor'},
  {it:'Carino imperatore', en:'Carinus', kind:'emperor'},
  {it:'Quintillo', en:'Quintillus', kind:'emperor'},
  {it:'Glicerio', en:'Glycerius', kind:'emperor'},
  {it:'Maggioriano', en:'Majorian', kind:'emperor'},
  {it:'Avito imperatore', en:'Avitus', kind:'emperor'},
  {it:'Libio Severo', en:'Libius Severus', kind:'emperor'},
  {it:'Olibrio', en:'Olybrius', kind:'emperor'},
  {it:'Petronio Massimo', en:'Petronius Maximus', kind:'emperor'},
  {it:'Antemio', en:'Anthemius', kind:'emperor'},
  {it:'Gallieno', en:'Gallienus', kind:'emperor'},
  {it:'Claudio il Gotico', en:'Claudius Gothicus', kind:'emperor'},
  {it:'Probo imperatore', en:'Probus emperor', kind:'emperor'},
  {it:'Giulio Nepote', en:'Julius Nepos', kind:'emperor'},
  {it:'Massimino il Trace', en:'Maximinus Thrax', kind:'emperor'},
  {it:'Decio imperatore', en:'Decius', kind:'emperor'},
  {it:'Ottavio Bottecchia', en:'Ottavio Bottecchia', kind:'sport'},
  {it:'Learco Guerra', en:'Learco Guerra', kind:'sport'},
  {it:'Costante Girardengo', en:'Costante Girardengo', kind:'sport'},
  {it:'Fiorenzo Magni', en:'Fiorenzo Magni', kind:'sport'},
  {it:'Gastone Nencini', en:'Gastone Nencini', kind:'sport'},
  {it:'Luigi Ganna', en:'Luigi Ganna', kind:'sport'},
  {it:'Giovanni Brunero', en:'Giovanni Brunero', kind:'sport'},
  {it:'Carlo Galetti', en:'Carlo Galetti', kind:'sport'},
  {it:'Duilio Loi', en:'Duilio Loi', kind:'sport'},
  {it:'Silvio Piola', en:'Silvio Piola', kind:'sport'},
  {it:'Adolfo Baloncieri', en:'Adolfo Baloncieri', kind:'sport'},
  {it:'Virginio Rosetta', en:'Virginio Rosetta', kind:'sport'},
  {it:'Renato Cesarini', en:'Renato Cesarini', kind:'sport'},
  {it:'Nedo Nadi', en:'Nedo Nadi', kind:'sport'},
  {it:'Aldo Nadi', en:'Aldo Nadi', kind:'sport'},
  {it:'Giulio Gaudini', en:'Giulio Gaudini', kind:'sport'},
  {it:'Luigi Beccali', en:'Luigi Beccali', kind:'sport'},
  {it:'Ondina Valla', en:'Ondina Valla', kind:'sport'},
  {it:'Adolfo Consolini', en:'Adolfo Consolini', kind:'sport'},
  {it:'Achille Varzi', en:'Achille Varzi', kind:'sport'},
  {it:'Giuseppe Campari', en:'Giuseppe Campari', kind:'sport'},
  {it:'Emilio Materassi', en:'Emilio Materassi', kind:'sport'},
  {it:'Alfredo Binda', en:'Alfredo Binda', kind:'sport'},
  {it:'Gaetano Belloni', en:'Gaetano Belloni', kind:'sport'},
  {it:'Domenico Piemontesi', en:'Domenico Piemontesi', kind:'sport'},
  {it:'Annibale Frossi', en:'Annibale Frossi', kind:'sport'},
  {it:'Eraldo Monzeglio', en:'Eraldo Monzeglio', kind:'sport'},
  {it:'Piero Taruffi', en:'Piero Taruffi', kind:'sport'},
  {it:'Guido Masetti', en:'Guido Masetti', kind:'sport'},
  {it:'Raimondo D\'Inzeo', en:'Raimondo D\'Inzeo', kind:'sport'},
];

const TPL_IT = ['chi è %s', 'chi era %s', 'parlami di %s', 'cosa sai di %s', 'dimmi chi era %s', '%s chi era?'];
const TPL_EN = ['who is %s', 'who was %s', 'tell me about %s', 'what do you know about %s', 'give me info on %s'];

const ABST = [/non lo so/i, /mi serve internet/i, /non ho (informazioni|notizie|dati)/i, /i need internet/i,
  /i (don'?t|do not) know/i, /posso aprire app/i, /i can open apps/i, /nessun[ao]? (risultat|informazion)/i,
  /non sono sicur/i, /^non saprei/i, /no internet/i, /low memory/i, /non ho trovato/i];
const STOP = new Set(['imperatore','emperor','romano','roman','the','il','lo',"l'",'di','de','von','van']);

const sleep = ms => new Promise(r => setTimeout(r, ms));
const fmt = (tpl, name) => tpl.replace('%s', name);
const fold = s => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();

function isAbstention(reply, tier) {
  if (!reply || reply.trim().length < 35) return true;
  if (!tier || tier === 'none') return true;
  return ABST.some(r => r.test(reply));
}
// Distinctive name tokens (>=4 chars, not generic). A correct bio repeats at least one.
function nameTokens(f) {
  const out = new Set();
  for (const form of [f.it, f.en]) for (const w0 of fold(form).split(/[^a-z0-9]+/)) {
    const w = w0.trim();
    if (w.length >= 4 && !STOP.has(w)) out.add(w);
  }
  return [...out];
}
function nameMatch(f, reply) {
  const r = fold(reply);
  return nameTokens(f).some(t => r.includes(t.slice(0, Math.max(4, t.length - 1)))); // tolerate it/en suffix drift
}

async function ask(q, mode, lang, timeoutMs = 45000) {
  try {
    const u = new URL(HOST + '/api/anima');
    u.searchParams.set('q', q);
    if (mode) u.searchParams.set('mode', mode);
    if (lang) u.searchParams.set('lang', lang);
    const r = await fetch(u, { signal: AbortSignal.timeout(timeoutMs) });
    return await r.json();
  } catch (e) { return { error: String(e), tier: 'none', reply: '' }; }
}
async function heap() { try { const j = await (await fetch(HOST + '/api/heap', { signal: AbortSignal.timeout(6000) })).json(); return j.internal; } catch { return null; } }
async function alive() { try { return (await fetch(HOST + '/api/status', { signal: AbortSignal.timeout(6000) })).ok; } catch { return false; } }

async function main() {
  console.log(`[live] ${HOST} — ${FIGURES.length} figures, hybrid online, varied IT/EN\n`);
  const h0 = await heap();
  if (!h0) { console.error('[live] device not reachable — aborting'); process.exit(2); }
  console.log(`[live] start heap: largest=${h0.largest_free_block} free=${h0.free_bytes} frag=${h0.frag_pct}%\n`);

  const results = []; let crashes = 0, heapMin = h0.largest_free_block;

  for (let i = 0; i < FIGURES.length; i++) {
    const f = FIGURES[i];
    const lang = (i % 2 === 0) ? 'it' : 'en';
    const tpls = lang === 'it' ? TPL_IT : TPL_EN;
    const q = fmt(tpls[i % tpls.length], f[lang]);

    let res = null, attempts = 0, reached = false, correct = false;
    for (attempts = 1; attempts <= 3; attempts++) {
      res = await ask(q, 'on', lang);
      reached = !res.error && !isAbstention(res.reply, res.tier);
      correct = reached && nameMatch(f, res.reply);
      if (correct) break;                       // got the right person -> done
      if (reached && !correct) break;           // wrong person -> a real mismatch, don't retry (deterministic)
      if (!(await alive())) { crashes++; console.log(`  !! unreachable after "${q}" — waiting`); await sleep(12000); }
      else await sleep(2500);                   // transient heap/net -> retry
    }
    const tier = res.tier || 'none';
    const verdict = correct ? 'OK ' : reached ? 'WRONG' : 'MISS';
    results.push({ i, name: f.it, q, lang, kind: f.kind, reached, correct, tier, attempts, reply: (res.reply || res.error || '').slice(0, 130) });
    console.log(`  ${String(i + 1).padStart(2)} [${lang}] ${verdict} (${tier},a${attempts}) "${q}" -> ${(res.reply || res.error || '').slice(0, 80)}`);
    if (i % 12 === 11) { const h = await heap(); if (h) { heapMin = Math.min(heapMin, h.largest_free_block); console.log(`     ~heap largest=${h.largest_free_block} free=${h.free_bytes}`); } }
    await sleep(1300);
  }

  const correctList = results.filter(r => r.correct);
  const mismatches  = results.filter(r => r.reached && !r.correct);
  const misses      = results.filter(r => !r.reached);
  const remoteCorrect = correctList.filter(r => r.tier === 'remote');

  // PASS 2: offline re-query of correctly-found figures -> did ANIMA persist what it learned?
  console.log(`\n[live] PASS 2 (offline re-query of ${remoteCorrect.length} online-correct — self-learning):`);
  let learned = 0; const notLearned = [];
  for (const r of remoteCorrect) {
    const f = FIGURES[r.i];
    const res = await ask(fmt('chi è %s', f.it), 'off', 'it', 12000);
    const ok = !res.error && !isAbstention(res.reply, res.tier) && nameMatch(f, res.reply);
    if (ok) learned++; else notLearned.push(f.it);
    await sleep(700);
  }

  const hN = await heap();
  console.log(`\n==================== SUMMARY ====================`);
  console.log(`  CORRECT (right person, online): ${correctList.length}/${FIGURES.length}`);
  console.log(`    IT: ${correctList.filter(r=>r.lang==='it').length}/${results.filter(r=>r.lang==='it').length}   EN: ${correctList.filter(r=>r.lang==='en').length}/${results.filter(r=>r.lang==='en').length}`);
  console.log(`  WRONG person (mismatch): ${mismatches.length}   MISS (abstain/err): ${misses.length}`);
  console.log(`  knowledge persisted OFFLINE on re-query: ${learned}/${remoteCorrect.length}`);
  console.log(`  device crashes/unreachable: ${crashes}   heap: start=${h0.largest_free_block} min=${heapMin} end=${hN?hN.largest_free_block:'?'}`);
  if (mismatches.length) { console.log(`  -- WRONG (${mismatches.length}):`); for (const m of mismatches) console.log(`     [${m.lang}] "${m.q}" (${m.tier}) -> ${m.reply}`); }
  if (misses.length) { console.log(`  -- MISS (${misses.length}):`); for (const m of misses) console.log(`     [${m.lang}] "${m.q}" (${m.tier}) -> ${m.reply}`); }
  console.log(`================================================`);
  const fs = await import('node:fs');
  fs.writeFileSync(new URL('./live-figures-results.json', import.meta.url),
    JSON.stringify({ host: HOST, total: FIGURES.length, correct: correctList.length, mismatch: mismatches.length, miss: misses.length, learned, remoteCorrect: remoteCorrect.length, crashes, heapMin, results }, null, 2));
  process.exit((mismatches.length === 0 && misses.length === 0 && crashes === 0) ? 0 : 1);
}
main();
