// forge-verify-gen.mjs — generate a large NATURAL-LANGUAGE test corpus for the device-side grounded
// verifier (nucleo_anima_verify_claim), with GROUND-TRUTH expectations anchored to what the real
// corpus actually knows (probed via anima.exe). Capitals (KGE) + birth years + arithmetic + fictional
// entities, in Italian and English. Writes fixtures/forge-verify-cases.jsonl; the gate replays them.
//   node tools/anima-host/forge-verify-gen.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');

// ensure the exe is current
{ const b = spawnSync('node', [join(here, 'anima.mjs'), '--ensure'], { encoding: 'utf8' }); if (b.status !== 0) { console.error('build failed'); process.exit(1); } }

function verdict(spec, en) {
  const r = spawnSync(exe, (en ? ['--en'] : []).concat(['--verify', spec]), { encoding: 'utf8' });
  return (/VERDICT=(\w+)/.exec(r.stdout || '') || [, 'error'])[1];
}

// [country_en, country_it, capital_en, capital_it]
const COUNTRIES = [
  ['France', 'Francia', 'Paris', 'Parigi'], ['Germany', 'Germania', 'Berlin', 'Berlino'],
  ['Spain', 'Spagna', 'Madrid', 'Madrid'], ['Italy', 'Italia', 'Rome', 'Roma'],
  ['Portugal', 'Portogallo', 'Lisbon', 'Lisbona'], ['England', 'Inghilterra', 'London', 'Londra'],
  ['Russia', 'Russia', 'Moscow', 'Mosca'], ['China', 'Cina', 'Beijing', 'Pechino'],
  ['Japan', 'Giappone', 'Tokyo', 'Tokyo'], ['Greece', 'Grecia', 'Athens', 'Atene'],
  ['Austria', 'Austria', 'Vienna', 'Vienna'], ['Poland', 'Polonia', 'Warsaw', 'Varsavia'],
  ['Belgium', 'Belgio', 'Brussels', 'Bruxelles'], ['Netherlands', 'Paesi Bassi', 'Amsterdam', 'Amsterdam'],
  ['Switzerland', 'Svizzera', 'Bern', 'Berna'], ['Sweden', 'Svezia', 'Stockholm', 'Stoccolma'],
  ['Norway', 'Norvegia', 'Oslo', 'Oslo'], ['Denmark', 'Danimarca', 'Copenhagen', 'Copenaghen'],
  ['Finland', 'Finlandia', 'Helsinki', 'Helsinki'], ['Ireland', 'Irlanda', 'Dublin', 'Dublino'],
  ['Hungary', 'Ungheria', 'Budapest', 'Budapest'], ['Romania', 'Romania', 'Bucharest', 'Bucarest'],
  ['Czechia', 'Repubblica Ceca', 'Prague', 'Praga'], ['Turkey', 'Turchia', 'Ankara', 'Ankara'],
  ['Egypt', 'Egitto', 'Cairo', 'Cairo'], ['Morocco', 'Marocco', 'Rabat', 'Rabat'],
  ['Canada', 'Canada', 'Ottawa', 'Ottawa'], ['Mexico', 'Messico', 'Mexico City', 'Citta del Messico'],
  ['Brazil', 'Brasile', 'Brasilia', 'Brasilia'], ['Argentina', 'Argentina', 'Buenos Aires', 'Buenos Aires'],
  ['India', 'India', 'New Delhi', 'Nuova Delhi'], ['Australia', 'Australia', 'Canberra', 'Canberra'],
  ['Cuba', 'Cuba', 'Havana', 'Avana'], ['Peru', 'Peru', 'Lima', 'Lima'],
  ['Chile', 'Cile', 'Santiago', 'Santiago'], ['Colombia', 'Colombia', 'Bogota', 'Bogota'],
  ['Iran', 'Iran', 'Tehran', 'Teheran'], ['Iraq', 'Iraq', 'Baghdad', 'Baghdad'],
  ['Thailand', 'Thailandia', 'Bangkok', 'Bangkok'], ['Vietnam', 'Vietnam', 'Hanoi', 'Hanoi'],
  ['Cambodia', 'Cambogia', 'Phnom Penh', 'Phnom Penh'], ['Kenya', 'Kenya', 'Nairobi', 'Nairobi'],
  ['Nigeria', 'Nigeria', 'Abuja', 'Abuja'], ['Ukraine', 'Ucraina', 'Kyiv', 'Kiev'],
  ['Croatia', 'Croazia', 'Zagreb', 'Zagabria'], ['Serbia', 'Serbia', 'Belgrade', 'Belgrado'],
  ['Bulgaria', 'Bulgaria', 'Sofia', 'Sofia'], ['Iceland', 'Islanda', 'Reykjavik', 'Reykjavik'],
  ['Cyprus', 'Cipro', 'Nicosia', 'Nicosia'], ['Lebanon', 'Libano', 'Beirut', 'Beirut'],
  ['Israel', 'Israele', 'Jerusalem', 'Gerusalemme'], ['Indonesia', 'Indonesia', 'Jakarta', 'Giacarta'],
];

// people for birth-year claims: [name, year]
const PEOPLE = [
  ['Albert Einstein', 1879], ['Leonardo da Vinci', 1452], ['Dante Alighieri', 1265],
  ['Galileo Galilei', 1564], ['Isaac Newton', 1643], ['Napoleon', 1769], ['Mozart', 1756],
  ['Charles Darwin', 1809], ['Marie Curie', 1867], ['Cristoforo Colombo', 1451],
];

const capTplIt = (i, c) => (i % 2 ? `qual e la capitale di ${c}` : `capitale di ${c}`);
const capTplEn = (i, c) => (i % 2 ? `what is the capital of ${c}` : `capital of ${c}`);
const yearTplIt = (n) => `in che anno e nato ${n}`;
const yearTplEn = (n) => `what year was ${n} born`;

const cases = [];
const add = (lang, kind, key, asserted, expect) => cases.push({ lang, kind, key, asserted, expect });

// ---- CAPITALS: probe with the TRUE capital; emit confirmed/contradicted (covered) or unknown ----
for (let i = 0; i < COUNTRIES.length; i++) {
  const [cEn, cIt, capEn, capIt] = COUNTRIES[i];
  const wrongEn = COUNTRIES[(i + 7) % COUNTRIES.length][2];
  const wrongIt = COUNTRIES[(i + 7) % COUNTRIES.length][3];
  // IT
  const keyIt = capTplIt(i, cIt);
  const vIt = verdict(`fact|${keyIt}|${capIt}`, false);
  if (vIt === 'confirmed') { add('it', 'fact', keyIt, capIt, 'confirmed'); if (wrongIt !== capIt) add('it', 'fact', keyIt, wrongIt, 'contradicted'); }
  else if (vIt === 'unknown') add('it', 'fact', keyIt, capIt, 'unknown');
  // EN
  const keyEn = capTplEn(i, cEn);
  const vEn = verdict(`fact|${keyEn}|${capEn}`, true);
  if (vEn === 'confirmed') { add('en', 'fact', keyEn, capEn, 'confirmed'); if (wrongEn !== capEn) add('en', 'fact', keyEn, wrongEn, 'contradicted'); }
  else if (vEn === 'unknown') add('en', 'fact', keyEn, capEn, 'unknown');
}

// ---- BIRTH YEARS: probe; emit confirmed (true)/contradicted (wrong) where covered ----
for (const [name, year] of PEOPLE) {
  const kIt = yearTplIt(name), kEn = yearTplEn(name);
  if (verdict(`fact|${kIt}|${year}`, false) === 'confirmed') { add('it', 'fact', kIt, String(year), 'confirmed'); add('it', 'fact', kIt, String(year + 5), 'contradicted'); }
  if (verdict(`fact|${kEn}|${year}`, true) === 'confirmed') { add('en', 'fact', kEn, String(year), 'confirmed'); add('en', 'fact', kEn, String(year + 5), 'contradicted'); }
}

// ---- ARITHMETIC: ground-truth (computed here); + -,*,/,^ are exact on a_try_calc ----
function rngPairs(n, seed) { const out = []; let s = seed; const nx = () => (s = (s * 1103515245 + 12345) & 0x7fffffff); for (let i = 0; i < n; i++) { const a = 2 + (nx() % 97), b = 2 + (nx() % 40); out.push([a, b]); } return out; }
const ops = [['+', (a, b) => a + b], ['-', (a, b) => a + b - b /*keep positive*/], ['*', (a, b) => a * b], ['/', null]];
let ai = 0;
for (const [a, b] of rngPairs(10, 7)) {
  for (const lang of ['it', 'en']) {
    const en = lang === 'en';
    add(lang, 'numeric', `${a}+${b}`, String(a + b), 'confirmed');
    add(lang, 'numeric', `${a}*${b}`, String(a * b), 'confirmed');
    add(lang, 'numeric', `${a}+${b}`, String(a + b + 1), 'contradicted');
    add(lang, 'numeric', `${a + b}-${b}`, String(a), 'confirmed');
    ai++;
  }
}
// powers + non-computable (unknown)
for (const lang of ['it', 'en']) {
  add(lang, 'numeric', '2^10', '1024', 'confirmed');
  add(lang, 'numeric', '2^10', '1000', 'contradicted');
  add(lang, 'numeric', lang === 'it' ? '15% di 80' : '15% of 80', '12', 'unknown');   // basic calc abstains
  add(lang, 'numeric', 'sqrt(144)', '12', 'unknown');
}

// ---- FICTIONAL / UNKNOWN-DOMAIN: the brain must abstain, never fabricate ----
const FICT_IT = ['capitale di Atlantide', 'capitale di Wakanda', 'capitale di Floonkistan', 'qual e la capitale di Gondor', 'capitale di Narnia'];
const FICT_EN = ['capital of Atlantis', 'capital of Wakanda', 'capital of Floonkistan', 'what is the capital of Gondor', 'capital of Narnia'];
for (const k of FICT_IT) add('it', 'fact', k, 'Boopolis', 'unknown');
for (const k of FICT_EN) add('en', 'fact', k, 'Boopville', 'unknown');

// ---- write + report ----
const it = cases.filter((c) => c.lang === 'it'), en = cases.filter((c) => c.lang === 'en');
const dist = (a) => a.reduce((m, c) => ((m[c.expect] = (m[c.expect] || 0) + 1), m), {});
mkdirSync(join(here, 'fixtures'), { recursive: true });
writeFileSync(join(here, 'fixtures', 'forge-verify-cases.jsonl'), cases.map((c) => JSON.stringify(c)).join('\n') + '\n');
console.log(`IT: ${it.length}  ${JSON.stringify(dist(it))}`);
console.log(`EN: ${en.length}  ${JSON.stringify(dist(en))}`);
console.log(`total ${cases.length} -> fixtures/forge-verify-cases.jsonl`);
