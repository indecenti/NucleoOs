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
// WHERE IT LIVES: ON THE SD, NEXT TO THE CORPUS IT IS DERIVED FROM (2026-10-02). It used to be a
// compiled table (42 KB of flash: 984 people, 858 surnames, pointers + strings) when RAM was the only
// scarce resource; then the image outgrew M5Launcher's app slot. On the SD it costs no flash and
// still no heap: fixed-size records, binary-searched with fseek by the firmware
// (nucleo_anima/anima_person.c), one record on the stack at a time. It ships in the same verified SD
// payload as the knowledge index, so it cannot drift from the corpus either — and without an SD
// there are no person cards to clarify between anyway. It still carries no answer offsets: when the
// user picks an option the firmware re-queries L1 with the full name, so a clarify can never assert
// something the corpus does not contain.
//
// FORMAT (little endian), data/anima/anima-person-ambig.bin:
//   header 16 B   "APAMB1\0\0", u16 surnames, u16 people, u32 0
//   surnames      24 B each, sorted:  char surname[20] (NUL-padded), u16 first person, u8 count, u8 0
//   people        80 B each:          char given[32] (space-separated, folded ASCII), char display[48] (UTF-8)
// Placed in every SD tree that carries the ANIMA brain (deploy/sd, deploy/sd-safe, tools/sd-sim =
// the release payload source, and the host gate fixture tools/anima-host/sd).
//
// Regenerate with `npm run anima:person`; `npm run anima:person:check` gates freshness of every copy.

import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const KNOWLEDGE = join(ROOT, 'tools', 'anima', 'knowledge');
const REL = join('data', 'anima', 'anima-person-ambig.bin');
const TREES = ['deploy/sd', 'deploy/sd-safe', 'tools/sd-sim', 'tools/anima-host/sd'];
const OUTS = TREES.map((t) => join(ROOT, ...t.split('/'), REL));
const SURNAME_MAX = 20, GIVEN_MAX = 32, DISPLAY_MAX = 48;   // field sizes incl. the NUL (anima_person.h)

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

// The fixed-record image the firmware binary-searches (see FORMAT above). Every field is checked
// against its size: a name that does not fit fails the build instead of being silently truncated.
export function emit(table) {
  const people = [], groups = [];
  for (const g of table) {
    groups.push({ surname: g.surname, off: people.length, n: g.members.length });
    for (const m of g.members) people.push(m);
  }
  if (groups.length > 0xffff || people.length > 0xffff) throw new Error('person table too large for u16 indices');
  const buf = Buffer.alloc(16 + groups.length * 24 + people.length * 80);
  buf.write('APAMB1', 0, 'latin1');
  buf.writeUInt16LE(groups.length, 8); buf.writeUInt16LE(people.length, 10);
  const put = (s, off, size, what) => {
    const b = Buffer.from(s, 'utf8');                       // display names are UTF-8 ("Rúben Amorim"), as in replies
    if (/[\x00-\x1f\x7f]/.test(s)) throw new Error(`${what} has a control character: ${JSON.stringify(s)}`);
    if (b.length >= size) throw new Error(`${what} too long for ${size - 1} chars: ${JSON.stringify(s)}`);
    b.copy(buf, off);
  };
  let o = 16;
  for (const g of groups) {
    if (g.n > 255) throw new Error(`surname ${g.surname}: ${g.n} people > 255`);
    put(g.surname, o, SURNAME_MAX, 'surname'); buf.writeUInt16LE(g.off, o + 20); buf.writeUInt8(g.n, o + 22); o += 24;
  }
  for (const p of people) {
    put(p.given.join(' '), o, GIVEN_MAX, 'given names'); put(p.display, o + 32, DISPLAY_MAX, 'display name'); o += 80;
  }
  return buf;
}

// Run as a script (also on Windows: compare resolved paths, never a hand-built file:// URL — the old
// check never ran there and the freshness gate passed vacuously).
if (resolve(fileURLToPath(import.meta.url)) === resolve(process.argv[1] || '')) {
  const table = buildTable(loadCards());
  const img = emit(table);
  const check = process.argv.includes('--check');
  if (check) {
    const stale = OUTS.filter((p) => { try { return !readFileSync(p).equals(img); } catch { return true; } });
    if (!stale.length) { console.log(`OK  person-ambig fresh in ${OUTS.length} SD trees (${table.length} surnames, corpus ${corpusHash()})`); process.exit(0); }
    console.error('FAIL  anima-person-ambig.bin is missing or stale w.r.t. the corpus in:\n  ' + stale.join('\n  ') + '\n  -> run `npm run anima:person`');
    process.exit(1);
  }
  for (const p of OUTS) { mkdirSync(dirname(p), { recursive: true }); writeFileSync(p, img); }
  const total = table.reduce((s, g) => s + g.members.length, 0);
  const shared = table.filter((g) => g.members.length > 1).length;
  console.log(`OK  ${table.length} surnames (${shared} shared), ${total} people, ${img.length} B -> ${TREES.length} SD trees`);
  console.log(`    largest: ${table.slice().sort((a, b) => b.members.length - a.members.length).slice(0, 5)
    .map((g) => `${g.surname}(${g.members.length})`).join(' ')}`);
}
