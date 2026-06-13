// sheet-actions.js — the CLOSED, typed-action vocabulary the ANIMA spreadsheet copilot may execute,
// plus the schema FIREWALL that turns any model output (incl. adversarial / garbled JSON) into a
// validated, IN-BOUNDS action list. This is the spreadsheet analogue of forge/actions.js: it is what
// makes a tiny grammar-constrained orchestrator LLM trustworthy as a planner — it physically cannot
// emit an unknown op, a malformed argument, an out-of-grid cell/column, or stray prose that reaches
// an executor. The vocabulary mirrors the existing copilot executors 1:1 (doAggregate, doHighlight,
// doSort, doEnrich, doFormula, …) so the runCopilot switch is reused verbatim — the LLM only ever
// produces a PLAN of these; the deterministic engine still does every computation. Pure & DOM-free
// → host-testable (tools/spread-copilot/*.test.mjs).

// field spec grammar (mirrors forge with spreadsheet types):
//   'str' | '?str'              required / optional string
//   'bool' | '?bool'           required / optional boolean
//   'num' | '?num'             required / optional finite number
//   'col' | '?col'             a single column, A..Z (resolved to 0-based index in validation)
//   'cell' | '?cell'           an A1-style cell reference (resolved to {r,c})
//   'enum:a,b,c'               one of the listed literals
export const SHEET_ACTION_SCHEMA = {
  // ---- aggregates & analysis (read-only or append-below) ----
  aggregate: { fn: 'enum:SUM,AVERAGE,MIN,MAX,COUNT,PRODUCT,MEDIAN,STDEV', col: '?col' },
  total:     {},                                                  // total row under the data block
  describe:  {},                                                  // statistics card for the region
  insights:  {},                                                  // anomalies / trends / correlation
  chart:     { kind: 'enum:bar,line', col: '?col' },
  // ---- conditional formatting ----
  highlight: { test: 'enum:gt,lt,ge,le,eq,ne,max,min,duplicates,empty,negative,positive', value: '?num', col: '?col' },
  // ---- structural / cleaning transforms ----
  sort:      { col: '?col', order: 'enum:asc,desc' },
  fill:      { seed: '?str' },                                    // series seed e.g. "1,2,3"
  clean:     {},                                                  // trim / normalise whitespace
  dedupe:    {},                                                  // remove duplicate rows
  rmempty:   {},                                                  // remove empty rows
  transform: { mode: 'enum:upper,lower,proper' },                 // text case
  format:    { style: 'enum:bold,italic' },
  numfmt:    { kind: 'enum:currency,percent,comma' },
  // ---- formulas & knowledge ----
  formula:   { nl: 'str', target: '?cell' },                     // NL→formula; ALWAYS recomputed locally
  setcell:   { target: 'cell', value: 'str' },                   // write a literal/formula to one cell
  enrich:    { attr: 'str', col: '?col' },                       // =ANIMA() knowledge column (offline)
  refresh:   {},                                                  // recompute =ANIMA() cells
  find:      { term: 'str' },
  explain:   {},                                                  // explain the current cell's formula
  // ---- escape hatch to the coder substrate (sandboxed, never auto-applied) ----
  transform_code: { spec: 'str', target_col: '?col' },           // NL→JS transform run in nucleo-run.js
  // ---- conversational / control ----
  answer:    { text: 'str' },                                    // textual reply (a FACT is labelled unverified)
  ask:       { question: 'str' },                               // clarify (human-in-the-loop)
  done:      { summary: '?str' },                               // stop the plan
};

// Ops that MUTATE the grid (every one routes through the staged-diff path; the copilot's commitDiffs
// is already undo-aware, so "approval" for low-risk ops is implicit + reversible, while the coder
// path (transform_code) and bulk plans get an explicit preview). Mirrors forge isMutating.
const MUTATING = new Set(['total', 'highlight', 'sort', 'fill', 'clean', 'dedupe', 'rmempty', 'transform', 'format', 'numfmt', 'formula', 'setcell', 'enrich']);
export function isMutating(op) { return MUTATING.has(op); }

// Ops the deterministic engine can fully VERIFY by recomputation (a number/formula it re-derives).
// Used by sheet-verify to decide coverage: a plan made only of these is fully grounded.
const VERIFIABLE = new Set(['aggregate', 'total', 'formula', 'setcell', 'describe', 'insights', 'chart', 'highlight', 'sort', 'dedupe', 'rmempty', 'clean', 'transform', 'fill', 'find', 'numfmt', 'format', 'enrich', 'refresh', 'explain']);
export function isVerifiable(op) { return VERIFIABLE.has(op); }

const COL_RE = /^[A-Za-z]$/;
const CELL_RE = /^\$?([A-Za-z])\$?(\d+)$/;

// A..Z → 0-based index, bounded by `cols`. Returns -1 when invalid / out of grid.
export function colIndex(letter, cols = 26) {
  if (typeof letter !== 'string' || !COL_RE.test(letter)) return -1;
  const i = letter.toUpperCase().charCodeAt(0) - 65;
  return (i >= 0 && i < cols) ? i : -1;
}

// "B5" → {r:4, c:1} (0-based), bounded by rows/cols. Returns null when invalid / out of grid.
export function parseCell(ref, rows = 10000, cols = 26) {
  if (typeof ref !== 'string') return null;
  const m = CELL_RE.exec(ref.trim());
  if (!m) return null;
  const c = m[1].toUpperCase().charCodeAt(0) - 65;
  const r = parseInt(m[2], 10) - 1;
  if (c < 0 || c >= cols || r < 0 || r >= rows) return null;
  return { r, c };
}

function coerce(spec, v, dims) {
  const opt = spec[0] === '?';
  const base = opt ? spec.slice(1) : spec;
  if (v == null || v === '') {
    if (opt) return base === 'col' ? null : (base === 'cell' ? null : (base === 'bool' ? false : (base === 'num' ? null : '')));
    return undefined; // required, missing
  }
  if (base === 'str') return typeof v === 'string' && v.length ? v : (opt ? '' : undefined);
  if (base === 'bool') return typeof v === 'boolean' ? v : (opt ? false : undefined);
  if (base === 'num') { const n = typeof v === 'number' ? v : Number(v); return Number.isFinite(n) ? n : undefined; }
  if (base === 'col') { const i = colIndex(typeof v === 'number' ? String.fromCharCode(65 + v) : v, dims.cols); return i >= 0 ? i : undefined; }
  if (base === 'cell') { const p = parseCell(v, dims.rows, dims.cols); return p || undefined; }
  if (base.startsWith('enum:')) { const opts = base.slice(5).split(','); return opts.includes(v) ? v : undefined; }
  return undefined;
}

// Validate ONE action object → { ok, action } | { ok:false, reason }. `action` carries resolved
// fields: col/target are turned into indices/{r,c} so the executor needs no re-parsing, and there is
// no path for an out-of-grid coordinate to survive.
export function validateAction(a, opts = {}) {
  const dims = { rows: opts.rows || 10000, cols: opts.cols || 26 };
  if (!a || typeof a !== 'object' || Array.isArray(a)) return { ok: false, reason: 'not-object' };
  const op = a.op || a.action || a.tool;
  if (typeof op !== 'string' || !SHEET_ACTION_SCHEMA[op]) return { ok: false, reason: 'unknown-op:' + op };
  const schema = SHEET_ACTION_SCHEMA[op];
  const out = { op };
  for (const [k, spec] of Object.entries(schema)) {
    const val = coerce(spec, a[k], dims);
    if (val === undefined) return { ok: false, reason: 'bad-field:' + op + '.' + k };
    // keep optional empties off the object unless they carry meaning (false/0/null are dropped)
    if (val !== '' && val !== null && val !== false) out[k] = val;
    else if (spec[0] !== '?') out[k] = val;
  }
  return { ok: true, action: out };
}

// Pull a JSON value out of possibly fenced / prose-wrapped model text. Returns null if none.
// Identical strategy to forge/actions.extractJson (fence → whole → first bracket span).
export function extractJson(text) {
  if (text && typeof text === 'object') return text;
  const s = String(text || '');
  const fence = /```(?:json)?\s*([\s\S]*?)```/i.exec(s);
  const cands = [];
  if (fence) cands.push(fence[1]);
  cands.push(s);
  const span = s.match(/[\[{][\s\S]*[\]}]/);
  if (span) cands.push(span[0]);
  for (const c of cands) { try { return JSON.parse(c); } catch { /* next */ } }
  return null;
}

// THE FIREWALL: any model output → { actions:[validated], rejected:[{raw,reason}] }.
// Guarantee: every action in `actions` is in the closed set, well-typed, and references only
// in-grid coordinates. Garbage / prose / unknown ops are quarantined in `rejected`, never executed.
export function parseSheetActions(modelOutput, opts = {}) {
  const json = extractJson(modelOutput);
  let list = [];
  if (Array.isArray(json)) list = json;
  else if (json && Array.isArray(json.actions)) list = json.actions;
  else if (json && Array.isArray(json.plan)) list = json.plan;
  else if (json && (json.op || json.action || json.tool)) list = [json];
  const actions = [], rejected = [];
  for (const a of list) {
    const v = validateAction(a, opts);
    if (v.ok) actions.push(v.action); else rejected.push({ raw: a, reason: v.reason });
  }
  return { actions, rejected };
}

// The op names the schema covers (guards the grammar against drift; used by the gate).
export function schemaOps() { return Object.keys(SHEET_ACTION_SCHEMA); }
