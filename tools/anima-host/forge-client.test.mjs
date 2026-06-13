// Gate: ANIMA Forge — the single client adapter (substrate routing → canonical Envelope v1) and the
// pure transcript/dial view-model. Proves the UX behaviour is CERTAIN (correct substrate, honest
// provenance, M4 never borrows a device label, dial greys out 'local' without WebGPU) before any DOM.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeClient } from '../../apps/anima/www/forge/client.js';
import { validate, provenance } from '../../apps/anima/www/forge/envelope.js';
import { turnModel, dialModel } from '../../apps/anima/www/forge/transcript.js';
import { verdictChip } from '../../apps/anima/www/forge/verify.js';

// A device transport mock that echoes the mode it was called with (so we can assert routing).
function mockDevice() {
  const calls = [];
  const fn = async (q, opts) => {
    calls.push({ q, opts });
    return { query: q, reply: 'device says ' + q, tier: 'L1', trace: 'L0 > L1', lang: opts.lang };
  };
  fn.calls = calls;
  return fn;
}
function mockLocal() {
  const calls = [];
  // Note: the local provider deliberately TRIES to claim a device substrate — the client must ignore it.
  const fn = async (q, opts) => {
    calls.push({ q, opts });
    return { query: q, reply: 'gpu says ' + q, substrate: 'M2', trace: 'plan · gen · verify' };
  };
  fn.calls = calls;
  return fn;
}

test('ask routes off/on/only to deviceTransport with the right mode + valid envelope/substrate', async () => {
  const dev = mockDevice();
  const c = makeClient({ deviceTransport: dev, localProvider: mockLocal(), caps: {} });

  const off = await c.ask('ciao', { mode: 'off', lang: 'it' });
  assert.equal(dev.calls.at(-1).opts.mode, 'off');
  assert.equal(off.substrate, 'M1');
  assert.equal(validate(off).ok, true);

  const on = await c.ask('ciao', { mode: 'on' });
  assert.equal(dev.calls.at(-1).opts.mode, 'on');
  assert.equal(on.substrate, 'M2');

  const only = await c.ask('ciao', { mode: 'only' });
  assert.equal(dev.calls.at(-1).opts.mode, 'only');
  assert.equal(only.substrate, 'M3');
  assert.equal(validate(only).ok, true);
});

test("mode 'local' uses localProvider, stamps M4-local, and is NEVER grounded (no borrowed device label)", async () => {
  const dev = mockDevice();
  const loc = mockLocal();
  const c = makeClient({ deviceTransport: dev, localProvider: loc, caps: { webgpu: true } });

  const env = await c.ask('scrivi una funzione', { mode: 'local' });
  assert.equal(loc.calls.length, 1);                 // local provider was used
  assert.equal(dev.calls.length, 0);                 // device was NOT touched
  assert.equal(env.substrate, 'M4-local');           // forced — provider's claimed 'M2' ignored
  assert.equal(validate(env).ok, true);
  assert.equal(provenance(env).grounded, false);     // honesty contract
});

test("mode 'auto': code request with webgpu+coderCached → local; knowledge question → device", async () => {
  const dev = mockDevice();
  const loc = mockLocal();
  const c = makeClient({ deviceTransport: dev, localProvider: loc, caps: { webgpu: true, coderCached: true, online: true } });

  const code = await c.ask('scrivi una funzione debounce in javascript', { mode: 'auto' });
  assert.equal(code.substrate, 'M4-local');
  assert.equal(loc.calls.length, 1);

  const know = await c.ask('chi è Einstein', { mode: 'auto' });
  assert.equal(know.substrate, 'M2');                // caps.online → hybrid grounded device
  assert.equal(dev.calls.at(-1).opts.mode, 'on');
});

test("mode 'auto': a deterministic file op returns a {substrate:'hands'} marker, not an envelope", async () => {
  const dev = mockDevice();
  const c = makeClient({ deviceTransport: dev, localProvider: mockLocal(), caps: {} });
  const r = await c.ask('leggi note.txt', { mode: 'auto', root: '/data' });
  assert.equal(r.substrate, 'hands');
  assert.ok(r.intent && typeof r.intent === 'object');   // fsclient consumes this
  assert.equal(dev.calls.length, 0);                      // no device round-trip
});

// A device transport whose reply depends on q + mode, to exercise the translation ladder.
function mockTransDevice() {
  const calls = [];
  const fn = async (q, opts) => {
    calls.push({ q, opts });
    if (opts.mode === 'only') return { query: q, reply: `cloud: ${q} → widget`, tier: 'L3', lang: opts.lang };
    if (/floombix/i.test(q)) return { query: q, reply: 'Non ho "floombix" nel dizionario offline IT<->EN.', tier: 'L0', lang: opts.lang };
    return { query: q, reply: '"casa" in inglese: house, home.', tier: 'L0', lang: opts.lang };
  };
  fn.calls = calls;
  return fn;
}

test("translation 'auto' is dictionary-first: a known word hits the device, NO LLM escalation", async () => {
  const dev = mockTransDevice();
  const loc = mockLocal();
  const c = makeClient({ deviceTransport: dev, localProvider: loc, caps: { webgpu: true, coderCached: true, online: true } });
  const hit = await c.ask('traduci casa in inglese', { mode: 'auto' });
  assert.equal(hit.substrate, 'M1');                  // grounded device dictionary
  assert.equal(dev.calls.at(-1).opts.mode, 'off');    // offline dictionary path
  assert.equal(loc.calls.length, 0);                  // the LLM was NOT touched
  assert.match(hit.reply, /house/);
});

test('translation escalates to the local LLM (M4) on a dictionary miss', async () => {
  const dev = mockTransDevice();
  const loc = mockLocal();
  const c = makeClient({ deviceTransport: dev, localProvider: loc, caps: { webgpu: true, coderCached: true } });
  const esc = await c.ask('traduci floombix in inglese', { mode: 'auto' });
  assert.equal(esc.substrate, 'M4-local');            // climbed to the generative tier
  assert.equal(loc.calls.length, 1);
  assert.equal(loc.calls[0].opts.kind, 'translate');  // provider told it's a translation
});

test('translation escalates to the cloud teacher (M3) when no local model, on a miss', async () => {
  const dev = mockTransDevice();
  const c = makeClient({ deviceTransport: dev, localProvider: mockLocal(), caps: { online: true } });
  const esc = await c.ask('traduci floombix in inglese', { mode: 'auto' });
  assert.equal(dev.calls.at(-1).opts.mode, 'only');   // cloud teacher
  assert.equal(esc.substrate, 'M3');
  assert.match(esc.reply, /widget/);
});

test('translation offline with no models → honest device decline (the last available)', async () => {
  const dev = mockTransDevice();
  const c = makeClient({ deviceTransport: dev, localProvider: mockLocal(), caps: { online: false } });
  const r = await c.ask('traduci floombix in inglese', { mode: 'auto' });
  assert.equal(r.substrate, 'M1');                    // degraded to the grounded device decline
  assert.match(r.reply, /dizionario/);
});

test('makeClient requires deviceTransport; ask throws helpfully if local is needed but absent', async () => {
  assert.throws(() => makeClient({}), /deviceTransport/);
  const c = makeClient({ deviceTransport: mockDevice(), caps: { webgpu: true } });   // no localProvider
  await assert.rejects(() => c.ask('x', { mode: 'local' }), /localProvider/);
});

test('dialModel: greys out local without webgpu (with an honest reason), available with webgpu', () => {
  const off = dialModel('on', { webgpu: false });
  const local = off.find((s) => s.id === 'local');
  assert.equal(local.available, false);
  assert.match(local.reason, /WebGPU/);
  assert.equal(off.find((s) => s.id === 'on').current, true);   // current is marked
  assert.deepEqual(off.map((s) => s.id), ['off', 'on', 'only', 'local', 'auto']);  // ordered, auto present

  const on = dialModel('local', { webgpu: true });
  assert.equal(on.find((s) => s.id === 'local').available, true);

  // bilingual labels
  assert.equal(dialModel('off', { webgpu: true, lang: 'en' }).find((s) => s.id === 'on').label, 'Hybrid');
  assert.equal(dialModel('off', { webgpu: true, lang: 'it' }).find((s) => s.id === 'on').label, 'Ibrida');
});

test('turnModel: chips, trace split, grounding, awaiting — pass/warn/veto', () => {
  // a grounded M2 turn with a PASS verdict
  const pass = turnModel({
    v: 1, substrate: 'M2', reply: 'r', trace: 'L0 > L1 · rerank', awaiting: false,
    grounding: [{ id: 'wd.einstein' }], verdict: { verdict: 'pass' }, lang: 'it',
  });
  assert.equal(pass.substrateChip.kind, 'hybrid');
  assert.equal(pass.substrateChip.grounded, true);
  assert.ok(pass.substrateChip.icon && pass.substrateChip.label);     // icon+text, not colour-only
  assert.deepEqual(pass.traceSteps, ['L0', 'L1', 'rerank']);           // split on > and ·
  assert.deepEqual(pass.verdictChip, verdictChip({ verdict: 'pass' }));
  assert.equal(pass.grounding.length, 1);
  assert.equal(pass.awaiting, false);

  // M4-local WARN turn — ungrounded chip + warn verdict
  const warn = turnModel({ v: 1, substrate: 'M4-local', reply: 'r', trace: 'plan', verdict: { verdict: 'warn' }, lang: 'en' });
  assert.equal(warn.substrateChip.grounded, false);
  assert.equal(warn.substrateChip.label, 'local GPU');                 // EN label
  assert.equal(warn.verdictChip.label, 'unverified');

  // VETO turn
  const veto = turnModel({ v: 1, substrate: 'M4-local', verdict: { verdict: 'veto' } });
  assert.equal(veto.verdictChip.icon, verdictChip({ verdict: 'veto' }).icon);

  // no verdict at all → 'unchecked', and awaiting passthrough
  const bare = turnModel({ v: 1, substrate: 'M1', awaiting: true });
  assert.equal(bare.verdictChip.label, 'unchecked');
  assert.equal(bare.awaiting, true);
  assert.deepEqual(bare.traceSteps, []);
});
