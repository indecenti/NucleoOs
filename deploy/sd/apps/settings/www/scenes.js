// scenes.js — one-tap device "scenes" (profiles). Each scene bundles a settings patch (persisted to
// settings.json) and an ORDERED list of live API calls fired sequentially (never in parallel — the
// device is single-task). Pure: applyScene() returns a new model + the calls to run; it never mutates
// its input and never touches the network itself (the caller does, one at a time).
//
// Hard rule honoured here: every scene references ONLY real keys present in DEFAULTS and ONLY
// endpoints that exist on the device. The scene tests assert exactly that, so a scene can't drift.

import { merge } from './settings-merge.js';

// Endpoints a scene is allowed to drive (the live, safe, RAM-relevant levers). Kept in sync with the
// self-test ENDPOINTS and the firmware routes.
export const SCENE_ENDPOINTS = ['/api/display', '/api/voice/always', '/api/anima/l1', '/api/tts'];

export const SCENES = [
  {
    id: 'focus', icon: '🎯', it: 'Focus', en: 'Focus',
    desc: { it: 'Schermo spento, cervello offline a riposo, silenzio: massimo RAM e zero distrazioni.',
            en: 'Screen off, offline brain stood down, quiet: maximum free RAM and zero distraction.' },
    patch: { power: { display_brightness: 40 }, voice: { alwaysOn: false } },
    apiCalls: [
      { path: '/api/voice/always', method: 'POST', body: { on: false } },
      { path: '/api/anima/l1',     method: 'POST', body: { mode: 'off' } },   // frees ~31 KB
      { path: '/api/display',      method: 'POST', query: { on: 0 } },
    ],
  },
  {
    id: 'battery', icon: '🔋', it: 'Risparmio', en: 'Battery Saver',
    desc: { it: 'Taglia radio e RAM: Swarm off, voce off, cervello offline off, luminosità bassa.',
            en: 'Cut radios and RAM: Swarm off, voice off, offline brain off, low brightness.' },
    patch: { power: { display_brightness: 30 }, network: { swarm: { enabled: false } }, voice: { alwaysOn: false } },
    apiCalls: [
      { path: '/api/voice/always', method: 'POST', body: { on: false } },
      { path: '/api/anima/l1',     method: 'POST', body: { mode: 'off' } },
    ],
  },
  {
    id: 'demo', icon: '🎤', it: 'Presentazione', en: 'Demo',
    desc: { it: 'Schermo acceso e luminoso, tema scuro, cervello offline in automatico.',
            en: 'Screen on and bright, dark theme, offline brain on auto.' },
    patch: { power: { display_brightness: 100 }, ui: { theme: 'dark' } },
    apiCalls: [
      { path: '/api/display',  method: 'POST', query: { on: 1 } },
      { path: '/api/anima/l1', method: 'POST', body: { mode: 'auto' } },
    ],
  },
  {
    id: 'diagnostics', icon: '🩺', it: 'Diagnostica', en: 'Diagnostics',
    desc: { it: 'Esegue il self-test del dispositivo e apre la Diagnostica.',
            en: 'Runs the device self-test and opens Diagnostics.' },
    patch: {},
    apiCalls: [],
    selfTest: true,        // handled specially by the UI: run the self-test sweep + go to the tab
  },
];

export function applyScene(model, id) {
  const scene = SCENES.find((s) => s.id === id);
  if (!scene) return null;
  return {
    scene,
    patch: scene.patch || {},
    model: merge(model, scene.patch || {}),
    apiCalls: (scene.apiCalls || []).map((c) => ({ ...c, body: c.body ? { ...c.body } : undefined, query: c.query ? { ...c.query } : undefined })),
    selfTest: !!scene.selfTest,
  };
}
