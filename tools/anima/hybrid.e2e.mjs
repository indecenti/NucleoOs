// ANIMA HYBRID (mode=on) broad answer test — hits a running device/sim and checks that ANIMA
// actually ANSWERS across many domains. "Non lo so" is only acceptable as a LAST resort: this test
// flags every dontknow so we can confirm it only appears after the online tiers were genuinely tried.
//
// Usage:  HOST=http://192.168.0.166 node tools/anima/hybrid.e2e.mjs     (real device, has Grok key)
//         node tools/anima/hybrid.e2e.mjs                                (defaults to the sim :5599)
const HOST = process.env.HOST || 'http://localhost:5599';
const TIMEOUT = parseInt(process.env.TIMEOUT || '45000');

// The web app's "don't know" fallbacks (IT/EN) — a reply equal to these means a real miss.
const DONTKNOW = [
  'non lo so ancora', "i don't know yet", 'non lo so', "i don't know",
];
const isDontknow = (s) => { const t = (s || '').toLowerCase(); return !s || DONTKNOW.some(d => t.includes(d)); };

async function ask(q, lang = 'it', mode = 'on') {
  const u = `${HOST}/api/anima?q=${encodeURIComponent(q)}&lang=${lang}&mode=${mode}`;
  const r = await fetch(u, { signal: AbortSignal.timeout(TIMEOUT) });
  return r.json();
}

// Questions that MUST yield a real answer in hybrid mode (offline tier or online escalation).
const MUST_ANSWER = [
  // --- precise facts (Wikidata / KGE) ---
  ['chi è Leonardo da Vinci', 'it'],
  ['quando è nato Albert Einstein', 'it'],
  ['chi ha scritto la Divina Commedia', 'it'],
  ['capitale del Giappone', 'it'],
  ['in che continente si trova il Brasile', 'it'],
  // --- definitions / concepts (Wikipedia / Wiktionary) ---
  ['cos’è la fotosintesi', 'it'],
  ['cosa significa effimero', 'it'],
  ['cos’è Python', 'it'],
  ['che cos’è un transistor', 'it'],
  // --- bare entities ---
  ['torre eiffel', 'it'],
  ['Napoleone', 'it'],
  // --- open reasoning (only Grok can answer -> tests the teacher escalation) ---
  ['perché il cielo è blu', 'it'],
  ['come si fa il caffè', 'it'],
  ['raccontami una barzelletta', 'it'],
  ['consigliami un libro di fantascienza', 'it'],
  ['spiegami la relatività in parole semplici', 'it'],
  ['scrivi una breve poesia sul mare', 'it'],
  ['come stai?', 'it'],
  // --- math / utilities (offline) ---
  ['quanto fa 15 per 23', 'it'],
  ['quanto fa 100 diviso 7', 'it'],
  // --- time / system (offline) ---
  ['che ora è', 'it'],
  ['che giorno è oggi', 'it'],
  // --- identity / capabilities (offline) ---
  ['chi sei', 'it'],
  ['cosa sai fare', 'it'],
  // --- weather (live) ---
  ['meteo milano', 'it'],
  ['che tempo fa domani a roma', 'it'],
  // --- greetings / social (L0 or Grok) ---
  ['ciao', 'it'],
  ['grazie', 'it'],
  // --- English ---
  ['who is Marie Curie', 'en'],
  ['what is the speed of light', 'en'],
  ['tell me a fun fact', 'en'],
];

let answered = 0, dontknow = 0, errored = 0;
const misses = [], errors = [];

async function main() {
  console.log(`HYBRID test vs ${HOST} (mode=on, timeout ${TIMEOUT}ms)\n`);
  for (const [q, lang] of MUST_ANSWER) {
    try {
      const r = await ask(q, lang);
      const reply = (r.reply || '').replace(/\s+/g, ' ').trim();
      if (isDontknow(reply)) { dontknow++; misses.push(`✗ [${lang}] ${q}  ::  tier=${r.tier} intent=${r.intent} :: ${reply || '(vuoto)'}`); console.log(`✗ DONTKNOW [${lang}] ${q} (tier=${r.tier} intent=${r.intent})`); }
      else { answered++; console.log(`✓ [${lang}] ${q}\n    (${r.domain||r.tier}/${r.intent}) ${reply.slice(0, 120)}${reply.length > 120 ? '…' : ''}`); }
    } catch (e) { errored++; errors.push(`⚠ [${lang}] ${q}  ::  ${e.message}`); console.log(`⚠ ERROR [${lang}] ${q} :: ${e.message}`); }
  }
  console.log(`\n=== HYBRID: ${answered} answered, ${dontknow} don't-know, ${errored} errored (of ${MUST_ANSWER.length}) ===`);
  if (misses.length) { console.log('\nMISSES (should have answered):\n' + misses.join('\n')); }
  if (errors.length) { console.log('\nERRORS:\n' + errors.join('\n')); }
  process.exit(dontknow + errored);
}
main().catch(e => { console.error('runner error', e); process.exit(99); });
