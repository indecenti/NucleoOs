// NucleoOS — shared online-AI key manager (the ONE place the OS asks for / stores a provider key).
//
// Before this module the key UI was copy-pasted into three surfaces (Settings, the ANIMA web app, and
// partly groq-chat), each with its OWN provider table + teacher.json read/write + cloud test — and they
// had already DRIFTED (ANIMA lacked xAI, accepted AQ. Gemini tokens the others rejected; two divergent
// key tests). This component is the single owner of that UI, built entirely on shell/ai.js (the single
// owner of the LOGIC: PROVIDERS/CAPMATRIX/TIERS, teacher.json I/O, cloud calls). Mount it; never re-implement.
//
// Persistence is /data/anima/teacher.json via the paired /api/fs/* — the device-held vault the firmware
// reads. The active provider sits at top-level (what the firmware loads) plus a keys{} map so switching
// Claude↔Groq never loses the other key. The RELEASE system (tools/deploy.ps1, sd-sync.ps1, sd_deploy.py)
// treats that file as device-state and never overwrites it — a key on the SD always wins over the repo.
//
//   import { mountKeyManager } from '/ai-keys.js';
//   const km = mountKeyManager(containerEl, { lang:'it', variant:'full', onChange(cfg){ /* refresh chrome */ } });
//   // handle: km.reload(), km.getCfg(), km.setLang('en'), km.destroy()
//
// opts: variant 'full' (Settings: provider chips + capability hint + Gemini plan + exec toggle + model + key)
//       | 'compact' (ANIMA drawer: provider chips + model + key). onChange(cfg) fires after load + every
//       mutation so the host can refresh dependent UI (preset engine, capability cards). exec:false hides
//       the browser/device toggle (kept from the loaded value); tier:false hides the Gemini plan row.

import * as AI from './ai.js';

const STR = {
  it: {
    checking: 'controllo…', saving: 'salvo…', testing: 'provo…', deleting: 'elimino…',
    active: 'Chiave attiva:', nokey: 'Nessuna chiave per questo provider.', noset: 'Nessuna chiave impostata.',
    typefirst: 'Scrivi prima una chiave.', notlooklike: 'Non sembra una chiave ', saveanyway: '. Salvo lo stesso?',
    saved: 'Salvata:', deleted: 'Chiave eliminata.', delActive: 'Chiave eliminata. Attivo: ',
    confirmDel: 'Elimino la chiave ', confirmDelTail: ' da questo dispositivo?',
    pair: 'Accoppia il browser per gestire la chiave.', pairSave: 'Accoppia prima il browser (pairing).',
    cantread: 'Non riesco a leggere la chiave.', savefail: 'Salvataggio fallito.', delfail: 'Eliminazione fallita.',
    works: '✓ La chiave funziona — ', rejected: '✗ Chiave rifiutata.', typeortest: 'Inserisci o salva prima una chiave.',
    key: 'Chiave', model: 'Modello', exec: 'Esecuzione', plan: 'Piano', detect: '🔎 Rileva piano',
    browser: 'Browser', device: 'Device', save: '💾 Salva', test: '⚡ Prova', del: '🗑 Elimina',
    note: '🔒 La chiave è salvata solo sul tuo dispositivo (SD), mai inviata altrove né nei log. In Browser resta nel tuo browser e va diretta al provider — il Cardputer non viene caricato.',
    execBrowser: 'Le superfici web chiamano il provider direttamente dal browser: il Cardputer non viene caricato.',
    execDevice: 'Il Cardputer effettua le chiamate online (TLS leggero, gestito dall’arbitro anti-OOM).',
    paid: 'a pagamento · Pro disponibile', free: 'Free tier · solo Flash', noplan: 'piano non rilevato',
  },
  en: {
    checking: 'checking…', saving: 'saving…', testing: 'testing…', deleting: 'deleting…',
    active: 'Active key:', nokey: 'No key for this provider.', noset: 'No key set.',
    typefirst: 'Type a key first.', notlooklike: 'That does not look like a ', saveanyway: ' key. Save anyway?',
    saved: 'Saved:', deleted: 'Key deleted.', delActive: 'Key deleted. Active: ',
    confirmDel: 'Delete the ', confirmDelTail: ' key from this device?',
    pair: 'Pair this browser to manage the key.', pairSave: 'Pair this browser first (Settings ▸ pairing).',
    cantread: 'Could not read the key.', savefail: 'Save failed.', delfail: 'Delete failed.',
    works: '✓ Key works — ', rejected: '✗ Key rejected.', typeortest: 'Type or save a key first.',
    key: 'Key', model: 'Model', exec: 'Execution', plan: 'Plan', detect: '🔎 Detect plan',
    browser: 'Browser', device: 'Device', save: '💾 Save', test: '⚡ Test', del: '🗑 Delete',
    note: '🔒 The key is stored only on your device (SD), never sent elsewhere or logged. In Browser it stays in your browser and goes straight to the provider — the Cardputer isn’t loaded.',
    execBrowser: 'Web surfaces call the provider straight from the browser: the Cardputer isn’t loaded.',
    execDevice: 'The Cardputer makes the online calls (light TLS, handled by the anti-OOM arbiter).',
    paid: 'paid · Pro available', free: 'Free tier · Flash only', noplan: 'plan not detected',
  },
};

// One short capability hint per provider (full variant), derived from CAPMATRIX so it never drifts.
function capHint(p, en) {
  const c = AI.CAPMATRIX[p] || {};
  const can = [], cant = [];
  (en ? [['chat', 'chat'], ['image', 'image gen'], ['whisper', 'voice transcription'], ['toolUse', 'OS tools']]
      : [['chat', 'chat'], ['image', 'immagini'], ['whisper', 'trascrizione voce'], ['toolUse', 'strumenti OS']])
    .forEach(([k, lbl]) => (c[k] ? can : cant).push(lbl));
  return (en ? 'Can: ' : 'Sa: ') + can.join(', ') + (cant.length ? (en ? ' · No: ' : ' · No: ') + cant.join(', ') : '');
}

let _cssInjected = false;
function injectCss() {
  if (_cssInjected) return;
  _cssInjected = true;
  const s = document.createElement('style');
  s.textContent = `
.nkm{display:flex;flex-direction:column;gap:10px;font:inherit}
.nkm-chips{display:flex;flex-wrap:wrap;gap:6px}
.nkm-chip{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,var(--muted,#cfcfe0));
  border-radius:var(--r-pill,999px);padding:5px 11px;font-size:13px;line-height:1;transition:.12s}
.nkm-chip:hover{border-color:var(--accent,#9b8cff)}
.nkm-chip.cur{background:var(--accent,#9b8cff);color:var(--accent-on,var(--ink,#0e0e12));border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-chip .ok{opacity:.7;margin-left:5px}
.nkm-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.nkm-row label{min-width:64px;color:var(--dim,#8a8a9a);font-size:13px}
.nkm select,.nkm input{background:var(--field,var(--bg,#0e0e12));color:var(--ink,#e8e8ee);border:1px solid var(--line,#2a2a35);
  border-radius:var(--r-sm,8px);padding:7px 9px;font:inherit;font-size:14px}
.nkm input{flex:1;min-width:180px}
.nkm select{cursor:pointer;min-width:190px}
.nkm-seg{display:inline-flex;border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);overflow:hidden}
.nkm-seg .it{cursor:pointer;padding:6px 12px;font-size:13px;color:var(--dim,#8a8a9a)}
.nkm-seg .it.on{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);font-weight:600}
.nkm-btns{display:flex;gap:7px;flex-wrap:wrap}
.nkm-btn{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,#e8e8ee);
  border-radius:var(--r-sm,8px);padding:7px 12px;font:inherit;font-size:13px}
.nkm-btn:hover{border-color:var(--accent,#9b8cff)}
.nkm-btn.primary{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-btn.danger:hover{border-color:var(--bad,var(--danger-text,#ff6b6b));color:var(--bad,var(--danger-text,#ff6b6b))}
.nkm-cap,.nkm-stat,.nkm-exechint{font-size:12.5px;color:var(--dim,#8a8a9a);line-height:1.4}
.nkm-stat b{color:var(--ink,#e8e8ee)}
.nkm-note{font-size:12px;color:var(--dim,#8a8a9a);line-height:1.45;opacity:.85}
.nkm-badge{font-size:12px;padding:2px 8px;border-radius:var(--r-pill,999px);border:1px solid var(--line,#2a2a35);color:var(--dim,#8a8a9a)}`;
  document.head.appendChild(s);
}

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export function mountKeyManager(container, opts = {}) {
  injectCss();
  let lang = opts.lang === 'en' ? 'en' : 'it';
  const variant = opts.variant === 'compact' ? 'compact' : 'full';
  const full = variant === 'full';
  const showExec = full && opts.exec !== false;
  const showTier = opts.tier !== false;
  const onChange = typeof opts.onChange === 'function' ? opts.onChange : () => {};
  const t = () => STR[lang];

  // The live config (one per mount). keys{} remembers every provider's saved config so a switch is lossless.
  let cfg = { provider: 'anthropic', base: AI.PROVIDERS.anthropic.base, model: AI.PROVIDERS.anthropic.def,
              key: '', version: '2023-06-01', exec: 'browser', keys: {}, geminiTier: '', geminiModels: null };
  let hydrated = false;
  const prov = () => AI.providerOf(cfg.provider);

  // ---- DOM skeleton ----
  const root = document.createElement('div');
  root.className = 'nkm';
  const providerIds = Object.keys(AI.PROVIDERS);
  root.innerHTML =
    `<div class="nkm-chips" data-el="chips"></div>` +
    (full ? `<div class="nkm-cap" data-el="cap"></div>` : '') +
    (full && showTier ? `<div class="nkm-row" data-el="tierrow" style="display:none"><label>${esc(t().plan)}</label><span class="nkm-badge" data-el="tier">…</span><button type="button" class="nkm-btn" data-el="detect">${esc(t().detect)}</button></div>` : '') +
    `<div class="nkm-row"><label>${esc(t().model)}</label><select data-el="model"></select></div>` +
    (showExec ? `<div class="nkm-row"><label>${esc(t().exec)}</label><span class="nkm-seg" data-el="exec"><span class="it" data-x="browser">${esc(t().browser)}</span><span class="it" data-x="device">${esc(t().device)}</span></span></div>` : '') +
    (showExec ? `<div class="nkm-exechint" data-el="exechint"></div>` : '') +
    `<div class="nkm-row"><label>${esc(t().key)}</label><input data-el="key" type="password" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="sk-ant-…"></div>` +
    `<div class="nkm-btns"><button type="button" class="nkm-btn primary" data-el="save">${esc(t().save)}</button><button type="button" class="nkm-btn" data-el="test">${esc(t().test)}</button><button type="button" class="nkm-btn danger" data-el="del">${esc(t().del)}</button></div>` +
    `<div class="nkm-stat" data-el="stat">…</div>` +
    (full ? `<div class="nkm-note">${esc(t().note)}</div>` : '');
  container.innerHTML = '';
  container.appendChild(root);
  const $ = (n) => root.querySelector(`[data-el="${n}"]`);

  const setStat = (html) => { $('stat').innerHTML = html; };
  const statText = () => cfg.key
    ? `${t().active} <b>${esc(AI.maskKey(cfg.key))}</b> · ${esc(prov().label)} · ${esc(cfg.model)}`
    : t().nokey;

  function fillChips() {
    $('chips').innerHTML = providerIds.map((id) => {
      const set = !!(cfg.keys && cfg.keys[id] && cfg.keys[id].key) || (id === cfg.provider && cfg.key);
      return `<button type="button" class="nkm-chip${id === cfg.provider ? ' cur' : ''}" data-p="${id}">${esc(AI.PROVIDERS[id].label)}${set ? '<span class="ok">✓</span>' : ''}</button>`;
    }).join('');
    $('chips').querySelectorAll('[data-p]').forEach((b) => b.addEventListener('click', () => chooseProvider(b.dataset.p)));
  }
  function fillModels() {
    const sel = $('model'); if (!sel) return;
    const list = (cfg.provider === 'google' && cfg.geminiModels && cfg.geminiModels.length) ? cfg.geminiModels : prov().models;
    sel.innerHTML = '';
    for (const m of list) { const o = document.createElement('option'); o.value = m[0]; o.textContent = AI.modelLabel(m); sel.appendChild(o); }
    sel.value = (cfg.model && list.some((m) => m[0] === cfg.model)) ? cfg.model : (list[0] ? list[0][0] : prov().def);
    cfg.model = sel.value;
  }
  function paint() {
    const p = prov();
    fillChips();
    if ($('key')) $('key').placeholder = p.ph;
    if (full && $('cap')) $('cap').textContent = capHint(cfg.provider, lang === 'en');
    if (full && showTier && $('tierrow')) {
      $('tierrow').style.display = cfg.provider === 'google' ? '' : 'none';
      if ($('tier')) $('tier').textContent = AI.geminiTierLabel(cfg.geminiTier, lang === 'en');
    }
    if (showExec && $('exec')) {
      $('exec').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.x === (cfg.exec || 'browser')));
      if ($('exechint')) $('exechint').textContent = (cfg.exec === 'device') ? t().execDevice : t().execBrowser;
    }
    fillModels();
  }

  function adopt(p) {
    const d = AI.providerOf(p), st = (cfg.keys && cfg.keys[p]) || {};
    cfg.provider = p; cfg.base = st.base || d.base; cfg.model = st.model || d.def;
    cfg.version = st.version || d.version; cfg.key = st.key || ''; cfg.geminiModels = null;
  }
  function chooseProvider(p) {
    if (!AI.PROVIDERS[p]) return;
    adopt(p); paint(); setStat(statText()); if ($('key')) $('key').value = '';
    onChange(getCfg());
  }

  async function reload() {
    setStat(t().checking); if ($('key')) $('key').value = '';
    const c = await AI.readTeacher({ fresh: true });
    if (c && c.unpaired) { setStat(t().pair); return; }
    if (!c) { setStat(t().cantread); return; }
    hydrated = true;
    cfg.keys = (c.keys && typeof c.keys === 'object') ? c.keys : {};
    cfg.exec = c.exec || 'browser'; cfg.geminiTier = c.geminiTier || ''; cfg.geminiModels = null;
    cfg.provider = c.provider || 'anthropic';
    const p = prov();
    cfg.base = c.base || p.base; cfg.model = c.model || p.def; cfg.key = c.key || ''; cfg.version = c.version || p.version;
    if (cfg.key) cfg.keys[cfg.provider] = Object.assign({ base: cfg.base, model: cfg.model, key: cfg.key }, cfg.provider === 'anthropic' ? { version: cfg.version } : {});
    paint(); setStat(cfg.key ? statText() : t().noset);
    onChange(getCfg());
  }

  async function calibrate() {
    if (cfg.provider !== 'google' || !cfg.key) return;
    try {
      const c = await AI.calibrateGemini({ base: cfg.base, key: cfg.key });
      cfg.geminiTier = c.tier; cfg.geminiModels = (c.models || []).map((id) => [id, id]);
      if (!c.models.includes(cfg.model)) cfg.model = c.recommended;
      paint();
      const lbl = c.tier === 'paid' ? t().paid : c.tier === 'free' ? t().free : t().noplan;
      setStat(statText() + ' · <b>' + esc(lbl) + '</b>');
      await AI.writeTeacher(cfg);   // persist the detected plan + recommended model
      onChange(getCfg());
    } catch {}
  }

  async function save() {
    const v = $('key').value.trim(), p = prov();
    if (!v) { setStat(t().typefirst); return; }
    if (!p.prefix.test(v) && !confirm(t().notlooklike + p.label + t().saveanyway)) return;
    cfg.key = v; if ($('model')) cfg.model = $('model').value || cfg.model;
    setStat(t().saving);
    const r = await AI.writeTeacher(cfg);
    if (r === true) {
      cfg.keys = cfg.keys || {};
      cfg.keys[cfg.provider] = Object.assign({ base: cfg.base, model: cfg.model, key: v }, cfg.provider === 'anthropic' ? { version: cfg.version } : {});
      $('key').value = '';
      setStat(`${t().saved} <b>${esc(AI.maskKey(v))}</b> · ${esc(p.label)} · ${esc(cfg.model)}`);
      fillChips();
      onChange(getCfg());
      if (cfg.provider === 'google') calibrate();
    } else if (r === 'unpaired') setStat(t().pairSave);
    else setStat(t().savefail);
  }

  async function test() {
    const v = ($('key').value.trim()) || cfg.key;
    if (!v) { setStat(t().typeortest); return; }
    const model = $('model') ? $('model').value : cfg.model;
    setStat(t().testing);
    try {
      const ok = await AI.cloudPing({ provider: cfg.provider, base: cfg.base || prov().base, model, key: v, version: cfg.version || prov().version });
      setStat(ok ? (t().works + esc(prov().label) + ' · ' + esc(model)) : t().rejected);
    } catch (e) { setStat('✗ ' + esc(String(e.message || e))); }
  }

  async function del() {
    if (!confirm(t().confirmDel + prov().label + t().confirmDelTail)) return;
    setStat(t().deleting);
    const keys = Object.assign({}, cfg.keys || {}); delete keys[cfg.provider];
    const remaining = Object.keys(keys).filter((k) => keys[k] && keys[k].key);
    try {
      if (remaining.length) {
        const np = remaining[0], e = keys[np];
        cfg.keys = keys;
        cfg.provider = np; cfg.base = e.base; cfg.model = e.model; cfg.key = e.key; cfg.version = e.version || '2023-06-01';
        const r = await AI.writeTeacher(cfg);
        if (r === true) { paint(); if ($('key')) $('key').value = ''; setStat(t().delActive + esc(AI.PROVIDERS[np].label)); onChange(getCfg()); return; }
        if (r === 'unpaired') { setStat(t().pairSave); return; }
        setStat(t().delfail); return;
      }
      const r = await fetch('/api/fs/delete?path=' + encodeURIComponent(AI.AI_PATH), { method: 'POST' });
      AI.invalidateTeacher();
      if (r.ok || r.status === 404) { cfg.key = ''; cfg.keys = {}; if ($('key')) $('key').value = ''; paint(); setStat(t().deleted); onChange(getCfg()); return; }
      if (r.status === 401 || r.status === 403) { setStat(t().pairSave); return; }
      setStat(t().delfail);
    } catch { setStat(t().delfail); }
  }

  function getCfg() { return { provider: cfg.provider, base: cfg.base, model: cfg.model, key: cfg.key, version: cfg.version, exec: cfg.exec, geminiTier: cfg.geminiTier, hasKey: !!cfg.key, keys: cfg.keys }; }

  // ---- wire ----
  $('save').addEventListener('click', save);
  $('test').addEventListener('click', test);
  $('del').addEventListener('click', del);
  $('key').addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); save(); } });
  if ($('model')) $('model').addEventListener('change', () => { cfg.model = $('model').value; });
  if ($('detect')) $('detect').addEventListener('click', () => { setStat(t().checking); calibrate(); });
  if ($('exec')) $('exec').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => {
    cfg.exec = b.dataset.x === 'device' ? 'device' : 'browser'; paint();
    AI.writeTeacher(cfg).then(() => onChange(getCfg()));   // exec is a teacher.json field — persist immediately
  }));

  paint();
  reload();

  return {
    reload, getCfg,
    setLang(l) { lang = l === 'en' ? 'en' : 'it'; /* re-render labels by rebuilding */ mountKeyManager(container, Object.assign({}, opts, { lang })); },
    destroy() { container.innerHTML = ''; },
  };
}
