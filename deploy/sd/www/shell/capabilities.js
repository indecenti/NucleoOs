// capabilities.js — ONE honest answer to "what can this browser do for NucleoOS, and if not, why and
// how do I fix it?". Shared by ANIMA, Settings, the Agent, Paint and the model installer (lazy import:
// `import('/capabilities.js')` — the shell never loads it at boot, so it costs the device nothing).
//
// Why it exists: the Cardputer serves the OS over plain http on a LAN IP, which is NOT a secure context.
// There the browser hides WebGPU, the Cache API, service workers, OPFS, crypto.subtle and Chrome's
// built-in AI — i.e. every local-AI capability — and five surfaces explained it five different (and
// partly wrong) ways. This module measures, diagnoses once, and describes the fix per browser.
//
// The pure parts (browser detection, diagnosis, fix description, adapter classification) take plain
// inputs and are host-tested (tools/shell-capabilities.test.mjs); probeCapabilities() is the only
// browser-touching function, plus the optional DOM helper renderCapabilityHelp().

// ── pure: which browser, and where its "treat this http origin as secure" switch lives ─────────────
export function detectBrowser(ua = '', brands = []) {
  const b = (brands || []).map((x) => String(x && x.brand || '')).join(' ');
  const has = (re) => re.test(b) || re.test(ua);
  const mobile = /Android|iPhone|iPad|Mobile/i.test(ua);
  let name = 'other';
  if (has(/Edg(e|A|iOS)?\//i) || /Microsoft Edge/i.test(b)) name = 'edge';
  else if (/Brave/i.test(b)) name = 'brave';
  else if (has(/OPR\/|Opera/i)) name = 'opera';
  else if (/Firefox\//i.test(ua)) name = 'firefox';
  else if (/Chrome\/|Chromium|Google Chrome/i.test(ua + ' ' + b)) name = 'chrome';
  else if (/Safari\//i.test(ua)) name = 'safari';
  const m = ua.match(name === 'firefox' ? /Firefox\/(\d+)/ : name === 'edge' ? /Edg\w*\/(\d+)/ : name === 'safari' ? /Version\/(\d+)/ : /Chrom(?:e|ium)\/(\d+)/);
  return { name, version: m ? Number(m[1]) : 0, mobile, chromium: ['chrome', 'edge', 'brave', 'opera'].includes(name) };
}

const FLAG_SCHEME = { chrome: 'chrome', edge: 'edge', brave: 'brave', opera: 'opera' };
// What the user must do so THIS origin counts as secure. { kind, flagUrl?, origin } — the UI renders the
// steps from the kind (strings live in the core catalog, 5 languages).
export function secureFix(browser, origin) {
  if (browser.chromium) {
    return { kind: 'chromium-flag', origin, flagUrl: `${FLAG_SCHEME[browser.name] || 'chrome'}://flags/#unsafely-treat-insecure-origin-as-secure` };
  }
  if (browser.name === 'firefox') return { kind: 'firefox-pref', origin, host: hostOf(origin), prefUrl: 'about:config', pref: 'dom.securecontext.allowlist' };
  if (browser.name === 'safari') return { kind: 'use-chromium', origin };
  return { kind: 'use-chromium', origin };
}
const hostOf = (origin) => { try { return new URL(origin).hostname; } catch { return String(origin || ''); } };
export const isIpHost = (origin) => /^(\d{1,3}\.){3}\d{1,3}$/.test(hostOf(origin));

// ── pure: classify the GPU adapter (WebGPU never exposes total VRAM) ───────────────────────────────
// Returns a coarse class the model recommender can use: 'discrete' | 'integrated' | 'fallback' | 'none'.
export function adapterClass(info) {
  if (!info) return 'none';
  if (info.isFallback) return 'fallback';
  const v = String(info.vendor || '').toLowerCase(), a = String(info.architecture || '').toLowerCase(), d = String(info.description || '').toLowerCase();
  if (/swiftshader|llvmpipe|basic render|microsoft basic/.test(d + ' ' + a)) return 'fallback';
  if (/nvidia/.test(v + d) || (/amd|ati/.test(v + d) && !/radeon\(tm\) graphics|vega \d graphics|680m|780m|890m/.test(d))) return 'discrete';
  if (/apple/.test(v) && /m[1-9]/.test(a + d)) return 'integrated';      // unified memory, strong
  return 'integrated';
}

// ── pure: one diagnosis for the local-AI stack ─────────────────────────────────────────────────────
// caps → { gpu: 'ok'|'insecure'|'browser'|'blocked'|'no-f16', storage: 'ok'|'insecure'|'low', ready }
export function diagnose(caps) {
  const c = caps || {};
  let gpu;
  if (c.webgpu && c.webgpu.available) gpu = c.webgpu.f16 ? 'ok' : 'no-f16';
  else if (c.secure === false) gpu = 'insecure';
  else if (c.webgpu && c.webgpu.api && !c.webgpu.available) gpu = 'blocked';
  else gpu = 'browser';
  let storage = 'ok';
  if (c.secure === false || !c.cacheApi) storage = 'insecure';
  else if (c.storage && c.storage.quotaMB && c.storage.quotaMB - (c.storage.usageMB || 0) < 1500) storage = 'low';
  return { gpu, storage, ready: (gpu === 'ok' || gpu === 'no-f16') && storage !== 'insecure' };
}

// ── browser: measure ───────────────────────────────────────────────────────────────────────────────
let _caps = null;
export async function probeCapabilities({ fresh = false } = {}) {
  if (_caps && !fresh) return _caps;
  const g = globalThis, nav = g.navigator || {};
  const caps = {
    origin: (g.location && g.location.origin) || '',
    secure: !!g.isSecureContext,
    browser: detectBrowser(nav.userAgent || '', (nav.userAgentData && nav.userAgentData.brands) || []),
    cacheApi: typeof g.caches !== 'undefined',
    serviceWorker: !!(nav.serviceWorker),
    opfs: !!(nav.storage && typeof nav.storage.getDirectory === 'function'),
    cryptoSubtle: !!(g.crypto && g.crypto.subtle),
    deviceMemoryGB: nav.deviceMemory || null,
    cores: nav.hardwareConcurrency || null,
    webgpu: { api: !!nav.gpu, available: false },
    storage: null,
    builtinAI: { prompt: typeof g.LanguageModel !== 'undefined', translator: typeof g.Translator !== 'undefined', languageDetector: typeof g.LanguageDetector !== 'undefined', summarizer: typeof g.Summarizer !== 'undefined' },
  };
  if (nav.gpu && typeof nav.gpu.requestAdapter === 'function') {
    try {
      const ad = await nav.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (ad) {
        const info = ad.info || {};
        caps.webgpu = {
          api: true, available: true,
          f16: !!(ad.features && ad.features.has && ad.features.has('shader-f16')),
          vendor: info.vendor || '', architecture: info.architecture || '', description: info.description || '',
          isFallback: !!(info.isFallbackAdapter || ad.isFallbackAdapter),
          maxBufferMB: ad.limits ? Math.round(ad.limits.maxBufferSize / 1048576) : 0,
          maxStorageMB: ad.limits ? Math.round(ad.limits.maxStorageBufferBindingSize / 1048576) : 0,
        };
        caps.webgpu.class = adapterClass(caps.webgpu);
      }
    } catch (e) { caps.webgpu.error = String(e && e.message || e); }
  }
  try {
    if (nav.storage && nav.storage.estimate) {
      const e = await nav.storage.estimate();
      caps.storage = { quotaMB: Math.round((e.quota || 0) / 1048576), usageMB: Math.round((e.usage || 0) / 1048576),
        persisted: nav.storage.persisted ? await nav.storage.persisted() : false };
    }
  } catch {}
  caps.fix = secureFix(caps.browser, caps.origin);
  caps.diagnosis = diagnose(caps);
  _caps = caps;
  return caps;
}

// ── browser: the shared help panel (checklist + exact fix + copy + verify) ─────────────────────────
// t: a translate function over the CORE catalog (cap_* keys). onVerified: called when a re-probe passes.
// Minimal self-contained look (any app can host the panel); an app's own .cap-* rules win if present.
const CAP_CSS = `.cap-help{font-size:12.5px;line-height:1.5}.cap-list{list-style:none;margin:0 0 10px;padding:0;display:grid;gap:4px}
.cap-row{display:flex;gap:8px;align-items:baseline;flex-wrap:wrap}.cap-row small{opacity:.75}.cap-dot{width:16px;text-align:center;font-weight:700}
.cap-row.ok .cap-dot{color:#36d399}.cap-row.no .cap-dot{color:#f87272}.cap-steps{margin:6px 0 8px;padding-left:24px;display:grid;gap:6px}
.cap-copy{cursor:pointer;padding:1px 5px;border-radius:5px;border:1px dashed currentColor;word-break:break-all;font-family:ui-monospace,monospace}
pre.cap-copy{display:block;white-space:pre-wrap;margin:4px 0 0;padding:4px 7px}
.cap-copy.copied{border-color:#36d399;color:#36d399}.cap-tip{margin:6px 0 0;font-size:11.5px;opacity:.85}
.cap-actions{display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap}.cap-actions button{cursor:pointer;padding:5px 12px;border-radius:7px}`;
export function ensureCss(doc) {
  if (!doc || doc.getElementById('cap-help-css')) return;
  const s = doc.createElement('style'); s.id = 'cap-help-css'; s.textContent = CAP_CSS;
  (doc.head || doc.documentElement).insertBefore(s, (doc.head || doc.documentElement).firstChild);   // first → app CSS overrides
}
export function renderCapabilityHelp(host, caps, t, { onVerified } = {}) {
  ensureCss(host.ownerDocument);
  const d = caps.diagnosis || diagnose(caps);
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const row = (ok, label, note) => `<li class="cap-row ${ok ? 'ok' : 'no'}"><span class="cap-dot" aria-hidden="true">${ok ? '✓' : '✕'}</span><span>${esc(label)}</span>${note ? `<small>${esc(note)}</small>` : ''}</li>`;
  const gpu = caps.webgpu || {};
  const list = [
    row(caps.secure, t('cap_secure'), caps.secure ? '' : t('cap_secure_no')),
    row(!!gpu.available, t('cap_webgpu'), gpu.available ? [gpu.vendor, gpu.architecture].filter(Boolean).join(' · ') : ''),
    gpu.available ? row(!!gpu.f16, t('cap_f16'), gpu.f16 ? '' : t('cap_f16_no')) : '',
    row(!!caps.cacheApi, t('cap_cache')),
    row(!!caps.serviceWorker, t('cap_sw')),
    caps.storage ? row(d.storage !== 'low', t('cap_storage'), t('cap_storage_free', { mb: Math.max(0, caps.storage.quotaMB - caps.storage.usageMB) })) : '',
  ].join('');
  let fix = '';
  const f = caps.fix || {};
  if (d.gpu === 'insecure' || d.storage === 'insecure') {
    if (f.kind === 'chromium-flag') {
      fix = `<ol class="cap-steps"><li>${esc(t('cap_fix_open'))} <code class="cap-copy" data-copy="${esc(f.flagUrl)}">${esc(f.flagUrl)}</code></li>`
        + `<li>${esc(t('cap_fix_type'))} <code class="cap-copy" data-copy="${esc(f.origin)}">${esc(f.origin)}</code></li>`
        + `<li>${esc(t('cap_fix_enable'))}</li></ol>`;
    } else if (f.kind === 'firefox-pref') {
      fix = `<ol class="cap-steps"><li>${esc(t('cap_ff_open'))} <code class="cap-copy" data-copy="about:config">about:config</code></li>`
        + `<li>${esc(t('cap_ff_pref'))} <code class="cap-copy" data-copy="${esc(f.pref)}">${esc(f.pref)}</code> = <code class="cap-copy" data-copy="${esc(f.host)}">${esc(f.host)}</code></li>`
        + `<li>${esc(t('cap_ff_restart'))}</li></ol>`;
    } else fix = `<p>${esc(t('cap_use_chromium'))}</p>`;
    fix += `<p class="cap-tip">${esc(t('cap_tip_unlocks'))}</p>`;
    if (isIpHost(f.origin)) fix += `<p class="cap-tip">${esc(t('cap_tip_ip'))}</p>`;
  } else if (d.gpu === 'browser') fix = `<p>${esc(t('cap_fix_browser'))}</p>`;
  else if (d.gpu === 'blocked') fix = `<p>${esc(t('cap_fix_blocked'))}</p>`;
  else if (d.gpu === 'no-f16') fix = `<p>${esc(t('cap_fix_nof16'))}</p>`;
  host.innerHTML = `<div class="cap-help"><ul class="cap-list">${list}</ul>${fix}`
    + (fix ? `<div class="cap-actions"><button type="button" class="cap-verify">${esc(t('cap_verify'))}</button><span class="cap-verify-msg" role="status"></span></div>` : '')
    + `</div>`;
  host.querySelectorAll('.cap-copy').forEach((el) => {
    el.setAttribute('role', 'button'); el.setAttribute('tabindex', '0'); el.title = t('cap_copy');
    const copy = () => copyText(el.dataset.copy).then((ok) => { if (ok) { el.classList.add('copied'); setTimeout(() => el.classList.remove('copied'), 1200); } });
    el.addEventListener('click', copy);
    el.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); copy(); } });
  });
  const vb = host.querySelector('.cap-verify');
  if (vb) vb.addEventListener('click', async () => {
    vb.disabled = true;
    const c2 = await probeCapabilities({ fresh: true });
    vb.disabled = false;
    const ok = c2.diagnosis.ready;
    host.querySelector('.cap-verify-msg').textContent = ok ? t('cap_verify_ok') : t('cap_verify_still');
    if (ok) { renderCapabilityHelp(host, c2, t, { onVerified }); if (onVerified) onVerified(c2); }
  });
  return d;
}

// Clipboard that also works on plain http (navigator.clipboard is secure-context only): the textarea
// + execCommand fallback still works on http, and the OS clipboard bridge covers apps inside the shell.
export async function copyText(text) {
  try { if (navigator.clipboard && globalThis.isSecureContext) { await navigator.clipboard.writeText(text); return true; } } catch {}
  try {
    const ta = document.createElement('textarea');
    ta.value = text; ta.setAttribute('readonly', ''); ta.style.cssText = 'position:fixed;left:-9999px;top:0';
    document.body.appendChild(ta); ta.select();
    const ok = document.execCommand('copy');
    ta.remove();
    if (ok) return true;
  } catch {}
  try { window.parent.postMessage({ type: 'clipboard-write', kind: 'text', data: text }, '*'); return true; } catch { return false; }
}
