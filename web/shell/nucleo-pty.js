/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS — browser client for the device's OWN terminal bridge (/api/pty).
//
// This replaces the "run a bridge on some other machine" model the SSH app was
// built around (ws://localhost:8022 — a service that existed nowhere in this
// project, so the app could never connect to anything). The console IS the
// machine: server.py forks a pty on it. `kind=shell` gives the device's login
// shell, `kind=ssh` runs the real OpenSSH client inside that pty — which is why
// passwords, host-key prompts and 2FA all just work: they are typed into the
// terminal, and never become a form field this server has to be trusted with.
//
// Two consequences worth knowing:
//   * Same origin, so an https:// (:8443) page opens wss:// — the mixed-content
//     wall that made the old bridge unusable from the secure origin is gone.
//   * No token. The socket sits behind the device pairing gate; a second secret
//     parked in localStorage would add the look of security and none of it.
//
// Frames: JSON text for control, binary for the byte stream, both directions.
//   -> {t:'auth'}                      <- {t:'ready', kind, caps}
//   -> {t:'connect', ...profile}       <- {t:'status', state:'connecting'|'connected'|'closed'}
//   -> {t:'resize', cols, rows}        <- {t:'error', msg}
//   -> {t:'exec', id, cmd}             <- {t:'exec-result', id, code, stdout, stderr}
//   -> {t:'disconnect'}

const wsOrigin = () => (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host;

export const PTY_UNPAIRED = 'pairing_required';

/** Ask what the device's terminal can do, without opening a session.
 *  Returns {ok, available, kinds, sessions, max, shell, ssh} or null if unreachable. */
export async function ptyCaps() {
  try {
    const r = await fetch('/api/pty/caps', { cache: 'no-store' });
    if (!r.ok) return null;
    return await r.json();
  } catch { return null; }
}

export function createPtyClient({ kind = 'shell' } = {}) {
  let ws = null, ready = false, connected = false, caps = null;
  const handlers = { data: [], status: [], error: [], ready: [] };
  const execWaiters = new Map(); let execSeq = 0;
  const enc = new TextEncoder();

  const on = (ev, cb) => { (handlers[ev] || (handlers[ev] = [])).push(cb); return () => { handlers[ev] = handlers[ev].filter((f) => f !== cb); }; };
  const emit = (ev, ...a) => { (handlers[ev] || []).forEach((f) => { try { f(...a); } catch {} }); };

  function open() {
    return new Promise((resolve, reject) => {
      const url = wsOrigin() + '/api/pty?kind=' + encodeURIComponent(kind);
      try { ws = new WebSocket(url); } catch (e) { reject(new Error('WebSocket non apribile: ' + e.message)); return; }
      ws.binaryType = 'arraybuffer';
      const to = setTimeout(() => { try { ws.close(); } catch {} reject(new Error('il dispositivo non risponde')); }, 10000);
      // A 401 on the upgrade closes the socket before it ever opens, and the
      // browser refuses to tell JS the status code. Ask the plain-HTTP twin what
      // happened so the app can say "accoppia questo browser" instead of the
      // uselessly generic "connessione fallita".
      const explain = async () => {
        try {
          const r = await fetch('/api/pty/caps', { cache: 'no-store' });
          if (!r.ok) return new Error('bridge non disponibile (HTTP ' + r.status + ')');
          const j = await r.json();
          if (!j.available) return new Error('questo dispositivo non offre un terminale');
          if (j.paired === false) return new Error(PTY_UNPAIRED);
          if (j.sessions >= j.max) return new Error('troppe sessioni aperte (max ' + j.max + ')');
          return new Error('il terminale ha rifiutato la connessione');
        } catch { return new Error('dispositivo non raggiungibile'); }
      };
      ws.onopen = () => { try { ws.send(JSON.stringify({ t: 'auth' })); } catch {} };
      ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') { emit('data', new Uint8Array(ev.data)); return; }
        let m; try { m = JSON.parse(ev.data); } catch { return; }
        switch (m.t) {
          case 'ready': clearTimeout(to); ready = true; caps = m.caps || null; emit('ready', caps); resolve(caps); break;
          case 'status':
            if (m.state === 'connected') connected = true;
            if (m.state === 'closed') connected = false;
            emit('status', m.state, m.msg);
            break;
          case 'error': emit('error', m.msg || 'errore'); if (!ready) { clearTimeout(to); reject(new Error(m.msg || 'errore')); } break;
          case 'exec-result': {
            const w = execWaiters.get(m.id);
            if (w) { execWaiters.delete(m.id); w.resolve({ code: m.code, stdout: m.stdout, stderr: m.stderr }); }
            break;
          }
        }
      };
      ws.onerror = () => {};   // onclose always follows; explaining once is enough
      ws.onclose = () => {
        const wasReady = ready;
        ready = false; connected = false;
        for (const w of execWaiters.values()) w.reject(new Error('connessione chiusa'));
        execWaiters.clear();
        if (!wasReady) { clearTimeout(to); explain().then(reject); }
        else emit('status', 'closed');
      };
    });
  }

  async function connect(profile = {}) {
    if (!ready) await open();
    ws.send(JSON.stringify({ t: 'connect', ...profile }));
  }
  function send(strOrBytes) {
    if (ws && ws.readyState === 1) ws.send(typeof strOrBytes === 'string' ? enc.encode(strOrBytes) : strOrBytes);
  }
  function resize(cols, rows) { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'resize', cols, rows })); }
  function exec(cmd) {
    return new Promise((resolve, reject) => {
      if (!ws || ws.readyState !== 1) return reject(new Error('non connesso'));
      const id = 'e' + (++execSeq);
      execWaiters.set(id, { resolve, reject });
      ws.send(JSON.stringify({ t: 'exec', id, cmd: String(cmd || '') }));
      setTimeout(() => { if (execWaiters.has(id)) { execWaiters.delete(id); reject(new Error('exec timeout')); } }, 65000);
    });
  }
  function disconnect() {
    try { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'disconnect' })); } catch {}
    try { ws && ws.close(); } catch {}
  }

  return { open, connect, send, resize, exec, disconnect, on,
           get connected() { return connected; }, get ready() { return ready; }, get caps() { return caps; } };
}

// Agent seam: live sessions register here so the in-shell agent can find one and
// call session.exec('…') on the device (or on a host reached over ssh with a key).
if (typeof window !== 'undefined') {
  window.NucleoPty = window.NucleoPty || { sessions: [], createClient: createPtyClient };
}
