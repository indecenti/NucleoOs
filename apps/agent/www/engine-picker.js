// engine-picker.js — the F4 surface: ONE place where the user sees every substrate ANIMA Code can
// run on, installs a local model, and picks. The design constraints, in order:
//
//   HONEST FIRST. A rung this hardware cannot run is shown greyed WITH THE REASON ("this browser has
//   no WebGPU"), and its install button is disabled BEFORE any bytes move — never a 300 MB download
//   that ends in an error the user could have been warned about. The reasons are i18n KEYS, not
//   strings: the OS speaks five languages and hardware bad news must arrive in the user's own.
//
//   PURE CORE. rungRows/pickEngine/localRungOrder take capabilities and cache state as VALUES and
//   return plain rows — host-tested with no browser (tools/anima-host/anima-code-picker.test.mjs).
//   The DOM half below consumes them and is deliberately thin.
//
//   SHARED CACHE. Models install into the same Cache-API bucket the ANIMA Forge panel uses
//   ('anima-forge-models', keys '/fc/<id>/<file>') — same origin, so a model installed here is
//   already installed there, and vice versa. One download serves the whole OS.

// NO static imports: the forge modules live at the SERVED path /apps/anima/forge/* (www/ is
// invisible in URLs), which node cannot resolve — and a relative path that works in node breaks in
// the browser for the same reason. So the pure core below is import-free (host tests import THIS
// file directly), and the browser half loads forge lazily via dynamic import at first use.
const forge = { mods: null };
async function loadForge() {
  if (!forge.mods) {
    const ms = await import('/apps/anima/forge/model-store.js');
    const fl = await import('/apps/anima/forge/install-flow.js');
    forge.mods = { ms, fl };
  }
  return forge.mods;
}

export const ENGINE_LS = 'agent.engine';          // persisted user choice: 'auto' | 'cloud' | 'webgpu' | 'wasm'
export const WEBGPU_MODEL = 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC';
export const WASM_MODEL = 'Qwen2.5-Coder-0.5B-Instruct-GGUF';
const FM_CACHE = 'anima-forge-models';            // MUST match apps/anima/www/index.html (_fmCache)

// ── pure core ─────────────────────────────────────────────────────────────────────────────────

// Probe what this client can actually do. Async because WebGPU only answers via requestAdapter.
export async function probeCaps(glob = (typeof globalThis !== 'undefined' ? globalThis : {})) {
  const caps = { webgpu: false, vramMB: 0, wasm: typeof glob.WebAssembly !== 'undefined', online: !!(glob.navigator && glob.navigator.onLine) };
  try {
    const gpu = glob.navigator && glob.navigator.gpu;
    if (gpu && gpu.requestAdapter) {
      const ad = await gpu.requestAdapter();
      if (ad) { caps.webgpu = true; caps.vramMB = Math.round(((ad.limits && ad.limits.maxBufferSize) || 0) / 1048576); }
    }
  } catch { /* no WebGPU — the row will say so, honestly */ }
  return caps;
}

// caps + cache status + key state → displayable rows. status: { [modelId]: 'cached'|'absent'|'downloading' }.
// Every non-runnable state carries reasonKey — an i18n key, resolved by the UI in the OS language.
export const MODEL_SIZE_TEXT = { [WEBGPU_MODEL]: '~290 MB', [WASM_MODEL]: '~490 MB' };
export function rungRows(caps = {}, status = {}, hasKey = false) {
  const rows = [];
  rows.push({ id: 'cloud', state: hasKey ? (caps.online ? 'ready' : 'offline') : 'no-key',
    reasonKey: hasKey ? (caps.online ? null : 'eng_cloud_offline') : 'eng_cloud_nokey' });
  rows.push({ id: 'webgpu', model: WEBGPU_MODEL, sizeText: MODEL_SIZE_TEXT[WEBGPU_MODEL], runnable: true,
    state: !caps.webgpu ? 'unsupported' : (status[WEBGPU_MODEL] === 'cached' ? 'ready' : (status[WEBGPU_MODEL] === 'downloading' ? 'downloading' : 'needs-model')),
    reasonKey: !caps.webgpu ? 'eng_no_webgpu' : null });
  // runnable:false — HONESTY over symmetry. The wasm MODEL installs for real (same shared cache the
  // Forge panel reads, real air-gap value) but the agent loop's wllama chat adapter is F3 work that
  // does not exist yet, and a picker that says "ready" for an engine that cannot run is a lie. The
  // row says so (eng_wasm_soon) instead of pretending.
  rows.push({ id: 'wasm', model: WASM_MODEL, sizeText: MODEL_SIZE_TEXT[WASM_MODEL], runnable: false, noteKey: 'eng_wasm_soon',
    state: !caps.wasm ? 'unsupported' : (status[WASM_MODEL] === 'cached' ? 'ready' : (status[WASM_MODEL] === 'downloading' ? 'downloading' : 'needs-model')),
    reasonKey: !caps.wasm ? 'eng_no_wasm' : null });
  return rows;
}

// The stored choice is honored ONLY while its rung is ready — hardware and caches change under a
// stored string, and silently running a different engine than the label says would be a lie.
export function pickEngine(stored, rows) {
  const ok = (id) => { const r = rows.find((x) => x.id === id); return r && r.state === 'ready' && r.runnable !== false; };
  if (stored && stored !== 'auto' && ok(stored)) return stored;
  return 'auto';
}

// The local rungs the F0 cascade should try, in engine-policy order (GPU first), ready ones only.
// choice !== 'auto' narrows to that single rung: an explicit pick means "this one, not the ladder".
export function localRungOrder(rows, choice = 'auto') {
  const ready = rows.filter((r) => (r.id === 'webgpu' || r.id === 'wasm') && r.state === 'ready' && r.runnable !== false);
  if (choice === 'webgpu' || choice === 'wasm') return ready.filter((r) => r.id === choice);
  return ready.sort((a, b) => (a.id === 'webgpu' ? -1 : 1) - (b.id === 'webgpu' ? -1 : 1));
}

// ── browser wiring ────────────────────────────────────────────────────────────────────────────

// The same cache contract the forge panel uses; kept tiny and local so this module stays lazy.
const sha256 = async (u8) => { const b = await crypto.subtle.digest('SHA-256', u8); return [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join(''); };
const cacheHas = async (id, n) => { const c = await caches.open(FM_CACHE); return !!(await c.match('/fc/' + id + '/' + n)); };
const cachePut = async (id, n, u8) => { const c = await caches.open(FM_CACHE); await c.put('/fc/' + id + '/' + n, new Response(u8)); };

export async function makeAgentStore() {
  const { ms } = await loadForge();
  return ms.makeModelStore({
    // Manifests resolve from the embedded registry when the SD carries none (getManifest → null is fine).
    getManifest: async (id) => { try { const r = await fetch((ms.modelById(id) || {}).sdBase + 'manifest.json', { cache: 'no-store' }); return r.ok ? await r.json() : null; } catch { return null; } },
    fetchWhole: async (u, opts = {}) => {
      try {
        const r = await fetch(u, { signal: opts.signal });
        if (!r.ok) return { ok: false, status: r.status };
        if (opts.onChunk && r.body && r.body.getReader) {
          const rd = r.body.getReader(); const cs = []; let n = 0;
          for (;;) { const { done, value } = await rd.read(); if (done) break; cs.push(value); n += value.length; try { opts.onChunk(n); } catch {} }
          const out = new Uint8Array(n); let o = 0; for (const c of cs) { out.set(c, o); o += c.length; }
          return { ok: true, status: r.status, bytes: out };
        }
        return { ok: true, status: r.status, bytes: new Uint8Array(await r.arrayBuffer()) };
      } catch { return { ok: false, status: 0 }; }
    },
    fetchRange: async (u, rg, opts = {}) => {
      try { const r = await fetch(u, { headers: { Range: 'bytes=' + rg.start + '-' + rg.end }, signal: opts.signal }); return (r.ok || r.status === 206) ? { ok: true, status: r.status, bytes: new Uint8Array(await r.arrayBuffer()) } : { ok: false, status: r.status }; } catch { return { ok: false, status: 0 }; }
    },
    sha256, cacheHas, cachePut, isOnline: () => navigator.onLine,
    telemetry: () => ({ freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false }),
  });
}

// Render the picker into `host`. t() resolves i18n in the ACTIVE OS language (all five supported —
// every user-facing string below goes through it). onChange(choice, rows) fires on pick/install.
export function initEnginePicker({ host, t, hasKey, onChange }) {
  let store = null;
  const getStore = async () => (store || (store = await makeAgentStore()));
  let caps = null, rows = [], choice = localStorage.getItem(ENGINE_LS) || 'auto';

  const statusOf = async () => {
    const st = await getStore();
    const s = {};
    for (const id of [WEBGPU_MODEL, WASM_MODEL]) { try { s[id] = await st.status(id); } catch { s[id] = 'absent'; } }
    return s;
  };

  const LABELS = { cloud: 'eng_cloud', webgpu: 'eng_webgpu', wasm: 'eng_wasm' };
  const STATE_KEY = { ready: 'eng_ready', 'needs-model': 'eng_needs_model', unsupported: 'eng_unsupported', 'no-key': 'eng_nokey_short', offline: 'eng_offline_short', downloading: 'eng_downloading' };

  function render() {
    const parts = ['<div class="ep-head">' + esc(t('eng_title')) + '<small>' + esc(t('eng_sub')) + '</small></div>'];
    for (const r of rows) {
      const sel = (choice === r.id) || (choice === 'auto' && r.id === 'cloud' && r.state === 'ready');
      const canPick = r.state === 'ready' && r.runnable !== false;
      parts.push('<div class="ep-row' + (canPick ? '' : ' dim') + '" data-id="' + r.id + '">'
        + '<label><input type="radio" name="eng" value="' + r.id + '"' + (sel ? ' checked' : '') + (canPick ? '' : ' disabled') + '> '
        + esc(t(LABELS[r.id])) + '</label>'
        + '<span class="ep-state s-' + r.state + '">' + esc(t(STATE_KEY[r.state] || r.state)) + '</span>'
        + (r.reasonKey ? '<div class="ep-why">' + esc(t(r.reasonKey)) + '</div>' : '')
        + (r.noteKey ? '<div class="ep-why">' + esc(t(r.noteKey)) + '</div>' : '')
        + (r.state === 'needs-model' ? '<button class="ep-get" data-model="' + r.model + '" data-kind="' + r.id + '">' + esc(t('eng_install', { size: r.sizeText })) + '</button>' : '')
        + '<div class="ep-prog" hidden><div class="ep-bar"></div><span class="ep-pct"></span></div>'
        + '</div>');
    }
    parts.push('<div class="ep-row"><label><input type="radio" name="eng" value="auto"' + (choice === 'auto' ? ' checked' : '') + '> ' + esc(t('eng_auto')) + '</label><div class="ep-why">' + esc(t('eng_auto_why')) + '</div></div>');
    host.innerHTML = parts.join('');
    host.querySelectorAll('input[name="eng"]').forEach((el) => el.addEventListener('change', () => {
      choice = el.value; localStorage.setItem(ENGINE_LS, choice); onChange && onChange(choice, rows);
    }));
    host.querySelectorAll('.ep-get').forEach((btn) => btn.addEventListener('click', () => install(btn)));
  }

  async function install(btn) {
    const row = btn.closest('.ep-row'); const modelId = btn.dataset.model; const kind = btn.dataset.kind;
    const prog = row.querySelector('.ep-prog'), bar = row.querySelector('.ep-bar'), pct = row.querySelector('.ep-pct');
    btn.hidden = true; prog.hidden = false;
    const ui = {
      label: modelId,
      onCancel: (fn) => { prog.onclick = fn; },     // tap the bar to cancel — small surface, no extra chrome
      setPhase: () => {}, setCancelled: () => { prog.hidden = true; btn.hidden = false; },
      setReconnecting: ({ attempt }) => { pct.textContent = t('eng_retrying', { n: attempt }); },
      setError: (msg) => { prog.hidden = true; btn.hidden = false; row.querySelector('.ep-why')?.remove(); row.insertAdjacentHTML('beforeend', '<div class="ep-why err">' + esc(msg) + '</div>'); },
      onProgress: (p) => { const f = p.bytesTotal ? p.bytesDone / p.bytesTotal : 0; bar.style.width = Math.round(f * 100) + '%'; pct.textContent = Math.round(f * 100) + '%'; },
      setDone: () => {},
    };
    const { fl } = await loadForge();
    const r = await fl.installModel({ store: await getStore(), modelId, kind: kind === 'webgpu' ? 'webgpu' : 'wasm', caps, sizeText: '', lang: (t('_lang') || 'en'), ui });
    await refresh();
    if (r && r.ok) onChange && onChange(choice, rows);
  }

  async function refresh() {
    caps = caps || await probeCaps();
    rows = rungRows(caps, await statusOf(), hasKey());
    choice = pickEngine(localStorage.getItem(ENGINE_LS) || 'auto', rows);
    render();
  }

  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  refresh();
  return { refresh, get rows() { return rows; }, get choice() { return choice; } };
}
