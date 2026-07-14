'use strict';
function $(id) { return document.getElementById(id); }

// i18n: the bootstrap module (in index.html) sets window.__t once the engine resolves.
// Until then (and as a safety net) fall back to the literal IT string passed in.
function t(key, args, fallback) {
  if (typeof window.__t === 'function') return window.__t(key, args);
  return fallback != null ? fallback : key;
}

const MAX_TPLS = 20;

// Curriculum — every phrase maps to a REAL intent ANIMA resolves (app aliases in
// registry/app-aliases.json, or a built-in system/skill query). Training a phrase
// stores one template; saying it (one breath) launches the action.
const CURRICULUM = [
  { cat: "🚀 Avvia app", catKey: "cat_apps", items: [
    "apri musica", "apri radio", "apri video", "apri registratore",
    "apri calcolatrice", "apri calendario", "apri orologio", "apri note",
    "apri file", "apri impostazioni", "apri terminale", "apri giochi",
    "apri browser", "apri foto", "apri monitor"
  ]},
  { cat: "📊 Sistema", catKey: "cat_system", items: [
    "che ore sono", "livello batteria", "spazio libero", "data di oggi"
  ]},
  { cat: "🌤️ Skill", catKey: "cat_skill", items: [
    "meteo", "agenda di oggi", "cosa sai fare"
  ]},
];

// A distinct, high-value starter set used by the guided tutorial.
const STARTER = ["apri musica", "apri registratore", "che ore sono", "livello batteria", "apri impostazioni", "meteo"];

const ALL_KNOWN = CURRICULUM.flatMap(c => c.items);

let savedTriggers = [];      // labels of trained templates (filename minus .tpl)
let tutStep = 0;
const TUT_STEPS = 5;

// ───────────────────────── Tabs ─────────────────────────
function showTab(tab) {
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
  const panel = $('panel-' + tab);
  if (panel) panel.classList.add('active');
  if (tab === 'commands') renderCurriculum();
  if (tab === 'custom') renderCustom();
  if (tab === 'tutorial') renderStarter();
}

// ───────────────────────── Triggers state ─────────────────────────
async function loadTriggers() {
  try {
    const res = await fetch('/api/fs/list?path=/system/voice');
    const data = await res.json();
    savedTriggers = [];
    if (data.entries) {
      for (const f of data.entries) {
        if (f.name.endsWith('.tpl')) savedTriggers.push(f.name.slice(0, -4));
      }
    }
  } catch (e) {
    savedTriggers = [];
  }
  updateProgress();
  renderCurriculum();
  renderCustom();
  renderStarter();
}

function isSaved(word) { return savedTriggers.includes(word); }

function updateProgress() {
  const n = savedTriggers.length;
  $('prog-label').textContent = t('prog_label', { count: n },
    n === 1 ? '1 comando addestrato' : n + ' comandi addestrati');
  $('prog-cap').textContent = t('prog_cap', { count: MAX_TPLS }, 'max ' + MAX_TPLS);
  $('prog-fill').style.width = Math.min(100, (n / MAX_TPLS) * 100) + '%';
}

function atCap(word) { return !isSaved(word) && savedTriggers.length >= MAX_TPLS; }

// ───────────────────────── Curriculum render ─────────────────────────
function trainBtn(word) {
  if (isSaved(word)) {
    return `<div class="trigger-actions">
      <span class="badge-ok">🟢 <span data-i18n="badge_trained">Addestrato</span></span>
      <button class="act" onclick="startRecord('${esc(word)}')">↻</button>
      <button class="act del" onclick="deleteTrigger('${esc(word)}')" data-i18n="remove">Rimuovi</button></div>`;
  }
  if (atCap(word)) {
    return `<div class="trigger-actions"><span style="font-size:12px;color:var(--text-muted)" data-i18n="cap_reached">limite raggiunto</span></div>`;
  }
  return `<div class="trigger-actions"><button class="act" onclick="startRecord('${esc(word)}')">🎙️ <span data-i18n="btn_train">Addestra</span></button></div>`;
}

function renderCurriculum() {
  let html = '';
  for (const c of CURRICULUM) {
    html += `<div class="category-title" data-i18n="${c.catKey}">${c.cat}</div>`;
    for (const item of c.items) {
      html += `<div class="trigger-item ${isSaved(item) ? 'saved' : ''}">
        <span class="trigger-name">${item}</span>${trainBtn(item)}</div>`;
    }
  }
  $('curriculum-list').innerHTML = html;
}

function renderCustom() {
  const custom = savedTriggers.filter(w => !ALL_KNOWN.includes(w));
  let html = '';
  for (const item of custom) {
    html += `<div class="trigger-item saved"><span class="trigger-name">${item}</span>
      <div class="trigger-actions">
        <button class="act" onclick="startRecord('${esc(item)}')">↻</button>
        <button class="act del" onclick="deleteTrigger('${esc(item)}')" data-i18n="delete">Elimina</button></div></div>`;
  }
  if (!html) html = '<div class="empty" data-i18n="custom_empty">Nessun comando custom salvato.</div>';
  $('custom-trigger-list').innerHTML = html;
}

function renderStarter() {
  let html = '';
  for (const w of STARTER) {
    html += `<div class="trigger-item ${isSaved(w) ? 'saved' : ''}"><span class="trigger-name">${w}</span>${trainBtn(w)}</div>`;
  }
  const el = $('starter-list');
  if (el) el.innerHTML = html;
}

function esc(s) { return s.replace(/'/g, "\\'"); }

async function deleteTrigger(word) {
  if (!confirm(t('confirm_delete', { word }, `Eliminare il comando “${word}”?`))) return;
  try {
    const res = await fetch(`/api/fs/delete?path=/system/voice/${encodeURIComponent(word)}.tpl`, { method: 'POST' });
    const data = await res.json();
    if (data.ok) loadTriggers();
    else alert(t('err_delete', null, "Errore durante l'eliminazione"));
  } catch (e) { alert(t('err_network', null, 'Errore di rete')); }
}

// ───────────────────────── Recording wizard ─────────────────────────
let recordingWord = '';
let onRecordDone = null;     // optional callback after a successful save (tutorial chaining)
let recTimer = null;         // "no response from device" safety timeout
function clearRecTimer() { if (recTimer) { clearTimeout(recTimer); recTimer = null; } }
function recOpen() { return $('panel-record').classList.contains('active') && $('rec-step').style.display !== 'none'; }

function startRecord(word, doneCb) {
  if (atCap(word)) { alert(t('alert_cap', { max: MAX_TPLS }, `Hai raggiunto il limite di ${MAX_TPLS} comandi. Rimuovine uno prima.`)); return; }
  recordingWord = word;
  onRecordDone = doneCb || null;
  $('rec-step').style.display = 'block';
  $('ok-step').style.display = 'none';
  $('display-word').textContent = word;
  $('rec-status').textContent = t('rec_waiting', null, 'In attesa del Cardputer…');
  $('rec-timeout-hint').style.display = 'none';
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  $('panel-record').classList.add('active');
  clearRecTimer();
  recTimer = setTimeout(() => { if (recOpen()) $('rec-timeout-hint').style.display = 'block'; }, 18000);
  armEngine(word);
}

function startCustomRecord() {
  const word = $('trigger-word').value.trim().toLowerCase();
  if (!word) return;
  $('trigger-word').value = '';
  $('btn-start-custom').disabled = true;
  startRecord(word);
}

function checkInput() {
  $('btn-start-custom').disabled = $('trigger-word').value.trim().length === 0;
}

async function armEngine(word) {
  try {
    const res = await fetch('/api/voice/learn', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ word })
    });
    const data = await res.json();
    if (!data.ok) { alert(t('err_arm', null, "Impossibile armare il motore (dispositivo occupato?)")); cancelRecord(); }
  } catch (e) { alert(t('err_connect', null, 'Errore di connessione al Cardputer')); cancelRecord(); }
}

function cancelRecord() {
  clearRecTimer();
  onRecordDone = null;
  showTab(currentBaseTab());
}

// Which non-overlay tab to return to after recording.
let lastBaseTab = 'tutorial';
function currentBaseTab() { return lastBaseTab; }

function onLearned(ok) {
  if (!$('panel-record').classList.contains('active')) return;
  clearRecTimer();
  if (!ok) {
    $('rec-status').textContent = t('rec_too_short', null, '⚠️ Troppo corto o silenzio — riprova: tieni FN e parla chiaro.');
    $('rec-timeout-hint').style.display = 'none';
    return;
  }
  $('rec-step').style.display = 'none';
  $('ok-step').style.display = 'block';
  $('ok-msg').textContent = t('ok_msg', { word: recordingWord }, `“${recordingWord}” registrato con la tua voce.`);
  loadTriggers();

  const cb = onRecordDone; onRecordDone = null;
  const btn = $('ok-next');
  btn.textContent = cb ? t('ok_next_cmd', null, 'Prossimo comando →') : t('continue', null, 'Continua');
  btn.onclick = () => { if (cb) cb(); else showTab(currentBaseTab()); };
}

// ───────────────────────── Tutorial ─────────────────────────
function renderStepper() {
  let html = '';
  for (let i = 0; i < TUT_STEPS; i++)
    html += `<div class="step-dot ${i < tutStep ? 'done' : (i === tutStep ? 'cur' : '')}"></div>`;
  $('stepper').innerHTML = html;
}
function gotoStep(n) {
  tutStep = Math.max(0, Math.min(TUT_STEPS - 1, n));
  document.querySelectorAll('.tut-step').forEach(s => s.classList.toggle('active', +s.dataset.step === tutStep));
  renderStepper();
}
function tutNext() { gotoStep(tutStep + 1); }
function tutPrev() { gotoStep(tutStep - 1); }

// ───────────────────────── Live recognition feed (Prova) ─────────────────────────
let matchCount = 0;
function classify(dist, second, radius) {
  const margin = second > 0 ? dist / second : 1;        // lower = clearer winner
  const ratio  = radius > 0 ? dist / radius : 1;        // <1 = inside radius
  if (ratio <= 0.6 || margin <= 0.45) return { label: t('conf_excellent', null, 'Ottimo'), color: '#18e06a', pct: 92 };
  if (ratio <= 1.0 || margin <= 0.65) return { label: t('conf_good', null, 'Buono'),  color: '#7ad32f', pct: 72 };
  if (margin <= 0.75)                 return { label: t('conf_fair', null, 'Discreto'), color: '#fd20', pct: 48 };
  return { label: t('conf_weak', null, 'Debole'), color: '#ff6b3d', pct: 25 };
}

function onMatch(m) {
  const c = classify(m.dist, m.second, m.radius);
  const feed = $('match-feed');
  if (matchCount === 0) feed.innerHTML = '';
  matchCount++;
  const card = document.createElement('div');
  card.className = 'match-card';
  card.innerHTML = `
    <div class="match-head">
      <span class="match-word">${m.word}</span>
      <span class="conf-label" style="background:${c.color}22;color:${c.color}">${c.label}</span>
    </div>
    <div class="conf-bar"><div class="conf-fill" style="width:${c.pct}%;background:${c.color}"></div></div>
    <div class="match-meta"><span>${t('meta_dist', null, 'dist')} ${m.dist}</span><span>${t('meta_second', null, '2°')}: ${m.second}</span><span>${t('meta_radius', null, 'raggio')} ${m.radius}</span></div>`;
  feed.insertBefore(card, feed.firstChild);
  while (feed.children.length > 12) feed.removeChild(feed.lastChild);
}

// ───────────────────────── WebSocket ─────────────────────────
let ws;
function setWs(on) {
  $('ws-dot').classList.toggle('on', on);
  $('ws-txt').textContent = on ? t('connected', null, 'Connesso') : t('offline', null, 'Offline');
}
function setListening(on) {
  $('listen-dot').classList.toggle('live', on);
  $('listen-dot').classList.toggle('on', false);
  // Wizard feedback: confirm the device actually heard the FN press and is capturing.
  if (recOpen()) {
    if (on) {
      $('rec-status').textContent = t('rec_recording', null, '🔴 Registrazione in corso… rilascia FN per salvare.');
      $('rec-timeout-hint').style.display = 'none';
      clearRecTimer();
    } else {
      $('rec-status').textContent = t('rec_processing', null, 'Elaborazione…');
    }
  }
}
// Route one bus event to the UI. Source-agnostic: a raw WS frame (standalone) or a frame the shell
// forwarded to our iframe (embedded) both land here.
function handleVoiceEvent(t, dRaw) {
  const d = (typeof dRaw === 'string') ? safeJSON(dRaw) : dRaw;
  if (t === 'voice/learned') onLearned(d && d.status === 'ok');
  else if (t === 'voice/match' && d) onMatch(d);
  else if (t === 'voice/state' && d) setListening(!!d.listening);
}

const EMBEDDED = window.top !== window.self;
function connectWS() {
  // The firmware /ws is single-client (last-wins eviction). Inside the shell, the desktop already
  // holds that socket AND forwards every bus event to our iframe — opening our OWN /ws here just
  // fought the shell for the slot, an endless reconnect ping-pong that dropped events on both ends.
  // Embedded → consume the forwarded events; standalone → own the socket.
  if (EMBEDDED) {
    setWs(true);   // reachability is the shell's WS; it forwards voice/* to us
    window.addEventListener('message', (e) => { const m = e.data; if (m && m.t) handleVoiceEvent(m.t, m.d); });
    return;
  }
  try { ws = new WebSocket(`ws://${location.host}/ws`); }
  catch (e) { setWs(false); setTimeout(connectWS, 2500); return; }
  ws.onopen = () => { setWs(true); ws.send(JSON.stringify({ op: 'subscribe', since: 0 })); };
  ws.onclose = () => { setWs(false); setTimeout(connectWS, 2500); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch (_) { return; }
    handleVoiceEvent(msg.t, msg.d);
  };
}
function safeJSON(s) { try { return JSON.parse(s); } catch (_) { return null; } }

// Re-run purely-imperative redraws after a live language change. The DOM
// [data-i18n] nodes are handled by the engine's observer; this only covers
// strings the app writes via textContent (progress label, connection status).
window.redrawI18n = function () {
  updateProgress();
  setWs(!!(ws && ws.readyState === 1));
};

// ───────────────────────── Init ─────────────────────────
window.addEventListener('DOMContentLoaded', () => {
  // track the last real tab so the overlay can return to it
  document.querySelectorAll('.tab-btn').forEach(b =>
    b.addEventListener('click', () => { lastBaseTab = b.dataset.tab; }));
  renderStepper();
  gotoStep(0);
  loadTriggers();
  connectWS();
});
