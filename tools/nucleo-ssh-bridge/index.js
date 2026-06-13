#!/usr/bin/env node
// nucleo-ssh-bridge — the small self-hosted companion that lets the NucleoOS web "SSH" app reach
// REAL remote SSH hosts. Browsers can't open raw TCP (SSH is TCP/22) and the Cardputer has no RAM
// to do SSH crypto, so this process — running on YOUR machine (PC / Pi / NAS / the target host) —
// terminates a WebSocket from the browser and opens the actual SSH connection with `ssh2`.
//
// No third-party / cloud service: it runs entirely on your LAN. Credentials pass browser→bridge→host
// and are held only for the life of the session (never written to disk). Protect it with the token.
//
//   npm install && NUCLEO_SSH_TOKEN=yourtoken node index.js      (or just `node index.js` → it prints a token)
//
// Protocol (one WebSocket == one SSH session):
//   client→bridge : TEXT frames = JSON control  | BINARY frames = keystrokes for the shell
//     {t:'auth', token}                              first message; bad token → closed
//     {t:'connect', host, port?, username, password?, privateKey?, passphrase?, cols?, rows?}
//     {t:'resize', cols, rows}
//     {t:'exec', id, cmd}                            non-interactive command (the agent seam)
//     {t:'disconnect'}
//   bridge→client : TEXT = JSON control            | BINARY = shell output
//     {t:'ready'} {t:'hostkey',fingerprint,hash} {t:'status',state,msg} {t:'exec-result',id,code,stdout,stderr} {t:'error',msg}

'use strict';
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
let WebSocketServer, SSHClient;
try { WebSocketServer = require('ws').WebSocketServer; SSHClient = require('ssh2').Client; }
catch (e) { console.error('\n[nucleo-ssh-bridge] Missing deps. Run:  npm install\n'); process.exit(1); }

const PORT = parseInt(process.env.NUCLEO_SSH_PORT || process.argv[2] || '8022', 10);
const HOST = process.env.NUCLEO_SSH_BIND || '0.0.0.0';
const MAX_EXEC_BYTES = 256 * 1024;

// ── token: env → .token file → generate+persist+print ──────────────────────────
function resolveToken() {
  if (process.env.NUCLEO_SSH_TOKEN) return process.env.NUCLEO_SSH_TOKEN.trim();
  const f = path.join(__dirname, '.token');
  try { const t = fs.readFileSync(f, 'utf8').trim(); if (t) return t; } catch {}
  const t = crypto.randomBytes(16).toString('hex');
  try { fs.writeFileSync(f, t, { mode: 0o600 }); } catch {}
  return t;
}
const TOKEN = resolveToken();
const safeEq = (a, b) => { try { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && crypto.timingSafeEqual(x, y); } catch { return false; } };

// ── tiny health endpoint so the app can probe "is the bridge up?" (no token needed; no secrets) ──
const server = http.createServer((req, res) => {
  res.setHeader('access-control-allow-origin', '*');
  if (req.url && req.url.startsWith('/health')) { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ ok: true, service: 'nucleo-ssh-bridge', version: '0.1.0' })); return; }
  res.writeHead(404); res.end('nucleo-ssh-bridge');
});
const wss = new WebSocketServer({ server });

wss.on('connection', (ws, req) => {
  const peer = (req.socket && req.socket.remoteAddress) || '?';
  let authed = false, conn = null, stream = null, connecting = false;
  const ctlSend = (o) => { try { ws.send(JSON.stringify(o)); } catch {} };
  const log = (...a) => console.log('[' + new Date().toISOString().slice(11, 19) + ']', peer, ...a);

  function cleanup() { try { if (stream) stream.end(); } catch {} try { if (conn) conn.end(); } catch {} stream = null; conn = null; }

  function doConnect(m) {
    if (connecting || conn) { ctlSend({ t: 'error', msg: 'già connesso' }); return; }
    if (!m.host || !m.username) { ctlSend({ t: 'error', msg: 'host e username obbligatori' }); return; }
    connecting = true;
    ctlSend({ t: 'status', state: 'connecting', msg: m.username + '@' + m.host + ':' + (m.port || 22) });
    conn = new SSHClient();
    conn.on('ready', () => {
      connecting = false;
      ctlSend({ t: 'status', state: 'authenticated' });
      conn.shell({ term: 'xterm-256color', cols: m.cols || 80, rows: m.rows || 24 }, (err, s) => {
        if (err) { ctlSend({ t: 'error', msg: 'shell: ' + err.message }); cleanup(); return; }
        stream = s;
        ctlSend({ t: 'status', state: 'connected' });
        s.on('data', (d) => { try { ws.send(d, { binary: true }); } catch {} });
        if (s.stderr) s.stderr.on('data', (d) => { try { ws.send(d, { binary: true }); } catch {} });
        s.on('close', () => { ctlSend({ t: 'status', state: 'closed', msg: 'shell terminata' }); cleanup(); });
      });
    });
    conn.on('error', (err) => { connecting = false; ctlSend({ t: 'status', state: 'error', msg: err.message }); cleanup(); });
    conn.on('close', () => { ctlSend({ t: 'status', state: 'closed' }); });
    conn.on('keyboard-interactive', (name, inst, lang, prompts, cb) => cb(prompts.map(() => m.password || '')));
    const cfg = {
      host: m.host, port: m.port || 22, username: m.username, readyTimeout: 20000, keepaliveInterval: 15000,
      tryKeyboard: true,
      // TOFU-lite: accept the host key but report its fingerprint so the UI can show it (strict pinning = TODO).
      hostVerifier: (key) => { try { const fp = crypto.createHash('sha256').update(key).digest('base64').replace(/=+$/, ''); ctlSend({ t: 'hostkey', hash: 'SHA256', fingerprint: fp }); } catch {} return true; },
    };
    if (m.password) cfg.password = m.password;
    if (m.privateKey) { cfg.privateKey = m.privateKey; if (m.passphrase) cfg.passphrase = m.passphrase; }
    if (!m.password && !m.privateKey) { ctlSend({ t: 'error', msg: 'serve password o chiave privata' }); connecting = false; cleanup(); return; }
    try { conn.connect(cfg); } catch (e) { connecting = false; ctlSend({ t: 'status', state: 'error', msg: String(e.message || e) }); cleanup(); }
  }

  // The AGENT SEAM: run one command non-interactively on the live connection, capture stdout/stderr/exit.
  function doExec(m) {
    if (!conn) { ctlSend({ t: 'exec-result', id: m.id, code: -1, stdout: '', stderr: 'non connesso' }); return; }
    conn.exec(String(m.cmd || ''), (err, s) => {
      if (err) { ctlSend({ t: 'exec-result', id: m.id, code: -1, stdout: '', stderr: err.message }); return; }
      let out = '', er = '', code = null;
      const cap = (cur, d) => (cur.length < MAX_EXEC_BYTES ? cur + d : cur);
      s.on('data', (d) => { out = cap(out, d.toString('utf8')); });
      s.stderr.on('data', (d) => { er = cap(er, d.toString('utf8')); });
      s.on('close', (c) => { code = c; ctlSend({ t: 'exec-result', id: m.id, code: code, stdout: out, stderr: er }); });
    });
  }

  ws.on('message', (data, isBinary) => {
    if (isBinary) { if (stream) { try { stream.write(data); } catch {} } return; }   // keystrokes
    let m; try { m = JSON.parse(data.toString('utf8')); } catch { return; }
    if (!authed) { if (m.t === 'auth' && safeEq(m.token, TOKEN)) { authed = true; log('authed'); ctlSend({ t: 'ready' }); } else { ctlSend({ t: 'error', msg: 'token non valido' }); ws.close(); } return; }
    switch (m.t) {
      case 'connect': doConnect(m); break;
      case 'resize': if (stream) try { stream.setWindow(m.rows || 24, m.cols || 80, 0, 0); } catch {} break;
      case 'data': if (stream && typeof m.d === 'string') try { stream.write(Buffer.from(m.d, 'base64')); } catch {} break;   // text-frame fallback for keystrokes
      case 'exec': doExec(m); break;
      case 'disconnect': cleanup(); break;
    }
  });
  ws.on('close', () => { cleanup(); log('disconnected'); });
  ws.on('error', () => cleanup());
});

server.listen(PORT, HOST, () => {
  console.log('\n  nucleo-ssh-bridge  ·  ws://<this-host>:' + PORT + '   (health: http://localhost:' + PORT + '/health)');
  console.log('  TOKEN:  ' + TOKEN + '   ← incollalo nell\'app SSH di NucleoOS');
  console.log('  In ascolto su ' + HOST + ':' + PORT + '. Ctrl+C per uscire.\n');
  // optional mDNS so the app can auto-discover the bridge on the LAN
  try {
    const { Bonjour } = require('bonjour-service');
    new Bonjour().publish({ name: 'NucleoOS SSH bridge', type: 'nucleo-ssh', port: PORT, protocol: 'tcp' });
    console.log('  mDNS: pubblicato come _nucleo-ssh._tcp\n');
  } catch { /* bonjour-service not installed → no discovery, manual host:port still works */ }
});
