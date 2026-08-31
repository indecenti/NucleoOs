// system-ui.js — OS-chrome extras that make the shell feel like a real desktop OS:
//   • Night light  — a warm veil over the whole console (per-browser, like brightness/eco)
//   • Lock screen  — a privacy veil with clock + date (Ctrl+Alt+L / Action Center). It is a
//     presentation lock, NOT an auth boundary: the device's pairing cookie stays valid, so we
//     honestly unlock on any input instead of pretending a PIN protects the session.
//   • Shortcuts overlay — the keyboard cheat-sheet every desktop OS ships
//   • Widgets — taskbar weather pill + a flyout fed ONLY from data already in the browser
//     (the Weather app's localStorage cache, the shell's last /api/status snapshot, recents):
//     ZERO device traffic, which is the whole point on a PSRAM-less single-task server.
// Everything here is browser-side; the Cardputer is never touched. Loaded eagerly by shell.js
// (tiny), i18n through the 'shell' namespace.
import I18N from './nucleo-i18n.js';

const t = I18N.scope('shell');
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ─── Night light ─────────────────────────────────────────────────────────────
const LS_NIGHT = 'nucleo.night';
let veil = null;
function nightOn() { return localStorage.getItem(LS_NIGHT) === '1'; }
function applyNight(on) {
  if (!veil) { veil = document.createElement('div'); veil.id = 'night-veil'; document.body.appendChild(veil); }
  veil.classList.toggle('on', !!on);
  try { localStorage.setItem(LS_NIGHT, on ? '1' : '0'); } catch {}
}
function toggleNight() { applyNight(!nightOn()); return nightOn(); }

// ─── Lock screen ─────────────────────────────────────────────────────────────
let lockEl = null, lockTimer = null, locked = false, lockedAt = 0;
function lockMarkup() {
  const el = document.createElement('div');
  el.id = 'lock-screen'; el.className = 'hidden';
  el.innerHTML =
    '<div class="lk-center">' +
      '<div class="lk-time">--:--</div>' +
      '<div class="lk-date"></div>' +
      '<div class="lk-dev"></div>' +
      `<div class="lk-hint">${esc(t('lock_hint'))}</div>` +
    '</div>';
  document.body.appendChild(el);
  return el;
}
function lockTick() {
  if (!lockEl) return;
  const d = new Date();
  lockEl.querySelector('.lk-time').textContent =
    String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
  lockEl.querySelector('.lk-date').textContent = I18N.fmtDate(d, { weekday: 'long', day: 'numeric', month: 'long' });
}
function lock() {
  if (locked) return;
  // Pull focus out of any app iframe first — otherwise a keydown while an app was focused would go
  // to that iframe, not the shell's unlock listener (the pointerdown path still works either way).
  try { const ae = document.activeElement; if (ae && ae.tagName === 'IFRAME') ae.blur(); window.focus(); } catch {}
  if (!lockEl) lockEl = lockMarkup();
  lockEl.querySelector('.lk-hint').textContent = t('lock_hint');
  lockEl.querySelector('.lk-dev').textContent = (document.getElementById('sm-device') || {}).textContent || 'NucleoOS';
  lockTick();
  lockTimer = setInterval(lockTick, 20000);
  lockEl.classList.remove('hidden', 'lk-out');
  locked = true; lockedAt = Date.now();
}
function unlock() {
  if (!locked || !lockEl) return;
  clearInterval(lockTimer); lockTimer = null;
  locked = false;
  lockEl.classList.add('lk-out');                       // slide-up reveal, then hide
  setTimeout(() => { if (!locked) lockEl.classList.add('hidden'); }, 380);
}
// Any input unlocks — but swallow the unlocking event so it can't also click something under
// the veil or type into an app. A ~400 ms arm delay eats the keyup of the locking shortcut.
function onLockInput(e) {
  if (!locked) return;
  if (Date.now() - lockedAt < 400) { e.preventDefault(); e.stopPropagation(); return; }
  e.preventDefault(); e.stopPropagation();
  unlock();
}
window.addEventListener('keydown', onLockInput, true);
window.addEventListener('pointerdown', onLockInput, true);

// ─── Shortcuts cheat-sheet ───────────────────────────────────────────────────
// Rows are [keys[], i18n-key]; rendered on every open so a live language switch is honoured.
const KBD_GROUPS = () => [
  [t('kb_g_system'), [
    [['Win'], 'kb_start'],
    [['Ctrl', 'Space'], 'kb_copilot'],
    [['Alt', 'Tab'], 'kb_switch'],
    [['Win', '← → ↑ ↓'], 'kb_snap'],
    [['Ctrl', 'Alt', 'L'], 'kb_lock'],
    [['Ctrl', 'W'], 'kb_close'],
    [['Ctrl', 'Shift', 'V'], 'kb_clip'],
  ]],
  [t('kb_g_desktop'), [
    [['F2'], 'kb_rename'],
    [['Del'], 'kb_delete'],
    [['Ctrl', 'A'], 'kb_selall'],
    [['Enter'], 'kb_open_sel'],
  ]],
  [t('kb_g_docs'), [
    [['Ctrl', 'S'], 'kb_save'],
    [['Ctrl', 'C'], 'kb_copy'],
    [['Ctrl', 'V'], 'kb_paste'],
    [['Esc'], 'kb_esc'],
  ]],
];
let kbdEl = null;
function showShortcuts() {
  if (!kbdEl) {
    kbdEl = document.createElement('div');
    kbdEl.id = 'kbd-overlay'; kbdEl.className = 'hidden';
    kbdEl.addEventListener('mousedown', (e) => { if (e.target === kbdEl) hideShortcuts(); });
    kbdEl.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.stopPropagation(); hideShortcuts(); } }, true);
    document.body.appendChild(kbdEl);
  }
  const groups = KBD_GROUPS().map(([title, rows]) =>
    `<div class="kb-group"><div class="kb-gt">${esc(title)}</div>` +
    rows.map(([keys, k]) =>
      `<div class="kb-row"><span class="kb-keys">${keys.map((x) => `<kbd>${esc(x)}</kbd>`).join('<i>+</i>')}</span>` +
      `<span class="kb-what">${esc(t(k))}</span></div>`).join('') + '</div>').join('');
  kbdEl.innerHTML =
    `<div class="kb-card" tabindex="-1"><div class="kb-head"><span>${esc(t('kb_title'))}</span>` +
    `<button class="kb-x" aria-label="${esc(t('close'))}">×</button></div><div class="kb-body">${groups}</div></div>`;
  kbdEl.querySelector('.kb-x').addEventListener('click', hideShortcuts);
  kbdEl.classList.remove('hidden');
  kbdEl.querySelector('.kb-card').focus();
}
function hideShortcuts() { if (kbdEl) kbdEl.classList.add('hidden'); }

// ─── Widgets (taskbar weather pill + flyout) ─────────────────────────────────
// Everything painted here is already in the browser: weather.cache is written by the Weather
// app (same origin), the status snapshot comes from the shell's ONE 15 s poll, recents live in
// the shared UI state. Opening the panel costs the device NOTHING.
const WX_EMOJI = { sun: '☀️', partly: '⛅', cloud: '☁️', fog: '🌫️', drizzle: '🌦️', rain: '🌧️', snow: '🌨️', storm: '⛈️' };
function wmoClass(code) {
  if (code === 0) return 'sun';
  if (code === 1 || code === 2) return 'partly';
  if (code === 3) return 'cloud';
  if (code === 45 || code === 48) return 'fog';
  if ([51, 53, 55, 56, 57].includes(code)) return 'drizzle';
  if ([61, 63, 65, 66, 67, 80, 81, 82].includes(code)) return 'rain';
  if ([71, 73, 75, 77, 85, 86].includes(code)) return 'snow';
  if ([95, 96, 99].includes(code)) return 'storm';
  return 'cloud';
}
function wxEmoji(code, isDay) {
  const cls = wmoClass(code);
  if (cls === 'sun' && isDay === 0) return '🌙';
  return WX_EMOJI[cls];
}
function readWeather() {
  try {
    const c = JSON.parse(localStorage.getItem('weather.cache'));
    return (c && c.place && c.data && c.data.current) ? c : null;
  } catch { return null; }
}

export function initSystemUI(ctx) {
  // ctx: { byId, WM, openFile, getStatusSnap(), getRecent(), fmtBytes(b) }
  applyNight(nightOn());                                 // restore the veil on boot

  // — taskbar pill —
  const holder = document.querySelector('.tb-side.tb-left');
  let pill = null, panel = null;
  if (holder) {
    pill = document.createElement('button');
    pill.id = 'tb-widgets';
    pill.setAttribute('aria-haspopup', 'true'); pill.setAttribute('aria-expanded', 'false');
    holder.appendChild(pill);
    paintPill();
    pill.addEventListener('click', (e) => { e.stopPropagation(); togglePanel(); });
    // The Weather app writing its cache fires 'storage' here (different document) → live pill.
    window.addEventListener('storage', (e) => { if (e.key === 'weather.cache') paintPill(); });
    I18N.onChange(() => { paintPill(); if (panel && !panel.classList.contains('hidden')) renderPanel(); });
  }
  function paintPill() {
    if (!pill) return;
    const wx = readWeather();
    if (wx) {
      const c = wx.data.current;
      pill.innerHTML = `<span class="wg-emo">${wxEmoji(c.weather_code, c.is_day)}</span>` +
        `<span class="wg-t">${Math.round(c.temperature_2m)}°</span>`;
      pill.title = wx.place.name;
    } else {
      pill.innerHTML = '<span class="wg-emo">🧩</span>';
      pill.title = t('widgets');
    }
  }
  function togglePanel() {
    if (!panel) { panel = document.createElement('section'); panel.id = 'widgets-panel'; panel.className = 'hidden'; document.body.appendChild(panel); }
    const opening = panel.classList.contains('hidden');
    if (opening) { renderPanel(); panel.classList.remove('hidden'); }
    else panel.classList.add('hidden');
    pill && pill.setAttribute('aria-expanded', String(opening));
  }
  function closePanel() { if (panel) panel.classList.add('hidden'); pill && pill.setAttribute('aria-expanded', 'false'); }
  document.addEventListener('click', (e) => {
    if (panel && !panel.classList.contains('hidden') && !panel.contains(e.target) && (!pill || !pill.contains(e.target))) closePanel();
  });

  function renderPanel() {
    const wx = readWeather();
    const snap = ctx.getStatusSnap && ctx.getStatusSnap();
    const recent = (ctx.getRecent && ctx.getRecent() || []).slice(0, 4);
    let html = '';
    // — weather card —
    if (wx) {
      const c = wx.data.current, D = wx.data.daily || {};
      let days = '';
      // A partially-written weather cache (time present, series missing) must degrade to "no daily
      // rows", not throw mid-render and leave the whole widgets panel empty.
      const daysOk = D.weather_code && D.temperature_2m_min && D.temperature_2m_max;
      for (let i = 0; daysOk && i < Math.min(3, (D.time || []).length); i++) {
        const dn = i === 0 ? t('wg_today') : I18N.fmtDate(new Date(D.time[i] + 'T12:00:00'), { weekday: 'short' });
        days += `<div class="wg-day"><span>${esc(dn)}</span><span class="e">${wxEmoji(D.weather_code[i], 1)}</span>` +
          `<span class="mm">${Math.round(D.temperature_2m_min[i])}°·<b>${Math.round(D.temperature_2m_max[i])}°</b></span></div>`;
      }
      html += `<div class="wg-card wg-wx" data-open="weather"><div class="wg-wx-main">` +
        `<span class="wg-wx-emo">${wxEmoji(c.weather_code, c.is_day)}</span>` +
        `<span class="wg-wx-t">${Math.round(c.temperature_2m)}°</span>` +
        `<span class="wg-wx-place">${esc(wx.place.name)}</span></div>` +
        `<div class="wg-days">${days}</div></div>`;
    } else {
      html += `<div class="wg-card wg-wx" data-open="weather"><div class="wg-empty">🌤 ${esc(t('wg_weather_empty'))}</div></div>`;
    }
    // — device pulse card (from the shell's last snapshot: zero fetch) —
    if (snap) {
      const kb = Math.round((snap.free_heap || 0) / 1024);
      const sd = snap.storage && snap.storage.mounted ? ctx.fmtBytes(snap.storage.free_bytes) : '—';
      const up = fmtUp(snap.uptime_s || 0);
      html += `<div class="wg-card wg-pulse" data-open="system-monitor"><div class="wg-ct">${esc(t('wg_pulse'))}</div>` +
        `<div class="wg-stats"><span><i>${esc(t('wg_heap'))}</i><b>${kb} KB</b></span>` +
        `<span><i>${esc(t('wg_sd'))}</i><b>${esc(sd)}</b></span>` +
        `<span><i>${esc(t('wg_up'))}</i><b>${esc(up)}</b></span></div></div>`;
    }
    // — recent files —
    html += `<div class="wg-card"><div class="wg-ct">${esc(t('wg_recent'))}</div>`;
    if (recent.length) {
      html += recent.map((r) => `<button class="wg-file" data-path="${esc(r.path)}">📄 <span>${esc(r.path.split('/').pop())}</span></button>`).join('');
    } else html += `<div class="wg-empty">${esc(t('wg_recent_empty'))}</div>`;
    html += '</div>';
    panel.innerHTML = html;
    panel.querySelectorAll('[data-open]').forEach((el) => el.addEventListener('click', () => {
      const a = ctx.byId && ctx.byId(el.dataset.open);
      if (a && ctx.WM) { ctx.WM.open(a); closePanel(); }
    }));
    panel.querySelectorAll('.wg-file').forEach((el) => el.addEventListener('click', (e) => {
      e.stopPropagation(); if (ctx.openFile) { ctx.openFile(el.dataset.path); closePanel(); }
    }));
  }
  function fmtUp(s) { s |= 0; const d = s / 86400 | 0, h = s % 86400 / 3600 | 0, m = s % 3600 / 60 | 0; return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`; }

  return { toggleNight, nightOn, lock, unlock, isLocked: () => locked, showShortcuts, closeWidgets: closePanel };
}
