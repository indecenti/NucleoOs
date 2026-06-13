// Gate: ANIMA Forge — least-privilege capability inference + danger scan over generated code.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { inferCapabilities, scanDangers, assess } from '../../apps/anima/www/forge/capguard.js';

test('inferCapabilities detects the os.* surface the code actually uses', () => {
  const caps = inferCapabilities('await os.fs.read("a"); await os.fs.write("b","c"); await os.http.get("u"); os.anima("q")');
  assert.ok(caps.includes('fs.read'));
  assert.ok(caps.includes('fs.write'));
  assert.ok(caps.includes('http'));
  assert.ok(caps.includes('anima'));
  assert.deepEqual(inferCapabilities('console.log(1)'), []);
});

test('scanDangers flags eval / dynamic Function / importScripts as BLOCK', () => {
  assert.ok(scanDangers('eval("x")').some((d) => d.kind === 'dynamic-eval' && d.level === 'block'));
  assert.ok(scanDangers('const f = new Function("return 1")').some((d) => d.kind === 'dynamic-eval'));
  assert.ok(scanDangers('importScripts("x")').some((d) => d.kind === 'dynamic-import'));
});

test('mutation inside a loop is BLOCK (a hallucinated delete-loop)', () => {
  const d = scanDangers('for (let i=0;i<100;i++){ await os.fs.remove("f"+i); }');
  assert.ok(d.some((x) => x.kind === 'mutation-in-loop' && x.level === 'block'));
  // a single guarded write is NOT flagged as a loop danger
  assert.equal(scanDangers('await os.fs.write("a","b")').some((x) => x.kind === 'mutation-in-loop'), false);
});

test('network is BLOCK when the task is offline, allowed when granted', () => {
  assert.ok(scanDangers('os.http.get("http://x")', { allowNetwork: false }).some((d) => d.kind === 'network-exfil'));
  assert.equal(scanDangers('os.http.get("http://x")', { allowNetwork: true }).some((d) => d.kind === 'network-exfil'), false);
});

test('assess severity: clean→ok, over-privilege→warn, dangerous→block', () => {
  assert.equal(assess('console.log(1)').severity, 'ok');
  const over = assess('await os.fs.write("a","b")', { granted: ['fs.read'] });
  assert.equal(over.severity, 'warn');
  assert.deepEqual(over.over, ['fs.write']);
  assert.equal(assess('eval(userInput)').severity, 'block');
});
