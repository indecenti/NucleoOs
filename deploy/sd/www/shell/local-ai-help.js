// local-ai-help.js — "let NucleoOS use the AI server on THIS computer": the exact fix when a local server
// (Ollama, LM Studio, …) is running but refuses the page's origin (CORS). NucleoOS is served by the
// Cardputer from http://<device-ip>; Ollama only admits localhost-ish origins by default, so it answers
// 403 and the browser sees an opaque failure. ai-engines.js tells that apart from "down" (status 'cors');
// this module turns it into the right steps for the user's OS and server — commands ready to copy — and a
// "check again" that re-probes. Wording lives in the CORE catalog (lai_* keys, five languages).
//
// Sources (checked 2026-09-30): Ollama FAQ "How can I allow additional web origins" + "Setting environment
// variables on Windows / macOS / Linux"; LM Studio CLI `lms server start --cors`.
// Pure pieces (detectOS, originsFix) are host-tested in tools/shell-local-ai-help.test.mjs.
import { copyText, ensureCss } from './capabilities.js';
import { probeServer, forgetServers } from './ai-engines.js';

export function detectOS(ua = (globalThis.navigator && globalThis.navigator.userAgent) || '', platform = '') {
  const s = ua + ' ' + platform;
  if (/Android/i.test(s)) return 'android';
  if (/iPhone|iPad|iPod/i.test(s)) return 'ios';
  if (/Windows/i.test(s)) return 'windows';
  if (/Mac OS X|Macintosh|macOS/i.test(s)) return 'mac';
  if (/Linux|X11|CrOS/i.test(s)) return 'linux';
  return 'other';
}

// → { steps: [{ k, vars?, code?, block? }], notes: [{ k, vars?, code? }] } — k = a core catalog key.
export function originsFix({ os, origin, server }) {
  const name = (server && server.name) || 'Ollama';
  const vars = { server: name, origin };
  const id = server && server.id;
  if (id === 'lmstudio') {
    return { steps: [{ k: 'lai_lms_cli', vars, code: 'lms server start --cors' }], notes: [{ k: 'lai_lms_gui', vars }] };
  }
  if (!server || server.kind !== 'ollama') return { steps: [{ k: 'lai_generic', vars }], notes: [] };
  const ip = /^https?:\/\/(\d{1,3}\.){3}\d{1,3}(:\d+)?$/.test(origin || '');
  const tail = [{ k: 'lai_keep', vars }].concat(ip ? [{ k: 'cap_tip_ip', vars }] : []);
  if (os === 'windows') return {
    steps: [{ k: 'lai_win_quit', vars }, { k: 'lai_win_run', vars, code: `setx OLLAMA_ORIGINS "${origin}"` }, { k: 'lai_win_start', vars }],
    notes: tail,
  };
  if (os === 'mac') return {
    steps: [{ k: 'lai_mac_run', vars, code: `launchctl setenv OLLAMA_ORIGINS "${origin}"` }, { k: 'lai_mac_restart', vars }],
    notes: [{ k: 'lai_mac_reboot', vars }].concat(tail),
  };
  if (os === 'linux') return {
    steps: [
      { k: 'lai_linux_edit', vars, code: 'sudo systemctl edit ollama.service' },
      { k: 'lai_linux_add', vars, code: `[Service]\nEnvironment="OLLAMA_ORIGINS=${origin}"`, block: true },
      { k: 'lai_linux_restart', vars, code: 'sudo systemctl daemon-reload && sudo systemctl restart ollama' },
    ],
    notes: [{ k: 'lai_linux_manual', vars, code: `OLLAMA_ORIGINS="${origin}" ollama serve` }].concat(tail),
  };
  return { steps: [{ k: 'lai_other', vars }], notes: [] };
}

// Render the wizard into `host`. server: an ai-engines server record ({id, kind, name, base}); t(key, vars):
// the core catalog. onVerified(probe) runs once the server accepts this page.
export function renderLocalAiHelp(host, { server, origin = globalThis.location && globalThis.location.origin, os = detectOS() }, t, { onVerified, probe = probeServer } = {}) {
  ensureCss(host.ownerDocument);
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const fix = originsFix({ os, origin, server });
  const code = (x) => x.code == null ? ''
    : x.block ? `<pre class="cap-copy" data-copy="${esc(x.code)}">${esc(x.code)}</pre>`
      : ` <code class="cap-copy" data-copy="${esc(x.code)}">${esc(x.code)}</code>`;
  const vars = { server: (server && server.name) || 'Ollama', origin };
  host.innerHTML = `<div class="cap-help lai-help"><p>${esc(t('lai_why', vars))}</p>`
    + `<ol class="cap-steps">${fix.steps.map((s) => `<li>${esc(t(s.k, s.vars))}${code(s)}</li>`).join('')}</ol>`
    + fix.notes.map((n) => `<p class="cap-tip">${esc(t(n.k, n.vars))}${code(n)}</p>`).join('')
    + `<div class="cap-actions"><button type="button" class="cap-verify">${esc(t('cap_verify'))}</button><span class="cap-verify-msg" role="status"></span></div></div>`;
  host.querySelectorAll('.cap-copy').forEach((el) => {
    el.setAttribute('role', 'button'); el.setAttribute('tabindex', '0'); el.title = t('cap_copy');
    const doCopy = () => copyText(el.dataset.copy).then((ok) => { if (ok) { el.classList.add('copied'); setTimeout(() => el.classList.remove('copied'), 1200); } });
    el.addEventListener('click', doCopy);
    el.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); doCopy(); } });
  });
  const vb = host.querySelector('.cap-verify'), msg = host.querySelector('.cap-verify-msg');
  vb.addEventListener('click', async () => {
    vb.disabled = true;
    let p = null; try { p = await probe(server, { details: false }); } catch {}
    vb.disabled = false;
    const st = p && p.status;
    msg.textContent = st === 'ok' ? t('lai_ok', { ...vars, n: p.models.length }) : st === 'cors' ? t('lai_still', vars) : t('lai_down', vars);
    if (st === 'ok') { forgetServers(); if (onVerified) onVerified(p); }   // every AI surface re-detects on its next turn
  });
  return fix;
}
