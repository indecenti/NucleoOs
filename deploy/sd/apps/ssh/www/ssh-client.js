// NucleoOS SSH — browser transport to nucleo-ssh-bridge (WebSocket ↔ real SSH).
//
// The Cardputer only serves these files; the live SSH traffic goes browser → bridge → host and never
// touches the chip. Control messages are JSON text frames; shell I/O is binary frames.
//
// AGENT SEAM (predisposed, not wired to the agent app yet): every client exposes exec(cmd) for
// non-interactive commands, and registers itself in window.NucleoSSH.sessions so a future in-shell
// agent can discover a live session and run commands on the host programmatically.

export function createSSHClient({ bridgeUrl, token }) {
  let ws = null, ready = false, connected = false;
  const handlers = { data: [], status: [], hostkey: [], error: [] };
  const execWaiters = new Map(); let execSeq = 0;
  const enc = new TextEncoder();

  const on = (ev, cb) => { (handlers[ev] || (handlers[ev] = [])).push(cb); return () => { handlers[ev] = handlers[ev].filter((f) => f !== cb); }; };
  const emit = (ev, ...a) => { (handlers[ev] || []).forEach((f) => { try { f(...a); } catch {} }); };

  function open() {
    return new Promise((resolve, reject) => {
      try { ws = new WebSocket(bridgeUrl); } catch (e) { reject(new Error('URL bridge non valido: ' + bridgeUrl)); return; }
      ws.binaryType = 'arraybuffer';
      const to = setTimeout(() => { try { ws.close(); } catch {} reject(new Error('bridge non raggiungibile (' + bridgeUrl + ')')); }, 8000);
      ws.onopen = () => { try { ws.send(JSON.stringify({ t: 'auth', token: token || '' })); } catch {} };
      ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') { emit('data', new Uint8Array(ev.data)); return; }
        let m; try { m = JSON.parse(ev.data); } catch { return; }
        switch (m.t) {
          case 'ready': clearTimeout(to); ready = true; resolve(); break;
          case 'status': if (m.state === 'connected') connected = true; if (m.state === 'closed' || m.state === 'error') connected = false; emit('status', m.state, m.msg); break;
          case 'hostkey': emit('hostkey', m.fingerprint, m.hash); break;
          case 'error': clearTimeout(to); emit('error', m.msg); if (!ready) reject(new Error(m.msg || 'bridge')); break;
          case 'exec-result': { const w = execWaiters.get(m.id); if (w) { execWaiters.delete(m.id); w.resolve({ code: m.code, stdout: m.stdout, stderr: m.stderr }); } break; }
        }
      };
      ws.onerror = () => { clearTimeout(to); if (!ready) reject(new Error('errore WebSocket verso il bridge')); };
      ws.onclose = () => { ready = false; connected = false; emit('status', 'closed'); for (const w of execWaiters.values()) w.reject(new Error('connessione chiusa')); execWaiters.clear(); };
    });
  }

  async function connect(profile) { if (!ready) await open(); ws.send(JSON.stringify({ t: 'connect', ...profile })); }
  function send(strOrBytes) { if (ws && ws.readyState === 1) ws.send(typeof strOrBytes === 'string' ? enc.encode(strOrBytes) : strOrBytes); }
  function resize(cols, rows) { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'resize', cols, rows })); }
  function exec(cmd) {
    return new Promise((resolve, reject) => {
      if (!ws || ws.readyState !== 1) return reject(new Error('non connesso'));
      const id = 'e' + (++execSeq); execWaiters.set(id, { resolve, reject });
      ws.send(JSON.stringify({ t: 'exec', id, cmd: String(cmd || '') }));
      setTimeout(() => { if (execWaiters.has(id)) { execWaiters.delete(id); reject(new Error('exec timeout')); } }, 60000);
    });
  }
  function disconnect() { try { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'disconnect' })); } catch {} try { ws && ws.close(); } catch {} }

  const api = { open, connect, send, resize, exec, disconnect, on, get connected() { return connected; }, get ready() { return ready; } };
  return api;
}

// Probe whether a bridge is up (cheap, no token, no secrets). bridgeUrl is the ws:// URL.
export async function probeBridge(bridgeUrl) {
  try {
    const httpUrl = String(bridgeUrl || '').replace(/^ws/, 'http').replace(/\/+$/, '') + '/health';
    const r = await fetch(httpUrl, { cache: 'no-store', signal: AbortSignal.timeout(3000) });
    return r.ok;
  } catch { return false; }
}

// Agent seam: a global registry of live SSH sessions, so a future NucleoOS agent can discover one and
// call session.exec('...'). Populated by the app on connect; intentionally NOT consumed by the agent
// app yet (predisposition only).
if (typeof window !== 'undefined') {
  window.NucleoSSH = window.NucleoSSH || { sessions: [], createClient: createSSHClient };
}
