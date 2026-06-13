#!/usr/bin/env node
// ANIMA importer — IMPORTANT PEOPLE from WIKIDATA (CC0), grounded & bilingual. NO gossip: only substantive
// occupations (science, letters, philosophy, art, history, invention, statecraft), filtered by notability
// (Wikipedia sitelink count). Every fact (birth/death year, occupation, country) is COPIED from Wikidata;
// the reply is a fixed TEMPLATE over those facts (no model, no invention) -> cannot hallucinate.
// Output: tools/anima/knowledge.staged/people-wd.jsonl  (lands via AKB5, certified by anima:gate-akb5).
//
//   node tools/anima/import_people.mjs [--min <sitelinks>] [--per <perOcc>]
import { writeFileSync, readFileSync, readdirSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const OUT = join(here, 'knowledge.staged', 'people-wd.jsonl');
const UA = 'NucleoOS-ANIMA-Import/1.0 (offline edge assistant; people knowledge; CC0 Wikidata)';
const WDQS = 'https://query.wikidata.org/sparql';
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const MIN_SITELINKS = +arg('--min', 45);     // notability floor ("important")
const PER_OCC = +arg('--per', 120);          // cap per occupation (keeps the endpoint happy)

// Substantive occupations only (QID -> [it, en] role word). Deliberately NO actor/singer/footballer/model.
const OCC = {
  Q901:      ['scienziato/a', 'scientist'],        Q169470:  ['fisico/a', 'physicist'],
  Q170790:   ['matematico/a', 'mathematician'],    Q593644:  ['chimico/a', 'chemist'],
  Q864503:   ['biologo/a', 'biologist'],           Q11063:   ['astronomo/a', 'astronomer'],
  Q36180:    ['scrittore/scrittrice', 'writer'],   Q49757:   ['poeta/poetessa', 'poet'],
  Q4964182:  ['filosofo/a', 'philosopher'],        Q201788:  ['storico/a', 'historian'],
  Q1028181:  ['pittore/pittrice', 'painter'],      Q36834:   ['compositore/compositrice', 'composer'],
  Q205375:   ['inventore/inventrice', 'inventor'], Q188094:  ['economista', 'economist'],
  Q11631:    ['astronauta', 'astronaut'],          Q1234713: ['teologo/a', 'theologian'],
  Q3242115:  ['matematico/a', 'mathematician'],    Q14467526:['esploratore/esploratrice', 'explorer'],
  Q82955:    ['politico/a', 'politician'],         Q372436:  ['statista', 'statesperson'],
  Q116:      ['monarca', 'monarch'],               Q1097498: ['ingegnere', 'engineer'],
  Q82594:    ['informatico/a', 'computer scientist'], Q5482740: ['programmatore/programmatrice', 'programmer'],
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const qid = (u) => (u || '').replace(/.*\/(Q\d+)$/, '$1');
const yr = (iso) => { const m = /(-?\d{1,4})-\d\d-\d\d/.exec(iso || ''); if (!m) return null; const y = +m[1]; return y < 0 ? `${-y} a.C.` : String(y); };
const slug = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

async function sparql(query) {
  const u = WDQS + '?format=json&query=' + encodeURIComponent(query);
  for (let a = 0; a < 3; a++) {
    try {
      const r = await fetch(u, { headers: { 'User-Agent': UA, Accept: 'application/sparql-results+json' } });
      if (r.status === 429) { await sleep(2500 * (a + 1)); continue; }
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return (await r.json()).results.bindings;
    } catch (e) { if (a === 2) throw e; await sleep(1500); }
  }
  return [];
}

// one occupation -> the most notable people in it, with grounded dates + country (IT/EN labels)
function queryFor(occQ) {
  return `SELECT ?p ?pIt ?pEn ?dob ?dod ?cIt ?cEn ?sl WHERE {
    ?p wdt:P106 wd:${occQ} ; wdt:P569 ?dob ; wikibase:sitelinks ?sl .
    FILTER(?sl >= ${MIN_SITELINKS})
    OPTIONAL { ?p wdt:P570 ?dod }
    OPTIONAL { ?p wdt:P27 ?c .
      ?c rdfs:label ?cIt FILTER(LANG(?cIt)="it") . OPTIONAL { ?c rdfs:label ?cEn FILTER(LANG(?cEn)="en") } }
    ?p rdfs:label ?pIt FILTER(LANG(?pIt)="it") .
    OPTIONAL { ?p rdfs:label ?pEn FILTER(LANG(?pEn)="en") }
  } ORDER BY DESC(?sl) LIMIT ${PER_OCC}`;
}

// existing asks (cross-corpus dedup) so we never clash with curated cards
const norm = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9 ]/g, '').replace(/\s+/g, ' ').trim();
const claimed = new Set();
const kdir = join(here, 'knowledge');
for (const f of readdirSync(kdir)) {
  if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(kdir, f), 'utf8').split('\n')) {
    if (!l.trim() || l.startsWith('//')) continue;
    try { const c = JSON.parse(l); for (const a of (c.ask?.it || [])) claimed.add(norm(a)); } catch {}
  }
}

const byQid = new Map();
const occs = Object.keys(OCC);
for (let i = 0; i < occs.length; i++) {
  const oq = occs[i];
  process.stderr.write(`[people] ${oq} ${OCC[oq][1]} (${i + 1}/${occs.length}) … `);
  let rows = [];
  try { rows = await sparql(queryFor(oq)); } catch (e) { process.stderr.write('ERR ' + e.message + '\n'); continue; }
  process.stderr.write(rows.length + ' rows\n');
  for (const b of rows) {
    const q = qid(b.p?.value); if (!q) continue;
    const cur = byQid.get(q);
    const sl = +(b.sl?.value || 0);
    if (cur && cur.sl >= sl) { cur.occ.add(oq); continue; }   // keep highest-sitelink row, accumulate occupations
    byQid.set(q, { q, sl, it: b.pIt?.value, en: b.pEn?.value || b.pIt?.value,
      dob: b.dob?.value, dod: b.dod?.value, cIt: b.cIt?.value, cEn: b.cEn?.value || b.cIt?.value,
      occ: new Set([oq]) });
  }
  await sleep(300);   // be a good citizen
}

// occupation priority: pick the most specific role, not a random one from the matched set
const OCC_PRI = ['Q82594', 'Q5482740', 'Q169470', 'Q170790', 'Q593644', 'Q864503', 'Q11063', 'Q4964182', 'Q36180', 'Q49757',
  'Q201788', 'Q1028181', 'Q36834', 'Q205375', 'Q188094', 'Q11631', 'Q1234713', 'Q14467526', 'Q1097498',
  'Q3242115', 'Q901', 'Q372436', 'Q116', 'Q82955'];
// DOMAIN sub-sharding: each person lands in a category shard by domain, so AKB5 builds several smaller
// person shards (finer routing, bounded per-query SD read, better recall) instead of one giant slab.
const DOM = {
  Q169470: 'science', Q170790: 'science', Q593644: 'science', Q864503: 'science', Q11063: 'science',
  Q188094: 'science', Q1097498: 'science', Q205375: 'science', Q901: 'science', Q3242115: 'science', Q11631: 'science',
  Q82594: 'tech', Q5482740: 'tech',
  Q36180: 'letters', Q49757: 'letters', Q4964182: 'letters', Q201788: 'letters', Q1234713: 'letters',
  Q1028181: 'arts', Q36834: 'arts',
  Q82955: 'power', Q372436: 'power', Q116: 'power', Q14467526: 'explore',
};
const MON_IT = ['gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'];
const MON_EN = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
function fdate(iso, en) {                                  // grounded full date from Wikidata ISO; year-only if imprecise
  const m = /(-?\d{1,4})-(\d\d)-(\d\d)/.exec(iso || ''); if (!m) return null;
  let y = +m[1]; const mo = +m[2], d = +m[3];
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || y === 0) { const yy = yr(iso); return yy ? (en ? `in ${yy}` : `nel ${yy}`) : null; }
  const bc = y < 0; if (bc) y = -y;
  return en ? `on ${MON_EN[mo - 1]} ${d}, ${y}${bc ? ' BC' : ''}` : `il ${d} ${MON_IT[mo - 1]} ${y}${bc ? ' a.C.' : ''}`;
}
// SURNAME asks: people are usually queried by surname ("chi è Mozart", not the full label). Assign each
// surname's short asks to the SINGLE most-notable bearer (highest sitelinks) to avoid collisions (Freud ->
// Sigmund not Lucian; Curie -> Marie). Closes the biggest people-recall hole (surname > full-name queries).
const PARTICLES = new Set(['di', 'de', 'del', 'della', 'dei', 'degli', 'da', 'dal', 'van', 'von', 'der', 'den', 'la', 'le', 'el', 'du', 'dos', 'das', 'ten', 'ter']);
const surnameOf = (nm) => { const t = (nm || '').split(/\s+/).filter(Boolean); if (t.length < 2) return null;
  let s = t[t.length - 1]; if (PARTICLES.has(t[t.length - 2].toLowerCase())) s = t[t.length - 2] + ' ' + s;
  return s.length >= 3 ? s : null; };
const topSurIt = new Map(), topSurEn = new Map();
for (const p of byQid.values()) { if (!p.it || p.it.startsWith('Q') || !yr(p.dob)) continue;
  const si = surnameOf(p.it), se = surnameOf(p.en);
  if (si) { const k = norm(si); const c = topSurIt.get(k); if (!c || p.sl > c.sl) topSurIt.set(k, { q: p.q, sl: p.sl, s: si }); }
  if (se) { const k = norm(se); const c = topSurEn.get(k); if (!c || p.sl > c.sl) topSurEn.set(k, { q: p.q, sl: p.sl, s: se }); } }
const cards = [];
for (const p of byQid.values()) {
  const name = p.it; if (!name || name.startsWith('Q')) continue;             // skip unlabelled
  const by = yr(p.dob); if (!by) continue;                                    // require a birth year (dated)
  const dy = yr(p.dod);
  const oq = OCC_PRI.find((o) => p.occ.has(o)) || [...p.occ][0];              // most specific role
  const occIt = OCC[oq][0], occEn = OCC[oq][1];
  const cat = 'person-' + (DOM[oq] || 'misc');                               // domain sub-shard
  const span = dy ? `${by}–${dy}` : `n. ${by}`, spanEn = dy ? `${by}–${dy}` : `b. ${by}`;
  const cIt = p.cIt ? `, ${p.cIt}` : '', cEn = p.cEn ? `, ${p.cEn}` : '';
  const add = (kind, id, askIt, askEn, rIt, rEn) => {
    if (askIt.some((a) => claimed.has(norm(a)))) return;                      // already covered by a curated card
    cards.push({ id, category: cat, action: 'answer', arg: '', reply: { it: rIt.slice(0, 248), en: rEn.slice(0, 248) },
      ask: { it: askIt, en: askEn }, source: `wikidata:${p.q}${kind}`, lang_primary: 'bi', tags: ['person', 'wikidata'] });
  };
  // surname asks for the MOST-notable bearer only (avoids "chi è Freud" routing to the wrong Freud)
  const siT = surnameOf(name), siE = surnameOf(p.en);
  const snIt = (siT && norm(siT) !== norm(name) && topSurIt.get(norm(siT))?.q === p.q) ? siT : null;
  const snEn = (siE && norm(siE) !== norm(p.en) && topSurEn.get(norm(siE))?.q === p.q) ? siE : null;
  // BIO card
  add('', `wd.person.${slug(name)}-${p.q.toLowerCase()}`,
    [`chi è ${name}`, `chi era ${name}`, `cosa sai di ${name}`, `parlami di ${name}`, `che lavoro faceva ${name}`, `${name}`,
      ...(snIt ? [`chi è ${snIt}`, `chi era ${snIt}`, `${snIt}`] : [])],
    [`who is ${p.en}`, `who was ${p.en}`, `tell me about ${p.en}`, `${p.en}`, ...(snEn ? [`who is ${snEn}`, `${snEn}`] : [])],
    `${name} (${span}): ${occIt}${cIt}.`, `${p.en} (${spanEn}): ${occEn}${cEn}.`);
  // BORN card with the PRECISE date — covers "quando è nato X" universally (closes the KGE-via-card AKB5 gap)
  const fbIt = fdate(p.dob, false), fbEn = fdate(p.dob, true);
  if (fbIt) add('#P569', `wd.born.${slug(name)}-${p.q.toLowerCase()}`,
    [`quando è nato ${name}`, `quando è nata ${name}`, `in che anno è nato ${name}`, `data di nascita di ${name}`,
      ...(snIt ? [`quando è nato ${snIt}`, `quando è nata ${snIt}`] : [])],
    [`when was ${p.en} born`, `${p.en} birth date`, ...(snEn ? [`when was ${snEn} born`] : [])],
    `${name} è nato/a ${fbIt}.`, `${p.en} was born ${fbEn}.`);
  // DIED card with the precise date
  const fdIt = fdate(p.dod, false), fdEn = fdate(p.dod, true);
  if (fdIt) add('#P570', `wd.died.${slug(name)}-${p.q.toLowerCase()}`,
    [`quando è morto ${name}`, `quando è morta ${name}`, `data di morte di ${name}`],
    [`when did ${p.en} die`, `${p.en} death date`],
    `${name} è morto/a ${fdIt}.`, `${p.en} died ${fdEn}.`);
}

if (!existsSync(dirname(OUT))) mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, cards.map((c) => JSON.stringify(c)).join('\n') + '\n');
console.log(`[people] ${cards.length} grounded people cards -> ${OUT}  (min sitelinks ${MIN_SITELINKS}, ${occs.length} occupations)`);
