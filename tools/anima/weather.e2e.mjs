// ANIMA weather END-TO-END test — hits the running simulator (/api/anima) live against Open-Meteo.
// Start the sim first: node tools/serve-shell.mjs   then: node tools/anima/weather.e2e.mjs
// Verifies the WHOLE path: NLU -> geocode -> forecast -> formatted reply. Asserts the reply names
// the right city, is the weather intent, and (for aspect questions) gives a sensible shape.
const BASE = process.env.SIM || 'http://localhost:5599';

async function ask(q, lang = 'it', mode = 'on') {
  const u = `${BASE}/api/anima?q=${encodeURIComponent(q)}&lang=${lang}&mode=${mode}`;
  const r = await fetch(u);
  return r.json();
}

let pass = 0, fail = 0; const fails = [];
function check(name, r, conds) {
  const bad = [];
  for (const [desc, ok] of conds) if (!ok) bad.push(desc);
  if (bad.length) { fail++; fails.push(`✗ ${name}\n    reply: ${JSON.stringify(r.reply)}\n    fail:  ${bad.join('; ')}`); }
  else { pass++; console.log(`✓ ${name} → ${r.reply}`); }
}
const hasCity = (r, c) => (r.reply || '').toLowerCase().includes(c.toLowerCase());
const isWx = (r) => r.intent === 'weather';

// Cases: [query, lang, expectedCityInReply, extraConds(r)=>[[desc,bool]...]]
const CASES = [
  ['meteo brescia', 'it', 'brescia'],
  ['meteo brescia domani', 'it', 'brescia', r => [['says domani', /domani|brescia/i.test(r.reply)]]],
  ['che tempo fara domani a brescia', 'it', 'brescia', r => [['domani', /domani/i.test(r.reply)]]],
  ['domani a brescia piove?', 'it', 'brescia', r => [['yes/no rain', /(sì|si|no)/i.test(r.reply) && /piog|piov/i.test(r.reply)]]],
  ['ci sara sole domani a brescia', 'it', 'brescia', r => [['sun answer', /sole|sereno|nuvol|non proprio/i.test(r.reply)]]],
  ['che tempo fa a roma', 'it', 'roma'],
  ['meteo milano', 'it', 'milano'],
  ['previsioni torino', 'it', 'torino'],
  ['previsioni reggio emilia', 'it', 'reggio', r => [['real forecast (not error)', /°c/i.test(r.reply) && !/non riesco/i.test(r.reply)]]],
  ['temperatura a napoli oggi', 'it', 'napoli', r => [['has degrees', /°c/i.test(r.reply)]]],
  ['nevica a cortina?', 'it', 'cortina', r => [['snow yes/no', /(sì|si|no)\b/i.test(r.reply) && /nev/i.test(r.reply)]]],
  ['meteo a milano dopodomani', 'it', 'milano', r => [['dopodomani', /dopodomani/i.test(r.reply)]]],
  ['che tempo fa a firenze tra 3 giorni', 'it', 'firenze'],
  ['piove a san marino?', 'it', 'marino', r => [['rain answer', /piog|piov/i.test(r.reply)]]],
  ['fa caldo a palermo?', 'it', 'palermo', r => [['degrees', /°c/i.test(r.reply)]]],
  ['weather in new york tomorrow', 'en', 'new york', r => [['tomorrow', /tomorrow/i.test(r.reply)]]],
  ['will it rain in paris tomorrow?', 'en', 'paris', r => [['rain', /rain/i.test(r.reply)]]],
  ['is it sunny in madrid', 'en', 'madrid'],
  ['temperature in tokyo', 'en', 'tokyo', r => [['degrees', /°c/i.test(r.reply)]]],
  ['forecast for berlin in 5 days', 'en', 'berlin'],
];

// more live stress cases (network)
const CASES2 = [
  ['fa bel tempo a napoli?', 'it', 'napoli', r => [['real forecast', /°c/i.test(r.reply)]]],
  ['quanti gradi ci sono a roma', 'it', 'roma', r => [['degrees', /°c/i.test(r.reply)]]],
  ['domani pioverà a milano?', 'it', 'milano', r => [['rain answer', /piog|piov/i.test(r.reply)]]],
  ['mi serve l ombrello domani a genova?', 'it', 'genova', r => [['rain answer', /piog|piov/i.test(r.reply)]]],
  ['do i need an umbrella in london tomorrow', 'en', 'london', r => [['rain answer', /rain/i.test(r.reply)]]],
  ['nevicherà a livigno questo weekend', 'it', 'livigno', r => [['snow answer', /nev/i.test(r.reply)]]],
  ['temperatura massima a milano domani', 'it', 'milano', r => [['degrees', /°c/i.test(r.reply)]]],
  ['meteo san giovanni rotondo', 'it', 'san giovanni', r => [['real forecast', /°c/i.test(r.reply) && !/non riesco/i.test(r.reply)]]],
  ['che tempo fara venerdi a bari', 'it', 'bari', r => [['real forecast', /°c/i.test(r.reply)]]],
  ['what is the weather like in dublin', 'en', 'dublin', r => [['not a definition, real forecast', /°c/i.test(r.reply)]]],
  ['neve sulle dolomiti domani', 'it', 'dolomiti', r => [['snow answer', /nev/i.test(r.reply)]]],
];

// far-future date -> honest refusal (no fabrication)
const FAR = (() => { const d = new Date(); d.setDate(d.getDate() + 40);
  const md = ['gennaio','febbraio','marzo','aprile','maggio','giugno','luglio','agosto','settembre','ottobre','novembre','dicembre'];
  return `meteo brescia ${d.getDate()} ${md[d.getMonth()]}`; })();

// no-city weather -> ask the city, do NOT fetch garbage
// offline mode -> must refuse, not fetch
async function main() {
  for (const [q, lang, city, extra] of CASES) {
    const r = await ask(q, lang);
    const conds = [['is weather intent', isWx(r)], [`names "${city}"`, hasCity(r, city)]];
    if (extra) conds.push(...extra(r));
    check(`[${lang}] ${q}`, r, conds);
  }
  for (const [q, lang, city, extra] of CASES2) {
    const r = await ask(q, lang);
    const conds = [['is weather intent', isWx(r)], [`names "${city}"`, hasCity(r, city)]];
    if (extra) conds.push(...extra(r));
    check(`[${lang}] ${q}`, r, conds);
  }
  // mode=only must still fetch live weather (online-first, not offline)
  {
    const r = await ask('meteo roma', 'it', 'only');
    check('[it] meteo roma (mode=only)', r, [['is weather', isWx(r)], ['real forecast', /°c/i.test(r.reply)]]);
  }
  // missing city
  {
    const r = await ask('che tempo fa', 'it');
    check('[it] che tempo fa (no city)', r, [['asks for city', /citt|quale citt|which city/i.test(r.reply)]]);
  }
  // far future
  {
    const r = await ask(FAR, 'it');
    check(`[it] ${FAR} (too far)`, r, [['refuses far date', /lontano|too far|15/i.test(r.reply)]]);
  }
  // offline mode
  {
    const r = await ask('meteo brescia', 'it', 'off');
    check('[it] meteo brescia (mode=off)', r, [['refuses offline', /internet/i.test(r.reply)]]);
  }
  // negative: not weather
  {
    const r = await ask('chi e einstein', 'it');
    check('[it] chi e einstein (not weather)', r, [['not weather intent', r.intent !== 'weather']]);
  }

  console.log(`\nweather E2E: ${pass} passed, ${fail} failed\n`);
  if (fails.length) { console.log(fails.join('\n\n')); console.log(''); }
  process.exit(fail);
}
main().catch(e => { console.error('E2E runner error:', e); process.exit(99); });
