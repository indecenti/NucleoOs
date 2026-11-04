/* SPDX-License-Identifier: AGPL-3.0-or-later */
// scenes.js — one-tap device "scenes" (profiles). Each scene bundles a settings patch (persisted to
// settings.json, only when it has one) and an ORDERED list of live API calls fired sequentially (never in
// parallel — the device is single-task). Pure: applyScene() returns a new model + the calls to run; it never mutates
// its input and never touches the network itself (the caller does, one at a time).
//
// Hard rule honoured here: every scene references ONLY real keys present in DEFAULTS and ONLY
// endpoints that exist on some device. The scene tests assert exactly that, so a scene can't drift.
// Each device has its own levers: the Cardputer firmware's (screen, voice, offline brain — its settings.json
// patch applied at boot) and the Handheld's live brightness / volume. The Settings UI shows only the scenes
// whose every call this device answers (index.html epOk), so a scene never "applies" nothing.

import { merge } from './settings-merge.js';

// Endpoints a scene is allowed to drive (the live, safe levers). Kept in sync with the firmware routes
// (Cardputer: the first four) and the Handheld server (server.py: brightness, volume).
export const SCENE_ENDPOINTS = ['/api/display', '/api/voice/always', '/api/anima/l1', '/api/tts', '/api/brightness', '/api/volume'];

const level = (path, value) => ({ path, method: 'POST', body: { value } });

export const SCENES = [
  {
    id: 'focus', icon: '🎯', it: 'Focus', en: 'Focus', es: 'Concentración', fr: 'Concentration', de: 'Fokus', zh: '专注',
    desc: { it: 'Schermo spento, cervello offline a riposo, silenzio: massimo RAM e zero distrazioni.',
            en: 'Screen off, offline brain stood down, quiet: maximum free RAM and zero distraction.',
            es: 'Pantalla apagada, cerebro offline en reposo, silencio: máxima RAM libre y cero distracciones.',
            fr: 'Écran éteint, cerveau hors ligne au repos, silence : un maximum de RAM libre et zéro distraction.',
            de: 'Bildschirm aus, Offline-Gehirn im Ruhezustand, Stille: maximal freier Arbeitsspeicher und keine Ablenkung.', zh: '屏幕关闭，离线大脑停用，保持安静：最多的可用 RAM，零干扰。' },
    patch: { power: { display_brightness: 40 }, voice: { alwaysOn: false } },
    apiCalls: [
      { path: '/api/voice/always', method: 'POST', body: { on: false } },
      { path: '/api/anima/l1',     method: 'POST', body: { mode: 'off' } },   // frees ~31 KB
      { path: '/api/display',      method: 'POST', query: { on: 0 } },
    ],
  },
  {
    id: 'battery', icon: '🔋', it: 'Risparmio', en: 'Battery Saver', es: 'Ahorro', fr: 'Économie', de: 'Energiesparen', zh: '省电',
    desc: { it: 'Taglia radio e RAM: Swarm off, voce off, cervello offline off, luminosità bassa.',
            en: 'Cut radios and RAM: Swarm off, voice off, offline brain off, low brightness.',
            es: 'Recorta radios y RAM: Swarm apagado, voz apagada, cerebro offline apagado, brillo bajo.',
            fr: 'Réduit radios et RAM : Swarm désactivé, voix désactivée, cerveau hors ligne désactivé, luminosité basse.',
            de: 'Spart Funk und Arbeitsspeicher: Swarm aus, Sprache aus, Offline-Gehirn aus, niedrige Helligkeit.', zh: '减少无线和 RAM 占用：Swarm 关，语音关，离线大脑关，低亮度。' },
    patch: { power: { display_brightness: 30 }, network: { swarm: { enabled: false } }, voice: { alwaysOn: false } },
    apiCalls: [
      { path: '/api/voice/always', method: 'POST', body: { on: false } },
      { path: '/api/anima/l1',     method: 'POST', body: { mode: 'off' } },
    ],
  },
  {
    id: 'demo', icon: '🎤', it: 'Presentazione', en: 'Demo', es: 'Presentación', fr: 'Présentation', de: 'Präsentation', zh: '演示',
    desc: { it: 'Schermo acceso e luminoso, tema scuro, cervello offline in automatico.',
            en: 'Screen on and bright, dark theme, offline brain on auto.',
            es: 'Pantalla encendida y brillante, tema oscuro, cerebro offline en automático.',
            fr: 'Écran allumé et lumineux, thème sombre, cerveau hors ligne en automatique.',
            de: 'Bildschirm an und hell, dunkles Design, Offline-Gehirn auf Automatik.', zh: '屏幕开启并调亮，深色主题，离线大脑设为自动。' },
    patch: { power: { display_brightness: 100 }, ui: { theme: 'dark' } },
    apiCalls: [
      { path: '/api/display',  method: 'POST', query: { on: 1 } },
      { path: '/api/anima/l1', method: 'POST', body: { mode: 'auto' } },
    ],
  },
  // ── Handheld (live brightness / volume) ──
  {
    id: 'night', icon: '🌙', it: 'Notte', en: 'Night', es: 'Noche', fr: 'Nuit', de: 'Nacht', zh: '夜间',
    desc: { it: 'Schermo al 15% e volume al 25%: per giocare al buio senza svegliare nessuno.',
            en: 'Screen at 15% and volume at 25%: for playing in the dark without waking anyone.',
            es: 'Pantalla al 15% y volumen al 25%: para jugar a oscuras sin despertar a nadie.',
            fr: 'Écran à 15 % et volume à 25 % : pour jouer dans le noir sans réveiller personne.',
            de: 'Bildschirm auf 15 % und Lautstärke auf 25 %: zum Spielen im Dunkeln, ohne jemanden zu wecken.', zh: '屏幕 15%，音量 25%：在黑暗中玩游戏，不吵醒任何人。' },
    patch: {},
    apiCalls: [level('/api/brightness', 15), level('/api/volume', 25)],
  },
  {
    id: 'saver', icon: '🔋', it: 'Risparmio', en: 'Battery Saver', es: 'Ahorro', fr: 'Économie', de: 'Energiesparen', zh: '省电',
    desc: { it: 'Luminosità al 30%: lo schermo è ciò che consuma di più.',
            en: 'Brightness at 30%: the screen is what drains the most.',
            es: 'Brillo al 30%: la pantalla es lo que más consume.',
            fr: 'Luminosité à 30 % : l’écran est ce qui consomme le plus.',
            de: 'Helligkeit auf 30 %: Der Bildschirm verbraucht am meisten.', zh: '亮度 30%：屏幕是最耗电的部分。' },
    patch: {},
    apiCalls: [level('/api/brightness', 30)],
  },
  {
    id: 'sun', icon: '☀️', it: 'Pieno sole', en: 'Full sun', es: 'Pleno sol', fr: 'Plein soleil', de: 'Volle Sonne', zh: '烈日',
    desc: { it: 'Luminosità al massimo e volume al 70%: per giocare all’aperto.',
            en: 'Maximum brightness and volume at 70%: for playing outdoors.',
            es: 'Brillo al máximo y volumen al 70%: para jugar al aire libre.',
            fr: 'Luminosité au maximum et volume à 70 % : pour jouer en plein air.',
            de: 'Maximale Helligkeit und Lautstärke auf 70 %: zum Spielen im Freien.', zh: '最高亮度，音量 70%：适合在户外玩游戏。' },
    patch: {},
    apiCalls: [level('/api/brightness', 100), level('/api/volume', 70)],
  },
  {
    id: 'diagnostics', icon: '🩺', it: 'Diagnostica', en: 'Diagnostics', es: 'Diagnóstico', fr: 'Diagnostic', de: 'Diagnose', zh: '诊断',
    desc: { it: 'Esegue il self-test del dispositivo e apre la Diagnostica.',
            en: 'Runs the device self-test and opens Diagnostics.',
            es: 'Ejecuta el autodiagnóstico del dispositivo y abre Diagnóstico.',
            fr: 'Lance l’autotest de l’appareil et ouvre Diagnostic.',
            de: 'Führt den Selbsttest des Geräts aus und öffnet die Diagnose.', zh: '运行设备自检并打开「诊断」。' },
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
