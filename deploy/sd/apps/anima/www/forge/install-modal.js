// install-modal.js — the BLOCKING install UI for an offline model, plus the OS-wide scrim bridge.
//
// While a model downloads the user may do exactly two things: WATCH it or CANCEL it. This modal enforces
// that inside the ANIMA app (a full-window overlay that eats every pointer/key event); and, when ANIMA runs
// inside the NucleoOS shell, it tells the shell to raise THIS window above a full-desktop scrim — so the
// taskbar, Start menu and every other window are unreachable too. The whole OS is "wait or cancel".
//
// It owns no download logic: it renders the events install-flow.js emits and resolves Cancel/Retry/Close.
// startInstall() wires the real model-store + the OS-wide dlgate lock to the pure driver and returns a
// promise that settles when the model is installed, cancelled, or abandoned.

import { installModel } from './install-flow.js';
import { humanBytes, etaSeconds, etaText } from './install-flow.js';

const EN = (lang) => lang === 'en';

// One-time scoped stylesheet (theme-aware via the app's CSS vars). id-guarded so re-opens don't pile up.
function ensureStyle() {
  if (document.getElementById('nfi-style')) return;
  const s = document.createElement('style'); s.id = 'nfi-style';
  s.textContent = `
  .nfi-scrim{position:fixed;inset:0;z-index:2147483600;display:flex;align-items:center;justify-content:center;
    background:rgba(4,8,18,.72);backdrop-filter:blur(8px) saturate(1.3);animation:nfi-fade .18s ease;padding:18px}
  @keyframes nfi-fade{from{opacity:0}to{opacity:1}}
  .nfi-card{width:min(520px,94vw);background:var(--panel,#18233a);color:var(--text,#e8eefc);
    border:1px solid var(--line,#2a3a5e);border-radius:16px;box-shadow:0 30px 80px #000b;overflow:hidden;font:14px/1.5 system-ui,sans-serif}
  .nfi-hd{display:flex;gap:13px;align-items:center;padding:18px 20px;border-bottom:1px solid var(--line,#2a3a5e)}
  .nfi-ic{width:40px;height:40px;flex:0 0 auto;display:grid;place-items:center;border-radius:11px;
    background:color-mix(in srgb,var(--accent,#4ea1ff) 22%,transparent);font-size:20px}
  .nfi-title{font-weight:700;font-size:15px}
  .nfi-sub{color:var(--muted,#8aa0c8);font-size:12.5px;margin-top:2px}
  .nfi-body{padding:18px 20px;display:flex;flex-direction:column;gap:12px}
  .nfi-bar{height:10px;border-radius:6px;background:var(--field,#0b1220);overflow:hidden;border:1px solid var(--line,#2a3a5e)}
  .nfi-bar>i{display:block;height:100%;width:0;border-radius:6px;background:linear-gradient(90deg,var(--accent,#4ea1ff),#7ec8ff);
    transition:width .3s ease}
  .nfi-bar.indet>i{width:35%;animation:nfi-slide 1.2s ease-in-out infinite}
  @keyframes nfi-slide{0%{margin-left:-35%}100%{margin-left:100%}}
  .nfi-stat{display:flex;justify-content:space-between;gap:10px;font:12.5px var(--mono,ui-monospace,monospace);color:var(--muted,#8aa0c8);flex-wrap:wrap}
  .nfi-phase{color:var(--text,#e8eefc);font-weight:600}
  .nfi-src{font-size:12px;color:var(--muted,#8aa0c8)}
  .nfi-msg{font-size:13px;line-height:1.5;padding:11px 13px;border-radius:10px;background:var(--field,#0b1220);
    border:1px solid var(--line,#2a3a5e);display:none}
  .nfi-msg.show{display:block}
  .nfi-msg.err{border-color:#f8727255;background:#f872721a}
  .nfi-msg b{display:block;margin-bottom:3px}
  .nfi-note{font-size:11.5px;color:var(--muted,#8aa0c8);opacity:.85}
  .nfi-ft{display:flex;gap:10px;justify-content:flex-end;padding:14px 20px;border-top:1px solid var(--line,#2a3a5e);background:var(--panel2,#1f2d4a)}
  .nfi-ft button{padding:8px 16px;border-radius:9px;border:1px solid var(--line,#2a3a5e);background:var(--field,#0b1220);
    color:var(--text,#e8eefc);font:600 13px system-ui;cursor:pointer}
  .nfi-ft button.pri{background:var(--accent,#4ea1ff);border-color:transparent;color:#04132b}
  .nfi-ft button:hover{filter:brightness(1.12)}
  .nfi-ft button[disabled]{opacity:.5;cursor:default}`;
  document.head.appendChild(s);
}

// Tell the shell (if we're inside it) to block the rest of the OS behind a scrim and float THIS window
// on top. Best-effort: standalone ANIMA (no shell) just relies on the in-app overlay.
function tellShell(state, label) {
  try { if (window.parent && window.parent !== window) window.parent.postMessage({ type: 'os-install-modal', state, label }, '*'); } catch {}
}

// Build the modal + an object implementing the `ui` surface install-flow expects.
function makeModal(label, lang) {
  ensureStyle();
  const en = EN(lang);
  const el = (t, c, txt) => { const e = document.createElement(t); if (c) e.className = c; if (txt != null) e.textContent = txt; return e; };

  const scrim = el('div', 'nfi-scrim');
  const card = el('div', 'nfi-card');
  // header
  const hd = el('div', 'nfi-hd');
  hd.append(el('div', 'nfi-ic', '⬇'));
  const hdtx = el('div'); hdtx.append(el('div', 'nfi-title', en ? 'Installing offline model' : 'Installazione modello offline'), el('div', 'nfi-sub', label));
  hd.append(hdtx);
  // body
  const body = el('div', 'nfi-body');
  const bar = el('div', 'nfi-bar indet'); const fill = el('i'); bar.append(fill);
  const stat = el('div', 'nfi-stat');
  const phaseEl = el('span', 'nfi-phase', en ? 'Starting…' : 'Avvio…');
  const bytesEl = el('span', 'nfi-bytes', '');
  const etaEl = el('span', 'nfi-eta', '');
  stat.append(phaseEl, bytesEl, etaEl);
  const srcEl = el('div', 'nfi-src', '');
  const msgEl = el('div', 'nfi-msg');
  const note = el('div', 'nfi-note', en
    ? 'Downloaded parts are checksum-verified and kept — if the connection drops it resumes by itself. The rest of NucleoOS is paused until this finishes or you cancel.'
    : 'Le parti scaricate sono verificate (checksum) e conservate — se la connessione cade riprende da sola. Il resto di NucleoOS è in pausa finché non finisce o annulli.');
  body.append(bar, stat, srcEl, msgEl, note);
  // footer
  const ft = el('div', 'nfi-ft');
  const cancelBtn = el('button', '', en ? 'Cancel' : 'Annulla');
  const retryBtn = el('button', 'pri', en ? 'Retry' : 'Riprova'); retryBtn.hidden = true;
  const closeBtn = el('button', '', en ? 'Close' : 'Chiudi'); closeBtn.hidden = true;
  ft.append(cancelBtn, retryBtn, closeBtn);
  card.append(hd, body, ft); scrim.append(card);

  // swallow stray clicks/keys on the backdrop so nothing behind reacts.
  scrim.addEventListener('click', (e) => { if (e.target === scrim) e.stopPropagation(); });

  const srcLabel = (s) => s === 'sd' ? (en ? 'from the Cardputer SD' : 'dalla SD del Cardputer') : s === 'cdn' ? (en ? 'from the internet (CDN)' : 'da Internet (CDN)') : '';
  const setBar = (pct) => { if (pct == null || !Number.isFinite(pct)) { bar.classList.add('indet'); fill.style.width = ''; } else { bar.classList.remove('indet'); fill.style.width = Math.max(2, pct) + '%'; } };

  // rate/ETA estimator over a short trailing window
  let samples = []; let lastBytesTotal = 0;
  const rate = (bytesDone) => {
    const now = Date.now(); samples.push({ t: now, b: bytesDone }); samples = samples.filter((s) => now - s.t <= 4000);
    if (samples.length < 2) return 0; const a = samples[0], z = samples[samples.length - 1];
    const dt = (z.t - a.t) / 1000; return dt > 0 ? Math.max(0, (z.b - a.b) / dt) : 0;
  };

  let cancelCb = null, rcTimer = null;
  const clearRc = () => { if (rcTimer) { clearInterval(rcTimer); rcTimer = null; } };

  const ui = {
    el: scrim, label,
    onCancel(cb) { cancelCb = cb; },
    setPhase(name) {
      clearRc();
      phaseEl.textContent = name === 'resuming' ? (en ? 'Resuming…' : 'Ripresa…') : (en ? 'Starting…' : 'Avvio…');
      msgEl.classList.remove('show', 'err'); setBar(null);
    },
    onProgress(p) {
      if (!p) return;
      if (p.phase === 'fallback') {
        phaseEl.textContent = en ? 'Source unreachable → trying the Cardputer SD…' : 'Sorgente non raggiungibile → provo la SD del Cardputer…';
        return;
      }
      if (p.phase === 'verifying') { phaseEl.textContent = en ? 'Verifying integrity (SHA-256)…' : 'Verifica integrità (SHA-256)…'; return; }
      if (p.phase === 'done') { return; }
      // 'progress'
      clearRc(); msgEl.classList.remove('show', 'err');
      if (Number.isFinite(p.pct)) setBar(p.pct); else setBar(null);
      phaseEl.textContent = en ? 'Downloading…' : 'Scaricamento…';
      if (p.source) srcEl.textContent = (en ? 'Source: ' : 'Sorgente: ') + srcLabel(p.source);
      if (Number.isFinite(p.bytesDone) && Number.isFinite(p.bytesTotal) && p.bytesTotal > 0) {
        lastBytesTotal = p.bytesTotal;
        bytesEl.textContent = humanBytes(p.bytesDone) + ' / ' + humanBytes(p.bytesTotal);
        etaEl.textContent = etaText(etaSeconds(p.bytesDone, p.bytesTotal, rate(p.bytesDone)), lang);
      }
    },
    setReconnecting({ attempt, delayMs, msg }) {
      clearRc(); setBar(null);
      msgEl.className = 'nfi-msg show'; msgEl.textContent = ''; const b = el('b', '', msg.title); msgEl.append(b, document.createTextNode(msg.detail));
      let left = Math.ceil((delayMs || 0) / 1000);
      const paint = () => { phaseEl.textContent = (en ? 'Reconnecting in ' : 'Riconnessione tra ') + left + 's… (' + (en ? 'attempt ' : 'tentativo ') + attempt + ')'; };
      paint(); rcTimer = setInterval(() => { left = Math.max(0, left - 1); paint(); if (left <= 0) clearRc(); }, 1000);
    },
    setError(e) {
      clearRc(); setBar(0); bar.style.display = 'none';
      phaseEl.textContent = en ? 'Stopped' : 'Interrotto';
      bytesEl.textContent = ''; etaEl.textContent = ''; srcEl.textContent = '';
      msgEl.className = 'nfi-msg show err'; msgEl.textContent = ''; msgEl.append(el('b', '', e.title), document.createTextNode(e.detail));
      cancelBtn.hidden = true; retryBtn.hidden = !e.canRetry; closeBtn.hidden = false;
    },
    setCancelled() {
      clearRc(); phaseEl.textContent = en ? 'Cancelled' : 'Annullato';
      msgEl.className = 'nfi-msg show'; msgEl.textContent = en
        ? 'Stopped. The parts already downloaded are kept — installing again later resumes from here.'
        : 'Interrotto. Le parti già scaricate sono conservate — reinstallando più tardi riprende da qui.';
      bar.style.display = 'none'; bytesEl.textContent = ''; etaEl.textContent = '';
      cancelBtn.hidden = true; retryBtn.hidden = true; closeBtn.hidden = false;
    },
    setDone(r) {
      clearRc(); setBar(100); phaseEl.textContent = en ? '✓ Installed — runs offline now' : '✓ Installato — ora funziona offline';
      srcEl.textContent = r && r.source ? ((en ? 'Fetched ' : 'Scaricato ') + srcLabel(r.source)) : '';
      bytesEl.textContent = ''; etaEl.textContent = '';
      msgEl.classList.remove('show', 'err');
      cancelBtn.hidden = true; retryBtn.hidden = true; closeBtn.hidden = false; closeBtn.classList.add('pri');
    },
    // promise plumbing for the footer buttons
    _wire(onCancel, onRetry, onClose) {
      cancelBtn.addEventListener('click', () => { cancelBtn.disabled = true; if (cancelCb) cancelCb(); if (onCancel) onCancel(); });
      retryBtn.addEventListener('click', () => onRetry && onRetry());
      closeBtn.addEventListener('click', () => onClose && onClose());
    },
    mount() { document.body.appendChild(scrim); tellShell('open', label); },
    destroy() { clearRc(); tellShell('close', label); try { scrim.remove(); } catch {} },
  };
  return ui;
}

// startInstall — the one call the app makes. Returns { ok, source } | { ok:false, reason }.
//   model : a registry entry { id, kind, label, manifest? }
//   caps  : { webgpu, wasm, online }   sizeText derived from the manifest when present
export async function startInstall({ store, model, caps, lang = 'it', dlLock }) {
  const sizeText = model.manifest && model.manifest.totalBytes ? humanBytes(model.manifest.totalBytes) : '';
  const ui = makeModal(model.label || model.id, lang);

  // guard against abandoning silently: if the tab/window goes away mid-install, the AbortController in the
  // driver still tears down (the fetch aborts); we just make sure the scrim is reported closed to the shell.
  const onHide = () => { try { tellShell('close', model.label || model.id); } catch {} };
  window.addEventListener('pagehide', onHide, { once: true });

  ui.mount();
  try {
    for (;;) {
      // Wait for the user's footer choice in parallel with the run; the driver's own onCancel handles aborting.
      let resolveChoice; const choice = new Promise((r) => { resolveChoice = r; });
      ui._wire(/*cancel*/() => {}, /*retry*/() => resolveChoice('retry'), /*close*/() => resolveChoice('close'));

      const res = await installModel({ store, modelId: model.id, kind: model.kind, caps, sizeText, lang, ui, dlLock });

      if (res.ok) { ui.setDone(res); await waitClose(choice, resolveChoice, 1800); return res; }
      if (res.reason === 'cancelled') { await waitClose(choice, resolveChoice, 1500); return res; }   // show "kept parts", then dismiss
      // fatal: setError already rendered Retry/Close — wait for the user.
      const pick = await choice;
      if (pick !== 'retry') return res;          // close → give up
      // retry: fresh modal state, loop.
    }
  } finally {
    window.removeEventListener('pagehide', onHide);
    ui.destroy();
  }
}

// Resolve when the user clicks Close, or after `autoMs` (0 = wait for the click only).
function waitClose(choice, resolveChoice, autoMs) {
  if (autoMs > 0) setTimeout(() => resolveChoice('close'), autoMs);
  return choice;
}
