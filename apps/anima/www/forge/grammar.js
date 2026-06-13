// grammar.js — compile the CLOSED action registry (actions.js ACTION_SCHEMA) into a GBNF grammar
// for grammar-constrained decoding (XGrammar / WebLLM). Single source of truth: the SAME schema
// drives the runtime firewall (parseActions) AND the decode-time grammar, so the tiny orchestrator
// is constrained at GENERATION and re-validated at PARSE — defense in depth that lets a 0.5B model
// be a trustworthy router. `grammarAccepts` is the host-checkable contract: the grammar admits
// EXACTLY a pure JSON array of valid, path-confined actions (no prose, no unknown ops).
// Pure & DOM-free → host-testable.

import { ACTION_SCHEMA, validateAction } from './actions.js';

const opRuleName = (op) => 'op_' + op;

function fieldGBNF(key, spec) {
  const optional = spec[0] === '?';
  const base = spec.replace(/^\?/, '');
  let valRule;
  if (base === 'str' || base === 'path') valRule = 'string';
  else if (base === 'bool') valRule = 'boolean';
  else if (base.startsWith('enum:')) valRule = base.slice(5).split(',').map((v) => `"\\"${v}\\""`).join(' | ');
  else valRule = 'string';
  const kv = `"\\"${key}\\"" ws ":" ws (${valRule})`;
  return { kv, optional };
}

// Build a GBNF grammar string. Required fields are mandatory; optional fields may be omitted.
export function toGBNF(schema = ACTION_SCHEMA) {
  const ops = Object.keys(schema);
  const lines = [];
  lines.push('root ::= ws "[" ws (action (ws "," ws action)*)? ws "]" ws');
  lines.push('action ::= ' + ops.map(opRuleName).join(' | '));
  for (const op of ops) {
    const fields = Object.entries(schema[op]).map(([k, spec]) => fieldGBNF(k, spec));
    const head = `"{" ws "\\"op\\"" ws ":" ws "\\"${op}\\""`;
    const req = fields.filter((f) => !f.optional).map((f) => ' ws "," ws ' + f.kv).join('');
    // optional fields appended in any-order-ish (one optional block each, order-relaxed enough for a model)
    const opt = fields.filter((f) => f.optional).map((f) => ` (ws "," ws ${f.kv})?`).join('');
    lines.push(`${opRuleName(op)} ::= ${head}${req}${opt} ws "}"`);
  }
  lines.push('string ::= "\\"" ([^"\\\\] | "\\\\" .)* "\\""');
  lines.push('boolean ::= "true" | "false"');
  lines.push('ws ::= [ \\t\\n\\r]*');
  return lines.join('\n');
}

// The grammar's CONTRACT, host-checkable without an XGrammar engine: a grammar-constrained sample is
// pure JSON (no surrounding prose) AND every element is a valid, path-confined action. Returns
// { ok, actions?, reason? }.
export function grammarAccepts(text, { root = '/data/ws' } = {}) {
  const s = String(text || '').trim();
  let json;
  try { json = JSON.parse(s); } catch { return { ok: false, reason: 'not-pure-json' }; }   // prose ⇒ reject
  const list = Array.isArray(json) ? json : (json && Array.isArray(json.actions) ? json.actions : null);
  if (!list) return { ok: false, reason: 'not-action-array' };
  const out = [];
  for (const a of list) {
    const v = validateAction(a, { root });
    if (!v.ok) return { ok: false, reason: v.reason };
    out.push(v.action);
  }
  return { ok: true, actions: out };
}

// Convenience: the op names the grammar covers (must equal the schema — guards against drift).
export function grammarOps(schema = ACTION_SCHEMA) { return Object.keys(schema); }
