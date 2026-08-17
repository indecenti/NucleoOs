#!/usr/bin/env node
// build_person_ambig.mjs — generate the AMBIGUOUS-SURNAME table that lets ANIMA ask
// "did you mean Donald Trump or Fred Trump?" instead of guessing.
//
// WHY THIS EXISTS. 1000 person cards share 85 surnames between them; "Trump" alone names ten
// different people. Asked a bare surname, retrieval picked whichever card scored highest and
// asserted it as fact: "chi è Trump" answered *Frederick Christ Trump Sr.* at 82% confidence,
// "chi è Kennedy" answered *Ethel* at 80%. Neither is a hallucination — both cards are real — and
// that is exactly what makes it worse than a miss: a coin flip that looks verified.
//
// The existing dialogic clarify band cannot cover this. It fires in [0.82, 0.85) on the top-2
// cosine candidates, and a bare surname lands at 0.55-0.72. Worse, cosine margin is the wrong
// signal entirely: "chi è Kennedy" separates its top two by 0.186 — it looks *confident* while
// being the wrong Kennedy. The right signal is not a similarity at all, it is an exact fact about
// the corpus: how many people carry this surname. Trump 10, Kennedy 4, Einstein 1. That question
// has an exact answer, so it is answered here, at build time, and never estimated at runtime.
//
// WHY A COMPILED TABLE AND NOT AN SD SIDECAR. The device's scarce resource is RAM (~18 KB heap),
// not flash. A static table lives in flash rodata: it costs zero heap, zero SD reads, and — the
// part that matters for maintenance — it carries no answer offsets, so it cannot fall out of sync
// with a rebuilt index. When the user picks an option, the firmware re-queries L1 with the full
// name, which scores 100%. The answer therefore always comes from a real card through the normal
// path: a clarify can never assert something the corpus does not contain.
//
// Regenerate with `npm run anima:person` (tools/anima/check_person_ambig.mjs gates freshness).

import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const KNOWLEDGE = join(ROOT, 'tools', 'anima', 'knowledge');
const OUT = join(ROOT, 'firmware', 'components', 'nucleo_anima', 'anima_person_ambig.h');

// A phrase is a NAME phrase when it carries no interrogative — the corpus lists the bare name
// among each card's `ask` variants ("Kurt Cobain"), so the shortest such phrase is the display name.
// Measured: this yields a clean name for 1000/1000 person cards.
const INTERROGATIVE = /\b(chi|cosa|come|quando|dove|perch|conosci|parlami|sai|famoso|fatto|sentito|who|what|when|where|why|tell|know|about|is|was|did|do|for)\b/i;
const SUFFIX = new Set(['jr', 'jr.', 'sr', 'sr.', 'i', 'ii', 'iii', 'iv', 'v']);

const fold = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z]/g, '');

export function loadCards(dir = KNOWLEDGE) {
  const people = [], others = [];
  for (const f of readdirSync(dir).filter((f) => f.endsWith('.jsonl')).sort()) {
    for (const line of readFileSync(join(dir, f), 'utf8').split('\n')) {
      if (!line.trim()) continue;
      let c; try { c = JSON.parse(line); } catch { continue; }
      (c.category === 'person' ? people : others).push(c);
    }
  }
  return { people, others };
}

export function displayName(card) {
  const phrases = [...(card.ask?.it || []), ...(card.ask?.en || [])];
  const named = phrases.filter((p) => !INTERROGATIVE.test(p)).sort((a, b) => a.length - b.length);
  return named[0] || null;
}

// Split a display name into the surname we index on and the given names that distinguish it.
// The corpus author already disambiguated some entries by hand — "Prince (musician)", "Rosé
// (singer)" — so the parenthetical is kept for DISPLAY and dropped for matching. A comma cuts a
// title off a regnal name ("Diana, Princess of Wales" -> "Diana"), and a trailing "family"
// ("Rothschild family") is a collective, not a given name.
export function splitName(display) {
  let base = display.replace(/\s*\([^)]*\)\s*/g, ' ').split(',')[0].trim();
  let toks = base.split(/\s+/).filter(Boolean);
  // A generational suffix is part of the identity, not decoration: "Donald Trump" and "Donald Trump
  // Jr." are two people whose ONLY distinguishing token is the "Jr.". It is lifted out of the
  // surname position (so both index under "trump") and kept among the given names (so a query that
  // says it, or pointedly does not, resolves to the right one).
  const suffix = [];
  while (toks.length > 1 && SUFFIX.has(toks[toks.length - 1].toLowerCase().replace(/[^a-z.]/g, ''))) suffix.unshift(toks.pop());
  if (toks.length > 1 && toks[toks.length - 1].toLowerCase() === 'family') toks.pop();
  const surname = fold(toks[toks.length - 1] || '');
  // Middle initials are kept: "B." is the whole of what separates Michael B. Jordan from Michael
  // Jordan, so dropping it would turn an answerable question into a needless one.
  const given = [...toks.slice(0, -1), ...suffix].map(fold).filter((t) => t.length >= 1);
  return { surname, given };
}

// A surname that is ALSO the subject of a non-person card is ambiguous across categories, not
// within the person set: "curry" (the dish) and "richardson" have their own cards, so a bare query
// belongs to the normal cascade, not to a "which person did you mean" question. Measured: this
// excludes exactly those two and leaves washington/king/martin/trump/kennedy/lee alone.
export function nonPersonSubjects(others) {
  const subj = new Set();
  for (const c of others) {
    for (const part of String(c.id).split('.').pop().split('-')) if (part.length >= 3) subj.add(fold(part));
    for (const p of [...(c.ask?.it || []), ...(c.ask?.en || [])]) {
      const t = p.trim().split(/\s+/);
      if (t.length === 1 && t[0].length >= 3) subj.add(fold(t[0]));
    }
  }
  return subj;
}

export function buildTable({ people, others }) {
  const subj = nonPersonSubjects(others);
  const bySurname = new Map();
  people.forEach((card, rank) => {
    const display = displayName(card);
    if (!display) return;
    const { surname, given } = splitName(display);
    if (surname.length < 3) return;                       // 2-letter surnames are noise, not names
    if (!bySurname.has(surname)) bySurname.set(surname, []);
    // `rank` is the card's position in the corpus, which the curator ordered by prominence — it is
    // why "chi è Kennedy" offers John F. Kennedy first instead of Ethel.
    bySurname.get(surname).push({ rank, display, given });
  });

  // Unique surnames are kept too, and carry the other half of the same idea. "who was einstein"
  // scores 0.682 against a corpus whose Einstein card is only ever phrased with the full name — under
  // the 0.72 rescue floor, so it refused, while Italian squeaked through at 0.722. That asymmetry is
  // not a threshold to tune: exactly one Einstein exists, so the surname RESOLVES. The firmware uses
  // these entries only after the cascade has already decided to refuse, which is why widening the
  // table cannot cost a correct answer — it can only turn "I don't know" into the card.
  const table = [];
  for (const [surname, members] of bySurname) {
    if (subj.has(surname)) continue;
    members.sort((a, b) => a.rank - b.rank);
    table.push({ surname, members });
  }
  table.sort((a, b) => (a.surname < b.surname ? -1 : a.surname > b.surname ? 1 : 0));  // binary-searchable
  return table;
}

export function corpusHash(dir = KNOWLEDGE) {
  const h = createHash('sha256');
  for (const f of readdirSync(dir).filter((f) => f.endsWith('.jsonl')).sort()) {
    h.update(f); h.update(readFileSync(join(dir, f)));
  }
  return h.digest('hex').slice(0, 16);
}

const cesc = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');

export function emit(table, hash) {
  const people = [];
  const groups = [];
  for (const g of table) {
    groups.push({ surname: g.surname, off: people.length, n: g.members.length });
    for (const m of g.members) people.push(m);
  }
  const L = [];
  L.push('// GENERATED by tools/anima/build_person_ambig.mjs — DO NOT EDIT BY HAND.');
  L.push('// Regenerate with `npm run anima:person`; `npm run anima:person:check` gates freshness.');
  L.push('//');
  L.push('// Surnames carried by two or more people in the knowledge corpus. Asked a bare surname,');
  L.push('// ANIMA offers the real cards by name instead of asserting whichever one scored highest.');
  L.push('// Sorted by surname so the lookup is a binary search; lives in flash, costs no heap.');
  L.push(`#define ANIMA_PERSON_AMBIG_CORPUS "${hash}"`);
  L.push('');
  L.push('typedef struct { const char *given; const char *display; } anima_person_t;');
  L.push('typedef struct { const char *surname; uint16_t off; uint8_t n; } anima_surname_t;');
  L.push('');
  L.push(`static const anima_person_t ANIMA_PERSON[] = {`);
  for (const p of people) L.push(`    { "${cesc(p.given.join(' '))}", "${cesc(p.display)}" },`);
  L.push('};');
  L.push('');
  L.push(`static const anima_surname_t ANIMA_SURNAME[] = {`);
  for (const g of groups) L.push(`    { "${cesc(g.surname)}", ${g.off}, ${g.n} },`);
  L.push('};');
  L.push(`#define ANIMA_SURNAME_N ${groups.length}`);
  L.push('');
  return L.join('\n');
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const cards = loadCards();
  const table = buildTable(cards);
  const hash = corpusHash();
  const text = emit(table, hash);
  const check = process.argv.includes('--check');
  let prev = null; try { prev = readFileSync(OUT, 'utf8'); } catch {}
  if (check) {
    if (prev === text) { console.log(`OK  person-ambig table fresh (${table.length} surnames, corpus ${hash})`); process.exit(0); }
    console.error('FAIL  anima_person_ambig.h is stale w.r.t. the corpus — run `npm run anima:person`');
    process.exit(1);
  }
  writeFileSync(OUT, text);
  const total = table.reduce((s, g) => s + g.members.length, 0);
  const shared = table.filter((g) => g.members.length > 1).length;
  console.log(`OK  ${table.length} surnames (${shared} shared), ${total} people -> ${OUT}`);
  console.log(`    largest: ${table.slice().sort((a, b) => b.members.length - a.members.length).slice(0, 5)
    .map((g) => `${g.surname}(${g.members.length})`).join(' ')}`);
}
