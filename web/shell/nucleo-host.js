/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-host.js — the web OS behind a hosting server's own sign-in (capability host.session, contract /api/host/*).
//
// One module for the three things only the server can do for real:
//   • who is signed in (Host.session());
//   • a REAL lock (Host.lock() / Host.unlock()): the server stops serving the web OS and its services until one of
//     this person's passkeys signs again (PIN, fingerprint or face). The shell's veil is only the picture of it;
//   • sign out (Host.signOut()): this browser forgets everything the site left in it (storage, caches, the service
//     worker), then the server's sign-out page ends the session and asks the browser to clear the rest
//     (Clear-Site-Data). Files in the cloud archive stay on the server.
// Where the capability is absent (a device, a static page) every call is a no-op answering null/false.
import Caps from './nucleo-caps.js';

const b64 = {
  toBuf(s) {
    const b = atob(String(s).replace(/-/g, '+').replace(/_/g, '/') + '==='.slice((String(s).length + 3) % 4));
    return Uint8Array.from(b, (c) => c.charCodeAt(0)).buffer;
  },
  fromBuf(buf) {
    let s = '';
    new Uint8Array(buf).forEach((x) => { s += String.fromCharCode(x); });
    return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  },
};

async function call(method, path, body) {
  const r = await fetch(path, {
    method, cache: 'no-store', credentials: 'same-origin',
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : {},
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  let d = {};
  try { d = await r.json(); } catch {}
  if (!r.ok) throw Object.assign(new Error(d.error || 'http_' + r.status), { status: r.status, code: d.error || '' });
  return d;
}

// Everything this origin keeps in the browser, gone. Bounded: an IndexedDB the page still holds open is deleted
// once the page leaves (the server's Clear-Site-Data covers it too).
async function forgetBrowser() {
  const jobs = [];
  try { localStorage.clear(); } catch {}
  try { sessionStorage.clear(); } catch {}
  if (self.caches) jobs.push(caches.keys().then((ks) => Promise.all(ks.map((k) => caches.delete(k)))).catch(() => {}));
  if (navigator.serviceWorker) {
    jobs.push(navigator.serviceWorker.getRegistrations().then((rs) => Promise.all(rs.map((r) => r.unregister()))).catch(() => {}));
  }
  if (self.indexedDB && indexedDB.databases) {
    jobs.push(indexedDB.databases().then((dbs) => Promise.all(dbs.filter((d) => d && d.name).map((d) => new Promise((ok) => {
      const q = indexedDB.deleteDatabase(d.name);
      q.onsuccess = q.onerror = q.onblocked = () => ok();
    })))).catch(() => {}));
  }
  await Promise.race([Promise.all(jobs), new Promise((ok) => setTimeout(ok, 3000))]);
}

const Host = {
  async available() { return Caps.has('host.session').catch(() => false); },

  // { user: { name, role } | null, locked, signout_url, signin_url } or null where not hosted.
  async session() {
    if (!(await Host.available())) return null;
    try { return await call('GET', '/api/host/session'); } catch { return null; }
  },

  async lock() {
    if (!(await Host.available())) return false;
    await call('POST', '/api/host/lock', {});
    return true;
  },

  // Asks the passkey (the device offers PIN, fingerprint or face), then the server opens again.
  async unlock() {
    const { publicKey: pk } = await call('POST', '/api/host/unlock/options', {});
    const c = await navigator.credentials.get({ publicKey: {
      challenge: b64.toBuf(pk.challenge), rpId: pk.rpId, timeout: pk.timeout, userVerification: pk.userVerification,
      hints: pk.hints, allowCredentials: (pk.allowCredentials || []).map((x) => ({ type: x.type, id: b64.toBuf(x.id) })),
    } });
    await call('POST', '/api/host/unlock', {
      id: c.id,
      clientDataJSON: b64.fromBuf(c.response.clientDataJSON),
      authenticatorData: b64.fromBuf(c.response.authenticatorData),
      signature: b64.fromBuf(c.response.signature),
    });
    return true;
  },

  async signOut() {
    const s = await Host.session();
    const url = s && typeof s.signout_url === 'string' && s.signout_url.startsWith('/') && !s.signout_url.startsWith('//')
      ? s.signout_url : null;
    if (!url) return false;
    await forgetBrowser();
    location.replace(url);
    return true;
  },
};

export default Host;
