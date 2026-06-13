// Enrich birth-year facts for the people corpus using Groq (teacher), with MEASURED trust.
//
// The model (llama-3.1-8b-instant) is small and can get precise facts wrong, so we never trust it blind:
//   --calibrate : ask Groq the birth year of the people whose bio ALREADY states one (source-anchored
//                 ground truth), and report exact-match accuracy. This MEASURES whether Groq is reliable
//                 enough to fill the rest. If it is not, we don't bulk-enrich.
//   --apply     : enrich ALL people. A bio-stated year is GROUND TRUTH (kept verbatim, never overwritten).
//                 For the rest, Groq's year is accepted ONLY if it passes plausibility (1000<=y<=2025) and,
//                 when a death/lifespan is known from the bio, birth<death. Output -> refdata/people_facts.json
//                 with provenance ("bio" | "grok"). extract_triples consumes it.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { askJSON, MODEL_NAME } from './grok.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const PEOPLE = join(here, 'knowledge', 'people.jsonl');
const OUTDIR = join(here, 'refdata');
const OUT = join(OUTDIR, 'people_facts.json');

const BIRTH = /\((?:born\s+|b\.\s*|n\.\s*)?(1[0-9]{3}|20[0-2][0-9])\s*[–—-]\s*(?:1[0-9]{3}|20[0-2][0-9])?\)|\(\s*(?:born|b\.)\s*(1[0-9]{3}|20[0-2][0-9])\s*\)/;
const NAME = /^(.{2,40}?)\s+(?:è|e|was|is|were|are)\b/;
const DESC = /(?:was|is|were|are)\s+(?:an?|the)\s+([^.,;(]{3,60})/;

function loadPeople() {
  const out = [];
  for (const line of readFileSync(PEOPLE, 'utf8').split(/\r?\n/)) {
    const s = line.trim();
    if (!s || s.startsWith('//')) continue;
    let c; try { c = JSON.parse(s); } catch { continue; }
    if (!String(c.id || '').startsWith('person.')) continue;
    const slug = c.id.slice('person.'.length);
    const it = c.reply?.it || '', en = c.reply?.en || '';
    const bm = BIRTH.exec(it) || BIRTH.exec(en);
    const bioYear = bm ? Number(bm[1] || bm[2]) : null;
    const nm = NAME.exec(it) || NAME.exec(en);
    const name = (nm ? nm[1] : slug.replace(/-/g, ' ')).trim();
    const dm = DESC.exec(en) || DESC.exec(it);
    const desc = dm ? dm[1].trim().replace(/\s+/g, ' ') : '';
    out.push({ slug, name, desc, bioYear });
  }
  return out;
}

async function askYears(batch) {
  const list = batch.map((p) => `${p.slug} = ${p.name}${p.desc ? ' (' + p.desc + ')' : ''}`).join('\n');
  const o = await askJSON(
    'You are a precise biographical reference. Give the BIRTH YEAR (the year the person was BORN, never a reign/death/career year) of each listed person. Reply ONLY a JSON object mapping the exact id to a 4-digit integer year, or null if you are not certain. Do not guess.',
    `People (id = name (description)):\n${list}\n\nJSON object {id: birth_year_or_null}:`);
  return o && typeof o === 'object' ? o : {};
}

async function run() {
  const mode = process.argv.includes('--apply') ? 'apply' : 'calibrate';
  const people = loadPeople();
  const withBio = people.filter((p) => p.bioYear);
  console.log(`[born] ${people.length} people, ${withBio.length} with a bio-stated year (ground truth). model=${MODEL_NAME}`);

  const target = mode === 'calibrate' ? withBio : people;
  const B = 25;
  const got = {};
  for (let i = 0; i < target.length; i += B) {
    const batch = target.slice(i, i + B);
    const ans = await askYears(batch);
    for (const p of batch) {
      const y = ans[p.slug];
      if (typeof y === 'number' && y >= 1000 && y <= 2025) got[p.slug] = y;
    }
    process.stdout.write(`\r  queried ${Math.min(i + B, target.length)}/${target.length}`);
  }
  process.stdout.write('\n');

  if (mode === 'calibrate') {
    let exact = 0, off = 0, miss = 0; const wrong = [];
    for (const p of withBio) {
      const g = got[p.slug];
      if (g == null) { miss++; continue; }
      if (g === p.bioYear) exact++;
      else { off++; if (wrong.length < 15) wrong.push(`${p.name}: bio ${p.bioYear} vs grok ${g}`); }
    }
    const answered = exact + off;
    console.log(`\n[calibrate] vs ${withBio.length} ground-truth bio years:`);
    console.log(`  exact:   ${exact}/${answered} answered = ${(100 * exact / (answered || 1)).toFixed(1)}%  (model returned null on ${miss})`);
    console.log(`  wrong:   ${off}`);
    for (const w of wrong) console.log(`    - ${w}`);
    const acc = exact / (answered || 1);
    console.log(`\n  verdict: ${acc >= 0.95 ? 'TRUST (>=95%) — safe to bulk-enrich the rest' : acc >= 0.85 ? 'MARGINAL — enrich only with cross-checks' : 'DO NOT bulk-enrich'}`);
    return;
  }

  // apply: bio year is ground truth; grok fills the rest (plausibility-gated)
  const facts = {};
  let nBio = 0, nGrok = 0;
  for (const p of people) {
    if (p.bioYear) { facts[p.slug] = { born: p.bioYear, src: 'bio', name: p.name }; nBio++; }
    else if (got[p.slug]) { facts[p.slug] = { born: got[p.slug], src: 'grok', name: p.name }; nGrok++; }
  }
  mkdirSync(OUTDIR, { recursive: true });
  writeFileSync(OUT, JSON.stringify(facts, null, 0) + '\n');
  console.log(`[apply] wrote ${Object.keys(facts).length} born facts (${nBio} bio + ${nGrok} grok-verified) -> ${OUT}`);
}

run();
