// verify.js — the verdict combiner for cross-substrate grounded verification. It fuses three
// signals into ONE honest verdict that rides in the envelope:
//   1. a hermetic client dry-run (did the generated code even run with I/O denied?)
//   2. device M1 claim-checks (anima_solve re-derives numbers; L1/KGE checks grounded facts)
//   3. extraction COVERAGE (how many assertive spans we could NOT route to a checker)
// Honesty contract (must-fix): absence of evidence yields WARN, never silent PASS.
//   - code that fails the hermetic dry-run            → VETO
//   - any checked claim CONTRADICTED                  → VETO
//   - any UNCOVERED assertive span OR UNKNOWN check   → WARN
//   - else                                            → PASS
// "pass" means "grounded claims hold AND it ran hermetically" — NOT "this code is correct".
// Pure & DOM-free → host-testable.

export function combineVerdict({ run = null, checks = [], coverage = { found: 0, checkable: 0, uncovered: 0 }, capguard = null } = {}) {
  const reasons = [];
  let verdict = 'pass';

  if (run && run.ok === false) { verdict = 'veto'; reasons.push('dry-run-failed:' + (run.error || (run.timeout && 'timeout') || 'error')); }

  const contradicted = checks.filter((c) => c && c.status === 'contradicted');
  if (contradicted.length) { verdict = 'veto'; reasons.push('contradicted:' + contradicted.length); }

  // Least-privilege / dangerous-pattern guard over the generated code (capguard.assess).
  if (capguard && capguard.severity === 'block') { verdict = 'veto'; reasons.push('capability-block:' + (capguard.dangers || []).map((d) => d.kind).join(',')); }

  if (verdict !== 'veto') {
    const unknown = checks.filter((c) => c && c.status === 'unknown').length;
    const uncovered = coverage.uncovered || 0;
    const capWarn = capguard && capguard.severity === 'warn';
    if (uncovered > 0 || unknown > 0 || capWarn) {
      verdict = 'warn';
      if (uncovered > 0 || unknown > 0) reasons.push('unverified-spans:' + (uncovered + unknown));
      if (capWarn) reasons.push('capability-warn:' + ((capguard.over || []).join(',') || (capguard.dangers || []).map((d) => d.kind).join(',')));
    }
  }
  return { verdict, reasons, checks, coverage, capguard };
}

// UI helper: the verdict chip (icon+text, NEVER colour-only — accessibility + honest provenance).
export function verdictChip(verdict) {
  switch (verdict && verdict.verdict) {
    case 'pass': return { icon: '✓', label: 'grounded', tone: 'ok' };
    case 'warn': return { icon: '⚠', label: 'unverified', tone: 'warn' };
    case 'veto': return { icon: '⛔', label: 'vetoed', tone: 'bad' };
    default: return { icon: '·', label: 'unchecked', tone: 'neutral' };
  }
}
