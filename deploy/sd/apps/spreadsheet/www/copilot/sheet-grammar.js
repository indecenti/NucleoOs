// sheet-grammar.js — compile the CLOSED SHEET_ACTION_SCHEMA into a GBNF grammar for
// grammar-constrained decoding (XGrammar / WebLLM). Single source of truth: the SAME schema drives
// the runtime firewall (parseSheetActions) AND the decode-time grammar, so the tiny in-browser
// orchestrator is constrained at GENERATION and re-validated at PARSE — defense in depth that lets a
// 0.5B–1B model be a trustworthy spreadsheet planner. `grammarAccepts` is the host-checkable
// contract: the grammar admits EXACTLY a pure JSON array of valid, in-grid actions (no prose, no
// unknown ops). Mirrors forge/grammar.js but with spreadsheet field types (num/col/cell). Pure &
// DOM-free → host-testable.

import { SHEET_ACTION_SCHEMA, parseSheetActions, schemaOps } from './sheet-actions.js';

const opRuleName = (op) => 'op_' + op;

function fieldGBNF(key, spec) {
  const optional = spec[0] === '?';
  const base = optional ? spec.slice(1) : spec;
  let valRule;
  if (base === 'num') valRule = 'number';
  else if (base === 'bool') valRule = 'boolean';
  else if (base === 'col') valRule = 'colstr';
  else if (base === 'cell') valRule = 'cellstr';
  else if (base.startsWith('enum:')) valRule = base.slice(5).split(',').map((v) => `"\\"${v}\\""`).join(' | ');
  else valRule = 'string';
  const kv = `"\\"${key}\\"" ws ":" ws (${valRule})`;
  return { kv, optional };
}

// Build a GBNF grammar string. Required fields are mandatory; optional fields may be omitted. The
// grammar restricts `col` to a quoted single A–Z letter and `cell` to a quoted A1 reference, so even
// at decode time the model cannot name a column/cell that doesn't exist syntactically.
export function toGBNF(schema = SHEET_ACTION_SCHEMA) {
  const ops = Object.keys(schema);
  const lines = [];
  lines.push('root ::= ws "[" ws (action (ws "," ws action)*)? ws "]" ws');
  lines.push('action ::= ' + ops.map(opRuleName).join(' | '));
  for (const op of ops) {
    const fields = Object.entries(schema[op]).map(([k, spec]) => fieldGBNF(k, spec));
    const head = `"{" ws "\\"op\\"" ws ":" ws "\\"${op}\\""`;
    const req = fields.filter((f) => !f.optional).map((f) => ' ws "," ws ' + f.kv).join('');
    const opt = fields.filter((f) => f.optional).map((f) => ` (ws "," ws ${f.kv})?`).join('');
    lines.push(`${opRuleName(op)} ::= ${head}${req}${opt} ws "}"`);
  }
  lines.push('string ::= "\\"" ([^"\\\\] | "\\\\" .)* "\\""');
  lines.push('colstr ::= "\\"" [A-Z] "\\""');
  lines.push('cellstr ::= "\\"" [A-Z] [0-9]+ "\\""');
  lines.push('number ::= "-"? [0-9]+ ("." [0-9]+)?');
  lines.push('boolean ::= "true" | "false"');
  lines.push('ws ::= [ \\t\\n\\r]*');
  return lines.join('\n');
}

// The grammar's CONTRACT, host-checkable WITHOUT an XGrammar engine: a grammar-constrained sample is
// pure JSON (no surrounding prose) AND every element is a valid, in-grid action. Returns
// { ok, actions?, reason? }. This is the assertion the host gate runs in lieu of a real GPU decode.
export function grammarAccepts(text, opts = {}) {
  const s = String(text || '').trim();
  let json;
  try { json = JSON.parse(s); } catch { return { ok: false, reason: 'not-pure-json' }; }   // prose ⇒ reject
  const list = Array.isArray(json) ? json : (json && Array.isArray(json.actions) ? json.actions : null);
  if (!list) return { ok: false, reason: 'not-action-array' };
  const { actions, rejected } = parseSheetActions(list, opts);
  if (rejected.length) return { ok: false, reason: rejected[0].reason };
  return { ok: true, actions };
}

// Convenience: the op names the grammar covers (must equal the schema — guards against drift).
export function grammarOps(schema = SHEET_ACTION_SCHEMA) { return Object.keys(schema); }
export { schemaOps };
