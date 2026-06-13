// Gate: ANIMA Forge — intelligent engine fallback. The user must NEVER be left with nothing: the
// device's grounded recipes are the always-reachable floor (no GPU, no network → still answers).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { planEngine, readyTiers, bestPossible, capabilityNote } from '../../apps/anima/www/forge/engine-policy.js';

test('WebGPU + cached model → local-webgpu, not degraded', () => {
  const p = planEngine({ webgpu: true, vramMB: 8000, coderCached: true, deviceBrain: true });
  assert.equal(p.selected, 'local-webgpu');
  assert.equal(p.degraded, false);
  assert.equal(p.chain[0], 'local-webgpu');
});

test('NO WebGPU but WASM cached → local-wasm (offline generative fallback)', () => {
  const p = planEngine({ webgpu: false, wasm: true, wasmCached: true, online: false, deviceBrain: true });
  assert.equal(p.selected, 'local-wasm');
  assert.equal(p.engine, 'wllama');
  assert.equal(p.degraded, true);
});

test('No local path at all (no WebGPU, no WASM) + online+key → online-grok', () => {
  const p = planEngine({ webgpu: false, wasm: false, online: true, hasKey: true, deviceBrain: true });
  assert.equal(p.selected, 'online-grok');
});

test('PRIVACY: no-WebGPU + online+key but WASM available → bridge privately (device floor) while WASM downloads, not cloud', () => {
  const p = planEngine({ webgpu: false, wasm: true, wasmCached: false, online: true, hasKey: true, deviceBrain: true });
  assert.equal(p.target, 'local-wasm');         // a local/private path exists → that's the target
  assert.equal(p.selected, 'device-recipe');    // don't auto-leak to cloud; user can pick online explicitly
  assert.equal(p.private, true);
});

test('THE FLOOR: no GPU, no network, no model → device-recipe (never "decline")', () => {
  const p = planEngine({ webgpu: false, wasm: false, online: false, deviceBrain: true });
  assert.equal(p.selected, 'device-recipe');
  assert.equal(p.engine, 'anima');
  assert.notEqual(p.selected, 'decline');
});

test('offline-first ordering: local tiers precede online, which precedes device', () => {
  const p = planEngine({ webgpu: true, vramMB: 8000, coderCached: true, wasm: true, wasmCached: true, online: true, hasKey: true, deviceBrain: true });
  assert.deepEqual(p.chain, ['local-webgpu', 'online-grok', 'device-recipe']);   // wasm hidden when webgpu cached
});

test('PRIVACY: WebGPU model downloading + online → bridge through the PRIVATE device floor, NOT the cloud', () => {
  const p = planEngine({ webgpu: true, vramMB: 8000, coderCached: false, wasm: false, online: true, hasKey: true, deviceBrain: true });
  assert.equal(p.target, 'local-webgpu');
  assert.equal(p.needsDownload, true);
  assert.equal(p.selected, 'device-recipe');   // a user who opted into LOCAL is never silently sent to cloud
  assert.equal(p.bridge, 'device-recipe');
  assert.equal(p.private, true);
});

test('WebGPU present, model not cached, OFFLINE → bridge via device recipes while it downloads', () => {
  const p = planEngine({ webgpu: true, vramMB: 8000, coderCached: false, wasm: false, online: false, deviceBrain: true });
  assert.equal(p.target, 'local-webgpu');
  assert.equal(p.needsDownload, true);
  assert.equal(p.selected, 'device-recipe');
  assert.equal(p.bridge, 'device-recipe');
});

test('insufficient VRAM disqualifies WebGPU codegen → falls through', () => {
  const p = planEngine({ webgpu: true, vramMB: 400, coderCached: true, wasm: true, wasmCached: true, deviceBrain: true });
  assert.notEqual(p.selected, 'local-webgpu');
  assert.equal(p.selected, 'local-wasm');
});

test('detached page (no device brain), nothing else → decline (honest)', () => {
  const p = planEngine({ webgpu: false, wasm: false, online: false, deviceBrain: false });
  assert.equal(p.selected, 'decline');
  assert.equal(p.engine, null);
});

test('readyTiers never lists local-webgpu unless cached; bestPossible may target it', () => {
  const caps = { webgpu: true, vramMB: 8000, coderCached: false, deviceBrain: true };
  assert.equal(readyTiers(caps).some((t) => t.tier === 'local-webgpu'), false);
  assert.equal(bestPossible(caps).tier, 'local-webgpu');
  assert.equal(bestPossible(caps).needsDownload, true);
});

test('capabilityNote is an honest one-liner (mentions the one-time download when relevant)', () => {
  const n = capabilityNote({ webgpu: true, vramMB: 8000, coderCached: false, online: true, hasKey: true, deviceBrain: true });
  assert.match(n, /download/i);
  assert.match(n, /local-webgpu/);
});

test('EXHAUSTIVE: across the full capability matrix there is ALWAYS a usable tier when device is reachable', () => {
  let combos = 0;
  for (const webgpu of [true, false]) for (const vramMB of [400, 8000]) for (const coderCached of [true, false])
    for (const wasm of [true, false]) for (const wasmCached of [true, false])
      for (const online of [true, false]) for (const hasKey of [true, false]) {
        const p = planEngine({ webgpu, vramMB, coderCached, wasm, wasmCached, online, hasKey, deviceBrain: true });
        assert.notEqual(p.selected, 'decline', `no fallback for ${JSON.stringify({ webgpu, vramMB, coderCached, wasm, wasmCached, online, hasKey })}`);
        assert.ok(['local-webgpu', 'local-wasm', 'online-grok', 'device-recipe'].includes(p.selected));
        combos++;
      }
  assert.equal(combos, 128);
});

// ---------- hardening from the adversarial review (privacy + honesty) ----------

test('PRIVACY INVARIANT: when the target is a LOCAL/private model, the answer-now tier is never the cloud', () => {
  for (const webgpu of [true, false]) for (const vramMB of [400, 8000]) for (const coderCached of [true, false])
    for (const wasm of [true, false]) for (const wasmCached of [true, false])
      for (const online of [true, false]) for (const hasKey of [true, false]) {
        const caps = { webgpu, vramMB, coderCached, wasm, wasmCached, online, hasKey, deviceBrain: true };
        const p = planEngine(caps);
        if (['local-webgpu', 'local-wasm'].includes(p.target) && p.needsDownload) {
          assert.notEqual(p.selected, 'online-grok', `leaked to cloud while downloading a private model: ${JSON.stringify(caps)}`);
          assert.notEqual(p.bridge, 'online-grok');
          assert.equal(p.private, true, `bridge tier must be private: ${JSON.stringify(caps)}`);
        }
      }
});

test('HONESTY: device-recipe reason does NOT claim "no network" when the network is actually up', () => {
  const p = planEngine({ webgpu: false, wasm: false, online: true, hasKey: false, deviceBrain: true });
  assert.equal(p.selected, 'device-recipe');
  assert.equal(/no network|offline/i.test(p.reason), false);
  assert.match(p.reason, /key/i);
});

test('HONESTY: offline yields the same honest reason whether webgpu is false or undefined', () => {
  const a = planEngine({ wasm: false, online: false, deviceBrain: true });
  const b = planEngine({ webgpu: false, wasm: false, online: false, deviceBrain: true });
  assert.equal(a.reason, b.reason);
  assert.match(a.reason, /offline/i);
});

test('HONESTY: capabilityNote surfaces "not private"/cloud when answering via online-grok', () => {
  const caps = { webgpu: false, wasm: true, wasmCached: false, online: true, hasKey: true, deviceBrain: false };
  assert.equal(planEngine(caps).selected, 'online-grok');
  assert.match(capabilityNote(caps), /not private|cloud/i);
});

test('HONESTY: capabilityNote carries the slower/CPU caveat when local-wasm is selected', () => {
  const caps = { webgpu: false, wasm: true, wasmCached: true, online: false, deviceBrain: true };
  assert.equal(planEngine(caps).selected, 'local-wasm');
  assert.match(capabilityNote(caps), /slow|smaller|cpu/i);
});

test('DECLINE: no download CTA, needsDownload false, degraded false, no "upgrade" pitch', () => {
  const caps = { webgpu: true, vramMB: 8000, coderCached: false, wasm: false, online: false, deviceBrain: false };
  const p = planEngine(caps);
  assert.equal(p.selected, 'decline');
  assert.equal(p.needsDownload, false);     // can't fetch weights: offline AND no device to serve them
  assert.equal(p.degraded, false);
  assert.equal(/download|upgrade/i.test(capabilityNote(caps)), false);
});

test('REACHABILITY: offline but device present → download is phrased "from the device", still reachable', () => {
  const caps = { webgpu: true, vramMB: 8000, coderCached: false, wasm: false, online: false, deviceBrain: true };
  const p = planEngine(caps);
  assert.equal(p.selected, 'device-recipe');   // private floor while it fetches from SD
  assert.equal(p.needsDownload, true);
  assert.match(capabilityNote(caps), /from the device/i);
});

test('CONTROL: when the target IS the cloud (no local path), bridging/answering via cloud is acceptable', () => {
  const caps = { webgpu: false, wasm: false, online: true, hasKey: true, deviceBrain: false };
  const p = planEngine(caps);
  assert.equal(p.selected, 'online-grok');     // no local option and no device floor → cloud is correct
});
