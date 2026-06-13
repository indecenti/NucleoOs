// NucleoOS AI Copilot — ANIMA as a system service, not just an app.
//
// A floating command/AI bar summonable from anywhere (Ctrl+Space or the taskbar button).
// It talks to the SAME on-device engine the ANIMA app uses (GET /api/anima) and translates
// the returned typed action contract — {action,tool,arg,content,reply,intent,trace,...} —
// into real OS effects, IN-PROCESS, via the shell API the shell hands us (WM.open, openFile,
// showToast, FsIndex). No framework, no new firmware route: the engine already executes the
// side-effecting tools (create_file/add_event) server-side under the pairing gate, exactly as
// it does for the ANIMA app; here we additionally wire launch/open_file straight to the shell.
//
// Settings (language/mode) are shared with the ANIMA app via the same localStorage keys, so
// the two surfaces stay in sync.

let api = null;                       // { byId, WM, openFile, showToast, refreshStatus, FsIndex }
let scrim, root, logEl, inputEl, sendBtn, dotEl, subEl, modeBtn, langBtn, tbBtn;
let isOpen = false, busy = false, aborter = null, seq = 0, elapsedTimer = null;
let history = [];                     // in-memory transcript for this session: [{role,text,r?}]

// ---- i18n (kept tiny; mirrors the ANIMA app's tone) ----
const STR = {
  it: {
    sub: 'copilot', placeholder: 'Chiedi qualcosa o dai un comando…',
    welLead: 'Sono ANIMA, l’assistente di NucleoOS. Apro app, creo file, do ora/meteo/spazio, gestisco il calendario e rispondo a domande — da qui, ovunque tu sia nell’OS.',
    examples: ['che ore sono', 'meteo brescia', 'apri la musica', 'crea una nota'],
    offline: 'Non riesco a raggiungere il motore ANIMA sul dispositivo.',
    stopped: 'Interrotto.', dontknow: 'Non lo so ancora. Posso aprire app, dare ora/spazio/meteo, creare file e rispondere a domande.',
    opened: 'aperto', thinking: ['Sto pensando', 'Rifletto', 'Elaboro', 'Consulto la memoria', 'Sto cercando'],
    reveal: 'Mostra nella cartella', openCal: 'Apri Calendario', openMon: 'Apri System Monitor', openSet: 'Apri Impostazioni',
    nomatch: 'Nessuna corrispondenza.', memory: 'memoria',
    footHint: ['<kbd>⏎</kbd> invia', '<kbd>⇧⏎</kbd> a capo', '<kbd>esc</kbd> chiudi'],
  },
  en: {
    sub: 'copilot', placeholder: 'Ask anything or give a command…',
    welLead: 'I’m ANIMA, NucleoOS’s assistant. I open apps, create files, give time/weather/space, manage the calendar and answer questions — from here, anywhere in the OS.',
    examples: ['what time is it', 'weather london', 'open music', 'create a note'],
    offline: "I can't reach the ANIMA engine on the device.",
    stopped: 'Stopped.', dontknow: "I don't know yet. I can open apps, give time/space/weather, create files and answer questions.",
    opened: 'opened', thinking: ['Thinking', 'Pondering', 'Reasoning', 'Recalling', 'Searching'],
    reveal: 'Reveal in folder', openCal: 'Open Calendar', openMon: 'Open System Monitor', openSet: 'Open Settings',
    nomatch: 'No match.', memory: 'memory',
    footHint: ['<kbd>⏎</kbd> send', '<kbd>⇧⏎</kbd> newline', '<kbd>esc</kbd> close'],
  },
};
const lang = () => (localStorage.getItem('anima.lang') === 'en' ? 'en' : 'it');
const mode = () => { const m = localStorage.getItem('anima.mode'); return ['off', 'on', 'only'].includes(m) ? m : 'on'; };
const T = () => STR[lang()];
const modeLabel = () => ({ off: lang() === 'en' ? 'offline' : 'offline', on: lang() === 'en' ? 'hybrid' : 'ibrida', only: lang() === 'en' ? 'online' : 'online' }[mode()]);

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
// Minimal, XSS-safe inline markdown: escape first, then **bold**, `code`, links.
const mdInline = (text) => esc(text)
  .replace(/`([^`]+)`/g, '<code>$1</code>')
  .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
  .replace(/\bhttps?:\/\/[^\s<]+/g, (m) => `<a href="${m}" target="_blank" rel="noopener noreferrer">${m}</a>`);
const basename = (p) => String(p || '').split('/').pop();
const dirOf = (p) => { const b = basename(p); return String(p || '').slice(0, Math.max(0, String(p).length - b.length - 1)) || '/'; };

// ===========================================================================
export function initCopilot(_api) {
  api = _api;
  buildDom();
  wire();
  return { open: openBar, close: closeBar, toggle, isOpen: () => isOpen, ask: askExternal };
}

function buildDom() {
  scrim = document.createElement('div'); scrim.id = 'copilot-scrim'; scrim.className = 'hidden';
  root = document.createElement('section'); root.id = 'copilot'; root.className = 'hidden';
  root.setAttribute('role', 'dialog'); root.setAttribute('aria-modal', 'true'); root.setAttribute('aria-label', 'ANIMA Copilot');
  root.innerHTML =
    `<div class="cp-head">
       <span class="cp-spark">✻</span>
       <span class="cp-title">ANIMA</span><span class="cp-sub" id="cp-sub">copilot</span>
       <span class="cp-sp"></span>
       <button class="cp-chip" id="cp-mode" title="Modalità motore"></button>
       <button class="cp-chip" id="cp-lang" title="Lingua"></button>
       <span class="cp-dot" id="cp-dot" title="motore ANIMA"></span>
       <button class="cp-x" id="cp-close" aria-label="Chiudi">✕</button>
     </div>
     <div class="cp-inputrow">
       <span class="cp-prompt">›</span>
       <textarea id="cp-q" rows="1" autocomplete="off" spellcheck="false"></textarea>
       <button class="cp-send" id="cp-send" type="button">Invia</button>
     </div>
     <div class="cp-log" id="cp-log" role="log" aria-live="polite" aria-relevant="additions"></div>
     <div class="cp-foot" id="cp-foot"></div>`;
  document.body.appendChild(scrim);
  document.body.appendChild(root);
  logEl = root.querySelector('#cp-log');
  inputEl = root.querySelector('#cp-q');
  sendBtn = root.querySelector('#cp-send');
  dotEl = root.querySelector('#cp-dot');
  subEl = root.querySelector('#cp-sub');
  modeBtn = root.querySelector('#cp-mode');
  langBtn = root.querySelector('#cp-lang');
  tbBtn = document.getElementById('copilot-btn');
}

function wire() {
  scrim.addEventListener('click', closeBar);
  root.querySelector('#cp-close').addEventListener('click', closeBar);
  sendBtn.addEventListener('click', () => (busy ? stop() : submit()));
  if (tbBtn) tbBtn.addEventListener('click', toggle);
  // engine mode / language quick toggles, shared with the ANIMA app
  modeBtn.addEventListener('click', () => { const next = { off: 'on', on: 'only', only: 'off' }[mode()]; localStorage.setItem('anima.mode', next); syncChips(); inputEl.focus(); });
  langBtn.addEventListener('click', () => { localStorage.setItem('anima.lang', lang() === 'it' ? 'en' : 'it'); syncChips(); renderFoot(); if (!history.length) renderWelcome(); inputEl.focus(); });
  inputEl.addEventListener('input', autogrow);
  inputEl.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); submit(); }
    else if (e.key === 'Escape') { e.preventDefault(); if (busy) stop(); else closeBar(); }
  });
  // welcome / clarify chips delegate to ask()
  logEl.addEventListener('click', (e) => { const c = e.target.closest('.cp-chip-q'); if (c) askCopilot(c.dataset.q); });
}

function autogrow() { inputEl.style.height = 'auto'; inputEl.style.height = Math.min(inputEl.scrollHeight, 120) + 'px'; }
function submit() { const q = inputEl.value; inputEl.value = ''; autogrow(); askCopilot(q); }

// ---- open / close ----
function openBar() {
  if (isOpen) return;
  isOpen = true;
  scrim.classList.remove('hidden'); root.classList.remove('hidden');
  if (tbBtn) tbBtn.classList.add('on');
  syncChips(); renderFoot();
  if (!history.length) renderWelcome();
  inputEl.placeholder = T().placeholder;
  setTimeout(() => inputEl.focus(), 30);
  ping();
}
function closeBar() {
  if (!isOpen) return;
  isOpen = false;
  scrim.classList.add('hidden'); root.classList.add('hidden');
  if (tbBtn) tbBtn.classList.remove('on');
  stop();
}
function toggle() { isOpen ? closeBar() : openBar(); }
// Called by the shell (e.g. the "Ask ANIMA" search row): open, then ask.
function askExternal(q) { openBar(); askCopilot(q); }

function syncChips() {
  subEl.textContent = T().sub;
  modeBtn.innerHTML = 'ANIMA · <b>' + esc(modeLabel()) + '</b>';
  langBtn.innerHTML = '<b>' + lang().toUpperCase() + '</b>';
  inputEl.placeholder = T().placeholder;
}
function renderFoot() { root.querySelector('#cp-foot').innerHTML = T().footHint.join(' · '); }

// ---- transcript primitives ----
function el(tag, cls, html) { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; }
function scrollDown() { logEl.scrollTop = logEl.scrollHeight; }

function renderWelcome() {
  logEl.innerHTML = '';
  const w = el('div', 'cp-welcome');
  w.innerHTML = `<span class="lead">${esc(T().welLead)}</span>`;
  const chips = el('div', 'cp-chips');
  for (const ex of T().examples) {
    const b = el('button', 'cp-act cp-chip-q'); b.type = 'button'; b.dataset.q = ex;
    b.innerHTML = `<span class="pfx">›</span>${esc(ex)}`;
    chips.appendChild(b);
  }
  w.appendChild(chips);
  logEl.appendChild(w);
}

function addUser(text) {
  const turn = el('div', 'cp-turn cp-me');
  turn.appendChild(el('div', 'cp-gut', '›'));
  const body = el('div', 'cp-body'); body.textContent = text; turn.appendChild(body);
  logEl.appendChild(turn); scrollDown();
}
function addBot(text) {
  const turn = el('div', 'cp-turn cp-bot');
  turn.appendChild(el('div', 'cp-gut', '⏺'));
  const body = el('div', 'cp-body'); body.innerHTML = mdInline(text); turn.appendChild(body);
  logEl.appendChild(turn); scrollDown();
  return turn;
}
function addLined(turn, text, kind) {
  const l = el('div', 'cp-lined' + (kind ? ' ' + kind : ''));
  l.innerHTML = `<span class="cor">⎿</span><span>${mdInline(text)}</span>`;
  turn.appendChild(l); scrollDown();
}
function addActions(turn, actions) {
  const row = el('div', 'cp-actions');
  for (const a of actions) {
    const b = el('button', 'cp-act'); b.type = 'button';
    b.innerHTML = `<span class="pfx">↗</span>${esc(a.label)}`;
    b.addEventListener('click', a.fn);
    row.appendChild(b);
  }
  turn.appendChild(row); scrollDown();
}
function addClarify(turn, options) {
  const row = el('div', 'cp-chips');
  for (const opt of options) {
    const b = el('button', 'cp-act cp-chip-q'); b.type = 'button'; b.dataset.q = opt;
    b.innerHTML = `<span class="pfx">›</span>${esc(opt)}`;
    row.appendChild(b);
  }
  turn.appendChild(row); scrollDown();
}
function addMeta(turn, r) {
  const tags = [];
  if (r.domain && r.domain !== 'none') tags.push(['dom', r.domain]);
  if (r.confidence) tags.push(['conf', r.confidence + '%']);
  if (r.memory) tags.push(['mem', T().memory]);
  if (!tags.length) return;
  const m = el('div', 'cp-meta');
  m.innerHTML = tags.map(([c, t]) => `<span class="cp-tag ${c}">${esc(t)}</span>`).join('');
  turn.appendChild(m);
}

const SPARK = ['✳', '✶', '✻', '✺', '✸', '✷'];
function addThinking() {
  const turn = el('div', 'cp-turn cp-bot');
  turn.appendChild(el('div', 'cp-gut', ''));
  const verbs = T().thinking; const verb = verbs[Math.floor(history.length) % verbs.length];
  turn.innerHTML = `<div class="cp-gut"></div><div class="cp-think"><span class="sp">✻</span><span class="vb">${esc(verb)}…</span><span class="el">(0s)</span></div>`;
  logEl.appendChild(turn); scrollDown();
  const sp = turn.querySelector('.sp'), elx = turn.querySelector('.el');
  let k = 0, t0 = Date.now();
  clearInterval(elapsedTimer);
  elapsedTimer = setInterval(() => { sp.textContent = SPARK[k++ % SPARK.length]; elx.textContent = '(' + Math.round((Date.now() - t0) / 1000) + 's)'; }, 120);
  return { remove() { clearInterval(elapsedTimer); turn.remove(); } };
}

// ---- engine status dot ----
function setDot(s) { dotEl.className = 'cp-dot' + (s ? ' ' + s : ''); }
async function ping() { try { const r = await fetch('/api/status', { cache: 'no-store' }); setDot(r.ok ? 'ok' : 'err'); } catch { setDot('err'); } }

function setBusy(on) { busy = on; sendBtn.textContent = on ? 'Stop' : (lang() === 'en' ? 'Send' : 'Invia'); sendBtn.classList.toggle('stop', on); }
function stop() { if (aborter) { try { aborter.abort('user'); } catch {} } }

// ---- the ask cycle ----
async function askCopilot(q) {
  q = (q || '').trim();
  if (!q || busy) return;
  if (!isOpen) openBar();
  if (!history.length) logEl.innerHTML = '';      // clear the welcome on first ask
  addUser(q); history.push({ role: 'user', text: q });
  const my = ++seq;
  if (aborter) { try { aborter.abort('superseded'); } catch {} }
  aborter = new AbortController();
  const to = setTimeout(() => { try { aborter.abort('timeout'); } catch {} }, 30000);
  setBusy(true);
  const think = addThinking();
  let r;
  try {
    r = await (await fetch('/api/anima?q=' + encodeURIComponent(q) + '&lang=' + lang() + '&mode=' + mode(), { signal: aborter.signal })).json();
  } catch (e) {
    clearTimeout(to); think.remove();
    if (my !== seq) { setBusy(false); return; }   // a newer query already took over
    const aborted = aborter && aborter.signal && aborter.signal.aborted;
    addBot(aborted ? T().stopped : T().offline);
    if (!aborted) setDot('err');
    history.push({ role: 'bot', text: aborted ? T().stopped : T().offline });
    aborter = null; setBusy(false); inputEl.focus(); return;
  }
  clearTimeout(to);
  if (my !== seq) { think.remove(); setBusy(false); return; }   // stale response — ignore
  aborter = null; think.remove(); setDot('ok');
  const reply = r.reply || T().dontknow;
  const turn = addBot(reply);
  dispatch(r, turn);
  history.push({ role: 'bot', text: reply, r });
  setBusy(false); inputEl.focus();
}

// ---- the action router: map the engine's typed contract to real OS effects ----
function dispatch(r, turn) {
  // multi-step reasoning plan → dim ⎿ steps (Claude-Code style), before the action result
  if (r.trace && r.trace.includes(' > ')) for (const s of r.trace.split(' > ')) addLined(turn, s, 'dim');

  if (r.action === 'launch' && r.arg) {
    const app = api.byId(r.arg);
    if (app) { addLined(turn, `${app.name} ${T().opened}`, 'ok'); api.WM.open(app); closeBar(); return; }
    addLined(turn, r.arg, 'warn');
  } else if (r.action === 'tool' && (r.tool === 'open_file' || r.intent === 'open_file') && r.arg) {
    addLined(turn, basename(r.arg), 'ok'); api.openFile(r.arg); closeBar(); return;
  } else if (r.action === 'tool' && (r.tool === 'create_file' || r.intent === 'create_file')) {
    // The engine already created the file server-side (under the pairing gate). Surface it.
    const made = r.path || (r.state === 'tool' ? r.arg : '');
    const exists = /esiste|exists|già|gia/i.test(reply_(r));
    if (made) addLined(turn, made, exists ? 'warn' : 'ok');
    if (r.path) {
      api.FsIndex.invalidate();                       // SD changed → keep search fresh
      const dir = dirOf(r.path);
      addActions(turn, [{ label: T().reveal, fn: () => { const fc = api.byId('file-commander'); if (fc) { api.WM.open(fc, 'path=' + encodeURIComponent(dir)); closeBar(); } } }]);
    }
  } else if (r.action === 'tool' && (r.tool === 'add_event' || r.intent === 'add_event')) {
    api.FsIndex && api.FsIndex.invalidate && api.FsIndex.invalidate();
    addActions(turn, [{ label: T().openCal, fn: () => { const cal = api.byId('calendar'); if (cal) { api.WM.open(cal); closeBar(); } } }]);
  } else if (r.action === 'system') {
    // Turn live-state answers into clickable destinations.
    if (r.intent === 'agenda') addActions(turn, [{ label: T().openCal, fn: () => { const c = api.byId('calendar'); if (c) { api.WM.open(c); closeBar(); } } }]);
    else if (r.intent === 'storage' || r.intent === 'ram') { api.refreshStatus(); const mon = api.byId('system-monitor'); if (mon) addActions(turn, [{ label: T().openMon, fn: () => { api.WM.open(mon); closeBar(); } }]); }
    else if (r.intent === 'network') { const s = api.byId('settings'); if (s) addActions(turn, [{ label: T().openSet, fn: () => { api.WM.open(s); closeBar(); } }]); }
  } else if (r.intent === 'clarify') {
    // pull the two "…, A o B?" / "… — A or B?" options out of the question text
    const m = reply_(r).match(/[,—–-]\s*([^?]+?)\s+(?:o|or)\s+([^?]+?)\?/i);
    if (m) addClarify(turn, [m[1].trim(), m[2].trim()]);
  } else if (r.action === 'none') {
    // honest miss — leave the dontknow reply, no action
  }
  addMeta(turn, r);
}
const reply_ = (r) => r.reply || '';
