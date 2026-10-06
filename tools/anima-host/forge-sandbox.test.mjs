// Gate: ANIMA Forge — sandbox parse-only check (mode:'check'). Host-safe (no Worker): the VERIFY
// gate must validate a candidate WITHOUT executing it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { checkSyntax, createRunner, fixStackLines } from '../../apps/code-runner/www/nucleo-run.js';

test('checkSyntax accepts valid code and rejects syntax errors — without running it', () => {
  assert.equal(checkSyntax('console.log(1); const x = 2+2;').ok, true);
  assert.equal(checkSyntax('await os.fs.read("a")').ok, true);              // top-level await ok (async fn body)
  assert.equal(checkSyntax('function (').ok, false);
  assert.equal(checkSyntax('const x = ;').ok, false);
  assert.equal(checkSyntax('for(;;){').ok, false);
});

test('check NEVER executes — an infinite loop / side effect does not run', () => {
  // if this executed it would hang or throw; parse-only returns instantly.
  const r = checkSyntax('while(true){}; throw new Error("should not run")');
  assert.equal(r.ok, true);
});

test('runner mode:check short-circuits to parse-only (no Worker spawned in Node)', async () => {
  const rt = createRunner({});
  const ok = await rt.run('const a = 1;', undefined, { mode: 'check' });
  assert.equal(ok.ok, true);
  const bad = await rt.run('const a = ;', undefined, { mode: 'check' });
  assert.equal(bad.ok, false);
  rt.dispose();
});

// Found on a real Cardputer: a `throw` on line 4 of a snippet was reported at nucleo-script.js:7:7 — the
// AsyncFunction header and the "use strict" line shift every line. The runner maps them back.
test('fixStackLines: an error points at the line the user wrote', async () => {
  await new Promise((r) => setTimeout(r, 0));                     // the shift probe settles on a microtask at load
  const user = ['const a = 1;', 'const b = 2;', 'const c = 3;', 'throw new Error("boom");'].join('\n');   // the throw is on line 4
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
  const fn = new AsyncFn('os', 'console', 'print', 'args', 'env', ['"use strict";', user, '//# sourceURL=nucleo-script.js'].join('\n'));
  let stack = '';
  try { await fn(); } catch (e) { stack = String(e.stack); }
  const raw = +/nucleo-script\.js:(\d+):/.exec(stack)[1];
  assert.ok(raw > 4, 'precondition: the engine reports a shifted line (' + raw + ')');
  assert.match(fixStackLines(stack), /nucleo-script\.js:4:7/);
  const other = ['Error: x', '    at foo (other.js:9:1)'].join('\n');
  assert.equal(fixStackLines(other), other, 'other files untouched');
  assert.equal(fixStackLines(''), '');
});
