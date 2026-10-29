// Browser E2E — Video Studio against the simulator. The app converts IN THE BROWSER with ffmpeg.wasm and
// writes the result to /data/Videos on the SD (it does not talk to the PC companion — that is only a hint).
// ffmpeg.wasm is replaced IN THE PAGE by a stand-in with the same API and the same habits that matter:
// exec() resolves an exit code (it does not throw), the virtual FS keeps files between conversions, and an
// existing output is NOT overwritten without -y. It answers the three passes of an .nfv v4 (loudness measure,
// IMA ADPCM WAV, concatenated JPEGs) with real data: the app decodes every frame to find what changed.
// The rest (queue, strip masks, .nfv assembly, writes) is the real app.
//   - the conversion asks ffmpeg for the chosen profile / fps / fit and delivers a valid .nfv v4 (sound inside);
//   - the video name is text, and the SD file name is one the card can store (FAT has no : < > ? * …);
//   - bad input (not a video, an empty file) is refused up front, without loading the 30 MB engine;
//   - a failed conversion never leaks its audio into the next video;
//   - a failed SD write is reported, and the queue carries on.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/video-studio/www/i18n.en.json') };

// A "video" for the stand-in is a JSON descriptor { frames, audio } — anything else is not media.
const FAKE_FFMPEG = `(() => {
  if (!location.pathname.startsWith('/apps/video-studio/')) return;
  const FF = window.__ff = { scriptLoads: 0, loads: 0, execs: [] };
  class FFmpeg {
    constructor() { this.fs = new Map(); this.h = {}; }
    on(ev, fn) { (this.h[ev] ||= []).push(fn); }
    off(ev, fn) { this.h[ev] = (this.h[ev] || []).filter((f) => f !== fn); }
    async load() { FF.loads++; return true; }
    async writeFile(n, d) { this.fs.set(n, new Uint8Array(d)); }
    async readFile(n) { if (!this.fs.has(n)) throw new Error('ErrnoError: FS error'); return this.fs.get(n); }
    async deleteFile(n) { if (!this.fs.has(n)) throw new Error('ErrnoError: FS error'); this.fs.delete(n); }
    async exec(args) {
      FF.execs.push(args.slice());
      const n = FF.execs.length;
      const inName = args[args.indexOf('-i') + 1], out = args[args.length - 1];
      let media = null;
      try { media = JSON.parse(new TextDecoder().decode(this.fs.get(inName))); } catch {}
      if (!media || typeof media !== 'object') return 1;                       // "Invalid data found when processing input"
      if (out !== '-' && this.fs.has(out) && !args.includes('-y')) return 1;   // "File exists. Not overwriting - exiting"
      (this.h.progress || []).forEach((f) => f({ progress: 0.5 }));
      if (out === '-') {                                                       // loudness measure: a JSON report in the log
        if (!media.audio) return 1;                                            // "Stream map '0:a:0' matches no streams"
        const rep = '{ "input_i" : "-20.0", "input_tp" : "-3.0", "input_lra" : "5.0", "input_thresh" : "-30.0", "target_offset" : "0.0" }';
        (this.h.log || []).forEach((f) => f({ type: 'stderr', message: rep }));
        return 0;
      }
      if (args.includes('-an')) {                                             // the picture: concatenated baseline JPEGs
        if (!media.frames) return 1;
        const cv = new OffscreenCanvas(32, 16), g = cv.getContext('2d'), parts = [];
        for (let i = 0; i < media.frames; i++) {
          g.fillStyle = 'rgb(' + ((i * 40) & 255) + ',80,160)'; g.fillRect(0, 0, 32, 16);
          parts.push(new Uint8Array(await (await cv.convertToBlob({ type: 'image/jpeg', quality: 0.8 })).arrayBuffer()));
        }
        const all = new Uint8Array(parts.reduce((a, b) => a + b.length, 0));
        parts.reduce((o, b) => (all.set(b, o), o + b.length), 0);
        this.fs.set(out, all);
        return 0;
      }
      if (!media.audio) return 1;                                              // "Output file does not contain any stream"
      // A mono IMA ADPCM WAV (16 kHz, 256-byte blocks of 505 samples); every data byte is this pass's number, so
      // the test can tell WHICH pass a film's sound came from.
      const block = 256, nblocks = 3, wav = new Uint8Array(48 + block * nblocks), dv = new DataView(wav.buffer);
      const put = (o, str) => { for (let i = 0; i < 4; i++) wav[o + i] = str.charCodeAt(i); };
      put(0, 'RIFF'); dv.setUint32(4, wav.length - 8, true); put(8, 'WAVE');
      put(12, 'fmt '); dv.setUint32(16, 20, true); dv.setUint16(20, 0x11, true); dv.setUint16(22, 1, true);
      dv.setUint32(24, 16000, true); dv.setUint32(28, 8000, true); dv.setUint16(32, block, true); dv.setUint16(34, 4, true);
      dv.setUint16(36, 2, true); dv.setUint16(38, 505, true);
      put(40, 'data'); dv.setUint32(44, block * nblocks, true); wav.fill(n & 255, 48);
      this.fs.set(out, wav);
      return 0;
    }
  }
  const app = Element.prototype.appendChild;
  Element.prototype.appendChild = function (node) {
    if (node && node.tagName === 'SCRIPT' && String(node.src).endsWith('/vendor/ffmpeg/ffmpeg.js')) {
      FF.scriptLoads++; window.FFmpegWASM = { FFmpeg };
      setTimeout(() => node.onload && node.onload(), 0);
      return node;
    }
    return app.call(this, node);
  };
})();`;

async function open(browser, sim) {
  const page = await browser.newPage();
  await page.initScript(FAKE_FFMPEG);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/video-studio/`);
  await page.waitFor(`document.getElementById('drop') && window.__ff`, { timeout: 45000 });
  await page.networkIdle({ quiet: 400, timeout: 15000 });
  return page;
}
// Drop files onto the window the way a user does. files: [{ name, type, body }] (body: string)
const drop = (files) => `(() => {
  const dt = new DataTransfer();
  for (const f of ${JSON.stringify(files)}) dt.items.add(new File([f.body], f.name, { type: f.type }));
  document.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
  return true; })()`;
const vid = (name, media, type = 'video/mp4') => ({ name, type, body: JSON.stringify(media) });
// Jobs are prepended: newest first. Return them oldest first.
const JOBS = `[...document.querySelectorAll('#jobs .job')].reverse().map((j) => ({ name: j.querySelector('.nm').textContent,
  badge: j.querySelector('[data-badge]').className.replace('badge ', ''), stage: j.querySelector('[data-stage]').textContent,
  err: (j.querySelector(':scope > .err') || {}).textContent || '' }))`;
const settled = (n) => `(() => { const j = ${JOBS}; return j.length === ${n} && j.every((x) => x.badge === 'done' || x.badge === 'err') && j; })()`;
const videosDir = (sim) => join(sim.sd, 'data', 'Videos');
// A provisioned card has /data/Videos (nucleo_storage_provision); the simulated SD copy does not.
async function startCard() { const sim = await startSim(); await mkdir(videosDir(sim), { recursive: true }); return sim; }
const sdFiles = (sim) => existsSync(videosDir(sim)) ? readdirSync(videosDir(sim)).sort() : [];
function nfvHeader(buf) {
  return { magic: buf.subarray(0, 4).toString('latin1'), version: buf[4], flags: buf[5], w: buf.readUInt16LE(6), h: buf.readUInt16LE(8),
    fps: buf.readUInt16LE(10), rate: buf.readUInt16LE(12), frames: buf.readUInt32LE(16) };
}
// The chunks after the 64-byte header, up to the index: [u8 type][u8 0][u16 mask][u32 len][u32 stamp][payload].
function nfvChunks(buf) {
  const end = buf.readUInt32LE(32), out = [];
  for (let p = 64; p + 12 <= end;) { const len = buf.readUInt32LE(p + 4); out.push({ type: buf[p], payload: buf.subarray(p + 12, p + 12 + len) }); p += 12 + len; }
  return out;
}

test('video-studio: converts with the chosen profile / fps / fit and delivers a valid .nfv v4 with its sound; names are text and SD-safe', { skip }, async (t) => {
  const sim = await startCard();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  await page.eval(`(() => { const s = (id, v) => { const e = document.getElementById(id); e.value = v; e.dispatchEvent(new Event('change')); };
    s('profile', 'quality'); s('fit', 'pad'); s('fps', ''); return true; })()`);
  await page.eval(drop([vid('Holiday.mp4', { frames: 30, audio: true })]));
  const jobs = await page.waitFor(settled(1), { timeout: 20000 });
  assert.ok(jobs, 'the job finished');
  assert.equal(jobs[0].badge, 'done', 'done: ' + JSON.stringify(jobs[0]));
  assert.equal(jobs[0].stage, EN.stage_ready.replace('{frames}', 30).replace('{fps}', 15).replace('{path}', '/data/Videos/Holiday.nfv') + ' · 100%');

  const execs = await page.eval(`window.__ff.execs`);
  assert.equal(execs.length, 3, 'a loudness measure, one audio pass, one video pass');
  const [m, a, v] = execs;
  assert.ok(m.join(' ').includes('print_format=json') && m[m.length - 1] === '-', 'the first pass only measures: ' + m.join(' '));
  assert.ok(a[a.indexOf('-c:a') + 1] === 'adpcm_ima_wav' && a[a.indexOf('-ar') + 1] === '16000' && a[a.indexOf('-ac') + 1] === '1'
    && a[a.indexOf('-block_size') + 1] === '256', 'mono 16 kHz IMA ADPCM in 256-byte blocks: ' + a.join(' '));
  assert.ok(a[a.indexOf('-af') + 1].includes('measured_I=-20.0'), 'the measured loudness drives a linear gain: ' + a.join(' '));
  const vf = v[v.indexOf('-vf') + 1];
  assert.ok(vf.endsWith(',fps=15') && vf.includes('pad=320:240') && vf.includes('force_original_aspect_ratio=decrease'), 'pad fit at the profile fps: ' + vf);
  assert.equal(v[v.indexOf('-q:v') + 1], '3'); assert.equal(v[v.indexOf('-c:v') + 1], 'mjpeg');

  const nfv = readFileSync(join(videosDir(sim), 'Holiday.nfv'));
  assert.deepEqual(nfvHeader(nfv), { magic: 'NFV1', version: 4, flags: 3, w: 320, h: 240, fps: 15, rate: 16000, frames: 30 }, 'header: audio + strip masks, 320x240, 15 fps, 30 frames');
  const ch = nfvChunks(nfv);
  assert.equal(ch.filter((c) => c.type === 1).length, 30, 'every frame is in the film');
  assert.equal(ch.filter((c) => c.type === 2).length, 3, 'and every block of its sound');
  assert.deepEqual(sdFiles(sim), ['Holiday.nfv'], 'one file: the sound is inside it');

  // A fixed fps override and the crop fit; a name that is markup, and one with characters FAT cannot store.
  await page.eval(`(() => { const s = (id, v) => { const e = document.getElementById(id); e.value = v; e.dispatchEvent(new Event('change')); };
    s('profile', 'compat'); s('fit', 'crop'); s('fps', '15'); return true; })()`);
  const evil = '<img src=x onerror="window.__pwned=1">.webm';
  await page.eval(drop([vid(evil, { frames: 3, audio: false }, 'video/webm'), vid('Talk: part 1? <final>.mov', { frames: 4, audio: false }, 'video/quicktime')]));
  const all = await page.waitFor(settled(3), { timeout: 20000 });
  assert.ok(all, 'both jobs finished');
  assert.deepEqual(all.map((j) => j.name), ['Holiday.mp4', evil, 'Talk: part 1? <final>.mov'], 'names shown verbatim');
  assert.equal(await page.eval(`document.querySelectorAll('#jobs .nm *').length`), 0, 'a video name is never markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
  const v2 = (await page.eval(`window.__ff.execs`)).filter((x) => x.includes('-an'));
  assert.ok(v2[1][v2[1].indexOf('-vf') + 1].endsWith(',fps=15') && v2[1][v2[1].indexOf('-vf') + 1].includes('crop=320:240'), 'fps override + crop');
  assert.equal(all[1].badge, 'done', JSON.stringify(all[1]));
  assert.equal(all[2].badge, 'done', 'a name with : ? < > still lands on the card: ' + JSON.stringify(all[2]));
  for (const f of sdFiles(sim)) assert.ok(!/[<>:"\\|?*]/.test(f), 'SD file names carry no FAT-forbidden characters: ' + f);
  assert.ok(sdFiles(sim).includes('Talk_ part 1_ _final_.nfv'), 'sanitized name: ' + sdFiles(sim));
  assert.ok(all[2].stage.includes('/data/Videos/Talk_ part 1_ _final_.nfv'), 'and the job says where it went');
  assert.deepEqual(nfvHeader(readFileSync(join(videosDir(sim), 'Talk_ part 1_ _final_.nfv'))).flags, 2, 'no audio track → audio flag clear');
});

test('video-studio: refuses files that are not videos, and empty files, before loading the engine', { skip }, async (t) => {
  const sim = await startCard();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  await page.eval(drop([{ name: 'notes.txt', type: 'text/plain', body: 'hello' }, { name: 'empty.mp4', type: 'video/mp4', body: '' },
    { name: 'photo.jpg', type: 'image/jpeg', body: 'jpeg' }]));
  const jobs = await page.waitFor(settled(3), { timeout: 10000 });
  assert.ok(jobs, 'each file got a card');
  assert.deepEqual(jobs.map((j) => [j.name, j.badge, j.err]), [
    ['notes.txt', 'err', '⚠ ' + EN.err_not_video], ['empty.mp4', 'err', '⚠ ' + EN.err_empty], ['photo.jpg', 'err', '⚠ ' + EN.err_not_video]]);
  assert.equal(await page.eval(`window.__ff.scriptLoads + window.__ff.loads`), 0, 'the 30 MB engine was never loaded for them');
  assert.deepEqual(sdFiles(sim), [], 'nothing written');

  // A video with a missing MIME type but a known extension (common for .mkv) is accepted.
  await page.eval(drop([vid('film.mkv', { frames: 2, audio: false }, '')]));
  const j2 = await page.waitFor(settled(4), { timeout: 20000 });
  assert.equal(j2[3].badge, 'done', JSON.stringify(j2[3]));
});

test('video-studio: a failed conversion never leaks its audio into the next video', { skip }, async (t) => {
  const sim = await startCard();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  // 1) audio converts, then the video stage produces nothing → the job fails.
  // 2) a SILENT video converts fine — it must not come out with the first one's soundtrack.
  await page.eval(drop([vid('broken.mp4', { frames: 0, audio: true }), vid('silent.mp4', { frames: 5, audio: false })]));
  const jobs = await page.waitFor(settled(2), { timeout: 20000 });
  assert.ok(jobs);
  assert.equal(jobs[0].badge, 'err'); assert.equal(jobs[0].err, '⚠ ' + EN.err_no_frames);
  assert.equal(jobs[1].badge, 'done', JSON.stringify(jobs[1]));
  assert.deepEqual(sdFiles(sim), ['silent.nfv'], 'nothing from the broken job');
  const silent = readFileSync(join(videosDir(sim), 'silent.nfv'));
  assert.equal(nfvHeader(silent).flags, 2, 'the silent video is not flagged as having audio');
  assert.equal(nfvChunks(silent).filter((c) => c.type === 2).length, 0, 'and carries no sound: the broken job left none behind');

  // 3) a third video with audio gets ITS OWN audio (not a leftover a.wav).
  await page.eval(drop([vid('talk.mp4', { frames: 2, audio: true })]));
  assert.ok(await page.waitFor(settled(3), { timeout: 20000 }));
  const sound = nfvChunks(readFileSync(join(videosDir(sim), 'talk.nfv'))).filter((c) => c.type === 2);
  const lastAudioExec = (await page.eval(`window.__ff.execs`)).reduce((n, x, i) => (x[x.length - 1] === 'a.wav' ? i + 1 : n), 0);
  assert.ok(sound.length && sound.every((c) => c.payload.every((b) => b === lastAudioExec)), 'the sound is the one encoded for this video');
});

test('video-studio: a failed SD write is reported with the path, and the queue carries on', { skip }, async (t) => {
  const sim = await startCard();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 507, times: 1 });
  await page.eval(drop([vid('first.mp4', { frames: 2, audio: false }), vid('second.mp4', { frames: 2, audio: false })]));
  const jobs = await page.waitFor(settled(2), { timeout: 20000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.ok(jobs);
  assert.equal(jobs[0].badge, 'err');
  assert.equal(jobs[0].err, '⚠ ' + EN.err_write_failed.replace('{status}', 507).replace('{path}', '/data/Videos/first.nfv'));
  assert.equal(jobs[1].badge, 'done', 'the next video still converts');
  assert.deepEqual(sdFiles(sim), ['second.nfv']);
});
