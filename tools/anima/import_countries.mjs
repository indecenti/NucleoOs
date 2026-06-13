#!/usr/bin/env node
// ANIMA importer #2 — world countries (GeoNames countryInfo, CC-BY) -> grounded bilingual cards.
// Highest-value, CERTAIN geography knowledge: capital, continent, population for ~195 sovereign countries.
// "capitale del Kenya", "in che continente è il Brasile", "quanti abitanti ha l'India" — all answered offline.
// Values come from the downloaded table (authoritative). IT country/capital names are a curated map; the
// long tail falls back to the EN name (still correct). Cross-corpus ask-dedup avoids clashing with existing
// geo cards. Output: tools/anima/knowledge.staged/countries.jsonl (staged; lands via AKB5 / a gated rebuild).
import { readFileSync, writeFileSync, existsSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const SRC = join(here, '.cache', 'countryInfo.txt');
const OUT = join(here, 'knowledge.staged', 'countries.jsonl');
if (!existsSync(SRC)) { console.error('missing countryInfo.txt — fetch download.geonames.org/export/dump/countryInfo.txt into tools/anima/.cache/'); process.exit(2); }

const CONT = { EU: ['Europa', 'Europe'], AS: ['Asia', 'Asia'], AF: ['Africa', 'Africa'],
  NA: ['America del Nord', 'North America'], SA: ['America del Sud', 'South America'],
  OC: ['Oceania', 'Oceania'], AN: ['Antartide', 'Antarctica'] };

// Curated EN->IT country names (the ones users actually ask about). Long tail falls back to the EN name.
const IT_COUNTRY = {
  'Italy':'Italia','France':'Francia','Germany':'Germania','Spain':'Spagna','Portugal':'Portogallo',
  'United Kingdom':'Regno Unito','Ireland':'Irlanda','Netherlands':'Paesi Bassi','Belgium':'Belgio',
  'Switzerland':'Svizzera','Austria':'Austria','Greece':'Grecia','Poland':'Polonia','Sweden':'Svezia',
  'Norway':'Norvegia','Finland':'Finlandia','Denmark':'Danimarca','Iceland':'Islanda','Russia':'Russia',
  'Ukraine':'Ucraina','Czechia':'Repubblica Ceca','Slovakia':'Slovacchia','Hungary':'Ungheria',
  'Romania':'Romania','Bulgaria':'Bulgaria','Croatia':'Croazia','Serbia':'Serbia','Slovenia':'Slovenia',
  'Turkey':'Turchia','United States':'Stati Uniti','Canada':'Canada','Mexico':'Messico','Brazil':'Brasile',
  'Argentina':'Argentina','Chile':'Cile','Colombia':'Colombia','Peru':'Perù','Venezuela':'Venezuela',
  'China':'Cina','Japan':'Giappone','South Korea':'Corea del Sud','North Korea':'Corea del Nord',
  'India':'India','Pakistan':'Pakistan','Indonesia':'Indonesia','Thailand':'Thailandia','Vietnam':'Vietnam',
  'Philippines':'Filippine','Malaysia':'Malesia','Singapore':'Singapore','Israel':'Israele','Iran':'Iran',
  'Iraq':'Iraq','Saudi Arabia':'Arabia Saudita','United Arab Emirates':'Emirati Arabi Uniti','Egypt':'Egitto',
  'Morocco':'Marocco','Algeria':'Algeria','Tunisia':'Tunisia','Libya':'Libia','Nigeria':'Nigeria',
  'Kenya':'Kenya','Ethiopia':'Etiopia','South Africa':'Sudafrica','Ghana':'Ghana','Senegal':'Senegal',
  'Australia':'Australia','New Zealand':'Nuova Zelanda','Cuba':'Cuba','Jamaica':'Giamaica',
  'Lithuania':'Lituania','Latvia':'Lettonia','Estonia':'Estonia','Luxembourg':'Lussemburgo',
  'Cyprus':'Cipro','Malta':'Malta','Albania':'Albania','Bosnia and Herzegovina':'Bosnia ed Erzegovina',
  'North Macedonia':'Macedonia del Nord','Montenegro':'Montenegro','Qatar':'Qatar','Kuwait':'Kuwait',
  'Jordan':'Giordania','Lebanon':'Libano','Syria':'Siria','Afghanistan':'Afghanistan','Bangladesh':'Bangladesh',
};
// Curated IT exonyms for well-known capitals (others keep the GeoNames spelling).
const IT_CAPITAL = {
  'London':'Londra','Paris':'Parigi','Berlin':'Berlino','Madrid':'Madrid','Rome':'Roma','Lisbon':'Lisbona',
  'Moscow':'Mosca','Beijing':'Pechino','Athens':'Atene','Vienna':'Vienna','Brussels':'Bruxelles',
  'Warsaw':'Varsavia','Prague':'Praga','Budapest':'Budapest','Bucharest':'Bucarest','Belgrade':'Belgrado',
  'Copenhagen':'Copenaghen','Stockholm':'Stoccolma','Helsinki':'Helsinki','Oslo':'Oslo','Dublin':'Dublino',
  'Bern':'Berna','Amsterdam':'Amsterdam','Cairo':'Il Cairo','New Delhi':'Nuova Delhi','Tokyo':'Tokyo',
  'Seoul':'Seul','Bangkok':'Bangkok','Lisbon':'Lisbona','Kyiv':'Kiev','Ankara':'Ankara','Tehran':'Teheran',
};
const slug = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/[^a-z0-9]+/g,'-').replace(/^-|-$/g,'');
const fmtPop = (n) => { n = +n; return n >= 1e6 ? (n/1e6).toFixed(n>=1e7?0:1).replace('.0','')+ ' milioni' : n.toLocaleString('it-IT'); };

// cross-corpus ask dedup (don't clash with existing geo cards)
const norm = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/[^a-z0-9 ]/g,'').replace(/\s+/g,' ').trim();
const claimed = new Set();
const kdir = join(here, 'knowledge');
for (const f of readdirSync(kdir)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(kdir,f),'utf8').split('\n')) { if (!l.trim()||l.startsWith('//')) continue;
    let j; try { j = JSON.parse(l); } catch { continue; }
    for (const lg of ['it','en']) for (const a of (j.ask?.[lg]||[])) claimed.add(norm(a)); } }
const dedup = (arr) => arr.filter(a => { const n = norm(a); if (claimed.has(n)) return false; claimed.add(n); return true; });

const cards = []; let dropped = 0;
for (const line of readFileSync(SRC,'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#')) continue;
  const c = line.split('\t');
  const [iso, , , , nameEn, capital, , pop, contCode] = c;
  if (!nameEn || !capital) continue;
  if (+pop < 100000) continue;                    // skip micro-territories (keep it non-niche)
  const it = IT_COUNTRY[nameEn] || nameEn;
  const cap = capital, capIt = IT_CAPITAL[capital] || capital;
  const cont = CONT[contCode];
  const askIt = dedup([`capitale di ${it}`, `qual è la capitale di ${it}`, `capitale del ${it}`, `capitale della ${it}`]);
  const askEn = dedup([`capital of ${nameEn}`, `what is the capital of ${nameEn}`]);
  if (!askIt.length && !askEn.length) { dropped++; continue; }
  const detIt = cont ? `${it} si trova in ${cont[0]}${+pop ? ` e ha circa ${fmtPop(pop)} di abitanti` : ''}.` : '';
  const detEn = cont ? `${nameEn} is in ${cont[1]}${+pop ? `, population about ${(+pop/1e6).toFixed(1)} million` : ''}.` : '';
  // Parenthetical phrasing sidesteps Italian country articles (del/della/degli/dell'… too irregular for a
  // safe heuristic — Kenya/Canada end in -a yet are masculine) while staying natural and 100% correct.
  cards.push({ id: `geo.country.${slug(nameEn)}`, category: 'geography', action: 'answer', arg: '',
    reply: { it: `La capitale è ${capIt} (${it}).`, en: `The capital is ${cap} (${nameEn}).` },
    ask: { it: askIt.length?askIt:[`capitale di ${it}`], en: askEn.length?askEn:[`capital of ${nameEn}`] },
    detail: { it: detIt, en: detEn }, lang_primary: 'bi', source: 'geonames:countryInfo', tags: ['country','capital','geography'] });
}
writeFileSync(OUT, cards.map(c => JSON.stringify(c)).join('\n') + '\n');
console.log(`[countries] wrote ${cards.length} country cards -> ${OUT}  (${dropped} skipped: all asks already claimed)`);
console.log(`[countries] sample: ${cards.find(c=>c.id.includes('kenya'))?.reply.it}  ||  ${cards.find(c=>c.id.includes('brazil'))?.reply.it}`);
