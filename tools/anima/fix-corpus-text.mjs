#!/usr/bin/env node
// CORPUS TEXT LINT — two defects that reach the screen verbatim, because ANIMA relays corpus fields
// without rewriting them (that is the 0-hallucination contract: the text you see IS the card).
//
//  1. DOUBLE-ENCODED UTF-8 — "Ã¨" where the card means "è". A source that was already UTF-8 got
//     decoded as Latin-1 and re-encoded, so every accented character became two. Nothing downstream
//     can tell it apart from real text: it is valid Latin, it survives every filter, and it shipped
//     ("una percentuale Ã¨ un numero..."). The repair is exact: C3 83 + C2 yy -> C3 yy, C3 82 + C2 yy
//     -> C2 yy, applied repeatedly for a doubly-mangled source.
//  2. WIKIPEDIA MATH-RENDER RESIDUE — "{\displaystyle {\frac {x}{100}}}" left by the extractor. It
//     reads as noise, and its braces trip the "leaked {template}" guard the hallucination probes use.
//
// The firmware ALSO repairs both at output time (a_strip_foreign in anima_text.c), so an index built
// before this lint still displays clean. This fixes them at the source, so a rebuilt index is clean
// and the repair stays a safety net rather than the only thing standing between the card and the user.
//
//   node tools/anima/fix-corpus-text.mjs --check   # report only, exit 1 if anything is contaminated
//   node tools/anima/fix-corpus-text.mjs           # repair in place, report what changed
import { readFileSync, writeFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
// The corpora that feed an index build. `.knowledge.bak` is deliberately NOT here: it is a backup of
// a previous state, and silently rewriting a backup destroys the thing it exists for.
const ROOTS = [join(here, 'knowledge'), join(here, 'knowledge.staged')];
const check = process.argv.includes('--check');

// C3 83 ("Ã") / C3 82 ("Â") followed by a continuation byte = one character that was encoded twice.
const MOJIBAKE = /[ÂÃ][-¿]/;
function demojibake(s) {
  let out = s;
  for (let pass = 0; pass < 3; pass++) {
    if (!MOJIBAKE.test(out)) break;
    const bytes = [];
    for (const ch of out) {
      const c = ch.codePointAt(0);
      if (c <= 0xff) bytes.push(c);                       // a char that came from a mis-decoded byte
      else { bytes.push(...Buffer.from(ch, 'utf8')); }     // real text: keep its own bytes
    }
    const decoded = Buffer.from(bytes).toString('utf8');
    if (decoded === out || decoded.includes('�')) break;   // no progress, or we'd corrupt it
    out = decoded;
  }
  return out;
}

// Drop "{\displaystyle ...}" and any other TeX block through its MATCHING brace. Returns null when
// there was nothing to remove — the cosmetic tidy below must NOT run on untouched fields, or every
// trailing space in the corpus counts as a repair and the report becomes a lie (it briefly did:
// 1084 "TeX blocks" in people.jsonl were trailing spaces on ordinary one-line bios).
function stripTex(s) {
  if (!s.includes('{\\')) return null;
  let out = '', i = 0;
  while (i < s.length) {
    if (s[i] === '{' && s[i + 1] === '\\') {
      let depth = 0, j = i;
      for (; j < s.length; j++) {
        if (s[j] === '{') depth++;
        else if (s[j] === '}' && --depth === 0) { j++; break; }
      }
      i = j;                                              // skip the block (or the unterminated tail)
      continue;
    }
    out += s[i++];
  }
  // A formula cut mid-sentence strands its spacing and full stop: "in centesimi x 100 . Spesso" -> "100. Spesso".
  // Only a SENTENCE-ending dot: a dot followed by a letter is a file extension, and closing the gap
  // there corrupts real text ("sta nel .c" -> "sta nel.c"). Mirrors a_strip_foreign step 3.
  return out.replace(/[ \t]{2,}/g, ' ').replace(/ +([,;:)])/g, '$1').replace(/ +\.(?=\s|$)/g, '.').trim();
}

function walk(dir) {
  const out = [];
  let entries;
  try { entries = readdirSync(dir); } catch { return out; }
  for (const name of entries) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walk(p));
    else if (name.endsWith('.jsonl')) out.push(p);
  }
  return out;
}

let filesTouched = 0, mojiFixed = 0, texFixed = 0;
const report = [];
for (const root of ROOTS) {
  for (const file of walk(root)) {
    const raw = readFileSync(file, 'utf8');
    const lines = raw.split('\n');
    let moji = 0, tex = 0, changed = false;
    const out = lines.map((line) => {
      if (!line.trim() || line.startsWith('//')) return line;
      let obj;
      try { obj = JSON.parse(line); } catch { return line; }   // not our business to fix broken JSON
      let hit = false;
      const fix = (v) => {
        if (typeof v === 'string') {
          let s = v;
          const a = demojibake(s); if (a !== s)   { moji++; hit = true; s = a; }
          const b = stripTex(s);   if (b !== null) { tex++;  hit = true; s = b; }
          return s;
        }
        if (Array.isArray(v)) return v.map(fix);
        if (v && typeof v === 'object') {
          const o = {};
          for (const [k, val] of Object.entries(v)) o[k] = fix(val);
          return o;
        }
        return v;
      };
      const fixed = fix(obj);
      if (!hit) return line;
      changed = true;
      return JSON.stringify(fixed);
    });
    if (moji || tex) {
      report.push(`  ${relative(repo, file)}: ${moji} double-encoded, ${tex} TeX block(s)`);
      mojiFixed += moji; texFixed += tex; filesTouched++;
      if (!check && changed) writeFileSync(file, out.join('\n'));
    }
  }
}

if (!filesTouched) { console.log('[fix-corpus-text] corpus clean — no double-encoding, no TeX residue.'); process.exit(0); }
console.log(`[fix-corpus-text] ${filesTouched} file(s): ${mojiFixed} double-encoded field(s), ${texFixed} TeX block(s)`);
for (const r of report) console.log(r);
if (check) {
  console.log('✗ corpus contaminated — run: node tools/anima/fix-corpus-text.mjs');
  process.exit(1);
}
console.log('✓ repaired in place. Rebuild the index (npm run anima:packs) for it to reach the device.');
