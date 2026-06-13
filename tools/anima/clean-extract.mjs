#!/usr/bin/env node
// Host-tested MIRROR of strip_pronun() in nucleo_anima_online.c — keep the two in lock-step. ANIMA relays
// the Wikipedia lead extract VERBATIM (0-hallucination), but the lead carries the IPA/AFI phonetic
// pronunciation ("(IPA: [ˈmɪçaːʔeːl ˈʃuːmaxɐ]; born 1969)") and reference markers ("[1]") that are
// unreadable clutter on the Cardputer. This strips them WITHOUT removing real prose. Run: node tools/anima/clean-extract.mjs
const LBL = ['ipa:', 'afi:', 'pronuncia', 'pronunciation', 'pronounced'];

export function stripPronun(input) {
  let s = input;
  // (1) labeled pronunciation: label -> next ';'/')'/',' (<=90 chars), but ONLY if the span carries a
  //     phonetic marker ('['/'/'), so a real verb ("was pronounced dead,") is left intact. ')' is kept.
  for (const lbl of LBL) {
    for (let i = 0; i < s.length; i++) {
      if (s.slice(i, i + lbl.length).toLowerCase() !== lbl) continue;
      let e = i + lbl.length, seen = 0, phon = false;
      while (e < s.length && ![';', ')', ','].includes(s[e]) && seen < 90) { if (s[e] === '[' || s[e] === '/') phon = true; e++; seen++; }
      if (!phon) continue;
      if (![';', ')', ','].includes(s[e])) continue;
      const cut = s[e] === ')' ? e : e + 1;
      s = s.slice(0, i) + s.slice(cut);
      i--;
    }
  }
  // (2) drop "[...]" spans (IPA / refs).
  s = s.replace(/\[[^\]]*\]/g, '');
  // (3) tidy spacing / empty parens.
  s = s.replace(/\(\s+/g, '(').replace(/\s+([),.;])/g, '$1').replace(/\(\)/g, '').replace(/ {2,}/g, ' ').trim();
  return s;
}

if (process.argv[1]?.endsWith('clean-extract.mjs')) {
  const CASES = [
    ['Michael Schumacher (IPA: [ˈmɪçaːʔeːl ˈʃuːmaxɐ]; born 3 January 1969) is a German former racing driver.',
     'Michael Schumacher (born 3 January 1969) is a German former racing driver.'],
    ['Michael Schumacher (AFI: [ˈmɪçaːʔeːl ˈʃuːmaxɐ]; Hürth-Hermülheim, 3 gennaio 1969) è un ex pilota automobilistico tedesco.',
     'Michael Schumacher (Hürth-Hermülheim, 3 gennaio 1969) è un ex pilota automobilistico tedesco.'],
    ['Gianni Versace (pronuncia [ˈdʒanni verˈsaːtʃe]; Reggio Calabria, 2 dicembre 1946 – Miami Beach, 15 luglio 1997) è stato uno stilista italiano.',
     'Gianni Versace (Reggio Calabria, 2 dicembre 1946 – Miami Beach, 15 luglio 1997) è stato uno stilista italiano.'],
    ['Tizio Caio (pronounced /dʒɒn/) is an inventor.', 'Tizio Caio is an inventor.'],
    ['Era uno stilista[1] italiano molto noto[2].', 'Era uno stilista italiano molto noto.'],
    // FALSE-POSITIVE guard: "pronounced" as a real verb (no phonetic marker) must be KEPT verbatim.
    ['He was pronounced dead, and the news shocked everyone.', 'He was pronounced dead, and the news shocked everyone.'],
    // no pronunciation at all -> unchanged
    ['Albert Einstein was a German-born theoretical physicist.', 'Albert Einstein was a German-born theoretical physicist.'],
  ];
  let pass = 0; const fails = [];
  for (const [inp, want] of CASES) { const got = stripPronun(inp); if (got === want) pass++; else fails.push({ inp, got, want }); }
  console.log(`[clean-extract] ${pass}/${CASES.length} pass`);
  for (const f of fails) { console.log('  ✗ in :', f.inp); console.log('    got:', f.got); console.log('    want:', f.want); }
  process.exit(fails.length ? 1 : 0);
}
