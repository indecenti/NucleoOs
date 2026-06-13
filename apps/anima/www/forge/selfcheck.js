// selfcheck.js — grounded best-of-N selection (test-time compute for code). The coder emits N
// candidates; the loop runs each through the hermetic check + (optionally) model-written assertions
// in the sandbox; this module RANKS them by GROUNDED signal, not by model self-report:
//   1. must pass the parse/dry-run check and not be vetoed
//   2. higher fraction of its own assertions passing (executed, not claimed)
//   3. fewer capabilities used (least privilege)
//   4. smaller (simplicity tie-breaker)
// Returns the winner, or null when no candidate is admissible (→ the loop takes a FIX turn).
// Pure & DOM-free → host-testable.

// candidate: { code, check:{ok}, verdict:'pass'|'warn'|'veto', caps:number, bytes:number, tests:{passed,total} }
function ratio(c) { const t = c.tests || {}; return t.total ? (t.passed || 0) / t.total : 0; }

export function admissible(c) {
  return !!(c && c.check && c.check.ok && c.verdict !== 'veto');
}

export function rank(candidates) {
  return (candidates || []).filter(admissible).slice().sort((a, b) => {
    const r = ratio(b) - ratio(a); if (r) return r;                       // more tests passing first
    const verdictRank = (x) => (x.verdict === 'pass' ? 0 : 1);
    const v = verdictRank(a) - verdictRank(b); if (v) return v;           // pass before warn
    const cap = (a.caps || 0) - (b.caps || 0); if (cap) return cap;       // least privilege
    return (a.bytes || 0) - (b.bytes || 0);                              // simplest
  });
}

export function pickBest(candidates) {
  const ordered = rank(candidates);
  return ordered.length ? ordered[0] : null;
}
