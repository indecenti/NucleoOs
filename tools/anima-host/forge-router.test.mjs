// Gate: ANIMA Forge — two-stage router + VRAM-aware model plan. Stage A must route deterministically
// with the device grounded brain first; `auto` must never attempt two-model residency it can't fit.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { prerouteStageA, isCodeRequest, isTranslateRequest, translateTarget, modelPlan, needsOrchestrator } from '../../apps/anima/www/forge/router.js';

test('isCodeRequest: gen-verb + (code-noun | lang), not a bare knowledge query', () => {
  assert.equal(isCodeRequest('scrivimi una funzione debounce in javascript'), true);
  assert.equal(isCodeRequest('write a function that sorts an array'), true);
  assert.equal(isCodeRequest('genera uno snippet'), true);
  assert.equal(isCodeRequest("cos'è javascript"), false);          // lang, no gen verb
  assert.equal(isCodeRequest('chi ha creato python'), false);
  assert.equal(isCodeRequest('che ore sono'), false);
});

test('Stage A routing order: slash → file → code → knowledge', () => {
  assert.equal(prerouteStageA('/help').kind, 'slash');
  assert.equal(prerouteStageA('leggi note.txt').substrate, 'hands');
  // code request, capability-aware degradation
  assert.equal(prerouteStageA('scrivi una funzione fetch in js', { webgpu: true, coderCached: true }).substrate, 'M4-local');
  assert.equal(prerouteStageA('scrivi una funzione fetch in js', { webgpu: false, online: true }).substrate, 'M3');
  assert.equal(prerouteStageA('scrivi una funzione fetch in js', { webgpu: false, online: false }).substrate, 'M1');
  // knowledge → grounded device first
  assert.equal(prerouteStageA('chi è Einstein', { online: true }).substrate, 'M2');
  assert.equal(prerouteStageA('chi è Einstein', { online: false }).substrate, 'M1');
});

test('isTranslateRequest + target: strong trigger, no false positives', () => {
  assert.equal(isTranslateRequest('traduci casa in inglese'), true);
  assert.equal(isTranslateRequest('traducimi acqua'), true);
  assert.equal(isTranslateRequest('come si dice grazie in inglese'), true);
  assert.equal(isTranslateRequest('how do you say hello in italian'), true);
  assert.equal(isTranslateRequest('translate dog to italian'), true);
  assert.equal(isTranslateRequest("cos'è una traduzione"), false);   // noun, no language → knowledge
  assert.equal(isTranslateRequest('scrivi una funzione'), false);
  assert.equal(isTranslateRequest('che ore sono'), false);
  assert.equal(translateTarget('traduci casa in inglese'), 'en');
  assert.equal(translateTarget('translate dog to italian'), 'it');
  assert.equal(translateTarget('traduci casa'), null);               // auto-detect from the word
});

test('Stage A: translation routes to the dictionary floor, capability-aware', () => {
  const gpu = prerouteStageA('traduci casa in inglese', { webgpu: true, coderCached: true });
  assert.equal(gpu.kind, 'translate');
  assert.equal(gpu.substrate, 'M4-local');      // a local LLM can translate any phrase
  assert.equal(gpu.target, 'en');
  assert.equal(prerouteStageA('traduci casa in inglese', { online: true }).substrate, 'M2');   // device dict → cloud
  assert.equal(prerouteStageA('traduci casa in inglese', {}).substrate, 'M1');                 // offline dict floor
});

test('modelPlan is VRAM-gated (no two-model residency on small iGPUs)', () => {
  assert.equal(modelPlan({ webgpu: false }).mode, 'none');
  assert.equal(modelPlan({ webgpu: true, vramMB: 8000 }).mode, 'two-model');
  assert.equal(modelPlan({ webgpu: true, vramMB: 4000 }).mode, 'two-model-paged');
  assert.equal(modelPlan({ webgpu: true, vramMB: 2000 }).mode, 'single-model');
  assert.equal(modelPlan({ webgpu: true, vramMB: 2000 }).orchestrator, false);
});

test('orchestrator only for ambiguous tail, and only if a planner fits', () => {
  // Stage A decided → no orchestrator needed
  assert.equal(needsOrchestrator(prerouteStageA('leggi a.txt'), { webgpu: true, vramMB: 8000 }), false);
  // ambiguous (null Stage A) on a small GPU → single-model plan has no orchestrator
  assert.equal(needsOrchestrator(null, { webgpu: true, vramMB: 2000 }), false);
  // ambiguous on a roomy GPU → orchestrator available
  assert.equal(needsOrchestrator(null, { webgpu: true, vramMB: 8000 }), true);
});
