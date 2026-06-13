// sheet-plan.js — the DETERMINISTIC offline multi-step planner. The honest "works offline NOW, no
// model" core: it turns a COMPOUND natural-language request ("fai i totali, poi ordina per colonna B
// decrescente ed evidenzia il massimo") into an ordered PLAN of closed, firewalled sheet actions —
// with ZERO model and ZERO network. It reuses the already-proven single-intent parser (localIntent,
// INJECTED so this module stays pure & host-testable) clause by clause, maps each recognised intent
// to a typed action, and validates the whole plan through the sheet-actions firewall. A request it
// can't fully decompose degrades gracefully (the single-intent floor or escalation handles it). This
// is grounded-by-construction: every action comes from the deterministic parser, then the engine
// re-derives the result (sheet-verify) and a verified plan is learnable (sheet-learn). Pure & DOM-free.

import { parseSheetActions } from './sheet-actions.js';

// Strong sequence separators — splitting here is SAFE (these almost always mark distinct steps).
// Order matters: multi-word markers before single words. Case-insensitive, accent-tolerant upstream.
const SEQ = /\s*(?:;|\n|\be\s+poi\b|\bpoi\b|\be\s+quindi\b|\bquindi\b|\be\s+infine\b|\binfine\b|\bdopodiche\b|\bdopo\s+di\s+che\b|\band\s+then\b|\bafter\s+that\b|\bafter\s+which\b|\bthen\b|\bnext\b)\s*/i;
// Weak conjunction — only used when BOTH halves independently parse to an intent (avoids splitting
// "media e somma di A" wrongly, where the right half has no column of its own).
const CONJ = /\s+(?:e|ed|and)\s+/i;
const deaccent = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '');

function colLetter(idx) { return (idx == null || idx < 0) ? undefined : String.fromCharCode(65 + idx); }

// localIntent's {type,...} result → a typed sheet action (or null when it isn't a grid mutation we
// plan, e.g. help). The col index from localIntent becomes a letter; the firewall re-resolves it.
export function intentToAction(it) {
  if (!it) return null;
  switch (it.type) {
    case 'agg':       return { op: 'aggregate', fn: it.fn, col: colLetter(it.col) };
    case 'total':     return { op: 'total' };
    case 'describe':  return { op: 'describe' };
    case 'insights':  return { op: 'insights' };
    case 'chart':     return { op: 'chart', kind: it.chart === 'line' ? 'line' : 'bar', col: colLetter(it.col) };
    case 'sort':      return { op: 'sort', col: colLetter(it.col), order: it.asc ? 'asc' : 'desc' };
    case 'fill':      return { op: 'fill', seed: it.seed || '' };
    case 'clean':     return { op: 'clean' };
    case 'dedupe':    return { op: 'dedupe' };
    case 'rmempty':   return { op: 'rmempty' };
    case 'transform': return { op: 'transform', mode: it.mode };
    case 'format':    return { op: 'format', style: it.fmt };
    case 'numfmt':    return { op: 'numfmt', kind: it.kind };
    case 'find':      return { op: 'find', term: it.term || '' };
    case 'explain':   return { op: 'explain' };
    case 'enrich':    return { op: 'enrich', attr: it.attr || '', col: colLetter(it.col) };
    case 'formula':   return { op: 'formula', nl: it.nl || '' };
    case 'refresh':   return { op: 'refresh' };
    case 'highlight': return highlightAction(it.cond);
    case 'help':      return null;   // help is not a plannable mutation
    default:          return null;
  }
}

function highlightAction(cond) {
  if (!cond) return null;
  if (cond.ext === 'max') return { op: 'highlight', test: 'max' };
  if (cond.ext === 'min') return { op: 'highlight', test: 'min' };
  if (cond.dup) return { op: 'highlight', test: 'duplicates' };
  if (cond.empty) return { op: 'highlight', test: 'empty' };
  if (cond.op != null && cond.n != null) {
    const map = { '>': 'gt', '<': 'lt', '>=': 'ge', '<=': 'le', '=': 'eq' };
    const test = map[cond.op]; if (!test) return null;
    return { op: 'highlight', test, value: cond.n };
  }
  return null;
}

// Comma is a separator ONLY before a word ("pulisci i dati, rimuovi i duplicati") — never before a
// digit, so a number series ("riempi con 1, 2, 3") stays intact.
const COMMA = /,(?=\s*[a-z])/i;

// Split a phrase into ordered clauses: STRONG markers + comma-before-a-word, then for EACH clause try
// the weak "e/and" split and adopt it when ALL halves independently parse (catches "media di A e
// somma di B" while leaving "ordina per A e B" / "valori sopra 100 e sotto 50" whole). Returns the
// ordered clause list; intentToAction turns each into an action or leaves it uncovered.
function clauses(text, localIntent) {
  const first = deaccent(text).split(SEQ).flatMap((s) => s.split(COMMA)).map((s) => s.trim()).filter(Boolean);
  const out = [];
  for (const cl of first) {
    const halves = cl.split(CONJ).map((s) => s.trim()).filter(Boolean);
    if (halves.length > 1 && halves.every((h) => !!localIntent(h))) out.push(...halves);
    else out.push(cl);
  }
  return out;
}

// planFromNL(text, { localIntent, cols }) → { actions, covered, uncovered, clauses, dropped }.
//   actions   — firewall-validated, in-grid typed actions (executable as a plan)
//   covered   — clauses that produced an action
//   uncovered — clauses the parser could NOT type (knowledge / unrecognised) — NEVER fabricated
//   dropped   — actions the firewall rejected (should be ~0; a guard against mapper bugs)
// A plan is "compound" (worth running as a multi-step plan) iff actions.length >= 2.
export function planFromNL(text, { localIntent, cols = 26 } = {}) {
  if (typeof localIntent !== 'function') throw new Error('planFromNL requires an injected localIntent');
  const cls = clauses(text, localIntent);
  const raw = [], covered = [], uncovered = [];
  for (const cl of cls) {
    const a = intentToAction(localIntent(cl));
    if (a) { raw.push(a); covered.push(cl); } else uncovered.push(cl);
  }
  const { actions, rejected } = parseSheetActions(raw, { cols });
  return { actions, covered, uncovered, clauses: cls, dropped: rejected };
}

// Is this phrase a genuine COMPOUND the deterministic planner should own (≥2 plannable steps)?
export function isCompound(text, deps) {
  const p = planFromNL(text, deps);
  return p.actions.length >= 2;
}
