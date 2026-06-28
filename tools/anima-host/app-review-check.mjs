// app-review-check.mjs — DETERMINISTIC verification of the advisory cross-provider review core
// (apps/agent/www/app-review.js): the prompt builder and the TOLERANT verdict parser. The hard property
// is ADVISORY-SAFETY — a non-parsable / empty reply must degrade to {ok:true,issues:[]} so a reviewer
// hiccup can NEVER become a false block. The single cloud call lives in runtime.js (best-effort).
//   node tools/anima-host/app-review-check.mjs
import { buildReviewPrompt, parseReviewVerdict, reviewNote, REVIEW_SCHEMA } from '../../apps/agent/www/app-review.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond, detail) => { if (cond) { pass++; } else { fail++; fails.push(name + (detail ? ' — ' + detail : '')); } };

/* ---- parseReviewVerdict ---- */
const v1 = parseReviewVerdict('{"ok":false,"issues":["#go non esiste nel DOM","conv() non definita"]}');
ok('parses ok:false + issues', v1.ok === false && v1.issues.length === 2 && v1.issues[0].includes('#go'));
ok('extracts JSON wrapped in prose', parseReviewVerdict('Ecco l\'analisi: {"ok":false,"issues":["x"]} fine').issues[0] === 'x');
ok('non-JSON → safe {ok:true,[]}', (() => { const v = parseReviewVerdict('Non rispondo in JSON.'); return v.ok === true && v.issues.length === 0; })());
ok('empty/null → safe', parseReviewVerdict('').ok === true && parseReviewVerdict(null).issues.length === 0);
ok('ok:true valid', (() => { const v = parseReviewVerdict('{"ok":true,"issues":[]}'); return v.ok === true && v.issues.length === 0; })());
ok('missing ok + issues → ok:true, issues shown', (() => { const v = parseReviewVerdict('{"issues":["a"]}'); return v.ok === true && v.issues[0] === 'a'; })());
ok('normalises empty/null issues', (() => { const v = parseReviewVerdict('{"ok":false,"issues":["a","",null,"b"]}'); return v.issues.length === 2 && v.issues[1] === 'b'; })());
ok('caps issues at 8', parseReviewVerdict('{"ok":false,"issues":["1","2","3","4","5","6","7","8","9","10"]}').issues.length === 8);
ok('non-array issues → []', (() => { const v = parseReviewVerdict('{"ok":false,"issues":"oops"}'); return v.ok === false && v.issues.length === 0; })());

/* ---- buildReviewPrompt ---- */
const p = buildReviewPrompt({ id: 'todo-list', name: 'Todo' }, '<h1>x</h1>');
ok('returns non-empty system+user', typeof p.system === 'string' && p.system.length > 0 && typeof p.user === 'string' && p.user.length > 0);
ok('system carries the schema', p.system.includes(REVIEW_SCHEMA));
ok('system says SOLO blocking + anti-style', /SOLO bug BLOCCANTI/i.test(p.system) && /stile/i.test(p.system));
ok('user embeds the manifest', p.user.includes('todo-list'));
ok('user embeds the html', p.user.includes('<h1>x</h1>'));
const big = buildReviewPrompt({ id: 'xy' }, 'y'.repeat(20000));
ok('html truncated AT 12000 (tight)', big.user.includes('(troncato a 12000') && big.user.length < 13500);
ok('manifest accepted as string too', buildReviewPrompt(JSON.stringify({ id: 'as-string' }), '<b/>').user.includes('as-string'));
ok('parseReviewVerdict NEVER throws on weird input', (() => { for (const x of ['{', '}{', '{"issues":[{"o":1}]}', '[]', 'null', '{"ok":1}', '   ', '{not json']) parseReviewVerdict(x); return true; })());

/* ---- reviewNote ---- */
ok('reviewNote empty when no issues', reviewNote({ ok: true, issues: [] }, 'Grok') === '');
ok('reviewNote formats issues with provider', (() => { const n = reviewNote({ ok: false, issues: ['a', 'b'] }, 'Grok'); return n.includes('Grok') && n.includes('a; b') && n.startsWith('\n'); })());
ok('reviewNote safe on null', reviewNote(null, 'X') === '');

console.log(`\napp-review-check: ${pass} passed, ${fail} failed`);
if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
console.log('all green ✓');
