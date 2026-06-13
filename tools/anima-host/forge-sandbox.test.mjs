// Gate: ANIMA Forge — sandbox parse-only check (mode:'check'). Host-safe (no Worker): the VERIFY
// gate must validate a candidate WITHOUT executing it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { checkSyntax, createRunner } from '../../apps/code-runner/www/nucleo-run.js';

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
