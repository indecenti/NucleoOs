/* SPDX-License-Identifier: AGPL-3.0-or-later */
// In-browser video → .nfv v4 converter. The heavy lifting (ffmpeg) runs as WebAssembly right here
// in the browser — NO PC companion, NO install. The finished .nfv (picture AND sound in one file) is written
// to the device in resumable parts (fs.write, where the device appends by offset), so a Wi-Fi drop costs one
// part, not the film.
//
// Single-thread @ffmpeg/core (vendored under vendor/ffmpeg/) → no SharedArrayBuffer, so it runs
// without cross-origin-isolation headers. For a whole film the PC encoder is faster: tools/nfv/nfv4.py
// (same format, byte-identical container — tools/nfv/nfv.test.mjs checks it).
import { buildNfv4, splitMjpeg, wavAdpcmBlocks, stripMask, REFRESH_S } from './nfv.js';
import I18N from '/nucleo-i18n.js';
import fs from '/nucleo-fs.js';
const t = await I18N.init('video-studio');

// q = ffmpeg's MJPEG quantiser (2 = best). The device shows ~15 fps at most (SD + SPI bus), so more fps only
// costs card bandwidth; bigger frames cost read time: "quality" is for calm content.
export const PROFILES = {
  compat:   { fps: 12, q: 6 },
  balanced: { fps: 15, q: 5 },
  quality:  { fps: 15, q: 3 },
};
// universal: 320x240 for the external 2.8" panel; on the Cardputer alone the player scales it to fit.
// cardputer: 240x135 (coded 240x144, a multiple of 16) — smaller, for the built-in screen only.
export const TARGETS = {
  universal: { w: 320, h: 240, visW: 320, visH: 240 },
  cardputer: { w: 240, h: 144, visW: 240, visH: 135 },
};
const AUDIO_RATE = 16000, AUDIO_BLOCK = 256;
const LOUD = 'I=-14:TP=-1.5:LRA=11';            // every clip to the same loudness (EBU R128), as nfv4.py
const MAX_JPEG = 48000;                          // the player's frame buffer limit (app_video.cpp)

const ext = (n) => { const m = /\.[^.]+$/.exec(n || ''); return m ? m[0] : '.bin'; };
const base = (n) => (n || 'video').replace(/\.[^.]+$/, '');
// The SD is FAT: a name with : < > ? * " | \ / (or a trailing dot/space) cannot be created there, so a video
// picked on a Mac/Linux box ("Talk: part 1.mov") converted for minutes and then failed on the write.
export const sdSafe = (n) => String(n).replace(/[\u0000-\u001f<>:"/\\|?*]/g, '_').replace(/[. ]+$/, '') || 'video';

function loadScript(src) {
  return new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = src; s.onload = res; s.onerror = () => rej(new Error('failed to load ' + src));
    document.head.appendChild(s);
  });
}

let _ff = null, _loading = null;
// Lazily load the ~30 MB wasm engine on first use. onStage reports human progress.
export function loadEngine(onStage) {
  if (_ff) return Promise.resolve(_ff);
  if (_loading) return _loading;
  _loading = (async () => {
    onStage && onStage(t('stage_loading_engine'));
    await loadScript('vendor/ffmpeg/ffmpeg.js');
    const { FFmpeg } = window.FFmpegWASM;
    const ff = new FFmpeg();
    // Surface core errors (helps when a codec/filter is missing).
    ff.on('log', ({ message }) => { if (/error|invalid|unable|not found/i.test(message)) console.warn('[ffmpeg]', message); });
    // Files are vendored SAME-ORIGIN, so pass direct URLs (no toBlobURL): a blob: coreURL breaks
    // the worker's importScripts of the UMD core ("failed to import ffmpeg-core.js").
    // Do NOT pass classWorkerURL: that makes @ffmpeg/ffmpeg spawn a *module* worker (no
    // importScripts), which can't load the UMD core. Omitting it spawns the CLASSIC worker
    // (814.ffmpeg.js, auto-found next to ffmpeg.js) that importScripts the UMD core.
    const b = (p) => new URL('vendor/ffmpeg/' + p, location.href).href;
    await ff.load({
      coreURL: b('ffmpeg-core.js'),
      wasmURL: b('ffmpeg-core.wasm'),
    });
    _ff = ff;
    return ff;
  })();
  return _loading;
}

function vf(fit, fps, tg) {
  const s = fit === 'pad'
    ? `scale=${tg.visW}:${tg.visH}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${tg.visW}:${tg.visH}:(ow-iw)/2:(oh-ih)/2:color=black`
    : `scale=${tg.visW}:${tg.visH}:force_original_aspect_ratio=increase:flags=lanczos,crop=${tg.visW}:${tg.visH}`;
  return `${s},pad=${tg.w}:${tg.h}:(ow-iw)/2:(oh-ih)/2:color=black,fps=${fps}`;
}

// Two-pass EBU R128: pass 1 measures, pass 2 applies a LINEAR gain (no pumping). null = no audio stream.
async function measureLoudness(ff, inName) {
  const lines = [];
  const onLog = ({ message }) => lines.push(message);
  ff.on('log', onLog);
  try {
    await ff.exec(['-hide_banner', '-nostats', '-i', inName, '-map', '0:a:0', '-ac', '1',
      '-af', `loudnorm=${LOUD}:print_format=json`, '-f', 'null', '-']);
  } finally { ff.off('log', onLog); }
  const m = /\{[^{}]*"input_i"[^{}]*\}/.exec(lines.join('\n'));
  if (!m) return null;
  try { return JSON.parse(m[0]); } catch { return null; }
}

// Convert a File to { nfv: Blob, frames, fps, size, base }. onStage(text, pct?) for UI.
export async function convertFile(file, opts = {}, onStage = () => {}) {
  const ff = await loadEngine((s) => onStage(s));
  const prof = { ...PROFILES[opts.profile || 'balanced'] };
  if (opts.fps) prof.fps = +opts.fps;
  const tg = TARGETS[opts.target] || TARGETS.universal;
  const fit = opts.fit === 'pad' ? 'pad' : 'crop';
  const inName = 'in' + ext(file.name);

  // ffmpeg.wasm emits an overall 0..1 progress event; map it onto the current stage.
  let stageLabel = t('badge_converting'), stageBase = 0, stageSpan = 100;
  const onProg = ({ progress }) => { if (progress >= 0 && progress <= 1) onStage(stageLabel, Math.min(99, Math.round(stageBase + progress * stageSpan))); };
  ff.on('progress', onProg);

  // ffmpeg.wasm's virtual FS outlives a conversion, and ffmpeg will not overwrite an existing output without
  // -y. A conversion that failed half-way used to leave its audio behind, and the next (silent) video was
  // delivered with that soundtrack. Start and end every conversion with a clean slate.
  const scratch = [inName, 'a.wav', 'v.mjpeg'];
  const clean = async () => { for (const f of scratch) { try { await ff.deleteFile(f); } catch {} } };
  await clean();
  try {
    onStage(t('stage_reading'), 0);
    await ff.writeFile(inName, new Uint8Array(await file.arrayBuffer()));

    // 1) audio → loudness-normalised 16 kHz mono IMA ADPCM (optional; some clips have none)
    stageLabel = t('stage_loudness'); stageBase = 0; stageSpan = 10;
    const loud = await measureLoudness(ff, inName);
    let audio = null;
    if (loud) {
      stageLabel = t('stage_audio'); stageBase = 10; stageSpan = 10;
      const finite = (x) => Number.isFinite(parseFloat(x));
      const af = finite(loud.input_i) && finite(loud.input_tp)
        ? ['-af', `loudnorm=${LOUD}:measured_I=${loud.input_i}:measured_TP=${loud.input_tp}:measured_LRA=${loud.input_lra}` +
                  `:measured_thresh=${loud.input_thresh}:offset=${loud.target_offset}:linear=true`]
        : [];                                                   // a silent track: nothing to normalise
      const code = await ff.exec(['-i', inName, '-map', '0:a:0', '-ac', '1', ...af, '-ar', String(AUDIO_RATE),
        '-c:a', 'adpcm_ima_wav', '-block_size', String(AUDIO_BLOCK), 'a.wav']);
      if (code !== 0) throw new Error(t('err_audio'));
      audio = wavAdpcmBlocks(await ff.readFile('a.wav'));
    }

    // 2) video → concatenated baseline 4:2:0 JPEGs → frames
    stageLabel = t('stage_video'); stageBase = 20; stageSpan = 55;
    const vcode = await ff.exec(['-i', inName, '-map', '0:v:0', '-an', '-vf', vf(fit, prof.fps, tg), '-pix_fmt', 'yuvj420p',
      '-q:v', String(prof.q), '-c:v', 'mjpeg', '-f', 'image2pipe', 'v.mjpeg']);
    // A failed pass leaves no v.mjpeg: say "no frames", not the virtual FS's cryptic "ErrnoError: FS error".
    const mjpeg = vcode === 0 ? await ff.readFile('v.mjpeg').catch(() => null) : null;
    const frames = mjpeg ? splitMjpeg(mjpeg) : [];
    if (!frames.length) throw new Error(t('err_no_frames'));

    // 3) strip masks: which 16-row strips each frame changes (the device sends only those over its SPI bus)
    const masks = new Array(frames.length);
    const cv = new OffscreenCanvas(tg.w, tg.h), g = cv.getContext('2d', { willReadFrequently: true });
    const shown = new Array(tg.h >> 4).fill(null), refresh = Math.max(2, Math.round(prof.fps * REFRESH_S));
    for (let i = 0; i < frames.length; i++) {
      if (frames[i].length > MAX_JPEG) throw new Error(t('err_frame_big'));
      const bmp = await createImageBitmap(new Blob([frames[i]], { type: 'image/jpeg' }));
      g.drawImage(bmp, 0, 0); bmp.close();
      masks[i] = stripMask(shown, g.getImageData(0, 0, tg.w, tg.h).data, tg.w, tg.h, i, refresh);
      if (i % 60 === 0) onStage(t('stage_masks'), 75 + Math.round(i / frames.length * 20));
    }

    // 4) assemble .nfv v4
    onStage(t('stage_assembling'), 96);
    const { blob, size } = buildNfv4(frames, { fps: prof.fps, width: tg.w, height: tg.h, visW: tg.visW, visH: tg.visH, masks, audio });
    return { nfv: blob, frames: frames.length, fps: prof.fps, size, base: sdSafe(base(file.name)) };
  } finally {
    try { ff.off('progress', onProg); } catch {}
    await clean();                      // success OR failure: nothing may leak into the next conversion
  }
}

// Write the result onto the device SD (same-origin — no CORS). fs.write sends it in resumable parts where the
// device appends by offset (a dropped request costs one part, not the film), in one write elsewhere, and
// lets the finished file take its name only once it is complete: a half film never replaces a whole one.
export async function deliverToDevice(result, onStage = () => {}) {
  const dst = `/data/Videos/${result.base}.nfv`;
  onStage(t('stage_writing'), 0);
  try {
    await fs.write(dst, result.nfv, { onProgress: (done, total) => onStage(t('stage_writing'), total ? Math.floor(done / total * 100) : 0) });
  } catch (e) {
    if (e && e.status) throw new Error(t('err_write_failed', { status: e.status, path: dst }));
    throw e;                            // no HTTP answer (connection lost, no space...): its own message, as before
  }
  return dst;
}
