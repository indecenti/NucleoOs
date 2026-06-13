// Centro Notifiche di sistema — la spina dorsale web delle notifiche NucleoOS.
// Un contratto, N produttori, 2 superfici (qui il web; il device è il gemello nativo).
// Vedi docs/notify-protocol.md. Caricato pigramente da shell.js come copilot.js.
//
// Tutto è event-driven: nessun polling, nessun /api per keystroke. Le notifiche arrivano
// o da Notify.emit(...) in-process, o dal bus eventi via WebSocket (topic notify.post,
// più il legacy calendar.reminder finché il firmware non è migrato).

const LS_LIST = 'nucleo.notify.list';   // storico (cap 50)
const LS_DND  = 'nucleo.notify.dnd';    // '1' = Non disturbare
const LS_VOL  = 'nucleo.notify.vol';    // 0..100 volume melodia
const LS_QUIET= 'nucleo.notify.quiet';  // 'HH:MM-HH:MM' ore silenziose, vuoto = off
const CAP = 50;

// Default per sorgente: icona + tag leggibile.
const SRC = {
  calendar: { ic: '🔔', tag: 'Calendario' },
  system:   { ic: '⚙️', tag: 'Sistema' },
  anima:    { ic: '✨', tag: 'ANIMA' },
  voice:    { ic: '🎤', tag: 'Voce' },
  recorder: { ic: '🎙️', tag: 'Registratore' },
  ota:      { ic: '⬆️', tag: 'Aggiornamento' },
  app:      { ic: '📦', tag: 'App' },
};

// Accordi consonanti, un timbro per livello — vera polifonia (più oscillatori insieme).
// Brevi, attacco morbido, release esponenziale: campanella/carillon, non buzzer.
const CHORD = {
  info:     { notes: [659.25, 987.77],            type: 'sine',     gain: 0.16, roll: 0.0  }, // E5+B5 quinta
  success:  { notes: [523.25, 659.25, 783.99],    type: 'sine',     gain: 0.16, roll: 0.06 }, // DO maggiore, arpeggio
  warn:     { notes: [587.33, 880.0],             type: 'triangle', gain: 0.15, roll: 0.05 }, // RE5+LA5
  critical: { notes: [659.25, 659.25, 987.77],    type: 'triangle', gain: 0.18, roll: 0.06 }, // più attento, mai stridulo
  none:     null,
};

// Earcon SIGNATURE per sorgente (CHI) — mirror esatto del firmware notify_signature(): suona PRIMA
// dell'accordo di livello (urgenza). Stessa "DNA sonora" su entrambi i corpi (web + device).
const SIG = {
  anima:    [{ hz: 2093.0, t: 0, d: 0.10 }, { hz: 2637.02, t: 0.05, d: 0.12 }], // sparkle acuto
  calendar: [{ hz: 783.99, t: 0, d: 0.16 }],                                    // rintocco caldo
  ota:      [{ hz: 523.25, t: 0, d: 0.10 }, { hz: 783.99, t: 0.07, d: 0.12 }],  // ascendente
  recorder: [{ hz: 880.0,  t: 0, d: 0.10 }, { hz: 698.46, t: 0.07, d: 0.12 }],  // discendente
  voice:    [{ hz: 659.25, t: 0, d: 0.14 }],
  app:      [{ hz: 587.33, t: 0, d: 0.12 }],
  system:   [{ hz: 523.25, t: 0, d: 0.13 }],
};
const SIG_LEN = 0.16;

export function initNotify(OS_API) {
  const { byId, WM, openFile, FsIndex } = OS_API || {};
  let all = load();          // notifiche [{id,src,lvl,icon,title,body,action,sound,sticky,ts,read,count}]
  let dnd = localStorage.getItem(LS_DND) === '1';
  let actx = null;           // AudioContext pigro (creato al primo suono dopo un gesto utente)

  // ---- persistenza -------------------------------------------------------
  // localStorage è sincrono: una raffica di notifiche farebbe N scritture. Le coalesciamo in UNA
  // (timer breve), con flush immediato se la pagina sta per chiudersi → niente perdita di storico.
  function load() {
    try { const a = JSON.parse(localStorage.getItem(LS_LIST)); return Array.isArray(a) ? a : []; } catch { return []; }
  }
  let saveTimer = null;
  function flushSave() {
    if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
    try { localStorage.setItem(LS_LIST, JSON.stringify(all.slice(0, CAP))); } catch {}
  }
  function save() { if (!saveTimer) saveTimer = setTimeout(flushSave, 300); }
  window.addEventListener('pagehide', flushSave);
  document.addEventListener('visibilitychange', () => { if (document.hidden) flushSave(); });

  // ---- DOM: campanella in tray + flyout ---------------------------------
  const tray = document.getElementById('tray');
  const bell = document.createElement('span');
  bell.id = 'notif-bell'; bell.title = 'Notifiche'; bell.setAttribute('role', 'button');
  bell.innerHTML =
    '<svg viewBox="0 0 16 16" width="15" height="15" fill="none" stroke="currentColor" stroke-width="1.3" aria-hidden="true">' +
    '<path d="M4 6.5a4 4 0 0 1 8 0c0 3 1 4 1.4 4.5H2.6C3 10.5 4 9.5 4 6.5z" stroke-linejoin="round"/>' +
    '<path d="M6.5 13a1.5 1.5 0 0 0 3 0" stroke-linecap="round"/></svg>' +
    '<span class="nb-badge" hidden>0</span>';
  // Inserita prima dell'orologio (ordine Win11: notifiche vicino alla data/ora).
  const clock = document.getElementById('clock');
  if (tray && clock) tray.insertBefore(bell, clock); else if (tray) tray.appendChild(bell);

  const center = document.createElement('section');
  center.id = 'notif-center'; center.className = 'hidden';
  center.setAttribute('aria-label', 'Centro Notifiche');
  center.innerHTML =
    '<div class="nc-head">' +
      '<span class="nc-title">Notifiche</span>' +
      '<button id="nc-dnd" title="Non disturbare">🌙 <span>DND</span></button>' +
      '<button id="nc-clear" title="Pulisci tutto">Pulisci</button>' +
    '</div>' +
    '<div class="nc-digest" id="nc-digest" hidden></div>' +
    '<div class="nc-list" id="nc-list"></div>';
  document.body.appendChild(center);

  const listEl = center.querySelector('#nc-list');
  const digestEl = center.querySelector('#nc-digest');
  const dndBtn = center.querySelector('#nc-dnd');
  const clearBtn = center.querySelector('#nc-clear');

  // ---- apertura/chiusura -------------------------------------------------
  function isOpen() { return !center.classList.contains('hidden'); }
  function open() {
    center.classList.remove('hidden');
    markAllRead(); render(); syncBadge();
    // chiudi gli altri flyout per coerenza Win11
    const ac = document.getElementById('action-center'); if (ac) ac.classList.add('hidden');
    const sm = document.getElementById('start-menu'); if (sm) sm.classList.add('open') && sm.classList.remove('open');
  }
  function close() { center.classList.add('hidden'); }
  function toggle() { isOpen() ? close() : open(); }

  bell.addEventListener('click', (e) => { e.stopPropagation(); toggle(); });
  document.addEventListener('click', (e) => {
    if (isOpen() && !center.contains(e.target) && !bell.contains(e.target)) close();
  });
  dndBtn.addEventListener('click', () => setDND(!dnd));
  clearBtn.addEventListener('click', () => clearAll());

  // ---- DND ---------------------------------------------------------------
  function inQuietHours() {
    const q = localStorage.getItem(LS_QUIET) || '';
    const m = q.match(/^(\d{2}):(\d{2})-(\d{2}):(\d{2})$/); if (!m) return false;
    const now = new Date(); const cur = now.getHours() * 60 + now.getMinutes();
    const a = (+m[1]) * 60 + (+m[2]), b = (+m[3]) * 60 + (+m[4]);
    return a <= b ? (cur >= a && cur < b) : (cur >= a || cur < b);  // finestra che scavalca mezzanotte
  }
  function muted() { return dnd || inQuietHours(); }
  function setDND(on) {
    dnd = !!on; localStorage.setItem(LS_DND, dnd ? '1' : '0');
    dndBtn.classList.toggle('active', dnd); bell.classList.toggle('dnd', dnd);
  }
  setDND(dnd);

  // ---- melodia polifonica (Web Audio, vera polifonia) --------------------
  // Una voce: fondamentale + 2ª armonica (timbro carillon) con ADSR morbido.
  function playVoice(master, hz, t, dur, type) {
    const osc = actx.createOscillator(); osc.type = type; osc.frequency.value = hz;
    const o2 = actx.createOscillator(); o2.type = 'sine'; o2.frequency.value = hz * 2;
    const g = actx.createGain();
    g.gain.setValueAtTime(0.0001, t);
    g.gain.exponentialRampToValueAtTime(1, t + 0.012);            // attacco morbido ~12ms
    g.gain.exponentialRampToValueAtTime(0.0001, t + dur);         // release esponenziale
    const g2 = actx.createGain(); g2.gain.value = 0.35; g2.connect(g);
    osc.connect(g); o2.connect(g2); g.connect(master);
    osc.start(t); o2.start(t); osc.stop(t + dur + 0.05); o2.stop(t + dur + 0.05);
  }
  // Earcon = firma sorgente (CHI) + accordo livello (urgenza) in una sola frase polifonica.
  function chime(n) {
    const level = (n && (n.sound || n.lvl)) || 'info';
    const spec = CHORD[level] || CHORD.info; if (!spec) return;
    const vol = (parseInt(localStorage.getItem(LS_VOL) || '60', 10) || 0) / 100;
    if (vol <= 0) return;
    try {
      actx = actx || new (window.AudioContext || window.webkitAudioContext)();
      if (actx.state === 'suspended') actx.resume();
      const t0 = actx.currentTime + 0.01;
      const master = actx.createGain();
      master.gain.value = spec.gain * vol;
      master.connect(actx.destination);
      (SIG[n && n.src] || SIG.system).forEach((v) => playVoice(master, v.hz, t0 + v.t, v.d, 'sine'));     // firma (chi)
      spec.notes.forEach((hz, i) => playVoice(master, hz, t0 + SIG_LEN + i * spec.roll, 0.5, spec.type)); // accordo (urgenza)
    } catch {}
  }

  // ---- badge -------------------------------------------------------------
  function unread() { return all.reduce((n, x) => n + (x.read ? 0 : 1), 0); }
  function syncBadge() {
    const b = bell.querySelector('.nb-badge'); const n = unread();
    if (n > 0) { b.textContent = n > 99 ? '99+' : String(n); b.hidden = false; }
    else b.hidden = true;
  }
  function markAllRead() { all.forEach((x) => { x.read = true; }); save(); }

  // ---- rendering del flyout ----------------------------------------------
  function fmtWhen(ts) {
    const d = new Date(ts), now = new Date();
    const same = d.toDateString() === now.toDateString();
    const hh = String(d.getHours()).padStart(2, '0'), mm = String(d.getMinutes()).padStart(2, '0');
    return same ? `${hh}:${mm}` : `${String(d.getDate()).padStart(2, '0')}/${String(d.getMonth() + 1).padStart(2, '0')} ${hh}:${mm}`;
  }
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  // Digest: una riga riassuntiva calcolata dallo storico (zero costo).
  function digestText() {
    if (!all.length) return '';
    let cal = 0, ai = 0, unread = 0;
    for (const n of all) { if (n.src === 'calendar') cal++; else if (n.src === 'anima') ai++; if (!n.read) unread++; }
    const p = [`${all.length} ${all.length === 1 ? 'notifica' : 'notifiche'}`];
    if (cal) p.push(`${cal} calendario`);
    if (ai) p.push(`${ai} ANIMA`);
    if (unread) p.push(`${unread} da leggere`);
    return p.join(' · ');
  }
  function updateDigest() { const t = digestText(); digestEl.textContent = t; digestEl.hidden = !t; }

  function render() {
    updateDigest();
    if (!all.length) { listEl.innerHTML = '<div class="nc-empty">Nessuna notifica</div>'; return; }
    listEl.innerHTML = '';
    for (const n of all) {
      const meta = SRC[n.src] || SRC.app;
      const row = document.createElement('div');
      row.className = `nc-item lvl-${n.lvl || 'info'}`;
      row.innerHTML =
        `<div class="nc-ic">${esc(n.icon || meta.ic)}</div>` +
        '<div class="nc-body">' +
          '<div class="nc-row">' +
            `<span class="nc-ttl">${esc(n.title)}</span>` +
            (n.count > 1 ? `<span class="nc-count">×${n.count}</span>` : '') +
            `<span class="nc-when">${fmtWhen(n.ts)}</span>` +
          '</div>' +
          (n.body ? `<div class="nc-txt">${esc(n.body)}</div>` : '') +
          `<div class="nc-tag">${esc(meta.tag)}</div>` +
        '</div>' +
        '<button class="nc-x" title="Rimuovi">×</button>';
      row.querySelector('.nc-x').addEventListener('click', (e) => { e.stopPropagation(); remove(n.id); });
      row.addEventListener('click', () => { runAction(n.action); close(); });
      listEl.appendChild(row);
    }
  }

  function remove(id) { all = all.filter((x) => x.id !== id); save(); render(); syncBadge(); }
  function clearAll() { all = []; save(); render(); syncBadge(); }

  // ---- toast transitorio (arricchito: livello + azione) ------------------
  function toast(n) {
    const container = document.getElementById('toast-container'); if (!container) return;
    const meta = SRC[n.src] || SRC.app;
    const t = document.createElement('div');
    t.className = `toast n-toast ${n.lvl || 'info'}`;
    const actLabel = actionLabel(n.action);
    t.innerHTML =
      '<div class="nt-main">' +
        `<div class="toast-icon">${esc(n.icon || meta.ic)}</div>` +
        `<div class="toast-body"><div class="toast-title">${esc(n.title)}</div>` +
        (n.body ? `<div class="toast-msg">${esc(n.body)}</div>` : '') + '</div>' +
      '</div>' +
      (actLabel ? `<button class="nt-act">${esc(actLabel)}</button>` : '');
    const kill = () => { t.classList.add('removing'); setTimeout(() => t.remove(), 220); };
    t.addEventListener('click', kill);
    const actBtn = t.querySelector('.nt-act');
    if (actBtn) actBtn.addEventListener('click', (e) => { e.stopPropagation(); runAction(n.action); kill(); });
    container.appendChild(t);
    if (!n.sticky) setTimeout(kill, n.lvl === 'critical' ? 12000 : 6500);
  }

  // ---- router azioni (stesso contratto del copilot) ----------------------
  function actionLabel(act) {
    if (!act) return '';
    const [k, v] = splitAct(act);
    if (k === 'app') return 'Apri';
    if (k === 'file') return 'Apri';
    if (k === 'anima') return 'Chiedi ad ANIMA';
    return '';
  }
  function splitAct(act) { const i = String(act).indexOf(':'); return i < 0 ? [act, ''] : [act.slice(0, i), act.slice(i + 1)]; }
  function runAction(act) {
    if (!act) return;
    const [k, v] = splitAct(act);
    try {
      if (k === 'app') { const a = byId && byId(v); if (a && WM) WM.open(a); }
      else if (k === 'file') { if (openFile) openFile(v); }
      else if (k === 'anima') {
        // Inoltra al copilot di sistema se presente (Ctrl+Spazio).
        document.dispatchEvent(new CustomEvent('nucleo:anima-ask', { detail: { q: v } }));
      }
    } catch (e) { console.warn('[notify] action failed', act, e); }
  }

  // ---- ingresso unico: emit ----------------------------------------------
  function emit(input) {
    const n = normalize(input); if (!n) return;
    // coalescing per id: rimpiazza in cima, incrementa il contatore.
    const prev = all.find((x) => x.id === n.id);
    all = all.filter((x) => x.id !== n.id);
    n.count = prev ? (prev.count || 1) + 1 : 1;
    all.unshift(n);
    if (all.length > CAP) all = all.slice(0, CAP);
    save();
    if (isOpen()) { n.read = true; save(); render(); }
    else if (!muted()) { toast(n); chime(n); }
    else { /* DND/ore silenziose: niente toast/suono, resta nello storico */ }
    syncBadge();
    return n.id;
  }

  function normalize(input) {
    if (!input || (!input.title && !input.ttl)) return null;
    const src = (input.src || 'system').toLowerCase();
    const lvl = (input.lvl || input.level || 'info').toLowerCase();
    const meta = SRC[src] || SRC.app;
    return {
      id: input.id || `${src}-${Date.now()}-${Math.floor(Math.random() * 1e4)}`,
      src, lvl,
      icon: input.icon || input.ic || meta.ic,
      title: String(input.title || input.ttl || '').slice(0, 120),
      body: String(input.body || input.bd || '').slice(0, 240),
      action: input.action || input.act || '',
      sound: input.sound || input.snd || lvl,
      sticky: !!(input.sticky),
      ts: input.ts ? (input.ts < 1e12 ? input.ts * 1000 : input.ts) : Date.now(),  // accetta epoch s o ms
      read: false,
    };
  }

  // ---- ingresso dal bus eventi (WebSocket) -------------------------------
  function fromBus(topic, d) {
    if (topic === 'notify.post' && d) return emit(d);
    if (topic === 'calendar.reminder' && d) {  // legacy, finché il firmware non emette notify.post
      return emit({
        id: 'cal-' + (d.time || ''), src: 'calendar', lvl: 'info', icon: '🔔',
        title: d.text || 'Promemoria', body: d.time ? `Promemoria · ${d.time}` : '',
        action: 'app:calendar', sound: 'info',
      });
    }
  }

  syncBadge();   // ripristina il badge dallo storico persistito
  return { emit, fromBus, open, close, toggle, isOpen, clearAll, setDND, get all() { return all; } };
}
