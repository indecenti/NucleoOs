// NucleoOS Video Studio — a local companion that converts ordinary videos into the device's
// .nfv format (MJPEG 240x136 + sibling mp3) and delivers them straight onto the Cardputer's
// microSD. The Cardputer can't run ffmpeg, so the heavy lifting happens here on the PC with
// NATIVE ffmpeg (+ optional NVIDIA GPU decode), exactly the "client does the heavy lifting"
// principle in docs/media.md. Big films convert in minutes and are written directly to the
// SD — no flaky multi-hundred-MB WiFi upload.
//
//   node tools/nfv/studio.mjs            ->  http://localhost:5577
//
// Zero dependencies (Node 18+). Drag-drop a file (it streams here) OR paste a path on disk
// (no copy, ideal for big local films). Pick a profile + target SD and it converts, shows
// live progress, handles errors (bad/again-unsupported files, ffmpeg failures, full/absent
// SD) and drops the result into <SD>\data\Videos.
import { createServer, request as httpRequest } from 'node:http';
import { spawn, execFile } from 'node:child_process';
import { createWriteStream, createReadStream, promises as fsp, openSync, writeSync, closeSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, basename, extname, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const PORT = 5577;
const SCREEN_W = 240, SCREEN_H = 136;       // even height for 4:2:0 MJPEG; panel clips last row
const SOI = Buffer.from([0xff, 0xd8]), EOI = Buffer.from([0xff, 0xd9]);
const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const LOCAL_OUT = join(REPO, 'tools', 'nfv', 'out');
const TMP = join(tmpdir(), 'nucleo-studio');

const PROFILES = {
  compat:   { fps: 12, q: 8, ar: 22050, ab: '32k' },
  balanced: { fps: 20, q: 7, ar: 22050, ab: '40k' },
  quality:  { fps: 24, q: 5, ar: 24000, ab: '48k' },
};

// ---------- small helpers ----------
const sh = (cmd, args) => new Promise((res) => {
  execFile(cmd, args, { maxBuffer: 1 << 20 }, (err, stdout, stderr) => res({ err, stdout, stderr }));
});
const has = async (bin) => !(await sh(bin, ['-version'])).err;
const human = (n) => { for (const u of ['B', 'KB', 'MB', 'GB']) { if (n < 1024 || u === 'GB') return `${n.toFixed(1)} ${u}`; n /= 1024; } };
const mmss = (s) => `${(s / 60) | 0}:${String(Math.round(s % 60)).padStart(2, '0')}`;

// ---------- device (Wi-Fi delivery, optional) ----------
async function deviceCfg() {
  try {
    const j = JSON.parse((await fsp.readFile(join(REPO, 'tools', 'release.local.json'), 'utf8')).replace(/^﻿/, ''));
    if (j.host) return { host: /^https?:\/\//.test(j.host) ? j.host : 'http://' + j.host, pin: String(j.pin || '') };
  } catch {}
  return null;
}
function devReq(host, path, { method = 'GET', headers = {}, body = null, timeout = 8000 } = {}) {
  return new Promise((resolve, reject) => {
    const u = new URL(host + path);
    const req = httpRequest({ hostname: u.hostname, port: u.port || 80, path: u.pathname + u.search, method, headers, timeout },
      (res) => { let d = ''; res.on('data', (c) => (d += c)); res.on('end', () => resolve({ status: res.statusCode, body: d, headers: res.headers })); });
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    if (body && body.pipe) body.pipe(req); else { if (body) req.write(body); req.end(); }
  });
}
async function deviceProbe() {
  const cfg = await deviceCfg(); if (!cfg) return null;
  try { const r = await devReq(cfg.host, '/api/status', { timeout: 4000 }); if (r.status === 200) { const s = JSON.parse(r.body); return { ...cfg, online: true, free: s.storage?.free_bytes || 0, ver: s.version }; } } catch {}
  return { ...cfg, online: false };
}
// Stream a file to the device with pairing + backpressure (gentler than a single buffered POST).
async function devicePush(cfg, localPath, devPath, onPct) {
  let cookie = null;
  const st = await devReq(cfg.host, '/api/auth/status').then((r) => JSON.parse(r.body)).catch(() => null);
  if (st && st.required && !st.paired) {
    if (!cfg.pin) throw new Error('device needs pairing — set "pin" in tools/release.local.json');
    const pr = await devReq(cfg.host, '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: cfg.pin }) });
    if (pr.status !== 200) throw new Error('pairing rejected (check PIN)');
    const m = /(nucleo_session=[^;]+)/.exec(pr.headers['set-cookie']?.[0] || '');
    cookie = m ? m[1] : null;
    if (!cookie) throw new Error('no session cookie from device');
  }
  const total = (await fsp.stat(localPath)).size;
  let sent = 0;
  const rs = createReadStream(localPath);
  rs.on('data', (c) => { sent += c.length; onPct && onPct(Math.min(99, Math.round(sent / total * 100))); });
  const headers = { 'content-type': 'application/octet-stream', 'content-length': total };
  if (cookie) headers.cookie = cookie;
  const r = await devReq(cfg.host, '/api/fs/write?path=' + encodeURIComponent(devPath), { method: 'POST', headers, body: rs, timeout: 0 });
  if (r.status !== 200) throw new Error('device write failed (HTTP ' + r.status + ') — large files often drop over Wi-Fi; use SD');
}

// NucleoOS SD cards have a /data/Videos tree — scan drive letters for it (+ always offer the
// repo's local out/ folder as a fallback "save to PC" target).
async function detectTargets() {
  const out = [];
  if (process.platform === 'win32') {
    for (const L of 'DEFGHIJKLMNOPQRSTUVWXYZ') {
      const p = `${L}:\\data\\Videos`;
      if (existsSync(p)) {
        let free = 0; try { const s = await fsp.statfs(`${L}:\\`); free = s.bavail * s.bsize; } catch {}
        out.push({ id: `${L}:`, label: `SD ${L}:\\ (${free ? human(free) + ' free' : 'removable'})`, dir: p, kind: 'sd' });
      }
    }
  } else {
    for (const base of ['/media', '/mnt', '/run/media']) {
      try { for (const e of await fsp.readdir(base)) { const p = join(base, e, 'data', 'Videos'); if (existsSync(p)) out.push({ id: p, label: `SD ${e}`, dir: p, kind: 'sd' }); } } catch {}
    }
  }
  out.push({ id: 'local', label: `PC folder (tools/nfv/out)`, dir: LOCAL_OUT, kind: 'local' });
  return out;
}

async function detectEnv() {
  const [ffmpeg, ffprobe, targets, dev] = await Promise.all([has('ffmpeg'), has('ffprobe'), detectTargets(), deviceProbe()]);
  let gpu = false;
  if (ffmpeg) { const r = await sh('ffmpeg', ['-hide_banner', '-hwaccels']); gpu = /cuda/.test(r.stdout || ''); }
  if (dev && dev.online) targets.unshift({ id: 'device', kind: 'device', host: dev.host, pin: dev.pin,
    label: `Cardputer ${dev.host.replace(/^https?:\/\//, '')} (${human(dev.free)} free)` });
  return { ffmpeg, ffprobe, gpu, device: dev, targets };
}

// Probe a source: must have a video stream; returns duration + dimensions.
async function probe(path) {
  const r = await sh('ffprobe', ['-v', 'error', '-show_entries', 'format=duration:stream=codec_type,codec_name,width,height', '-of', 'json', path]);
  if (r.err) throw new Error('ffprobe could not read this file (is it a video?)');
  let j; try { j = JSON.parse(r.stdout); } catch { throw new Error('ffprobe returned no info'); }
  const v = (j.streams || []).find((s) => s.codec_type === 'video');
  if (!v) throw new Error('No video stream found — unsupported or audio-only file');
  return { duration: parseFloat(j.format?.duration || '0') || 0, w: v.width, h: v.height, vcodec: v.codec_name };
}

function vf(fit, fps) {
  const fit_s = fit === 'pad'
    ? `scale=${SCREEN_W}:${SCREEN_H}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${SCREEN_W}:${SCREEN_H}:(ow-iw)/2:(oh-ih)/2:color=black`
    : `scale=${SCREEN_W}:${SCREEN_H}:force_original_aspect_ratio=increase:flags=lanczos,crop=${SCREEN_W}:${SCREEN_H}`;
  return `fps=${fps},${fit_s}`;
}

// ---------- jobs ----------
let JOB_SEQ = 0;
const JOBS = new Map();
const sse = new Set();
function broadcast() {
  const data = `data: ${JSON.stringify({ jobs: [...JOBS.values()].map(publicJob) })}\n\n`;
  for (const res of sse) { try { res.write(data); } catch {} }
}
const publicJob = (j) => ({ id: j.id, name: j.name, status: j.status, pct: j.pct, stage: j.stage, error: j.error,
  out: j.out, sizeMB: j.sizeMB, target: j.targetLabel, profile: j.profile, fps: j.fps, src: j.src, canRetry: !!j.retryable });
function setJob(j, patch) { Object.assign(j, patch); broadcast(); }

// Stream a concatenated-MJPEG pipe into an .nfv, patching the header at the end. Backpressure
// aware so a multi-hundred-MB film never blooms in RAM.
function makeNfvWriter(outPath, fps, hasAudio) {
  const fd = openSync(outPath, 'w');
  writeSync(fd, Buffer.alloc(32));                 // placeholder header
  const stride = Math.max(1, Math.round(fps * 3)); // sparse seek index every ~3 s
  let count = 0, maxFrame = 0, pos = 32;
  const offsets = [];
  return {
    frame(buf) {
      const e = buf.lastIndexOf(0xd9);
      if (e > 0 && buf[e - 1] === 0xff) buf = buf.subarray(0, e + 1);
      if (count % stride === 0) offsets.push(pos);
      const sz = Buffer.alloc(4); sz.writeUInt32LE(buf.length, 0);
      writeSync(fd, sz); writeSync(fd, buf);
      count++; pos += 4 + buf.length; if (buf.length > maxFrame) maxFrame = buf.length;
    },
    close() {
      const idxOff = pos;
      const idx = Buffer.alloc(4 + offsets.length * 4);
      idx.writeUInt32LE(offsets.length, 0);
      offsets.forEach((o, i) => idx.writeUInt32LE(o, 4 + i * 4));
      writeSync(fd, idx);
      const h = Buffer.alloc(32);
      h.write('NFV1', 0, 'ascii'); h.writeUInt8(2, 4); h.writeUInt8((hasAudio ? 1 : 0) | 2, 5);
      h.writeUInt16LE(SCREEN_W, 6); h.writeUInt16LE(SCREEN_H, 8); h.writeUInt16LE(fps, 10); h.writeUInt16LE(stride, 12);
      h.writeUInt32LE(count, 14); h.writeUInt32LE(Math.round(count * 1000 / fps), 18); h.writeUInt32LE(maxFrame, 22);
      h.writeUInt32LE(idxOff, 26);
      writeSync(fd, h, 0, 32, 0); closeSync(fd);
      return { count, maxFrame };
    },
  };
}

// Already-an-.nfv input: don't re-transcode — just make sure it carries a seek index so the
// device skips the slow on-device "Preparing" scan. Walks the [u32 size][JPEG] chain (fast on a
// PC), copies the clip to outPath, appends the sparse offset table at EOF and patches the header
// to v2. A clip that already has the index is copied through untouched.
async function reindexNfvToTmp(inPath, outPath) {
  const inH = await fsp.open(inPath, 'r');
  try {
    const hdr = Buffer.alloc(32);
    await inH.read(hdr, 0, 32, 0);
    if (hdr.toString('ascii', 0, 4) !== 'NFV1') throw new Error('not an NFV1 clip');
    const flags = hdr.readUInt8(5);
    const W = hdr.readUInt16LE(6), H = hdr.readUInt16LE(8), fps = hdr.readUInt16LE(10);
    const frames = hdr.readUInt32LE(14);
    if ((flags & 2) && hdr.readUInt32LE(26)) {           // already indexed -> pass through
      await fsp.copyFile(inPath, outPath);
      return { w: W, h: H, fps, frames, indexed: false };
    }
    const stride = Math.max(1, Math.round(fps * 3));     // sparse: every ~3 s, matches encode.py
    const fsize = (await inH.stat()).size;
    const four = Buffer.alloc(4); const offsets = []; let pos = 32;
    for (let i = 0; i < frames; i++) {
      if (i % stride === 0) offsets.push(pos);
      const { bytesRead } = await inH.read(four, 0, 4, pos);
      if (bytesRead !== 4) throw new Error('clip truncated at frame ' + i);
      pos += 4 + four.readUInt32LE(0);
      if (pos > fsize) throw new Error('frame ' + i + ' runs past EOF');
    }
    if (pos !== fsize) throw new Error(`chain ends at ${pos}, file is ${fsize} (trailing data?)`);
    await fsp.copyFile(inPath, outPath);
    const outH = await fsp.open(outPath, 'r+');
    try {
      const idx = Buffer.alloc(4 + offsets.length * 4);
      idx.writeUInt32LE(offsets.length, 0);
      offsets.forEach((o, i) => idx.writeUInt32LE(o, 4 + i * 4));
      await outH.write(idx, 0, idx.length, pos);         // append index at EOF (= pos)
      const nh = Buffer.from(hdr);                        // patch header -> v2
      nh.writeUInt8(2, 4); nh.writeUInt8(flags | 2, 5); nh.writeUInt16LE(stride, 12); nh.writeUInt32LE(pos, 26);
      await outH.write(nh, 0, 32, 0);
    } finally { await outH.close(); }
    return { w: W, h: H, fps, frames, indexed: true };
  } finally { await inH.close(); }
}

// Split a streaming MJPEG buffer into frames at SOI boundaries (FF D8). Robust across chunks.
function frameSplitter(onFrame) {
  let acc = Buffer.alloc(0), start = -1;
  return {
    feed(chunk) {
      acc = acc.length ? Buffer.concat([acc, chunk]) : chunk;
      for (;;) {
        if (start < 0) { const s = acc.indexOf(SOI); if (s < 0) { if (acc.length) acc = acc.subarray(acc.length - 1); return; } start = s; }
        const next = acc.indexOf(SOI, start + 2);
        if (next < 0) { if (start > 0) { acc = acc.subarray(start); start = 0; } return; }
        onFrame(acc.subarray(start, next)); start = next;
      }
    },
    end() { if (start >= 0 && start < acc.length) onFrame(acc.subarray(start)); },
  };
}

// Serial queue: one ffmpeg conversion at a time (parallel ffmpeg would thrash CPU/disk and
// make every job slower). Dropped/queued files wait their turn with a 'queued' badge.
const QUEUE = [];
let RUNNING = false;
function enqueue(j) { JOBS.set(j.id, j); QUEUE.push(j); broadcast(); pump(); }
async function pump() {
  if (RUNNING) return;
  const j = QUEUE.shift(); if (!j) return;
  if (j.status === 'error') return pump();                 // was cancelled while queued
  RUNNING = true;
  try { await runJob(j); } catch (e) { setJob(j, { status: 'error', error: String(e), retryable: true }); }
  RUNNING = false; pump();
}

// Deliver the finished clip (+ sibling mp3) to the device over Wi-Fi or by copy to the SD/folder,
// then clean up temps. Shared by the transcode path and the .nfv reindex passthrough.
async function deliver(j, base, nfvTmp, mp3Tmp, hasAudio) {
  const nfvSize = (await fsp.stat(nfvTmp)).size;
  const mp3Size = hasAudio ? (await fsp.stat(mp3Tmp)).size : 0;
  let out;
  if (j.targetKind === 'device') {                       // Wi-Fi push (best for clips)
    const cfg = { host: j.targetHost, pin: j.targetPin };
    setJob(j, { status: 'delivering', stage: 'Uploading to device (Wi-Fi)…', pct: 0 });
    await devicePush(cfg, nfvTmp, `/data/Videos/${base}.nfv`, (p) => setJob(j, { pct: Math.round(p * 0.85) }));
    if (hasAudio) await devicePush(cfg, mp3Tmp, `/data/Videos/${base}.mp3`, (p) => setJob(j, { pct: 85 + Math.round(p * 0.14) }));
    out = `${j.targetHost}/data/Videos/${base}.nfv`;
  } else {                                                // copy to SD / PC folder
    setJob(j, { status: 'delivering', stage: 'Copying to ' + j.targetLabel, pct: 99 });
    const destDir = j.targetDir;
    if (!existsSync(destDir)) throw new Error('Target not found (SD removed?): ' + destDir);
    try { const s = await fsp.statfs(destDir); if (s.bavail * s.bsize < nfvSize + mp3Size + (4 << 20)) throw new Error('Not enough free space on target'); } catch (e) { if (/free space/.test(e.message)) throw e; }
    out = join(destDir, `${base}.nfv`);
    await fsp.copyFile(nfvTmp, out);
    if (hasAudio) await fsp.copyFile(mp3Tmp, join(destDir, `${base}.mp3`));
  }
  await fsp.rm(nfvTmp, { force: true }); await fsp.rm(mp3Tmp, { force: true });
  if (j.input.startsWith(TMP)) await fsp.rm(j.input, { force: true });   // drop uploaded temp
  setJob(j, { status: 'done', stage: 'Ready on ' + j.targetLabel, pct: 100, out, sizeMB: +(((nfvSize + mp3Size) / 1048576).toFixed(1)) });
}

async function runJob(j) {
  try {
    // Already an .nfv? Skip transcoding — just ensure it has a seek index (reindex a legacy v1
    // clip in seconds) so the device never runs the slow on-device "Preparing" scan. Its sibling
    // .mp3 (same basename) is delivered alongside, unchanged.
    if (/\.nfv$/i.test(j.input)) {
      await fsp.mkdir(TMP, { recursive: true });
      const base = basename(j.name).replace(/\.[^.]+$/, '');
      const nfvTmp = join(TMP, `${j.id}.nfv`), mp3Tmp = join(TMP, `${j.id}.mp3`);
      setJob(j, { status: 'video', stage: 'Indexing clip', pct: 20 });
      const r = await reindexNfvToTmp(j.input, nfvTmp);
      j.src = `${r.w}x${r.h} NFV · ${r.frames} frames${r.indexed ? '' : ' (already indexed)'}`;
      const sib = j.input.replace(/\.nfv$/i, '.mp3');
      let hasAudio = false;
      if (existsSync(sib)) { await fsp.copyFile(sib, mp3Tmp); hasAudio = true; }
      setJob(j, { pct: 95 });
      await deliver(j, base, nfvTmp, mp3Tmp, hasAudio);
      return;
    }
    setJob(j, { status: 'probing', stage: 'Reading file', pct: 0 });
    const info = await probe(j.input);
    j.src = `${info.w}x${info.h} ${info.vcodec} · ${mmss(info.duration)}`;
    const prof = { ...PROFILES[j.profile] };
    if (j.fps) prof.fps = j.fps;
    j.fps = prof.fps;
    const totalFrames = Math.max(1, Math.round((info.duration || 0) * prof.fps));
    await fsp.mkdir(TMP, { recursive: true });
    const base = basename(j.name).replace(/\.[^.]+$/, '');
    const nfvTmp = join(TMP, `${j.id}.nfv`), mp3Tmp = join(TMP, `${j.id}.mp3`);

    // 1) audio -> mono mp3
    setJob(j, { status: 'audio', stage: 'Encoding audio', pct: 2 });
    const aOk = await new Promise((res) => {
      // -map_metadata -1 + -id3v2_version 0: bare MP3, no ID3 tag, so the player's CBR byte-map seek is exact.
      const a = spawn('ffmpeg', ['-y', '-i', j.input, '-vn', '-ac', '1', '-ar', String(prof.ar), '-c:a', 'libmp3lame', '-b:a', prof.ab, '-map_metadata', '-1', '-id3v2_version', '0', mp3Tmp]);
      a.on('error', () => res(false)); a.on('close', (c) => res(c === 0));
      j.proc = a;
    });
    const hasAudio = aOk && existsSync(mp3Tmp);

    // 2) video -> mjpeg pipe -> .nfv (streamed)
    setJob(j, { status: 'video', stage: 'Encoding video', pct: 5 });
    const writer = makeNfvWriter(nfvTmp, prof.fps, hasAudio);
    const hw = j.gpu ? ['-hwaccel', 'cuda'] : [];
    const v = spawn('ffmpeg', [...hw, '-i', j.input, '-an', '-vf', vf(j.fit, prof.fps), '-q:v', String(prof.q), '-c:v', 'mjpeg', '-f', 'image2pipe', 'pipe:1']);
    j.proc = v;
    const split = frameSplitter((fr) => writer.frame(Buffer.from(fr)));
    v.stdout.on('data', (chunk) => {
      split.feed(chunk);
      if (v.stdout.isPaused?.()) return;
    });
    let lastErr = '';
    v.stderr.on('data', (d) => {
      const s = d.toString(); lastErr = s.slice(-400);
      const m = /frame=\s*(\d+)/.exec(s);
      if (m) { const p = Math.min(99, Math.round((+m[1] / totalFrames) * 100)); if (p !== j.pct) setJob(j, { pct: Math.max(5, p) }); }
    });
    const vCode = await new Promise((res) => { v.on('error', () => res(-1)); v.on('close', (c) => res(c)); });
    split.end();
    const meta = writer.close();
    if (vCode !== 0 || meta.count === 0) throw new Error('Video encode failed' + (lastErr ? ': ' + lastErr.split('\n').filter(Boolean).pop() : ''));

    // 3) deliver
    await deliver(j, base, nfvTmp, mp3Tmp, hasAudio);
  } catch (e) {
    j.retryable = true;
    setJob(j, { status: 'error', error: e.message || String(e), pct: j.pct || 0 });
  } finally { j.proc = null; }
}

// ---------- HTTP ----------
function send(res, code, type, body) { res.writeHead(code, { 'content-type': type, 'access-control-allow-origin': '*' }); res.end(body); }
const json = (res, obj, code = 200) => send(res, code, 'application/json', JSON.stringify(obj));

const server = createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  const p = u.pathname;
  if (req.method === 'OPTIONS') return send(res, 204, 'text/plain', '');

  if (p === '/' ) return send(res, 200, 'text/html; charset=utf-8', UI);
  if (p === '/api/env') return json(res, await detectEnv());
  if (p === '/api/jobs' && req.method === 'GET') return json(res, { jobs: [...JOBS.values()].map(publicJob) });

  if (p === '/api/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache', connection: 'keep-alive', 'access-control-allow-origin': '*' });
    res.write(`data: ${JSON.stringify({ jobs: [...JOBS.values()].map(publicJob) })}\n\n`);
    sse.add(res); req.on('close', () => sse.delete(res));
    return;
  }

  if (p === '/api/cancel' && req.method === 'POST') {
    const j = JOBS.get(u.searchParams.get('id'));
    if (j?.proc) { try { j.proc.kill('SIGKILL'); } catch {} }
    if (j && j.status !== 'done') setJob(j, { status: 'error', error: 'Cancelled' });
    return json(res, { ok: true });
  }
  if (p === '/api/clear' && req.method === 'POST') {
    for (const [id, j] of JOBS) if (['done', 'error'].includes(j.status)) JOBS.delete(id);
    broadcast(); return json(res, { ok: true });
  }
  if (p === '/api/retry' && req.method === 'POST') {
    const old = JOBS.get(u.searchParams.get('id'));
    if (!old || !old.input || !existsSync(old.input)) return json(res, { error: 'Cannot retry (source gone)' }, 400);
    JOBS.delete(old.id);
    const id = 'job' + (++JOB_SEQ);
    enqueue({ ...old, id, status: 'queued', pct: 0, stage: 'Queued', error: null, out: null, retryable: false, proc: null });
    return json(res, { id });
  }

  // Create a job. Either streams an uploaded body to a temp file (?upload=1) or converts an
  // existing path on disk (?path=...). Common params: name, profile, fps, fit, gpu, target.
  if (p === '/api/jobs' && req.method === 'POST') {
    const env = await detectEnv();
    if (!env.ffmpeg || !env.ffprobe) return json(res, { error: 'ffmpeg/ffprobe not found in PATH' }, 400);
    const q = u.searchParams;
    const name = q.get('name') || 'video';
    const targetId = q.get('target') || (env.targets[0] && env.targets[0].id);
    const target = env.targets.find((t) => t.id === targetId) || env.targets[0];
    if (!target) return json(res, { error: 'No delivery target available' }, 400);
    const id = 'job' + (++JOB_SEQ);
    const j = { id, name, status: 'queued', pct: 0, stage: 'Queued', profile: q.get('profile') || 'balanced',
      fps: q.get('fps') ? +q.get('fps') : 0, fit: q.get('fit') === 'pad' ? 'pad' : 'crop', gpu: q.get('gpu') !== '0' && env.gpu,
      targetKind: target.kind, targetDir: target.dir, targetLabel: target.label, targetHost: target.host, targetPin: target.pin, input: null };

    if (q.get('path')) {
      const src = q.get('path').replace(/^"|"$/g, '');
      if (!existsSync(src)) return json(res, { error: 'Path not found: ' + src }, 400);
      j.input = src; json(res, { id }); enqueue(j); return;
    }
    // upload: stream body to temp
    await fsp.mkdir(TMP, { recursive: true });
    const tmpIn = join(TMP, `${id}_in${extname(name) || '.bin'}`);
    const ws = createWriteStream(tmpIn);
    req.pipe(ws);
    ws.on('finish', () => { j.input = tmpIn; json(res, { id }); enqueue(j); });
    ws.on('error', () => json(res, { error: 'upload failed' }, 500));
    return;
  }

  send(res, 404, 'text/plain', 'not found');
});

server.listen(PORT, () => {
  console.log(`\n  NucleoOS Video Studio  ->  http://localhost:${PORT}\n`);
  detectEnv().then((e) => {
    console.log(`  ffmpeg:${e.ffmpeg ? ' ok' : ' MISSING'}  gpu:${e.gpu ? ' cuda' : ' no'}  targets: ${e.targets.map((t) => t.label).join(' | ') || 'none'}`);
    if (!e.ffmpeg) console.log('  ! Install ffmpeg and put it on PATH.');
  });
});

// ---------- embedded UI ----------
const UI = /* html */ `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NucleoOS Video Studio</title>
<style>
:root{--bg:#0b1020;--panel:#121a30;--panel2:#0e1526;--line:#243250;--fg:#eaf2ff;--mut:#8aa0c8;--acc:#4ea1ff;--good:#7CFC9A;--warm:#ffd166;--bad:#ff6b6b}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(1200px 600px at 70% -10%,#16213f,#0b1020);color:var(--fg);font:14px/1.45 ui-sans-serif,system-ui,"Segoe UI",sans-serif}
.wrap{max-width:920px;margin:0 auto;padding:22px}
header{display:flex;align-items:center;gap:12px;margin-bottom:6px}
header h1{font-size:18px;margin:0;font-weight:700;letter-spacing:.2px}
header .logo{width:30px;height:30px;border-radius:8px;background:linear-gradient(135deg,#4ea1ff,#7C5CFF);display:grid;place-items:center;font-weight:800;color:#06122b}
.env{margin-left:auto;display:flex;gap:8px;font-size:12px}
.pill{padding:3px 9px;border-radius:999px;border:1px solid var(--line);color:var(--mut);background:#0e1526}
.pill.ok{color:var(--good);border-color:#244a36}.pill.bad{color:var(--bad);border-color:#4a2424}
.sub{color:var(--mut);margin:0 0 16px}
.drop{border:2px dashed #2c3c63;border-radius:14px;padding:26px;text-align:center;background:#0e1526aa;transition:.15s;cursor:pointer}
.drop.hot{border-color:var(--acc);background:#13203b}
.drop b{color:var(--fg)}.drop small{color:var(--mut)}
.or{display:flex;align-items:center;gap:10px;color:var(--mut);margin:14px 0}.or::before,.or::after{content:"";flex:1;height:1px;background:var(--line)}
.pathrow{display:flex;gap:8px}.pathrow input{flex:1;background:#0e1526;border:1px solid var(--line);color:var(--fg);border-radius:9px;padding:9px 11px;font:13px ui-monospace,monospace}
.opts{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0;align-items:center}
.opts label{color:var(--mut);font-size:12px}
select,button{background:#0e1526;border:1px solid var(--line);color:var(--fg);border-radius:9px;padding:8px 11px;font:13px system-ui}
button.primary{background:linear-gradient(135deg,#4ea1ff,#3b7fe0);border:0;color:#04122b;font-weight:700;cursor:pointer}
button:disabled{opacity:.5}
.chk{display:flex;align-items:center;gap:6px;color:var(--mut)}
.jobs{display:flex;flex-direction:column;gap:10px;margin-top:18px}
.job{background:linear-gradient(180deg,#121a30,#0e1526);border:1px solid var(--line);border-radius:12px;padding:12px 14px}
.job .top{display:flex;align-items:center;gap:10px}
.job .nm{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.job .meta{color:var(--mut);font-size:12px;margin-left:auto;white-space:nowrap}
.badge{font-size:11px;padding:2px 8px;border-radius:999px;border:1px solid var(--line);color:var(--mut)}
.badge.run{color:var(--acc);border-color:#234a7a}.badge.done{color:var(--good);border-color:#244a36}.badge.err{color:var(--bad);border-color:#4a2424}
.bar{height:7px;background:#0a1426;border-radius:6px;margin-top:9px;overflow:hidden;border:1px solid #1a2742}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,#4ea1ff,#7CFC9A);transition:width .25s}
.err{color:var(--bad);font-size:12px;margin-top:7px}
.srcline{color:var(--mut);font-size:11px;margin-top:3px}
.stage{font-size:12px;color:var(--mut);margin-top:6px}
.row2{display:flex;gap:8px;margin-top:9px}
.tiny{font-size:11px;color:var(--mut);background:none;border:0;cursor:pointer;padding:2px 6px}
.foot{color:var(--mut);font-size:12px;margin-top:18px;text-align:center}
.empty{color:var(--mut);text-align:center;padding:20px}
</style></head><body><div class="wrap">
<header>
  <div class="logo">▶</div>
  <h1>NucleoOS Video Studio</h1>
  <div class="env" id="env"></div>
</header>
<p class="sub">Convert any video to the Cardputer's <b>.nfv</b> format and drop it straight onto the SD.</p>

<div class="drop" id="drop">
  <b>Drag &amp; drop video files here</b><br><small>mp4 · mkv · mov · avi · webm … — they stream to this PC and convert with native ffmpeg</small>
</div>
<div class="or">or convert a file already on disk (no copy — best for big films)</div>
<div class="pathrow">
  <input id="path" placeholder="C:\\Users\\you\\Downloads\\movie.mkv" spellcheck="false">
  <button class="primary" id="addPath">Convert path</button>
</div>

<div class="opts">
  <label>Profile <select id="profile"><option value="compat">Compat (12fps, smallest)</option><option value="balanced" selected>Balanced (20fps)</option><option value="quality">Quality (24fps)</option></select></label>
  <label>FPS <select id="fps"><option value="">auto</option><option>12</option><option>15</option><option>18</option><option>20</option><option>24</option></select></label>
  <label>Fit <select id="fit"><option value="crop" selected>Crop (fill, no bars)</option><option value="pad">Pad (letterbox)</option></select></label>
  <label>Target <select id="target"></select></label>
  <span class="chk"><input type="checkbox" id="gpu" checked><label for="gpu" style="margin:0">GPU</label></span>
  <button class="tiny" id="clear" style="margin-left:auto">Clear finished</button>
</div>

<div class="jobs" id="jobs"><div class="empty">No conversions yet.</div></div>
<div class="foot" id="foot"></div>
</div>
<script>
const $=s=>document.querySelector(s), api=(u,o)=>fetch(u,o);
let ENV={targets:[]};
let SAVED=loadOpts();
async function loadEnv(){
  ENV=await (await api('/api/env')).json();
  const e=$('#env');
  e.innerHTML='<span class="pill '+(ENV.ffmpeg?'ok':'bad')+'">ffmpeg '+(ENV.ffmpeg?'✓':'✗')+'</span>'
    +'<span class="pill '+(ENV.gpu?'ok':'')+'">GPU '+(ENV.gpu?'CUDA':'CPU')+'</span>'
    +'<span class="pill '+(ENV.targets.some(t=>t.kind==="sd")?'ok':'')+'">'+(ENV.targets.filter(t=>t.kind==="sd").length||0)+' SD</span>'
    +(ENV.device&&ENV.device.online?'<span class="pill ok">Cardputer ✓</span>':'');
  const sel=$('#target'), keep=sel.value||SAVED.target;
  sel.innerHTML=ENV.targets.map(t=>'<option value="'+t.id+'">'+esc(t.label)+'</option>').join('');
  if([...sel.options].some(o=>o.value===keep)) sel.value=keep;
  const dev=ENV.targets.find(t=>t.id===sel.value)&&ENV.targets.find(t=>t.id===sel.value).kind==='device';
  $('#foot').innerHTML=!ENV.ffmpeg?'ffmpeg not found — install it and put it on PATH, then reload.'
    :dev?'Wi-Fi delivery to the Cardputer — great for short clips. For full films use an SD card (big uploads can drop).'
    :'Files land in &lt;target&gt;\\\\data\\\\Videos. Then on the Cardputer: Media → Video.';
}
function params(name){
  const q=new URLSearchParams({name,profile:$('#profile').value,fit:$('#fit').value,gpu:$('#gpu').checked?'1':'0',target:$('#target').value});
  if($('#fps').value) q.set('fps',$('#fps').value);
  return q;
}
async function convertPath(){
  const path=$('#path').value.trim(); if(!path) return;
  const q=params(path.split(/[\\\\/]/).pop()); q.set('path',path);
  const r=await api('/api/jobs?'+q.toString(),{method:'POST'});
  const j=await r.json(); if(j.error) alert(j.error); else $('#path').value='';
}
async function convertFile(file){
  const q=params(file.name);
  await api('/api/jobs?'+q.toString(),{method:'POST',body:file}); // stream the file
}
$('#addPath').onclick=convertPath;
$('#path').addEventListener('keydown',e=>{if(e.key==='Enter')convertPath();});
$('#clear').onclick=()=>api('/api/clear',{method:'POST'});
const drop=$('#drop');
drop.onclick=()=>{const i=document.createElement('input');i.type='file';i.multiple=true;i.accept='video/*';i.onchange=()=>[...i.files].forEach(convertFile);i.click();};
// Accetta il drop OVUNQUE nella finestra, non solo nel riquadro tratteggiato: senza questo un
// file lasciato 1px fuori dal riquadro fa "navigare" il browser sul file invece di convertirlo
// (la causa tipica di "il drag and drop non si attiva"). Il riquadro resta solo come highlight.
const hasFiles=e=>e.dataTransfer&&[...e.dataTransfer.types].includes('Files');
['dragenter','dragover'].forEach(ev=>document.addEventListener(ev,e=>{if(!hasFiles(e))return;e.preventDefault();drop.classList.add('hot');}));
document.addEventListener('dragleave',e=>{if(!e.relatedTarget)drop.classList.remove('hot');});
document.addEventListener('drop',e=>{if(!hasFiles(e))return;e.preventDefault();drop.classList.remove('hot');[...e.dataTransfer.files].forEach(convertFile);});

const ST={queued:['Queued',''],probing:['Reading','run'],audio:['Audio','run'],video:['Encoding','run'],delivering:['Delivering','run'],done:['Done','done'],error:['Error','err']};
function render(jobs){
  const c=$('#jobs');
  if(!jobs.length){c.innerHTML='<div class="empty">No conversions yet.</div>';return;}
  c.innerHTML=jobs.slice().reverse().map(j=>{
    const st=ST[j.status]||[j.status,''];
    const meta=(j.src?esc(j.src)+'  →  ':'')+(j.profile||'')+(j.fps?' '+j.fps+'fps':'')+'  →  '+esc(j.target||'');
    const running=['queued','probing','audio','video','delivering'].includes(j.status);
    return '<div class="job"><div class="top"><span class="nm">'+esc(j.name)+'</span>'
      +'<span class="badge '+st[1]+'">'+st[0]+'</span>'
      +'<span class="meta">'+(j.sizeMB?j.sizeMB+' MB':'')+'</span></div>'
      +'<div class="srcline">'+meta+'</div>'
      +'<div class="bar"><i style="width:'+(j.pct||0)+'%"></i></div>'
      +(j.error?'<div class="err">⚠ '+esc(j.error)+'</div>':'<div class="stage">'+esc(j.stage||'')+(j.pct?' · '+j.pct+'%':'')+'</div>')
      +'<div class="row2">'
        +(running?'<button class="tiny" onclick="cancel(\\''+j.id+'\\')">Cancel</button>':'')
        +(j.canRetry?'<button class="tiny" onclick="retry(\\''+j.id+'\\')">↻ Retry</button>':'')
      +'</div></div>';
  }).join('');
}
function esc(s){return (s||'').replace(/[&<>"]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[m]));}
window.cancel=id=>api('/api/cancel?id='+id,{method:'POST'});
window.retry=id=>api('/api/retry?id='+id,{method:'POST'});
// remember settings
const SK='nucleo-studio-opts';
function saveOpts(){try{localStorage.setItem(SK,JSON.stringify({profile:$('#profile').value,fps:$('#fps').value,fit:$('#fit').value,gpu:$('#gpu').checked,target:$('#target').value}));}catch{}}
function loadOpts(){try{const o=JSON.parse(localStorage.getItem(SK)||'{}');for(const k of ['profile','fps','fit'])if(o[k]!=null&&$('#'+k))$('#'+k).value=o[k];if(o.gpu!=null)$('#gpu').checked=o.gpu;return o;}catch{return{}}}
['profile','fps','fit','target'].forEach(id=>$('#'+id).addEventListener('change',saveOpts));
$('#gpu').addEventListener('change',saveOpts);
const es=new EventSource('/api/events'); es.onmessage=e=>{try{render(JSON.parse(e.data).jobs);}catch{}};
loadEnv(); setInterval(loadEnv,8000);
</script></body></html>`;
