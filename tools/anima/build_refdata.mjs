// Build the bilingual WORLD reference dataset -> refdata/world.json (the maintainable data file the
// triple/card extractor reads). Two layers:
//   CORE   : a curated, authoritative gazetteer of the load-bearing countries (continent + capital +
//            demonyms, IT/EN). Hand-verified standard geography — NEVER overridden. Guarantees the
//            system's working set is correct (Italia->Europa, statunitense->Stati Uniti, ...).
//   EXTEND : Groq adds the remaining sovereign countries. Geography (continent) measured at 100% on the
//            40 curated capitals; capitals measured at 90% (spelling variants) so a curated capital ALWAYS
//            wins, and a Groq country is dropped unless its continent is one of the five valid slugs.
// Birth years are intentionally NOT sourced here — Groq-8b measured 49% on them (see enrich_born.mjs):
// born stays source-anchored (bios + curated seed). Geography is common, stable, and verifiable; births
// of non-famous people are not.
//
//   node tools/anima/build_refdata.mjs            # regenerate world.json (CORE + Groq EXTEND)
//   node tools/anima/build_refdata.mjs --core     # CORE only, no network
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { askJSON, MODEL_NAME } from './grok.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const OUT = join(here, 'refdata', 'world.json');
const VALID = new Set(['Europa', 'Asia', 'Africa', 'America', 'Oceania']);

// CORE — curated [name_en, continent_it, continent_en, capital_it, capital_en, demonym_it, demonym_en]
const C = (name_en, ci, ce, cap_it, cap_en, di, de) => ({ name_en, continent_it: ci, continent_en: ce, capital_it: cap_it, capital_en: cap_en, demonym_it: di, demonym_en: de });
const CORE = {
  // Europe
  "Italia": C("Italy", "Europa", "Europe", "Roma", "Rome", "italiano", "Italian"),
  "Francia": C("France", "Europa", "Europe", "Parigi", "Paris", "francese", "French"),
  "Spagna": C("Spain", "Europa", "Europe", "Madrid", "Madrid", "spagnolo", "Spanish"),
  "Germania": C("Germany", "Europa", "Europe", "Berlino", "Berlin", "tedesco", "German"),
  "Regno Unito": C("United Kingdom", "Europa", "Europe", "Londra", "London", "britannico", "British"),
  "Russia": C("Russia", "Europa", "Europe", "Mosca", "Moscow", "russo", "Russian"),
  "Grecia": C("Greece", "Europa", "Europe", "Atene", "Athens", "greco", "Greek"),
  "Portogallo": C("Portugal", "Europa", "Europe", "Lisbona", "Lisbon", "portoghese", "Portuguese"),
  "Svizzera": C("Switzerland", "Europa", "Europe", "Berna", "Bern", "svizzero", "Swiss"),
  "Austria": C("Austria", "Europa", "Europe", "Vienna", "Vienna", "austriaco", "Austrian"),
  "Paesi Bassi": C("Netherlands", "Europa", "Europe", "Amsterdam", "Amsterdam", "olandese", "Dutch"),
  "Belgio": C("Belgium", "Europa", "Europe", "Bruxelles", "Brussels", "belga", "Belgian"),
  "Norvegia": C("Norway", "Europa", "Europe", "Oslo", "Oslo", "norvegese", "Norwegian"),
  "Svezia": C("Sweden", "Europa", "Europe", "Stoccolma", "Stockholm", "svedese", "Swedish"),
  "Danimarca": C("Denmark", "Europa", "Europe", "Copenaghen", "Copenhagen", "danese", "Danish"),
  "Finlandia": C("Finland", "Europa", "Europe", "Helsinki", "Helsinki", "finlandese", "Finnish"),
  "Polonia": C("Poland", "Europa", "Europe", "Varsavia", "Warsaw", "polacco", "Polish"),
  "Irlanda": C("Ireland", "Europa", "Europe", "Dublino", "Dublin", "irlandese", "Irish"),
  "Ungheria": C("Hungary", "Europa", "Europe", "Budapest", "Budapest", "ungherese", "Hungarian"),
  "Repubblica Ceca": C("Czech Republic", "Europa", "Europe", "Praga", "Prague", "ceco", "Czech"),
  "Romania": C("Romania", "Europa", "Europe", "Bucarest", "Bucharest", "rumeno", "Romanian"),
  "Ucraina": C("Ukraine", "Europa", "Europe", "Kiev", "Kyiv", "ucraino", "Ukrainian"),
  "Croazia": C("Croatia", "Europa", "Europe", "Zagabria", "Zagreb", "croato", "Croatian"),
  "Serbia": C("Serbia", "Europa", "Europe", "Belgrado", "Belgrade", "serbo", "Serbian"),
  "Slovenia": C("Slovenia", "Europa", "Europe", "Lubiana", "Ljubljana", "sloveno", "Slovenian"),
  // Asia
  "Giappone": C("Japan", "Asia", "Asia", "Tokyo", "Tokyo", "giapponese", "Japanese"),
  "Cina": C("China", "Asia", "Asia", "Pechino", "Beijing", "cinese", "Chinese"),
  "India": C("India", "Asia", "Asia", "Nuova Delhi", "New Delhi", "indiano", "Indian"),
  "Corea del Sud": C("South Korea", "Asia", "Asia", "Seul", "Seoul", "sudcoreano", "South Korean"),
  "Turchia": C("Turkey", "Asia", "Asia", "Ankara", "Ankara", "turco", "Turkish"),
  "Thailandia": C("Thailand", "Asia", "Asia", "Bangkok", "Bangkok", "thailandese", "Thai"),
  "Indonesia": C("Indonesia", "Asia", "Asia", "Giacarta", "Jakarta", "indonesiano", "Indonesian"),
  "Israele": C("Israel", "Asia", "Asia", "Gerusalemme", "Jerusalem", "israeliano", "Israeli"),
  "Pakistan": C("Pakistan", "Asia", "Asia", "Islamabad", "Islamabad", "pakistano", "Pakistani"),
  // Africa
  "Egitto": C("Egypt", "Africa", "Africa", "Il Cairo", "Cairo", "egiziano", "Egyptian"),
  "Sudafrica": C("South Africa", "Africa", "Africa", "Pretoria", "Pretoria", "sudafricano", "South African"),
  "Marocco": C("Morocco", "Africa", "Africa", "Rabat", "Rabat", "marocchino", "Moroccan"),
  // America
  "Stati Uniti": C("United States", "America", "America", "Washington", "Washington", "statunitense", "American"),
  "Brasile": C("Brazil", "America", "America", "Brasilia", "Brasilia", "brasiliano", "Brazilian"),
  "Canada": C("Canada", "America", "America", "Ottawa", "Ottawa", "canadese", "Canadian"),
  "Messico": C("Mexico", "America", "America", "Citta del Messico", "Mexico City", "messicano", "Mexican"),
  "Argentina": C("Argentina", "America", "America", "Buenos Aires", "Buenos Aires", "argentino", "Argentine"),
  "Giamaica": C("Jamaica", "America", "America", "Kingston", "Kingston", "giamaicano", "Jamaican"),
  "Colombia": C("Colombia", "America", "America", "Bogota", "Bogota", "colombiano", "Colombian"),
  "Cuba": C("Cuba", "America", "America", "L'Avana", "Havana", "cubano", "Cuban"),
  "Cile": C("Chile", "America", "America", "Santiago", "Santiago", "cileno", "Chilean"),
  "Venezuela": C("Venezuela", "America", "America", "Caracas", "Caracas", "venezuelano", "Venezuelan"),
  // Oceania
  "Australia": C("Australia", "Oceania", "Oceania", "Canberra", "Canberra", "australiano", "Australian"),
  "Nuova Zelanda": C("New Zealand", "Oceania", "Oceania", "Wellington", "Wellington", "neozelandese", "New Zealander"),
};

async function askGeo(countriesIt) {
  const o = await askJSON(
    'You are a precise geography reference. For each country (ITALIAN name) return capital and continent. Continent MUST be exactly one of: Europa, Asia, Africa, America, Oceania (America covers both Americas). Reply ONLY JSON.',
    `Countries:\n${countriesIt.join('\n')}\n\nJSON {"<Italian name>": {"name_en":"","capital_it":"","capital_en":"","continent_it":"","continent_en":"","demonym_it":"","demonym_en":""}}:`);
  return o && typeof o === 'object' ? o : {};
}

async function run() {
  const coreOnly = process.argv.includes('--core');
  const world = {};
  for (const [k, v] of Object.entries(CORE)) world[k] = { name_it: k, ...v };
  let added = 0;

  if (!coreOnly) {
    console.log(`[refdata] CORE=${Object.keys(CORE).length} curated. Extending via ${MODEL_NAME}...`);
    const enumO = await askJSON('You are a geography reference.',
      'Reply ONLY JSON {"countries":[...]} listing ~170 sovereign countries by common ITALIAN name.');
    let names = (Array.isArray(enumO?.countries) ? enumO.countries : []).filter((n) => !CORE[n]);
    names = [...new Set(names)];
    const recs = {};
    for (let i = 0; i < names.length; i += 20) {
      Object.assign(recs, await askGeo(names.slice(i, i + 20)));
      process.stdout.write(`\r  queried ${Math.min(i + 20, names.length)}/${names.length}`);
    }
    process.stdout.write('\n');
    // dedup by EN identity: Groq sometimes adds a country under a variant/misspelled Italian name
    // (Cechia, Tailandia, Marrocco, Jamaica) that collides with a CORE entry in English and would
    // break it/en graph parity. Skip any whose name_en or capital_en is already taken.
    const usedEn = new Set(Object.values(world).map((v) => v.name_en.toLowerCase()));
    const usedCap = new Set(Object.values(world).map((v) => (v.capital_en || '').toLowerCase()).filter(Boolean));
    for (const [name, g] of Object.entries(recs)) {
      if (CORE[name] || !g || !VALID.has(g.continent_it)) continue;     // CORE wins; drop invalid continent
      if (!g.name_en || !g.capital_it) continue;
      const en = g.name_en.toLowerCase(), cap = (g.capital_en || '').toLowerCase();
      if (usedEn.has(en) || (cap && usedCap.has(cap))) continue;        // EN-identity collision -> skip
      usedEn.add(en); if (cap) usedCap.add(cap);
      world[name] = {
        name_it: name, name_en: g.name_en,
        continent_it: g.continent_it, continent_en: g.continent_en || g.continent_it,
        capital_it: g.capital_it, capital_en: g.capital_en || g.capital_it,
        demonym_it: g.demonym_it || '', demonym_en: g.demonym_en || '',
      };
      added++;
    }
  }

  // invariant: every record has a valid continent
  const bad = Object.entries(world).filter(([, v]) => !VALID.has(v.continent_it)).map(([k]) => k);
  if (bad.length) { console.error('[refdata] ABORT: invalid continent for', bad.join(', ')); process.exit(1); }

  mkdirSync(dirname(OUT), { recursive: true });
  writeFileSync(OUT, JSON.stringify(world, null, 1) + '\n');
  const byCont = {};
  for (const v of Object.values(world)) byCont[v.continent_it] = (byCont[v.continent_it] || 0) + 1;
  console.log(`[refdata] wrote ${Object.keys(world).length} countries (CORE ${Object.keys(CORE).length} + Groq ${added}) -> world.json`);
  console.log(`  by continent: ${JSON.stringify(byCont)}`);
}

run();
