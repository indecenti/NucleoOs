// Host check for apps/anima/www/context.js (history compaction). Pure module.
// Run: node tools/anima-host/context-check.mjs
import { estimateBytes, summarizeTurns, compact, usage } from '../../apps/anima/www/context.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond) => { if (cond) pass++; else { fail++; fails.push(name); } };

// synthetic history: a long conversation with file ops, asks, facts, one pinned turn
const big = [];
big.push({ role: 'user', text: 'ciao' });
big.push({ role: 'bot', text: 'Buongiorno!' });
for (let i = 0; i < 30; i++) {
  big.push({ role: 'user', text: 'crea file' + i + '.txt con scritto contenuto numero ' + i + ' che è abbastanza lungo da pesare' });
  big.push({ role: 'bot', text: 'Creato file' + i + '.txt', fileop: { sum: 'write file' + i + '.txt' } });
}
big.push({ role: 'user', text: 'chi è Einstein', pinned: true });
big.push({ role: 'bot', text: 'Albert Einstein è stato un fisico tedesco, premio Nobel.', engine: true, meta: { domain: 'knowledge' }, pinned: true });
for (let i = 0; i < 6; i++) { big.push({ role: 'user', text: 'recent ' + i }); big.push({ role: 'bot', text: 'ok ' + i }); }

ok('estimateBytes>0', estimateBytes(big) > 1000);

// under budget -> untouched
const small = [{ role: 'user', text: 'hi' }, { role: 'bot', text: 'hello' }];
const r0 = compact(small, { budget: 18000 });
ok('under-budget no change', r0.compacted === 0 && r0.history === small);

// over budget -> compacts
const r1 = compact(big, { budget: 4000, lang: 'it' });
ok('compacted some', r1.compacted > 0);
ok('first is digest', r1.history[0] && r1.history[0].kind === 'digest');
ok('result smaller', estimateBytes(r1.history) < estimateBytes(big));
ok('digest mentions files', /File:/.test(r1.history[0].text) && /file\d/.test(r1.history[0].text));
ok('pinned survived', r1.history.some((h) => h.pinned && /Einstein/.test(h.text)));
ok('recent kept verbatim', r1.history.some((h) => h.text === 'recent 5') || r1.history.some((h) => h.text === 'ok 5'));
ok('digest covers count', r1.history[0].covers >= 1);

// summarizeTurns extracts categories
const sm = summarizeTurns([
  { role: 'user', text: 'crea note.txt' },
  { role: 'bot', text: 'x', fileop: { sum: 'write note.txt' } },
  { role: 'bot', text: 'Roma è la capitale d\'Italia.', engine: true, meta: { domain: 'knowledge' } },
], 'it');
ok('summary has File', /File:/.test(sm));
ok('summary has Chiesto', /Chiesto:/.test(sm));
ok('summary has Appreso', /Appreso:/.test(sm));

// idempotent-ish: compacting again merges into one leading digest (no second digest)
const r2 = compact(r1.history, { budget: 4000, lang: 'it', force: true });
ok('still single leading digest', r2.history.filter((h) => h.kind === 'digest').length === 1);

// usage ratio
ok('usage 0..1', usage(big, 18000) > 0 && usage(small, 18000) < 0.1);

console.log(`\ncontext-check: ${pass} passed, ${fail} failed (of ${pass + fail})`);
if (fails.length) { console.log('FAILURES:\n  ' + fails.join('\n  ')); process.exit(1); }
console.log('all green ✓');
