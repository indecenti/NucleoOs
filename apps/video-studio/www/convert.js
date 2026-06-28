// In-browser video → .nfv converter. The heavy lifting (ffmpeg) runs as WebAssembly right here
// in the browser — NO PC companion, NO install. We then write only the small .nfv (+ sibling
// .mp3) to the device, same-origin (/api/fs/write), so there's no huge raw upload to drop.
//
// Single-thread @ffmpeg/core (vendored under vendor/ffmpeg/) → no SharedArrayBuffer, so it runs
// without cross-origin-isolation headers. Slower than the native companion's CUDA path, but it
// works for everyone. (A WebCodecs hardware-decode fast path is a planned phase-2 accel.)
import { buildNfv, splitMjpeg } from './nfv.js';
import I18N from '/nucleo-i18n.js';
const t = await I18N.init('video-studio');

export const SCREEN_W = 240, SCREEN_H = 136;
export const PROFILES = {
  compat:   { fps: 12, q: 8, ar: 22050, ab: '32k' },
  balanced: { fps: 20, q: 7, ar: 22050, ab: '40k' },
  quality:  { fps: 24, q: 5, ar: 24000, ab: '48k' },
};

const ext = (n) => { const m = /\.[^.]+$/.exec(n || ''); return m ? m[0] : '.bin'; };
const base = (n) => (n || 'video').replace(/\.[^.]+$/, '');

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

function vf(fit, fps) {
  const s = fit === 'pad'
    ? `scale=${SCREEN_W}:${SCREEN_H}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${SCREEN_W}:${SCREEN_H}:(ow-iw)/2:(oh-ih)/2:color=black`
    : `scale=${SCREEN_W}:${SCREEN_H}:force_original_aspect_ratio=increase:flags=lanczos,crop=${SCREEN_W}:${SCREEN_H}`;
  return `fps=${fps},${s}`;
}

// Convert a File to { nfv: Blob, mp3: Blob|null, frames, fps, base }. onStage(text, pct?) for UI.
export async function convertFile(file, opts = {}, onStage = () => {}) {
  const ff = await loadEngine((s) => onStage(s));
  const prof = { ...PROFILES[opts.profile || 'balanced'] };
  if (opts.fps) prof.fps = +opts.fps;
  const fit = opts.fit === 'pad' ? 'pad' : 'crop';
  const inName = 'in' + ext(file.name);

  // ffmpeg.wasm emits an overall 0..1 progress event; map it onto the current stage.
  let stageLabel = t('badge_converting'), stageBase = 0, stageSpan = 100;
  const onProg = ({ progress }) => { if (progress >= 0 && progress <= 1) onStage(stageLabel, Math.min(99, Math.round(stageBase + progress * stageSpan))); };
  ff.on('progress', onProg);

  try {
    onStage(t('stage_reading'), 0);
    await ff.writeFile(inName, new Uint8Array(await file.arrayBuffer()));

    // 1) audio → mono mp3 (optional; many clips have it, some don't)
    let mp3 = null;
    stageLabel = t('stage_audio'); stageBase = 0; stageSpan = 30;
    try {
      // -map_metadata -1 + -id3v2_version 0: bare MP3, no ID3 tag, so the player's CBR byte-map seek is exact.
      await ff.exec(['-i', inName, '-vn', '-ac', '1', '-ar', String(prof.ar), '-c:a', 'libmp3lame', '-b:a', prof.ab, '-map_metadata', '-1', '-id3v2_version', '0', 'a.mp3']);
      const d = await ff.readFile('a.mp3');
      if (d && d.length > 0) mp3 = new Blob([d], { type: 'audio/mpeg' });
    } catch { /* no audio stream — fine */ }

    // 2) video → concatenated MJPEG → frames
    stageLabel = t('stage_video'); stageBase = 30; stageSpan = 60;
    await ff.exec(['-i', inName, '-an', '-vf', vf(fit, prof.fps), '-q:v', String(prof.q), '-c:v', 'mjpeg', '-f', 'image2pipe', 'v.mjpeg']);
    const mjpeg = await ff.readFile('v.mjpeg');
    const frames = splitMjpeg(mjpeg);
    if (!frames.length) throw new Error(t('err_no_frames'));

    // 3) assemble .nfv (v2, indexed)
    stageLabel = t('stage_assembling'); stageBase = 90; stageSpan = 9;
    onStage(stageLabel, 95);
    const nfv = buildNfv(frames, { fps: prof.fps, hasAudio: !!mp3 });

    // cleanup MEMFS so a second conversion starts clean
    for (const f of [inName, 'a.mp3', 'v.mjpeg']) { try { await ff.deleteFile(f); } catch {} }

    return { nfv, mp3, frames: frames.length, fps: prof.fps, base: base(file.name) };
  } finally {
    try { ff.off('progress', onProg); } catch {}
  }
}

// Write the result onto the device SD (same-origin — no CORS, no huge raw upload).
export async function deliverToDevice(result, onStage = () => {}) {
  const put = async (path, blob) => {
    const r = await fetch('/api/fs/write?path=' + encodeURIComponent(path), {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: blob,
    });
    if (!r.ok) throw new Error(t('err_write_failed', { status: r.status, path }));
  };
  onStage(t('stage_writing'), 99);
  await put(`/data/Videos/${result.base}.nfv`, result.nfv);
  if (result.mp3) await put(`/data/Videos/${result.base}.mp3`, result.mp3);
  return `/data/Videos/${result.base}.nfv`;
}
