// sheet-verify.js — cross-substrate grounded verification for the spreadsheet copilot. A generative
// substrate (the browser orchestrator/coder, or cloud Grok) only ever PROPOSES; the deterministic
// formula engine RE-DERIVES every number, and this module fuses the signals into one honest verdict
// that rides on the message. Same VETO/WARN/PASS contract and honesty rule as forge/verify.js:
//   • a plan whose actions don't all parse the firewall   → VETO  (it can't run)
//   • any proposed value the engine recomputes DIFFERENTLY → VETO  (contradicted)
//   • any action we cannot route to a recompute check      → WARN  (uncovered — never silent PASS)
//   • else                                                 → PASS  (grounded: the engine agrees)
// "pass" means "the engine reproduces every claimed result" — NOT "this is what you wanted".
// Pure & DOM-free → host-testable.

import { isVerifiable } from './sheet-actions.js';

// Pure control ops carry no claim to verify (they steer the loop, they don't assert a result).
const CONTROL = new Set(['done', 'ask']);

// Build the coverage of a parsed plan: how many actions are engine-verifiable vs. uncovered. Control
// ops (done/ask) are neither — they are skipped. Everything else that ISN'T recompute-checkable
// (a free-text answer, a custom transform_code) is an honest UNCOVERED span → downgrades PASS→WARN.
export function planCoverage(actions = []) {
  let found = 0, checkable = 0, uncovered = 0;
  for (const a of actions) {
    if (CONTROL.has(a.op)) continue;
    found++;
    if (isVerifiable(a.op)) checkable++; else uncovered++;
  }
  return { found, checkable, uncovered };
}

// A single recompute check. `proposed` is what the model SAID a cell/formula equals; `recomputed` is
// what the engine returns for the same expression. Numbers compare with a small epsilon; an engine
// error (#…) on the model's formula is itself a contradiction.
export function checkRecompute({ expr, proposed, recomputed, eps = 1e-6 }) {
  const ev = { expr, proposed, recomputed };
  if (typeof recomputed === 'string' && recomputed[0] === '#') return { status: 'contradicted', evidence: { ...ev, why: 'engine-error' } };
  if (proposed == null || proposed === '') return { status: 'unknown', evidence: ev };   // nothing claimed → can't confirm
  const pn = Number(proposed), rn = Number(recomputed);
  if (Number.isFinite(pn) && Number.isFinite(rn)) {
    return Math.abs(pn - rn) <= eps ? { status: 'confirmed', evidence: ev } : { status: 'contradicted', evidence: ev };
  }
  // textual compare (case-insensitive, trimmed)
  const ps = String(proposed).trim().toLowerCase(), rs = String(recomputed).trim().toLowerCase();
  if (!rs) return { status: 'unknown', evidence: ev };
  return ps === rs ? { status: 'confirmed', evidence: ev } : { status: 'contradicted', evidence: ev };
}

// combineVerdict({ parsed, checks, coverage }) → { verdict, reasons, checks, coverage }.
//   parsed   — { ok:bool, rejected:[…] } from the firewall on the whole plan
//   checks   — array of {status:'confirmed'|'contradicted'|'unknown'} recompute results
//   coverage — planCoverage(actions)
// Precedence is fixed: any VETO source wins and cannot be downgraded (forge contract).
export function combineVerdict({ parsed = { ok: true, rejected: [] }, checks = [], coverage = { found: 0, checkable: 0, uncovered: 0 } } = {}) {
  const reasons = [];
  let verdict = 'pass';

  if (parsed && parsed.ok === false) { verdict = 'veto'; reasons.push('unparseable-plan:' + ((parsed.rejected && parsed.rejected.length) || '?')); }
  const contradicted = checks.filter((c) => c && c.status === 'contradicted');
  if (contradicted.length) { verdict = 'veto'; reasons.push('contradicted:' + contradicted.length); }

  if (verdict !== 'veto') {
    const unknown = checks.filter((c) => c && c.status === 'unknown').length;
    const uncovered = coverage.uncovered || 0;
    if (uncovered > 0 || unknown > 0) { verdict = 'warn'; reasons.push('unverified-spans:' + (uncovered + unknown)); }
  }
  return { verdict, reasons, checks, coverage };
}

// UI helper: the verdict chip (icon+text, NEVER colour-only — accessibility + honest provenance).
export function verdictChip(verdict) {
  switch (verdict && verdict.verdict) {
    case 'pass': return { icon: '✓', label: 'verificato', tone: 'ok' };
    case 'warn': return { icon: '⚠', label: 'da verificare', tone: 'warn' };
    case 'veto': return { icon: '⛔', label: 'respinto', tone: 'bad' };
    default: return { icon: '·', label: 'non controllato', tone: 'neutral' };
  }
}

// Provenance badge per substrate — a browser/cloud substrate must NEVER borrow the on-device look.
export function provenance(substrate) {
  switch (substrate) {
    case 'det':     return { kind: 'deterministic', grounded: true,  offline: true,  label: 'motore' };
    case 'plan-det':return { kind: 'deterministic', grounded: true,  offline: true,  label: 'piano' };
    case 'recipe':  return { kind: 'learned',       grounded: true,  offline: true,  label: 'appreso' };
    case 'M1':      return { kind: 'on-device',     grounded: true,  offline: true,  label: 'on-device' };
    case 'M2':      return { kind: 'hybrid',        grounded: true,  offline: false, label: 'grounded' };
    case 'M3':      return { kind: 'cloud',         grounded: false, offline: false, label: 'cloud' };
    case 'M3-plan': return { kind: 'cloud',         grounded: false, offline: false, label: 'cloud' };

    case 'M4-plan': return { kind: 'local-gpu',     grounded: false, offline: true,  label: 'GPU locale' };
    case 'M4-code': return { kind: 'local-gpu',     grounded: false, offline: true,  label: 'GPU locale' };
    default:        return { kind: 'unknown',       grounded: false, offline: false, label: '?' };
  }
}
