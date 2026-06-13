// Score the live localIntent() (extracted from the spreadsheet HTML) against the
// generated corpus. Reports accuracy, per-intent recall, and the actual failures.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = path.dirname(fileURLToPath(import.meta.url));
const HTML = fs.readFileSync(path.join(DIR, '..', '..', 'apps', 'spreadsheet', 'www', 'index.html'), 'utf8');

function extractFn(name) {
  const start = HTML.indexOf('function ' + name + '(');
  if (start < 0) return '';
  let i = HTML.indexOf('{', start), depth = 0;
  for (let j = i; j < HTML.length; j++) { if (HTML[j] === '{') depth++; else if (HTML[j] === '}') { depth--; if (depth === 0) return HTML.slice(start, j + 1); } }
  return '';
}
// Build a sandbox with localIntent + any helpers it uses (extractCol if present).
const src = [extractFn('extractCol'), extractFn('localIntent')].filter(Boolean).join('\n');
const localIntent = new Function(src + '\nreturn localIntent;')();

// localIntent result -> corpus label
function toLabel(r) {
  if (!r) return 'remote';                              // null -> graceful AI fallback (knowledge/chitchat)
  switch (r.type) {
    case 'agg': return 'agg_' + ({ SUM: 'sum', AVERAGE: 'avg', MIN: 'min', MAX: 'max', COUNT: 'count', PRODUCT: 'product', MEDIAN: 'median' }[r.fn] || r.fn.toLowerCase());
    case 'transform': return 'transform_' + r.mode;
    case 'numfmt': return 'numfmt_' + r.kind;
    case 'format': return 'format_' + r.fmt;
    default: return r.type;
  }
}
// a prediction is correct if it equals the expected label; knowledge & chitchat are both
// "the parser should NOT claim a sheet command" -> correct when it returns remote(null).
function correct(expected, pred) {
  if (expected === 'knowledge' || expected === 'chitchat') return pred === 'remote';
  return pred === expected;
}

const corpus = fs.readFileSync(path.join(DIR, 'corpus.jsonl'), 'utf8').trim().split('\n').filter(Boolean).map(l => JSON.parse(l));
let ok = 0; const per = {}, fails = [];
for (const e of corpus) {
  const pred = toLabel(localIntent(e.text));
  const c = correct(e.intent, pred);
  per[e.intent] = per[e.intent] || { ok: 0, n: 0, confused: {} };
  per[e.intent].n++; if (c) { per[e.intent].ok++; ok++; } else { per[e.intent].confused[pred] = (per[e.intent].confused[pred] || 0) + 1; fails.push({ ...e, pred }); }
}
console.log(`\nACCURACY: ${ok}/${corpus.length} = ${(ok / corpus.length * 100).toFixed(1)}%\n`);
console.log('per-intent recall (sorted worst-first):');
for (const [k, v] of Object.entries(per).sort((a, b) => a[1].ok / a[1].n - b[1].ok / b[1].n)) {
  const conf = Object.entries(v.confused).sort((a, b) => b[1] - a[1]).slice(0, 3).map(([p, n]) => p + '×' + n).join(' ');
  console.log(`  ${(v.ok + '/' + v.n).padEnd(7)} ${(100 * v.ok / v.n).toFixed(0).padStart(3)}%  ${k.padEnd(16)} ${conf ? '→ ' + conf : ''}`);
}
// dump failures for inspection (limited per intent)
const show = process.argv[2] === 'fails';
if (show) {
  console.log('\n--- FAILURES (max 6 per intent) ---');
  const cnt = {};
  for (const f of fails) { cnt[f.intent] = (cnt[f.intent] || 0) + 1; if (cnt[f.intent] > 6) continue; console.log(`  [${f.intent} → ${f.pred}] (${f.lang}) ${f.text}`); }
}
fs.writeFileSync(path.join(DIR, 'fails.jsonl'), fails.map(f => JSON.stringify(f)).join('\n'));
console.log(`\n(${fails.length} failures written to fails.jsonl)`);
