// Unit tests for the intelligent AI presets (apps/settings/www/preset-engine.js).
// Builds the registry from the REAL single source (web/shell/ai.js) so a drift between the engine and the
// provider/model catalog fails here. Mirrors the scenes test discipline: presets reference only real
// localStorage keys + only endpoints that exist; plan() is total; resolveModel never invents an id.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PROVIDERS, CAPMATRIX, TIERS } from '../web/shell/ai.js';
import {
  PRESETS, PRESET_IDS, PRESET_ENDPOINTS, buildRegistry, defaultSig,
  resolveModel, planPreset, feasibility, applyPreset, gaps, APP_MAP, appStatus,
} from '../apps/settings/www/preset-engine.js';
import { DEFAULTS, merge, clone } from '../apps/settings/www/settings-merge.js';

const REG = buildRegistry(PROVIDERS, CAPMATRIX, TIERS);
const sig = (over = {}) => Object.assign(defaultSig(), over);
const withKey = (provider, over = {}) => sig(Object.assign({
  provider, hasKey: true, online: true, enabled: true,
  keys: { anthropic: provider === 'anthropic', openai: provider === 'openai', xai: provider === 'xai', google: provider === 'google' },
}, over));

test('preset ids are unique and DEFAULTS carries a durable ai.preset intent', () => {
  assert.equal(new Set(PRESET_IDS).size, PRESET_IDS.length);
  assert.equal(DEFAULTS.ai.preset, 'auto');                       // the only SD-durable AI field
});

test('registry is built from the single ai.js source for every provider', () => {
  for (const id of Object.keys(PROVIDERS)) {
    assert.ok(REG.providers[id], `missing provider ${id}`);
    assert.ok(Array.isArray(REG.providers[id].models));
    assert.ok(REG.providers[id].caps && typeof REG.providers[id].caps.chat === 'boolean', `${id} has no capability row`);
  }
  assert.ok(REG.providers.xai, 'xai must exist in ai.js (the historical drift)');
});

test('resolveModel returns only ids present in the registry, never invents', () => {
  for (const id of Object.keys(REG.providers)) {
    for (const tier of ['max', 'mid', 'fast']) {
      const m = resolveModel(id, tier, REG, true);
      assert.ok((REG.providers[id].models).some((x) => x[0] === m), `${id}.${tier} → ${m} not in registry`);
    }
  }
});

test('Gemini max downgrades to Flash unless the plan is paid', () => {
  assert.equal(resolveModel('google', 'max', REG, false), 'gemini-2.5-flash');
  assert.equal(resolveModel('google', 'max', REG, true), 'gemini-2.5-pro');
});

test('Auto ladder: cloud → GPU → WASM → device, by what is actually ready', () => {
  assert.equal(planPreset('auto', withKey('anthropic'), REG).mode, 'only');                       // key + online
  assert.equal(planPreset('auto', sig({ webgpu: true, vramMB: 2000, localModelReady: true }), REG).mode, 'local');
  assert.equal(planPreset('auto', sig({ packUsable: true }), REG).mode, 'edge');
  assert.equal(planPreset('auto', sig(), REG).mode, 'on');                                          // bare floor → device cascade
  assert.equal(feasibility('auto', sig(), REG).state, 'recommended');                              // Auto is never blocked
});

test('Max is the only blockable preset and needs a key + online', () => {
  assert.equal(feasibility('max', sig(), REG).state, 'blocked');                                   // no key, offline
  assert.equal(feasibility('max', withKey('anthropic', { online: false }), REG).state, 'blocked'); // key but offline
  const ok = feasibility('max', withKey('anthropic'), REG);
  assert.equal(ok.state, 'available');
  const plan = planPreset('max', withKey('anthropic'), REG);
  assert.equal(plan.mode, 'only'); assert.equal(plan.l1, 'off');                                    // device stands down to free RAM
  assert.equal(plan.teacherModel, 'claude-opus-4-8');                                               // top tier of the active provider
});

test('Balanced is always offerable and routes through the firmware cascade (heap-gate respected)', () => {
  assert.equal(feasibility('balanced', sig(), REG).state, 'available');
  const noKey = planPreset('balanced', sig(), REG);
  assert.equal(noKey.mode, 'on'); assert.equal(noKey.teacherModel, null);                           // no key → device only
  const withC = planPreset('balanced', withKey('anthropic'), REG);
  assert.equal(withC.teacherModel, 'claude-sonnet-4-6');                                            // mid tier
});

test('Local is needs-prep without a usable browser pack, available with one', () => {
  assert.equal(feasibility('local', sig(), REG).state, 'needs-prep');
  assert.equal(feasibility('local', sig({ packUsable: true }), REG).state, 'available');
  assert.equal(planPreset('local', sig({ webgpu: true, vramMB: 4000, localModelReady: true }), REG).mode, 'local');
});

test('Privacy kills network egress: edgeWeb off, no cloud, L1 resident', () => {
  const p = planPreset('private', sig({ packUsable: true }), REG);
  assert.equal(p.edgeWeb, 0); assert.equal(p.l1, 'on'); assert.equal(p.mode, 'edge');
  assert.equal(planPreset('private', sig(), REG).mode, 'off');                                      // no pack → pure device offline
});

test('applyPreset returns ordered DATA only, drives only real endpoints + real localStorage keys', () => {
  const res = applyPreset(clone(DEFAULTS), 'max', withKey('anthropic'), REG);
  assert.equal(res.model.ai.preset, 'max');                                                         // SD intent
  assert.equal(res.local['anima.mode'], 'only');
  assert.equal(res.local['anima.modeSet'], '1');
  for (const k of Object.keys(res.local)) assert.ok(/^anima\.(mode|modeSet|localModel|edgeWeb)$/.test(k), `unexpected localStorage key ${k}`);
  for (const c of res.apiCalls) { assert.ok(PRESET_ENDPOINTS.includes(c.path), `unknown endpoint ${c.path}`); assert.equal(c.method, 'POST'); }
  assert.equal(res.apiCalls[0].body.mode, 'off');                                                   // L1 policy
});

test('applyPreset is pure (does not mutate the input model)', () => {
  const input = clone(DEFAULTS), snap = clone(input);
  applyPreset(input, 'balanced', withKey('google'), REG);
  assert.deepEqual(input, snap);
});

test('applyPreset preserves unmanaged settings keys (drift-aware)', () => {
  const onDisk = merge(DEFAULTS, { _custom: 7, ui: { theme: 'light' } });
  const res = applyPreset(onDisk, 'private', sig(), REG);
  assert.equal(res.model._custom, 7);
  assert.equal(res.model.ui.theme, 'light');
});

test('gaps name the real provider limits and clear when a secondary key covers them', () => {
  const g = gaps(withKey('anthropic'), REG).map((x) => x.feature);
  assert.ok(g.includes('image') && g.includes('whisper'));                                          // Claude: no image/whisper
  assert.ok(!g.includes('ir'), 'IR NL now works on every provider (cloudToolCall) — no longer a gap');
  const g2 = gaps(withKey('anthropic', { keys: { anthropic: true, openai: true, xai: true, google: false } }), REG).map((x) => x.feature);
  assert.ok(!g2.includes('image') && !g2.includes('whisper'), 'secondary keys clear image+whisper gaps');
  assert.equal(gaps(withKey('openai'), REG).find((x) => x.feature === 'whisper'), undefined);       // Groq does Whisper natively
});

test('which-apps status reflects the active provider capability matrix', () => {
  const c = APP_MAP.reduce((o, a) => (o[a.id] = appStatus(a, withKey('anthropic'), REG).ok, o), {});
  assert.equal(c.chat, true); assert.equal(c.image, false); assert.equal(c.whisper, false);
  assert.equal(c.ir, true);                                                                          // IR NL repaired for Claude/xAI
  const x = appStatus(APP_MAP.find((a) => a.id === 'image'), withKey('xai'), REG);
  assert.equal(x.ok, true);                                                                          // xAI draws
});
