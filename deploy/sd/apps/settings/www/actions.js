// actions.js — the command palette (Ctrl/Cmd+K) action registry + a tiny bilingual fuzzy filter.
//
// One registry powers the palette and the keyboard-driven "command the Cardputer" bar. Each action
// carries an id, IT/EN titles, IT+EN keywords and a `run()` that calls back into the host (nav,
// device API, scene application, reboot, self-test). The host injects those callbacks so this module
// stays pure and unit-testable. `filterActions` ranks title-prefix matches above title-substring
// above keyword matches, so the obvious command floats to the top as you type.

import { SCENES } from './scenes.js';

// ctx: { nav(tabId), api(path,{query,body,method}), reboot(), selfTest(), applyScene(id), toast(msg) }
export function buildActionRegistry(ctx) {
  const a = (id, it, en, kit, ken, section, run) =>
    ({ id, it, en, keywords: { it: kit, en: ken }, section, run });

  const navAction = (tab, it, en, kit, ken) =>
    a('go-' + tab, it, en, kit, ken, 'nav', () => ctx.nav(tab));

  const list = [
    // ── navigation (jump to a tab) ───────────────────────────────────────────
    navAction('control', 'Vai a Centro di Controllo', 'Go to Control Center', ['centro', 'controllo', 'battito', 'pulse'], ['control', 'center', 'pulse', 'dashboard']),
    navAction('display', 'Vai a Schermo', 'Go to Display', ['schermo', 'display', 'luminosita'], ['screen', 'display', 'brightness']),
    navAction('personalization', 'Vai ad Aspetto', 'Go to Appearance', ['aspetto', 'tema', 'colore', 'lingua', 'sfondo'], ['appearance', 'theme', 'accent', 'language', 'wallpaper']),
    navAction('network', 'Vai a Rete', 'Go to Network', ['rete', 'wifi', 'bluetooth', 'swarm', 'ip'], ['network', 'wifi', 'bluetooth', 'swarm', 'ip']),
    navAction('ai', 'Vai a IA / Modelli', 'Go to AI / Models', ['ia', 'anima', 'modello', 'chiave', 'cervello'], ['ai', 'anima', 'model', 'key', 'brain']),
    navAction('device', 'Vai a Dispositivo', 'Go to Device', ['dispositivo', 'nome', 'fuso'], ['device', 'name', 'timezone']),
    navAction('diagnostics', 'Vai a Diagnostica', 'Go to Diagnostics', ['diagnostica', 'log', 'heap', 'cpu'], ['diagnostics', 'logs', 'heap', 'cpu']),
    navAction('about', 'Vai a Info', 'Go to About', ['info', 'versione', 'sistema'], ['about', 'version', 'system']),

    // ── live device actions ──────────────────────────────────────────────────
    a('screen-off', 'Spegni schermo', 'Turn screen off', ['spegni', 'schermo', 'display', 'buio'], ['screen', 'off', 'display', 'blank'], 'action',
      () => ctx.api('/api/display', { method: 'POST', query: { on: 0 } })),
    a('screen-on', 'Accendi schermo', 'Turn screen on', ['accendi', 'schermo', 'display'], ['screen', 'on', 'display', 'wake'], 'action',
      () => ctx.api('/api/display', { method: 'POST', query: { on: 1 } })),
    a('voice-on', 'Voce sempre attiva: ON', 'Voice always-on: ON', ['voce', 'attiva', 'ptt'], ['voice', 'always', 'on'], 'action',
      () => ctx.api('/api/voice/always', { method: 'POST', body: { on: true } })),
    a('voice-off', 'Voce sempre attiva: OFF', 'Voice always-on: OFF', ['voce', 'spegni', 'silenzio'], ['voice', 'always', 'off'], 'action',
      () => ctx.api('/api/voice/always', { method: 'POST', body: { on: false } })),
    a('l1-auto', 'Cervello offline: AUTO', 'Offline brain: AUTO', ['cervello', 'offline', 'l1', 'auto'], ['brain', 'offline', 'l1', 'auto'], 'action',
      () => ctx.api('/api/anima/l1', { method: 'POST', body: { mode: 'auto' } })),
    a('l1-off', 'Cervello offline: OFF (libera RAM)', 'Offline brain: OFF (free RAM)', ['cervello', 'offline', 'l1', 'spegni', 'ram'], ['brain', 'offline', 'l1', 'off', 'ram'], 'action',
      () => ctx.api('/api/anima/l1', { method: 'POST', body: { mode: 'off' } })),
    a('tts-on', 'Voce on-device: ON', 'On-device voice: ON', ['voce', 'parla', 'tts'], ['voice', 'speak', 'tts'], 'action',
      () => ctx.api('/api/tts', { method: 'POST', body: { enabled: true } })),
    a('tts-off', 'Voce on-device: OFF', 'On-device voice: OFF', ['voce', 'muto', 'tts'], ['voice', 'mute', 'tts'], 'action',
      () => ctx.api('/api/tts', { method: 'POST', body: { enabled: false } })),
    a('revoke-others', 'Revoca altre sessioni', 'Revoke other sessions', ['revoca', 'sessioni', 'sicurezza', 'logout', 'associazione'], ['revoke', 'sessions', 'security', 'logout', 'pairing'], 'action',
      () => ctx.api('/api/unpair', { method: 'POST', body: { scope: 'others' } })),
    a('revoke-all', 'Revoca TUTTE le sessioni (ri-associa)', 'Revoke ALL sessions (re-pair)', ['revoca', 'tutte', 'sessioni', 'sicurezza'], ['revoke', 'all', 'sessions', 'security'], 'danger',
      () => ctx.api('/api/unpair', { method: 'POST', body: { scope: 'all' } })),

    // ── orchestration ────────────────────────────────────────────────────────
    a('selftest', 'Esegui Self-Test', 'Run Self-Test', ['test', 'diagnostica', 'battito', 'salute'], ['test', 'selftest', 'health', 'check'], 'action',
      () => ctx.selfTest()),
    a('reboot', 'Riavvia dispositivo', 'Reboot device', ['riavvia', 'reboot', 'reset'], ['reboot', 'restart', 'reset'], 'danger',
      () => ctx.reboot()),
    a('scan-wifi', 'Scansiona reti Wi-Fi', 'Scan Wi-Fi networks', ['scansiona', 'wifi', 'reti'], ['scan', 'wifi', 'networks'], 'action',
      () => { ctx.nav('network'); ctx.scanWifi && ctx.scanWifi(); }),
  ];

  // Scene actions are generated from the single SCENES source so the palette never drifts from them.
  for (const s of SCENES) {
    list.push(a('scene-' + s.id, 'Scena: ' + s.it, 'Scene: ' + s.en,
      ['scena', s.it.toLowerCase()], ['scene', s.en.toLowerCase()], 'scene',
      () => ctx.applyScene(s.id)));
  }
  return list;
}

const norm = (s) => String(s || '').toLowerCase()
  .normalize('NFD').replace(/[̀-ͯ]/g, '');   // strip accents so "luminosità" matches "luminosita"

export function filterActions(list, query, lang = 'it') {
  const q = norm(query).trim();
  if (!q) return list.slice();
  const other = lang === 'it' ? 'en' : 'it';
  const scored = [];
  for (const act of list) {
    const title = norm(act[lang] || act.it);
    const kws = [...(act.keywords?.[lang] || []), ...(act.keywords?.[other] || [])].map(norm);
    let score = -1;
    if (title.startsWith(q)) score = 4;
    else if (title.includes(q)) score = 3;
    else if (kws.some((k) => k.startsWith(q))) score = 2;
    else if (kws.some((k) => k.includes(q))) score = 1;
    if (score >= 0) scored.push({ act, score });
  }
  scored.sort((x, y) => y.score - x.score || (x.act[lang] || x.act.it).localeCompare(y.act[lang] || y.act.it));
  return scored.map((s) => s.act);
}
