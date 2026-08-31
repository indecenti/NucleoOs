// extract.js — claim EXTRACTION with COVERAGE, the must-build-first core of cross-substrate
// verification. Turning free generative output into discrete checkable assertions is the genuinely
// hard part; a missed claim would yield a false PASS. So coverage is an explicit, gated property:
// every assertive span is counted, and any span we cannot route to a checker is reported as
// UNCOVERED (the verdict combiner downgrades PASS→WARN on any uncovered span — never silent PASS).
// For CODE we lean on mechanically-extractable literals (no NLP): numbers/strings the code PRINTS
// or RETURNS. For PROSE, a numeric sentence is checkable; a bare factual assertion is uncovered by
// default unless a known pattern (e.g. "capital of X") routes it to the device verifier.
// Pure & DOM-free → host-testable.

const NUM = /-?\d+(?:\.\d+)?/;

// Literals the code asserts by printing / returning — extractable without parsing semantics.
export function extractCodeClaims(code) {
  const claims = [];
  const s = String(code || '');
  const printRx = /(?:console\s*\.\s*(?:log|info|warn|error)|\bprint)\s*\(\s*([^)]*?)\s*\)/g;
  let m;
  while ((m = printRx.exec(s))) {
    const arg = m[1].trim();
    if (/^-?\d+(?:\.\d+)?$/.test(arg)) { claims.push({ kind: 'numeric', text: arg, value: Number(arg), checkable: true, source: 'print' }); continue; }
    const str = /^(['"`])([\s\S]*)\1$/.exec(arg);
    if (str) claims.push({ kind: 'string', text: str[2], checkable: true, source: 'print' });
  }
  const retRx = /\breturn\s+(-?\d+(?:\.\d+)?)\s*;?/g;
  while ((m = retRx.exec(s))) claims.push({ kind: 'numeric', text: m[1], value: Number(m[1]), checkable: true, source: 'return' });
  return claims;
}

// Assertive sentences in prose. Numeric sentences are checkable; bare factual assertions are
// uncovered unless a known device-checkable pattern is present.
export function extractProseClaims(prose) {
  const claims = [];
  const text = String(prose || '').replace(/```[\s\S]*?```/g, ' ');   // ignore fenced code
  const sentences = text.split(/(?<=[.!?])\s+|\n+/).map((x) => x.trim()).filter(Boolean);
  for (const sen of sentences) {
    const known = /\b(capital|capitale)\s+(?:of|di|della|del)\b/i.test(sen);
    if (NUM.test(sen)) { claims.push({ kind: 'numeric', text: sen, value: Number((sen.match(NUM) || [])[0]), checkable: true, source: 'prose' }); continue; }
    const assertive = /\b(is|are|was|were|è|sono|era|erano|equals?|uguale|means?|significa)\b/i.test(sen) && /\b\w{3,}\b/.test(sen);
    if (known) claims.push({ kind: 'fact', text: sen, checkable: true, source: 'prose-known' });
    else if (assertive) claims.push({ kind: 'assertion', text: sen, checkable: false, source: 'prose' });
  }
  return claims;
}

// Route ONE extracted prose claim to a device-verifier spec ({kind,key,asserted}), or null when
// the sentence carries nothing the deterministic brain can re-derive. This is the truth-lamp
// router, shared by the copilot badges and the yield harness (tools/anima-host/truthlamp-yield.mjs
// — measured 40% decisive on a realistic corpus, which is what earned the feature its UI).
//   numeric → only when a DERIVABLE expression is present ("2+2 è 4", "90/2 = 45"); a bare
//             measurement ("330 metres tall") has nothing to recompute → null.
//   fact    → the known key patterns (capital-of, it/en), mirroring the extractor's own gate.
export function routeClaim(c) {
  const s = String((c && c.text) || '');
  if (c && c.kind === 'numeric') {
    const m = /([-\d\s+*/().]+?)\s*,?\s*(?:=|fa|è(?:\s+uguale\s+a)?|equals|is(?:\s+equal\s+to)?|makes)\s*(-?\d+(?:\.\d+)?)/i.exec(s);
    if (m && /[+*/-]/.test(m[1]) && /\d/.test(m[1])) return { kind: 'numeric', key: m[1].trim(), asserted: m[2] };
    return null;
  }
  if (c && c.kind === 'fact') {
    let m = /\b(?:la\s+)?capitale\s+(della|dell'|del|di)\s*([\p{L} ']{2,30}?)\s+è\s+([\p{L}'-]{2,30})/iu.exec(s);
    if (m) return { kind: 'fact', key: ('capitale ' + m[1] + ' ' + m[2]).replace(/\s+/g, ' ').trim(), asserted: m[3].trim(), lang: 'it' };
    m = /\bthe\s+capital\s+(?:city\s+)?of\s+([\p{L} ']{2,30}?)\s+(?:is|was)\s+([\p{L}'-]{2,30})/iu.exec(s);
    if (m) return { kind: 'fact', key: 'capital of ' + m[1].trim(), asserted: m[2].trim(), lang: 'en' };
    return null;
  }
  return null;
}

// artifact: a string (code) or { code?, prose? }. Returns { claims, coverage }.
export function extract(artifact) {
  const code = typeof artifact === 'string' ? artifact : (artifact && artifact.code) || '';
  const prose = (artifact && typeof artifact === 'object') ? (artifact.prose || '') : '';
  const claims = [...extractCodeClaims(code), ...extractProseClaims(prose)];
  const found = claims.length;
  const checkable = claims.filter((c) => c.checkable).length;
  const uncovered = claims.filter((c) => !c.checkable).length;
  return { claims, coverage: { found, checkable, uncovered } };
}
