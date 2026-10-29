/* SPDX-License-Identifier: AGPL-3.0-or-later */
// Settings › Accounts (capability users.multi, docs/multiuser.md): everyone's own account, and the administrator's
// list of users. Talks to the contract routes (/api/auth/*, /api/users*) and to nucleo-user.js; renders text only
// through textContent, never HTML built from what the device sent.

export const ICON = '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 4-6 8-6s8 2 8 6" stroke-linecap="round"/></svg>';

// Error words of the contract → catalog keys (the rest say "failed").
const ERR = {
  bad_credentials: 'accErrCredentials', weak_password: 'accErrWeak', name_taken: 'accErrNameTaken', users_max: 'accErrMax',
  last_admin: 'accErrLastAdmin', locked: 'accErrLocked', forbidden: 'accErrForbidden', signin_required: 'accErrSignin',
  offline: 'accErrOffline', not_found: 'accErrNotFound', bad_request: 'accErrBad',
};

function el(tag, props = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'on') for (const [ev, fn] of Object.entries(v)) e.addEventListener(ev, fn);
    else if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids) if (k != null) e.append(k.nodeType ? k : document.createTextNode(String(k)));
  return e;
}

export function createAccountsApi(fetchFn = (...a) => fetch(...a)) {
  async function call(method, path, body) {
    let r;
    try {
      r = await fetchFn(path, { method, cache: 'no-store', headers: body ? { 'content-type': 'application/json' } : {},
        body: body ? JSON.stringify(body) : undefined });
    } catch { return { ok: false, error: 'offline' }; }
    let j = {};
    try { j = await r.json(); } catch {}
    return r.ok ? { ok: true, ...j } : { ok: false, error: j.error || 'failed', status: r.status };
  }
  return {
    status: () => call('GET', '/api/auth/status'),
    users: () => call('GET', '/api/users'),
    create: (u) => call('POST', '/api/users/create', u),
    update: (u) => call('POST', '/api/users/update', u),
    remove: (u) => call('POST', '/api/users/delete', u),
    sessions: () => call('GET', '/api/users/sessions'),
    revoke: (id) => call('POST', '/api/users/sessions/revoke', { id }),
    audit: () => call('GET', '/api/users/audit?limit=50'),
  };
}

const MB = 1024 * 1024;
const fmtBytes = (n) => (n == null ? '—' : n < MB ? Math.ceil(n / 1024) + ' KB' : (n / MB).toFixed(n < 100 * MB ? 1 : 0) + ' MB');

export function mountAccounts(host, { T, os, api = createAccountsApi(), User }) {
  const say = (r) => (r.ok ? '' : T(ERR[r.error] || 'accErrFailed'));
  const status = (node, text, good) => { node.textContent = text; node.style.color = good ? 'var(--ok, #2fd07a)' : 'var(--warn, #e2576b)'; };

  // Built aside and swapped in at once: the page keeps its scroll position after an action.
  async function render() {
    const st = await api.status();
    const parts = [];
    if (st.ok && st.setup) parts.push(setupPanel(st));
    else if (!st.ok || !st.multi || !st.user) parts.push(el('div', { class: 'sect' }, el('div', { class: 'hint', text: T('accNotOn') })));
    else {
      parts.push(ownAccount(st.user));
      if (st.user.role === 'admin') parts.push(await usersPanel(st.user));
    }
    host.replaceChildren(...parts);
  }

  // A device updated to a firmware with accounts: nobody has turned them on yet. The administrator is created here;
  // the device's files and settings become theirs. From the network the device asks for its pairing PIN as proof.
  function setupPanel(st) {
    const sect = el('div', { class: 'sect' }, el('h2', { text: T('accSetupTitle') }), el('div', { class: 'hint', text: T('accSetupBody') }));
    const name = el('input', { type: 'text', maxlength: '32', autocomplete: 'username', 'aria-label': T('accName'), placeholder: T('accName') });
    const pw = el('input', { type: 'password', autocomplete: 'new-password', 'aria-label': T('accNew'), placeholder: T('accNew') });
    const again = el('input', { type: 'password', autocomplete: 'new-password', 'aria-label': T('accAgain'), placeholder: T('accAgain') });
    const pin = st.physical ? null : el('input', { type: 'text', inputmode: 'numeric', maxlength: '6', autocomplete: 'off', 'aria-label': T('accPin'), placeholder: T('accPin') });
    const out = el('span', { class: 'hint', role: 'status', 'aria-live': 'polite' });
    const go = el('button', { class: 'primary', type: 'button', text: T('accSetupGo'), on: { click: async () => {
      if (!name.value.trim() || !pw.value) return;
      if (pw.value !== again.value) { status(out, T('accMismatch')); return; }
      go.disabled = true;
      const r = await User.setup(name.value.trim(), pw.value, pin ? pin.value.trim() : '');
      go.disabled = false;
      if (!r.ok) { status(out, r.error === 'proof_required' ? T('accErrProof') : say(r)); return; }
      await User.switchTo(r.user.id);
      try { (window.top || window).location.reload(); } catch { location.reload(); }
    } } });
    sect.append(el('div', { class: 'ctl acc-form', style: 'margin-top:10px' }, name, pw, again, pin, go, out));
    return sect;
  }

  function ownAccount(me) {
    const sect = el('div', { class: 'sect' }, el('h2', { text: T('accMine') }));
    sect.append(el('div', { class: 'row' }, el('label', { text: T('accName') }), el('div', { class: 'ctl' }, el('span', { class: 'iv', text: me.name }))));
    sect.append(el('div', { class: 'row' }, el('label', { text: T('accRole') }),
      el('div', { class: 'ctl' }, el('span', { class: 'iv', text: T(me.role === 'admin' ? 'accRoleAdmin' : 'accRoleUser') }))));
    // password
    const old = el('input', { type: 'password', autocomplete: 'current-password', 'aria-label': T('accOld'), placeholder: T('accOld') });
    const nw = el('input', { type: 'password', autocomplete: 'new-password', 'aria-label': T('accNew'), placeholder: T('accNew') });
    const again = el('input', { type: 'password', autocomplete: 'new-password', 'aria-label': T('accAgain'), placeholder: T('accAgain') });
    const out = el('span', { class: 'hint', role: 'status', 'aria-live': 'polite' });
    const save = el('button', { class: 'primary', type: 'button', text: T('accChange'), on: { click: async () => {
      if (!old.value || !nw.value) return;
      if (nw.value !== again.value) { status(out, T('accMismatch')); return; }
      save.disabled = true;
      const r = await User.changePassword(old.value, nw.value);
      save.disabled = false;
      if (r.ok) { old.value = nw.value = again.value = ''; status(out, T('accChanged'), true); loadSessions(); }
      else status(out, say(r));
    } } });
    sect.append(el('h3', { class: 'acc-h3', text: T('accPassword') }),
      el('div', { class: 'ctl acc-form' }, old, nw, again, save, out));
    // sessions
    const list = el('div', { class: 'acc-list' });
    async function loadSessions() {
      const r = await api.sessions();
      list.replaceChildren();
      for (const s of (r.ok ? r.sessions : []).filter((x) => x.user === me.id)) {
        const when = s.seen ? new Date(s.seen * 1000).toLocaleString() : '';
        const row = el('div', { class: 'arow' },
          el('span', { class: 'grow', text: (s.current ? T('accThis') + ' · ' : '') + (s.ua || '?').slice(0, 60) }),
          el('span', { class: 'hint', text: [s.ip, when].filter(Boolean).join(' · ') }));
        if (!s.current) row.append(el('button', { type: 'button', text: T('accRevoke'), on: { click: async () => { await api.revoke(s.id); loadSessions(); } } }));
        list.append(row);
      }
    }
    loadSessions();
    sect.append(el('h3', { class: 'acc-h3', text: T('accSessions') }), list,
      el('div', { class: 'ctl', style: 'margin-top:10px' }, el('button', { type: 'button', text: T('accSignOut'), on: { click: async () => {
        await User.signOut();
        try { (window.top || window).location.reload(); } catch { location.reload(); }
      } } })));
    return sect;
  }

  async function usersPanel(me) {
    const sect = el('div', { class: 'sect' }, el('h2', { text: T('accUsers') }));
    const r = await api.users();
    if (!r.ok) { sect.append(el('div', { class: 'hint', text: say(r) })); return sect; }
    const out = el('div', { class: 'hint', role: 'status', 'aria-live': 'polite' });
    const after = (res, okText) => { if (res.ok) { if (okText) os.toast(okText, { kind: 'success' }); render(); } else status(out, say(res)); };
    const list = el('div', { class: 'acc-list' });
    for (const u of r.users) {
      const tags = [T(u.role === 'admin' ? 'accRoleAdmin' : 'accRoleUser')];
      if (u.disabled) tags.push(T('accDisabled'));
      if (u.active) tags.push(T('accOnline'));
      const space = u.quota ? fmtBytes(u.used) + ' / ' + fmtBytes(u.quota) : fmtBytes(u.used);
      const row = el('div', { class: 'arow' },
        el('span', { class: 'grow' }, el('b', { text: u.name }), ' ', el('span', { class: 'hint', text: tags.join(' · ') })),
        el('span', { class: 'hint', text: space }));
      const mine = u.id === me.id;
      row.append(...[
        el('button', { type: 'button', text: T('accReset'), on: { click: async () => {
          const pw = await os.prompt(T('accResetAsk').replace('{name}', u.name), { password: true });
          if (pw) after(await api.update({ id: u.id, password: pw }), T('accResetDone'));
        } } }),
        el('button', { type: 'button', text: T(u.role === 'admin' ? 'accMakeUser' : 'accMakeAdmin'), on: { click: async () =>
          after(await api.update({ id: u.id, role: u.role === 'admin' ? 'user' : 'admin' })) } }),
        mine ? null : el('button', { type: 'button', text: T(u.disabled ? 'accEnable' : 'accDisable'), on: { click: async () =>
          after(await api.update({ id: u.id, disabled: !u.disabled })) } }),
        mine ? null : el('button', { class: 'danger', type: 'button', text: T('accDelete'), on: { click: async () => {
          if (!(await os.confirm(T('accDeleteAsk').replace('{name}', u.name), { danger: true }))) return;
          const erase = await os.confirm(T('accEraseAsk'), { danger: true });
          let confirm_password;
          if (erase) { confirm_password = await os.prompt(T('accConfirmPw'), { password: true }); if (!confirm_password) return; }
          after(await api.remove({ id: u.id, data: erase ? 'erase' : 'archive', confirm_password }), T('accDeleted'));
        } } })].filter(Boolean));          // no Disable or Delete on one's own row
      list.append(row);
    }
    sect.append(list, out, el('div', { class: 'hint', text: T('accCount').replace('{n}', r.users.length).replace('{max}', r.max) }));
    // create
    if (r.users.length < r.max) {
      const name = el('input', { type: 'text', maxlength: '32', autocomplete: 'off', 'aria-label': T('accName'), placeholder: T('accName') });
      const pw = el('input', { type: 'password', autocomplete: 'new-password', 'aria-label': T('accNew'), placeholder: T('accNew') });
      const role = el('select', { 'aria-label': T('accRole') }, el('option', { value: 'user', text: T('accRoleUser') }), el('option', { value: 'admin', text: T('accRoleAdmin') }));
      const quota = el('input', { type: 'number', min: '0', step: '100', 'aria-label': T('accQuota'), placeholder: T('accQuota') });
      const add = el('button', { class: 'primary', type: 'button', text: T('accCreate'), on: { click: async () => {
        if (!name.value.trim() || !pw.value) return;
        const q = quota.value ? Math.round(+quota.value * MB) : null;
        after(await api.create({ name: name.value.trim(), password: pw.value, role: role.value, quota: q }), T('accCreated'));
      } } });
      sect.append(el('h3', { class: 'acc-h3', text: T('accNewUser') }), el('div', { class: 'ctl acc-form' }, name, pw, role, quota, add));
    }
    // audit
    const audit = el('details', { class: 'adv' }, el('summary', { text: T('accAudit') }));
    audit.addEventListener('toggle', async () => {
      if (!audit.open || audit.dataset.loaded) return;
      audit.dataset.loaded = '1';
      const a = await api.audit();
      const box = el('div', { class: 'acc-list' });
      for (const ev of (a.ok ? a.events : []).slice().reverse()) {
        box.append(el('div', { class: 'arow' },
          el('span', { class: 'hint', text: new Date(ev.t * 1000).toLocaleString() }),
          el('span', { class: 'grow', text: [ev.ev, ev.target, ev.ok ? '' : T('accFailed')].filter(Boolean).join(' · ') }),
          el('span', { class: 'hint', text: ev.ip || '' })));
      }
      audit.append(box);
    });
    sect.append(audit);
    return sect;
  }

  render();
  return { refresh: render };
}
