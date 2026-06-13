// Zero-dependency dev simulator of the NucleoOS device. Mirrors the firmware:
//   /api/status /api/apps /api/associations  (simulated OS endpoints)
//   /api/fs/{list,read,write,delete,mkdir,move}  (on tools/sd-sim)
//   /ws                                       (live event deltas; minimal WS)
//   /apps/<id>/<rest> -> apps/<id>/www/<rest> ;  /<asset> -> web/shell/<asset>
// Usage: node tools/serve-shell.mjs   (http://localhost:5599)
import { createServer } from 'node:http';
import { readFile, writeFile, readdir, stat, rm, mkdir, rename } from 'node:fs/promises';
import { readFileSync, existsSync } from 'node:fs';   // sync reads for the ANIMA agenda/capabilities executor
import { createHash, randomBytes, randomInt } from 'node:crypto';
import { join, dirname, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Mind } from './anima/hdc.mjs';   // hyperdimensional reasoning core (offline compose/recall/analogy)
import { KG } from './anima/kge.mjs';      // permutation-KGE deductive core (inverse/transitive/multi-hop)
import { answer as combinatorRun, parseQuery as combinatorParse } from './anima/combinator.mjs';   // neuro-symbolic combinators
import { parseWeather, formatWeather } from './anima/weather.mjs';   // shared weather NLU (mirrors firmware)
import { translateAnswer, isTranslateRequest } from './anima/translate.mjs';   // offline IT<->EN translator (twin of nucleo_anima_translate.c)

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const SHELL = join(REPO, 'web', 'shell');
const SD = join(REPO, 'tools', 'sd-sim');
const PORT = 5599;
// ANIMA app-launch vocabulary — single source of truth shared verbatim with the firmware,
// which generates its APP_ALIAS[] from this same file (tools/anima/gen_aliases.py). Editing the
// JSON keeps the device and this simulator resolving "apri <app>" through one vocabulary.
const APP_ALIASES = (() => {
  try { return JSON.parse(readFileSync(join(REPO, 'registry', 'app-aliases.json'), 'utf8')).aliases || {}; }
  catch { return {}; }
})();
const TYPES = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.svg': 'image/svg+xml', '.wasm': 'application/wasm',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif',
  '.mp3': 'audio/mpeg', '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.txt': 'text/plain', '.md': 'text/plain' };
const jread = async (p) => JSON.parse(await readFile(join(REPO, p), 'utf8'));

// ---- live events over WebSocket ----
const sockets = new Set();
let seq = 0;
const wsEncode = (s) => {
  const d = Buffer.from(s); const len = d.length;
  let h;
  if (len < 126) h = Buffer.from([0x81, len]);
  else if (len < 65536) { h = Buffer.alloc(4); h[0] = 0x81; h[1] = 126; h.writeUInt16BE(len, 2); }
  else { h = Buffer.alloc(10); h[0] = 0x81; h[1] = 127; h.writeBigUInt64BE(BigInt(len), 2); }
  return Buffer.concat([h, d]);
};
const publish = (t, d) => {
  const msg = JSON.stringify({ t, seq: ++seq, d });
  for (const s of sockets) try { s.write(wsEncode(msg)); } catch {}
};

// ---- simulated OS endpoints ----
async function apiApps() {
  const { installed } = await jread('registry/apps.json');
  const apps = [];
  for (const a of installed) {
    let m = {}; try { m = await jread(`apps/${a.id}/manifest.json`); } catch {}
    apps.push({ id: a.id, name: m.name || a.id, route: m.web_route || '', icon: m.icon || '', enabled: a.enabled });
  }
  return { apps };
}
const apiStatus = { os: 'NucleoOS', version: '0.1.0', uptime_s: 0, free_heap: 210000,
  min_free_heap: 17000, largest_free_block: 42000,
  storage: { mounted: true, fs: 'exFAT', total_bytes: 63864569856, free_bytes: 63800000000 },
  network: { mode: 'sta', ssid: 'home-wifi', ip: '192.168.1.42', time_synced: true }, apps: { installed: 9 },
  ota: { running: 'factory', next: 'ota_0', state: 'valid', rollback_enabled: true },
  arbiter: { busy: false, job: '', held_ms: 0, waiters: 0, grants: 4, denials: 0, yields: 0, heap_free_min: 12800 } };

// ---- Settings/Control-Center live-diagnostics + device-state mocks (mirror the firmware shapes the
// Control Center app reads). Module-level state so toggles round-trip in the preview. ----
const simState = {
  bootMs: Date.now(),
  minFree: 17000,                 // drifts slowly downward like the real watermark
  l1Mode: 'auto', externalBrain: false,
  ttsEnabled: false,
  voiceAlwaysOn: false,
  logs: [],                       // appended to by sim actions; served oldest→newest as text/plain
  anima: { q: 0, none: 0, cmd: 0, fact: 0, stitch: 0, remote: 0, last_conf: 0, last: '-' },  // grows on /api/diag for the Log Viewer
  oom: 0,                         // stays 0 (healthy); bump by hand to exercise the OOM flag
};
function simLog(line) { simState.logs.push(line); if (simState.logs.length > 60) simState.logs.shift(); }
simLog('I (220) boot: NucleoOS 0.1.0, reset reason POWERON');
simLog('I (640) wifi: connected to home-wifi, ip=192.168.1.42');
simLog('I (910) anima: L1 index loaded (AKB2), 18.2 KB free internal');
const jit = (base, spread) => Math.max(0, Math.round(base + (Math.random() * 2 - 1) * spread));

// Mock the firmware OTA endpoint so the Updates app can be exercised end-to-end (upload
// progress + reboot-poll). It just drains the body and reports success — no real flashing.
async function otaApi(req, res) {
  let bytes = 0;
  req.on('data', (c) => { bytes += c.length; });
  req.on('end', () => {
    if (bytes === 0) { send(res, 400, 'text/plain', 'empty image'); return; }
    apiStatus.ota = { running: 'ota_0', next: 'ota_1', state: 'pending', rollback_enabled: true };  // as after a real OTA
    sendJSON(res, { ok: true, bytes, slot: 'ota_0', reboot: true });
  });
}

// ---- LLM proxy (same-origin -> Groq / any OpenAI-compatible endpoint) -----------------------
// The chat app posts here instead of hitting the provider directly, so the browser never faces a
// CORS wall (mirrors the firmware /api/llm). Forwards the client's Authorization header + body to
// ?url=<provider endpoint> and streams the response (SSE) straight back. Node has no CORS limits.
async function llmApi(req, res, url) {
  const target = url.searchParams.get('url') || '';
  if (!/^https:\/\//i.test(target)) return send(res, 400, 'text/plain', 'bad url');
  const headers = { 'Content-Type': 'application/json' };
  const auth = req.headers['authorization'];
  if (auth) headers['Authorization'] = auth;
  const body = req.method === 'POST' ? await readBody(req) : undefined;
  try {
    const up = await fetch(target, { method: req.method, headers, body });
    res.writeHead(up.status, { 'content-type': up.headers.get('content-type') || 'application/json', 'access-control-allow-origin': '*' });
    if (up.body) { const reader = up.body.getReader(); for (;;) { const { value, done } = await reader.read(); if (done) break; res.write(Buffer.from(value)); } }
    res.end();
  } catch (e) { send(res, 502, 'application/json', JSON.stringify({ error: String(e.message || e) })); }
}

// ---- page/file proxy (same-origin -> any http(s) URL) ---------------------------------------
// Mirrors the firmware /api/proxy: the Browser app's SD downloader fetches a remote URL through
// here so the browser never hits a CORS wall (the device serves the app). Streams the body back
// with the upstream Content-Type. Node has no CORS limits, so this just relays the bytes.
async function proxyApi(req, res, url) {
  const target = url.searchParams.get('url') || '';
  if (!/^https?:\/\//i.test(target)) return send(res, 400, 'text/plain', 'url must be http(s)');
  try {
    const up = await fetch(target, { redirect: 'follow', headers: { 'user-agent': 'Mozilla/5.0 NucleoOS-sim' } });
    res.writeHead(up.status, { 'content-type': up.headers.get('content-type') || 'application/octet-stream', 'access-control-allow-origin': '*' });
    if (up.body) { const reader = up.body.getReader(); for (;;) { const { value, done } = await reader.read(); if (done) break; res.write(Buffer.from(value)); } }
    res.end();
  } catch (e) { send(res, 502, 'text/plain', String(e.message || e)); }
}

// ---- recorder API (simulates nucleo_recorder: PDM mic -> WAV on SD) ----
// The device writes real PCM; here we synthesize a tone so the Recorder app can be
// exercised end-to-end (timer, level meter, WAV playback, in-browser MP3 convert).
const REC_RATE = 16000, REC_DIR = '/data/Recordings';
let rec = { recording: false, path: '', startedAt: 0, level: 0, timer: null };
const recSeconds = () => rec.recording ? Math.floor((nowMs() - rec.startedAt) / 1000) : rec.lastSeconds || 0;
let _t = 0; const nowMs = () => (_t += 137);   // monotonic fake clock (Date.now unused for determinism)

function makeWav(seconds) {
  const n = REC_RATE * seconds, data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {                 // 440 Hz sine, fades so it sounds like "something"
    const v = Math.sin(2 * Math.PI * 440 * i / REC_RATE) * 0.3 * 32767;
    data.writeInt16LE(v | 0, i * 2);
  }
  const h = Buffer.alloc(44); h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4);
  h.write('WAVE', 8); h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20);
  h.writeUInt16LE(1, 22); h.writeUInt32LE(REC_RATE, 24); h.writeUInt32LE(REC_RATE * 2, 28);
  h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  return Buffer.concat([h, data]);
}

async function recApi(req, res, url) {
  const op = url.pathname.split('/').pop();
  if (op === 'status') return sendJSON(res, { recording: rec.recording, path: rec.path, seconds: recSeconds(), level: rec.level, rate: REC_RATE });
  if (op === 'start') {
    if (rec.recording) return send(res, 409, 'text/plain', 'already recording');
    await mkdir(sdPath(REC_DIR), { recursive: true });
    rec = { recording: true, path: `${REC_DIR}/rec-${++seq}.wav`, startedAt: nowMs(), level: 0, timer: null };
    rec.timer = setInterval(() => { rec.level = 40 + Math.round(40 * Math.abs(Math.sin(recSeconds()))); publish('rec.level', { path: rec.path, level: rec.level }); }, 400);
    publish('rec.started', { path: rec.path });
    return sendJSON(res, { ok: true, recording: true, path: rec.path });
  }
  if (op === 'stop') {
    if (!rec.recording) return sendJSON(res, { ok: true, recording: false, path: rec.path });
    const secs = Math.max(1, recSeconds());
    clearInterval(rec.timer);
    await writeFile(sdPath(rec.path), makeWav(secs));
    rec.recording = false; rec.lastSeconds = secs; rec.level = 0;
    publish('rec.stopped', { path: rec.path, seconds: secs, bytes: secs * REC_RATE * 2 });
    publish('fs.changed', { op: 'write', path: REC_DIR });
    return sendJSON(res, { ok: true, recording: false, path: rec.path });
  }
  if (op === 'stream') {
    // Live PCM stream (16 kHz mono int16 LE), mirroring the firmware /api/rec/stream. The device
    // sends real mic audio; here we emit a 440 Hz tone so the dictation wiring (fetch reader ->
    // int16->float -> Vosk) can be exercised on the host. Runs until the client disconnects.
    res.writeHead(200, { 'content-type': 'application/octet-stream', 'cache-control': 'no-store', 'access-control-allow-origin': '*' });
    let i = 0, alive = true;
    const stop = () => { alive = false; };
    req.on('close', stop); req.on('aborted', stop);
    const tick = () => {
      if (!alive || res.writableEnded) return;
      const buf = Buffer.alloc(512 * 2);        // 512 samples = 32 ms of audio at 16 kHz
      for (let s = 0; s < 512; s++) { buf.writeInt16LE((Math.sin(2 * Math.PI * 440 * i / REC_RATE) * 0.2 * 32767) | 0, s * 2); i++; }
      res.write(buf);
      setTimeout(tick, 32);
    };
    tick();
    return;
  }
  return send(res, 404, 'text/plain', '404');
}

// ---- voice command engine (simulates nucleo_voice: on-device PTT DTW recognizer) ----
// The real device records on a held GO/FN key and matches MFCC templates entirely on-device;
// here we only simulate the LIFECYCLE so the Voice Manager wizard + "Prova" tab can be exercised
// headless on a PC: a learn arms → goes listening → saves a stub template → publishes voice/learned,
// and a sample matcher publishes voice/match telemetry so the live feed and the Ottimo/Buono/Debole
// classifier can be seen. There is no real DSP here (that is host-verified in tools/voice-host).
const VOICE_DIR = '/system/voice';
let _vm = 0;
function simMatch(word) {
  // Cycle through distances that land in each confidence tier (mirrors app.js classify()).
  const samples = [
    { dist: 1850, second: 5200, radius: 6000 },  // Ottimo
    { dist: 3200, second: 5600, radius: 6000 },  // Buono
    { dist: 4200, second: 5200, radius: 5000 },  // Discreto
    { dist: 5400, second: 6000, radius: 5200 },  // Debole
  ];
  publish('voice/match', { word, ...samples[_vm++ % samples.length] });
}

async function voiceApi(req, res, url) {
  const op = url.pathname.split('/').pop();
  if (op === 'learn') {
    let word = '';
    try { word = String(JSON.parse((await readBody(req)).toString('utf8')).word || '').trim(); } catch {}
    if (!word) return sendJSON(res, { ok: false });
    publish('voice/state', { listening: true });            // device confirms it heard the FN press
    setTimeout(async () => {
      try {
        await mkdir(sdPath(VOICE_DIR), { recursive: true });
        await writeFile(sdPath(`${VOICE_DIR}/${word}.tpl`), Buffer.alloc(3400));  // stub template
      } catch {}
      publish('voice/state', { listening: false });
      publish('fs.changed', { op: 'write', path: VOICE_DIR });
      publish('voice/learned', { status: 'ok' });
      setTimeout(() => simMatch(word), 400);                 // a sample recognition for the Prova tab
    }, 900);
    return sendJSON(res, { ok: true });
  }
  // (/api/voice/always is handled inline above — it also tracks simState.voiceAlwaysOn.)
  if (op === 'match') {   // SIM-ONLY dev hook: POST /api/voice/match?word=foo emits a voice/match event
    simMatch(url.searchParams.get('word') || 'prova');
    return sendJSON(res, { ok: true });
  }
  return send(res, 404, 'text/plain', '404');
}

// ---- transcription + summary pipeline (mirrors the firmware /api/transcribe) ----
// Real device: reads the MP3 from SD, POSTs it to Groq Whisper (auto language detect, falls back to
// the requested/system language), then asks the cloud teacher (Grok) for a summary IN THAT LANGUAGE,
// and writes two sidecars next to the audio: <base>.txt (transcript) and <base>.sum.txt (summary).
// The simulator fakes the cloud calls but writes the SAME sidecars so the web app is exercised fully.
async function transcribeApi(req, res, url) {
  const p = url.searchParams.get('path') || '';
  const langReq = (url.searchParams.get('lang') || 'auto').toLowerCase();
  const doSum = url.searchParams.get('summarize') !== '0';
  if (!/\.(mp3|wav)$/i.test(p)) return sendJSON(res, { ok: false, error: 'not an audio file' });
  const abs = sdPath(p); if (!abs) return sendJSON(res, { ok: false, error: 'bad path' });
  const base = p.replace(/\.(mp3|wav)$/i, '');
  // Use an existing transcript (e.g. from live dictation) if present; else a canned sample.
  let text = '';
  try { text = await readFile(sdPath(base + '.txt'), 'utf8'); } catch {}
  if (!text.trim()) {
    text = 'Promemoria: comprare il latte e il pane. Richiamare Marco domani mattina per il preventivo. '
         + 'Finire il report entro venerdì e inviarlo al team. Idea: provare la trascrizione automatica nel registratore.';
  }
  // language: 'auto' pretends Whisper detected Italian; otherwise honour the requested language.
  const language = langReq === 'auto' ? 'it' : (langReq === 'en' ? 'en' : 'it');
  let summary = '';
  if (doSum) {
    summary = (language === 'en'
      ? '• Buy milk and bread\n• Call Marco tomorrow morning about the quote\n• Finish the report by Friday and send it to the team\n• Idea: try auto-transcription in the recorder'
      : '• Comprare latte e pane\n• Richiamare Marco domani mattina per il preventivo\n• Finire il report entro venerdì e inviarlo al team\n• Idea: provare la trascrizione automatica nel registratore');
  }
  try {
    await mkdir(sdPath(REC_DIR), { recursive: true });
    await writeFile(sdPath(base + '.txt'), text);
    if (summary) await writeFile(sdPath(base + '.sum.txt'), summary);
    publish('fs.changed', { op: 'write', path: REC_DIR });
  } catch {}
  return sendJSON(res, { ok: true, text, language, summary });
}

// ---- file API (sandboxed to tools/sd-sim) ----
const sdPath = (p) => { const abs = normalize(join(SD, p || '/')); return abs.startsWith(SD) ? abs : null; };
// Collect the body as a Buffer (NOT a string): string concatenation mangles binary
// uploads (e.g. an .exe), so the OTA push would see every binary as "changed" forever.
// Rejecting on 'error' keeps the Promise from hanging if the socket dies mid-upload.
const readBody = (req) => new Promise((resolve, reject) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => resolve(Buffer.concat(chunks)));
  req.on('error', reject);
});

async function fsApi(req, res, url) {
  const p = url.searchParams.get('path') || '/';
  const abs = sdPath(p);
  if (!abs) return send(res, 400, 'text/plain', 'bad path');
  const op = url.pathname.split('/').pop();
  try {
    if (op === 'list') {
      let names = [];
      try { names = await readdir(abs); }
      catch (e) { if (e.code === 'ENOENT') return sendJSON(res, { entries: [] }); throw e; }  // missing dir = empty (matches firmware)
      const entries = [];
      for (const name of names) { const st = await stat(join(abs, name)); entries.push({ name, type: st.isDirectory() ? 'dir' : 'file', size: st.size }); }
      return sendJSON(res, { entries });
    }
    if (op === 'read') {
      // Serve with HTTP Range support so the <audio>/<video> elements can SEEK (the device
      // streams from SD directly, so this only matters for the browser simulator). Files here
      // are small (<=~1.5 MB), so we read once and slice.
      const type = TYPES[extname(abs).toLowerCase()] || 'application/octet-stream';
      const data = await readFile(abs);
      const range = req.headers.range && /bytes=(\d*)-(\d*)/.exec(req.headers.range);
      if (range) {
        let start = range[1] ? parseInt(range[1], 10) : 0;
        let end = range[2] ? parseInt(range[2], 10) : data.length - 1;
        if (!(start >= 0)) start = 0;
        if (!(end < data.length)) end = data.length - 1;
        if (start > end) { res.writeHead(416, { 'content-range': `bytes */${data.length}`, 'access-control-allow-origin': '*' }); return res.end(); }
        res.writeHead(206, { 'content-type': type, 'accept-ranges': 'bytes',
          'content-range': `bytes ${start}-${end}/${data.length}`, 'content-length': end - start + 1, 'access-control-allow-origin': '*' });
        return res.end(data.subarray(start, end + 1));
      }
      res.writeHead(200, { 'content-type': type, 'accept-ranges': 'bytes', 'content-length': data.length, 'access-control-allow-origin': '*' });
      return res.end(data);
    }
    if (op === 'write') { await writeFile(abs, await readBody(req)); publish('fs.changed', { op: 'write', path: p }); return sendJSON(res, { ok: true }); }
    if (op === 'delete') { await rm(abs, { recursive: true }); publish('fs.changed', { op: 'delete', path: p }); return sendJSON(res, { ok: true }); }
    if (op === 'mkdir') { await mkdir(abs, { recursive: true }); publish('fs.changed', { op: 'mkdir', path: p }); return sendJSON(res, { ok: true }); }
    if (op === 'move') {
      const from = sdPath(url.searchParams.get('from') || '');
      const to = sdPath(url.searchParams.get('to') || '');
      if (!from || !to) return send(res, 400, 'text/plain', 'bad path');
      const overwrite = url.searchParams.get('overwrite') === '1';
      let exists = true; try { await stat(to); } catch { exists = false; }
      if (exists) { if (!overwrite) return send(res, 400, 'text/plain', 'destination exists'); await rm(to, { recursive: true }); }
      await mkdir(dirname(to), { recursive: true });
      await rename(from, to);
      publish('fs.changed', { op: 'move', path: url.searchParams.get('to') });
      return sendJSON(res, { ok: true });
    }
  } catch (e) { return send(res, 404, 'text/plain', String(e.code || e.message)); }
  send(res, 404, 'text/plain', '404');
}

// ---- helpers ----
const send = (res, code, type, body) => { res.writeHead(code, { 'content-type': type, 'access-control-allow-origin': '*' }); res.end(body); };
const sendJSON = (res, obj) => send(res, 200, 'application/json; charset=utf-8', JSON.stringify(obj));
async function sendFile(res, abs) {
  try { send(res, 200, TYPES[extname(abs)] || 'application/octet-stream', await readFile(abs)); }
  catch { send(res, 404, 'text/plain', '404'); }
}

// ---- pairing / session auth (mirrors firmware nucleo_auth) ----
// The device requires pairing: a 6-digit PIN is shown on the Cardputer SCREEN, and a client
// proves physical proximity by entering it (Chromecast-style). On success the device mints a
// session token delivered as an HttpOnly cookie, so every later request — including from app
// iframes and the WebSocket handshake — is authenticated by the browser automatically, with
// zero changes to the apps. Tokens persist (here: in RAM; on device: /cfg/config/auth.json).
// Protected: /api/fs, /api/ota, /api/rec, /ws. Public: static assets + /api/status|apps|
// associations (needed to load the shell and show the pairing overlay before auth).
const COOKIE = 'nucleo_session';
const AUTH = {
  required: true,                              // matches settings.security.require_pairing
  pin: String(randomInt(0, 1000000)).padStart(6, '0'),  // regenerated each boot; only new pairings need it
  tokens: new Set(),                           // valid session tokens (persisted on the real device)
  fails: 0, lockUntil: 0,                      // brute-force guard on the 6-digit PIN
};
console.log(`[auth] pairing PIN (shown on Cardputer screen): ${AUTH.pin}`);
const cookieToken = (req) => {
  const raw = req.headers.cookie || '';
  for (const part of raw.split(';')) { const [k, v] = part.trim().split('='); if (k === COOKIE) return v; }
  return null;
};
const isAuthed = (req) => !AUTH.required || AUTH.tokens.has(cookieToken(req));
const reject401 = (res) => send(res, 401, 'application/json', JSON.stringify({ error: 'unpaired' }));

async function pairApi(req, res) {
  const now = nowMs();
  if (now < AUTH.lockUntil) return send(res, 429, 'application/json', JSON.stringify({ error: 'locked', retry_ms: AUTH.lockUntil - now }));
  let pin = '';
  try { pin = String(JSON.parse((await readBody(req)).toString('utf8')).pin || ''); } catch {}
  if (pin && pin === AUTH.pin) {
    AUTH.fails = 0;
    const token = randomBytes(24).toString('hex');
    AUTH.tokens.add(token);
    res.writeHead(200, { 'content-type': 'application/json', 'access-control-allow-origin': '*',
      'set-cookie': `${COOKIE}=${token}; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax` });
    return res.end(JSON.stringify({ ok: true }));
  }
  AUTH.fails++;
  if (AUTH.fails >= 5) { AUTH.lockUntil = now + 30000; AUTH.fails = 0; }   // lock 30s after 5 misses
  return send(res, 401, 'application/json', JSON.stringify({ error: 'bad pin', locked: now < AUTH.lockUntil }));
}

// ---- SAFETY GUARD: is this input worth escalating to the knowledge/entity/teacher tiers? ----
// A bare DATE / number / time / cell-ref / code / wordless string is NOT a question. It must NEVER
// be sent to the online teacher (which would fabricate a "fact") nor learned. The deterministic
// L0/math/command tiers still run before this gate; this only fences off the knowledge escalation.
function isAskable(q) {
  const t = String(q || '').trim();
  if (t.length < 3) return false;
  if (/^\d{1,4}\s*[:/.\-]\s*\d{1,2}\s*[:/.\-]\s*\d{1,4}$/.test(t)) return false;          // date: 24:04:2027, 24/04/2027, 2027-04-24
  if (/^\d{1,2}\s*[:.]\s*\d{2}(\s*[:.]\s*\d{2})?$/.test(t)) return false;                  // time: 14:30
  if (/^[\s\d.,;:()%+\-*/x×÷€$£°#°'"]+$/.test(t) && /\d/.test(t)) return false;            // pure number / symbols
  if (/^\$?[a-z]{1,3}\$?\d+(\s*:\s*\$?[a-z]{1,3}\$?\d+)?$/i.test(t)) return false;          // cell ref / range: A1, A1:B10
  if (!/\s/.test(t) && t.length <= 14 && /\d/.test(t) && /[a-z]/i.test(t)) return false;   // alnum code: x7gq2, ab12cd
  if (!/[a-zàèéìòùáéíóúäëïöüñ]{3,}/i.test(t)) return false;                                 // no real word -> nothing to look up
  return true;
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const path = decodeURIComponent(url.pathname);

  // Pairing endpoints (public by nature) + the guard for protected routes.
  if (path === '/api/auth/status') return sendJSON(res, { required: AUTH.required, paired: isAuthed(req) });
  if (path === '/api/pair' && req.method === 'POST') return pairApi(req, res);
  if (path === '/api/_dev/pin') return sendJSON(res, { pin: AUTH.pin });   // SIMULATOR ONLY: stands in for "read the screen"; the firmware NEVER serves the PIN over HTTP
  if ((path.startsWith('/api/fs/') || path.startsWith('/api/rec/') || path.startsWith('/api/voice/') || (path === '/api/ota') || (path === '/api/transcribe') || (path === '/api/display')) && !isAuthed(req)) return reject401(res);

  if (path === '/api/status') { const a = await apiApps(); const up = Math.floor((Date.now() - simState.bootMs) / 1000);
    simState.minFree = Math.max(9000, simState.minFree - (Math.random() < 0.15 ? jit(120, 100) : 0));   // watermark only ever drops
    return sendJSON(res, { ...apiStatus, uptime_s: up, min_free_heap: simState.minFree, free_heap: jit(74000, 6000),
      largest_free_block: jit(42000, 5000), apps: { installed: a.apps.length },
      network: { ...apiStatus.network, time: Math.floor(Date.now() / 1000) } }); }   // live device clock for the Clock app badge
  if (path === '/api/apps') return sendJSON(res, await apiApps());
  if (path === '/api/associations') return sendJSON(res, await jread('registry/file-associations.json'));
  if (path === '/api/display') {   // mirrors the firmware: blank/relight the Cardputer screen (no-op on the sim)
    const on = url.searchParams.get('on') !== '0';
    console.log(`[display] screen ${on ? 'ON' : 'OFF'}`); simLog(`I (${Date.now() % 100000}) display: ${on ? 'ON' : 'OFF'}`);
    return sendJSON(res, { ok: true, on, brightness: 70 });
  }

  // ── live diagnostics (mirror nucleo_httpd.c shapes; public, cheap, no auth) ──
  if (path === '/api/heap') {                       // per-region heap (fragmentation investigation)
    const free = jit(74000, 8000), largest = jit(42000, 6000);
    const region = (total) => ({ total_bytes: total, free_bytes: free, allocated_bytes: total - free,
      largest_free_block: largest, min_free_bytes: simState.minFree, free_blocks: jit(34, 6), allocated_blocks: jit(900, 40),
      frag_pct: Math.max(0, Math.round(100 * (1 - largest / free))) });
    return sendJSON(res, { uptime_s: Math.floor((Date.now() - simState.bootMs) / 1000),
      internal: region(327680), dma: region(180000), default: region(327680), httpd_stack_free_min: jit(7000, 800) });
  }
  if (path === '/api/cpu') {                         // per-core load snapshot
    const spike = Math.random() < 0.18;
    const load = [jit(spike ? 70 : 14, 9), jit(spike ? 55 : 11, 8)].map((v) => Math.min(100, Math.max(0, Math.round(v * 10) / 10)));
    return sendJSON(res, { uptime_s: Math.floor((Date.now() - simState.bootMs) / 1000), cores: 2, freq_mhz: 240,
      tasks: jit(28, 3), load, load_avg: Math.round((load[0] + load[1]) / 2 * 10) / 10 });
  }
  if (path === '/api/diag') {                       // consolidated health snapshot (mirror nucleo_httpd.c diag_get)
    const up = Math.floor((Date.now() - simState.bootMs) / 1000);
    simState.minFree = Math.max(9000, simState.minFree - (Math.random() < 0.15 ? jit(120, 100) : 0));
    const free = jit(74000, 8000), lblk = jit(40000, 7000);
    const a = simState.anima;                       // grow the counters so the live probe shows ANIMA activity
    a.q += jit(2, 2); a.cmd += jit(1, 1); if (Math.random() < 0.3) a.fact += 1;
    if (Math.random() < 0.18) a.none += 1; if (Math.random() < 0.12) a.remote += 1;
    a.last_conf = jit(70, 25); a.last = ['greet', 'time', 'recall', 'miss', 'launch'][Math.floor(Math.random() * 5)];
    return sendJSON(res, {
      v: 1, ts: Math.floor(Date.now() / 1000),
      sys: { fw: '0.2.0', proj: 'nucleoos', built: 'Jun 9 2026', idf: 'v5.4', uptime_s: up,
             reset: 'SW', slot: 'ota_0', ota: 'valid', sd: true, sd_free: 7.1e9, sd_total: 14.8e9 },
      mem: { free, min: simState.minFree, lblk, frag: Math.max(0, Math.round(100 * (1 - lblk / free))),
             dma_free: jit(150000, 9000), stack_httpd: jit(7000, 800) },
      net: { mode: 'sta', ssid: 'home-wifi', ip: '192.168.1.42', rssi: -jit(58, 9), tsync: true, clients: 1 },
      anima: { online_en: true, online_only: false, online_avail: true, teacher: true,
               provider: 'anthropic', model: 'claude-opus', l1_mode: 0, l1_serving: true, l1_heap: jit(31000, 1500),
               q: a.q, none: a.none, cmd: a.cmd, fact: a.fact, stitch: a.stitch, remote: a.remote,
               last_conf: a.last_conf, last: a.last },
      arb: { busy: false, job: '', grants: jit(40, 8), denials: simState.oom ? 6 : 1, yields: 2, hfmin: simState.minFree },
      cpu: { cores: 2, freq: 240, tasks: jit(28, 3), load: [jit(14, 9), jit(11, 7)] },
      oom: { count: simState.oom, last_size: simState.oom ? 32768 : 0, last_caps: simState.oom ? 4 : 0 },
    });
  }
  if (path === '/api/wifi/scan') {                  // on-demand AP scan (deterministic; small delay to show the spinner)
    await new Promise((r) => setTimeout(r, 450));
    return sendJSON(res, { networks: [
      { ssid: 'home-wifi', rssi: -42, channel: 6, auth: 'WPA2' },
      { ssid: 'FRITZ!Box 7530', rssi: -67, channel: 11, auth: 'WPA2/WPA3' },
      { ssid: 'CoffeeShop_Free', rssi: -78, channel: 1, auth: 'Open' },
      { ssid: 'Vodafone-2261', rssi: -83, channel: 3, auth: 'WPA2' } ] });
  }
  // Known-networks store (mirror nucleo_setup multi-network API: /api/wifi/{known,join,forget}).
  // In-memory here; the firmware persists to /cfg/config/networks.json. Auth-gated on device.
  {
    const w = (simState.wifi ||= { known: [{ ssid: 'home-wifi', priority: 1 }], current: 'home-wifi' });
    if (path === '/api/wifi/known') {
      return sendJSON(res, { mode: 'sta', ssid: w.current,
        networks: w.known.map((n) => ({ ssid: n.ssid, priority: n.priority | 0, current: n.ssid === w.current })) });
    }
    if (path === '/api/wifi/join' && req.method === 'POST') {
      let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
      if (!b.ssid) return send(res, 400, 'application/json', '{"ok":false}');
      await new Promise((r) => setTimeout(r, 600));     // mimic the blocking join
      if (!w.known.some((n) => n.ssid === b.ssid)) w.known.push({ ssid: b.ssid, priority: 0 });
      w.current = b.ssid;
      simLog(`I (${Date.now() % 100000}) setup: joined '${b.ssid}'`);
      return sendJSON(res, { ok: true, ip: '192.168.1.42' });
    }
    if (path === '/api/wifi/forget' && req.method === 'POST') {
      let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
      if (b.all) { w.known = []; w.current = ''; }
      else { w.known = w.known.filter((n) => n.ssid !== b.ssid); if (w.current === b.ssid) w.current = ''; }
      return sendJSON(res, { ok: true });
    }
  }
  // ── IR transmit (mirror nucleo_ir: /api/ir/{db,send,tvbgone}). Public in the sim so preview needs
  // no pairing; the firmware gates send/tvbgone with NUCLEO_AUTH_GUARD. The sweep drives fake progress. ──
  if (path === '/api/ir/db') {
    return sendJSON(res, { ready: true, gpio: 44,
      protocols: ['nec', 'necext', 'samsung', 'sony12', 'sony15', 'sony20', 'rc5', 'jvc'],
      tvbgone: { count: 16, regions: ['all', 'us', 'eu', 'asia'] } });
  }
  if (path === '/api/ir/send' && req.method === 'POST') {
    let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
    const label = b.raw ? `raw[${b.raw.length}]@${b.carrier || 38000}` : `${b.protocol} a=${b.address || 0} c=${b.command}`;
    simLog(`I (${Date.now() % 100000}) ir: send ${label}`);
    publish('ir.sent', { protocol: b.protocol || 'raw', address: b.address | 0, command: b.command | 0 });
    return sendJSON(res, { ok: true });
  }
  if (path === '/api/ir/tvbgone') {
    const ir = (simState.ir ||= { running: false, sent: 0, total: 0, timer: null });
    if (req.method === 'POST') {
      let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
      if (String(b.action || 'start') === 'stop') { clearInterval(ir.timer); ir.running = false; return sendJSON(res, { ok: true, running: false }); }
      const counts = { all: 16, us: 5, eu: 4, asia: 7 };
      ir.total = counts[String(b.region || 'all').toLowerCase()] ?? 16;
      ir.sent = 0; ir.running = true; clearInterval(ir.timer);
      ir.timer = setInterval(() => { ir.sent++; publish('ir.sweep', { sent: ir.sent, total: ir.total });
        if (ir.sent >= ir.total) { clearInterval(ir.timer); ir.running = false; } }, 200);
      return sendJSON(res, { ok: true, running: true });
    }
    return sendJSON(res, { running: ir.running, sent: ir.sent, total: ir.total });
  }
  if (path === '/api/ir/jammer' && req.method === 'POST') {
    let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
    const on = String(b.action || 'start') !== 'stop';
    simLog(`I (${Date.now() % 100000}) ir: jammer ${on ? 'start ' + (b.mode || 'sweep') : 'stop'}`);
    return sendJSON(res, { ok: true, running: on, mode: b.mode || 'sweep' });
  }
  // ── Vicino device-to-device transfer (mirror /api/link/* over nucleo_link_espnow). The sim fakes the
  // ESP-NOW engine so the web app can be exercised headless: discovery populates peers, a send animates
  // progress (state 2=RUN→4=DONE, like nlink_state), status/offer/cmd are polled. ──
  if (path.startsWith('/api/link/')) {
    const L = (simState.link ||= { peers: [], st: { active: false, sending: false, proto: 'nucleo', state: 0, reason: 0, done: 0, total: 0, rate: 0, name: '', peer: '' }, offer: null, cmd: null, timer: null });
    const sub = path.slice('/api/link/'.length);
    const body = async () => { try { return JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch { return {}; } };
    if (sub === 'peers' && req.method === 'GET')
      return sendJSON(res, { name: 'nucleo-sim', channel: 1, inbox: '/data/Vicino', peers: L.peers.map((p, i) => ({ i, ...p })) });
    if (sub === 'status') return sendJSON(res, L.st);
    if (sub === 'offer' && req.method === 'GET')
      return sendJSON(res, L.offer ? { pending: true, ...L.offer } : { pending: false, name: '', from: '', size: 0 });
    if (sub === 'cmd' && req.method === 'GET')
      return sendJSON(res, L.cmd ? { pending: true, cmd: L.cmd.cmd, from: L.cmd.from } : { pending: false, cmd: '', from: '' });
    if (sub === 'discover' && req.method === 'POST') {
      await body();
      L.peers = [{ name: 'Nucleo-Bruno', mac: 'AA:BB:CC:00:11:22', proto: 'nucleo' }, { name: 'Bruce-Box', mac: 'DE:AD:BE:EF:00:01', proto: 'bruce' }];
      publish('link.peers', { count: L.peers.length });
      return sendJSON(res, { ok: true });
    }
    if (sub === 'listen' && req.method === 'POST') { await body(); return sendJSON(res, { ok: true }); }
    if (sub === 'send' && req.method === 'POST') {
      const b = await body();
      let total = 20000; try { total = (await stat(sdPath(b.path))).size || total; } catch {}
      const peer = L.peers[b.peer | 0]?.name || '?';
      clearInterval(L.timer);
      L.st = { active: true, sending: true, proto: b.proto || 'nucleo', state: 2, reason: 0, done: 0, total, rate: 0, name: String(b.path || '').split('/').pop(), peer };
      L.timer = setInterval(() => {
        L.st.done = Math.min(L.st.total, L.st.done + Math.max(2000, Math.floor(total / 12)));
        L.st.rate = 30000 + (Math.floor(total / 1000) % 5000);
        publish('link.progress', { done: L.st.done, total: L.st.total });
        if (L.st.done >= L.st.total) { clearInterval(L.timer); L.st.active = false; L.st.state = 4; publish('link.done', { name: L.st.name }); }
      }, 200);
      simLog(`I (${Date.now() % 100000}) link: send ${L.st.name} -> ${peer} (${L.st.proto})`);
      return sendJSON(res, { ok: true });
    }
    if (sub === 'cmd' && req.method === 'POST') { const b = await body(); simLog(`I (${Date.now() % 100000}) link: cmd "${b.command}" -> peer ${b.peer}`); return sendJSON(res, { ok: true }); }
    if (sub === 'cmd/confirm' && req.method === 'POST') { await body(); L.cmd = null; return sendJSON(res, { ok: true }); }
    if (sub === 'offer' && req.method === 'POST') { await body(); L.offer = null; return sendJSON(res, { ok: true }); }
    if (sub === 'cancel' && req.method === 'POST') { clearInterval(L.timer); L.st.active = false; L.st.state = 5; return sendJSON(res, { ok: true }); }
    return sendJSON(res, { ok: false });
  }
  if (path === '/api/gpio') {                         // mirror nucleo_gpio: raw pin read/write (safe allowlist)
    const ALLOW_W = [1, 2], ALLOW_R = [0, 1, 2];
    simState.gpio ||= {};
    if (req.method === 'POST') {
      let b = {}; try { b = JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch {}
      const pin = b.pin | 0, val = b.value ? 1 : 0;
      if (!ALLOW_W.includes(pin)) return send(res, 403, 'application/json', JSON.stringify({ ok: false, err: 'pin not writable' }));
      simState.gpio[pin] = val; simLog(`I (${Date.now() % 100000}) gpio: pin ${pin} = ${val}`);
      return sendJSON(res, { ok: true, pin, value: val });
    }
    const pin = parseInt(url.searchParams.get('pin') || '-1', 10);
    if (!ALLOW_R.includes(pin)) return send(res, 403, 'application/json', JSON.stringify({ ok: false, err: 'pin not readable' }));
    return sendJSON(res, { ok: true, pin, value: simState.gpio[pin] ?? 0 });
  }
  if (path === '/api/logs') {                        // RAM ring log — TEXT, not JSON
    res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8', 'access-control-allow-origin': '*', 'cache-control': 'no-store' });
    return res.end(simState.logs.join('\n') + '\n');
  }
  if (path === '/api/anima/caps') {                  // teacher provider/model (no key) + offline-brain policy
    let hasKey = false, provider, modelName;
    try { const j = JSON.parse(readFileSync(join(SD, 'data', 'anima', 'teacher.json'), 'utf8')); hasKey = !!j.key; provider = j.provider; modelName = j.model; } catch {}
    const serving = simState.l1Mode === 'on' || (simState.l1Mode === 'auto' && !simState.externalBrain && !hasKey);
    const out = { hasKey, online: hasKey, enabled: true, l1Mode: simState.l1Mode, l1Serving: serving };
    if (hasKey) { out.provider = provider; out.model = modelName; }
    res.writeHead(200, { 'content-type': 'application/json', 'access-control-allow-origin': '*', 'cache-control': 'no-store' });
    return res.end(JSON.stringify(out));
  }
  if (path === '/api/anima/l1' && req.method === 'POST') {   // offline L1 brain policy (stateful)
    try { const b = JSON.parse((await readBody(req)).toString('utf8') || '{}');
      if (b.mode === 'on' || b.mode === 'off' || b.mode === 'auto') simState.l1Mode = b.mode;
      if (typeof b.browserLLM === 'boolean') simState.externalBrain = b.browserLLM;
    } catch {}
    let hasKey = false; try { hasKey = !!JSON.parse(readFileSync(join(SD, 'data', 'anima', 'teacher.json'), 'utf8')).key; } catch {}
    const serving = simState.l1Mode === 'on' || (simState.l1Mode === 'auto' && !simState.externalBrain && !hasKey);
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    return res.end(JSON.stringify({ l1Mode: simState.l1Mode, l1Serving: serving }));
  }
  if (path === '/api/tts') {                          // on-device voice switch (GET state / POST set+say)
    if (req.method === 'POST') { try { const b = JSON.parse((await readBody(req)).toString('utf8') || '{}');
      if (typeof b.enabled === 'boolean') simState.ttsEnabled = b.enabled;
      if (b.say) console.log(`[tts] say(${b.lang || 'it'}): ${b.say}`); } catch {} }
    const available = existsSync(join(SD, 'data', 'tts'));
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    return res.end(JSON.stringify({ enabled: simState.ttsEnabled, available }));
  }
  if (path === '/api/voice/always' && req.method === 'POST') {   // PIN-gated (guarded above): always-on PTT
    try { simState.voiceAlwaysOn = !!JSON.parse((await readBody(req)).toString('utf8') || '{}').on; } catch {}
    return sendJSON(res, { ok: true, on: simState.voiceAlwaysOn });
  }
  if (path === '/api/anima') {
    if (url.searchParams.get('reset') === '1') resetAnimaSession();   // "pulisci conversazione"
    let q = url.searchParams.get('q') || '';
    let replayed = false;
    if (isRepeat(q) && animaMem.lastActionInput) { q = animaMem.lastActionInput; replayed = true; }   // action memory
    const lang0 = url.searchParams.get('lang') || 'it';
    // Online mode (mirrors native OM_OFF/OM_ON/OM_ONLY): off=offline-only (no network tier runs),
    // on=hybrid offline-first then online (default), only=online-first (skip the offline bio recall).
    const mode = (url.searchParams.get('mode') || 'on').toLowerCase();
    const allowOnline = mode !== 'off';
    let r = animaQuery(q, lang0, animaMem);
    // TRANSLATION precedence (mirrors firmware tool_translate): ONLINE-FIRST. In hybrid/online mode the
    // teacher LLM (Grok) translates — full sentences, real quality. The offline dictionary is the FLOOR:
    // used only when offline (mode=off / no key) or if the online call fails. Base translator = offline only.
    if (isTranslateRequest(q)) {
      let tr = allowOnline ? await translateOnline(q, lang0) : null;
      if (!tr) tr = translateAnswer(q, lang0 === 'en', join(SD, 'data', 'anima'));
      if (tr) r = tr;
    }
    // Live WEATHER tier — runs FIRST and OVERRIDES a stray L0 match: a weather question routinely
    // carries "oggi"/"domani", which the date/clock intent would otherwise grab ("temperatura a
    // napoli oggi" must be weather, not the calendar). Also pre-empts any Wikipedia/entity tier that
    // would hijack "meteo brescia" as a bare-noun lookup. Ephemeral: answered fresh, NEVER learned.
    // BUT never override a deterministic math/tool answer (the firmware runs the L0 solver before
    // any online tier, so "seno di 30 gradi" is sin(30), not weather for the city "Seno").
    const mathHeld = r.tier !== 'none' && (r.state === 'tool' ||
      ['calc', 'percent', 'convert', 'ohm', 'base', 'prime', 'roman', 'geo', 'phys'].includes(r.intent));
    if (!mathHeld) { const w = await weatherAnswer(q, lang0, allowOnline); if (w) r = w; }
    // Neuro-symbolic COMBINATORS first (specific composition questions): "chi è nato prima A o B",
    // "quanti anni tra la nascita di A e B", "A era europeo", "A e B erano connazionali" — answers
    // COMPUTED from ≥2 facts (nowhere stored). Runs before the forward fact detector can mis-parse them.
    const compositional = !!combinatorParse(q);   // a composition question: ONLY a computed answer is valid
    // SAFETY GATE: only a genuine question escalates to the knowledge/entity/teacher tiers. A bare
    // date / number / cell-ref / code / wordless string yields an honest miss — never a fabricated fact,
    // never learned. (The deterministic L0/math/command answer from animaQuery above still stands.)
    const askable = isAskable(q);
    if (askable) {
    if (r.tier === 'none' && allowOnline) { const c = await combinatorAnswer(q, lang0); if (c) r = c; }
    // Deductive tier FIRST (offline, specific): INVERSE-relation questions ("cosa ha scritto Dante" ⇐
    // "chi ha scritto la Divina Commedia", "di quale paese è capitale Tokyo") — answered by inverting the
    // relation's rotation in the permutation-KGE, before the forward fact detector can mis-parse them.
    if (r.tier === 'none' && allowOnline) { const k = await kgInfer(q, lang0); if (k) r = k; }
    // Precise-fact questions ("quando è nato X", "capitale di X", "chi ha scritto X") are unambiguous;
    // a broad L0 intent (whoami via "chi", date via "data") must not pre-empt them. Recall offline,
    // else verify on Wikidata. (Offline + not learned -> null -> the L0 result / honest miss stands.)
    const factWanted = factDetect(q, lang0 === 'en');
    if (factWanted && r.tier === 'none') {                    // (skip if the deductive tier already answered)
      // Offline-first: exact recall → HDC relational REASONING (unbinding, phrasing-robust, no network)
      // → online Wikidata. So a fact once learned is answered offline even in phrasings never stored.
      const f = (await learnedLookup(lang0, q, { factOnly: true })) || (await hdcReason(q, lang0)) || (allowOnline ? await factAnswer(q, lang0) : null);
      if (f) r = f;
    }
    if (r.tier === 'none') {                                  // typo rescue: correct-on-miss, retry once
      const fixed = spellfix(q);
      if (fixed !== q) { const r2 = animaQuery(fixed, lang0, animaMem); if (r2.tier !== 'none') { r = r2; r.corrected = fixed; } }
    }
    // Still a miss -> recall what we learned before (OFFLINE, no network) FIRST: a known entity/fact
    // answers with zero network. Then escalate through FREE structured sources, the teacher last.
    // (skip the generic bio recall for a precise-fact question: "quando è nato X" must answer the
    // DATE or honestly miss — never the person's bio, which is the wrong shape for the question.)
    // (a COMPOSITION question whose facts we couldn't fetch must NOT be answered by a stray entity bio —
    // "Dante e Einstein connazionali?" must refuse, never return an Einstein summary. So the generic
    // recall/entity/bare tiers are skipped when the query was a composition question — only a COMPUTED
    // answer or an honest miss is acceptable. This was a real pipeline hazard, now fenced off.)
    if (r.tier === 'none' && !factWanted && !compositional && mode !== 'only') { const rec = await learnedLookup(lang0, q); if (rec) r = rec; }
    // Transitive geo deduction ("dove si trova X", "in che continente è X"): deduce the containment chain
    // offline from the learned located_in graph; if unknown, learn the chain (Wikidata) then deduce. A
    // structured source — runs before the Wikipedia bio so a place question gets its chain, not a summary.
    if (r.tier === 'none' && allowOnline) { const g = await locAnswer(q, lang0); if (g) r = g; }
    // Encyclopedic entity ("chi è / cos'è X" -> Wikipedia summary) — FREE, no key. The headline path
    // for "chi è Einstein/Trump", "cos'è la fotosintesi". Learns the answer -> recalled offline forever.
    if (r.tier === 'none' && !compositional && allowOnline) { const e = await entityAnswer(q, lang0); if (e) r = e; }
    // Deterministic dictionary (Wiktionary, FREE/exact-match): only on a miss, so curated tech-concept
    // cards keep priority over a generic gloss. Learned -> next time offline.
    if (r.tier === 'none' && defDetect(q, lang0 === 'en') && allowOnline) { const d = await defAnswer(q, lang0); if (d) r = d; }
    // Bare noun phrase ("batman?", "einstein") -> Wikipedia (Wikipedia itself rejects non-entities).
    if (r.tier === 'none' && !compositional && allowOnline) { const b = await bareEntity(q, lang0); if (b) r = b; }
    // Last resort: the online LLM teacher (Grok/Groq), ONLY if a key is configured (selective memory:
    // general+verifiable -> learned, else ephemeral). No key / offline -> a no-op -> honest miss, never
    // a hallucination. This is the "via di fuga in extremis" — it runs only after every free source missed.
    if (r.tier === 'none' && allowOnline) { const t = await teacherAnswer(q, lang0); if (t) r = t; }
    }   // end askable gate — data / non-questions never reach the knowledge tiers
    if (r.action === 'tool' && r.tool === 'create_file' && r.arg) {       // execute the tool (mirrors firmware)
      const en = (url.searchParams.get('lang') || '')[0] === 'e';
      const bn = r.arg.split('/').pop();
      const fp = join(SD, r.arg.replace(/^\//, ''));
      try {
        await mkdir(dirname(fp), { recursive: true });        // ensure the routed folder exists
        let exists = true; try { await stat(fp); } catch { exists = false; }
        if (exists) r.reply = en ? `${bn} already exists — not overwritten.` : `${bn} esiste gia: non lo sovrascrivo.`;
        else { await writeFile(fp, r.content || ''); r.path = r.arg; }   // compose-then-act payload ("" -> empty)
        animaMem.last_file = r.arg; animaMem.last_kind = 'f';   // real file (created or existing)
      } catch { r.reply = en ? `Can't create ${bn}.` : `Non riesco a creare ${bn}.`; }
    }
    // add_event -> append the reminder to the OS calendar (mirrors anima_apply_event in nucleo_httpd.c)
    if (r.action === 'tool' && r.tool === 'add_event' && r.content) {
      const en = (url.searchParams.get('lang') || '')[0] === 'e';
      const m = { off: 0, time: '', text: '' };
      const mo = r.content.match(/off=(-?\d+)/); if (mo) m.off = parseInt(mo[1], 10);
      const mt = r.content.match(/;time=([^;]*)/); if (mt) m.time = mt[1];
      const mx = r.content.match(/;text=([\s\S]*)$/); if (mx) m.text = mx[1];
      try {
        const d = new Date(); d.setDate(d.getDate() + m.off);
        const date = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
        const fp = join(SD, 'system', 'config', 'calendar.json');
        await mkdir(dirname(fp), { recursive: true });
        let cal = { events: {} }; try { cal = JSON.parse(readFileSync(fp, 'utf8')); if (!cal.events) cal.events = {}; } catch {}
        (cal.events[date] = cal.events[date] || []).push({ time: m.time, text: m.text });
        await writeFile(fp, JSON.stringify(cal, null, 2));
        r.reply = m.time ? (en ? `Added "${m.text}" on ${date} at ${m.time}.` : `Aggiunto "${m.text}" il ${date} alle ${m.time}.`)
                         : (en ? `Added "${m.text}" on ${date}.` : `Aggiunto "${m.text}" il ${date}.`);
      } catch { r.reply = en ? `I couldn't add the event.` : `Non sono riuscito ad aggiungere l'evento.`; }
    }
    // utility memory: remember launches here; create_file is remembered above only when real
    if (r.action === 'launch' && r.arg) { animaMem.last_app = r.arg; animaMem.last_kind = 'a'; }
    if (r.tier === 'fact') animaMem.last_topic = q;
    if (replayed && r.action !== 'none') { r.memory = true; r.state = 'followup'; }     // action memory hit
    if (r.action !== 'none' && !r.awaiting && r.intent !== 'clarify') animaMem.lastActionInput = q;  // replay target
    // micro-thought: the structured decision behind the reply (observable)
    r.domain = r.intent === 'weather' ? 'meteo'
             : r.tier === 'remote' ? (r.learned ? 'teacher·learned' : r.verified ? 'teacher·wiki' : 'teacher')
             : r.tier === 'fact' ? 'knowledge' : ['calc','percent','convert','ohm','base','prime','roman','geo','phys'].includes(r.intent) ? 'calc' : r.intent === 'clarify' ? 'clarify'
             : r.intent === 'capabilities' ? 'faq' : r.intent === 'agenda' ? 'agenda'   // mirror nucleo_httpd.c domain ladder
             : r.action === 'tool' ? 'tool' : r.action === 'system' ? 'system' : r.action === 'launch' ? 'app'
             : r.action === 'answer' ? 'faq' : 'none';
    r.budget = r.budget || 0; r.memory = r.memory || false;
    r.intent = r.intent || r.tool || '';
    r.state = r.state || 'idle'; r.awaiting = r.awaiting || false; r.corrected = r.corrected || '';
    // reasoning trace (mirrors nucleo_anima.c done block): a multi-step plan sets it (" > " joined);
    // otherwise synthesize a one-line summary (" | " joined, so the UI won't render it as a ⎿ plan).
    if (!r.trace) {
      const tn = r.tier === 'command' ? 'L0' : r.tier === 'fact' ? 'L1' : r.tier === 'remote' ? 'web' : r.tier === 'none' ? '-' : 'L2';
      r.trace = r.budget > 0 ? `${tn} ${r.domain} | ${r.budget}cl | ${r.confidence || 0}%` : `${tn} ${r.domain} | ${r.confidence || 0}%`;
    }
    r.lang = (url.searchParams.get('lang') || '')[0] === 'e' ? 'en' : 'it';
    return sendJSON(res, r);
  }
  if (path === '/api/ota' && req.method === 'POST') return otaApi(req, res);
  if (path === '/api/reboot' && req.method === 'POST') { if (!isAuthed(req)) return reject401(res); return sendJSON(res, { ok: true, reboot: true }); }
  if (path === '/api/llm') return llmApi(req, res, url);   // same-origin LLM proxy (mirrors firmware /api/llm)
  if (path === '/api/proxy') return proxyApi(req, res, url);   // same-origin page/file proxy (mirrors firmware /api/proxy)
  if (path.startsWith('/api/rec/')) return recApi(req, res, url);
  if (path.startsWith('/api/voice/')) return voiceApi(req, res, url);   // learn / match (always handled inline above)
  if (path === '/api/transcribe') return transcribeApi(req, res, url);
  if (path.startsWith('/api/fs/')) return fsApi(req, res, url);

  // On-device screen simulator (Wear OS-style launcher) under /device/
  if (path === '/device' || path === '/device/') return sendFile(res, join(REPO, 'web', 'device', 'index.html'));
  const dm = path.match(/^\/device\/(.+)$/);
  if (dm) { const abs = normalize(join(REPO, 'web', 'device', dm[1])); if (!abs.startsWith(join(REPO, 'web', 'device'))) return send(res, 403, 'text/plain', '403'); return sendFile(res, abs); }

  const m = path.match(/^\/apps\/([a-z0-9-]+)\/(.*)$/);
  if (m) { const abs = normalize(join(REPO, 'apps', m[1], 'www', m[2] || 'index.html')); if (!abs.startsWith(join(REPO, 'apps'))) return send(res, 403, 'text/plain', '403'); return sendFile(res, abs); }

  const rel = path === '/' ? 'index.html' : path.replace(/^\//, '');
  const abs = normalize(join(SHELL, rel));
  if (!abs.startsWith(SHELL)) return send(res, 403, 'text/plain', '403');
  return sendFile(res, abs);
});

// minimal WebSocket upgrade at /ws
server.on('upgrade', (req, socket) => {
  if (new URL(req.url, 'http://x').pathname !== '/ws') { socket.destroy(); return; }
  // The browser sends the session cookie on the WS handshake automatically — gate it too.
  if (!isAuthed(req)) { socket.write('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n'); socket.destroy(); return; }
  const key = req.headers['sec-websocket-key'];
  const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  sockets.add(socket);
  socket.write(wsEncode(JSON.stringify({ op: 'sync', events: [] })));
  socket.on('data', () => {});            // ignore client frames (subscribe)
  socket.on('close', () => sockets.delete(socket));
  socket.on('error', () => sockets.delete(socket));
});

// Pull a filename from raw input (mirrors a_extract_filename in nucleo_anima.c).
function extractFilename(raw) {
  const words = String(raw).split(/\s+/).filter(Boolean);
  const trig = ['chiamato', 'chiamata', 'nome', 'named', 'called', 'titolo', 'title'];
  let cand = null;
  for (const w of words) if (w.includes('.') && /[a-z]/i.test(w)) { cand = w; break; }
  if (!cand) for (let i = 0; i < words.length - 1; i++) if (trig.includes(words[i].toLowerCase())) { cand = words[i + 1]; break; }
  if (!cand) return null;
  let s = cand.replace(/[^A-Za-z0-9._-]/g, '');
  if (!s || s[0] === '.') return null;
  if (!s.includes('.')) s += '.txt';
  return s;
}

// Tool: calc — pure arithmetic, mirrors a_try_calc in nucleo_anima.c. Returns
// {kind:'ok',value} | {kind:'divzero'} | null (not a calc).
function tryCalc(raw) {
  // a 3-part date with a 4-digit year is NOT arithmetic (24/04/2027, 2027-04-24) — don't let / - . make it math
  if (/^\s*(\d{1,2}[/.\-]\d{1,2}[/.\-]\d{4}|\d{4}[/.\-]\d{1,2}[/.\-]\d{1,2})\s*$/.test(String(raw))) return null;
  // fold: lowercase/de-accent, × ÷ -> * /, decimal comma -> dot, keep math chars only
  let f = String(raw).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/×/g, '*').replace(/÷/g, '/').replace(/,/g, '.')
    .replace(/(\d)x(?=\d)/g, '$1*')                        // glued multiply: "4x4" -> "4*4"
    .replace(/[^0-9a-z+\-*/^().\s]/g, ' ');                // mirror a_fold_calc: stray chars -> space ("3?" -> "3 ")
  const MUL = ['per', 'times', 'x', 'moltiplicato'], DIV = ['diviso', 'fratto', 'divided'],
        ADD = ['piu', 'plus'], SUB = ['meno', 'minus'];
  let ex = '', ndig = 0, nop = 0;
  for (const tk of f.split(/([+\-*/^()])/)) {           // split keeping symbol delimiters
    const t = tk.trim();
    if (!t) continue;
    if ('+-*/^()'.includes(t)) { ex += ' ' + t; if ('+-*/^'.includes(t)) nop++; continue; }
    for (const w of t.split(/\s+/)) {
      if (!w) continue;
      const hasAlpha = /[a-z]/.test(w), hasDigit = /[0-9]/.test(w);
      if (hasAlpha && !hasDigit) {                      // word -> operator or drop
        const op = MUL.includes(w) ? '*' : DIV.includes(w) ? '/' : ADD.includes(w) ? '+' : SUB.includes(w) ? '-' : '';
        if (op) { ex += ' ' + op; nop++; }
      } else { ex += w; ndig += (w.match(/[0-9]/g) || []).length; }
    }
  }
  if (ndig < 1 || nop < 1) return null;
  // recursive-descent over the cleaned expression (+ - * / and parens)
  let i = 0; const s = ex; let err = false, divzero = false;
  const skip = () => { while (s[i] === ' ') i++; };
  function prim() {
    skip();
    if (s[i] === '(') { i++; const v = expr(); skip(); if (s[i] === ')') i++; else err = true; return v; }
    if (s[i] === '-') { i++; return -prim(); }
    if (s[i] === '+') { i++; return prim(); }
    const m = /^[0-9]*\.?[0-9]+(?:e[+-]?[0-9]+)?/i.exec(s.slice(i));
    if (!m) { err = true; return 0; }
    i += m[0].length; return parseFloat(m[0]);
  }
  function powf() { let b = prim(); skip(); if (s[i] === '^') { i++; return Math.pow(b, powf()); } return b; }   // right-assoc
  function term() { let v = powf(); for (;;) { skip(); const o = s[i];
    if (o === '*') { i++; v *= powf(); } else if (o === '/') { i++; const d = powf(); if (d === 0) { divzero = err = true; return 0; } v /= d; } else break; } return v; }
  function expr() { let v = term(); for (;;) { skip(); const o = s[i];
    if (o === '+') { i++; v += term(); } else if (o === '-') { i++; v -= term(); } else break; } return v; }
  const v = expr(); skip();
  if (divzero) return { kind: 'divzero' };
  if (err || i < s.length) return null;
  return { kind: 'ok', value: v };
}
function fmtNum(v) {   // precisione (mirror a_fmt_num): %.6g
  return (isFinite(v) && Math.abs(v) < 1e15 && Math.abs(v - Math.round(v)) < 1e-9)
    ? String(Math.round(v)) : String(parseFloat(v.toPrecision(6)));
}
function fmtRound(v) {  // max 4 decimali (mirror a_fmt_round): calcolo "normale"
  if (isFinite(v) && Math.abs(v) < 1e15 && Math.abs(v - Math.round(v)) < 1e-9) return String(Math.round(v));
  if (!isFinite(v)) return String(v);
  return String(parseFloat(v.toFixed(4)));
}

// ---- MATH ENGINE v2 frames — exact mirrors of nucleo_anima.c (a_solve_base / _roman /
// _scale / _binop / _funcs / _numprop). Keep IDENTICAL to the firmware so the device and the
// web preview never drift. Each returns {intent, reply, conf} or null. ----
function jToBase(v, base) {
  if (v === 0) return '0';
  const D = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'; let s = '', n = v;
  while (n > 0) { s = D[n % base] + s; n = Math.floor(n / base); }
  return s;
}
function jFromBase(str, base) {
  let v = 0;
  for (const ch of str) {
    let d;
    if (ch >= '0' && ch <= '9') d = ch.charCodeAt(0) - 48;
    else if (ch >= 'a' && ch <= 'z') d = ch.charCodeAt(0) - 87;
    else if (ch >= 'A' && ch <= 'Z') d = ch.charCodeAt(0) - 55;
    else return null;
    if (d >= base) return null;
    v = v * base + d;
  }
  return str.length ? v : null;
}
function jBaseName(w) {
  if (['binario','binaria','binary','bin'].includes(w)) return 2;
  if (['ottale','octal','oct'].includes(w)) return 8;
  if (['decimale','decimal','denario','dec'].includes(w)) return 10;
  if (['esadecimale','hex','hexadecimal','esa'].includes(w)) return 16;
  return 0;
}
function solveBase(norm) {
  const wd = norm.split(/[^a-z0-9]+/).filter(Boolean);
  for (const w of wd) if (['log','logaritmo','logarithm','ln','log2'].includes(w)) return null;
  const used = new Array(wd.length).fill(false);
  let srcBase = 0, dstBase = 0, sawKw = false, convVerb = false, hexWord = false;
  for (let i = 0; i < wd.length; i++) {
    const prev = i > 0 ? wd[i - 1] : '', fromSrc = prev === 'da' || prev === 'from';
    if (['converti','convert','convertire','converte'].includes(wd[i])) convVerb = true;
    if (['esadecimale','hex','hexadecimal','esa'].includes(wd[i])) hexWord = true;
    const nb = jBaseName(wd[i]);
    if (nb) { sawKw = true; used[i] = true; if (fromSrc) { if (!srcBase) srcBase = nb; } else if (!dstBase) dstBase = nb; continue; }
    if (wd[i] === 'base') { sawKw = true; used[i] = true;
      if (i + 1 < wd.length) { const b = parseInt(wd[i + 1], 10);
        if (b >= 2 && b <= 36) { used[i + 1] = true; if (fromSrc) { if (!srcBase) srcBase = b; } else if (!dstBase) dstBase = b; } } }
  }
  if (!sawKw) return null;
  let val = null;
  for (let i = 0; i < wd.length && val === null; i++) if (!used[i] && /[0-9]/.test(wd[i])) val = wd[i];
  if (val === null && (convVerb || hexWord || srcBase === 16))
    for (let i = 0; i < wd.length && val === null; i++) if (!used[i] && wd[i].length >= 2 && /^[a-f]+$/.test(wd[i])) { val = wd[i]; if (!srcBase) srcBase = 16; }
  if (val === null) return null;
  let vbuf = val;
  if (vbuf[0] === '0' && 'xbo'.includes(vbuf[1]) && vbuf.length > 2) {
    const pb = vbuf[1] === 'x' ? 16 : vbuf[1] === 'b' ? 2 : 8;
    if (!srcBase || srcBase === 10) srcBase = pb;
    vbuf = vbuf.slice(2);
  }
  if (dstBase === 0 && srcBase === 0) return null;
  if (!srcBase) srcBase = 10;
  if (!dstBase) dstBase = 10;
  const v = jFromBase(vbuf, srcBase);
  if (v === null) return null;
  const dst = jToBase(v, dstBase), up = vbuf.toUpperCase();
  return { intent: 'base', conf: 96,
    reply: srcBase === 10 ? `${up} in base ${dstBase} = ${dst}.` : `${up} (base ${srcBase}) = ${dst} (base ${dstBase}).` };
}
function jIntToRoman(v) {
  const R = [[1000,'M'],[900,'CM'],[500,'D'],[400,'CD'],[100,'C'],[90,'XC'],[50,'L'],[40,'XL'],[10,'X'],[9,'IX'],[5,'V'],[4,'IV'],[1,'I']];
  let s = ''; for (const [n, sym] of R) while (v >= n) { s += sym; v -= n; } return s;
}
function jRomanToInt(s) {
  const M = { i:1, v:5, x:10, l:50, c:100, d:500, m:1000 };
  let prev = 0, total = 0;
  for (let i = s.length - 1; i >= 0; i--) { const d = M[s[i]]; if (d === undefined) return -1; if (d < prev) total -= d; else { total += d; prev = d; } }
  return s.length ? total : -1;
}
function solveRoman(norm, en) {
  const wd = norm.split(/[^a-z0-9]+/).filter(Boolean);
  let romanKw = false, decKw = false;
  for (const w of wd) { if (['romano','romani','roman','romana'].includes(w)) romanKw = true; if (['decimale','arabo','arabi','vale'].includes(w)) decKw = true; }
  if (!romanKw && !decKw) return null;
  if (romanKw) for (const w of wd) if (/^[0-9]+$/.test(w)) { const v = parseInt(w, 10);
    if (v >= 1 && v <= 3999) return { intent: 'roman', conf: 95, reply: en ? `${v} in roman numerals = ${jIntToRoman(v)}.` : `${v} in numeri romani = ${jIntToRoman(v)}.` }; }
  const STOP = ['di', 'mi', 'ci', 'vi', 'li'];               // common words that are valid roman letters
  for (const w of wd) {                                      // reverse: only a CANONICAL numeral, length >= 2
    if (w.length < 2 || STOP.includes(w) || !/^[ivxlcdm]+$/.test(w)) continue;
    const v = jRomanToInt(w);
    if (v > 0 && jIntToRoman(v) === w.toUpperCase()) return { intent: 'roman', conf: 95, reply: `${w.toUpperCase()} = ${v}.` };
  }
  return null;
}
function solveScale(items, en) {
  let op = 0, x = 0, nn = 0;
  const map = { doppio:1, double:1, triplo:2, triple:2, triplica:2, quadruplo:3, quadruple:3, meta:4, half:4,
    terzo:5, third:5, quarto:6, quarter:6, quadrato:7, square:7, squared:7, cubo:8, cube:8, cubed:8,
    reciproco:9, reciprocal:9, inverso:9, opposto:10, opposite:10, negato:10 };
  for (const it of items) { if (it.num) { x = it.val; nn++; } else if (!op && map[it.w]) op = map[it.w]; }
  if (!op || nn < 1) return null;
  let res;
  switch (op) { case 1: res = x*2; break; case 2: res = x*3; break; case 3: res = x*4; break; case 4: res = x/2; break;
    case 5: res = x/3; break; case 6: res = x/4; break; case 7: res = x*x; break; case 8: res = x*x*x; break;
    case 9: if (x === 0) return null; res = 1/x; break; case 10: res = -x; break; default: return null; }
  const LIT = ['','doppio','triplo','quadruplo','meta','un terzo','un quarto','quadrato','cubo','reciproco','opposto'];
  const LEN = ['','double','triple','quadruple','half','a third','a quarter','square','cube','reciprocal','opposite'];
  return { intent: 'calc', conf: 95, reply: en ? `${LEN[op]} of ${fmtNum(x)} = ${fmtNum(res)}.` : `${LIT[op]} di ${fmtNum(x)} = ${fmtNum(res)}.` };
}
function solveBinop(items, en) {
  const num = []; let sum=false, diff=false, prod=false, quot=false, take=false, addto=false, manca=false, hasDa=false, hasA=false;
  for (const it of items) { if (it.num) { num.push(it.val); continue; } const w = it.w;
    if (['somma','sum','addizione'].includes(w)) sum = true;
    else if (['differenza','difference','sottrazione'].includes(w)) diff = true;
    else if (['prodotto','product'].includes(w)) prod = true;
    else if (['quoziente','quotient'].includes(w)) quot = true;
    else if (['togli','tolgo','sottrai','leva','subtract','take','remove'].includes(w)) take = true;
    else if (['aggiungi','aggiungo','add','addiziona'].includes(w)) addto = true;
    else if (['manca','mancano','missing'].includes(w)) manca = true;
    if (['da','from'].includes(w)) hasDa = true;
    if (['a','to','al','ad'].includes(w)) hasA = true;
  }
  if (num.length < 2) return null;
  let L, R2, op;
  if (take && hasDa) { L = num[1]; R2 = num[0]; op = '-'; }
  else if (addto && hasA) { L = num[0]; R2 = num[1]; op = '+'; }
  else if (manca) { L = num[1]; R2 = num[0]; op = '-'; }
  else if (sum) { L = num[0]; R2 = num[1]; op = '+'; }
  else if (diff) { L = num[0]; R2 = num[1]; op = '-'; }
  else if (prod) { L = num[0]; R2 = num[1]; op = '*'; }
  else if (quot) { L = num[0]; R2 = num[1]; op = '/'; }
  else return null;
  if (op === '/' && R2 === 0) return { intent: 'calc', conf: 95, reply: en ? "I can't divide by zero." : 'Non posso dividere per zero.' };
  const res = op === '-' ? L - R2 : op === '+' ? L + R2 : op === '*' ? L * R2 : L / R2;
  return { intent: 'calc', conf: 95, reply: `${fmtNum(L)} ${op} ${fmtNum(R2)} = ${fmtNum(res)}.` };
}
function solveFuncs(items, en) {
  let fn = 0, x = 0, havex = false, lbase = 0, deg = false, rad = false, prev = '';
  // mirrors a_solve_funcs: a lightly mistyped cube modifier next to a root word means cbrt
  const hasRadice = items.some(it => !it.num && (it.w === 'radice' || it.w === 'root'));
  const fmap = { ln:2, log2:3, log:1, logaritmo:1, logarithm:1, cubica:4, cbrt:4, arrotonda:5, round:5, arrotondare:5,
    pavimento:6, floor:6, soffitto:7, ceil:7, ceiling:7, seno:8, sin:8, sen:8, coseno:9, cos:9, tangente:10, tan:10, tg:10, esponenziale:11, exp:11 };
  for (const it of items) {
    if (it.num) { if (prev === 'base') { if (lbase === 0) lbase = it.val; } else { x = it.val; havex = true; } prev = '#'; continue; }
    const w = it.w; prev = w;
    const cubeMod = hasRadice && (damlev(w, 'cubica', 1) <= 1 || w === 'cube' || w === 'cubic');
    if (!fn) { if (fmap[w]) fn = fmap[w]; else if (cubeMod) fn = 4; }
    if (['naturale','natural'].includes(w) && fn === 1) fn = 2;
    if (['gradi','degrees','deg'].includes(w)) deg = true;
    if (['radianti','radians','rad'].includes(w)) rad = true;
  }
  if (!fn || !havex) return null;
  let res, note = '';
  switch (fn) {
    case 1: if (x <= 0) return null; res = (lbase > 0 && lbase !== 1) ? Math.log(x) / Math.log(lbase) : Math.log10(x); break;
    case 2: if (x <= 0) return null; res = Math.log(x); break;
    case 3: if (x <= 0) return null; res = Math.log2(x); break;
    case 4: res = Math.cbrt(x); break;
    case 5: res = Math.round(x); break;
    case 6: res = Math.floor(x); break;
    case 7: res = Math.ceil(x); break;
    case 8: case 9: case 10: { const useDeg = deg || !rad, ang = useDeg ? x * Math.PI / 180 : x;
      res = fn === 8 ? Math.sin(ang) : fn === 9 ? Math.cos(ang) : Math.tan(ang);
      if (Math.abs(res) < 1e-12) res = 0;
      note = en ? (useDeg ? ' (deg)' : ' (rad)') : (useDeg ? ' (gradi)' : ' (radianti)'); break; }
    case 11: res = Math.exp(x); break;
    default: return null;
  }
  const NM = ['','log','ln','log2','cbrt','round','floor','ceil','sin','cos','tan','exp'];
  let reply;
  if (fn === 1 && lbase > 0 && lbase !== 1) reply = en ? `log base ${fmtNum(lbase)} of ${fmtNum(x)} = ${fmtNum(res)}.` : `log base ${fmtNum(lbase)} di ${fmtNum(x)} = ${fmtNum(res)}.`;
  else if (fn === 4) reply = en ? `cube root of ${fmtNum(x)} = ${fmtNum(res)}.` : `radice cubica di ${fmtNum(x)} = ${fmtNum(res)}.`;
  else reply = `${NM[fn]}(${fmtNum(x)})${note} = ${fmtNum(res)}.`;
  return { intent: 'calc', conf: 95, reply };
}
function jPrime(x) { if (x < 2) return false; if (x % 2 === 0) return x === 2; if (x % 3 === 0) return x === 3; for (let i = 5; i*i <= x; i += 6) if (x % i === 0 || x % (i+2) === 0) return false; return true; }
function jLeastFactor(x) { if (x < 2) return 0; if (x % 2 === 0) return 2; for (let i = 3; i*i <= x; i += 2) if (x % i === 0) return i; return x; }
function solveNumprop(items, en) {
  let prime = false, fib = false; const nums = [];
  for (const it of items) { if (it.num) { nums.push(it.val); continue; } const w = it.w;
    if (['primo','prime','primi'].includes(w)) prime = true; if (['fibonacci','fib'].includes(w)) fib = true; }
  if (fib && nums.length >= 1) { const k = nums[nums.length - 1];
    if (k >= 0 && k <= 90 && Number.isInteger(k)) { let a = 0n, b = 1n; for (let i = 0; i < k; i++) { const t = a + b; a = b; b = t; }
      return { intent: 'calc', conf: 95, reply: `Fibonacci(${fmtNum(k)}) = ${a.toString()}.` }; } }
  if (prime && nums.length >= 1) { const xv = nums[nums.length - 1];
    if (xv >= 0 && xv < 1e15 && Number.isInteger(xv)) {
      if (jPrime(xv)) return { intent: 'prime', conf: 95, reply: en ? `Is ${fmtNum(xv)} prime? Yes.` : `${fmtNum(xv)} e primo? Si.` };
      const f = jLeastFactor(xv);
      if (f && f !== xv) return { intent: 'prime', conf: 95, reply: en ? `Is ${fmtNum(xv)} prime? No (${fmtNum(xv)} = ${fmtNum(f)} x ${fmtNum(xv/f)}).` : `${fmtNum(xv)} e primo? No (${fmtNum(xv)} = ${fmtNum(f)} x ${fmtNum(xv/f)}).` };
      return { intent: 'prime', conf: 95, reply: en ? `Is ${fmtNum(xv)} prime? No.` : `${fmtNum(xv)} e primo? No.` };
    } }
  return null;
}

// ---- GEOMETRY + PHYSICS frames — exact mirrors of a_solve_geometry / a_solve_physics in
// nucleo_anima.c. Adjacency-based dimension binding (jDim), shape/keyword gating, COMPUTE or
// TEACH. Keep IDENTICAL to the firmware (math-check cross-checks reply parity). ----
function jDim(items, kw) {                          // mirrors a_dim: number adjacent to a label
  const CONN = ['di','of','con','with','del','dello','della'];
  for (let i = 0; i < items.length; i++) {
    if (items[i].num) continue;
    if (kw.includes(items[i].w)) {
      if (i+1 < items.length && items[i+1].num) return items[i+1].val;
      if (i+2 < items.length && !items[i+1].num && items[i+2].num && CONN.includes(items[i+1].w)) return items[i+2].val;
    }
  }
  return NaN;
}
function jShapeWord(w) {
  if (['cerchio','circle','cerchi'].includes(w)) return 'cir';
  if (['quadrato','square'].includes(w)) return 'sq';
  if (['rettangolo','rectangle'].includes(w)) return 'rec';
  if (['triangolo','triangle','ipotenusa','hypotenuse','cateto','cateti','leg','legs'].includes(w)) return 'tri';
  if (['trapezio','trapezoid','trapezium'].includes(w)) return 'trap';
  if (['rombo','rhombus'].includes(w)) return 'rho';
  if (['parallelogramma','parallelogram'].includes(w)) return 'par';
  if (['cubo','cube'].includes(w)) return 'cube';
  if (['sfera','sphere'].includes(w)) return 'sph';
  if (['cilindro','cylinder'].includes(w)) return 'cyl';
  if (['cono','cone'].includes(w)) return 'cone';
  return '';
}
function jGeometry(items, en) {
  let shape='', q='', formula=false, dimLabel=false, pyth=false, circW=false;
  let pigreco=false, haspi=false, hasgreco=false, hasval=false; const nums=[];
  for (const it of items) {
    if (it.num) { nums.push(it.val); continue; }
    const w = it.w; const s = jShapeWord(w); if (s && !shape) shape = s;
    if (['circonferenza','circumference'].includes(w)) circW = true;
    if (['ipotenusa','hypotenuse','cateto','cateti','leg','legs'].includes(w)) pyth = true;
    if (['pitagora','pythagoras','pythagorean'].includes(w)) { pyth = true; if (!shape) shape = 'tri'; formula = true; }
    if (w === 'area') q = 'area';
    else if (['perimetro','perimeter'].includes(w)) q = 'perim';
    else if (w === 'volume') q = 'vol';
    else if (['superficie','surface','superficiale'].includes(w)) q = 'surf';
    else if (['diagonale','diagonal'].includes(w)) q = 'diag';
    else if (['diametro','diameter'].includes(w) && !q) q = 'diam';
    else if (['circonferenza','circumference'].includes(w) && !q) q = 'circ';
    if (['formula','formule','regola','rule','calcola','calcolare','calcolo','calculate'].includes(w)) formula = true;
    if (['raggio','radius','lato','side','spigolo','edge','base','altezza','height','apotema','diametro','diameter'].includes(w)) dimLabel = true;
    if (w === 'pigreco') pigreco = true;
    if (w === 'pi') haspi = true;
    if (['greco','greca','greek'].includes(w)) hasgreco = true;
    if (['valore','vale','value','costante','constant'].includes(w)) hasval = true;
  }
  if (circW && !shape) shape = 'cir';
  if (circW && !q) q = 'circ';
  const f = fmtNum, PI = Math.PI, sq = Math.sqrt;
  const G = (reply) => ({ intent: 'geo', conf: 95, reply });
  if (pigreco || (haspi && (hasgreco || hasval))) return G(en ? 'pi (π) = 3.14159265.' : 'Pi greco (π) = 3.14159265.');
  const fire = (shape && (q || dimLabel || formula)) || (pyth && nums.length >= 2);
  if (!fire) return null;
  const R = jDim(items,['raggio','radius']), Dd = jDim(items,['diametro','diameter']),
        L = jDim(items,['lato','side','spigolo','edge']), Bb = jDim(items,['base']),
        Hh = jDim(items,['altezza','height','alta','alto']);
  if (shape === 'tri' && pyth) {
    const hk = jDim(items,['ipotenusa','hypotenuse']);
    if (!isNaN(hk) && nums.length >= 2) {
      let leg = NaN; for (const xx of nums) if (xx !== hk) { leg = xx; break; }
      if (isNaN(leg) || leg >= hk) return null;
      return G(en ? `Leg = √(${f(hk)}²-${f(leg)}²) = ${f(sq(hk*hk-leg*leg))}.` : `Cateto = √(${f(hk)}²-${f(leg)}²) = ${f(sq(hk*hk-leg*leg))}.`);
    }
    if (nums.length >= 2) return G(en ? `Hypotenuse = √(${f(nums[0])}²+${f(nums[1])}²) = ${f(sq(nums[0]*nums[0]+nums[1]*nums[1]))}.` : `Ipotenusa = √(${f(nums[0])}²+${f(nums[1])}²) = ${f(sq(nums[0]*nums[0]+nums[1]*nums[1]))}.`);
    return G(en ? 'Pythagoras: c = √(a²+b²).' : 'Pitagora: c = √(a²+b²).');
  }
  if (shape === 'cir') {
    let rad = R; if (isNaN(rad) && !isNaN(Dd)) rad = Dd/2; if (isNaN(rad) && nums.length >= 1) rad = nums[0];
    if (isNaN(rad)) {
      if (q === 'perim' || q === 'circ') return G(en ? 'Circumference: C = 2·π·r.' : 'Circonferenza: C = 2·π·r.');
      if (q === 'diam') return G(en ? 'Diameter: d = 2·r.' : 'Diametro: d = 2·r.');
      return G(en ? 'Circle area: A = π·r².' : 'Area del cerchio: A = π·r².');
    }
    if (q === 'perim' || q === 'circ') return G(`${en?'Circumference':'Circonferenza'} = 2·π·${f(rad)} = ${f(2*PI*rad)}.`);
    if (q === 'diam') return G(`${en?'Diameter':'Diametro'} = 2·${f(rad)} = ${f(2*rad)}.`);
    if (q === 'area') return G(en ? `Circle area = π·${f(rad)}² = ${f(PI*rad*rad)}.` : `Area del cerchio = π·${f(rad)}² = ${f(PI*rad*rad)}.`);
    return G(en ? `Circle r=${f(rad)}: area ${f(PI*rad*rad)}, circumference ${f(2*PI*rad)}.` : `Cerchio r=${f(rad)}: area ${f(PI*rad*rad)}, circonferenza ${f(2*PI*rad)}.`);
  }
  if (shape === 'sq') {
    let sd = L; if (isNaN(sd) && nums.length >= 1) sd = nums[0];
    if (isNaN(sd)) {
      if (q === 'perim') return G(en ? 'Square perimeter: P = 4·l.' : 'Perimetro del quadrato: P = 4·l.');
      if (q === 'diag') return G(en ? 'Square diagonal: d = l·√2.' : 'Diagonale del quadrato: d = l·√2.');
      return G(en ? 'Square area: A = l².' : 'Area del quadrato: A = l².');
    }
    if (q === 'perim') return G(`${en?'Perimeter':'Perimetro'} = 4·${f(sd)} = ${f(4*sd)}.`);
    if (q === 'diag') return G(`${en?'Diagonal':'Diagonale'} = ${f(sd)}·√2 = ${f(sd*sq(2))}.`);
    if (q === 'area') return G(en ? `Square area = ${f(sd)}² = ${f(sd*sd)}.` : `Area del quadrato = ${f(sd)}² = ${f(sd*sd)}.`);
    return G(en ? `Square l=${f(sd)}: area ${f(sd*sd)}, perimeter ${f(4*sd)}.` : `Quadrato l=${f(sd)}: area ${f(sd*sd)}, perimetro ${f(4*sd)}.`);
  }
  if (shape === 'rec') {
    let b = Bb, hh = Hh; if (isNaN(b) && nums.length >= 1) b = nums[0]; if (isNaN(hh) && nums.length >= 2) hh = nums[1];
    if (isNaN(b) || isNaN(hh)) {
      if (q === 'perim') return G(en ? 'Rectangle perimeter: P = 2·(b+h).' : 'Perimetro del rettangolo: P = 2·(b+h).');
      if (q === 'diag') return G(en ? 'Rectangle diagonal: d = √(b²+h²).' : 'Diagonale del rettangolo: d = √(b²+h²).');
      return G(en ? 'Rectangle area: A = b·h.' : 'Area del rettangolo: A = b·h.');
    }
    if (q === 'perim') return G(`${en?'Perimeter':'Perimetro'} = 2·(${f(b)}+${f(hh)}) = ${f(2*(b+hh))}.`);
    if (q === 'diag') return G(`${en?'Diagonal':'Diagonale'} = √(${f(b)}²+${f(hh)}²) = ${f(sq(b*b+hh*hh))}.`);
    if (q === 'area') return G(en ? `Rectangle area = ${f(b)}·${f(hh)} = ${f(b*hh)}.` : `Area del rettangolo = ${f(b)}·${f(hh)} = ${f(b*hh)}.`);
    return G(en ? `Rectangle ${f(b)}×${f(hh)}: area ${f(b*hh)}.` : `Rettangolo ${f(b)}×${f(hh)}: area ${f(b*hh)}.`);
  }
  if (shape === 'tri') {
    let b = Bb, hh = Hh; if (isNaN(b) && nums.length >= 1) b = nums[0]; if (isNaN(hh) && nums.length >= 2) hh = nums[1];
    if (isNaN(b) || isNaN(hh)) return G(en ? 'Triangle area: A = (b·h)/2.' : 'Area del triangolo: A = (b·h)/2.');
    return G(en ? `Triangle area = (${f(b)}·${f(hh)})/2 = ${f(b*hh/2)}.` : `Area del triangolo = (${f(b)}·${f(hh)})/2 = ${f(b*hh/2)}.`);
  }
  if (shape === 'trap') {
    const hh = isNaN(Hh) ? (nums.length >= 3 ? nums[2] : NaN) : Hh;
    if (nums.length < 3 || isNaN(hh)) return G(en ? 'Trapezoid area: A = (B+b)·h/2.' : 'Area del trapezio: A = (B+b)·h/2.');
    return G(en ? `Trapezoid area = (${f(nums[0])}+${f(nums[1])})·h/2 = ${f((nums[0]+nums[1])*hh/2)}.` : `Area del trapezio = (${f(nums[0])}+${f(nums[1])})·h/2 = ${f((nums[0]+nums[1])*hh/2)}.`);
  }
  if (shape === 'rho') {
    if (q === 'perim' && !isNaN(L)) return G(`${en?'Perimeter':'Perimetro'} = 4·${f(L)} = ${f(4*L)}.`);
    if (nums.length < 2) return G(en ? 'Rhombus area: A = (d1·d2)/2.' : 'Area del rombo: A = (d1·d2)/2.');
    return G(en ? `Rhombus area = (${f(nums[0])}·${f(nums[1])})/2 = ${f(nums[0]*nums[1]/2)}.` : `Area del rombo = (${f(nums[0])}·${f(nums[1])})/2 = ${f(nums[0]*nums[1]/2)}.`);
  }
  if (shape === 'par') {
    let b = Bb, hh = Hh; if (isNaN(b) && nums.length >= 1) b = nums[0]; if (isNaN(hh) && nums.length >= 2) hh = nums[1];
    if (isNaN(b) || isNaN(hh)) return G(en ? 'Parallelogram area: A = b·h.' : 'Area del parallelogramma: A = b·h.');
    return G(en ? `Parallelogram area = ${f(b)}·${f(hh)} = ${f(b*hh)}.` : `Area del parallelogramma = ${f(b)}·${f(hh)} = ${f(b*hh)}.`);
  }
  if (shape === 'cube') {
    let sd = L; if (isNaN(sd) && nums.length >= 1) sd = nums[0];
    if (isNaN(sd)) { if ((q === 'surf' || q === 'area')) return G(en ? 'Cube surface: S = 6·l².' : 'Superficie del cubo: S = 6·l².'); return G(en ? 'Cube volume: V = l³.' : 'Volume del cubo: V = l³.'); }
    if ((q === 'surf' || q === 'area')) return G(en ? `Cube surface = 6·${f(sd)}² = ${f(6*sd*sd)}.` : `Superficie del cubo = 6·${f(sd)}² = ${f(6*sd*sd)}.`);
    return G(en ? `Cube volume = ${f(sd)}³ = ${f(sd*sd*sd)}.` : `Volume del cubo = ${f(sd)}³ = ${f(sd*sd*sd)}.`);
  }
  if (shape === 'sph') {
    let rad = R; if (isNaN(rad) && !isNaN(Dd)) rad = Dd/2; if (isNaN(rad) && nums.length >= 1) rad = nums[0];
    if (isNaN(rad)) { if ((q === 'surf' || q === 'area')) return G(en ? 'Sphere surface: S = 4·π·r².' : 'Superficie della sfera: S = 4·π·r².'); return G(en ? 'Sphere volume: V = (4/3)·π·r³.' : 'Volume della sfera: V = (4/3)·π·r³.'); }
    if ((q === 'surf' || q === 'area')) return G(en ? `Sphere surface = 4·π·${f(rad)}² = ${f(4*PI*rad*rad)}.` : `Superficie della sfera = 4·π·${f(rad)}² = ${f(4*PI*rad*rad)}.`);
    return G(en ? `Sphere volume = (4/3)·π·${f(rad)}³ = ${f(4/3*PI*rad*rad*rad)}.` : `Volume della sfera = (4/3)·π·${f(rad)}³ = ${f(4/3*PI*rad*rad*rad)}.`);
  }
  if (shape === 'cyl') {
    let rad = R; if (isNaN(rad) && nums.length >= 1) rad = nums[0]; let hh = Hh; if (isNaN(hh) && nums.length >= 2) hh = nums[1];
    if (isNaN(rad) || isNaN(hh)) { if ((q === 'surf' || q === 'area')) return G(en ? 'Cylinder surface: S = 2·π·r·(r+h).' : 'Superficie del cilindro: S = 2·π·r·(r+h).'); return G(en ? 'Cylinder volume: V = π·r²·h.' : 'Volume del cilindro: V = π·r²·h.'); }
    if ((q === 'surf' || q === 'area')) return G(en ? `Cylinder surface = 2·π·${f(rad)}·(${f(rad)}+${f(hh)}) = ${f(2*PI*rad*(rad+hh))}.` : `Superficie cilindro = 2·π·${f(rad)}·(${f(rad)}+${f(hh)}) = ${f(2*PI*rad*(rad+hh))}.`);
    return G(en ? `Cylinder volume = π·${f(rad)}²·${f(hh)} = ${f(PI*rad*rad*hh)}.` : `Volume del cilindro = π·${f(rad)}²·${f(hh)} = ${f(PI*rad*rad*hh)}.`);
  }
  if (shape === 'cone') {
    let rad = R; if (isNaN(rad) && nums.length >= 1) rad = nums[0]; let hh = Hh; if (isNaN(hh) && nums.length >= 2) hh = nums[1];
    if (isNaN(rad) || isNaN(hh)) return G(en ? 'Cone volume: V = (1/3)·π·r²·h.' : 'Volume del cono: V = (1/3)·π·r²·h.');
    return G(en ? `Cone volume = (1/3)·π·${f(rad)}²·${f(hh)} = ${f(PI*rad*rad*hh/3)}.` : `Volume del cono = (1/3)·π·${f(rad)}²·${f(hh)} = ${f(PI*rad*rad*hh/3)}.`);
  }
  return null;
}
// Physics unit roles — exact mirror of phys_unit / phys_label / phys_label_dim in nucleo_anima.c.
const PU_NONE=0,PU_LEN=1,PU_TIME=2,PU_MASS=3,PU_AREA=4,PU_VOL=5,
      PU_VEL=6,PU_ACC=7,PU_FORCE=8,PU_ENERGY=9,PU_POWER=10,PU_PRESS=11,PU_FREQ=12,PU_DENS=13;
const PHYS_UNITS = {
  mm:[PU_LEN,0.001],cm:[PU_LEN,0.01],dm:[PU_LEN,0.1],m:[PU_LEN,1],metro:[PU_LEN,1],metri:[PU_LEN,1],
  meter:[PU_LEN,1],meters:[PU_LEN,1],metre:[PU_LEN,1],metres:[PU_LEN,1],
  km:[PU_LEN,1000],chilometro:[PU_LEN,1000],chilometri:[PU_LEN,1000],kilometer:[PU_LEN,1000],kilometers:[PU_LEN,1000],
  s:[PU_TIME,1],sec:[PU_TIME,1],secondo:[PU_TIME,1],secondi:[PU_TIME,1],second:[PU_TIME,1],seconds:[PU_TIME,1],
  min:[PU_TIME,60],minuto:[PU_TIME,60],minuti:[PU_TIME,60],minute:[PU_TIME,60],minutes:[PU_TIME,60],
  h:[PU_TIME,3600],ora:[PU_TIME,3600],ore:[PU_TIME,3600],hour:[PU_TIME,3600],hours:[PU_TIME,3600],
  mg:[PU_MASS,1e-6],g:[PU_MASS,0.001],grammo:[PU_MASS,0.001],grammi:[PU_MASS,0.001],gram:[PU_MASS,0.001],grams:[PU_MASS,0.001],
  kg:[PU_MASS,1],chilo:[PU_MASS,1],chili:[PU_MASS,1],chilogrammo:[PU_MASS,1],chilogrammi:[PU_MASS,1],
  kilogram:[PU_MASS,1],kilograms:[PU_MASS,1],tonnellata:[PU_MASS,1000],tonnellate:[PU_MASS,1000],ton:[PU_MASS,1000],
  m2:[PU_AREA,1],mq:[PU_AREA,1],cm2:[PU_AREA,0.0001],dm2:[PU_AREA,0.01],
  m3:[PU_VOL,1],dm3:[PU_VOL,0.001],cm3:[PU_VOL,1e-6],litro:[PU_VOL,0.001],litri:[PU_VOL,0.001],l:[PU_VOL,0.001],
  'm/s':[PU_VEL,1],'km/h':[PU_VEL,1/3.6],kmh:[PU_VEL,1/3.6],kph:[PU_VEL,1/3.6],mph:[PU_VEL,0.44704],
  'm/s2':[PU_ACC,1],ms2:[PU_ACC,1],
  n:[PU_FORCE,1],newton:[PU_FORCE,1],newtons:[PU_FORCE,1],kn:[PU_FORCE,1000],
  j:[PU_ENERGY,1],joule:[PU_ENERGY,1],joules:[PU_ENERGY,1],kj:[PU_ENERGY,1000],
  w:[PU_POWER,1],watt:[PU_POWER,1],watts:[PU_POWER,1],kw:[PU_POWER,1000],
  pa:[PU_PRESS,1],pascal:[PU_PRESS,1],kpa:[PU_PRESS,1000],hpa:[PU_PRESS,100],bar:[PU_PRESS,100000],atm:[PU_PRESS,101325],
  hz:[PU_FREQ,1],hertz:[PU_FREQ,1],khz:[PU_FREQ,1000],mhz:[PU_FREQ,1e6],
  'kg/m3':[PU_DENS,1],'g/cm3':[PU_DENS,1000],'g/l':[PU_DENS,1],
};
function jPhysUnit(w){ const e = PHYS_UNITS[w]; return e ? { role:e[0], si:e[1] } : { role:PU_NONE, si:1 }; }
const L_NONE=0,L_V=1,L_S=2,L_T=3,L_M=4,L_AC=5,L_F=6,L_H=7,L_LV=8,L_DN=9,L_PR=10,L_FR=11,L_TP=12,L_EN=13,L_VV=14,L_AR=15;
function jPhysLabel(w){
  if (['velocita','speed','celerita'].includes(w)) return L_V;
  if (['spazio','distanza','distance','space','spostamento','percorso','tragitto'].includes(w)) return L_S;
  if (['tempo','time','durata'].includes(w)) return L_T;
  if (['massa','mass'].includes(w)) return L_M;
  if (['accelerazione','acceleration'].includes(w)) return L_AC;
  if (['forza','force'].includes(w)) return L_F;
  if (['altezza','height','quota','alto','alta','altitudine'].includes(w)) return L_H;
  if (['lavoro','work'].includes(w)) return L_LV;
  if (['densita','density'].includes(w)) return L_DN;
  if (['pressione','pressure'].includes(w)) return L_PR;
  if (['frequenza','frequency'].includes(w)) return L_FR;
  if (['periodo','period'].includes(w)) return L_TP;
  if (['energia','energy'].includes(w)) return L_EN;
  if (w === 'volume') return L_VV;
  if (['area','superficie','surface'].includes(w)) return L_AR;
  return L_NONE;
}
function jPhysLabelDim(lbl){
  if (lbl === L_S || lbl === L_H) return PU_LEN;
  if (lbl === L_T || lbl === L_TP) return PU_TIME;
  if (lbl === L_M) return PU_MASS;
  if (lbl === L_AR) return PU_AREA;
  if (lbl === L_VV) return PU_VOL;
  return 0;
}
function jIsConn(w){ return ['di','of','con','with','del','dello','della'].includes(w); }
function jPhysics(items, en) {
  let kVel=0,kSpace=0,kTime=0,kMass=0,kAcc=0,kForce=0,kHeight=0,kWork=0,kPower=0,
      kWeight=0,kDens=0,kPress=0,kFreq=0,kPeriod=0,kKin=0,kPot=0,kEnergy=0,hasQ=0,hasMoto=0,hasMom=0,formula=0;
  let elec=false;                                    // electrical power -> a_solve_ohm owns it (collaboration)
  for (const it of items) if(!it.num && /volt|amper|ohm|tension|corrent|resisten/.test(it.w)) elec=true;
  for (const it of items) {
    if (it.num) continue;
    const w = it.w;
    if (['velocita','speed','celerita'].includes(w)) kVel=1;
    if (['spazio','distanza','distance','space','spostamento','percorso','tragitto'].includes(w)) kSpace=1;
    if (['tempo','time','durata'].includes(w)) kTime=1;
    if (['massa','mass'].includes(w)) kMass=1;
    if (['accelerazione','acceleration','accelera'].includes(w)) kAcc=1;
    if (['forza','force'].includes(w)) kForce=1;
    if (['altezza','height','quota','alto','alta','altitudine'].includes(w)) kHeight=1;
    if (['lavoro','work'].includes(w)) kWork=1;
    if (['potenza','power'].includes(w) && !elec) kPower=1;   // electrical power -> jUnits/ohm
    if (['peso','pesa','pesano','weight','weighs','weigh'].includes(w)) kWeight=1;
    if (['densita','density'].includes(w)) kDens=1;
    if (['pressione','pressure'].includes(w)) kPress=1;
    if (['frequenza','frequency'].includes(w)) kFreq=1;
    if (['periodo','period'].includes(w)) kPeriod=1;
    if (['energia','energy'].includes(w)) kEnergy=1;
    if (['cinetica','kinetic'].includes(w)) kKin=1;
    if (['potenziale','potential'].includes(w)) kPot=1;
    if (w === 'quantita') hasQ=1;
    if (w === 'moto') hasMoto=1;
    if (w === 'momentum') hasMom=1;
    if (['formula','formule','regola','rule','calcola','calcolare','calcolo','calculate'].includes(w)) formula=1;
  }
  const kMom = hasMom || (hasQ && hasMoto);
  let v=NaN,s=NaN,t=NaN,m=NaN,ac=NaN,F=NaN,h=NaN,Lv=NaN,Dn=NaN,Pr=NaN,Fr=NaN,Tp=NaN,En=NaN,Vv=NaN,Ar=NaN,Pw=NaN;
  const setf = (cur, val) => isNaN(cur) ? val : cur;
  for (let i = 0; i < items.length; i++) {
    if (!items[i].num) continue;
    const x = items[i].val; let urole = PU_NONE, usi = 1;
    if (i+1 < items.length && !items[i+1].num) { const u = jPhysUnit(items[i+1].w); urole = u.role; usi = u.si; }
    let lbl = L_NONE;
    if (i >= 1 && !items[i-1].num) {
      lbl = jPhysLabel(items[i-1].w);
      if (!lbl && jIsConn(items[i-1].w) && i >= 2 && !items[i-2].num) lbl = jPhysLabel(items[i-2].w);
    }
    if (!lbl && urole !== PU_NONE && i+2 < items.length && !items[i+2].num) {
      lbl = jPhysLabel(items[i+2].w);
      if (!lbl && jIsConn(items[i+2].w) && i+3 < items.length && !items[i+3].num) lbl = jPhysLabel(items[i+3].w);
    }
    if (lbl && urole > PU_NONE && urole < PU_VEL && jPhysLabelDim(lbl) !== urole) lbl = L_NONE;
    if (urole >= PU_VEL) {                              // derived-quantity unit -> decisive
      if (urole === PU_VEL) { v = setf(v, x*usi); kVel=1; }
      else if (urole === PU_ACC) { ac = setf(ac, x*usi); kAcc=1; }
      else if (urole === PU_FORCE) { F = setf(F, x*usi); kForce=1; }
      else if (urole === PU_ENERGY) { En = setf(En, x*usi); kEnergy=1; }
      else if (urole === PU_POWER) { Pw = setf(Pw, x*usi); kPower=1; }
      else if (urole === PU_PRESS) { Pr = setf(Pr, x*usi); kPress=1; }
      else if (urole === PU_FREQ) { Fr = setf(Fr, x*usi); kFreq=1; }
      else if (urole === PU_DENS) { Dn = setf(Dn, x*usi); kDens=1; }
    } else {
      const sf = (urole !== PU_NONE) ? usi : 1;
      if (lbl) {
        switch (lbl) {
          case L_V: v=setf(v,x*sf); break;  case L_S: s=setf(s,x*sf); break;
          case L_T: t=setf(t,x*sf); break;  case L_M: m=setf(m,x*sf); break;
          case L_AC: ac=setf(ac,x*sf); break; case L_F: F=setf(F,x*sf); break;
          case L_H: h=setf(h,x*sf); break;  case L_LV: Lv=setf(Lv,x*sf); break;
          case L_DN: Dn=setf(Dn,x*sf); break; case L_PR: Pr=setf(Pr,x*sf); break;
          case L_FR: Fr=setf(Fr,x*sf); break; case L_TP: Tp=setf(Tp,x*sf); break;
          case L_EN: En=setf(En,x*sf); break; case L_VV: Vv=setf(Vv,x*sf); break;
          case L_AR: Ar=setf(Ar,x*sf); break;
        }
      } else if (urole !== PU_NONE) {
        if (urole === PU_LEN) { s=setf(s,x*usi); kSpace=1; }
        else if (urole === PU_TIME) { t=setf(t,x*usi); kTime=1; }
        else if (urole === PU_MASS) { m=setf(m,x*usi); kMass=1; }
        else if (urole === PU_AREA) { Ar=setf(Ar,x*usi); }
        else if (urole === PU_VOL) { Vv=setf(Vv,x*usi); }
      }
    }
  }
  const f = fmtNum, g = 9.81, sq = Math.sqrt, P = (reply) => ({ intent: 'phys', conf: 93, reply });
  // COMPUTE first (teach deferred so a m/s in "calcola l'energia cinetica" can't shadow the kinetic compute)
  if (kWeight) {
    if (!isNaN(m)) return P(`P = m·g = ${f(m)}·9.81 = ${f(m*g)} N.`);
    if (!isNaN(F)) return P(`m = P/g = ${f(F)}/9.81 = ${f(F/g)} kg.`);
  }
  if (kVel || (kSpace && kTime)) {
    const known = (!isNaN(v)) + (!isNaN(s)) + (!isNaN(t));
    if (known >= 2) {
      if (isNaN(v) && t !== 0) return P(`v = s/t = ${f(s)}/${f(t)} = ${f(s/t)} m/s.`);
      if (isNaN(s)) return P(`s = v·t = ${f(v)}·${f(t)} = ${f(v*t)} m.`);
      if (isNaN(t) && v !== 0) return P(`t = s/v = ${f(s)}/${f(v)} = ${f(s/v)} s.`);
    }
  }
  if (kForce || (kMass && kAcc)) {
    const known = (!isNaN(F)) + (!isNaN(m)) + (!isNaN(ac));
    if (known >= 2) {
      if (isNaN(F)) return P(`F = m·a = ${f(m)}·${f(ac)} = ${f(m*ac)} N.`);
      if (isNaN(m) && ac !== 0) return P(`m = F/a = ${f(F)}/${f(ac)} = ${f(F/ac)} kg.`);
      if (isNaN(ac) && m !== 0) return P(`a = F/m = ${f(F)}/${f(m)} = ${f(F/m)} m/s².`);
    }
  }
  if (kKin || (kEnergy && kVel && !kPot)) {
    if (!isNaN(m) && !isNaN(v)) return P(`Ec = ½·m·v² = ½·${f(m)}·${f(v)}² = ${f(0.5*m*v*v)} J.`);
    if (!isNaN(En) && !isNaN(m) && m !== 0) return P(`v = √(2·Ec/m) = √(2·${f(En)}/${f(m)}) = ${f(sq(2*En/m))} m/s.`);
  }
  if (kPot || (kEnergy && kHeight)) {
    const hh = !isNaN(h) ? h : s;                      // a lone length in a PE problem is the height
    if (!isNaN(m) && !isNaN(hh)) return P(`Ep = m·g·h = ${f(m)}·9.81·${f(hh)} = ${f(m*g*hh)} J.`);
    if (!isNaN(En) && !isNaN(m) && m !== 0) return P(`h = Ep/(m·g) = ${f(En)}/(${f(m)}·9.81) = ${f(En/(m*g))} m.`);
  }
  if (kMom) {
    if (!isNaN(m) && !isNaN(v)) return P(`q = m·v = ${f(m)}·${f(v)} = ${f(m*v)} kg·m/s.`);
  }
  if (kDens) {
    if (!isNaN(m) && !isNaN(Vv) && Vv !== 0) return P(`ρ = m/V = ${f(m)}/${f(Vv)} = ${f(m/Vv)} kg/m³.`);
    if (!isNaN(Dn) && !isNaN(Vv)) return P(`m = ρ·V = ${f(Dn)}·${f(Vv)} = ${f(Dn*Vv)} kg.`);
    if (!isNaN(Dn) && !isNaN(m) && Dn !== 0) return P(`V = m/ρ = ${f(m)}/${f(Dn)} = ${f(m/Dn)} m³.`);
  }
  if (kPress) {
    if (!isNaN(F) && !isNaN(Ar) && Ar !== 0) return P(`p = F/A = ${f(F)}/${f(Ar)} = ${f(F/Ar)} Pa.`);
    if (!isNaN(Pr) && !isNaN(Ar)) return P(`F = p·A = ${f(Pr)}·${f(Ar)} = ${f(Pr*Ar)} N.`);
  }
  if (kWork) {
    if (!isNaN(F) && !isNaN(s)) return P(`L = F·s = ${f(F)}·${f(s)} = ${f(F*s)} J.`);
  }
  if (kPower) {
    const wk = !isNaN(Lv) ? Lv : En;
    if (!isNaN(wk) && !isNaN(t) && t !== 0) return P(`P = L/t = ${f(wk)}/${f(t)} = ${f(wk/t)} W.`);
    if (!isNaN(Pw) && !isNaN(t)) return P(`E = P·t = ${f(Pw)}·${f(t)} = ${f(Pw*t)} J.`);
  }
  if (kFreq || kPeriod) {
    if (!isNaN(Fr) && Fr !== 0) return P(`T = 1/f = 1/${f(Fr)} = ${f(1/Fr)} s.`);
    if (!isNaN(Tp) && Tp !== 0) return P(`f = 1/T = 1/${f(Tp)} = ${f(1/Tp)} Hz.`);
  }
  // TEACH fallback (no values) — specific quantities first; suppressed in electrical context
  if (formula && !elec) {
    if (kWeight) return P(en ? 'Weight: P = m·g  (g = 9.81 m/s²).' : 'Peso: P = m·g  (g = 9.81 m/s²).');
    if (kKin || (kEnergy && !kPot && !kHeight)) return P(en ? 'Kinetic energy: Ec = ½·m·v².' : 'Energia cinetica: Ec = ½·m·v².');
    if (kPot || (kEnergy && kHeight)) return P(en ? 'Potential energy: Ep = m·g·h.' : 'Energia potenziale: Ep = m·g·h.');
    if (kMom) return P(en ? 'Momentum: q = m·v.' : 'Quantità di moto: q = m·v.');
    if (kDens) return P(en ? 'Density: ρ = m/V.' : 'Densità: ρ = m/V.');
    if (kPress) return P(en ? 'Pressure: p = F/A.' : 'Pressione: p = F/A.');
    if (kWork) return P(en ? 'Work: L = F·s.' : 'Lavoro: L = F·s.');
    if (kPower) return P(en ? 'Power: P = work / time.' : 'Potenza: P = lavoro / tempo.');
    if (kForce || (kMass && kAcc)) return P(en ? "Newton's 2nd law: F = m·a." : '2ª legge di Newton: F = m·a.');
    if (kFreq || kPeriod) return P(en ? 'Frequency: f = 1/T.' : 'Frequenza: f = 1/T.');
    if (kVel || (kSpace && kTime)) return P(en ? 'Speed: v = distance / time.' : 'Velocità: v = spazio / tempo.');
  }
  return null;
}
function jVector(items, en) {                        // mirrors a_solve_vector: |v| and v·w
  let vec=false, dot=false; const nums=[];
  for (const it of items) {
    if (it.num) { if (nums.length < 12) nums.push(it.val); continue; }
    const w = it.w;
    if (['vettore','vettori','vector','vectors'].includes(w)) vec=true;
    if (['scalare','dot'].includes(w)) dot=true;
  }
  if (!vec || nums.length < 2) return null;
  const f = fmtNum;
  if (dot && nums.length % 2 === 0) {
    const half = nums.length/2; let s=0; const parts=[];
    for (let i=0;i<half;i++){ s+=nums[i]*nums[i+half]; parts.push(`${f(nums[i])}·${f(nums[i+half])}`); }
    return { intent:'calc', conf:94, reply: en?`v·w = ${parts.join('+')} = ${f(s)}.`:`Prodotto scalare = ${parts.join('+')} = ${f(s)}.` };
  }
  let s=0; const parts=[];
  for (let i=0;i<nums.length;i++){ s+=nums[i]*nums[i]; parts.push(`${f(nums[i])}²`); }
  return { intent:'calc', conf:94, reply: en?`|v| = √(${parts.join('+')}) = ${f(Math.sqrt(s))}.`:`Modulo |v| = √(${parts.join('+')}) = ${f(Math.sqrt(s))}.` };
}

// ---- UNIT ENGINE (jUnits) — exact mirror of a_solve_units in nucleo_anima.c. Dimensional analysis:
// each unit = (factor to SI, dim signature [L,M,T,K,D,A]); convert iff dims match, else refuse+explain;
// SI prefixes, powers of ten, learned units. Keep IDENTICAL to the firmware (math-check parity). ----
const U_TAB_RAW = [
  ['nm',1e-9,1,0,0,0,0,0],['um',1e-6,1,0,0,0,0,0],['micrometro',1e-6,1,0,0,0,0,0],
  ['mm',1e-3,1,0,0,0,0,0],['millimetro',1e-3,1,0,0,0,0,0],['millimetri',1e-3,1,0,0,0,0,0],
  ['cm',1e-2,1,0,0,0,0,0],['centimetro',1e-2,1,0,0,0,0,0],['centimetri',1e-2,1,0,0,0,0,0],
  ['dm',0.1,1,0,0,0,0,0],['decimetro',0.1,1,0,0,0,0,0],['decimetri',0.1,1,0,0,0,0,0],
  ['m',1,1,0,0,0,0,0],['metro',1,1,0,0,0,0,0],['metri',1,1,0,0,0,0,0],['meter',1,1,0,0,0,0,0],['meters',1,1,0,0,0,0,0],
  ['dam',10,1,0,0,0,0,0],['hm',100,1,0,0,0,0,0],
  ['km',1000,1,0,0,0,0,0],['chilometro',1000,1,0,0,0,0,0],['chilometri',1000,1,0,0,0,0,0],['kilometer',1000,1,0,0,0,0,0],['kilometers',1000,1,0,0,0,0,0],
  ['pollice',0.0254,1,0,0,0,0,0],['pollici',0.0254,1,0,0,0,0,0],['inch',0.0254,1,0,0,0,0,0],['inches',0.0254,1,0,0,0,0,0],
  ['piede',0.3048,1,0,0,0,0,0],['piedi',0.3048,1,0,0,0,0,0],['ft',0.3048,1,0,0,0,0,0],['feet',0.3048,1,0,0,0,0,0],['foot',0.3048,1,0,0,0,0,0],
  ['iarda',0.9144,1,0,0,0,0,0],['iarde',0.9144,1,0,0,0,0,0],['yard',0.9144,1,0,0,0,0,0],['yards',0.9144,1,0,0,0,0,0],
  ['miglio',1609.344,1,0,0,0,0,0],['miglia',1609.344,1,0,0,0,0,0],['mile',1609.344,1,0,0,0,0,0],['miles',1609.344,1,0,0,0,0,0],
  ['mg',1e-6,0,1,0,0,0,0],['milligrammo',1e-6,0,1,0,0,0,0],['milligrammi',1e-6,0,1,0,0,0,0],
  ['g',1e-3,0,1,0,0,0,0],['grammo',1e-3,0,1,0,0,0,0],['grammi',1e-3,0,1,0,0,0,0],['gram',1e-3,0,1,0,0,0,0],['grams',1e-3,0,1,0,0,0,0],
  ['hg',0.1,0,1,0,0,0,0],['etto',0.1,0,1,0,0,0,0],['etti',0.1,0,1,0,0,0,0],
  ['kg',1,0,1,0,0,0,0],['chilo',1,0,1,0,0,0,0],['chili',1,0,1,0,0,0,0],['chilogrammo',1,0,1,0,0,0,0],['chilogrammi',1,0,1,0,0,0,0],['kilogram',1,0,1,0,0,0,0],['kilograms',1,0,1,0,0,0,0],
  ['q',100,0,1,0,0,0,0],['quintale',100,0,1,0,0,0,0],['quintali',100,0,1,0,0,0,0],
  ['t',1000,0,1,0,0,0,0],['tonnellata',1000,0,1,0,0,0,0],['tonnellate',1000,0,1,0,0,0,0],['ton',1000,0,1,0,0,0,0],['tonne',1000,0,1,0,0,0,0],
  ['oncia',0.0283495,0,1,0,0,0,0],['once',0.0283495,0,1,0,0,0,0],['oz',0.0283495,0,1,0,0,0,0],['ounce',0.0283495,0,1,0,0,0,0],['ounces',0.0283495,0,1,0,0,0,0],
  ['libbra',0.453592,0,1,0,0,0,0],['libbre',0.453592,0,1,0,0,0,0],['lb',0.453592,0,1,0,0,0,0],['lbs',0.453592,0,1,0,0,0,0],['pound',0.453592,0,1,0,0,0,0],['pounds',0.453592,0,1,0,0,0,0],
  ['ms',1e-3,0,0,1,0,0,0],['millisecondo',1e-3,0,0,1,0,0,0],['millisecondi',1e-3,0,0,1,0,0,0],
  ['s',1,0,0,1,0,0,0],['sec',1,0,0,1,0,0,0],['secondo',1,0,0,1,0,0,0],['secondi',1,0,0,1,0,0,0],['second',1,0,0,1,0,0,0],['seconds',1,0,0,1,0,0,0],
  ['min',60,0,0,1,0,0,0],['minuto',60,0,0,1,0,0,0],['minuti',60,0,0,1,0,0,0],['minute',60,0,0,1,0,0,0],['minutes',60,0,0,1,0,0,0],
  ['h',3600,0,0,1,0,0,0],['ora',3600,0,0,1,0,0,0],['ore',3600,0,0,1,0,0,0],['hour',3600,0,0,1,0,0,0],['hours',3600,0,0,1,0,0,0],
  ['giorno',86400,0,0,1,0,0,0],['giorni',86400,0,0,1,0,0,0],['day',86400,0,0,1,0,0,0],['days',86400,0,0,1,0,0,0],
  ['settimana',604800,0,0,1,0,0,0],['settimane',604800,0,0,1,0,0,0],['week',604800,0,0,1,0,0,0],['weeks',604800,0,0,1,0,0,0],
  ['mese',2592000,0,0,1,0,0,0],['mesi',2592000,0,0,1,0,0,0],['month',2592000,0,0,1,0,0,0],['months',2592000,0,0,1,0,0,0],
  ['anno',31536000,0,0,1,0,0,0],['anni',31536000,0,0,1,0,0,0],['year',31536000,0,0,1,0,0,0],['years',31536000,0,0,1,0,0,0],
  ['ml',1e-6,3,0,0,0,0,0],['millilitro',1e-6,3,0,0,0,0,0],['millilitri',1e-6,3,0,0,0,0,0],
  ['cl',1e-5,3,0,0,0,0,0],['dl',1e-4,3,0,0,0,0,0],
  ['l',1e-3,3,0,0,0,0,0],['litro',1e-3,3,0,0,0,0,0],['litri',1e-3,3,0,0,0,0,0],['liter',1e-3,3,0,0,0,0,0],['litre',1e-3,3,0,0,0,0,0],['liters',1e-3,3,0,0,0,0,0],['litres',1e-3,3,0,0,0,0,0],
  ['hl',0.1,3,0,0,0,0,0],['m3',1,3,0,0,0,0,0],['dm3',1e-3,3,0,0,0,0,0],['cm3',1e-6,3,0,0,0,0,0],
  ['gallone',0.00378541,3,0,0,0,0,0],['galloni',0.00378541,3,0,0,0,0,0],['gallon',0.00378541,3,0,0,0,0,0],['gallons',0.00378541,3,0,0,0,0,0],['gal',0.00378541,3,0,0,0,0,0],
  ['pinta',0.000473176,3,0,0,0,0,0],['pint',0.000473176,3,0,0,0,0,0],
  ['tazza',0.000236588,3,0,0,0,0,0],['tazze',0.000236588,3,0,0,0,0,0],['cup',0.000236588,3,0,0,0,0,0],
  ['cucchiaio',1.4787e-5,3,0,0,0,0,0],['tablespoon',1.4787e-5,3,0,0,0,0,0],
  ['mm2',1e-6,2,0,0,0,0,0],['cm2',1e-4,2,0,0,0,0,0],['dm2',1e-2,2,0,0,0,0,0],['m2',1,2,0,0,0,0,0],['mq',1,2,0,0,0,0,0],['km2',1e6,2,0,0,0,0,0],
  ['ara',100,2,0,0,0,0,0],['ettaro',1e4,2,0,0,0,0,0],['ettari',1e4,2,0,0,0,0,0],['ha',1e4,2,0,0,0,0,0],
  ['acro',4046.86,2,0,0,0,0,0],['acri',4046.86,2,0,0,0,0,0],['acre',4046.86,2,0,0,0,0,0],['acres',4046.86,2,0,0,0,0,0],
  ['m/s',1,1,0,-1,0,0,0],['km/h',1/3.6,1,0,-1,0,0,0],['kmh',1/3.6,1,0,-1,0,0,0],['kph',1/3.6,1,0,-1,0,0,0],['mph',0.44704,1,0,-1,0,0,0],
  ['nodo',0.514444,1,0,-1,0,0,0],['nodi',0.514444,1,0,-1,0,0,0],['knot',0.514444,1,0,-1,0,0,0],['knots',0.514444,1,0,-1,0,0,0],
  ['n',1,1,1,-2,0,0,0],['newton',1,1,1,-2,0,0,0],['kn',1000,1,1,-2,0,0,0],['dyn',1e-5,1,1,-2,0,0,0],['kgf',9.80665,1,1,-2,0,0,0],
  ['j',1,2,1,-2,0,0,0],['joule',1,2,1,-2,0,0,0],['kj',1000,2,1,-2,0,0,0],['mj',1e6,2,1,-2,0,0,0],
  ['cal',4.184,2,1,-2,0,0,0],['caloria',4.184,2,1,-2,0,0,0],['calorie',4.184,2,1,-2,0,0,0],['kcal',4184,2,1,-2,0,0,0],
  ['wh',3600,2,1,-2,0,0,0],['kwh',3.6e6,2,1,-2,0,0,0],['btu',1055.06,2,1,-2,0,0,0],
  ['w',1,2,1,-3,0,0,0],['watt',1,2,1,-3,0,0,0],['kw',1000,2,1,-3,0,0,0],['mw',1e6,2,1,-3,0,0,0],['gw',1e9,2,1,-3,0,0,0],
  ['cv',735.49875,2,1,-3,0,0,0],['hp',745.699,2,1,-3,0,0,0],
  ['pa',1,-1,1,-2,0,0,0],['pascal',1,-1,1,-2,0,0,0],['hpa',100,-1,1,-2,0,0,0],['kpa',1000,-1,1,-2,0,0,0],['mpa',1e6,-1,1,-2,0,0,0],
  ['bar',1e5,-1,1,-2,0,0,0],['mbar',100,-1,1,-2,0,0,0],['atm',101325,-1,1,-2,0,0,0],['psi',6894.76,-1,1,-2,0,0,0],['mmhg',133.322,-1,1,-2,0,0,0],['torr',133.322,-1,1,-2,0,0,0],
  ['hz',1,0,0,-1,0,0,0],['hertz',1,0,0,-1,0,0,0],['khz',1000,0,0,-1,0,0,0],['mhz',1e6,0,0,-1,0,0,0],['ghz',1e9,0,0,-1,0,0,0],
  ['bit',0.125,0,0,0,0,1,0],['bits',0.125,0,0,0,0,1,0],['byte',1,0,0,0,0,1,0],['bytes',1,0,0,0,0,1,0],
  ['kb',1024,0,0,0,0,1,0],['kib',1024,0,0,0,0,1,0],['mb',1048576,0,0,0,0,1,0],['mib',1048576,0,0,0,0,1,0],
  ['gb',1073741824,0,0,0,0,1,0],['gib',1073741824,0,0,0,0,1,0],['tb',1.099511627776e12,0,0,0,0,1,0],['pb',1.125899906842624e15,0,0,0,0,1,0],
  ['grado',1,0,0,0,0,0,1],['gradi',1,0,0,0,0,0,1],['degree',1,0,0,0,0,0,1],['degrees',1,0,0,0,0,0,1],['deg',1,0,0,0,0,0,1],
  ['rad',57.29577951308232,0,0,0,0,0,1],['radiante',57.29577951308232,0,0,0,0,0,1],['radianti',57.29577951308232,0,0,0,0,0,1],['radian',57.29577951308232,0,0,0,0,0,1],
  ['giro',360,0,0,0,0,0,1],['giri',360,0,0,0,0,0,1],['turn',360,0,0,0,0,0,1],
  ['ampere',1,0,0,0,0,0,0,1],['amp',1,0,0,0,0,0,0,1],['amps',1,0,0,0,0,0,0,1],
  ['volt',1,2,1,-3,0,0,0,-1],['volts',1,2,1,-3,0,0,0,-1],
  ['ohm',1,2,1,-3,0,0,0,-2],['ohms',1,2,1,-3,0,0,0,-2],['kohm',1000,2,1,-3,0,0,0,-2],
  ['coulomb',1,0,0,1,0,0,0,1],
];
const U_TAB = new Map(U_TAB_RAW.map(r => [r[0], { f: r[1], d: [r[2],r[3],r[4],r[5],r[6],r[7],r[8]||0] }]));
const U_LEARN = [];                                       // learned units (per-process; mirrors g_ulearn)
function uTempCode(w){ if(['c','celsius','centigradi','centigrado'].includes(w))return 1; if(['f','fahrenheit'].includes(w))return 2; if(['k','kelvin'].includes(w))return 3; return 0; }
function uEq(a,b){ for(let i=0;i<7;i++) if((a[i]||0)!==(b[i]||0)) return false; return true; }
function uBase(w){ return U_TAB.get(w) || null; }
function uNameMatch(a,b){                                   // exact, or a singular/plural form (spanna~spanne, pertica~pertiche)
  if(a===b) return true;
  const mn=Math.min(a.length,b.length); if(mn<4) return false;
  let cp=0; while(cp<mn && a[cp]===b[cp]) cp++;             // share all but (at most) the last letter of the shorter
  return cp>=mn-1;
}
function uResolve(w){
  for(const e of U_LEARN){ if(uNameMatch(e.name,w)) return { f:e.f, d:e.d }; }
  const b=uBase(w); if(b) return b;
  const PFX=[['chilo',1e3],['kilo',1e3],['mega',1e6],['giga',1e9],['tera',1e12],['peta',1e15],
    ['milli',1e-3],['micro',1e-6],['nano',1e-9],['centi',1e-2],['deci',1e-1],['deca',1e1],['etto',1e2],['hecto',1e2]];
  for(const [p,e] of PFX){ if(w.startsWith(p) && w.length>p.length){ const bb=uBase(w.slice(p.length)); if(bb) return { f:bb.f*e, d:bb.d }; } }
  return null;
}
function uDimName(d, en){
  const N=[[[1,0,0,0,0,0],'lunghezza','length'],[[0,1,0,0,0,0],'massa','mass'],[[0,0,1,0,0,0],'tempo','time'],
    [[0,0,0,1,0,0],'temperatura','temperature'],[[0,0,0,0,1,0],'dati','data'],[[0,0,0,0,0,1],'angolo','angle'],
    [[2,0,0,0,0,0],'area','area'],[[3,0,0,0,0,0],'volume','volume'],[[1,0,-1,0,0,0],'velocita','speed'],
    [[1,1,-2,0,0,0],'forza','force'],[[2,1,-2,0,0,0],'energia','energy'],[[2,1,-3,0,0,0],'potenza','power'],
    [[-1,1,-2,0,0,0],'pressione','pressure'],[[0,0,-1,0,0,0],'frequenza','frequency'],
    [[0,0,0,0,0,0,1],'corrente','current'],[[2,1,-3,0,0,0,-1],'tensione','voltage'],
    [[2,1,-3,0,0,0,-2],'resistenza','resistance'],[[0,0,1,0,0,0,1],'carica','charge']];
  for(const [dd,it,e] of N) if(uEq(d,dd)) return en?e:it;
  return en?'quantity':'grandezza';
}
function uSkipword(w){ return ['di','of','con','with','del','dello','della','come','as','un','uno','una','the','a','e','ed','and',
  'in','to','è','sono','vale','valgono','uguale','equivale','equivalgono','equals','corrisponde','corrispondono','definisci',
  'define','impara','learn','memorizza','converti','convert','trasforma','quanti','quante','quanto','fa','ce','ci','ad'].includes(w); }
const U_CONN = ['di','of','con','with','del','dello','della'];
function jUnits(items, en){
  const fmt = fmtNum;
  let cue=false, learnCue=false, equivCue=false, hasEq=false, rangeWord=false, firstNum=-1;
  for(let i=0;i<items.length;i++){ const it=items[i];
    if(it.num){ if(firstNum<0) firstNum=i; continue; } const w=it.w;
    if(['in','to','into','converti','convert','trasforma','trasformare','quanti','quante','quanto','corrisponde','corrispondono','vale','valgono','sono','many'].includes(w)) cue=true;   // not bare "a" (idioms); "many" -> EN "how many"
    if(['definisci','define','impara','learn','memorizza'].includes(w)) learnCue=true;
    if(['equivale','equivalgono','equals','corrisponde','corrispondono'].includes(w)) equivCue=true;
    if(w==='=') hasEq=true;
    if(['da','dal','dalla','dalle','dallo','tra','fra','between'].includes(w)) rangeWord=true;
  }
  // power of ten: "10 alla 9"
  if(!rangeWord) for(let i=1;i+1<items.length;i++){
    if(!items[i-1].num || items[i].num || !items[i+1].num) continue;
    if(items[i].w==='alla'){ const base=items[i-1].val, ex=items[i+1].val;
      return { intent:'calc', conf:96, reply:`${fmt(base)}^${fmt(ex)} = ${fmt(Math.pow(base,ex))}.` }; }
  }
  // collect units; "gradi" is angle, but temperature next to celsius/fahrenheit/kelvin
  let hasTempUnit=false;
  for(const it of items) if(!it.num && ['celsius','centigradi','centigrado','fahrenheit','kelvin'].includes(it.w)) hasTempUnit=true;
  const U=[];
  for(let i=0;i<items.length && U.length<8;i++){ const it=items[i]; if(it.num) continue; const w=it.w;
    const isDeg = ['gradi','grado','degree','degrees','deg'].includes(w);
    if(isDeg && i+1<items.length && !items[i+1].num && uTempCode(items[i+1].w)) continue;
    if(isDeg && hasTempUnit){ U.push({pos:i,w,f:0,d:[0,0,0,1,0,0],temp:1}); continue; }
    const tc=uTempCode(w);
    if(tc){ U.push({pos:i,w,f:0,d:[0,0,0,1,0,0],temp:tc}); continue; }
    const rr=uResolve(w); if(rr){ U.push({pos:i,w,f:rr.f,d:rr.d,temp:0}); }
  }
  // DEFINITION
  if(learnCue||equivCue||hasEq){
    let rNum=-1,rUnit=-1,rf=0,rd=null;
    for(let i=0;i<items.length;i++){ if(!items[i].num) continue;
      let ui=-1; if(i+1<items.length && !items[i+1].num) ui=i+1;
      else if(i+2<items.length && !items[i+1].num && !items[i+2].num && U_CONN.includes(items[i+1].w)) ui=i+2;
      if(ui>=0){ const tc=uTempCode(items[ui].w); if(!tc){ const rr=uResolve(items[ui].w); if(rr){ rNum=i; rUnit=ui; rf=rr.f; rd=rr.d; } } }
    }
    let nameIdx=-1;
    for(let i=0;i<items.length;i++){ if(items[i].num||i===rUnit) continue; const w=items[i].w;
      if(uSkipword(w)||uTempCode(w)||w==='='||(!learnCue && uBase(w))) continue;   // learned units (not built-in) can be re-defined
      if(w.length<2) continue; nameIdx=i; break; }
    if(nameIdx>=0 && rUnit>=0){
      let lhsN = (nameIdx>=1 && items[nameIdx-1].num) ? items[nameIdx-1].val : 1; if(lhsN===0) lhsN=1;
      const perName = items[rNum].val*rf/lhsN, nm=items[nameIdx].w;
      const ix=U_LEARN.findIndex(e=>e.name===nm); const ent={name:nm,f:perName,d:rd}; if(ix>=0) U_LEARN[ix]=ent; else U_LEARN.push(ent);
      return { intent:'convert', conf:96, reply: (en?'Learned: 1 ':'Imparato: 1 ')+`${nm} = ${fmt(items[rNum].val/lhsN)} ${items[rUnit].w} (${uDimName(rd,en)}).` };
    }
  }
  // CONVERSION
  if(firstNum>=0 && U.length>=2 && cue){
    let si=-1; for(let k=0;k<U.length;k++) if(U[k].pos>firstNum){ si=k; break; } if(si<0) si=0;
    let ti=-1; for(let k=0;k<U.length;k++) if(k!==si){ ti=k; break; } if(ti<0) return null;
    const A=U[si], B=U[ti]; let V=items[firstNum].val; const sp=A.pos;   // value = number adjacent to the SOURCE unit
    if(sp>=1 && items[sp-1].num) V=items[sp-1].val;
    else if(sp>=2 && !items[sp-1].num && items[sp-2].num && U_CONN.includes(items[sp-1].w)) V=items[sp-2].val;
    if(A.temp && B.temp){ const toK=(v,c)=>c===1?v+273.15:c===2?(v-32)*5/9+273.15:v, fromK=(k,c)=>c===1?k-273.15:c===2?(k-273.15)*9/5+32:k;
      return { intent:'convert', conf:96, reply:`${fmt(V)} ${A.w} = ${fmt(fromK(toK(V,A.temp),B.temp))} ${B.w}.` }; }
    if(A.temp || B.temp) return { intent:'convert', conf:96, reply:(en?`Can't convert: ${A.w} is ${uDimName(A.d,en)}, ${B.w} is ${uDimName(B.d,en)} (different dimensions).`:`Non posso convertire: ${A.w} è ${uDimName(A.d,en)}, ${B.w} è ${uDimName(B.d,en)} (dimensioni diverse).`) };
    if(uEq(A.d,B.d)) return { intent:'convert', conf:96, reply:`${fmt(V)} ${A.w} = ${fmt(V*A.f/B.f)} ${B.w}.` };
    const MASS=[0,1,0,0,0,0], VOL=[3,0,0,0,0,0]; let extra='';
    if(uEq(A.d,MASS)&&uEq(B.d,VOL)){ const kg=V*A.f; extra=(en?` Water: ${fmt(kg)} kg ≈ ${fmt(kg)} litres.`:` Acqua: ${fmt(kg)} kg ≈ ${fmt(kg)} litri.`); }
    else if(uEq(A.d,VOL)&&uEq(B.d,MASS)){ const lt=V*A.f/1e-3; extra=(en?` Water: ${fmt(lt)} litres ≈ ${fmt(lt)} kg.`:` Acqua: ${fmt(lt)} litri ≈ ${fmt(lt)} kg.`); }
    return { intent:'convert', conf:96, reply:(en?`Can't convert: ${A.w} is ${uDimName(A.d,en)}, ${B.w} is ${uDimName(B.d,en)} (different dimensions).${extra}`:`Non posso convertire: ${A.w} è ${uDimName(A.d,en)}, ${B.w} è ${uDimName(B.d,en)} (dimensioni diverse).${extra}`) };
  }
  return null;
}

// Deterministic solver — mirrors anima_solve in nucleo_anima.c (units/percent/powers/Ohm).
// Returns {intent, reply, conf} or null. Exact arithmetic, no model.
function trySolve(raw, lang) {
  const en = lang === 'en';
  const norm = String(raw).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/²/g, '2').replace(/³/g, '3')                  // superscripts -> digits ("m/s²"->"m/s2")
    .replace(/(\d)[.,](\d)/g, '$1\x01$2')                   // decimal separator only BETWEEN digits (sentinel)
    .replace(/[^a-z0-9%/=\x01]/g, ' ')                      // mirror a_norm_solve: keep '/' (compound units) and '=' (unit defs), drop stray . , -> space
    .replace(/\x01/g, '.');                                 // restore decimal point
  // Tokenize like a_items in nucleo_anima.c: split on space/%, parse a LEADING number, keep the
  // rest of a token whole (so "log2"/"cm3" stay one word — the firmware does NOT split letter+digit).
  const items = [];
  for (let i = 0; i < norm.length;) {
    const ch = norm[i];
    if (ch === ' ') { i++; continue; }
    if (ch === '%') { items.push({ num: false, w: 'pct' }); i++; continue; }
    if ((ch >= '0' && ch <= '9') || (ch === '.' && norm[i + 1] >= '0' && norm[i + 1] <= '9')) {
      const mm = /^[0-9]*\.?[0-9]+(?:e[+-]?[0-9]+)?/i.exec(norm.slice(i));
      if (mm) { items.push({ num: true, val: parseFloat(mm[0]) }); i += mm[0].length; continue; }
      i++; continue;
    }
    let w = ''; while (i < norm.length && norm[i] !== ' ' && norm[i] !== '%') w += norm[i++];
    items.push({ num: false, w });
  }
  if (!items.length) return null;
  { const g = jGeometry(items, en); if (g) return g; }     // geometry (shape-gated) — before base
  { const ph = jPhysics(items, en); if (ph) return ph; }   // physics (kw-gated) — before base/ohm
  { const vc = jVector(items, en); if (vc) return vc; }    // vectors — before binop/absmod
  { const b = solveBase(norm); if (b) return b; }          // radix conversion (before everything)
  { const rm = solveRoman(norm, en); if (rm) return rm; }  // roman numerals
  const fmt = fmtNum;
  const unit = (w) => {
    const T = { mm:[1,.001],cm:[1,.01],dm:[1,.1],m:[1,1],metro:[1,1],metri:[1,1],meter:[1,1],meters:[1,1],
      km:[1,1000],chilometro:[1,1000],chilometri:[1,1000],kilometer:[1,1000],
      inch:[1,.0254],pollice:[1,.0254],pollici:[1,.0254],ft:[1,.3048],feet:[1,.3048],piede:[1,.3048],piedi:[1,.3048],
      mile:[1,1609.34],miles:[1,1609.34],miglio:[1,1609.34],miglia:[1,1609.34],
      mg:[2,.001],g:[2,1],grammo:[2,1],grammi:[2,1],gram:[2,1],grams:[2,1],kg:[2,1000],chilo:[2,1000],chili:[2,1000],kilogrammo:[2,1000],
      tonnellata:[2,1e6],ton:[2,1e6],lb:[2,453.592],libbra:[2,453.592],pound:[2,453.592],oz:[2,28.3495],oncia:[2,28.3495],
      bit:[3,.125],byte:[3,1],kb:[3,1024],kilobyte:[3,1024],mb:[3,1048576],megabyte:[3,1048576],gb:[3,1073741824],gigabyte:[3,1073741824],tb:[3,1099511627776],
      sec:[4,1],secondo:[4,1],secondi:[4,1],second:[4,1],seconds:[4,1],min:[4,60],minuto:[4,60],minuti:[4,60],minute:[4,60],minutes:[4,60],
      ora:[4,3600],ore:[4,3600],hour:[4,3600],hours:[4,3600],giorno:[4,86400],giorni:[4,86400],day:[4,86400],days:[4,86400] };
    if (T[w]) return { dim: T[w][0], f: T[w][1], tc: 0 };
    if (w==='c'||w==='celsius'||w==='centigradi') return { dim:5, f:0, tc:1 };
    if (w==='f'||w==='fahrenheit') return { dim:5, f:0, tc:2 };
    if (w==='k'||w==='kelvin') return { dim:5, f:0, tc:3 };
    return null;
  };
  const toK = (v,c)=> c===1? v+273.15 : c===2? (v-32)*5/9+273.15 : v;
  const fromK = (k,c)=> c===1? k-273.15 : c===2? (k-273.15)*9/5+32 : k;

  // percent: "X% di Y"
  { let haspct=false, conn=false, rate=-1; const nums=[];
    for (let i=0;i<items.length;i++){ const it=items[i];
      if (it.num) nums.push(it.val);
      else { if (it.w==='pct'||it.w.startsWith('percent')||it.w==='cento'){ haspct=true; if(i>0&&items[i-1].num) rate=items[i-1].val; }
             if (['di','of','del','dello','della'].includes(it.w)) conn=true; } }
    if (haspct&&conn&&nums.length>=2){ if(rate<0)rate=nums[0]; const base=(rate===nums[0])?nums[1]:nums[0]; const res=rate/100*base;
      return { intent:'percent', conf:95, reply: en?`${fmt(rate)}% of ${fmt(base)} = ${fmt(res)}.`:`Il ${fmt(rate)}% di ${fmt(base)} = ${fmt(res)}.` }; } }

  // Ohm's law (gated on an electrical keyword)
  if (/volt|ampere|ohm|watt|tension|corrent|resisten|potenz|current|voltage|resistance|power/.test(norm)) {
    let V=0,I=0,R=0,P=0,hV=false,hI=false,hR=false,hP=false;
    for (let i=0;i+1<items.length;i++){ if(!items[i].num||items[i+1].num) continue; const val=items[i].val, u=items[i+1].w;
      if (u==='v'||u.startsWith('volt')){ if(!hV){V=val;hV=true;} }
      else if (u==='ma'||u==='milliampere'){ if(!hI){I=val*0.001;hI=true;} }
      else if (u==='a'||u.startsWith('amper')||u==='amp'){ if(!hI){I=val;hI=true;} }
      else if (u==='kohm'||u==='kiloohm'){ if(!hR){R=val*1000;hR=true;} }
      else if (u.startsWith('ohm')){ if(!hR){R=val;hR=true;} }
      else if (u==='w'||u.startsWith('watt')){ if(!hP){P=val;hP=true;} } }
    if (hV+hI+hR+hP>=2){
      if (!hV){ if(hI&&hR)V=I*R; else if(hP&&hI&&I!==0)V=P/I; else if(hP&&hR)V=Math.sqrt(P*R); }
      if (!hI){ if(hV&&hR&&R!==0)I=V/R; else if(hP&&hV&&V!==0)I=P/V; else if(hP&&hR&&R!==0)I=Math.sqrt(P/R); }
      if (!hR){ if(hV&&I!==0)R=V/I; else if(hP&&I!==0)R=P/(I*I); else if(hP)R=(V*V)/P; }
      if (!hP) P=V*I;
      const ci = Math.abs(I)<1 ? `${fmt(I*1000)} mA` : `${fmt(I)} A`;
      return { intent:'ohm', conf:92, reply:`V=${fmt(V)} V, I=${ci}, R=${fmt(R)} ohm, P=${fmt(P)} W.` }; } }

  { const u2 = jUnits(items, en); if (u2) return u2; }     // dimensional-analysis converter + learned units (supersedes the legacy block)

  // unit conversion (legacy fallback): "<num> <unit> in <unit2>" — cue-gated (mirror a_solve_convert)
  { let num=0, have=false, cue=false; const dim=[0,0], tc=[0,0], f=[0,0], uw=[null,null]; let u=0;
    for (const it of items){ if(it.num){ if(!have){num=it.val;have=true;} continue; }
      if(['in','to','converti','convert','trasforma','quanti','quante','corrisponde'].includes(it.w)) cue=true;
      const r=unit(it.w); if(r&&u<2){ dim[u]=r.dim;f[u]=r.f;tc[u]=r.tc;uw[u]=it.w;u++; } }
    if (have&&u>=2&&dim[0]===dim[1]&&cue){ const res = dim[0]===5 ? fromK(toK(num,tc[0]),tc[1]) : num*f[0]/f[1];
      return { intent:'convert', conf:95, reply:`${fmt(num)} ${uw[0]} = ${fmt(res)} ${uw[1]}.` }; } }

  { const fr = solveFuncs(items, en); if (fr) return fr; }  // log/ln/cbrt/round/trig — before powroot

  // powers / roots
  { // idiom "<num> alla <ordinale>" -> num^ordinal (ordinal, not a range number — mirrors a_ordinal_power)
    const ORD = { seconda:2, terza:3, quarta:4, quinta:5, sesta:6, settima:7, ottava:8, nona:9, decima:10 };
    for (let i = 0; i + 2 < items.length; i++) {
      if (!items[i].num || items[i+1].num || items[i+2].num) continue;
      if (!['alla','elevato','elevata'].includes(items[i+1].w)) continue;
      const ord = ORD[items[i+2].w]; if (!ord) continue;
      return { intent: 'calc', conf: 95, reply: en ? `${fmt(items[i].val)}^${ord} = ${fmt(Math.pow(items[i].val, ord))}.` : `${fmt(items[i].val)} elevato ${ord} = ${fmt(Math.pow(items[i].val, ord))}.` };
    } }
  { let root=false, poww=false, cubeGarble=false; const nums=[];
    for (const it of items){ if(it.num)nums.push(it.val); else { if(['radice','sqrt','root'].includes(it.w))root=true; if(['elevato','elevata','potenza','power'].includes(it.w))poww=true; if(damlev(it.w,'cubica',3)<=3)cubeGarble=true; } }
    // a cube-ish modifier too garbled for solveFuncs to claim -> abstain, never guess the sqrt root
    if (root&&!cubeGarble&&nums.length>=1&&nums[nums.length-1]>=0){ const x=nums[nums.length-1]; return { intent:'calc', conf:95, reply: en?`sqrt(${fmt(x)}) = ${fmt(Math.sqrt(x))}.`:`radice di ${fmt(x)} = ${fmt(Math.sqrt(x))}.` }; }
    if (poww&&nums.length>=2){ return { intent:'calc', conf:95, reply: en?`${fmt(nums[0])}^${fmt(nums[1])} = ${fmt(Math.pow(nums[0],nums[1]))}.`:`${fmt(nums[0])} elevato ${fmt(nums[1])} = ${fmt(Math.pow(nums[0],nums[1]))}.` }; } }

  { const s = solveScale(items, en); if (s) return s; }    // double/half/square/cube/...
  { const bo = solveBinop(items, en); if (bo) return bo; }  // sum/difference/product/take-from/...
  { const np = solveNumprop(items, en); if (np) return np; } // prime? / fibonacci

  // abs / modulo (mirrors a_solve_absmod in nucleo_anima.c)
  { let isabs=false, ismod=false; const nums=[];
    for (const it of items){ if(it.num)nums.push(it.val); else { if(['assoluto','abs'].includes(it.w))isabs=true; if(['modulo','resto'].includes(it.w))ismod=true; } }
    if (isabs&&nums.length>=1){ const x=nums[nums.length-1]; return { intent:'calc', conf:95, reply: en?`abs(${fmt(x)}) = ${fmt(Math.abs(x))}.`:`valore assoluto di ${fmt(x)} = ${fmt(Math.abs(x))}.` }; }
    if (ismod&&nums.length>=2&&nums[1]!==0){ return { intent:'calc', conf:95, reply: en?`${fmt(nums[0])} mod ${fmt(nums[1])} = ${fmt(nums[0]%nums[1])}.`:`${fmt(nums[0])} modulo ${fmt(nums[1])} = ${fmt(nums[0]%nums[1])}.` }; } }

  // factorial (mirrors a_solve_factorial): N! for 0 <= N <= 20
  { let fact=false; const nums=[];
    for (const it of items) { if (it.num) nums.push(it.val); else if (['fattoriale','factorial'].includes(it.w)) fact = true; }
    if (fact && nums.length >= 1) { const x = nums[nums.length-1];
      if (x >= 0 && x <= 20 && Number.isInteger(x)) { let f = 1n; for (let k = 2; k <= x; k++) f *= BigInt(k);
        return { intent: 'calc', conf: 95, reply: `${fmt(x)}! = ${f.toString()}.` }; } } }

  // gcd / lcm (mirrors a_solve_gcdlcm): integers only
  { let g=false, l=false; const nums=[];
    for (const it of items) { if (it.num) nums.push(it.val); else { if (['mcd','gcd'].includes(it.w)) g = true; if (['mcm','lcm'].includes(it.w)) l = true; } }
    if ((g || l) && nums.length >= 2 && Number.isInteger(nums[0]) && Number.isInteger(nums[1])) {
      const gcd = (a, b) => { a = Math.abs(a); b = Math.abs(b); while (b) { [a, b] = [b, a % b]; } return a; };
      const A = nums[0], B = nums[1], gg = gcd(A, B); let res = l ? (gg ? Math.abs(A / gg * B) : 0) : gg;
      return { intent: 'calc', conf: 95, reply: l ? (en ? `lcm(${fmt(A)}, ${fmt(B)}) = ${fmt(res)}.` : `mcm(${fmt(A)}, ${fmt(B)}) = ${fmt(res)}.`)
                                                  : (en ? `gcd(${fmt(A)}, ${fmt(B)}) = ${fmt(res)}.` : `mcd(${fmt(A)}, ${fmt(B)}) = ${fmt(res)}.`) }; } }

  // average / mean (mirrors a_solve_average): arithmetic mean of all numbers (>= 2)
  { let avg=false, sum=0, nn=0;
    for (const it of items) { if (it.num) { sum += it.val; nn++; } else if (['media','average','mean','medio'].includes(it.w)) avg = true; }
    if (avg && nn >= 2) return { intent: 'calc', conf: 92, reply: en ? `The average is ${fmtRound(sum/nn)}.` : `La media e ${fmtRound(sum/nn)}.` }; }   // mirror a_fmt_round

  return null;
}

// Session state (decision-relevant) for the agentic controller — mirrors s_session in
// nucleo_anima.c: utility memory + pending tool slot (FSM AWAITING_SLOT) + clarify options.
let animaMem = { last_app: '', last_file: '', last_kind: '', last_topic: '',
                 pending_tool: '', pending_slot: '', pending_arg: '', clarify_opt: ['', ''], lastActionInput: '' };
function resetAnimaSession() {
  animaMem = { last_app: '', last_file: '', last_kind: '', last_topic: '',
               pending_tool: '', pending_slot: '', pending_arg: '', clarify_opt: ['', ''], lastActionInput: '' };
}
// "ripeti" / "di nuovo" / "again" / "repeat" — short inputs only (mirrors a_is_repeat).
function isRepeat(s) {
  const t = String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').split(/[^a-z0-9]+/).filter(Boolean);
  if (t.length === 0 || t.length > 3) return false;
  if (t.some(x => ['ripeti', 'rifai', 'again', 'repeat'].includes(x))) return true;
  return t.includes('di') && t.includes('nuovo');
}

// Bilingual typo tolerance (mirrors SPELL_VOCAB / a_damlev / a_spell_word / a_spellfix in
// nucleo_anima.c): correct each query word toward ANIMA's command vocabulary by bounded
// Damerau-Levenshtein. Used correct-on-miss only (so working queries are never altered).
const SPELL_VOCAB = [
  'apri','aprire','apre','avvia','lancia','esegui','mostra','mostrami','vai','voglio','portami',
  'vedere','vedi','guarda','guardare','vorrei','fammi',
  'open','show','launch','run','start','avviare','aprilo','aprila','riapri','ripeti','rifai','ancora','again','repeat',
  'crea','creare','crei','nuovo','nuova','create','make','file','files','documento','documenti','document',
  'nota','note','appunti','blocco','testo','foglio','cartella',
  'foto','fotografie','immagini','immagine','galleria','photos','photo','pictures','gallery',
  'musica','canzoni','brani','audio','music','songs','lettore','player',
  'video','filmati','film','movies','movie','calcolatrice','calcoli','calculator','math',
  'orologio','sveglia','timer','clock','cronometro','impostazioni','settings','opzioni','options',
  'monitor','stato','risorse','status','telecomando','infrarossi','remote','registratore','voce','recorder','record',
  'cestino','trash','aggiornamenti','aggiorna','updates','update','giochi','gioco','emulatore','games','game',
  'sciame','swarm','automazioni','automazione','automation','calendario','agenda','calendar',
  'ora','ore','orario','adesso','time','spazio','disco','scheda','archiviazione','storage','space','batteria','carica','battery',
  'per','piu','meno','diviso','fratto','moltiplicato','times','plus','minus','divided',
  'radice','sqrt','root','elevato','potenza','percento','percent','metri','centimetri','grammi','minuti',
  'volt','ampere','ohm','corrente','tensione','resistenza','watt','celsius','fahrenheit','legge',
  'chi','sei','cosa','come','quanto','quale','aiuto','comandi','help','puoi','presentati',
  'dimmi','dammi','raccontami','spiegami','fammi','approfondisci','dettagli','esempio','continua','tell','explain','give',
  'primo','secondo','prima','seconda','first','second','quello','questo','quella','questa',
  'connesso','connessione','collegato','collegata','online','connected','wifi','rete','internet','network',
  'memoria','libera','libero','liberi','disponibile','occupato','versione','version','uptime','acceso','capacita',
];
function damlev(a, b, max) {
  const la = a.length, lb = b.length;
  if (Math.abs(la - lb) > max) return max + 1;
  let prev = [], prev2 = [], cur = [];
  for (let j = 0; j <= lb; j++) { prev[j] = j; prev2[j] = 0; }
  for (let i = 1; i <= la; i++) {
    cur[0] = i; let best = i;
    for (let j = 1; j <= lb; j++) {
      const cost = a[i-1] === b[j-1] ? 0 : 1;
      let v = prev[j] + 1;
      if (cur[j-1] + 1 < v) v = cur[j-1] + 1;
      if (prev[j-1] + cost < v) v = prev[j-1] + cost;
      if (i > 1 && j > 1 && a[i-1] === b[j-2] && a[i-2] === b[j-1] && prev2[j-2] + 1 < v) v = prev2[j-2] + 1;
      cur[j] = v; if (v < best) best = v;
    }
    if (best > max) return max + 1;
    prev2 = prev.slice(); prev = cur.slice();
  }
  return prev[lb];
}
function transpose1(a, b) {   // exactly one adjacent swap
  if (a.length !== b.length || a.length < 2) return false;
  let i = 0; while (i < a.length && a[i] === b[i]) i++;
  if (i >= a.length - 1) return false;
  if (a[i] === b[i+1] && a[i+1] === b[i]) { let k = i + 2; while (k < a.length && a[k] === b[k]) k++; return k === a.length; }
  return false;
}
function spellWord(w) {
  if (w.length < 3) return null;
  if (w.length < 6) {                    // short: TRANSPOSITION only (so "che" !-> "chi")
    let cand = null, n = 0;
    for (const v of SPELL_VOCAB) { if (v === w) return null; if (transpose1(w, v)) { cand = v; n++; } }
    return n === 1 ? cand : null;
  }
  let best = null, bestd = 3, ties = 0;   // long (>=6): Damerau-Levenshtein <= 2
  for (const v of SPELL_VOCAB) { if (v === w) return null; const d = damlev(w, v, 2); if (d < bestd) { bestd = d; best = v; ties = 0; } else if (d === bestd) ties++; }
  return (best && bestd <= 2 && ties === 0) ? best : null;
}
function spellfix(raw) {
  let changed = false;
  const out = String(raw).split(/(\s+)/).map(tok => {
    if (/^\s*$/.test(tok) || /[0-9.]/.test(tok)) return tok;     // whitespace / number / filename
    const fix = spellWord(tok.toLowerCase());
    if (fix) { changed = true; return fix; }
    return tok;
  }).join('');
  return changed ? out : raw;
}

// ---- ANIMA online teacher (selective memory) ----------------------------------------------
// When the offline cascade MISSES and a teacher key is present (GROQ_KEY env / teacher.json), ask
// the LLM. It self-classifies: GENERAL knowledge -> distilled into one ODD card (deduped, source
// tagged) the device then recalls OFFLINE forever; SPECIFIC/personal/one-off -> answered but NEVER
// saved (ephemeral, "nulla di più"). With no key / offline, this whole path is skipped -> the
// device can never hallucinate offline; it only ever serves frozen, vetted ODD.
// The key lives ONLY in the environment (never committed). Mirrors the future firmware teacher tier.
const slugify = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
// Clip to <=250 chars on a sentence/word boundary (the ODD reply cap), never mid-word.
const clip250 = (s) => { s = String(s || '').trim(); if (s.length <= 250) return s;
  let c = s.lastIndexOf('. ', 250); if (c < 120) c = s.lastIndexOf(' ', 250); return (c > 0 ? s.slice(0, c) : s.slice(0, 250)).trim(); };

// Minimum question<->answer coherence gate (cheap, no second model call). Strips question words,
// articles/prepositions AND the fact-trigger words (so coherence rests on the real ENTITY/content,
// not on "capitale"/"nato" appearing in both) and requires >=1 shared content token. Catches a
// wrong-entity resolution ("capitale di Georgia" -> the US state) or LLM topic drift before we
// SAVE; it is a sanity floor, not a deep semantic judge.
const COH_STOP = new Set(('chi cosa cos che come quando dove perche perché quale quali qual '
  + 'e è era erano sono ha hanno il lo la i gli le un uno una di del dello della dei degli delle '
  + 'd al alla allo a da in su per con tra fra su nel nella '
  + 'nato nata nati nate morto morta morti morte nascita morte data capitale capitali autore autori '
  + 'scritto scrive scrivere significa significato '
  + 'the what who when where why which is are was were has have a an of in on to for with by '
  + 'born die died death birth capital author wrote write means meaning').split(/\s+/).filter(Boolean));
function cohTokens(s) {
  return new Set(String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
    .split(/[^a-z0-9]+/).filter(w => w.length >= 3 && !COH_STOP.has(w)));
}
function coherent(query, answer) {
  const q = cohTokens(query), a = cohTokens(answer);
  if (q.size === 0) return true;                          // nothing specific to check -> allow
  for (const w of q) if (a.has(w)) return true;           // exact shared content token
  for (const w of q) for (const x of a) if (w.length >= 4 && (x.includes(w) || w.includes(x))) return true;  // inflection/substring
  return false;
}

// ===========================================================================
// SC3 — Self-Calibrated Coherence Cross : the gate that decides whether a fetched ENTITY resolution
// (query -> Wikipedia title/extract) is faithful enough to PERSIST into the ODD. Asymmetric risk: a
// false learn poisons memory for 10 years, a false reject just re-fetches -> tilt conservative.
// Innovation: (1) judge the resolution through an ORTHOGRAPHIC lens (entities/typos: "berluscono"~
// "berlusconi") and a SEMANTIC lens (descriptions); legit resolutions pass >=1 lens, garbage passes
// neither. (2) NO hand threshold — accept iff the candidate is as coherent as the device's OWN learned
// cards (a conformal quantile of its history), shrunk to a conservative prior at cold start. The
// orthographic lens is the on-device hero (deterministic, encoder-free); semantic is a proxy here
// (the firmware uses the real e5 encoder). Concept/teacher learning keeps coherent() — names are
// orthographic, concepts-from-questions are semantic, two different regimes.
const _cohNorm = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9]+/g, ' ').trim();
function _cohTri(s) { const t = ' ' + _cohNorm(s) + ' '; const m = new Map(); for (let i = 0; i < t.length - 2; i++) { const g = t.slice(i, i + 3); m.set(g, (m.get(g) || 0) + 1); } return m; }
function _cohCos(A, B) { let d = 0, na = 0, nb = 0; for (const v of A.values()) na += v * v; for (const v of B.values()) nb += v * v; for (const [k, v] of A) { const w = B.get(k); if (w) d += v * w; } return (na && nb) ? d / Math.sqrt(na * nb) : 0; }
// LENS A (orthographic): char-trigram cosine, lifted by the best token-pair edit-similarity (so a
// one-char typo in a name still scores ~0.9). Encoder-free, deterministic — identical in firmware.
function lensOrtho(query, title) {
  const co = _cohCos(_cohTri(query), _cohTri(title));
  const qt = _cohNorm(query).split(' ').filter(x => x.length >= 3);
  const tt = _cohNorm(title).split(' ').filter(x => x.length >= 3);
  let best = 0;
  for (const a of qt) for (const b of tt) { const L = Math.max(a.length, b.length); if (!L) continue; const dl = damlev(a, b, L); if (dl <= L) best = Math.max(best, 1 - dl / L); }
  return Math.max(co, best);
}
// LENS B (lexical grounding): how many query content-tokens (len>=4) appear EXACTLY in the extract's
// defining (first) sentence — strong, encoder-free evidence the article is about what was asked. This
// rescues DESCRIPTIVE queries the name-shape lens can't see ("presidente americano" -> "...è il
// presidente degli Stati Uniti..."). Conservative: counts only if >=2 hits OR one long word (>=7), so
// a typo ("berluscono") can't self-ground in an unrelated article. Mirrors firmware coh_grounding().
// (The real e5 encoder is unloaded during the online fetch to free heap for TLS, so the semantic arm
// of the Cross is this cheap lexical proxy on both sides — see docs/anima-online.md.)
function lensGround(query, extract) {
  const fs = String(extract || '').split(/(?<=[.!?])\s/)[0] || extract || '';
  const qt = [...new Set(_cohNorm(query).split(' ').filter(w => w.length >= 4))];
  const et = new Set(_cohNorm(fs).split(' ').filter(Boolean));
  let hits = 0, lng = false;
  for (const w of qt) if (et.has(w)) { hits++; if (w.length >= 7) lng = true; }
  return (hits >= 2 || lng) ? hits : 0;
}
// Calibration distribution: kappa(alias, own-title) over the device's accepted cards = "what a
// coherent resolution looks like HERE". Orthographic only (cheap, deterministic, no encoder).
function cohCalibration(cards, en) {
  const ks = [];
  for (const c of cards || []) {
    const title = ((c.id || '').split('.').slice(2).join('.') || '').replace(/-/g, ' ').trim()
      || (c.ask?.[en ? 'en' : 'it']?.[0]) || '';
    if (!title) continue;
    for (const a of (c.ask?.[en ? 'en' : 'it'] || [])) ks.push(lensOrtho(a, title));
  }
  return ks.sort((x, y) => x - y);
}
// The gate. alpha = Neyman-Pearson risk knob (fraction of known-good we tolerate falling below);
// N0/prior = empirical-Bayes cold-start shrinkage; autoHi = near-exact auto-accept; semFloor = the
// semantic OR-rescue for descriptive queries. Returns {accept, co, cs, k, thr}.
function cohGate(query, title, extract, calib, opt = {}) {
  const { alpha = 0.30, prior = 0.42, N0 = 8, autoHi = 0.86 } = opt;
  const co = lensOrtho(query, title);
  const g = lensGround(query, extract);
  if (co >= autoHi) return { accept: true, co, g, thr: autoHi };
  const N = calib.length;
  const idx = Math.min(N - 1, Math.max(0, Math.floor(alpha * (N - 1))));
  const emp = N ? calib[idx] : prior;
  const thr = (N0 * prior + N * emp) / (N0 + N);
  return { accept: co >= thr || g > 0, co, g, thr };          // LENS A (name-shape) OR LENS B (grounding)
}
const learnedFile = (en) => join(SD, 'data', 'anima', 'learned', (en ? 'en' : 'it') + '.jsonl');

async function readLearned(en) {
  try {
    const t = await readFile(learnedFile(en), 'utf8');
    return t.split('\n').filter(Boolean).map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
  } catch { return []; }
}

// Offline recall: does any learned card answer `q`? Match the query slug against each card's ask
// phrasings / topic / id slug (substring, len>=4) — the JS sibling of the firmware card_matches.
async function learnedLookup(lang, q, opts = {}) {
  const en = lang === 'en';
  const list = await readLearned(en);
  if (!list.length) return null;
  const qs = slugify(q);
  if (!qs) return null;
  const hit = (c) => {
    const reply = c.reply?.[en ? 'en' : 'it'] || c.reply?.it || c.reply?.en;
    return reply ? { query: q, tier: 'fact', action: 'answer', intent: 'learned', confidence: 88, state: 'idle', reply, learned: false } : null;
  };
  for (const c of list) {
    // A precise-fact recall must match ONLY a fact card (wd.*) — never a generic bio whose canonical
    // name happens to appear in the question ("quando è nato EINSTEIN" must not return the Einstein bio).
    if (opts.factOnly && !(c.id || '').startsWith('wd.')) continue;
    // Canonical-slug match for entity/concept cards (wiki.*/wikt.*) — mirrors firmware card_matches so a
    // bare canonical name ("trump", "einstein") recalls offline. Fact ids (wd.*) keep ask-only matching:
    // their last segment is a property ("capital"/"author") that must NOT collide with words like "capitale".
    const id = c.id || '';
    if ((id.startsWith('wiki.') || id.startsWith('wikt.'))) {
      const canon = id.split('.').slice(2).join('.');        // wiki.it.donald-trump -> donald-trump
      if (canon && qs === canon) { const r = hit(c); if (r) return r; }
    }
    const keys = c.ask?.[en ? 'en' : 'it'] || [];            // then the stored ask phrasings / aliases
    for (const k of keys) {
      const s = slugify(k);
      if (!s) continue;
      // exact alias (any length, so short canonical aliases like "trump" recall), or substring either way
      // for longer phrasings ("parlami di einstein" contains "einstein"). len>=6 guards against noise.
      if (qs === s || (s.length >= 6 && qs.includes(s)) || (qs.length >= 6 && s.includes(qs))) { const r = hit(c); if (r) return r; }
    }
  }
  return null;
}

// Persist a fact into a learned ODD card — ONLY ever called with TRUSTED, source-grounded text
// (a Wikipedia extract), never raw LLM prose. Identity = the source's canonical TITLE slug, so every
// phrasing of the same thing maps to ONE card (robust dedup, no doubles). Bounded; device schema.
// `askList` = the phrasings used to recall it (title + the user's query + paraphrases).
// Low-level: write a learned card under an EXACT id, deduped by that id (no doubles). All persisted
// cards go through here, so a bio (wiki.<lang>.<slug>) and a fact (wd.<lang>.<slug>.<prop>) coexist
// (different questions) while re-asking the same thing never duplicates.
async function learnRaw(en, id, reply, askList, source, category) {
  if (!id) return false;
  const list = await readLearned(en);
  if (list.some(c => c.id === id)) return false;          // exact-id dedup
  const asks = [...new Set((Array.isArray(askList) ? askList : []).map(s => String(s || '').trim()).filter(Boolean))].slice(0, 8);
  const card = { id, category: category || 'concept', action: 'answer',
    reply: { [en ? 'en' : 'it']: clip250(reply) },
    ask: { [en ? 'en' : 'it']: asks.length ? asks : [String(id).split('.').pop()] },
    source: source || 'curated', last_updated: new Date().toISOString().slice(0, 10), ttl_days: 3650 };
  list.push(card);
  while (list.length > 256) list.shift();                 // bounded
  await mkdir(dirname(learnedFile(en)), { recursive: true });
  await writeFile(learnedFile(en), list.map(c => JSON.stringify(c)).join('\n') + '\n');
  publish('anima.learned', { id, lang: en ? 'en' : 'it' });
  return true;
}

// Bio/concept card from a trusted source. Identity = the source's canonical TITLE slug.
async function learnCard(lang, title, reply, askList, source, category) {
  const en = lang === 'en';
  const slug = slugify(title);
  if (!slug) return false;
  return learnRaw(en, `wiki.${en ? 'en' : 'it'}.${slug}`, reply, [title, ...(Array.isArray(askList) ? askList : [])], source, category);
}

// ---- O3: deterministic precise facts from Wikidata (the structured trusted source) ---------
// Pattern-detect a fact question (no LLM) -> {prop, entity}; resolve on Wikidata; format a crisp,
// SOURCE-GROUNDED answer; learn it (stable facts). Runs before the LLM teacher and needs NO key
// (Wikidata is free) — just connectivity. Offline -> the fetch fails -> falls through to a miss.
const WD_PROP = { born: { p: 'P569', kind: 'time' }, died: { p: 'P570', kind: 'time' },
                  capital: { p: 'P36', kind: 'item' }, author: { p: 'P50', kind: 'item' } };

// Strip a leading preposition/article so "della Francia" / "la Francia" / "il Manzoni" -> the bare
// name (one pass is enough for these patterns). Avoids the "del" matching inside "della" trap.
function stripLead(s) {
  s = String(s).trim();
  s = s.replace(/^(?:dell['’]|d['’]|l['’])\s*/i, '');     // apostrophe forms (no space): d'Italia, l'Aquila, dell'Etna
  s = s.replace(/^(?:della|dello|delle|degli|dei|del|di|the|la|il|lo|le|gli|un[ao]?|a|an)\s+/i, '');
  return s.trim();
}
function factDetect(q, en) {
  const nf = String(q).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  const T = [
    [/(?:quando|in che anno|in quale anno)\b.*\bnat[oaie]\b\s*(.+)/, 'born'],
    [/data di nascita\b\s*(?:di\s+)?(.+)/, 'born'],
    [/when (?:was|were)\s+(.+?)\s+born/, 'born'],
    [/(?:birth date|date of birth) of\s+(.+)/, 'born'],
    [/(?:quando)\b.*\bmort[oaie]\b\s*(.+)/, 'died'],
    [/data di morte\b\s*(?:di\s+)?(.+)/, 'died'],
    [/when did\s+(.+?)\s+die/, 'died'],
    [/capitale\b\s*(.+)/, 'capital'],
    [/capital of\s+(.+)/, 'capital'],
    [/chi ha scritto\s+(.+)/, 'author'],
    [/autore\b\s*(.+)/, 'author'],
    [/who wrote\s+(.+)/, 'author'],
  ];
  for (const [re, prop] of T) {
    const m = nf.match(re);
    if (m && m[1]) { const e = stripLead(m[1].replace(/[?.!]+$/, '').trim()); if (e.length >= 2) return { prop, entity: e }; }
  }
  return null;
}

function fmtWdTime(time, precision, en) {
  const m = /^([+-])(\d+)-(\d{2})-(\d{2})/.exec(time || '');
  if (!m) return null;
  const bc = m[1] === '-', y = +m[2], mo = +m[3], d = +m[4];
  const MON = en ? ['', 'January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December']
                 : ['', 'gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'];
  let s;
  if (precision >= 11 && mo >= 1 && d >= 1) s = en ? `${MON[mo]} ${d}, ${y}` : `${d} ${MON[mo]} ${y}`;
  else if (precision === 10 && mo >= 1) s = `${MON[mo]} ${y}`;
  else s = `${y}`;
  return s + (bc ? (en ? ' BC' : ' a.C.') : '');
}

async function wikidataFact(entity, prop, en) {
  const lang = en ? 'en' : 'it';
  const ua = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (fact)' };
  const P = WD_PROP[prop];
  const api = (qs) => fetch('https://www.wikidata.org/w/api.php?' + qs + '&format=json', { headers: ua });
  try {
    const s = await api(`action=wbsearchentities&search=${encodeURIComponent(entity)}&language=${lang}&uselang=${lang}&limit=1`);
    if (!s.ok) { console.error('WDDBG search !ok', s.status, entity); return null; }
    const q = (await s.json())?.search?.[0];
    if (!q?.id) { console.error('WDDBG no q.id', entity); return null; }
    const c = await api(`action=wbgetentities&ids=${q.id}&props=claims`);
    if (!c.ok) { console.error('WDDBG claims !ok', c.status, q.id); return null; }
    const claims = (await c.json())?.entities?.[q.id]?.claims?.[P.p];
    if (!Array.isArray(claims) || !claims.length) return null;
    // Multi-value properties (P36 capital lists HISTORICAL capitals too) -> pick the CURRENT one:
    // preferred rank, else one with no end-date qualifier (P582), else the first.
    const pick = claims.find(x => x.rank === 'preferred')
              || claims.find(x => !x.qualifiers || !x.qualifiers.P582)
              || claims[0];
    const v = pick?.mainsnak?.datavalue?.value;
    if (!v) return null;
    if (P.kind === 'time') { const t = fmtWdTime(v.time, v.precision, en); return t ? { label: q.label || entity, value: t } : null; }
    if (!v.id) return null;
    // Ask for BOTH the locale AND English: some items lack a localized label (e.g. Q1067 has no
    // 'it' label, only 'en'), and requesting one language alone drops the fallback from the response.
    const l = await api(`action=wbgetentities&ids=${v.id}&props=labels&languages=${lang}%7Cen`);
    if (!l.ok) return null;
    const labs = (await l.json())?.entities?.[v.id]?.labels;
    const label = labs?.[lang]?.value || labs?.en?.value;
    return label ? { label: q.label || entity, value: label } : null;
  } catch { return null; }
}

async function factAnswer(q, lang) {
  const en = lang === 'en';
  const det = factDetect(q, en);
  if (!det) return null;
  const f = await wikidataFact(det.entity, det.prop, en);
  if (!f) return null;
  const reply = {
    born:    en ? `${f.label} was born on ${f.value}.`        : `${f.label} è nato/a il ${f.value}.`,
    died:    en ? `${f.label} died on ${f.value}.`            : `${f.label} è morto/a il ${f.value}.`,
    capital: en ? `The capital of ${f.label} is ${f.value}.`  : `La capitale di ${f.label} è ${f.value}.`,
    author:  en ? `${f.label} was written by ${f.value}.`     : `${f.label} è stato scritto da ${f.value}.`,
  }[det.prop];
  // Coherence: the resolved entity must match what was asked (catches a wrong wbsearchentities hit,
  // e.g. "Georgia" the US state vs the country). If it doesn't, don't assert/save a likely-wrong fact.
  if (!coherent(`${det.entity}`, `${f.label}`)) return null;
  const e = det.entity;
  const asks = {
    born:    en ? [`when was ${e} born`, `${e} birth date`] : [`quando è nato ${e}`, `quando e nata ${e}`, `data di nascita di ${e}`],
    died:    en ? [`when did ${e} die`, `${e} death date`]  : [`quando è morto ${e}`, `data di morte di ${e}`],
    capital: en ? [`capital of ${e}`]                       : [`capitale di ${e}`, `qual è la capitale di ${e}`],
    author:  en ? [`who wrote ${e}`]                        : [`chi ha scritto ${e}`, `autore di ${e}`],
  }[det.prop];
  await learnRaw(en, `wd.${en ? 'en' : 'it'}.${slugify(e)}.${det.prop}`, reply, [q, ...asks], 'wikidata:' + det.prop, 'fact');
  await learnTriple(en, det.entity, det.prop, f.value, f.label);   // feed the HDC reasoning mind (compose/analogy offline)
  return { query: q, tier: 'fact', action: 'answer', intent: 'wikidata', confidence: 96, state: 'idle', reply };
}

// ---- HDC reasoning mind: structured triples (subject --rel--> value) feeding the hyperdimensional core.
// Every Wikidata fact we learn also drops a clean triple here. From these, ANIMA reasons OFFLINE: recall a
// relation by UNBINDING (robust to phrasing the exact-match cache never stored), do ANALOGY, and refuse by
// COHERENCE when it doesn't know — all with XOR/popcount, no network. See tools/anima/hdc.mjs.
const mindFile = (en) => join(SD, 'data', 'anima', 'learned', 'mind.' + (en ? 'en' : 'it') + '.jsonl');
async function readTriples(en) {
  try { return (await readFile(mindFile(en), 'utf8')).split('\n').filter(Boolean).map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean); }
  catch { return []; }
}
async function learnTriple(en, entity, rel, value, label) {
  const subject = slugify(entity); if (!subject || !value) return;
  const list = await readTriples(en);
  if (list.some(t => t.subject === subject && t.rel === rel)) return;   // dedup (subject,rel)
  list.push({ subject, rel, value: String(value), label: label || entity });
  while (list.length > 512) list.shift();
  await mkdir(dirname(mindFile(en)), { recursive: true });
  await writeFile(mindFile(en), list.map(t => JSON.stringify(t)).join('\n') + '\n');
}
// Build the in-RAM Mind from the triples (cheap; on-device this would be cached + paged from SD).
async function buildMind(en) {
  const m = new Mind(); const labels = new Map();
  for (const t of await readTriples(en)) { m.learn(t.subject, t.rel, t.value); labels.set(t.subject, t.label); }
  return { m, labels };
}
const FACT_REPLY = (prop, label, value, en) => ({
  born:    en ? `${label} was born on ${value}.`       : `${label} è nato/a il ${value}.`,
  died:    en ? `${label} died on ${value}.`           : `${label} è morto/a il ${value}.`,
  capital: en ? `The capital of ${label} is ${value}.` : `La capitale di ${label} è ${value}.`,
  author:  en ? `${label} was written by ${value}.`    : `${label} è stato scritto da ${value}.`,
}[prop]);
const HDC_GATE = 4.0;   // coherence units (margin/σ). Bench: known≈42, unknown≈0.3 → wide, safe separation.

// Offline relational reasoning: detect a fact question, UNBIND it from the grown mind. Answers phrasings
// the exact-match cache never stored, and gates by intrinsic coherence (refuse rather than misattribute).
async function hdcReason(q, lang) {
  const en = lang === 'en';
  const det = factDetect(q, en); if (!det) return null;
  const { m, labels } = await buildMind(en);
  const subj = slugify(det.entity); if (!m.record(subj)) return null;
  const r = m.ask(subj, det.prop);
  if (!r.key || r.coherence < HDC_GATE) return null;             // intrinsic-honesty gate
  const reply = FACT_REPLY(det.prop, labels.get(subj) || det.entity, r.key, en);
  if (!reply) return null;
  return { query: q, tier: 'fact', action: 'answer', intent: 'hdc', confidence: Math.min(95, 80 + Math.round(r.coherence)), state: 'idle', reply, coherence: Number(r.coherence.toFixed(1)) };
}

// ---- DEDUCTIVE tier: answer INVERSE-relation questions never stored, via the permutation-KGE. ANIMA
// learned "X capitale_di Y" / "Y autore X" (forward); this answers the OTHER direction by inverting the
// relation's rotation (ρ⁻¹), gated by coherence. Pure offline deduction. See tools/anima/kge.mjs.
const titleCase = (s) => String(s).replace(/\b([a-zàèéìòù])/, (c) => c.toUpperCase());
function inverseDetect(q) {
  const s = String(q).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[?.!]+$/, '').trim();
  let m;
  if ((m = s.match(/^(?:cosa|che cosa|quali opere|che opere|che)\s+(?:ha\s+)?(?:scritto|composto|scrisse|creato)\s+(.+)/))) return { rel: 'author', name: m[1].trim() };
  if ((m = s.match(/di\s+(?:quale|che)\s+(?:paese|stato|nazione)\s+e\s+(?:la\s+)?capitale\s+(.+)/))) return { rel: 'capital', name: m[1].trim() };
  if ((m = s.match(/^(.+?)\s+e\s+(?:la\s+)?capitale\s+di\s+(?:quale|che)\s+(?:paese|stato|nazione)/))) return { rel: 'capital', name: m[1].trim() };
  return null;
}
async function kgInfer(q, lang) {
  const det = inverseDetect(q); if (!det) return null;
  const en = lang === 'en';
  const triples = (await readTriples(en)).filter(t => t.rel === det.rel);
  if (!triples.length) return null;
  const kg = new KG(); const label = new Map();
  for (const t of triples) { const hs = slugify(t.label || t.subject), vs = slugify(t.value); kg.add(hs, det.rel, vs); label.set(hs, t.label || t.subject); label.set(vs, t.value); }
  kg.build();
  const res = kg.resolve(det.name);                          // map a partial name ("Manzoni") to the entity
  if (!res.key || res.coherence < 2.0) return null;
  const r = kg.inverse(res.key, det.rel);
  if (!r.key || r.coherence < HDC_GATE) return null;
  const ans = label.get(r.key) || r.key, who = titleCase(det.name);
  const reply = det.rel === 'capital'
    ? (en ? `${who} is the capital of ${ans}.` : `${who} è la capitale di ${ans}.`)
    : (en ? `${who} wrote ${ans}.` : `${who} ha scritto ${ans}.`);
  return { query: q, tier: 'fact', action: 'answer', intent: 'kge', confidence: Math.min(95, 80 + Math.round(r.coherence)), state: 'idle', reply, coherence: Number(r.coherence.toFixed(1)) };
}

// ---- TRANSITIVE knowledge: the geographic containment chain (Wikidata P131 "located in" + P30 continent).
// This is the FUEL the deductive KGE was missing: containment IS transitive, so once ANIMA learns the
// adjacent links (Lione→Francia, Francia→Europa) it DEDUCES "in che continente è Lione" by composing
// rotations — at any depth, offline, even across links learned from different queries. Real knowledge,
// growing the graph. Each adjacency is one located_in triple in the same mind the KGE reads.
const WD_UA = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (kge geo)' };
const wdSleep = (ms) => new Promise((r) => setTimeout(r, ms));   // gentle pacing: anonymous Wikidata throttles bursts
const wdJSON = async (qs) => { const r = await fetch('https://www.wikidata.org/w/api.php?' + qs + '&format=json', { headers: WD_UA }); return r.ok ? r.json() : null; };
async function wdSearch(name, lang) { const j = await wdJSON(`action=wbsearchentities&search=${encodeURIComponent(name)}&language=${lang}&uselang=${lang}&limit=1`); const s = j?.search?.[0]; return s?.id ? { id: s.id, label: s.label || name } : null; }
// One round-trip per entity: claims AND labels together (halves the call count vs separate fetches).
// Returns {id, label, p131 (located-in id), p30 (continent id)}, picking the CURRENT value (preferred /
// no end-date qualifier) for multi-valued claims.
async function wdGet(qid, lang) {
  const j = await wdJSON(`action=wbgetentities&ids=${qid}&props=claims%7Clabels&languages=${lang}%7Cen`);
  const e = j?.entities?.[qid]; if (!e) return null;
  const label = e.labels?.[lang]?.value || e.labels?.en?.value || qid;
  const claimItem = (p) => { const cl = e.claims?.[p]; if (!Array.isArray(cl) || !cl.length) return null; const pick = cl.find(x => x.rank === 'preferred') || cl.find(x => !x.qualifiers || !x.qualifiers.P582) || cl[0]; return pick?.mainsnak?.datavalue?.value?.id || null; };
  const claimYear = (p) => { const cl = e.claims?.[p]; if (!Array.isArray(cl) || !cl.length) return null; const t = (cl.find(x => x.rank === 'preferred') || cl[0])?.mainsnak?.datavalue?.value?.time; const m = t && t.match(/^([+-]?\d+)-/); return m ? parseInt(m[1], 10) : null; };
  return { id: qid, label, p131: claimItem('P131'), p30: claimItem('P30'), p27: claimItem('P27'), bornYear: claimYear('P569') };
}

// ---- NEURO-SYMBOLIC fact oracle: unified access for the combinators. Offline (learned triples) first,
// then Wikidata (learning the fact for next time). Composes nowhere-stored answers; see anima/combinator.mjs.
async function getFact(entity, rel, lang) {
  const en = lang === 'en';
  const se = slugify(entity);
  const cached = (await readTriples(en)).find(t => slugify(t.subject) === se && t.rel === rel);
  if (cached) return rel === 'born' ? { year: parseInt(cached.value, 10), conf: 0.9, src: 'learned' } : { value: cached.value, conf: 0.9, src: 'learned' };
  try {
    const s = await wdSearch(entity, lang); if (!s) return null;
    const g = await wdGet(s.id, lang); if (!g) return null;
    if (rel === 'born') { if (g.bornYear == null) return null; await learnTriple(en, entity, 'born', String(g.bornYear), s.label); return { year: g.bornYear, conf: 0.9, src: 'wikidata:P569' }; }
    if (rel === 'nationality') { if (!g.p27) return null; await wdSleep(120); const c = await wdGet(g.p27, lang); if (!c) return null; await learnTriple(en, entity, 'nationality', c.label, s.label); return { value: c.label, conf: 0.9, src: 'wikidata:P27' }; }
    if (rel === 'continent') { if (!g.p30) return null; await wdSleep(120); const c = await wdGet(g.p30, lang); if (!c) return null; await learnTriple(en, entity, 'continent', c.label, s.label); return { value: c.label, conf: 0.9, src: 'wikidata:P30' }; }
  } catch {}
  return null;
}
const combinatorAnswer = (q, lang) => combinatorRun(q, getFact, lang);
const CONTINENTS = new Set(['europa', 'asia', 'africa', 'america', 'americhe', 'nord-america', 'sud-america', 'oceania', 'antartide', 'europe', 'north-america', 'south-america', 'oceania', 'antarctica']);
async function learnChain(place, lang) {
  const en = lang === 'en';
  const s = await wdSearch(place, lang); if (!s) return null;
  let cur = await wdGet(s.id, lang); if (!cur) return null;
  const seen = new Set([cur.id]); const chain = [cur.label]; let depth = 0;
  while (depth < 4 && cur.p131 && !seen.has(cur.p131)) {     // walk up administrative containment (P131)
    await wdSleep(120); const nxt = await wdGet(cur.p131, lang); if (!nxt) break;
    await learnTriple(en, cur.label, 'located_in', nxt.label, cur.label); chain.push(nxt.label); seen.add(nxt.id); cur = nxt; depth++;
  }
  if (cur.p30 && !seen.has(cur.p30)) {                       // append the continent of the topmost entity
    await wdSleep(120); const cont = await wdGet(cur.p30, lang);
    if (cont) { await learnTriple(en, cur.label, 'located_in', cont.label, cur.label); chain.push(cont.label); }
  }
  return chain.length > 1 ? chain : null;
}
function locDetect(q) {
  const s = String(q).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/['’]/g, ' ').replace(/[?.!]+$/, '').replace(/\s+/g, ' ').trim();
  let m;
  if ((m = s.match(/in (?:che|quale) (continente|paese|stato|nazione|regione)\s+(?:e |si trova )?(.+)/))) return { level: m[1], place: m[2].trim() };
  if ((m = s.match(/^(?:dove (?:si trova|e )|dov e )\s*(.+)/))) return { level: 'dove', place: m[1].trim() };
  return null;
}
// Deduce the containment ancestors of `place` from the learned located_in graph (offline, KGE).
async function deduceLocation(en, place) {
  const triples = (await readTriples(en)).filter(t => t.rel === 'located_in');
  if (!triples.length) return null;
  const kg = new KG(); const label = new Map();
  for (const t of triples) { const c = slugify(t.label || t.subject), p = slugify(t.value); kg.add(c, 'located_in', p); label.set(c, t.label || t.subject); label.set(p, t.value); }
  kg.build();
  let key = slugify(place); if (!label.has(key)) { const res = kg.resolve(place); if (!res.key || res.coherence < 3) return null; key = res.key; }
  const reached = kg.reach(key, { maxDepth: 5, gate: HDC_GATE });
  if (!reached.length) return null;
  reached.sort((a, b) => a.path.length - b.path.length);
  return reached.map(r => ({ label: label.get(r.entity) || r.entity, depth: r.path.length }));
}
async function locAnswer(q, lang) {
  const det = locDetect(q); if (!det) return null;
  const en = lang === 'en';
  let chain = await deduceLocation(en, det.place);     // offline-first
  if (!chain) { try { if (await learnChain(det.place, lang)) chain = await deduceLocation(en, det.place); } catch {} }
  if (!chain || !chain.length) return null;
  const isCont = (lab) => CONTINENTS.has(slugify(lab));
  const who = titleCase(det.place);
  let reply;
  if (det.level === 'continente') { const c = chain.find(x => isCont(x.label)); if (!c) return null; reply = en ? `${who} is in ${c.label}.` : `${who} si trova in ${c.label}.`; }
  else if (['paese', 'stato', 'nazione', 'regione'].includes(det.level)) { const nc = chain.filter(x => !isCont(x.label)); const c = nc[nc.length - 1]; if (!c) return null; reply = en ? `${who} is in ${c.label}.` : `${who} si trova in ${c.label}.`; }
  else { reply = en ? `${who} is in ${chain.map(x => x.label).join(', ')}.` : `${who} si trova in ${chain.map(x => x.label).join(', ')}.`; }
  return { query: q, tier: 'fact', action: 'answer', intent: 'kge-geo', confidence: 90, state: 'idle', reply };
}

// ---- O3b: word definitions from Wiktionary (another structured, EXACT-match trusted source) ------
// "cosa significa X" / "what does X mean". The REST `definition` endpoint is an EXACT page lookup (no
// fuzzy search) -> no entity drift; a word that isn't in the dictionary is a clean 404 -> falls
// through to a miss. HTML is stripped to plain ODD text. Like the Wikidata facts, this runs as a
// deterministic, FREE source BEFORE the LLM teacher — but only on an L0 miss, so curated tech-concept
// cards (RAM/heap/wifi) still win over a generic dictionary gloss.
function stripHtml(s) {
  return String(s || '').replace(/<[^>]+>/g, '').replace(/&amp;/g, '&').replace(/&quot;/g, '"')
    .replace(/&#0?39;|&apos;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&nbsp;/g, ' ')
    .replace(/\s+/g, ' ').trim();
}
function defDetect(q, en) {
  // Lowercase but KEEP accents: the Wiktionary lookup is an EXACT page match, so "serendipità" must
  // stay accented (stripping -> "serendipita" -> 404). The triggers themselves carry no accents.
  const nf = String(q).toLowerCase();
  const T = en ? [
    /what does\s+(.+?)\s+mean\b/,
    /(?:meaning|definition) of\s+(.+)/,
    /define\s+(.+)/,
  ] : [
    /(?:che cosa|cosa|che)\s+significa\s+(.+)/,
    /(?:cosa|che)\s+(?:vuol|vuole)\s+dire\s+(.+)/,
    /(?:significato|definizione)\s+(?:di |della |del |dello )?(.+)/,
    /definisci\s+(.+)/,
  ];
  for (const re of T) {
    const m = nf.match(re);
    if (m && m[1]) {
      let w = m[1].replace(/[?.!]+$/, '').trim();
      w = w.replace(/^(?:la |il |the )?(?:parola|termine|word|term)\s+/i, '');  // "(della) parola X" -> X
      w = stripLead(w);
      if (w && w.length >= 2 && w.split(/\s+/).length <= 3) return { word: w };
    }
  }
  return null;
}

// English: the REST `definition` endpoint is clean & structured (HTML per part-of-speech). NOTE: that
// endpoint is enabled ONLY on en.wiktionary (it.wiktionary answers 501) — so Italian takes a different,
// equally-deterministic path below.
async function wiktionaryDefEn(word) {
  const ua = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (define)' };
  const title = encodeURIComponent(word.replace(/ /g, '_'));
  try {
    const r = await fetch(`https://en.wiktionary.org/api/rest_v1/page/definition/${title}`, { headers: ua });
    if (!r.ok) return null;                                 // 404 -> not in the dictionary -> clean miss
    const j = await r.json();
    const bucket = j.en || j[Object.keys(j)[0]];            // prefer the English-language section
    if (!Array.isArray(bucket) || !bucket.length) return null;
    for (const sec of bucket) {
      for (const d of (sec.definitions || [])) {
        const txt = stripHtml(d.definition);
        if (txt && txt.length >= 3) return { word, pos: sec.partOfSpeech || '', def: txt };
      }
    }
    return null;
  } catch { return null; }
}

// Italian: parse the page's plain-text extract (the Action API gives clean, sectioned text — no
// wikitext/HTML). Structure is stable: `== Italiano ==` -> `=== <PartOfSpeech> ===` -> headword line
// -> definition paragraph(s). We take the FIRST POS's first real definition. If we can't isolate one
// confidently (no Italian section, no definition) -> null -> a clean miss, never a guess.
function parseItExtract(extract, word) {
  const lines = String(extract || '').split('\n');
  const i = lines.findIndex(l => /^==\s*Italiano\s*==/.test(l.trim()));
  if (i < 0) return null;                                   // word exists but has no Italian entry
  let pos = '', headwordSeen = false;
  for (let k = i + 1; k < lines.length; k++) {
    const t = lines[k].trim();
    if (/^==[^=].*==$/.test(t)) break;                      // next top-level language section -> stop
    if (!t) continue;
    const ph = t.match(/^===+\s*(.+?)\s*===+$/);
    if (ph) { pos = ph[1]; headwordSeen = false; continue; } // part-of-speech header
    if (!headwordSeen) { headwordSeen = true; continue; }    // the headword/grammar line -> skip
    const def = t.replace(/\s+/g, ' ').trim();               // first substantive line = the definition
    if (def.length >= 8) return { word, pos, def };
  }
  return null;
}
async function wiktionaryDefIt(word) {
  const ua = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (define)' };
  try {
    const r = await fetch('https://it.wiktionary.org/w/api.php?action=query&format=json&prop=extracts'
      + '&explaintext=1&redirects=1&titles=' + encodeURIComponent(word), { headers: ua });
    if (!r.ok) return null;
    const pages = (await r.json())?.query?.pages;
    const pg = pages && Object.values(pages)[0];
    if (!pg || pg.missing !== undefined || !pg.extract) return null;
    return parseItExtract(pg.extract, word);
  } catch { return null; }
}
const wiktionaryDef = (word, en) => en ? wiktionaryDefEn(word) : wiktionaryDefIt(word);

async function defAnswer(q, lang) {
  const en = lang === 'en';
  const det = defDetect(q, en);
  if (!det) return null;
  const d = await wiktionaryDef(det.word, en);
  if (!d) return null;
  const head = d.pos ? `${d.word} (${d.pos.toLowerCase()})` : d.word;
  const reply = clip250(`${head}: ${d.def}`);
  const e = det.word;
  const asks = en
    ? [`what does ${e} mean`, `meaning of ${e}`, `definition of ${e}`]
    : [`cosa significa ${e}`, `che significa ${e}`, `significato di ${e}`, `definizione di ${e}`];
  // Identity = wikt.<lang>.<word> -> every phrasing of the same word maps to ONE card (robust dedup).
  await learnRaw(en, `wikt.${en ? 'en' : 'it'}.${slugify(e)}`, reply, [q, ...asks], 'wiktionary:' + (en ? 'en' : 'it'), 'concept');
  return { query: q, tier: 'fact', action: 'answer', intent: 'wiktionary', confidence: 94, state: 'idle', reply };
}

// ---- O1: encyclopedic entity tier ("chi è / cos'è X" -> Wikipedia summary) -------------------
// The single most-requested path ("chi è Einstein / Trump", "cos'è la fotosintesi", "batman?"). FREE
// (no API key — Wikipedia is open), runs on an L0/fact/def MISS, and LEARNS the answer so the same
// class of question is then served OFFLINE forever. Mirrors firmware nucleo_anima_online_entity/answer.

// Ephemeral guard (mirror firmware is_ephemeral): a query bound to "now"/current-state must never be
// frozen as a timeless fact. We still answer, but DON'T learn it (no stale "presidente oggi" card).
const EPHEMERAL_MARKERS = ['oggi','domani','dopodomani','stamattina','stamani','stasera','stanotte',
  'adesso','attuale','attualmente','in corso','questa settimana','questo mese','ultime','ultimora',
  'today','tomorrow','tonight','right now','this week','this month','latest','currently','now'];
const isEphemeral = (q) => { const s = String(q).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  return EPHEMERAL_MARKERS.some(m => new RegExp('\\b' + m.replace(/ /g, '\\s+') + '\\b').test(s)); };

// ---- live WEATHER (Open-Meteo, keyless, NEVER cached — volatility law §6) -------------------
// Geocode the place, then fetch the daily forecast for the requested day (start_date=end_date so any
// day within the ~15-day horizon works, incl. "24 febbraio"/weekday/"fra 3 giorni"). Mirrors the
// firmware weather_fetch(); the parsing/formatting come from the shared weather.mjs so web == device.
const WX_UA = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (weather)' };
const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
// Geocode with a multi-word fallback: Open-Meteo's index misses "reggio emilia" (it's stored as
// "Reggio nell'Emilia"), so on a miss we search the FIRST token and pick the candidate whose
// region/admin/country matches the remaining words ("emilia" -> Reggio nell'Emilia). Mirrors the
// firmware's full-then-first-token retry.
async function geocodeCity(city, en) {
  const q = async (name, count) => {
    const r = await fetch(`https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(name)}&count=${count}&language=${en ? 'en' : 'it'}&format=json`, { headers: WX_UA });
    return r.ok ? ((await r.json())?.results || []) : [];
  };
  let res = await q(city, 1);
  if (res.length && typeof res[0].latitude === 'number') return res[0];
  const toks = city.split(/\s+/).filter(Boolean);
  if (toks.length > 1) {
    const cand = (await q(toks[0], 10)).filter(c => typeof c.latitude === 'number');
    if (cand.length) {
      const rest = toks.slice(1).map(fold);
      let best = cand[0], bestScore = -1;
      for (const c of cand) {
        const hay = fold(`${c.name || ''} ${c.admin1 || ''} ${c.admin2 || ''} ${c.admin3 || ''} ${c.country || ''}`);
        const score = rest.reduce((s, t) => s + (hay.includes(t) ? 1 : 0), 0);
        if (score > bestScore) { bestScore = score; best = c; }
      }
      return best;   // score-matched region if any, else most-populous match of the first token
    }
  }
  return null;
}
async function fetchForecast(city, dayOffset, en, now = new Date()) {
  try {
    const hit = await geocodeCity(city, en);
    if (!hit || typeof hit.latitude !== 'number') return null;
    const d = new Date(now.getFullYear(), now.getMonth(), now.getDate() + dayOffset);
    const ds = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
    const url = `https://api.open-meteo.com/v1/forecast?latitude=${hit.latitude}&longitude=${hit.longitude}`
      + `&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max`
      + `&current=temperature_2m&timezone=auto&start_date=${ds}&end_date=${ds}`;
    const f = await fetch(url, { headers: WX_UA });
    if (!f.ok) return null;
    const fj = await f.json();
    const dy = fj?.daily;
    if (!dy || !Array.isArray(dy.time) || !dy.time.length) return null;
    if (typeof dy.weather_code?.[0] !== 'number') return null;
    return {
      place: hit.name || city,
      code: dy.weather_code[0],
      tmax: dy.temperature_2m_max?.[0],
      tmin: dy.temperature_2m_min?.[0],
      precipProb: typeof dy.precipitation_probability_max?.[0] === 'number' ? dy.precipitation_probability_max[0] : undefined,
      tcur: dayOffset === 0 ? fj?.current?.temperature_2m : undefined,
    };
  } catch { return null; }
}
function wxResult(q, reply, conf) {
  return { query: q, tier: 'remote', action: 'answer', intent: 'weather', confidence: conf, state: 'idle', reply, learned: false, verified: true, domain: 'weather' };
}
async function weatherAnswer(q, lang, allowOnline) {
  const en = lang === 'en';
  const p = parseWeather(q, { lang });
  if (!p.isWeather) return null;
  if (!p.city)
    return wxResult(q, en ? 'Which city? e.g. "weather in Rome".' : 'Per quale città? Es. "che tempo fa a Roma".', 55);
  if (p.tooFar)
    return wxResult(q, en ? `I only have a ~15-day forecast — ${p.dateLabel} is too far ahead.`
                          : `Ho previsioni solo fino a ~15 giorni: ${p.dateLabel} è troppo lontano.`, 60);
  if (!allowOnline)
    return wxResult(q, en ? 'I need internet to check the weather.' : 'Mi serve internet per controllare il meteo.', 45);
  const fc = await fetchForecast(p.city, p.dayOffset, en);
  if (!fc || typeof fc.tmax !== 'number' || typeof fc.tmin !== 'number')
    return wxResult(q, en ? `I couldn't get the weather for ${p.city}.` : `Non riesco a recuperare il meteo per ${p.city}.`, 40);
  return wxResult(q, formatWeather(p, fc.place, fc, lang), 90);
}

// "who/what is X" triggers, longest-first (so "che cos'è" wins over "cos'è"). Both accented and bare
// spellings; NB the "significa/definizione" forms are deliberately left to defDetect (Wiktionary).
const ENT_TRIG_IT = ['che cosa sono ','che cos e ','che cose ','cosa sono ','cosa e ','cose ',
  'cos e ','cose ','chi era ','chi erano ','chi sono ','chi e ','parlami di ','parlami della ',
  'parlami del ','dimmi chi e ','dimmi cos e ','raccontami di ','raccontami della ','spiegami ',
  'spiegami cos e ','sai chi e ','sai cos e ','conosci '];
const ENT_TRIG_EN = ['tell me about ','who is ','who was ','who are ','what is ','what are ',
  'what was ','whats ','whos ','do you know ','explain '];
const ENT_ARTICLES = ['the ','a ','an ','il ','lo ','la ','i ','gli ','le ','un ','uno ','una ','del ','della '];
// Detect an entity question. Returns the bare entity string or null. Tries BOTH languages' triggers
// (ANIMA understands IT+EN regardless of reply language). Apostrophes/accents normalized away first.
function entityDetect(q) {
  let s = String(q || '').toLowerCase().replace(/['’`]/g, ' ').normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/\s+/g, ' ').trim();
  const trigs = [...ENT_TRIG_IT, ...ENT_TRIG_EN].sort((a, b) => b.length - a.length);
  let ent = null;
  for (const t of trigs) { if (s.startsWith(t)) { ent = s.slice(t.length); break; } }
  if (ent == null) return null;
  ent = ent.trim();
  for (const a of ENT_ARTICLES) { if (ent.startsWith(a)) { ent = ent.slice(a.length).trim(); break; } }
  ent = ent.replace(/[?.!,;:]+$/, '').trim();
  if (!ent || slugify(ent).length < 2 || ent.split(' ').length > 6) return null;
  return ent;
}

// Resolve an entity on Wikipedia and relay its summary. Offline -> wikiVerify's fetch throws -> null
// (a clean miss). On a hit, persist a learned card (unless ephemeral) so it is recalled offline next
// time. Auto typo-rescue: if the raw entity 404s, retry the spell-corrected form once.
// Is the resolved Wikipedia title actually about what was asked? opensearch is FUZZY, so guard the
// bare-noun fallback against a loose match: require a title token to equal / contain / be within edit
// distance 2 of the query (keeps typo tolerance "enstein"->Einstein, rejects "xyzqw"->unrelated page).
function titleRelated(entity, title) {
  const a = slugify(entity);
  if (!a) return false;
  return slugify(title).split('-').filter(Boolean).some(t =>
    t === a || (t.length >= 4 && a.length >= 4 && (t.includes(a) || a.includes(t))) || damlev(a, t, 2) <= 2);
}
async function entityResolve(entity, q, lang, opts = {}) {
  const en = lang === 'en';
  let v = await wikiVerify(entity, en);
  // Typo rescue + cross-lingual ONLY for an explicit "chi è X" (the user named X). The bare-noun
  // fallback stays strict: same language, no fuzzy expansion (so a stray EN page can't hijack an IT term).
  if (!v && !opts.strict) { const fix = spellfix(entity); if (fix && fix !== entity) v = await wikiVerify(fix, en); }
  if (!v && !en && !opts.strict) v = await wikiVerify(entity, true);   // cross-lingual last resort (IT query, EN-only page)
  if (!v) return null;
  if (opts.strict && !titleRelated(entity, v.title)) return null;     // bare fallback: reject a fuzzy mismatch
  const reply = clip250(v.extract);
  const r = { query: q, tier: 'fact', action: 'answer', intent: 'wikipedia', confidence: 90, state: 'idle', reply, learned: false, verified: true };
  if (!isEphemeral(q)) {                                      // freeze only timeless facts (volatility law §6)
    // SC3 gate: persist ONLY if the resolution is coherent with the asked entity, judged against the
    // device's own learned cards (self-calibrated, no hand threshold). Blocks fuzzy/typo drift like
    // "berluscono" -> "Politica italiana" from poisoning the ODD. Answer is still relayed; just not saved.
    const g = cohGate(entity, v.title, v.extract, cohCalibration(await readLearned(en), en));
    if (g.accept) {
      r.learned = await learnCard(lang, v.title, v.extract, [entity, q, v.title], 'wikipedia:' + (en ? 'en' : 'it') + ':' + v.title, classifyDesc(v.desc));
    } else {
      r.incoherent = true;
      console.log(`[SC3] reject: "${entity}" -> "${v.title}" (co=${g.co.toFixed(2)} ground=${g.g} thr=${g.thr.toFixed(2)}) — answered, not learned`);
    }
  }
  return r;
}
async function entityAnswer(q, lang) {
  const ent = entityDetect(q);
  if (!ent) return null;
  return entityResolve(ent, q, lang);
}

// Bare-entity fallback: a short noun phrase with no command/verb structure ("batman?", "einstein",
// "torre eiffel") that L0 missed. Wikipedia itself is the validator — a non-entity ("asdfgh", "ciao")
// 404s / disambiguates and wikiVerify returns null -> clean honest miss. Only fires online, last
// before the teacher, and not for greetings/acks (cheap stoplist to avoid a wasted fetch).
const BARE_STOP = new Set(('ciao salve buongiorno buonasera buonanotte grazie prego scusa scusami ok okay '
  + 'si no forse certo bene male aiuto ciaone hey ehi hello hi thanks thank yes no maybe please bye '
  + 'anima nucleo nucleos').split(/\s+/).filter(Boolean));
async function bareEntity(q, lang) {
  const raw = String(q || '').replace(/[?.!]+$/, '').trim();
  const toks = raw.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').split(/[^a-z0-9]+/).filter(Boolean);
  if (toks.length < 1 || toks.length > 3) return null;        // entities are short noun phrases
  if (toks.some(t => t.length < 3 || /^\d+$/.test(t))) return null;
  if (toks.some(t => BARE_STOP.has(t))) return null;
  if (isEphemeral(q)) return null;
  return entityResolve(raw, q, lang, { strict: true });       // wikiVerify + titleRelated gate real vs junk
}

// Verify a topic against Wikipedia (the trusted source): opensearch -> canonical title -> REST
// summary. Returns {title, extract, desc} for a real "standard" page, or null (disambiguation / not
// found) -> the claim is NOT persisted. This is what makes "save only what's certain" real.
async function wikiVerify(topic, en) {
  const host = (en ? 'en' : 'it') + '.wikipedia.org';
  const ua = { 'User-Agent': 'NucleoOS-ANIMA/1.0 (teacher verify)' };
  try {
    const os = await fetch(`https://${host}/w/api.php?action=opensearch&limit=1&namespace=0&format=json&search=${encodeURIComponent(topic)}`, { headers: ua });
    if (!os.ok) return null;
    const arr = await os.json();
    const title = arr?.[1]?.[0];
    if (!title) return null;
    const su = await fetch(`https://${host}/api/rest_v1/page/summary/${encodeURIComponent(String(title).replace(/ /g, '_'))}`, { headers: ua });
    if (!su.ok) return null;
    const j = await su.json();
    if (j.type && /disambiguation/.test(j.type)) return null;   // ambiguous -> don't save
    if (!j.extract) return null;
    return { title: j.title || title, extract: j.extract, desc: j.description || '' };
  } catch { return null; }
}

// Light category from a Wikipedia one-line description (mirrors the firmware classify()).
function classifyDesc(desc) {
  const d = String(desc || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  const has = (...k) => k.some(x => d.includes(x));
  if (has('politic','fisic','scienziat','attore','attrice','scrittore','scrittrice','calciator','matematic','pittore','filosof','navigator','esplorat','compositor','regist','musicist',' nato',' nata','born ','physicist','scientist','actor','writer','singer','footballer','painter','philosopher','emperor','imperator')) return 'person';
  if (has('citta','comune','capitale','capital','nazione','regione','fiume','monte','montagn','lago','isola','city','town','country','region','river','mountain','lake','island','village','paese','continent')) return 'place';
  if (has('azienda','societa','organizzazione','squadra','partito','banca','universit','company','organization','team','party','bank','university')) return 'organization';
  if (has('film','movie','libro','book','romanzo','novel','canzone','song','album','videogioco','video game','dipinto','painting','serie','series')) return 'work';
  if (has('specie','species','genere','genus','animale','animal','pianta','plant','uccello','bird')) return 'species';
  if (has('guerra','war','battaglia','battle','torneo','tournament','evento','event')) return 'event';
  return 'concept';
}

// Teacher config: /data/anima/teacher.json on the (sim) SD — written by the AI Chat app's
// "enable teacher" toggle — with an env fallback. No key anywhere -> teacher disabled (honest miss).
// Mirrors the firmware teacher_cfg(); the same file path the device reads.
async function teacherCfg() {
  let cfg = {};
  try { cfg = JSON.parse(await readFile(join(SD, 'data', 'anima', 'teacher.json'), 'utf8')) || {}; } catch {}
  return {
    key: cfg.key || process.env.GROQ_KEY || '',
    base: (cfg.base || process.env.GROQ_BASE || 'https://api.groq.com/openai/v1').replace(/\/+$/, ''),
    model: cfg.model || process.env.GROQ_MODEL || 'llama-3.1-8b-instant',
  };
}

// ONLINE translation via the teacher LLM (Grok). Distinct from teacherAnswer (which is fact-shaped: JSON
// + Wikipedia verification) — here we just run the user's translation COMMAND and return the translation
// verbatim. Mirrors the firmware online-first translate path (tool_translate -> nucleo_anima_online_chat).
// Returns an ANIMA result (tier L3) or null (no key / network error -> caller falls back to the dictionary).
async function translateOnline(q, lang) {
  const { key, base, model } = await teacherCfg();
  if (!key) return null;
  const sys = 'You are a translation engine between Italian and English. The user gives a translation '
    + 'request such as "traduci X in inglese" / "translate X to italian" / "come si dice X in inglese". '
    + 'Carry it out: output ONLY the translation of the phrase X into the requested language — no preamble, '
    + 'no quotes, no explanation. If no target language is given, translate Italian->English or English->Italian.';
  try {
    const resp = await fetch(base + '/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + key },
      body: JSON.stringify({ model, temperature: 0.2, max_tokens: 512,
        messages: [{ role: 'system', content: sys }, { role: 'user', content: String(q) }] }),
    });
    if (!resp.ok) return null;
    const j = await resp.json();
    const txt = (j && j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content || '').trim();
    if (!txt) return null;
    return { tier: 'L3', action: 'answer', intent: 'translate', state: 'tool', confidence: 70,
             reply: txt, trace: 'online · grok' };
  } catch { return null; }
}

// Ask the teacher LLM (online), classify, maybe learn. Returns an ANIMA result or null (no key /
// error -> caller falls back to an honest miss; offline is therefore hallucination-proof).
async function teacherAnswer(q, lang) {
  const { key, base, model } = await teacherCfg();
  if (!key) return null;
  const en = lang === 'en';
  const sys = en
    ? 'You are the teacher for ANIMA, an OFFLINE assistant on a tiny device. Answer in English. Return ONLY JSON: {"reply":"<=250 chars","kind":"knowledge"|"ephemeral","topic":"short canonical name if knowledge, e.g. \\"Photosynthesis\\"","ask":["2-3 SHORT ways a user might ASK this, as questions, not statements"],"confidence":0..1}. "knowledge"=a GENERAL, reusable fact (definitions, history, science, how X works) worth remembering forever. "ephemeral"=personal/specific/one-off (reminders, "my ...", tasks, chit-chat, a specific calculation) — do NOT save it. If unsure or personal -> "ephemeral". Never invent: if you do not know, say so and set kind="ephemeral".'
    : 'Sei il teacher di ANIMA, un assistente OFFLINE su un dispositivo minuscolo. Rispondi in italiano. Restituisci SOLO JSON: {"reply":"<=250 caratteri","kind":"knowledge"|"ephemeral","topic":"nome canonico breve se knowledge, es. \\"Fotosintesi\\"","ask":["2-3 modi BREVI in cui un utente lo CHIEDEREBBE, come domande, non frasi"],"confidence":0..1}. "knowledge"=fatto GENERALE e riusabile (definizioni, storia, scienza, come funziona X) da ricordare per sempre. "ephemeral"=personale/specifico/una-tantum (promemoria, "mio/mia", compiti, chiacchiere, un calcolo specifico) — da NON salvare. Se incerto o personale -> "ephemeral". Non inventare: se non sai, dillo e metti kind="ephemeral".';
  let j;
  try {
    const resp = await fetch(base + '/chat/completions', { method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + key },
      body: JSON.stringify({ model, temperature: 0.2, response_format: { type: 'json_object' },
        messages: [{ role: 'system', content: sys }, { role: 'user', content: String(q) }] }) });
    if (!resp.ok) return null;
    const data = await resp.json();
    j = JSON.parse(data.choices?.[0]?.message?.content || '{}');
  } catch { return null; }
  const reply = clip250(j.reply);
  if (!reply) return null;
  const conf = Math.max(0, Math.min(1, Number(j.confidence) || 0.6));
  const r = { query: q, tier: 'remote', action: 'answer', intent: 'teacher', confidence: Math.round(conf * 100), state: 'idle', reply, learned: false, verified: false };
  // KNOWLEDGE is persisted ONLY if it verifies against a trusted source. We then store the SOURCE's
  // text (never the LLM's unchecked prose) AND show it, so shown == stored == trusted. Identity = the
  // Wikipedia canonical title (robust dedup). Unverifiable / personal -> answered, but NEVER saved.
  if (j.kind === 'knowledge' && j.topic) {
    const v = await wikiVerify(String(j.topic), en);
    if (v) {
      r.reply = clip250(v.extract);                  // trusted text replaces the LLM prose
      r.verified = true; r.intent = 'teacher+wiki';
      // Coherence: the verified topic must actually relate to the question (catches topic drift,
      // e.g. "perché il cielo è blu" -> topic "Cielo" that talks about the sky but not the why).
      // Title+extract give the content tokens to compare against. Incoherent -> answer, but DON'T save.
      if (coherent(q, `${v.title} ${v.extract}`)) {
        r.learned = await learnCard(lang, v.title, v.extract,
          [String(j.topic), q, ...(Array.isArray(j.ask) ? j.ask : [])],
          'wikipedia:' + (en ? 'en' : 'it') + ':' + v.title, classifyDesc(v.desc));
      }
    }
  }
  return r;
}

// ANIMA L0 mock — mirrors firmware/components/nucleo_anima (docs/anima.md). Lets the web
// app be previewed without the device. Keep the intents in sync with nucleo_anima.c.
function animaQuery(input, lang, mem) {
  lang = (lang === 'en') ? 'en' : 'it';   // understands all, replies in the chosen language
  mem = mem || { last_app: '', last_file: '' };
  if (!mem.clarify_opt) mem.clarify_opt = ['', ''];
  const APP_ALIAS = APP_ALIASES;   // shared with the firmware via registry/app-aliases.json
  const INTENTS = [
    { id: 'open_app', action: 'launch', kw: ['apri', 'aprire', 'avvia', 'lancia', 'esegui', 'vai', 'mostra', 'mostrami', 'vedere', 'vedi', 'guarda', 'guardare', 'vorrei', 'voglio', 'portami', 'fammi', 'metti', 'riproduci', 'suona', 'ascolta', 'accedi', 'entra', 'open', 'show', 'launch', 'run', 'play', 'start'] },
    { id: 'time', action: 'system', arg: 'time', reply: { it: '{value}.', en: '{value}.' }, kw: ['ora', 'ore', 'orario', 'adesso', 'time', 'clock'] },
    { id: 'storage', action: 'system', arg: 'storage', reply: { it: 'Spazio SD: {value}.', en: 'SD space: {value}.' }, kw: ['spazio', 'disco', 'scheda', 'archiviazione', 'space', 'storage', 'sd', 'capacita', 'gigabyte'] },  // no bare "memoria" (RAM/alloc -> L1; RAM-free -> isRam)
    { id: 'battery', action: 'system', arg: 'battery', reply: { it: 'Batteria: {value}.', en: 'Battery: {value}.' }, kw: ['batteria', 'carica', 'autonomia', 'energia', 'battery'] },
    { id: 'date', action: 'system', arg: 'date', reply: { it: '{value}.', en: '{value}.' }, kw: ['data', 'oggi', 'giorno', 'date'] },
    { id: 'year', action: 'system', arg: 'year', reply: { it: 'Siamo nel {value}.', en: "It's {value}." }, kw: ['anno', 'year', 'annata'] },
    { id: 'season', action: 'system', arg: 'season', reply: { it: 'Siamo in {value}.', en: "It's {value}." }, kw: ['stagione', 'season'] },
    { id: 'version', action: 'system', arg: 'version', reply: { it: 'Eseguo {value}.', en: 'Running {value}.' }, kw: ['versione', 'version'] },   // no bare "firmware" (definition -> L1)
    { id: 'uptime', action: 'system', arg: 'uptime', reply: { it: 'Acceso da {value}.', en: 'Up for {value}.' }, kw: ['uptime', 'acceso', 'accesa', 'avvio'] },
    { id: 'whoami', action: 'answer', kw: ['chi', 'anima', 'presentati', 'who', 'you'],   // no bare "sei" (-> "sei un robot" reaches L1)
      reply: { it: "Sono ANIMA, l'assistente offline di NucleoOS. Funziono senza internet, sul dispositivo.",
               en: "I'm ANIMA, NucleoOS's offline assistant. I work with no internet, on the device." } },
    // NB no static "help" intent: "cosa sai fare" / "aiuto" / "comandi" are caught by isCapabilities()
    // and answered DYNAMICALLY (live apps + pillars), mirroring nucleo_anima.c.
  ];
  const norm = (input || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  const tokens = norm.split(/[^a-z0-9]+/).filter(Boolean);
  const match = (a, b) => a === b || (Math.min(a.length, b.length) >= 4 && (a.startsWith(b) || b.startsWith(a)));  // >=4: 3-letter kw match exactly (chi!=chiedo)
  const score = (it) => it.kw.reduce((n, k) => n + (tokens.some(t => match(k, t)) ? 1 : 0), 0);

  // Shared predicates (mirror nucleo_anima.c): follow-up "aprilo"/"open it" + app resolution.
  const FU = ['aprilo', 'aprila', 'riaprilo', 'riaprila'];
  const OPENV = ['apri', 'aprire', 'open', 'mostra', 'show', 'riapri'];
  const PRON = ['lo', 'la', 'quello', 'quella', 'esso', 'essa', 'questo', 'questa', 'it', 'that', 'this'];
  const isFollowupOpen = () => tokens.some(t => FU.includes(t)) ||
    (tokens.some(t => OPENV.some(v => match(v, t))) && tokens.some(t => PRON.includes(t)));
  const resolveApps = (cap) => { const out = [];
    for (const [id, al] of Object.entries(APP_ALIAS)) if (al.some(a => tokens.some(t => match(a, t)))) { if (!out.includes(id)) out.push(id); if (out.length >= cap) break; }
    return out; };

  // File routing (mirrors a_folder_for_ext / a_folder_from_words / a_emit_create in nucleo_anima.c).
  const folderForExt = (name) => {
    const dot = name.lastIndexOf('.');
    if (dot < 1) return 'Documents';                 // no extension -> a text note
    const ext = name.slice(dot + 1).toLowerCase();
    const M = { Documents: ['txt','md','log','json','csv','ini','cfg','yaml','yml','toml','xml','html','htm','c','h','cpp','hpp','py','sh','js','css','todo'],
                Pictures: ['jpg','jpeg','png','bmp','gif','webp'], Music: ['mp3','wav','ogg','m4a','flac'], Videos: ['mp4','webm','mov','mkv','avi'] };
    for (const [f, exts] of Object.entries(M)) if (exts.includes(ext)) return f;
    return null;                                      // unknown -> ask which folder
  };
  const folderFromWords = () => {
    for (const w of tokens) {
      if (['documenti','documento','documents','doc','testo'].includes(w)) return 'Documents';
      if (['immagini','immagine','foto','pictures','images','pics'].includes(w)) return 'Pictures';
      if (['musica','music','audio','brani'].includes(w)) return 'Music';
      if (['video','videos','filmati'].includes(w)) return 'Videos';
    }
    return null;
  };
  const emitCreate = (name, folder) => {
    if (!folder) folder = folderForExt(name);
    if (!folder) {                                    // unknown extension -> AWAITING_SLOT(folder)
      mem.pending_tool = 'create_file'; mem.pending_slot = 'folder'; mem.pending_arg = name;
      return { query: input, tier: 'command', action: 'answer', tool: 'create_file', intent: 'create_file', confidence: 70, state: 'slot', awaiting: true,
        reply: lang === 'en' ? `Which folder should ${name} go in? (Documents, Pictures, Music, Videos)` : `In quale cartella metto ${name}? (Documenti, Immagini, Musica, Video)` };
    }
    return { query: input, tier: 'command', action: 'tool', tool: 'create_file', intent: 'create_file', arg: `/data/${folder}/${name}`, state: 'tool', confidence: 90,
      reply: lang === 'en' ? `Creating ${name} in ${folder}.` : `Creo ${name} in ${folder}.` };
  };

  // FSM AWAITING_SLOT: finish a create_file whose filename/folder we asked for last turn
  // (mirrors try_pending_slot). Bails out if the user clearly asked for something else instead.
  if (mem.pending_tool === 'create_file') {
    if (isFollowupOpen()) { mem.pending_tool = ''; mem.pending_slot = ''; mem.pending_arg = ''; }
    else if (mem.pending_slot === 'folder') {        // we have the name, they're telling us where
      const folder = folderFromWords();
      if (folder) { const name = mem.pending_arg; mem.pending_tool = ''; mem.pending_slot = ''; mem.pending_arg = ''; return emitCreate(name, folder); }
      mem.pending_tool = ''; mem.pending_slot = ''; mem.pending_arg = '';   // bail
    } else {                                          // awaiting the filename
      let name = extractFilename(input);
      if (!name && tokens.length === 1) { const s = tokens[0].replace(/[^a-z0-9_-]/gi, ''); if (s) name = s.includes('.') ? s : s + '.txt'; }
      if (name) { mem.pending_tool = ''; mem.pending_slot = ''; return emitCreate(name, null); }
      mem.pending_tool = ''; mem.pending_slot = '';   // nothing usable -> drop, handle normally
    }
  }

  // Image-generation request (mirrors a_is_image_gen / tool_image_gen in nucleo_anima.c): a generation
  // VERB *and* an image NOUN together, not a question, not a note/file/event. Grounded decline + redirect to
  // Paint's Atelier — never inline generation. amatch mirrors firmware a_match (prefix, length-gap <= 2) so
  // the sim matches the device exactly ("apri painting" must NOT trigger: paint↛painting at gap 3).
  {
    const amatch = (a, b) => { if (a === b) return true; const m = Math.min(a.length, b.length);
      return m >= 4 && Math.abs(a.length - b.length) <= 2 && a.slice(0, m) === b.slice(0, m); };
    const gverb = ['genera','generare','generami','generarmi','crea','creare','creami','crearmi','disegna','disegnami','disegnarmi','disegnare','dipingi','dipingimi','dipingermi','dipingere','produci','raffigura','raffigurami','raffigurarmi','generate','create','draw','paint','render','make','produce'];
    const gnoun = ['immagine','immagini','foto','fotografia','fotografie','disegno','disegni','illustrazione','illustrazioni','ritratto','ritratti','paesaggio','quadro','dipinto','figura','icona','logo','image','images','picture','pictures','photo','photos','photograph','drawing','drawings','illustration','portrait','artwork','painting','landscape'];
    const gother = ['nota','note','file','documento','document','promemoria','reminder','evento','event','appuntamento','cartella','folder'];
    const gqword = ['come','how','cosa','what','perche','why','quando','when','dove','where','quale','which','chi','who','quanti','quanto','quante','significa','significato','spiega','spiegami','definizione','define','meaning','posso'];  // how-to/who/how-many; "che" excluded (relative); polite "puoi/potresti/can you" stay real requests
    const greq = ['disegnami','disegnarmi','dipingimi','dipingermi','dipingi','dipingere','raffigurami','raffigurarmi','raffigura'];
    const hasV = tokens.some(t => gverb.some(k => amatch(k, t)));
    const hasN = tokens.some(t => gnoun.some(k => amatch(k, t)));
    const hasO = tokens.some(t => gother.some(k => amatch(k, t)));
    const isQ = tokens.some(t => gqword.includes(t));
    const req = tokens.some(t => greq.includes(t));
    const me = tokens.includes('me');
    const dps = tokens.some(t => ['draw','paint','sketch'].some(k => amatch(k, t)));
    const hit = !hasO && !isQ && ( (hasV && hasN) || (req && tokens.length >= 2) || (dps && me && tokens.length >= 3) );
    if (hit) {
      return { query: input, tier: 'command', action: 'answer', intent: 'image_gen', state: 'idle', confidence: 80,
        reply: lang === 'en'
          ? "Images are generated in the Atelier studio inside the Paint app - it runs on your browser's GPU, not here in chat. Open Paint, type what you want (or sketch it), press Generate, and the picture is saved to your Cardputer."
          : "Le immagini si generano nello studio Atelier dell'app Paint: gira sulla GPU del browser, non qui in chat. Apri Paint, scrivi cosa vuoi (o fai uno schizzo), premi Genera e l'immagine viene salvata sul Cardputer." };
    }
  }

  // Agent loop: content clause + payload compose (mirrors a_content_clause / ag_compose).
  // mode: 0 LITERAL (copy verbatim), 1 AUTO (calc / answer-if-question / literal), 2 RETRIEVE (answer it).
  const isQuestion = (sub) => {
    const cue = ['cos', 'cosa', 'chi', 'quale', 'quali', 'quando', 'dove', 'come', 'perche', 'quanti', 'quanto',
      'spiega', 'significa', 'significato', 'definizione', 'capitale', 'what', 'who', 'which', 'when', 'where', 'how', 'why', 'define', 'meaning', 'capital'];
    const art = ['la', 'il', 'lo', 'i', 'gli', 'le', 'un', 'uno', 'una', 'l', 'the', 'a', 'an'];
    const t = sub.toLowerCase().split(/[\s']+/).filter(Boolean);    // split apostrophes too (cos'e -> cos)
    if (!t.length) return false;
    const s = art.includes(t[0]) ? 1 : 0;                           // skip one leading article; cue must be at the start
    return t.slice(s, s + 2).some(w => cue.some(c => match(c, w)));
  };
  const contentClause = (raw) => {
    const ci = raw.indexOf(':');
    if (ci >= 0 && raw.slice(ci + 1).trim()) return { head: raw.slice(0, ci), content: raw.slice(ci + 1).trim(), mode: 0 };
    const w = raw.split(/\s+/).filter(Boolean), lw = w.map(x => x.toLowerCase());
    const naming = ['nome', 'chiamato', 'chiamata', 'titolo', 'title', 'named', 'called'];
    const at = (j, mode) => (j < w.length) ? { head: w.slice(0, j).join(' '), content: w.slice(j).join(' ').trim(), mode } : null;
    for (let i = 0; i < lw.length; i++) {
      const a = lw[i], b = lw[i + 1] || '', c = lw[i + 2] || '', d = lw[i + 3] || '';
      // RETRIEVE: "con la risposta/definizione a/di/su X", "rispondendo a X", "answering X"
      if (a === 'con' && b === 'la' && ['risposta', 'definizione'].includes(c) && ['a', 'di', 'su'].includes(d)) return at(i + 4, 2);
      if (a === 'con' && ['risposta', 'definizione'].includes(b) && ['a', 'di', 'su'].includes(c)) return at(i + 3, 2);
      if (a === 'rispondendo' && b === 'a') return at(i + 2, 2);
      if (a === 'answering') return at(i + 1, 2);
      // LITERAL connectors
      if (a === 'con'  && b === 'il'  && c === 'testo') return at(i + 3, 0);
      if (a === 'with' && b === 'the' && c === 'text')  return at(i + 3, 0);
      if (a === 'con'  && ['scritto', 'testo', 'dentro'].includes(b)) return at(i + 2, 0);
      if (a === 'che'  && ['dice', 'contiene', 'recita'].includes(b)) return at(i + 2, 0);
      if (a === 'with' && ['text', 'content'].includes(b)) return at(i + 2, 0);
      if (a === 'that' && ['says', 'reads'].includes(b)) return at(i + 2, 0);
      if (['scritto', 'contenente', 'dicendo', 'saying', 'containing'].includes(a)) return at(i + 1, 0);
      // bare connectors -> AUTO
      if (a === 'con')  { if (naming.includes(b)) continue; return at(i + 1, 1); }
      if (a === 'with') return at(i + 1, 1);
    }
    return null;
  };
  const agCompose = (sub, mode) => {
    const c = tryCalc(sub);
    if (c && c.kind !== 'divzero') return { body: String(fmtNum(c.value)), step: (lang === 'en' ? `compute ${sub}=` : `calcolo ${sub}=`) + fmtNum(c.value) };
    if (mode === 2 || (mode === 1 && isQuestion(sub))) {
      const kr = animaQuery(sub, lang, mem);                          // answer from the offline brain
      if (kr && kr.tier === 'fact' && kr.reply) return { body: kr.reply, step: (lang === 'en' ? 'recall: ' : 'cerco: ') + sub.slice(0, 20) };
    }
    return { body: sub, step: lang === 'en' ? 'literal text' : 'testo letterale' };
  };

  // Tool: add_event (mirrors tool_event) — a reminder/event with WHEN + optional TIME + TEXT.
  {
    const REM = ['ricordami', 'ricorda', 'promemoria', 'reminder', 'remind'];
    const EVV = ['crea', 'creare', 'aggiungi', 'segna', 'nuovo', 'nuova', 'add', 'new', 'set', 'metti'];
    const EVN = ['evento', 'eventi', 'appuntamento', 'appuntamenti', 'impegno', 'event', 'appointment'];
    const QW  = ['come', 'cosa', 'chi', 'quando', 'dove', 'perche', 'quale', 'how', 'what', 'who', 'when', 'where', 'why', 'which',
                 'posso', 'puoi', 'potresti', 'puo', 'can', 'could', 'may'];
    const rem = tokens.some(t => REM.some(k => match(k, t)));
    const isEv = tokens.some(t => EVV.some(k => match(k, t))) && tokens.some(t => EVN.some(k => match(k, t)));
    const q = tokens.some(t => QW.includes(t));
    if (!q && (rem || isEv)) {
      const w = input.split(/\s+/).filter(Boolean), lw = w.map(x => x.toLowerCase());
      const drop = new Array(w.length).fill(false);
      let off = 0;
      for (let i = 0; i < lw.length; i++) {
        if (lw[i] === 'oggi' || lw[i] === 'today') { off = 0; drop[i] = true; }
        else if (lw[i] === 'domani' || lw[i] === 'tomorrow') { off = 1; drop[i] = true; }
        else if (lw[i] === 'dopodomani') { off = 2; drop[i] = true; }
        else if (['tra', 'fra', 'in'].includes(lw[i]) && /^\d+$/.test(lw[i + 1] || '')) {
          const d = parseInt(lw[i + 1], 10); if (d > 0 && d <= 60) { off = d; drop[i] = true; drop[i + 1] = true; if (['giorni', 'days'].includes(lw[i + 2] || '')) drop[i + 2] = true; }
        }
      }
      let hh = -1, mm = 0;
      for (let i = 0; i < lw.length; i++) if (['alle', 'ore', 'at'].includes(lw[i]) && /^\d/.test(lw[i + 1] || '')) {
        const m = lw[i + 1].match(/^(\d{1,2})(?::(\d{2}))?/); if (m) { hh = +m[1]; mm = m[2] ? +m[2] : 0; drop[i] = true; drop[i + 1] = true; break; }
      }
      if (hh < 0) for (let i = 0; i < lw.length; i++) { const m = lw[i].match(/^(\d{1,2}):(\d{2})$/); if (m) { hh = +m[1]; mm = +m[2]; drop[i] = true; break; } }
      for (let i = 0; i < lw.length; i++) if (['pomeriggio', 'sera', 'pm'].includes(lw[i])) { if (hh >= 1 && hh < 12) hh += 12; drop[i] = true; }
      if (hh > 23 || mm > 59) { hh = -1; mm = 0; }
      const LEAD = ['ricordami', 'ricorda', 'promemoria', 'reminder', 'remind', 'me', 'mi', 'crea', 'creare', 'crei', 'aggiungi', 'segna', 'nuovo', 'nuova', 'add', 'new', 'set', 'metti', 'evento', 'eventi', 'appuntamento', 'appuntamenti', 'impegno', 'event', 'appointment', 'di', 'che', 'to', 'that', 'un', 'uno', 'una', 'il', 'lo', 'la', 'the', 'a', 'per', 'of'];
      for (let i = 0; i < w.length; i++) { if (drop[i] || LEAD.includes(lw[i])) drop[i] = true; else break; }
      const text = w.filter((_, i) => !drop[i]).join(' ').trim();
      if (text) {
        const time = hh >= 0 ? `${String(hh).padStart(2, '0')}:${String(mm).padStart(2, '0')}` : '';
        const content = `off=${off};time=${time};text=${text}`;
        const when = off === 0 ? (lang === 'en' ? 'today' : 'oggi') : off === 1 ? (lang === 'en' ? 'tomorrow' : 'domani')
                   : off === 2 ? (lang === 'en' ? 'in 2 days' : 'dopodomani') : (lang === 'en' ? `in ${off} days` : `tra ${off} giorni`);
        const tlabel = time ? (lang === 'en' ? ` at ${time}` : ` alle ${time}`) : '';
        return { query: input, tier: 'command', action: 'tool', tool: 'add_event', intent: 'add_event', arg: 'add_event', state: 'tool', confidence: 86, content,
          reply: (lang === 'en' ? `Reminder "${text}" ${when}${tlabel}.` : `Promemoria "${text}" ${when}${tlabel}.`),
          trace: (lang === 'en' ? 'plan: schedule' : 'piano: pianifica') + ' > ' + (lang === 'en' ? `when=${when}${tlabel}` : `quando=${when}${tlabel}`) + ' > ' + (lang === 'en' ? 'verify: ok' : 'verifica: ok') };
      }
    }
  }

  // Tool: create_file (mirrors nucleo_anima.c) — a create verb + a file noun, then route by extension.
  const VERBS = ['crea', 'creare', 'crei', 'nuovo', 'nuova', 'new', 'create', 'make', 'scrivi', 'annota', 'appunta', 'segna', 'prepara', 'genera', 'draft', 'jot'];
  const NOUNS = ['file', 'documento', 'document', 'nota', 'note', 'testo', 'text', 'foglio', 'appunto'];
  const QWORD = ['come', 'cosa', 'perche', 'quando', 'dove', 'quale', 'how', 'what', 'why', 'when', 'where', 'which',
                 'posso', 'puoi', 'potresti', 'puo', 'can', 'could', 'may'];
  if (tokens.some(t => VERBS.some(v => match(v, t))) && tokens.some(t => NOUNS.some(nn => match(nn, t)))) {
    // compose-then-act: a content clause -> compute/literal the payload, self-verify, write WITH it
    const cc = contentClause(input);
    if (cc && cc.content) {
      let name = extractFilename(cc.head) || (lang === 'en' ? 'note.txt' : 'nota.txt');
      const comp = agCompose(cc.content, cc.mode);
      if (comp.body) {
        const r0 = emitCreate(name, null);
        r0.content = comp.body;
        r0.trace = (lang === 'en' ? 'plan: compose+write' : 'piano: componi+scrivi') + ' > ' + comp.step + ' > ' + (lang === 'en' ? 'verify: ok' : 'verifica: ok');
        if (r0.action === 'tool') r0.reply = (lang === 'en' ? `Creating ${name} with: ` : `Creo ${name} con: `) + comp.body.slice(0, 40);
        return r0;
      }
    }
    const name = extractFilename(input);
    if (name) return emitCreate(name, null);
    // No filename AND a how-to question ("come si crea un file?") -> knowledge, not a create command.
    if (!QWORD.some(qw => tokens.includes(qw))) {
      mem.pending_tool = 'create_file'; mem.pending_slot = 'filename';   // arm FSM AWAITING_SLOT
      return { query: input, tier: 'command', action: 'answer', tool: 'create_file', intent: 'create_file', confidence: 70, state: 'slot', awaiting: true,
        reply: lang === 'en' ? 'What should the file be called? E.g. "create a file note.txt".' : 'Come vuoi chiamare il file? Es: "crea un file note.txt".' };
    }
  }

  // Tool: device settings (mirrors tool_setting) — volume / brightness, absolute "a 55" or relative.
  {
    let target = 0;
    for (const t of tokens) {
      if (['volume', 'audio', 'suono'].some(k => match(k, t))) target = 1;
      if (['luminosita', 'brightness', 'schermo', 'luce'].some(k => match(k, t))) target = 2;
    }
    if (target) {
      let dir = 0, ctrl = false;
      for (const t of tokens) {
        if (['alza', 'aumenta', 'raise', 'increase'].some(k => match(k, t))) { dir = 1; ctrl = true; }
        if (['abbassa', 'diminuisci', 'riduci', 'lower', 'decrease'].some(k => match(k, t))) { dir = -1; ctrl = true; }
        if (['imposta', 'metti', 'set', 'porta', 'regola'].some(k => match(k, t))) ctrl = true;
      }
      const mNum = input.match(/\d+/);
      const val = mNum ? Math.min(100, parseInt(mNum[0], 10)) : -1;
      const arg = val >= 0 ? String(val) : (dir ? (dir > 0 ? '+10' : '-10') : null);
      if (ctrl && arg !== null) {
        const intent = target === 1 ? 'set_volume' : 'set_brightness';
        const what = target === 1 ? 'volume' : (lang === 'en' ? 'brightness' : 'luminosita');
        const art = target === 1 ? 'il' : 'la';
        const reply = lang === 'en'
          ? (val >= 0 ? `Setting the ${what} to ${val}%.` : (dir > 0 ? `Raising the ${what}.` : `Lowering the ${what}.`))
          : (val >= 0 ? `Imposto ${art} ${what} al ${val}%.` : (dir > 0 ? `Alzo ${art} ${what}.` : `Abbasso ${art} ${what}.`));
        return { query: input, tier: 'command', action: 'tool', tool: intent, intent, arg, state: 'tool', confidence: 88, reply,
          trace: lang === 'en' ? 'tool: device setting' : 'tool: impostazione' };
      }
    }
  }

  // Tool: solver (units / percent / powers / Ohm) — mirrors anima_solve, checked before calc.
  const solved = trySolve(input, lang);
  if (solved) return { query: input, tier: 'command', action: 'answer', intent: solved.intent, confidence: solved.conf, state: 'tool', reply: solved.reply };

  // Tool: calc (mirrors nucleo_anima.c) — evaluated before retrieval.
  const calc = tryCalc(input);
  if (calc) {
    const reply = calc.kind === 'divzero'
      ? (lang === 'en' ? "I can't divide by zero." : 'Non posso dividere per zero.')
      : (lang === 'en' ? `It's ${fmtRound(calc.value)}.` : `Fa ${fmtRound(calc.value)}.`);   // calcolo base -> max 4 dec (mirror C)
    return { query: input, tier: 'command', action: 'answer', intent: 'calc', confidence: 95, state: 'tool', reply };
  }

  // Computed-from-state, executor-filled (mirror a_is_agenda / a_is_capabilities + the httpd
  // executor in nucleo_httpd.c): today's calendar agenda and the DYNAMIC capabilities answer.
  // Checked before the intent table so "cosa devo fare oggi" beats the date intent ("oggi").
  const isAgenda = () => {   // "che impegni ho oggi", "cosa devo fare oggi", "chi devo vedere oggi"
    const nouns = ['impegni','impegno','appuntamenti','appuntamento','agenda','eventi','scadenze','scadenza','appointments','schedule'];
    let devo = false, fv = false;
    for (const t of tokens) {
      if (nouns.some(nn => match(nn, t))) return true;
      if (t === 'devo') devo = true;
      if (t === 'fare' || t === 'vedere' || t === 'ricordare') fv = true;
    }
    return devo && fv;
  };
  const isCapabilities = () => {   // "cosa sai fare", "che puoi fare", "aiuto", "comandi"
    const kw = ['aiuto','help','comandi','funzioni','capacita','elenca'];
    let sp = false, fare = false;
    for (const t of tokens) {
      if (kw.some(k => match(k, t))) return true;
      if (t === 'sai' || t === 'puoi') sp = true;
      if (t === 'fare' || t === 'fai') fare = true;
    }
    return sp && fare;
  };
  // Read today's appointments from the OS calendar (tools/sd-sim/system/config/calendar.json,
  // { events: { "YYYY-MM-DD": [ {time,text,id} ] } }) and filter by today's date.
  const agendaValue = () => {
    const en = lang === 'en';
    let value = en ? 'you have no events today' : 'oggi non hai impegni';
    try {
      const d = new Date();
      const key = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
      const cal = JSON.parse(readFileSync(join(SD, 'system', 'config', 'calendar.json'), 'utf8'));
      const today = (cal && cal.events && cal.events[key]) || [];
      if (today.length) {
        const list = today.map((e) => `${e.time || ''}${e.time ? ' ' : ''}${e.text || ''}`).join('; ');
        value = en ? `you have ${today.length} today: ${list}`
                   : `oggi hai ${today.length} ${today.length === 1 ? 'impegno' : 'impegni'}: ${list}`;
      }
    } catch {}
    return value;
  };
  // DYNAMIC "what can I do": the live app list (from the registry) + the pillars — not a hardcoded list.
  const capabilitiesValue = () => {
    let ids = Object.keys(APP_ALIAS);   // fallback: the mock's app vocabulary
    try { const reg = JSON.parse(readFileSync(join(REPO, 'registry', 'apps.json'), 'utf8'));
      if (Array.isArray(reg.installed) && reg.installed.length) ids = reg.installed.map((a) => a.id); } catch {}
    const n = ids.length, applist = ids.slice(0, 5).join(', ');
    return lang === 'en'
      ? `I can open your ${n} apps (${applist}...), tell time/date/season/space, read today's calendar, do math with units and Ohm's law, create files, and answer about NucleoOS, shell commands, C, electronics and general topics — and I fix typos and ask when unsure`
      : `Posso aprire le tue ${n} app (${applist}...), dirti ora/data/stagione/spazio, leggere i tuoi impegni di oggi, fare calcoli con unita e legge di Ohm, creare file, e rispondere su NucleoOS, comandi shell, C, elettronica e cultura generale — e correggo i refusi e chiedo quando non sono sicura`;
  };
  // Network status — "sei connesso?", "che IP hai?", "qual è la mia rete?". Dodges "cos'è il
  // wifi" (definition -> L1): a topic word alone never fires, a strong/state signal is required.
  const isNetwork = () => {
    const strong = ['connesso','connessi','connessa','connessione','collegato','collegata','collegati','online','connected','ip'];
    const net = ['wifi','rete','internet','network'];
    const state = ['sei','sono','siamo','mia','mio','qual','quale','che','uso','usi','sto','attuale'];
    const def = ['cos','cosa','spiega','significa','vuol','definizione','differenza','funziona','serve'];
    let s = false, nt = false, st = false, d = false;
    for (const t of tokens) {
      if (def.some(k => match(k, t))) d = true;
      if (strong.some(k => match(k, t))) s = true;
      if (net.some(k => match(k, t))) nt = true;
      if (state.some(k => match(k, t))) st = true;
    }
    if (d) return false;
    return s || (nt && st);
  };
  // RAM status — "quanta RAM libera?", "memoria disponibile?". Dodges "alloco memoria"/"cos'è la
  // ram" (-> L1): a memory word needs an availability signal and no definition/allocation word.
  const isRam = () => {
    const mem = ['ram','memoria'];
    const avail = ['libera','libero','liberi','disponibile','disponibili','occupata','occupato','usata','resta','rimane','quanta','free'];
    const def = ['cos','cosa','spiega','significa','differenza','alloco','alloca','allocare','allocazione','dinamica','heap','stack','serve'];
    let m = false, a = false, d = false;
    for (const t of tokens) {
      if (def.some(k => match(k, t))) d = true;
      if (mem.some(k => match(k, t))) m = true;
      if (avail.some(k => match(k, t))) a = true;
    }
    if (d) return false;
    return m && a;
  };
  // Live values (mirror the httpd executor; the sim's apiStatus stands in for the device runtime).
  const networkValue = () => {
    const en = lang === 'en', net = apiStatus.network || {};
    if (net.mode === 'ap') return en ? `I'm a Wi-Fi hotspot "${net.ssid}", IP ${net.ip}` : `Sono un hotspot Wi-Fi "${net.ssid}", IP ${net.ip}`;
    if (net.ssid) return en ? `connected to "${net.ssid}", IP ${net.ip}` : `connesso a "${net.ssid}", IP ${net.ip}`;
    return en ? 'not connected' : 'non connesso';
  };
  const ramValue = () => {
    const kb = Math.round((apiStatus.free_heap || 0) / 1024);
    return lang === 'en' ? `${kb} KB of RAM free` : `${kb} KB di RAM liberi`;
  };
  if (isAgenda())
    return { query: input, tier: 'command', action: 'system', intent: 'agenda', arg: 'agenda', confidence: 85, state: 'tool', reply: agendaValue() };
  if (isCapabilities())
    return { query: input, tier: 'command', action: 'system', intent: 'capabilities', arg: 'capabilities', confidence: 80, state: 'tool', reply: capabilitiesValue() };
  if (isNetwork())
    return { query: input, tier: 'command', action: 'system', intent: 'network', arg: 'network', confidence: 82, state: 'tool', reply: networkValue() };
  if (isRam())
    return { query: input, tier: 'command', action: 'system', intent: 'ram', arg: 'ram', confidence: 82, state: 'tool', reply: ramValue() };

  // Drill-down: "dimmi di più"/"tell me more" (mirrors a_is_more_request). The mock is L0-only
  // (knowledge + detail live in the device AKB3), so here it just answers honestly.
  const SOLO = ['approfondisci', 'approfondire', 'dettagli', 'dettaglio', 'elaborate', 'esempio', 'example', 'continua'];
  const MOREW = ['piu', 'more'], VERBW = ['dimmi', 'dammi', 'raccontami', 'dicci', 'sai', 'spiegami', 'fammi', 'tell', 'give', 'show', 'explain', 'voglio'];
  if (tokens.some(t => SOLO.includes(t)) || (tokens.some(t => MOREW.includes(t)) && tokens.some(t => VERBW.some(v => match(v, t))))) {
    if (mem.last_topic) return { query: input, tier: 'command', action: 'answer', intent: 'more', confidence: 60, state: 'followup',
      reply: lang === 'en' ? "That's all I have on that." : 'Su questo non ho altri dettagli.' };
    // no remembered topic -> fall through to normal handling
  }

  // Follow-up: "aprilo"/"open it" -> reopen last file/app from memory (mirrors firmware).
  if (isFollowupOpen()) {
    const useFile = mem.last_file && (mem.last_kind === 'f' || !mem.last_app);   // recency, else fall back
    if (useFile) { const bn = mem.last_file.split('/').pop(); return { query: input, tier: 'command', action: 'tool', tool: 'open_file', intent: 'open_file', arg: mem.last_file, state: 'followup', reply: (lang === 'en' ? 'Opening ' : 'Apro ') + bn + '.', confidence: 80, memory: true }; }
    if (mem.last_app) { return { query: input, tier: 'command', action: 'launch', intent: 'open_app', arg: mem.last_app, state: 'followup', reply: (lang === 'en' ? 'Opening ' : 'Apro ') + mem.last_app + '.', confidence: 80, memory: true }; }
  }

  // FSM CLARIFY resolution: an ordinal ("il primo") or the app's name picks one of the two we
  // offered last turn — the runner-up signal we'd normally discard (mirrors firmware).
  if (mem.clarify_opt && mem.clarify_opt[0]) {
    let pick = -1;
    for (const t of tokens) {
      if (['primo', 'prima', 'first', 'uno', 'one'].includes(t)) pick = 0;
      if (['secondo', 'seconda', 'second', 'due', 'two'].includes(t)) pick = 1;
    }
    if (pick < 0) { const aps = resolveApps(1); if (aps[0]) { const i = mem.clarify_opt.indexOf(aps[0]); if (i >= 0) pick = i; } }
    if (pick >= 0) { const id = mem.clarify_opt[pick]; mem.clarify_opt = ['', ''];
      return { query: input, tier: 'command', action: 'launch', intent: 'open_app', arg: id, state: 'clarify', memory: true, confidence: 85, reply: (lang === 'en' ? 'Opening ' : 'Apro ') + id + '.' }; }
    mem.clarify_opt = ['', ''];   // not a resolution -> drop, handle normally
  }

  // whoami keys on bare "chi"/"who" -> guard to genuine self-questions so "chi è <entity>"
  // falls through to the knowledge tier instead of replying "Sono ANIMA" (mirrors a_whoami_self).
  const WHOAMI_SELF = ['sei', 'siete', 'tu', 'te', 'ti', 'tuo', 'tua', 'presentati', 'anima', 'chiami', 'you', 'your', 'yourself'];
  const whoamiSelf = () => tokens.some(t => WHOAMI_SELF.includes(t));
  let best = null, bs = 0, ss = 0;
  for (const it of INTENTS) { let s = score(it); if (s > 0 && it.id === 'whoami' && !whoamiSelf()) s = 0; if (s > bs) { ss = bs; bs = s; best = it; } else if (s > ss) ss = s; }
  const none = { query: input, tier: 'none', action: 'none', arg: '', reply: '', confidence: 0, state: 'idle' };
  if (!best || bs === 0) return none;

  let conf = 45 + 20 * bs; if (bs === ss) conf -= 25; if (conf > 100) conf = 100;
  let arg = best.arg || '';
  if (best.action === 'launch') {
    const apps = resolveApps(2);
    if (apps.length === 0) return none;
    if (apps.length >= 2) {   // ambiguous -> ask instead of guessing (uncertainty policy)
      mem.clarify_opt = [apps[0], apps[1]];
      return { query: input, tier: 'command', action: 'answer', intent: 'clarify', state: 'clarify', awaiting: true, confidence: 60,
        reply: lang === 'en' ? `Which one — ${apps[0]} or ${apps[1]}?` : `Quale dei due apro, ${apps[0]} o ${apps[1]}?` };
    }
    arg = apps[0];
    conf = Math.min(100, conf + 10);
  }
  if (conf < 55) return none;

  let reply = (best.reply && best.reply[lang]) || '';
  if (best.action === 'launch') reply = lang === 'en' ? `Opening ${arg}.` : `Apro ${arg}.`;
  else if (best.action === 'system') {
    let value = lang === 'en' ? 'unavailable' : 'non disponibile';
    if (arg === 'time') {   // ora PARLABILE esatta al minuto (mirror di nucleo_tts_speak_time): niente "HH:MM"
      const d = new Date(), h = d.getHours(), m = d.getMinutes();
      if (lang === 'en') {
        value = m === 0 ? (h === 0 ? 'It is midnight' : h === 12 ? 'It is noon' : `It is ${h} o'clock`) : `It is ${h} ${m}`;
      } else {
        const name = (hh) => hh === 0 ? 'Mezzanotte' : hh === 12 ? 'Mezzogiorno' : `Sono le ${hh}`;
        if (m === 45)      value = `${name((h + 1) % 24)} meno un quarto`;
        else if (m === 0)  value = (h === 0 || h === 12) ? name(h) : `${name(h)} in punto`;
        else if (m === 15) value = `${name(h)} e un quarto`;
        else if (m === 30) value = `${name(h)} e mezza`;
        else               value = `${name(h)} e ${m}`;
      }
    }
    else if (arg === 'storage') value = lang === 'en' ? '57.0 GB free of 58.1 GB' : '57.0 GB liberi su 58.1 GB';
    else if (arg === 'date' || arg === 'year' || arg === 'season') {   // computed-from-state (mirrors httpd)
      const d = new Date(), en = lang === 'en';
      const WD = en ? ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'] : ['domenica','lunedi','martedi','mercoledi','giovedi','venerdi','sabato'];
      const MO = en ? ['January','February','March','April','May','June','July','August','September','October','November','December'] : ['gennaio','febbraio','marzo','aprile','maggio','giugno','luglio','agosto','settembre','ottobre','novembre','dicembre'];
      const SE = en ? ['winter','spring','summer','autumn'] : ['inverno','primavera','estate','autunno'];
      const mo = d.getMonth();
      if (arg === 'year') value = String(d.getFullYear());
      else if (arg === 'season') { const day = d.getDate();   // astronomical boundaries (~20-22), not the 1st
        value = SE[((mo===2&&day>=20)||mo===3||mo===4||(mo===5&&day<=20)) ? 1 : ((mo===5&&day>=21)||mo===6||mo===7||(mo===8&&day<=22)) ? 2 : ((mo===8&&day>=23)||mo===9||mo===10||(mo===11&&day<=20)) ? 3 : 0]; }
      else value = en ? `Today is ${WD[d.getDay()]}, ${MO[mo]} ${d.getDate()} ${d.getFullYear()}` : `Oggi e ${WD[d.getDay()]} ${d.getDate()} ${MO[mo]} ${d.getFullYear()}`;
    }
    else if (arg === 'version') value = 'NucleoOS 0.1.0';
    else if (arg === 'uptime') {                        // process uptime stands in for the device's
      const s = Math.floor(process.uptime()), dd = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), mm = Math.floor((s % 3600) / 60);
      value = dd ? `${dd}${lang === 'en' ? 'd' : 'g'} ${hh}h` : hh ? `${hh}h ${mm}m` : `${mm}m`;
    }
    reply = reply.replace('{value}', value);
  }
  return { query: input, tier: 'command', action: best.action, intent: best.id, arg, reply, confidence: conf, state: 'idle' };
}

server.listen(PORT, () => console.log(`NucleoOS device simulator on http://localhost:${PORT}`));
