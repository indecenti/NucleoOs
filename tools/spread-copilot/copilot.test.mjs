// copilot.test.mjs — host gate for the ANIMA Spreadsheet Copilot 3.0 module suite (four substrates).
// Exercises the closed-action firewall, the schema→GBNF grammar contract, the offline-first router,
// the recompute verdict combiner, the offline recipe flywheel, and the orchestrator end-to-end with
// a MockEngine + a mock sheet (no GPU, no network, no browser). The coder path's generated wrapper is
// validated with the REAL nucleo-run checkSyntax. Run: node tools/spread-copilot/copilot.test.mjs
import assert from 'node:assert';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const C = (f) => path.resolve(here, '../../apps/spreadsheet/www/copilot/', f);

const { SHEET_ACTION_SCHEMA, validateAction, parseSheetActions, colIndex, parseCell, schemaOps, isMutating, isVerifiable } = await import('file://' + C('sheet-actions.js'));
const { toGBNF, grammarAccepts, grammarOps } = await import('file://' + C('sheet-grammar.js'));
const { route, looksLikeCodeTransform, modelPlan, isGenerative, isOffline } = await import('file://' + C('sheet-router.js'));
const { planCoverage, checkRecompute, combineVerdict, verdictChip, provenance } = await import('file://' + C('sheet-verify.js'));
const { learn, recall, forget, normQuery, trigramJaccard, memStore } = await import('file://' + C('sheet-learn.js'));
const { MockEngine } = await import('file://' + C('sheet-engine.js'));
const { handle, orderForExec } = await import('file://' + C('sheet-copilot.js'));
const { checkSyntax } = await import('file://' + path.resolve(here, '../../apps/code-runner/www/nucleo-run.js'));

let pass = 0, fail = 0;
const t = (name, fn) => { try { fn(); pass++; } catch (e) { fail++; console.error('✗ ' + name + '\n   ' + (e && e.message || e)); } };
const ta = async (name, fn) => { try { await fn(); pass++; } catch (e) { fail++; console.error('✗ ' + name + '\n   ' + (e && e.message || e)); } };

// ───────────────────────── 1. FIREWALL (sheet-actions) ─────────────────────────
t('schema covers the executor verbs', () => {
  for (const op of ['aggregate', 'total', 'highlight', 'sort', 'formula', 'enrich', 'transform', 'dedupe', 'rmempty', 'chart', 'transform_code', 'answer', 'done'])
    assert.ok(SHEET_ACTION_SCHEMA[op], 'missing op ' + op);
});
t('valid aggregate resolves col to index', () => {
  const v = validateAction({ op: 'aggregate', fn: 'SUM', col: 'C' }, { cols: 26 });
  assert.ok(v.ok); assert.strictEqual(v.action.col, 2);
});
t('unknown op rejected', () => assert.strictEqual(validateAction({ op: 'nuke' }).ok, false));
t('bad enum rejected', () => assert.strictEqual(validateAction({ op: 'aggregate', fn: 'FOO' }).ok, false));
t('out-of-grid column rejected', () => assert.strictEqual(validateAction({ op: 'sort', col: 'Z', order: 'asc' }, { cols: 5 }).ok, false));
t('out-of-grid cell rejected', () => assert.strictEqual(validateAction({ op: 'setcell', target: 'A99999', value: '1' }, { rows: 100 }).ok, false));
t('missing required field rejected', () => assert.strictEqual(validateAction({ op: 'formula' }).ok, false));
t('colIndex / parseCell bounds', () => {
  assert.strictEqual(colIndex('A'), 0); assert.strictEqual(colIndex('1'), -1);
  assert.deepStrictEqual(parseCell('B3'), { r: 2, c: 1 }); assert.strictEqual(parseCell('A0'), null);
});
t('firewall quarantines junk, keeps valid', () => {
  const { actions, rejected } = parseSheetActions('[{"op":"total"},{"op":"evil"},{"op":"sort","col":"A","order":"desc"}]');
  assert.strictEqual(actions.length, 2); assert.strictEqual(rejected.length, 1);
});
t('firewall extracts from prose-wrapped json', () => {
  const { actions } = parseSheetActions('Here is the plan:\n```json\n[{"op":"describe"}]\n```\nDone.');
  assert.strictEqual(actions.length, 1); assert.strictEqual(actions[0].op, 'describe');
});
t('isMutating / isVerifiable', () => { assert.ok(isMutating('sort')); assert.ok(!isMutating('describe')); assert.ok(isVerifiable('aggregate')); assert.ok(!isVerifiable('transform_code')); });

// ───────────────────────── 2. GRAMMAR (schema == firewall) ─────────────────────────
t('grammarOps == schemaOps (no drift)', () => assert.deepStrictEqual(grammarOps().sort(), schemaOps().sort()));
t('GBNF mentions every op rule', () => { const g = toGBNF(); for (const op of schemaOps()) assert.ok(g.includes('op_' + op), 'no rule for ' + op); });
t('grammarAccepts a pure valid plan', () => { const r = grammarAccepts('[{"op":"aggregate","fn":"SUM","col":"A"},{"op":"done"}]'); assert.ok(r.ok); assert.strictEqual(r.actions.length, 2); });
t('grammarAccepts rejects prose', () => assert.strictEqual(grammarAccepts('sure! [{"op":"done"}]').ok, false));
t('grammarAccepts rejects unknown op', () => assert.strictEqual(grammarAccepts('[{"op":"rm -rf"}]').ok, false));

// ───────────────────────── 3. ROUTER (offline-first ladder) ─────────────────────────
t('auto + localHit → deterministic floor', () => assert.strictEqual(route({ text: 'somma colonna A', dial: 'auto', localHit: true }).substrate, 'det'));
t('auto + recipeHit → recipe replay', () => assert.strictEqual(route({ text: 'x', dial: 'auto', localHit: false, recipeHit: true }).substrate, 'recipe'));
t('auto novel + GPU orch → M4-plan', () => assert.strictEqual(route({ text: 'do a thing', dial: 'auto', caps: { webgpu: true, orchReady: true } }).substrate, 'M4-plan'));
t('auto custom transform + coder → M4-code', () => assert.strictEqual(route({ text: 'group by region and rank', dial: 'auto', caps: { webgpu: true, coderReady: true } }).substrate, 'M4-code'));
t('auto novel + only cloud → M3', () => assert.strictEqual(route({ text: 'do a thing', dial: 'auto', caps: { online: true } }).substrate, 'M3'));
t('auto novel + nothing → M1 offline', () => assert.strictEqual(route({ text: 'do a thing', dial: 'auto', caps: {} }).substrate, 'M1'));
t('dial off never reaches the net', () => { const r = route({ text: 'do a thing', dial: 'off', caps: { online: true } }); assert.ok(isOffline(r.substrate)); });
t('dial only → cloud', () => assert.strictEqual(route({ text: 'x', dial: 'only', caps: { online: true } }).substrate, 'M3'));
t('looksLikeCodeTransform', () => { assert.ok(looksLikeCodeTransform('compute year-over-year growth')); assert.ok(!looksLikeCodeTransform('sum column A')); });
t('modelPlan VRAM tiers', () => { assert.strictEqual(modelPlan({ webgpu: true, vramMB: 8000 }).mode, 'two-model'); assert.strictEqual(modelPlan({ webgpu: true, vramMB: 2000 }).mode, 'single-model'); assert.strictEqual(modelPlan({}).mode, 'none'); });
t('isGenerative', () => { assert.ok(isGenerative('M4-plan')); assert.ok(isGenerative('M3')); assert.ok(!isGenerative('det')); assert.ok(!isGenerative('recipe')); });

// ───────────────────────── 4. VERIFY (recompute → verdict) ─────────────────────────
t('planCoverage counts uncovered', () => { const c = planCoverage([{ op: 'aggregate' }, { op: 'transform_code' }, { op: 'answer' }]); assert.strictEqual(c.checkable, 1); assert.strictEqual(c.uncovered, 2); });
t('checkRecompute confirmed / contradicted / unknown', () => {
  assert.strictEqual(checkRecompute({ expr: 'SUM', proposed: 10, recomputed: 10 }).status, 'confirmed');
  assert.strictEqual(checkRecompute({ expr: 'SUM', proposed: 10, recomputed: 11 }).status, 'contradicted');
  assert.strictEqual(checkRecompute({ expr: 'X', proposed: 1, recomputed: '#DIV/0!' }).status, 'contradicted');
  assert.strictEqual(checkRecompute({ expr: 'X', proposed: '', recomputed: 5 }).status, 'unknown');
});
t('combineVerdict precedence pass/warn/veto', () => {
  assert.strictEqual(combineVerdict({ checks: [{ status: 'confirmed' }], coverage: { uncovered: 0 } }).verdict, 'pass');
  assert.strictEqual(combineVerdict({ checks: [{ status: 'confirmed' }], coverage: { uncovered: 1 } }).verdict, 'warn');
  assert.strictEqual(combineVerdict({ checks: [{ status: 'contradicted' }], coverage: { uncovered: 0 } }).verdict, 'veto');
  assert.strictEqual(combineVerdict({ parsed: { ok: false, rejected: [1] } }).verdict, 'veto');
});
t('verdictChip + provenance are icon+text, GPU never grounded', () => {
  assert.strictEqual(verdictChip({ verdict: 'pass' }).icon, '✓');
  assert.strictEqual(provenance('M4-plan').grounded, false);
  assert.strictEqual(provenance('det').grounded, true);
  assert.strictEqual(provenance('M3').offline, false);
  assert.strictEqual(provenance('recipe').offline, true);
});

// ───────────────────────── 5. LEARN (offline recipe flywheel) ─────────────────────────
t('normQuery collapses column refs', () => assert.strictEqual(normQuery('Somma la colonna A'), normQuery('somma la colonna B')));
t('warn/veto/not-applied are NEVER learned', () => {
  const s = memStore();
  assert.strictEqual(learn(s, { query: 'q', plan: [{ op: 'total' }], verdict: { verdict: 'warn' }, applied: true }).learned, null);
  assert.strictEqual(learn(s, { query: 'q', plan: [{ op: 'total' }], verdict: { verdict: 'pass' }, applied: false }).learned, null);
  assert.strictEqual(learn(s, { query: 'q', plan: [], verdict: { verdict: 'pass' }, applied: true }).learned, null);
});
t('pass + applied is learned and recalled', () => {
  const s = memStore();
  const plan = [{ op: 'aggregate', fn: 'SUM', col: 0 }];
  const r = learn(s, { query: 'fammi i conti trimestrali per gruppo', plan, verdict: { verdict: 'pass' }, applied: true, ts: 1 });
  assert.ok(r.learned, 'should learn');
  const rec = recall(s, 'fammi i conti trimestrali per gruppo', 2);
  assert.ok(rec && rec.recipe, 'should recall'); assert.deepStrictEqual(rec.recipe.plan, plan);
});
t('near-duplicate refreshes, does not duplicate', () => {
  const s = memStore();
  learn(s, { query: 'classifica i venditori per fatturato totale annuo', plan: [{ op: 'sort', col: 1, order: 'desc' }], verdict: { verdict: 'pass' }, applied: true, ts: 1 });
  const r2 = learn(s, { query: 'classifica i venditori per fatturato totale annuo!', plan: [{ op: 'sort', col: 1, order: 'desc' }], verdict: { verdict: 'pass' }, applied: true, ts: 2 });
  assert.strictEqual(r2.reason, 'refreshed');
});
t('forget clears the store', () => { const s = memStore(); learn(s, { query: 'abc def ghi', plan: [{ op: 'total' }], verdict: { verdict: 'pass' }, applied: true, ts: 1 }); forget(s); assert.strictEqual(recall(s, 'abc def ghi'), null); });
t('trigramJaccard sane', () => { assert.strictEqual(trigramJaccard('hello', 'hello'), 1); assert.ok(trigramJaccard('hello', 'xyzzy') < 0.2); });

// ───────────────────────── 6. ORCHESTRATOR end-to-end (MockEngine + mock sheet) ─────────────────────────
function mockSheet() {
  const applied = [];
  return {
    applied,
    dims: () => ({ rows: 1000, cols: 26 }),
    context: () => ({ rows: 10, headers: 'A..C', cell: 'A1', md: '' }),
    dryRunAction: (a) => a.op === 'formula' && /BAD/.test(a.nl || '') ? { ok: false, error: '#ERR', expr: 'BAD' } : { ok: true, expr: a.op, value: 42 },
    applyAction: (a) => { applied.push(a); return { ok: true }; },
    snapshot: () => ({ rows: [[1, 2], [3, 4]], headers: ['x', 'y'], r1: 0, c1: 0, nrows: 2, ncols: 2 }),
    applyTransform: () => ({ ok: true, changed: 4 }),
  };
}
const realSandbox = { run: async (code, env, opts) => (opts && opts.mode === 'check') ? checkSyntax(code) : { ok: true, value: JSON.stringify([[2, 4], [6, 8]]) } };

t('exec ordering: sort/structure before append (total/aggregate)', () => {
  assert.deepStrictEqual(orderForExec([{ op: 'total' }, { op: 'sort' }]).map((a) => a.op), ['sort', 'total']);
  assert.deepStrictEqual(orderForExec([{ op: 'aggregate' }, { op: 'sort' }, { op: 'clean' }]).map((a) => a.op), ['clean', 'sort', 'aggregate']);
  assert.deepStrictEqual(orderForExec([{ op: 'total' }, { op: 'dedupe' }, { op: 'rmempty' }]).map((a) => a.op), ['rmempty', 'dedupe', 'total']);
  // stable within same priority (two aggregates keep their order)
  assert.deepStrictEqual(orderForExec([{ op: 'aggregate', fn: 'SUM' }, { op: 'aggregate', fn: 'AVERAGE' }]).map((a) => a.fn), ['SUM', 'AVERAGE']);
});
await ta('exec ordering applied: plan [total,sort] mutates sheet as [sort,total]', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['[{"op":"total"},{"op":"sort","col":"B","order":"desc"},{"op":"done"}]']);
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, caps: { webgpu: true, orchReady: true }, log: () => {}, ts: 1 };
  await handle('fai i totali poi ordina per B decrescente', { dial: 'auto', localHit: false }, deps);
  assert.deepStrictEqual(sheet.applied.map((a) => a.op), ['sort', 'total'], 'sort must run before total');
});

await ta('M4-plan: valid plan → verified pass, applied, learned; 2nd time → recipe replay (no engine)', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['[{"op":"aggregate","fn":"SUM","col":"A"},{"op":"total"},{"op":"done"}]']);
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, caps: { webgpu: true, orchReady: true }, log: () => {}, ts: 1 };
  const r1 = await handle('riepilogo e totali del blocco vendite', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r1.kind, 'executed'); assert.strictEqual(r1.substrate, 'M4-plan');
  assert.strictEqual(r1.verdict.verdict, 'pass'); assert.ok(r1.applied.length >= 2); assert.ok(r1.learned, 'should learn');
  // second identical turn: engine must NOT be called again (recipe replays)
  const callsBefore = eng.calls.length;
  const r2 = await handle('riepilogo e totali del blocco vendite', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r2.substrate, 'recipe'); assert.strictEqual(eng.calls.length, callsBefore, 'engine called on replay!');
  assert.strictEqual(r2.verdict.verdict, 'pass');
});

await ta('M4-plan: a contradicted formula → VETO, nothing applied, falls back', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['[{"op":"formula","nl":"BAD compute"},{"op":"done"}]']);
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, caps: { webgpu: true, orchReady: true, online: true }, log: () => {}, ts: 1 };
  const r = await handle('qualcosa di impossibile', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r.kind, 'delegate'); assert.strictEqual(r.to, 'grok'); assert.strictEqual(sheet.applied.length, 0, 'nothing should be applied on veto');
});

await ta('M4-plan: prose (no valid actions) → fall back to device offline', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['I cannot do that, sorry.']);
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, caps: { webgpu: true, orchReady: true }, log: () => {}, ts: 1 };
  const r = await handle('boh', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r.kind, 'delegate'); assert.strictEqual(r.to, 'remote'); assert.strictEqual(r.mode, 'off');
});

await ta('M4-code: coder transform → real checkSyntax passes, sandbox runs, approved → applied (warn, not learned)', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['```js\nreturn rows.map(r => r.map(x => x * 2));\n```']);
  let approvedDiff = null;
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, approve: async (d) => { approvedDiff = d; return true; }, caps: { webgpu: true, coderReady: true }, log: () => {}, ts: 1 };
  const r = await handle('raggruppa per regione e calcola lo z-score', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r.kind, 'executed'); assert.strictEqual(r.substrate, 'M4-code');
  assert.strictEqual(r.verdict.verdict, 'warn'); assert.ok(approvedDiff && approvedDiff.diff, 'should stage a diff');
  assert.strictEqual(recall(store, 'raggruppa per regione e calcola lo z-score'), null, 'warn transform must NOT be learned');
});

await ta('M4-code: rejected approval → not applied', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['```js\nreturn rows.map(r => r.map(x => x + 1));\n```']);
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, approve: async () => false, caps: { webgpu: true, coderReady: true }, log: () => {}, ts: 1 };
  const r = await handle('classifica con punteggio personalizzato', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r.kind, 'rejected'); assert.strictEqual(sheet.applied.length, 0);
});

await ta('M4-code: syntactically broken coder output → VETO via real checkSyntax', async () => {
  const sheet = mockSheet(); const store = memStore();
  const eng = new MockEngine(['```js\nreturn rows.map(r => r.map(x => x * ));\n```']);   // missing operand → syntax error
  const deps = { engine: eng, sheet, store, sandbox: realSandbox, approve: async () => true, caps: { webgpu: true, coderReady: true }, log: () => {}, ts: 1 };
  const r = await handle('trasformazione rotta growth', { dial: 'auto', localHit: false }, deps);
  assert.strictEqual(r.kind, 'failed'); assert.strictEqual(r.verdict.verdict, 'veto');
});

await ta('delegate paths for explicit dials', async () => {
  const base = { engine: new MockEngine([]), sheet: mockSheet(), store: memStore(), sandbox: realSandbox, caps: { online: true }, log: () => {}, ts: 1 };
  assert.strictEqual((await handle('chi è dante', { dial: 'only' }, base)).to, 'grok');
  assert.strictEqual((await handle('chi è dante', { dial: 'on' }, base)).to, 'remote');
  assert.strictEqual((await handle('chi è dante', { dial: 'auto', localHit: false }, { ...base, caps: {} })).to, 'remote');
  assert.strictEqual((await handle('somma A', { dial: 'auto', localHit: true }, base)).to, 'deterministic');
});

console.log(`\nspread-copilot: ${pass} passed, ${fail} failed`);
process.exitCode = fail ? 1 : 0;
